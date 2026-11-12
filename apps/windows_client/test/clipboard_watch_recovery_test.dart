// PRIVATE RED candidate against immutable actual22. No clipboard/OS/network I/O.
// Real RemoteSession.capture -> ClipboardSession -> ClipboardSync -> adapter;
// the MethodChannel, media, sockets and clipboard metadata are in-memory fakes.
import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/clipboard_sync.dart';
import 'package:multitap_client/data/native_clipboard.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart' show SessionSocket;
import 'session_recovery_test.dart' show RecoveryMedia, connected;
import 'clipboard_session_test.dart' show ClipboardSocket;

const _channel = MethodChannel('multitap/clipboard');
const _id = 'qa-session';
const _redWatch = 'explicit same-active public capture retries a failed local metadata watch';
const _redArm = 'explicit same-active public capture retries failed remote arm without restarting healthy local watch';

enum _Phase { capture, deactivate, retire, mediaClose, syncClose }

// Observe the exact Future under the existing fake clock; no unbounded await
// or redundant re-await of an already observed completed Future in teardown.
Future<void> _progress(WidgetTester t, Future<void> work, _Phase phase) async {
  var done = false;
  Object? failure;
  work.then<void>((_) { done = true; }, onError: (Object error) {
    failure = error;
    done = true;
  });
  for (var n = 0; n < 320 && !done; n++) {
    await t.pump(const Duration(milliseconds: 10));
    if (n % 8 == 0) await t.runAsync(() => Future<void>.delayed(Duration.zero));
  }
  debugPrintSynchronously('[clipboard-watch-red phase=${phase.name} done=$done failed=${failure != null}]');
  expect(done, isTrue, reason: 'bounded exact ${phase.name} Future completed');
  expect(failure, isNull, reason: 'exact ${phase.name} Future did not fail');
}

Future<void> _turns(WidgetTester t) async {
  for (var n = 0; n < 12; n++) { await t.pump(); }
}

class _Media extends RecoveryMedia {
  bool accepted = true;
  @override
  Future<bool> capture(bool enabled) async {
    captures.add(enabled);
    return enabled && accepted;
  }
}

class _Native {
  int sequence = 17, enabledWatches = 0, arms = 0, captures = 0;
  int writes = 0, sourceReads = 0;
  bool failWatch = false, failArm = false;
  Completer<Object?>? watchGate;
  final owners = <String>{};
  final unexpected = <String>[];
  Future<Object?> call(MethodCall call) async {
    final args = Map<String, Object?>.from(call.arguments as Map? ?? {});
    switch (call.method) {
      case 'watch':
        if (args['enabled'] == true) {
          enabledWatches++;
          owners.add(args['owner'] as String);
          final gate = watchGate;
          if (gate != null) return gate.future;
          if (failWatch) throw PlatformException(code: 'clipboard_not_active');
        }
        return {'sequence': sequence, 'own': true};
      case 'observePendingSequence':
        arms++;
        if (failArm) throw PlatformException(code: 'clipboard_busy');
        return sequence;
      case 'capture':
        captures++;
        return {'own': true, 'sequence': sequence};
      case 'cancelPendingOwner':
      case 'cancelPending':
      case 'cancel':
      case 'release':
      case 'pullShutdown':
      case 'pullClose':
      case 'pullAbort':
      case 'pullSupersede':
      case 'pullRetire':
      case 'closeOffers':
      case 'withdrawOffer':
        return null;
      case 'sourceChunk':
      case 'sourceDigest':
      case 'pullOpen':
      case 'pullRead':
        sourceReads++;
        break;
      case 'begin':
      case 'append':
      case 'finish':
      case 'commit':
      case 'claim':
      case 'beginPending':
      case 'publishOffer':
      case 'publishPendingOffer':
        writes++;
        break;
    }
    unexpected.add(call.method);
    throw PlatformException(code: 'fixture_unexpected_native_call');
  }
}

Map<String, dynamic> _ready({required bool intent, required bool sync}) => {
  'type': 'clipboard.ready', 'sessionId': _id, 'protocol': 'clipboard-v1',
  'clipboardSync': sync,
  if (intent) ...{'clipboardSettled': clipboardSettledProtocol, 'clipboardIntent': clipboardIntentProtocol},
  'clipboardPolicy': {'textToRemote': true, 'textToLocal': true, 'filesToRemote': true, 'filesToLocal': true},
  'limits': intent ? {
    'chunkBytes': 46080, 'maxFiles': 200000, 'maxFileBytes': 9007199254740991,
    'maxTotalBytes': 9007199254740991, 'maxTextUnits': 64*1024*1024,
    'maxTextBytes': 64*1024*1024, 'limitsProfile': 2, 'formatsProfile': 2,
    'maxEntries': 200000, 'maxDepth': 512, 'maxPathChars': 1024,
    'maxRichBytes': 64*1024*1024, 'maxImageBytes': 256*1024*1024,
    'maxImagePixels': 64000000, 'windowBytes': 1024*1024, 'windowUnits': 32,
    'pullProfile': 1, 'maxOffers': 2, 'scaleProfile': 1, 'onPasteProfile': 1,
    'pushTextBytes': 1024*1024, 'pushImageBytes': 8*1024*1024,
  } : {'chunkBytes': 32768, 'maxFiles': 128, 'maxFileBytes': 268435456,
       'maxTotalBytes': 536870912, 'maxTextUnits': 32768},
};

class _Fixture {
  _Fixture({this.intent = false, this.sync = true, this.granted = true}) {
    messenger.setMockMethodCallHandler(_channel, native.call);
    session = RemoteSession(
      connection: BrokerConnection(sessionId: _id,
        webSocketUrl: Uri.parse('wss://fixture.invalid/api/v1/sessions/$_id'),
        clipboardWebSocketUrl: Uri.parse('wss://fixture.invalid/api/v1/sessions/$_id/clipboard'),
        clipboardPolicy: granted ? const ClipboardPolicy.all() : const ClipboardPolicy(),
        clipboardSync: sync),
      token: 'fixture-only', media: media, dialer: (_, _) async => socket,
      clipboardFactory: (_, _) => clipboard = ClipboardSession(
        url: Uri.parse('wss://fixture.invalid/api/v1/sessions/$_id/clipboard'),
        token: 'fixture-only', sessionId: _id, policy: const ClipboardPolicy.all(),
        syncSupported: sync, bridge: NativeClipboardBridge(allowedText: true, allowedFiles: true),
        dialer: (_, _) async => clipSocket),
    );
  }
  final bool intent, sync, granted;
  final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final native = _Native(), media = _Media();
  final socket = SessionSocket(), clipSocket = ClipboardSocket();
  late final RemoteSession session;
  ClipboardSession? clipboard;
  Future<void> start(WidgetTester t) async {
    await connected(t, session, socket);
    await _turns(t);
    if (granted) {
      expect(clipboard, isNotNull);
      clipSocket.emit(_ready(intent: intent, sync: sync));
      await _turns(t);
      expect(clipboard!.ready, isTrue);
    } else { expect(clipboard, isNull); }
    expect(session.frameReady, isTrue);
  }
  Future<void> capture(WidgetTester t) async {
    await _progress(t, session.capture(true), _Phase.capture);
    await _turns(t);
  }
  int get remoteEnables => clipSocket.sent.where((m) => m['type']=='clipboard.watch' && m['enabled']==true).length;
  void noReplay() {
    expect(native.captures, 0); expect(native.sourceReads, 0); expect(native.writes, 0);
    expect(native.unexpected, isEmpty);
    expect(clipSocket.sent.where((m) => ['clipboard.begin','clipboard.read','clipboard.offer','clipboard.commit'].contains(m['type'])), isEmpty);
  }
  Future<void> close(WidgetTester t) async {
    final gate = native.watchGate;
    if (gate != null && !gate.isCompleted) gate.completeError(PlatformException(code: 'clipboard_not_active'));
    await _progress(t, session.close(), _Phase.retire);
    await _progress(t, media.events.close(), _Phase.mediaClose);
    messenger.setMockMethodCallHandler(_channel, null);
    expect(native.unexpected, isEmpty);
  }
}

void main() {
  final windows = TargetPlatformVariant.only(TargetPlatform.windows);
  final skip = !Platform.isWindows;
  testWidgets(_redWatch, (t) async {
    final f = _Fixture()..native.failWatch = true;
    try {
      await f.start(t); await f.capture(t);
      expect(f.session.captured, isTrue); expect(f.clipboard!.error, 'clipboard_watch_unavailable');
      expect(f.native.enabledWatches, 1); expect(f.remoteEnables, 0);
      final sameClipboard = f.clipboard;
      f.native.failWatch = false;
      await f.capture(t);
      expect(f.clipboard, same(sameClipboard)); expect(f.native.owners, hasLength(1));
      f.noReplay();
      expect(f.native.enabledWatches, 2, reason: 'explicit accepted same-owner recapture must retry failed metadata initialization');
      expect(f.remoteEnables, 1);
    } finally { await f.close(t); }
  }, variant: windows, skip: skip);
  testWidgets(_redArm, (t) async {
    final f = _Fixture(intent: true)..native.failArm = true;
    try {
      await f.start(t); await f.capture(t);
      expect(f.native.enabledWatches, 1); expect(f.native.arms, 1); expect(f.remoteEnables, 0);
      expect(f.clipboard!.error, 'clipboard_watch_unavailable');
      f.native.failArm = false; await f.capture(t); f.noReplay();
      expect(f.native.enabledWatches, 1, reason: 'healthy local watch and own baseline must survive arm-only recovery');
      expect(f.native.arms, 2, reason: 'explicit same-owner recapture must retry the failed remote arm');
      expect(f.remoteEnables, 1);
    } finally { await f.close(t); }
  }, variant: windows, skip: skip);
  testWidgets('healthy same-active recapture keeps watch and remote epoch idempotent', (t) async {
    final f = _Fixture(intent: true);
    try { await f.start(t); await f.capture(t); await f.capture(t);
      expect(f.native.enabledWatches, 1); expect(f.native.arms, 1); expect(f.remoteEnables, 1); f.noReplay();
    } finally { await f.close(t); }
  }, variant: windows, skip: skip);
  testWidgets('late foreground refusal after deactivation cannot rearm or report a current failure', (t) async {
    final f = _Fixture()..native.watchGate = Completer<Object?>();
    try { await f.start(t); await f.capture(t); expect(f.native.enabledWatches, 1);
      final off = f.session.capture(false); await _turns(t);
      f.native.watchGate!.completeError(PlatformException(code: 'clipboard_not_active'));
      await _progress(t, off, _Phase.deactivate); await _turns(t);
      expect(f.session.captured, isFalse); expect(f.clipboard!.error, isNull);
      expect(f.native.enabledWatches, 1); expect(f.remoteEnables, 0); f.noReplay();
    } finally { await f.close(t); }
  }, variant: windows, skip: skip);
  testWidgets('rejected native recapture cannot retry a failed clipboard watch', (t) async {
    final f = _Fixture()..native.failWatch = true;
    try { await f.start(t); await f.capture(t); f.native.failWatch = false; f.media.accepted = false;
      await f.capture(t); expect(f.session.captured, isFalse);
      expect(f.native.enabledWatches, 1); expect(f.remoteEnables, 0); f.noReplay();
    } finally { await f.close(t); }
  }, variant: windows, skip: skip);
  testWidgets('retired session cannot rearm after a successor starts with a distinct owner', (t) async {
    final old = _Fixture()..native.failWatch = true;
    await old.start(t); await old.capture(t); await old.close(t);
    final next = _Fixture();
    try { await next.start(t); await next.capture(t);
      await _progress(t, old.session.capture(true), _Phase.capture); await _turns(t);
      expect(old.native.enabledWatches, 1); expect(next.native.enabledWatches, 1);
      expect(next.native.owners.intersection(old.native.owners), isEmpty);
      expect(old.session.closed, isTrue); expect(next.session.closed, isFalse); next.noReplay();
    } finally { await next.close(t); }
  }, variant: windows, skip: skip);
  testWidgets('own-write baseline on focus return never captures or replays clipboard content', (t) async {
    final f = _Fixture();
    try { await f.start(t); await f.capture(t);
      await _progress(t, f.session.capture(false), _Phase.deactivate); await _turns(t); await f.capture(t);
      expect(f.native.enabledWatches, 2); f.noReplay();
    } finally { await f.close(t); }
  }, variant: windows, skip: skip);
  for (final denied in [false, true]) {
    testWidgets(denied ? 'no clipboard grant creates no watcher' : 'no sync capability enables no watcher', (t) async {
      final f = _Fixture(sync: false, granted: !denied);
      try { await f.start(t); await f.capture(t); await f.capture(t);
        expect(f.native.enabledWatches, 0); expect(f.remoteEnables, 0); f.noReplay();
      } finally { await f.close(t); }
    }, variant: windows, skip: skip);
  }
  testWidgets('uncertain recovery baseline and inherited remote sequence cannot replay contents', (t) async {
    final bridge = _Observation(); var sends = 0, receives = 0;
    final sync = ClipboardSync(owner: 'uncertain-fixture-owner', bridge: bridge,
      policy: const ClipboardPolicy.all(), observeInitialLocalOnly: true, receivedRemoteSequence: 41,
      sendLocal: () async { sends++; }, receiveRemote: () async { receives++; },
      watchRemote: (_) {}, waitIdle: () async {}, onFailure: () {});
    try { await sync.setActive(true); sync.remoteChanged(41);
      await t.pump(const Duration(milliseconds: 400)); await sync.setActive(false); await sync.setActive(true);
      await t.pump(const Duration(milliseconds: 400)); expect(sends, 0); expect(receives, 0);
      expect(sync.allowsAutomaticLocal(17), isFalse);
    } finally { await _progress(t, sync.close(), _Phase.syncClose); await _progress(t, bridge.events.close(), _Phase.syncClose); }
  }, variant: windows, skip: skip);
}

class _Observation implements ClipboardObservationBridge {
  final events = StreamController<ClipboardChange>.broadcast(sync: true);
  @override Stream<ClipboardChange> get changes => events.stream;
  @override int? get lastCommittedSequence => null;
  @override Future<int> watch(String owner, bool enabled) async => 17;
}

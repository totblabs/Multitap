import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';

// Real Dart session and adapter; modeled native STA/MTA/platform-reply queues.
// No HWND, COM, native clipboard, filesystem payload, input or network is used.
const _channel = MethodChannel('multitap/clipboard');
const _sessionId = 'pending-queue-fixture';
const _remoteSequence = 41;

class _Socket extends Fake implements WebSocket {
  final incoming = StreamController<dynamic>();
  final sent = <Map<String, dynamic>>[];
  void Function(String)? note;
  @override
  Duration? pingInterval;
  @override
  StreamSubscription<dynamic> listen(
    void Function(dynamic)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => incoming.stream.listen(
    onData,
    onError: onError,
    onDone: onDone,
    cancelOnError: cancelOnError,
  );
  @override
  void add(dynamic value) =>
      sent.add(jsonDecode(value as String) as Map<String, dynamic>);
  void emit(Map<String, dynamic> value) => incoming.add(jsonEncode(value));
  @override
  Future<void> close([int? code, String? reason]) async {
    note?.call('socket:close');
    unawaited(incoming.close());
  }
}

class _Job {
  _Job(this.method, this.args);
  final String method;
  final Map<String, Object?> args;
  final reply = Completer<Object?>();
  Object? value;
}

class _NativeQueues {
  _NativeQueues({this.sourceReceipts = false});
  final bool sourceReceipts;
  final sta = <_Job>[], controls = <_Job>[], pending = <_Job>[];
  final replies = <_Job>[], calls = <_Job>[];
  final trace = <String>[], unexpected = <String>[];
  bool holdCaptureReply = false, renderOnPendingBegin = false;
  bool capturePhysicallyDone = false, renderWaiting = false;
  bool renderTimedOut = false;
  bool holdPendingCommit = false, modelCommitReadiness = false;
  bool mediaPrepared = false, renderReadySignaled = false;
  bool renderPublished = false;
  int sequence = 10, epoch = 0, tokenCounter = 0, captureSerial = 0;
  Map<String, Object?>? lease;
  Timer? renderDeadline;

  void note(String value) {
    if (trace.length < 160) trace.add(value);
  }

  // Callbacks record/route only. No assertion or tester pump is inside them.
  Future<Object?> call(MethodCall call) {
    final args = Map<String, Object?>.from(call.arguments as Map? ?? {});
    final job = _Job(call.method, args);
    calls.add(job);
    note('submit:${job.method}');
    switch (job.method) {
      case 'watch':
        return Future.value({'sequence': sequence});
      case 'observePendingSequence':
        epoch = args['epoch'] as int;
        return Future.value(sequence);
      case 'bindPending':
        return Future.value(
          lease != null &&
              args['pendingToken'] == lease!['token'] &&
              args['pendingEpoch'] == epoch,
        );
      case 'cancelPending':
        if (args['pendingToken'] == lease?['token']) unblock('lease-cancel');
        return Future.value();
      case 'cancelPendingOwner':
        // Native invalidates now; its acknowledgement is an STA control job.
        if (args['epoch'] == epoch) unblock('epoch-cancel');
        controls.add(job);
        return job.reply.future;
      case 'pullData':
      case 'pullEnd':
      case 'pullFail':
      case 'pullShutdown':
      case 'pullAbort':
      case 'pullSupersede':
      case 'pullRetire':
        return Future.value();
    }
    if (args.containsKey('pendingToken')) {
      pending.add(job);
      return job.reply.future;
    }
    sta.add(job);
    return job.reply.future;
  }

  void runWorkers() {
    while (pending.isNotEmpty) {
      if (holdPendingCommit && pending.first.method == 'commit') break;
      final job = pending.removeAt(0);
      note('pending:${job.method}');
      if (job.method == 'commit') {
        // PendingClipboard::Commit prepares media, sets ready and notifies the
        // blocked renderer; it does not publish HGLOBAL in this worker. The
        // returned sequence is the lease's latest value before Render resumes.
        job.value = {'sequence': sequence};
        if (modelCommitReadiness) {
          mediaPrepared = true;
          renderReadySignaled = true;
          note('commit:model-media-ready-notify');
        }
      }
      replies.add(job);
    }
    while (!renderWaiting && (controls.isNotEmpty || sta.isNotEmpty)) {
      final job = controls.isNotEmpty ? controls.removeAt(0) : sta.removeAt(0);
      note('sta:${job.method}');
      switch (job.method) {
        case 'capture':
          capturePhysicallyDone = true;
          job.value = {
            'format': 'text',
            'sequence': sequence,
            if (sourceReceipts)
              'sourceReceipt': {
                'owner': job.args['owner'] as String,
                'captureSerial': ++captureSerial,
              },
            'items': [
              {'size': 1, 'units': 1, 'kind': 'text'},
            ],
          };
          note('capture:physical-done');
        case 'beginPending':
          if (job.args['epoch'] == epoch &&
              job.args['expectedLocalSequence'] == sequence) {
            lease = {
              'token': 'fixture-pending-${++tokenCounter}',
              'sequence': ++sequence,
              'sourceSequence': job.args['sourceSequence'],
              'kind': job.args['kind'],
              'epoch': epoch,
            };
            job.value = lease;
            note('pending:published');
          }
        case 'dropOffers':
          job.value = <String>[];
        case 'release':
          // Only the dispatch ordering is modeled. Do not mirror State cleanup.
          note('release:executed');
        case 'retireSource':
          // An ordinary STA reply only. No native source/receiver cleanup policy.
          job.value = {'retired': true};
          note('retire-source:executed');
        case 'sourceChunkScoped':
          // Fixed synthetic data; identity/range enforcement is native QA's scope.
          job.value = Uint8List.fromList([0x61]);
        case 'sourceDigestScoped':
          job.value = sha256.convert(Uint8List.fromList([0x61])).toString();
        case 'cancelPendingOwner':
          note('cancel-owner:sta-ack');
        case 'pullClose':
        case 'closeOffers':
        case 'withdrawOffer':
        case 'cancel':
        case 'begin':
          break;
        default:
          unexpected.add(job.method);
      }
      replies.add(job);
      if (job.method == 'beginPending' &&
          job.value != null &&
          renderOnPendingBegin) {
        renderWaiting = true;
        note('render:model-wait');
        // Flutter's test clock models the native default wait bound. This does
        // not execute or measure PendingClipboard::Render or real COM GetData.
        renderDeadline = Timer(const Duration(seconds: 5), () {
          renderTimedOut = true;
          unblock('modeled-five-second-deadline');
        });
      }
    }
  }

  void deliverPlatformReplies() {
    // Native Complete drains its queue in FIFO order. It cannot deliver a
    // later begin reply while an earlier capture reply is artificially held.
    if (holdCaptureReply && replies.any((job) => job.method == 'capture')) {
      return;
    }
    while (replies.isNotEmpty) {
      final job = replies.removeAt(0);
      note('reply:${job.method}');
      if (!job.reply.isCompleted) job.reply.complete(job.value);
    }
  }

  void unblock(String reason) {
    if (renderWaiting) note('render:unblock:$reason');
    renderWaiting = false;
    renderDeadline?.cancel();
    renderDeadline = null;
  }

  bool resumePreparedRender() {
    // Model the STA being scheduled after Commit's ready notification. This
    // action cannot bypass an unfinished transfer or be called at read-sent.
    if (!renderWaiting || !mediaPrepared || !renderReadySignaled) return false;
    note('render:model-ready-resume');
    unblock('modeled-ready-media');
    renderPublished = true;
    ++sequence;
    note('render:model-publish');
    return true;
  }

  bool get releaseQueued => sta.any((job) => job.method == 'release');
  bool get sourceCleanupQueued =>
      sta.any((job) => job.method == 'release' || job.method == 'retireSource');
  bool get empty =>
      sta.isEmpty && controls.isEmpty && pending.isEmpty && replies.isEmpty;
}

Map<String, dynamic> _ready() => {
  'type': 'clipboard.ready',
  'sessionId': _sessionId,
  'protocol': 'clipboard-v1',
  'clipboardSync': true,
  'clipboardSettled': clipboardSettledProtocol,
  'clipboardIntent': clipboardIntentProtocol,
  'clipboardPolicy': {
    'textToRemote': true,
    'textToLocal': true,
    'filesToRemote': true,
    'filesToLocal': true,
  },
  'limits': {
    'chunkBytes': 46080,
    'maxFiles': 200000,
    'maxFileBytes': 9007199254740991,
    'maxTotalBytes': 9007199254740991,
    'maxTextUnits': 64 * 1024 * 1024,
    'maxTextBytes': 64 * 1024 * 1024,
    'limitsProfile': 2,
    'formatsProfile': 2,
    'maxEntries': 200000,
    'maxDepth': 512,
    'maxPathChars': 1024,
    'maxRichBytes': 64 * 1024 * 1024,
    'maxImageBytes': 256 * 1024 * 1024,
    'maxImagePixels': 64000000,
    'windowBytes': 1024 * 1024,
    'windowUnits': 32,
    'pullProfile': 1,
    'maxOffers': 2,
    'scaleProfile': 1,
    'onPasteProfile': 1,
    'pushTextBytes': 1024 * 1024,
    'pushImageBytes': 8 * 1024 * 1024,
  },
};

class _Fixture {
  _Fixture(this.tester, {bool outgoing = true, bool sourceReceipts = false})
    : native = _NativeQueues(sourceReceipts: sourceReceipts) {
    socket.note = native.note;
    socket.incoming.onCancel = () {
      native.note('socket:subscription-cancel');
    };
    messenger.setMockMethodCallHandler(_channel, native.call);
    bridge = NativeClipboardBridge(allowedText: outgoing);
    session = ClipboardSession(
      url: Uri.parse(
        'wss://synthetic.invalid/api/v1/sessions/$_sessionId/clipboard',
      ),
      token: 'fixture-only',
      sessionId: _sessionId,
      policy: ClipboardPolicy(textToRemote: outgoing, textToLocal: true),
      syncSupported: true,
      bridge: bridge,
      dialer: (_, _) async => socket,
      pullLinger: Duration.zero,
    );
  }
  final WidgetTester tester;
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final _NativeQueues native;
  final socket = _Socket();
  late final NativeClipboardBridge bridge;
  late final ClipboardSession session;
  int epoch = 0;
  int get reads =>
      socket.sent.where((v) => v['type'] == 'clipboard.read').length;

  Future<void> tick([
    Duration elapsed = const Duration(milliseconds: 1),
  ]) async {
    native.runWorkers();
    native.deliverPlatformReplies();
    await tester.pump(elapsed);
  }

  Future<void> until(
    bool Function() predicate, {
    int maxTicks = 700,
    String stage = 'transition',
    bool drainRealEventLoop = false,
  }) async {
    for (var tickCount = 0; tickCount < maxTicks && !predicate(); tickCount++) {
      await tick();
      if (drainRealEventLoop && tickCount % 8 == 0) {
        // Stream cancellation can complete on its creation zone's real event
        // loop. This is teardown only; the witness still uses fake-clock pumps.
        await tester.runAsync(() => Future<void>.delayed(Duration.zero));
      }
    }
    if (!predicate()) {
      throw StateError(
        'bounded fixture $stage failed: '
        'empty=${native.empty}, sta=${native.sta.length}, '
        'controls=${native.controls.length}, pending=${native.pending.length}, '
        'replies=${native.replies.length}, trace=${native.trace}',
      );
    }
  }

  Future<void> connect() async {
    Object? failure;
    var started = false;
    session.start().then<void>(
      (_) {
        started = true;
      },
      onError: (Object error) {
        failure = error;
        started = true;
      },
    );
    await tester.pump();
    socket.emit(_ready());
    await until(() => started, stage: 'start');
    if (failure != null) throw StateError('fixture start rejected: $failure');
    var active = false;
    session.setSharingActive(true).then<void>((_) {
      active = true;
    });
    await until(
      () =>
          active &&
          socket.sent.any(
            (v) => v['type'] == 'clipboard.watch' && v['enabled'] == true,
          ),
      stage: 'arm',
    );
    epoch =
        socket.sent.lastWhere(
              (v) => v['type'] == 'clipboard.watch' && v['enabled'] == true,
            )['watchEpoch']
            as int;
  }

  void intent() => socket.emit({
    'type': 'clipboard.intent',
    'sessionId': _sessionId,
    'sequence': _remoteSequence,
    'kind': 'text',
    'watchEpoch': epoch,
  });
  void settled() => socket.emit({
    'type': 'clipboard.changed',
    'sessionId': _sessionId,
    'sequence': _remoteSequence,
    'settled': clipboardSettledProtocol,
    'watchEpoch': epoch,
  });

  Future<void> cycle() async {
    native.holdCaptureReply = true;
    await connect();
    await until(() => native.capturePhysicallyDone, stage: 'capture physical');
    native.renderOnPendingBegin = true;
    intent();
    await until(() => native.renderWaiting, stage: 'pending render');
    // Simulate the platform completion message after the STA entered Render.
    native.holdCaptureReply = false;
    await until(
      () => native.sourceCleanupQueued,
      stage: 'old source cleanup queued',
    );
    settled();
    await tick(const Duration(milliseconds: 400));
    await tick();
  }

  void report(String kind) => tester.printToConsole(
    'PENDING_QUEUE $kind ${jsonEncode({
      'nativeComExecuted': false,
      'realWindowsClipboard': false,
      'capturePhysicallyDoneModeled': native.capturePhysicallyDone,
      'renderWaitingModeled': native.renderWaiting,
      'releaseQueued': native.releaseQueued,
      'sessionBusy': session.busy,
      'remoteReads': reads,
      'pendingPayloadCalls': native.calls.where((job) => job.args.containsKey('pendingToken') && const {'begin', 'append', 'finish', 'commit', 'publishPendingOffer'}.contains(job.method)).length,
      'pendingCommits': native.calls.where((job) => job.method == 'commit' && job.args.containsKey('pendingToken')).length,
      'modeledRenderTimedOut': native.renderTimedOut,
      'trace': native.trace,
    })}',
  );

  Future<void> close() async {
    // Always clear artificial waits and drain every MethodChannel reply first.
    native.unblock('fixture-finally');
    native.holdCaptureReply = false;
    await tick();
    var closed = false;
    Object? failure;
    native.note('close:called');
    session.close().then<void>(
      (_) {
        native.note('close:completed');
        closed = true;
      },
      onError: (Object error) {
        native.note('close:error');
        failure = error;
        closed = true;
      },
    );
    try {
      await until(
        () => closed && native.empty,
        maxTicks: 6500,
        drainRealEventLoop: true,
        stage:
            'finally session.close=$closed and reply drain '
            'sta=${native.sta.length}/control=${native.controls.length}/'
            'pending=${native.pending.length}/replies=${native.replies.length}',
      );
      if (failure != null) throw StateError('fixture close rejected: $failure');
      expect(native.unexpected, isEmpty);
    } finally {
      native.unblock('fixture-dispose');
      messenger.setMockMethodCallHandler(_channel, null);
      session.dispose();
    }
  }
}

void _coupledFollowups(TargetPlatformVariant windows, bool skip) {
  testWidgets(
    'receipt admission waits for actual capture metadata and never releases the whole owner',
    (tester) async {
      final f = _Fixture(tester, sourceReceipts: true);
      try {
        f.native.holdCaptureReply = true;
        await f.connect();
        await f.until(() => f.native.capturePhysicallyDone);
        f.native.renderOnPendingBegin = true;
        f.intent();
        await f.until(() => f.native.renderWaiting);
        f.settled();
        await f.tick(const Duration(milliseconds: 400));
        expect(
          f.reads,
          0,
          reason: 'interface support alone cannot bypass an unresolved capture',
        );
        expect(f.native.sourceCleanupQueued, isFalse);
        f.native.holdCaptureReply = false;
        await f.until(() => f.reads == 1 && f.native.sourceCleanupQueued);
        final captured = f.native.calls.singleWhere(
          (job) => job.method == 'capture',
        );
        final receipt = (captured.value as Map)['sourceReceipt'] as Map;
        final retirement = f.native.calls.singleWhere(
          (job) => job.method == 'retireSource',
        );
        expect(retirement.args, {
          'owner': receipt['owner'],
          'captureSerial': receipt['captureSerial'],
        });
        expect(f.native.calls.where((job) => job.method == 'release'), isEmpty);
        expect(f.native.renderWaiting, isTrue);
        expect(f.native.renderTimedOut, isFalse);
      } finally {
        await f.close();
      }
    },
    skip: skip,
    variant: windows,
  );

  testWidgets(
    'close joins held metadata and cancels its epoch once without reopening a watch',
    (tester) async {
      final f = _Fixture(tester, sourceReceipts: true);
      try {
        f.native.holdCaptureReply = true;
        await f.connect();
        await f.until(() => f.native.capturePhysicallyDone);
        f.native.renderOnPendingBegin = true;
        f.intent();
        await f.until(() => f.native.renderWaiting);
        final beforeBegins = f.native.calls
            .where((job) => job.method == 'beginPending')
            .length;
        final beforeWatches = f.socket.sent
            .where(
              (v) => v['type'] == 'clipboard.watch' && v['enabled'] == true,
            )
            .length;
        var closed = false;
        Object? failure;
        f.session.close().then<void>(
          (_) {
            closed = true;
          },
          onError: (Object error) {
            failure = error;
            closed = true;
          },
        );
        await f.tick();
        await f.tick();
        expect(
          closed,
          isFalse,
          reason: 'a held platform capture reply is still owned',
        );
        f.intent();
        f.settled();
        await f.tick();
        expect(
          f.native.calls.where((job) => job.method == 'beginPending').length,
          beforeBegins,
        );
        f.native.holdCaptureReply = false;
        await f.until(
          () => closed && f.native.empty,
          maxTicks: 6500,
          drainRealEventLoop: true,
          stage: 'joined close',
        );
        expect(failure, isNull);
        expect(
          f.native.calls
              .where((job) => job.method == 'cancelPendingOwner')
              .length,
          1,
          reason: 'Sync watch-off joins the closed Pending barrier',
        );
        expect(
          f.socket.sent
              .where(
                (v) => v['type'] == 'clipboard.watch' && v['enabled'] == true,
              )
              .length,
          beforeWatches,
        );
        expect(
          f.native.calls.where(
            (job) => const {
              'begin',
              'append',
              'finish',
              'commit',
            }.contains(job.method),
          ),
          isEmpty,
        );
      } finally {
        await f.close();
      }
    },
    skip: skip,
    variant: windows,
  );
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final skip = !Platform.isWindows;
  final windows = TargetPlatformVariant.only(TargetPlatform.windows);
  _coupledFollowups(windows, skip);

  testWidgets(
    'wire contract: release has owner only, receiver begin has request',
    (tester) async {
      final f = _Fixture(tester, outgoing: false);
      try {
        var captureDone = false;
        f.bridge.capture('contract-owner').then((_) => captureDone = true);
        await f.until(() => captureDone);
        var beginDone = false;
        f.bridge
            .begin('contract-owner', 'receiver-new', 'text', const [
              ClipboardItem(size: 1),
            ])
            .then((_) => beginDone = true);
        await f.until(() => beginDone);
        var released = false;
        f.bridge.release('contract-owner').then((_) => released = true);
        await f.until(() => released);
        final capture = f.native.calls.singleWhere(
          (v) => v.method == 'capture',
        );
        final begin = f.native.calls.singleWhere((v) => v.method == 'begin');
        final release = f.native.calls.singleWhere(
          (v) => v.method == 'release',
        );
        expect(capture.args.containsKey('offerId'), isFalse);
        expect(capture.args.containsKey('requestId'), isFalse);
        expect(begin.args['requestId'], 'receiver-new');
        expect(release.args, {'owner': 'contract-owner'});
        expect(f.native.unexpected, isEmpty);
        // This proves actual adapter fields, not native State cleanup effects.
        f.report('wire-contract');
      } finally {
        await f.close();
      }
    },
    skip: skip,
    variant: windows,
  );

  testWidgets(
    'legacy: an absent source receipt keeps old capture cleanup serialized',
    (tester) async {
      final f = _Fixture(tester);
      try {
        await f.cycle();
        f.report('cycle-witness');
        expect(f.native.capturePhysicallyDone, isTrue);
        expect(f.native.renderWaiting, isTrue);
        expect(f.native.releaseQueued, isTrue);
        expect(f.session.busy, isTrue);
        expect(f.reads, 0);
        expect(f.native.renderTimedOut, isFalse);
      } finally {
        await f.close();
      }
    },
    skip: skip,
    variant: windows,
  );

  testWidgets(
    'control: without old capture, pending remote read can start',
    (tester) async {
      final f = _Fixture(tester, outgoing: false);
      try {
        await f.connect();
        f.native.renderOnPendingBegin = true;
        f.intent();
        await f.until(() => f.native.renderWaiting);
        // Deliver the begin response but leave the modeled native render blocked.
        await f.tick();
        f.settled();
        await f.until(() => f.reads > 0);
        f.report('no-obsolete-cleanup-control');
        expect(f.native.renderWaiting, isTrue);
        expect(f.reads, 1);
        expect(f.native.releaseQueued, isFalse);
      } finally {
        await f.close();
      }
    },
    skip: skip,
    variant: windows,
  );

  testWidgets(
    'control: real Dart focus grace invalidates modeled render',
    (tester) async {
      final f = _Fixture(tester);
      try {
        await f.cycle();
        var inactive = false;
        f.session.setSharingActive(false).then((_) => inactive = true);
        await f.until(() => inactive);
        await f.tick(const Duration(milliseconds: 1900));
        expect(f.native.renderWaiting, isTrue);
        await f.tick(const Duration(milliseconds: 101));
        await f.until(() => !f.native.renderWaiting && !f.native.releaseQueued);
        f.report('two-second-dart-grace-control');
        expect(f.native.trace, contains('render:unblock:epoch-cancel'));
        expect(f.native.renderTimedOut, isFalse);
      } finally {
        await f.close();
      }
    },
    skip: skip,
    variant: windows,
  );

  testWidgets(
    'control: modeled five-second render deadline releases queue',
    (tester) async {
      final f = _Fixture(tester);
      try {
        await f.cycle();
        expect(f.native.renderTimedOut, isFalse);
        // Advancing only a fake Timer: no real Render/GetData or five-second sleep.
        await f.tick(const Duration(seconds: 5));
        await f.until(() => !f.native.releaseQueued);
        f.report('modeled-five-second-deadline-control');
        expect(f.native.renderTimedOut, isTrue);
        expect(f.native.renderWaiting, isFalse);
        expect(
          f.native.trace,
          contains('render:unblock:modeled-five-second-deadline'),
        );
      } finally {
        await f.close();
      }
    },
    skip: skip,
    variant: windows,
  );

  testWidgets(
    'payload control: no old capture delivers small pending text before render resumes',
    (tester) async {
      final f = _Fixture(tester, outgoing: false);
      final bytes = Uint8List.fromList([0x61]);
      final digest = sha256.convert(bytes).toString();
      try {
        f.native.modelCommitReadiness = true;
        await f.connect();
        f.native.renderOnPendingBegin = true;
        f.intent();
        await f.until(() => f.native.renderWaiting);
        await f.tick();
        f.settled();
        await f.until(() => f.reads == 1);
        final read = f.socket.sent.singleWhere(
          (v) => v['type'] == 'clipboard.read',
        );
        final request = read['requestId'] as String;
        expect(read['expectedSequence'], _remoteSequence);
        expect(read['watchEpoch'], f.epoch);
        expect(f.native.mediaPrepared, isFalse);
        expect(f.native.renderWaiting, isTrue);

        // Peer messages follow the actual Session read, with its actual id.
        f.socket.emit({
          'type': 'clipboard.begin',
          'requestId': request,
          'format': 'text',
          'size': bytes.length,
        });
        await f.until(() => f.native.trace.contains('pending:begin'));
        expect(f.native.renderWaiting, isTrue);
        expect(f.native.mediaPrepared, isFalse);
        f.socket.emit({
          'type': 'clipboard.chunk',
          'requestId': request,
          'index': 0,
          'offset': 0,
          'data': base64Encode(bytes),
        });
        await f.until(() => f.native.trace.contains('pending:append'));
        expect(f.native.renderWaiting, isTrue);
        f.socket.emit({
          'type': 'clipboard.item.end',
          'requestId': request,
          'index': 0,
          'sha256': digest,
        });
        await f.until(
          () => f.socket.sent.any(
            (v) =>
                v['type'] == 'clipboard.item.ack' && v['requestId'] == request,
          ),
        );
        expect(f.native.trace, contains('pending:finish'));
        expect(f.native.renderWaiting, isTrue);
        expect(f.native.mediaPrepared, isFalse);

        f.native.holdPendingCommit = true;
        f.socket.emit({'type': 'clipboard.commit', 'requestId': request});
        await f.until(() => f.native.pending.any((v) => v.method == 'commit'));
        expect(f.native.mediaPrepared, isFalse);
        expect(f.native.renderReadySignaled, isFalse);
        expect(f.native.renderWaiting, isTrue);
        f.native.holdPendingCommit = false;
        await f.until(() => f.native.mediaPrepared);
        await f.until(
          () => f.socket.sent.any(
            (v) =>
                v['type'] == 'clipboard.applied' && v['requestId'] == request,
          ),
        );
        expect(f.native.renderReadySignaled, isTrue);
        expect(f.native.renderWaiting, isTrue);
        expect(f.native.renderPublished, isFalse);

        final payloadCalls = f.native.calls
            .where(
              (v) =>
                  const {
                    'begin',
                    'append',
                    'finish',
                    'commit',
                  }.contains(v.method) &&
                  v.args.containsKey('pendingToken'),
            )
            .toList();
        expect(payloadCalls.map((v) => v.method), [
          'begin',
          'append',
          'finish',
          'commit',
        ]);
        final binding = f.native.calls.singleWhere(
          (v) => v.method == 'bindPending',
        );
        expect(binding.args['requestId'], request);
        for (final call in [binding, ...payloadCalls]) {
          expect(call.args['requestId'], request);
          expect(call.args['pendingToken'], f.native.lease!['token']);
          expect(call.args['pendingSequence'], f.native.lease!['sequence']);
          expect(call.args['pendingSourceSequence'], _remoteSequence);
          expect(call.args['pendingEpoch'], f.epoch);
          expect(call.args['pendingKind'], 'text');
        }
        expect(payloadCalls[1].args['bytes'], orderedEquals(bytes));
        expect(payloadCalls[2].args['digest'], digest);
        expect(
          digest,
          'ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb',
        );
        expect(f.native.trace, isNot(contains('sta:begin')));
        expect(f.native.trace, isNot(contains('sta:commit')));

        final resumed = f.native.resumePreparedRender();
        expect(resumed, isTrue);
        expect(f.native.renderWaiting, isFalse);
        expect(f.native.renderPublished, isTrue);
        expect(f.native.renderTimedOut, isFalse);
        f.socket.emit({'type': 'clipboard.complete', 'requestId': request});
        await f.until(() => !f.session.busy);
        expect(f.session.error, isNull);
        expect(f.session.transferredBytes, 1);
        f.report('small-payload-control');
      } finally {
        f.native.holdPendingCommit = false;
        await f.close();
      }
    },
    skip: skip,
    variant: windows,
  );

  testWidgets(
    'receipt progress: remote read starts while receipt-capable old capture cleanup waits in Render',
    (tester) async {
      final f = _Fixture(tester, sourceReceipts: true);
      try {
        // Same physical capture and whole-FIFO reply hold as the original cycle.
        f.native.holdCaptureReply = true;
        await f.connect();
        await f.until(
          () => f.native.capturePhysicallyDone,
          stage: 'receipt capture physical',
        );
        f.native.renderOnPendingBegin = true;
        f.intent();
        await f.until(
          () => f.native.renderWaiting,
          stage: 'receipt pending render',
        );
        f.native.holdCaptureReply = false;
        await f.until(
          () => f.native.sourceCleanupQueued,
          stage: 'receipt or legacy source cleanup queued',
        );
        f.settled();
        await f.tick(const Duration(milliseconds: 400));
        await f.tick();
        final captured = f.native.calls.singleWhere(
          (job) => job.method == 'capture',
        );
        expect((captured.value as Map)['sourceReceipt'], {
          'owner': captured.args['owner'],
          'captureSerial': 1,
        });
        f.report('receipt-capable-desired-progress');
        expect(f.native.capturePhysicallyDone, isTrue);
        expect(f.native.renderWaiting, isTrue);
        expect(f.native.sourceCleanupQueued, isTrue);
        expect(f.native.renderTimedOut, isFalse);
        expect(
          f.reads,
          greaterThan(0),
          reason:
              'a valid exact source receipt permits the new remote read '
              'while old STA cleanup remains blocked in modeled Render',
        );
      } finally {
        await f.close();
        tester.printToConsole(
          'RECEIPT_WITNESS_FINALLY ${jsonEncode({'sessionCloseCompleted': f.native.trace.contains('close:completed'), 'queuesEmpty': f.native.empty, 'unexpectedEmpty': f.native.unexpected.isEmpty})}',
        );
      }
    },
    skip: skip,
    variant: windows,
  );
  testWidgets(
    'receipt payload: pending text commits while exact old source retirement waits',
    (tester) async {
      final f = _Fixture(tester, sourceReceipts: true);
      final bytes = Uint8List.fromList([0x61]);
      final digest = sha256.convert(bytes).toString();
      try {
        f.native.modelCommitReadiness = true;
        await f.cycle();
        expect(f.native.sourceCleanupQueued, isTrue);
        await f.until(() => f.reads == 1);
        final read = f.socket.sent.singleWhere(
          (v) => v['type'] == 'clipboard.read',
        );
        final request = read['requestId'] as String;
        expect(read['expectedSequence'], _remoteSequence);
        expect(read['watchEpoch'], f.epoch);
        expect(f.native.mediaPrepared, isFalse);
        expect(f.native.renderWaiting, isTrue);

        // Peer messages follow the actual Session read, with its actual id.
        f.socket.emit({
          'type': 'clipboard.begin',
          'requestId': request,
          'format': 'text',
          'size': bytes.length,
        });
        await f.until(() => f.native.trace.contains('pending:begin'));
        expect(f.native.renderWaiting, isTrue);
        expect(f.native.mediaPrepared, isFalse);
        f.socket.emit({
          'type': 'clipboard.chunk',
          'requestId': request,
          'index': 0,
          'offset': 0,
          'data': base64Encode(bytes),
        });
        await f.until(() => f.native.trace.contains('pending:append'));
        expect(f.native.renderWaiting, isTrue);
        f.socket.emit({
          'type': 'clipboard.item.end',
          'requestId': request,
          'index': 0,
          'sha256': digest,
        });
        await f.until(
          () => f.socket.sent.any(
            (v) =>
                v['type'] == 'clipboard.item.ack' && v['requestId'] == request,
          ),
        );
        expect(f.native.trace, contains('pending:finish'));
        expect(f.native.renderWaiting, isTrue);
        expect(f.native.mediaPrepared, isFalse);

        f.native.holdPendingCommit = true;
        f.socket.emit({'type': 'clipboard.commit', 'requestId': request});
        await f.until(() => f.native.pending.any((v) => v.method == 'commit'));
        expect(f.native.mediaPrepared, isFalse);
        expect(f.native.renderReadySignaled, isFalse);
        expect(f.native.renderWaiting, isTrue);
        f.native.holdPendingCommit = false;
        await f.until(() => f.native.mediaPrepared);
        await f.until(
          () => f.socket.sent.any(
            (v) =>
                v['type'] == 'clipboard.applied' && v['requestId'] == request,
          ),
        );
        expect(f.native.renderReadySignaled, isTrue);
        expect(f.native.renderWaiting, isTrue);
        expect(f.native.renderPublished, isFalse);

        final payloadCalls = f.native.calls
            .where(
              (v) =>
                  const {
                    'begin',
                    'append',
                    'finish',
                    'commit',
                  }.contains(v.method) &&
                  v.args.containsKey('pendingToken'),
            )
            .toList();
        expect(payloadCalls.map((v) => v.method), [
          'begin',
          'append',
          'finish',
          'commit',
        ]);
        final binding = f.native.calls.singleWhere(
          (v) => v.method == 'bindPending',
        );
        expect(binding.args['requestId'], request);
        for (final call in [binding, ...payloadCalls]) {
          expect(call.args['requestId'], request);
          expect(call.args['pendingToken'], f.native.lease!['token']);
          expect(call.args['pendingSequence'], f.native.lease!['sequence']);
          expect(call.args['pendingSourceSequence'], _remoteSequence);
          expect(call.args['pendingEpoch'], f.epoch);
          expect(call.args['pendingKind'], 'text');
        }
        expect(payloadCalls[1].args['bytes'], orderedEquals(bytes));
        expect(payloadCalls[2].args['digest'], digest);
        expect(
          digest,
          'ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb',
        );
        expect(f.native.trace, isNot(contains('sta:begin')));
        expect(f.native.trace, isNot(contains('sta:commit')));

        final resumed = f.native.resumePreparedRender();
        expect(resumed, isTrue);
        expect(f.native.renderWaiting, isFalse);
        expect(f.native.renderPublished, isTrue);
        expect(f.native.renderTimedOut, isFalse);
        f.socket.emit({'type': 'clipboard.complete', 'requestId': request});
        await f.until(() => !f.session.busy);
        expect(f.session.error, isNull);
        expect(f.session.transferredBytes, 1);
        f.report('small-payload-control');
      } finally {
        f.native.holdPendingCommit = false;
        await f.close();
      }
    },
    skip: skip,
    variant: windows,
  );
}

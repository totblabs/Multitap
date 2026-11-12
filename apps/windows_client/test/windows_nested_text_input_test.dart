import 'dart:async';
import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/text_input.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'remote_session_test.dart' show SessionSocket, info, packet, flush;
import 'signin_stream_contract_test.dart'
    show signinConnectionJson, signinBindingJson;
import 'signin_stream_session_test.dart' show phaseInfo, signinFrame;
import 'track_stream_test.dart' as tracks;

const _remote = MethodChannel('multitap/remote');
const _events = MethodChannel('multitap/remote/input');

class _Clipboard extends Fake implements ClipboardSession {
  final pasteCompletion = Completer<bool>();
  @override
  bool get automatic => true;
  @override
  bool get canSend => true;
  @override
  Future<bool> preparePaste() => pasteCompletion.future;
  @override
  Future<void> setSharingActive(bool active) async {}
  @override
  void removeListener(void Function() listener) {}
  @override
  Future<void> close({String reason = 'clipboard_transport_closed'}) async {}
}

class _NativeRig {
  _NativeRig(this.granted, {this.signin = false, this.track = false});
  final bool granted;
  final bool signin, track;
  final socket = SessionSocket();
  final calls = <MethodCall>[];
  late RemoteSession session;
  int lease = 0;
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  Future<void> start() async {
    messenger.setMockMethodCallHandler(_remote, (call) async {
      calls.add(call);
      if (call.method == 'start') {
        lease = (call.arguments as Map)['lease'] as int;
        return 42;
      }
      if (call.method == 'capture') return (call.arguments as Map)['enabled'];
      if (call.method == 'startTracks') {
        lease = (call.arguments as Map)['lease'] as int;
        return {'0': 90, '1': 91};
      }
      if (call.method == 'stats') {
        return track ? tracks.trackStats() : {'decodedFrames': 1, 'error': 0};
      }
      if (call.method == 'thumbnailPrivacy') return 7;
      if (call.method == 'thumbnailPrivacyStatus' ||
          call.method == 'thumbnailPrivacyLocked') {
        return true;
      }
      return null;
    });
    messenger.setMockMethodCallHandler(_events, (_) async => null);
    session = RemoteSession(
      connection: signin
          ? BrokerConnection.fromJson(
              {
                ...signinConnectionJson(),
                'signin': signinBindingJson(),
                if (granted) 'textInputProtocol': 'text-input-v1',
              },
              Uri.parse('https://cloud.example.com'),
              requestedSignin: true,
              requestedTextInput: granted,
            )
          : BrokerConnection(
              sessionId: track ? 'track-session' : 'nested-test',
              webSocketUrl: Uri.parse(
                'wss://fixture.invalid/api/v1/sessions/nested-test',
              ),
              textInput: granted,
              trackPlan: track ? tracks.plan() : null,
              options: track ? tracks.options : const StreamOptions(),
            ),
      token: 'synthetic-token',
      dialer: (_, _) async => socket,
    );
    unawaited(session.start());
    await flush();
    socket.incoming.add(
      jsonEncode(
        signin
            ? phaseInfo()
            : track
            ? tracks.info()
            : info,
      ),
    );
    await flush();
    if (track) {
      socket.incoming.add(tracks.packet(0));
      socket.incoming.add(tracks.packet(1));
    } else {
      socket.incoming.add(signin ? signinFrame() : packet());
    }
    await flush();
    for (var n = 0; n < 150 && !session.frameReady; n++) {
      await Future<void>.delayed(const Duration(milliseconds: 10));
    }
    expect(session.frameReady, isTrue);
    await session.capture(true);
    expect(session.captured, isTrue);
    socket.sent.clear();
  }

  Future<void> emit(Map<String, dynamic> event, {int? eventLease}) async {
    await messenger.handlePlatformMessage(
      'multitap/remote/input',
      const StandardMethodCodec().encodeSuccessEnvelope({
        'lease': eventLease ?? lease,
        ...event,
      }),
      (_) {},
    );
    await flush();
  }

  List<Map<String, dynamic>> get input =>
      socket.sent.where((v) => v['type'] == 'input').toList();

  Future<void> close() async {
    await session.close();
    messenger.setMockMethodCallHandler(_remote, null);
    messenger.setMockMethodCallHandler(_events, null);
    await flush();
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test('Granted ordinary session enables native committed text only while capturing', () async {
    final rig = _NativeRig(true);
    await rig.start();
    addTearDown(rig.close);
    final capture = rig.calls.lastWhere((c) => c.method == 'capture');
    expect(capture.arguments, containsPair('textInput', true));
    await rig.session.capture(false);
    expect(
      (rig.calls.lastWhere((c) => c.method == 'capture').arguments as Map)
          .containsKey('textInput'),
      isFalse,
    );
  });

  test('Native committed text preserves pointer ordering and physical keys without wire lease or samples', () async {
    final rig = _NativeRig(true);
    await rig.start();
    addTearDown(rig.close);
    final unbind = rig.session.bindPointerOrdering(
      beforeKey: () => rig.session.input({
        'type': 'input',
        'event': 'move',
        'x': .25,
        'y': .5,
      }),
      discard: () {},
    );
    addTearDown(unbind);
    await rig.emit({'type': 'input', 'event': 'text', 'text': '한😀'});
    expect(rig.input, [
      {'type': 'input', 'event': 'move', 'x': .25, 'y': .5},
      {'type': 'input', 'event': 'text', 'text': '한😀'},
    ]);
    await rig.emit({
      'type': 'input',
      'event': 'key',
      'vk': 13,
      'scan': 28,
      'down': true,
      'extended': false,
    });
    expect(rig.input.last, {
      'type': 'input',
      'event': 'key',
      'vk': 13,
      'scan': 28,
      'down': true,
      'extended': false,
    });
    expect(
      rig.input.any(
        (v) =>
            v.containsKey('lease') || v.containsKey('sample') || v['vk'] == 231,
      ),
      isFalse,
    );
  });

  test('Ungranted native text is dropped before pointer flushing while physical keys remain available', () async {
    final rig = _NativeRig(false);
    await rig.start();
    addTearDown(rig.close);
    var flushes = 0;
    rig.session.bindPointerOrdering(beforeKey: () => flushes++, discard: () {});
    expect(
      (rig.calls.lastWhere((c) => c.method == 'capture').arguments as Map)
          .containsKey('textInput'),
      isFalse,
    );
    await rig.emit({'type': 'input', 'event': 'text', 'text': 'x'});
    expect(rig.input, isEmpty);
    expect(flushes, 0);
    await rig.emit({
      'type': 'input',
      'event': 'key',
      'vk': 65,
      'scan': 30,
      'down': true,
      'extended': false,
    });
    expect(rig.input.single['vk'], 65);
    expect(flushes, 1);
  });

  test(
    'Native malformed text is rejected whole without pointer activity',
    () async {
      final rig = _NativeRig(true);
      await rig.start();
      addTearDown(rig.close);
      var flushes = 0;
      rig.session.bindPointerOrdering(
        beforeKey: () => flushes++,
        discard: () {},
      );
      for (final event in <Map<String, dynamic>>[
        {'type': 'input', 'event': 'text', 'text': ''},
        {'type': 'input', 'event': 'text', 'text': 'a\n'},
        {'type': 'input', 'event': 'text', 'text': 'a' * 257},
        {'type': 'input', 'event': 'text', 'text': 'x', 'sample': 1},
        {'type': 'input', 'event': 'text', 'text': 65},
      ]) {
        await rig.emit(event);
      }
      // StandardMessageCodec repairs a lone UTF16 surrogate during UTF8
      // encoding. Test that invalid in-memory boundary before such repair too.
      rig.session.input({
        'type': 'input',
        'event': 'text',
        'text': String.fromCharCode(0xd800),
      });
      expect(rig.input, isEmpty);
      expect(flushes, 0);
    },
  );

  test(
    'Retired native lease and capture loss cannot forward committed text',
    () async {
      final rig = _NativeRig(true);
      await rig.start();
      addTearDown(rig.close);
      await rig.emit({
        'type': 'input',
        'event': 'text',
        'text': 'old',
      }, eventLease: rig.lease + 1);
      expect(rig.input, isEmpty);
      await rig.session.capture(false);
      await rig.emit({'type': 'input', 'event': 'text', 'text': 'unfocused'});
      expect(rig.input, isEmpty);
      await rig.session.capture(true);
      await rig.emit({'type': 'input', 'event': 'text', 'text': 'a' * 256});
      expect(rig.input.single['text'], hasLength(256));
      await rig.session.close();
      await rig.emit({'type': 'input', 'event': 'text', 'text': 'closed'});
      expect(rig.input, hasLength(1));
    },
  );

  test('Strict text validation preserves Unicode boundaries and rejects whole malformed messages', () {
    Map<String, dynamic> text(String value) => {
      'type': 'input',
      'event': 'text',
      'text': value,
    };
    for (final value in ['A', '한', '😀', 'a' * 256, '😀' * 128]) {
      expect(validTextInputMessage(text(value)), isTrue);
    }
    for (final value in [
      '',
      '\x00',
      '\n',
      '\x7f',
      '\u0085',
      String.fromCharCode(0xd800),
      String.fromCharCode(0xdc00),
      '${String.fromCharCode(0xd800)}x',
      'a' * 257,
      '${'😀' * 128}a',
    ]) {
      expect(validTextInputMessage(text(value)), isFalse);
    }
    expect(validTextInputMessage({...text('x'), 'down': true}), isFalse);
    expect(
      validTextInputMessage({'type': 'input', 'event': 'text', 'value': 'x'}),
      isFalse,
    );
  });

  test('Pointer flush losing capture prevents the text observed before that loss from sending', () async {
    final rig = _NativeRig(true);
    await rig.start();
    addTearDown(rig.close);
    rig.session.bindPointerOrdering(
      beforeKey: () => unawaited(rig.session.capture(false)),
      discard: () {},
    );
    await rig.emit({'type': 'input', 'event': 'text', 'text': 'stale'});
    expect(rig.session.captured, isFalse);
    expect(rig.input, isEmpty);
  });

  for (final reset in [false, true]) {
    test(
      'Clipboard barrier ${reset ? 'reset discards' : 'preserves'} native text in physical key order',
      () async {
        final rig = _NativeRig(true);
        await rig.start();
        addTearDown(rig.close);
        final clipboard = _Clipboard();
        rig.session.clipboard = clipboard;
        Map<String, dynamic> key(int vk, bool down) => {
          'type': 'input',
          'event': 'key',
          'vk': vk,
          'scan': 0,
          'down': down,
        };
        await rig.emit(key(0x11, true));
        await rig.emit(key(0x56, true));
        expect(rig.session.preparingClipboardPaste, isTrue);
        await rig.emit({'type': 'input', 'event': 'text', 'text': 'queued'});
        await rig.emit(key(0x56, false));
        await rig.emit(key(0x11, false));
        expect(rig.input.where((v) => v['event'] == 'text'), isEmpty);
        if (reset) await rig.session.capture(false);
        clipboard.pasteCompletion.complete(true);
        await flush();
        if (reset) {
          expect(rig.input, [key(0x11, true)]);
        } else {
          expect(rig.input, [
            key(0x11, true),
            key(0x11, true),
            key(0x56, true),
            {'type': 'input', 'event': 'text', 'text': 'queued'},
            key(0x56, false),
            key(0x11, false),
          ]);
        }
      },
    );
  }

  test('A stale text-enabled native owner cannot enable capture on its replacement; default capture stays key-only', () async {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final calls = <MethodCall>[];
    messenger.setMockMethodCallHandler(_remote, (call) async {
      calls.add(call);
      if (call.method == 'start') return 42;
      if (call.method == 'capture') return true;
      return null;
    });
    addTearDown(() => messenger.setMockMethodCallHandler(_remote, null));
    final old = NativeRemoteMedia(textInputAllowed: true),
        next = NativeRemoteMedia();
    await old.start(1280, 720);
    await next.start(1280, 720);
    final before = calls.length;
    expect(await old.capture(true), isFalse);
    expect(calls, hasLength(before));
    expect(await next.capture(true), isTrue);
    expect((calls.last.arguments as Map).containsKey('textInput'), isFalse);
    await next.stop();
  });

  test('Independent native media never enables packet text even with a constructed text grant', () async {
    final rig = _NativeRig(true, track: true);
    await rig.start();
    addTearDown(rig.close);
    expect(rig.session.textInput, isFalse);
    expect(
      (rig.calls.lastWhere((c) => c.method == 'capture').arguments as Map)
          .containsKey('textInput'),
      isFalse,
    );
    await rig.emit({'type': 'input', 'event': 'text', 'text': 'denied'});
    expect(rig.input, isEmpty);
    await rig.emit({
      'type': 'input',
      'event': 'key',
      'vk': 13,
      'scan': 28,
      'down': true,
      'extended': false,
    });
    expect(rig.input.single['vk'], 13);
  });

  test('Default native signin capture stays text-disabled without changing existing phase-wrapped direct text', () async {
    final rig = _NativeRig(true, signin: true);
    await rig.start();
    addTearDown(rig.close);
    expect(
      (rig.calls.lastWhere((c) => c.method == 'capture').arguments as Map)
          .containsKey('textInput'),
      isFalse,
    );
    var flushes = 0;
    rig.session.bindPointerOrdering(beforeKey: () => flushes++, discard: () {});
    await rig.emit({'type': 'input', 'event': 'text', 'text': 'native-denied'});
    expect(flushes, 0);
    expect(rig.socket.sent.where((v) => v['type'] == 'input.phase'), isEmpty);
    rig.session.input({
      'type': 'input',
      'event': 'text',
      'text': 'direct-granted',
    });
    expect(rig.socket.sent.single['type'], 'input.phase');
    expect(rig.socket.sent.single['input'], {
      'type': 'input',
      'event': 'text',
      'text': 'direct-granted',
    });
  });
}

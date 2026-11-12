// The shared RemoteSession with the Android media engine, against a channel
// fake that applies the same envelope rules as the production Kotlin parser
// (AndroidMediaOwnership.kt, also run by the JVM check). No MediaCodec,
// device, relay or VM is involved.
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_media.dart';
import 'package:multitap_client/data/audio_stats.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';

import '../remote_session_test.dart' show SessionSocket, flush;

const _remote = MethodChannel('multitap/remote');
const _input = MethodChannel('multitap/remote/input');
const _media = 'multitap/remote/media';

/// The Android native media contract as AndroidMedia.kt answers it: control
/// methods on `multitap/remote`, media packets as media-wire-v1 on
/// `multitap/remote/media` (the released `push` method stays answered).
class AndroidNativeFake {
  AndroidNativeFake({this.preview3Envelope = false, this.fullAnswers = 0});

  /// preview.3 accepted only the exact two-key `{lease, bytes}` map, i.e. no
  /// watched packet.
  final bool preview3Envelope;

  /// The first this many media packets find the native queue full.
  int fullAnswers;
  final methods = <String>[];
  final pushes = <Map<Object?, Object?>>[];
  final frames = <List<int>>[];
  int lease = 0, decoded = 0, wirePackets = 0, fullAnswered = 0;

  /// audio-stats-v1 members of each `stats` answer. AndroidMedia.kt reports
  /// its AndroidAudioTrim counters, which count for the whole app.
  Map<String, Object> audio = const {};

  static ByteData _answer(int status) => ByteData(1)..setUint8(0, status);

  /// The checks of androidMediaWire/androidMediaPacketValid (Kotlin).
  ByteData _wire(ByteData? message) {
    // The session reuses its buffer once the engine has copied the message
    // (during send), so it is read before anything else.
    if (message == null) return _answer(4);
    final bytes = Uint8List.fromList(
      message.buffer.asUint8List(message.offsetInBytes, message.lengthInBytes),
    );
    final view = ByteData.sublistView(bytes);
    if (bytes.length < 16 + 13 ||
        bytes[0] != 1 ||
        bytes[1] & ~1 != 0 ||
        bytes.sublist(2, 8).any((b) => b != 0)) {
      return _answer(4);
    }
    final wireLease = view.getInt64(8, Endian.little);
    final watch = bytes[1] & 1 != 0;
    final packet = Uint8List.sublistView(bytes, 16);
    if ((watch && (packet[0] != 1 || preview3Envelope)) ||
        packet[2] != 0 ||
        packet[3] != 0 ||
        (packet[0] != 1 && packet[0] != 2)) {
      return _answer(4);
    }
    if (wireLease != lease) return _answer(2);
    if (fullAnswers > 0) {
      fullAnswers--;
      fullAnswered++;
      return _answer(1);
    }
    wirePackets++;
    pushes.add({'lease': wireLease, 'bytes': packet, if (watch) 'watch': true});
    if (packet[0] == 1) decoded++;
    return _answer(0);
  }

  void install() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(_input, (_) async => null);
    messenger.setMockMessageHandler(_media, (message) async => _wire(message));
    messenger.setMockMethodCallHandler(_remote, (call) async {
      methods.add(call.method);
      final args = (call.arguments as Map?) ?? const {};
      switch (call.method) {
        case 'start':
          lease = args['lease'] as int;
          decoded = 0;
          return 7;
        case 'push':
          final keys = args.keys.toSet();
          final watched = keys.contains('watch');
          final bytes = args['bytes'];
          if ((preview3Envelope ? keys.length != 2 : false) ||
              (watched && args['watch'] != true) ||
              keys.length != (watched ? 3 : 2) ||
              args['lease'] != lease ||
              bytes is! Uint8List ||
              bytes.length < 13 ||
              (watched && bytes[0] != 1)) {
            throw PlatformException(code: 'invalid_packet');
          }
          pushes.add(Map.of(args));
          if (bytes[0] == 1) decoded++;
          return null;
        case 'stats':
          final delivered = List<List<int>>.of(frames);
          frames.clear();
          return {
            'decodedFrames': decoded,
            'textureFrames': decoded,
            'error': 0,
            'audioError': 0,
            'audioPlayedFrames': 0,
            'inputFrames': delivered,
            ...audio,
          };
        case 'capture':
          return args['enabled'] == true;
        case 'stop':
          return null;
      }
      // Kotlin answers result.notImplemented() for anything else.
      throw MissingPluginException();
    });
  }

  static void uninstall() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(_remote, null);
    messenger.setMockMethodCallHandler(_input, null);
    messenger.setMockMessageHandler(_media, null);
  }
}

Uint8List _video(int pts, {bool key = false}) {
  final bytes = Uint8List(17);
  bytes[0] = 1;
  bytes[1] = key ? 1 : 0;
  for (var i = 0; i < 8; i++) {
    bytes[4 + i] = (pts >> (8 * i)) & 255;
  }
  bytes.setAll(12, [0, 0, 0, 1, 0x65]);
  return bytes;
}

/// One cursor-v1 binary message (media kind 3, epoch 0).
Uint8List _cursor(List<int> record) =>
    Uint8List.fromList([3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, ...record]);

// A 1x1 monochrome shape (id 1) and a visible state naming it.
const _shape = [
  1,
  2,
  0,
  0,
  1,
  0,
  0,
  0,
  1,
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
];
const _visible = [2, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];

Future<(RemoteSession, SessionSocket)> _started(
  RemoteMedia media, {
  void Function(RemoteSession)? attach,
}) async {
  final socket = SessionSocket();
  final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: 'android-session',
      webSocketUrl: Uri.parse(
        'wss://fixture.invalid/api/v1/sessions/android-session',
      ),
      inputLatency: true,
      cursor: true,
    ),
    token: 'not-persisted',
    media: media,
    dialer: (_, _) async => socket,
  );
  attach?.call(session);
  unawaited(session.start());
  await flush();
  socket.incoming.add(
    jsonEncode({
      'type': 'stream.info',
      'codec': 'h264',
      'width': 1280,
      'height': 720,
    }),
  );
  await flush();
  socket.incoming.add(_video(16667, key: true));
  await flush();
  // A refused packet is retried after short real waits; on a loaded machine
  // those take longer than a fixed sleep, so wait for the first frame itself.
  for (var n = 0; n < 60 && !session.frameReady && !session.closed; n++) {
    await Future<void>.delayed(const Duration(milliseconds: 50));
  }
  // The first-frame check also reads statistics once more 50 ms later (a
  // readiness-only read that does not deliver input frames); let it pass.
  await Future<void>.delayed(const Duration(milliseconds: 120));
  return (session, socket);
}

Future<void> _tagNextFrame(RemoteSession session, SessionSocket socket) async {
  await session.capture(true);
  session.input({
    'type': 'input',
    'event': 'button',
    'button': 'left',
    'down': true,
    'x': .5,
    'y': .5,
  });
  await flush();
  socket.incoming.add(
    jsonEncode({
      'type': 'input.ack',
      'sessionId': 'android-session',
      'sample': 1,
      'ok': true,
      'injectUs': 400,
      'holdUs': 900,
    }),
  );
  socket.incoming.add(
    jsonEncode({
      'type': 'input.frame',
      'sessionId': 'android-session',
      'epoch': 0,
      'sample': 1,
      'pts': 33334,
    }),
  );
  socket.incoming.add(_video(33334));
  await flush();
  await flush();
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(AndroidNativeFake.uninstall);

  test(
    'Android media: first frame, separated cursor and a watched packet',
    () async {
      final native = AndroidNativeFake()..install();
      final media = AndroidRemoteMedia();
      addTearDown(media.dispose);
      final (session, socket) = await _started(media);
      expect(session.frameReady, isTrue, reason: 'decodedFrames from stats');
      expect(session.textureId, 7);
      expect(session.cursorSeparated, isTrue);

      socket.incoming.add(_cursor(_shape));
      socket.incoming.add(_cursor(_visible));
      await flush();
      expect(session.closed, isFalse);
      expect(media.cursorState.visible, isTrue);
      expect(native.methods.contains('cursor'), isFalse);

      await _tagNextFrame(session, socket);
      expect(session.closed, isFalse, reason: session.diagnosticCode);
      final watched = native.pushes.where((push) => push['watch'] == true);
      expect(watched, hasLength(1));
      // Packets travel as media-wire-v1, never through the `push` method.
      expect(native.methods.contains('push'), isFalse);
      expect(native.wirePackets, native.pushes.length);
      expect(native.pushes.every((push) => push['lease'] == native.lease), isTrue);
      native.frames.add([33334, 2500]);
      await Future<void>.delayed(const Duration(milliseconds: 1150));
      expect(session.inputToFrameP50Ms, isNotNull);
      expect(session.inputLatency.frameSupported, isTrue);
      await session.close();
    },
  );

  test(
    'regression: the preview.3 envelope check ends a measured session',
    () async {
      AndroidNativeFake(preview3Envelope: true).install();
      final media = AndroidRemoteMedia();
      addTearDown(media.dispose);
      final (session, socket) = await _started(media);
      expect(session.frameReady, isTrue);
      await _tagNextFrame(session, socket);
      expect(session.closed, isTrue);
      expect(session.diagnosticCode, isNotNull);
    },
  );

  test('a full native queue is waited out with the same packet', () async {
    final native = AndroidNativeFake(fullAnswers: 3)..install();
    final media = AndroidRemoteMedia();
    addTearDown(media.dispose);
    final (session, socket) = await _started(media);
    expect(session.closed, isFalse, reason: session.diagnosticCode);
    expect(native.fullAnswered, 3);
    expect(session.frameReady, isTrue);
    // The refused key frame arrived once, intact, after the waits.
    expect(native.pushes, hasLength(1));
    expect(native.pushes.single['bytes'], _video(16667, key: true));
    socket.incoming.add(_video(33334));
    await flush();
    expect(native.pushes, hasLength(2));
    await session.close();
  });

  test('a missing media channel ends the session, never drops packets', () async {
    AndroidNativeFake().install();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMessageHandler('multitap/remote/media', (_) async => null);
    final media = AndroidRemoteMedia();
    addTearDown(media.dispose);
    final (session, _) = await _started(media);
    expect(session.closed, isTrue);
    expect(session.diagnosticCode, 'media_platform_error');
  });

  test('a packet of a replaced session is refused by its lease', () async {
    final native = AndroidNativeFake()..install();
    final media = AndroidRemoteMedia();
    addTearDown(media.dispose);
    final (session, socket) = await _started(media);
    expect(session.closed, isFalse);
    // The engine moved on to another session (a newer lease).
    native.lease++;
    socket.incoming.add(_video(33334));
    await flush();
    await flush();
    expect(session.closed, isTrue);
    expect(native.pushes, hasLength(1));
  });

  test(
    'regression: the shared native cursor call ends the session on Android',
    () async {
      final native = AndroidNativeFake()..install();
      final (session, socket) = await _started(NativeRemoteMedia());
      expect(session.frameReady, isTrue);
      socket.incoming.add(_cursor(_shape));
      await flush();
      await flush();
      expect(native.methods.contains('cursor'), isTrue);
      expect(session.closed, isTrue);
    },
  );

  test(
    'regression: a new Android session does not report the app audio totals again',
    () async {
      // The first streamStats window of a phone session must not re-send the
      // app's totals: a session that starts a tracker without baselines
      // would, because AndroidAudioTrim keeps counting for the whole app.
      final native = AndroidNativeFake()..install();
      Map<String, Object> counters(int underruns) => {
        'audioEnabled': true,
        'audioPlaying': false,
        'audioTargetMs': 40.0,
        'audioUnderruns': underruns,
        'audioOverruns': 22,
        'audioTrimmedFrames': 960,
        'audioInsertedFrames': 0,
      };
      // Every 1 s reading of a session, in order (each poll notifies once).
      List<AudioStatsReading> readings(RemoteSession session) {
        final seen = <AudioStatsReading>[];
        session.addListener(() {
          final tick = session.audioTick;
          if (tick != null && (seen.isEmpty || !identical(seen.last, tick))) {
            seen.add(tick);
          }
        });
        return seen;
      }

      Future<void> another(List<AudioStatsReading> seen) async {
        final count = seen.length;
        for (var n = 0; n < 60 && seen.length == count; n++) {
          await Future<void>.delayed(const Duration(milliseconds: 50));
        }
        expect(seen.length, greaterThan(count), reason: 'no statistics poll');
      }

      int sum(List<AudioStatsReading> seen, int Function(AudioStatsReading) f) =>
          seen.fold(0, (total, reading) => total + f(reading));

      native.audio = counters(39);
      final first = AndroidRemoteMedia();
      addTearDown(first.dispose);
      late List<AudioStatsReading> one;
      final (session1, _) = await _started(
        first,
        attach: (s) => one = readings(s),
      );
      await another(one);
      expect(session1.closed, isFalse, reason: session1.diagnosticCode);
      expect(
        sum(one, (r) => r.underruns),
        39,
        reason: 'the first reading of the app counts everything so far',
      );
      await session1.close();

      final second = AndroidRemoteMedia();
      addTearDown(second.dispose);
      late List<AudioStatsReading> two;
      final (session2, _) = await _started(
        second,
        attach: (s) => two = readings(s),
      );
      await another(two);
      expect(session2.closed, isFalse, reason: session2.diagnosticCode);
      expect(
        sum(two, (r) => r.underruns),
        0,
        reason: 'nothing happened since the previous session read 39',
      );
      expect(sum(two, (r) => r.overruns), 0);
      expect(sum(two, (r) => r.trimmedFrames), 0);
      native.audio = counters(41);
      await another(two);
      expect(sum(two, (r) => r.underruns), 2, reason: 'only the new underruns');
      await session2.close();
    },
  );
}

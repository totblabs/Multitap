import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/android_watchdog_observation.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';

class SessionSocket extends Fake implements WebSocket {
  final incoming = StreamController<dynamic>();
  final sent = <Map<String, dynamic>>[];
  bool wasClosed = false;
  @override
  int? closeCode;
  @override
  String? closeReason;
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
  void add(dynamic data) =>
      sent.add(jsonDecode(data as String) as Map<String, dynamic>);
  @override
  Future<void> close([int? code, String? reason]) async {
    wasClosed = true;
    unawaited(incoming.close());
  }
}

class SessionMedia implements RemoteMedia {
  SessionMedia({this.expectedWidth = 1280, this.expectedHeight = 720});
  final int expectedWidth, expectedHeight;
  final events = StreamController<Map<String, dynamic>>.broadcast();
  final packets = <Uint8List>[];
  final captures = <bool>[];
  bool stopped = false, rejectPackets = false, alwaysFull = false;
  int starts = 0;

  /// The next N pushes find the native decode queue full (backpressure).
  int fullPushes = 0, fullAnswers = 0;
  @override
  Stream<Map<String, dynamic>> get input => events.stream;
  @override
  Future<int> start(int width, int height) async {
    expect(width, expectedWidth);
    expect(height, expectedHeight);
    starts++;
    return 42;
  }

  @override
  Future<void> push(Uint8List packet) async {
    if (rejectPackets) throw PlatformException(code: 'media_backpressure');
    if (alwaysFull || fullPushes > 0) {
      if (fullPushes > 0) fullPushes--;
      fullAnswers++;
      throw PlatformException(code: 'media_queue_full');
    }
    packets.add(packet);
  }

  @override
  Future<bool> capture(bool enabled) async {
    captures.add(enabled);
    return enabled;
  }

  @override
  Future<Map<String, dynamic>> stats() async => {
    'decodedFrames': packets.length,
    'error': 0,
    'audioError': -1,
    'audioPlayedFrames': 0,
  };
  @override
  Future<void> stop() async {
    stopped = true;
  }
}

const info = {
  'type': 'stream.info',
  'codec': 'h264',
  'width': 1280,
  'height': 720,
};
Future<void> flush() => Future<void>.delayed(const Duration(milliseconds: 5));
Uint8List packet() =>
    Uint8List.fromList([1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x65]);

void main() {
  test('Android watchdog is opt-in and transaction/initial observation stays bounded', () {
    final observation = AndroidWatchdogObservation();
    expect(observation.stalled(16000000, 0, eligible: true), isTrue);
    observation.setForeground(false, 1000000);
    expect(observation.stalled(90000000, 0, eligible: true), isFalse);
    expect(observation.deferStatsFailure(90000000, eligible: true), isTrue);
    // Not-ready, multi-output and resize callers do not opt out of deadlines.
    expect(observation.stalled(90000000, 0, eligible: false), isTrue);
    expect(observation.deferStatsFailure(90000000, eligible: false), isFalse);
  });

  test(
    'Android resume uses a bounded observation floor, not manufactured samples',
    () {
      final observation = AndroidWatchdogObservation();
      observation.setForeground(false, 1000000);
      observation.setForeground(true, 90000000);
      expect(observation.stalled(104000000, 0, eligible: true), isFalse);
      expect(observation.stalled(105000001, 0, eligible: true), isTrue);
      expect(observation.deferStatsFailure(105000001, eligible: true), isFalse);
      // A genuinely fresh sample extends only its own watchdog.
      expect(
        observation.stalled(105000001, 103000000, eligible: true),
        isFalse,
      );
      // Duplicate foreground notifications must not extend the grace forever.
      observation.setForeground(true, 105000000);
      expect(observation.stalled(105000001, 0, eligible: true), isTrue);
    },
  );

  test('Android lifecycle epochs retire old stats and repeated background visits rearm once', () {
    final observation = AndroidWatchdogObservation();
    observation.setForeground(true, 0);
    final foreground = observation.epoch;
    observation.setForeground(false, 1000000);
    expect(observation.current(foreground), isFalse);
    final background = observation.epoch;
    observation.setForeground(true, 2000000);
    expect(observation.current(background), isFalse);
    observation.setForeground(false, 3000000);
    observation.setForeground(true, 90000000);
    expect(observation.stalled(104000000, 0, eligible: true), isFalse);
    expect(observation.stalled(105000001, 0, eligible: true), isTrue);
  });

  test('Legacy response retains the requested audio-off settings and only a stream connection is accepted', () {
    final data = {
      'sessionId': 'test-session',
      'webSocketUrl': 'wss://cloud.example.com/api/v1/sessions/test-session',
    };
    final connection = BrokerConnection.fromJson(
      data,
      Uri.parse('https://cloud.example.com'),
      requestedOptions: const StreamOptions(
        maxWidth: 1280,
        maxHeight: 720,
        audio: false,
      ),
    );
    expect(connection.options.audio, isFalse);
    expect(connection.options.maxWidth, 1280);
    expect(
      () => BrokerConnection.fromJson({
        ...data,
        'mode': 'other',
      }, Uri.parse('https://cloud.example.com')),
      throwsA(isA<BrokerException>()),
    );
  });
  test('Connect accepts default WSS443 and configured ports without leaking bearer tokens to another origin', () {
    for (final pair in [
      [
        'https://cloud.example.com',
        'wss://cloud.example.com/api/v1/sessions/test-session',
      ],
      [
        'https://cloud.example.com',
        'wss://cloud.example.com:443/api/v1/sessions/test-session',
      ],
      [
        'https://cloud.example.com:18443',
        'wss://cloud.example.com:18443/api/v1/sessions/test-session',
      ],
    ]) {
      expect(
        BrokerConnection.fromJson({
          'sessionId': 'test-session',
          'webSocketUrl': pair[1],
        }, Uri.parse(pair[0])).sessionId,
        'test-session',
      );
    }
    for (final unsafe in [
      'ws://cloud.example.com/api/v1/sessions/test-session',
      'wss://other.example.com/api/v1/sessions/test-session',
      'wss://cloud.example.com:18443/api/v1/sessions/test-session',
      'wss://cloud.example.com/api/v1/sessions/test-session?token=forbidden',
      'wss://user:secret@cloud.example.com/api/v1/sessions/test-session',
    ]) {
      expect(
        () => BrokerConnection.fromJson({
          'sessionId': 'test-session',
          'webSocketUrl': unsafe,
        }, Uri.parse('https://cloud.example.com')),
        throwsA(isA<BrokerException>()),
      );
    }
  });
  testWidgets(
    'Late cleanup of an old session cannot stop or release a newly opened native session',
    (tester) async {
      const channel = MethodChannel('multitap/remote');
      final methods = <String>[];
      var texture = 0;
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (call) async {
            methods.add(call.method);
            if (call.method == 'start') return ++texture;
            if (call.method == 'capture') return true;
            return null;
          });
      addTearDown(
        () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(channel, null),
      );
      final first = NativeRemoteMedia(), second = NativeRemoteMedia();
      await first.start(1280, 720);
      await second.start(1280, 720);
      await first.capture(false);
      await first.stop();
      expect(methods, ['start', 'start']);
      await second.capture(true);
      await second.push(packet());
      await second.stop();
      expect(methods, ['start', 'start', 'capture', 'push', 'stop']);
    },
  );
  RemoteSession make(
    SessionSocket socket,
    SessionMedia media, {
    StreamOptions options = const StreamOptions(),
    Duration mediaQueueWait = const Duration(seconds: 3),
  }) => RemoteSession(
    mediaQueueWait: mediaQueueWait,
    connection: BrokerConnection(
      sessionId: 'test-session',
      webSocketUrl: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/test-session',
      ),
      options: options,
    ),
    token: 'test-session-token',
    media: media,
    dialer: (uri, token) async {
      expect(uri.query, isEmpty);
      expect(token, 'test-session-token');
      return socket;
    },
  );

  test(
    'Host lossless tile stats are exposed only when the host reports them',
    () async {
      final socket = SessionSocket(), media = SessionMedia();
      final session = make(socket, media);
      unawaited(session.start());
      await flush();
      socket.incoming.add(jsonEncode(info));
      await flush();
      const stats = {
        'type': 'stream.stats',
        'capturedFps': 58.2,
        'encodedFps': 58,
        'bitrateKbps': 8560.4,
        'measuredMs': 1012,
      };
      socket.incoming.add(
        jsonEncode({
          ...stats,
          'lossless': {
            'protocol': 'tiles-v1',
            'presentTiles': 412,
            'packets': 3,
            'kbps': 2400.5,
            'tileMs': 7.2,
          },
        }),
      );
      await flush();
      expect(session.losslessTiles, 412);
      expect(session.losslessKbps, 2400.5);
      socket.incoming.add(jsonEncode(stats));
      await flush();
      expect(session.losslessTiles, isNull, reason: 'No report means no tiles');
      await session.close();
      await media.events.close();
    },
  );

  test(
    'A VM without a playback device says so instead of waiting for sound',
    () async {
      for (final available in [false, true]) {
        final socket = SessionSocket(), media = SessionMedia();
        final session = make(socket, media);
        unawaited(session.start());
        await flush();
        socket.incoming.add(
          jsonEncode({
            ...info,
            'audio': {
              'sampleRate': 48000,
              'channels': 2,
              'format': 's16le',
              'available': available,
            },
          }),
        );
        await flush();
        expect(session.remoteAudioMissing, !available);
        expect(
          session.audioStatus,
          available ? '수신 대기' : RemoteSession.remoteAudioMissingStatus,
        );
        await session.close();
        await media.events.close();
      }
    },
  );

  test('Relay media configures native texture and focused input resets before disconnect', () async {
    final socket = SessionSocket(), media = SessionMedia();
    final session = make(socket, media);
    unawaited(session.start());
    await flush();
    socket.incoming.add(jsonEncode(info));
    await flush();
    expect(session.textureId, 42);
    socket.incoming.add(packet());
    await flush();
    expect(media.packets.single, packet());
    session.input({
      'type': 'input',
      'event': 'key',
      'vk': 91,
      'scan': 0,
      'down': true,
      'extended': true,
    });
    expect(
      socket.sent,
      isEmpty,
      reason: 'Unfocused pointer input is not forwarded',
    );
    await session.capture(true);
    media.events.add({
      'type': 'input',
      'event': 'key',
      'vk': 91,
      'scan': 0,
      'down': true,
      'extended': true,
    });
    await flush();
    expect(socket.sent.last['vk'], 91);
    await session.capture(false);
    expect(socket.sent.last['type'], 'input.reset');
    await session.close();
    expect(media.captures.last, isFalse);
    expect(media.stopped, isTrue);
    expect(socket.wasClosed, isTrue);
    expect(socket.sent.last['type'], 'session.stop');
    await media.events.close();
  });

  test(
    'Malformed or oversized media closes the relay and restores local input',
    () async {
      for (final invalid in [
        Uint8List(4),
        Uint8List(4 * 1024 * 1024 + 1),
        packet()..[2] = 1,
      ]) {
        final socket = SessionSocket(), media = SessionMedia();
        final session = make(socket, media);
        unawaited(session.start());
        await flush();
        socket.incoming.add(jsonEncode(info));
        await flush();
        await session.capture(true);
        socket.incoming.add(invalid);
        await flush();
        expect(session.closed, isTrue);
        expect(media.stopped, isTrue);
        expect(media.captures.last, isFalse);
        expect(media.packets, isEmpty);
        await media.events.close();
      }
    },
  );

  test(
    'Native bounded-queue failure terminates instead of accumulating frames',
    () async {
      final socket = SessionSocket(),
          media = SessionMedia()..rejectPackets = true;
      final session = make(socket, media);
      unawaited(session.start());
      await flush();
      socket.incoming.add(jsonEncode(info));
      await flush();
      await session.capture(true);
      socket.incoming.add(packet());
      await flush();
      expect(session.closed, isTrue);
      expect(session.error, isNotNull);
      expect(media.stopped, isTrue);
      expect(media.captures.last, isFalse);
      await media.events.close();
    },
  );

  test('A full native decode queue is backpressure: the same packets are offered again in order and the stream stays live', () async {
    // Production diagnostics: streams ended as media_backpressure when a
    // burst after a network stall filled the eight-packet decode queue.
    final socket = SessionSocket(), media = SessionMedia()..fullPushes = 3;
    final session = make(socket, media);
    unawaited(session.start());
    await flush();
    socket.incoming.add(jsonEncode(info));
    await flush();
    final first = packet(), second = packet()..[16] = 0x41;
    socket.incoming
      ..add(first)
      ..add(second);
    await Future<void>.delayed(const Duration(milliseconds: 60));
    expect(media.fullAnswers, 3);
    expect(media.packets, [first, second]);
    expect(session.closed, isFalse);
    expect(session.error, isNull);
    expect(session.diagnosticCode, isNull);
    await session.close();
    await media.events.close();
  });

  test('A decode queue that stays full ends the stream as media_backpressure instead of waiting forever', () async {
    final socket = SessionSocket(), media = SessionMedia()..alwaysFull = true;
    final session = make(
      socket,
      media,
      mediaQueueWait: const Duration(milliseconds: 40),
    );
    unawaited(session.start());
    await flush();
    socket.incoming.add(jsonEncode(info));
    await flush();
    await session.capture(true);
    socket.incoming.add(packet());
    await Future<void>.delayed(const Duration(milliseconds: 300));
    expect(session.closed, isTrue);
    expect(session.diagnosticCode, 'media_backpressure');
    expect(media.fullAnswers, greaterThan(1));
    expect(media.packets, isEmpty);
    expect(media.stopped, isTrue);
    expect(media.captures.last, isFalse);
    await media.events.close();
  });

  test('Audio endpoint failure is reported while video and measured RTT remain live', () async {
    final socket = SessionSocket(), media = SessionMedia();
    final session = make(socket, media);
    unawaited(session.start());
    await flush();
    socket.incoming.add(jsonEncode(info));
    await flush();
    socket.incoming.add(packet());
    await Future<void>.delayed(const Duration(milliseconds: 1050));
    expect(session.closed, isFalse);
    expect(session.audioStatus, contains('출력 장치'));
    expect(session.decodedFps, isNotNull);
    expect(session.receivedMbps, greaterThan(0));
    final ping = socket.sent.singleWhere((event) => event['type'] == 'ping');
    socket.incoming.add(jsonEncode({'type': 'pong', 'id': ping['id']}));
    await flush();
    expect(session.rttMs, greaterThanOrEqualTo(0));
    socket.incoming.add(jsonEncode({'type': 'session.ended'}));
    await flush();
    expect(session.closed, isTrue);
    expect(media.stopped, isTrue);
    expect(media.captures.last, isFalse);
    await media.events.close();
  });

  test('Dual composite stream initializes the native dimensions while oversized dimensions are rejected before allocation', () async {
    final socket = SessionSocket(),
        media = SessionMedia(expectedWidth: 3840, expectedHeight: 1080);
    final session = make(socket, media);
    unawaited(session.start());
    await flush();
    socket.incoming.add(
      jsonEncode({
        ...info,
        'width': 3840,
        'height': 1080,
        'sourceWidth': 3840,
        'sourceHeight': 1080,
      }),
    );
    await flush();
    expect(session.textureId, 42);
    expect(session.width, 3840);
    expect(session.sourceWidth, 3840);
    await session.close();
    await media.events.close();
    final invalidSocket = SessionSocket(), invalidMedia = SessionMedia();
    final invalid = make(invalidSocket, invalidMedia);
    unawaited(invalid.start());
    await flush();
    invalidSocket.incoming.add(
      jsonEncode({...info, 'width': 3842, 'height': 2160}),
    );
    await flush();
    expect(invalid.closed, isTrue);
    expect(invalidMedia.starts, 0);
    await invalidMedia.events.close();
  });

  test('Audio-off drops received PCM before the native playback queue while retaining truthful received-byte accounting', () async {
    final socket = SessionSocket(), media = SessionMedia();
    final session = make(
      socket,
      media,
      options: const StreamOptions(audio: false),
    );
    unawaited(session.start());
    await flush();
    socket.incoming.add(jsonEncode(info));
    await flush();
    final audio = Uint8List(16)..[0] = 2;
    socket.incoming.add(audio);
    await flush();
    expect(media.packets, isEmpty);
    expect(session.receivedBytes, audio.length);
    await session.close();
    await media.events.close();
  });
}

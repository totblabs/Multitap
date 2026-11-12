import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';

class _Socket extends Fake implements WebSocket {
  final incoming = StreamController<dynamic>();
  final sent = <Map<String, dynamic>>[];
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
    unawaited(incoming.close());
  }
}

/// A decoder whose queue can stall, like a saturated native media engine.
class _StallingMedia implements RemoteMedia {
  final events = StreamController<Map<String, dynamic>>.broadcast();
  final packets = <Uint8List>[];
  Completer<void>? stall;
  int blocked = 0;
  @override
  Stream<Map<String, dynamic>> get input => events.stream;
  @override
  Future<int> start(int width, int height) async => 42;
  @override
  Future<void> push(Uint8List packet) async {
    packets.add(packet);
    final gate = stall;
    if (gate == null) return;
    blocked++;
    await gate.future;
    blocked--;
  }

  @override
  Future<bool> capture(bool enabled) async => enabled;
  @override
  Future<Map<String, dynamic>> stats() async => {
    'decodedFrames': packets.length,
    'error': 0,
    'audioError': 0,
    'audioPlayedFrames': 0,
  };
  @override
  Future<void> stop() async {}
}

Uint8List _keyFrame() =>
    Uint8List.fromList([1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x65]);
Future<void> _settle() => Future<void>.delayed(const Duration(milliseconds: 5));

void main() {
  test(
    'Input is sent immediately and in order while a video push is stalled behind decoder backpressure',
    () async {
      final socket = _Socket(), media = _StallingMedia();
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'latency-session',
          webSocketUrl: Uri.parse(
            'wss://cloud.example.com/api/v1/sessions/latency-session',
          ),
          options: const StreamOptions(),
        ),
        token: 'latency-session-token',
        media: media,
        dialer: (_, _) async => socket,
      );
      unawaited(session.start());
      await _settle();
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.info',
          'codec': 'h264',
          'width': 1280,
          'height': 720,
        }),
      );
      await _settle();
      socket.incoming.add(_keyFrame());
      await _settle();
      expect(session.frameReady, isTrue);
      await session.capture(true);
      expect(session.captured, isTrue);

      media.stall = Completer<void>();
      socket.incoming.add(_keyFrame());
      socket.incoming.add(_keyFrame());
      await _settle();
      expect(media.blocked, 1, reason: 'The second video push is stalled.');
      expect(
        media.packets,
        hasLength(2),
        reason: 'Media keeps its own ordered backpressure.',
      );

      final before = socket.sent.length;
      final events = <Map<String, dynamic>>[
        {'type': 'input', 'event': 'move', 'x': 0.25, 'y': 0.5},
        {
          'type': 'input',
          'event': 'button',
          'button': 'left',
          'down': true,
          'x': 0.25,
          'y': 0.5,
        },
        {'type': 'input', 'event': 'move', 'x': 0.75, 'y': 0.5},
        {
          'type': 'input',
          'event': 'button',
          'button': 'left',
          'down': false,
          'x': 0.75,
          'y': 0.5,
        },
        {
          'type': 'input',
          'event': 'key',
          'vk': 0x41,
          'scan': 0x1e,
          'down': true,
          'extended': false,
        },
      ];
      for (final event in events) {
        media.events.add(event);
      }
      // One event-loop turn; no video push has completed in between.
      await Future<void>.delayed(Duration.zero);
      expect(media.blocked, 1);
      expect(socket.sent.sublist(before), events);

      media.stall!.complete();
      media.stall = null;
      await _settle();
      expect(media.packets, hasLength(3));
      await session.close();
      await media.events.close();
    },
  );

  test('A ping answer is timed on arrival, not after the stalled media pushes ahead of it', () async {
    final socket = _Socket(), media = _StallingMedia();
    final session = RemoteSession(
      connection: BrokerConnection(
        sessionId: 'latency-session',
        webSocketUrl: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/latency-session',
        ),
        options: const StreamOptions(),
      ),
      token: 'latency-session-token',
      media: media,
      dialer: (_, _) async => socket,
    );
    unawaited(session.start());
    await _settle();
    socket.incoming.add(
      jsonEncode({
        'type': 'stream.info',
        'codec': 'h264',
        'width': 1280,
        'height': 720,
      }),
    );
    await _settle();
    socket.incoming.add(_keyFrame());
    await _settle();
    expect(session.frameReady, isTrue);
    // The once-a-second poll sends a ping.
    Map<String, dynamic>? ping;
    for (var n = 0; n < 40 && ping == null; n++) {
      await Future<void>.delayed(const Duration(milliseconds: 50));
      for (final message in socket.sent) {
        if (message['type'] == 'ping') ping = message;
      }
    }
    expect(ping, isNotNull);
    media.stall = Completer<void>();
    socket.incoming.add(_keyFrame());
    socket.incoming.add(_keyFrame());
    await _settle();
    expect(media.blocked, 1, reason: 'The reader is stuck behind a media push.');
    socket.incoming.add(jsonEncode({'type': 'pong', 'id': ping!['id']}));
    await _settle();
    expect(media.blocked, 1, reason: 'Still stuck.');
    expect(session.rttMs, isNotNull, reason: 'Answered at arrival.');
    media.stall!.complete();
    media.stall = null;
    await _settle();
    await session.close();
    await media.events.close();
  });
  test('A failing stalled push still releases remote input before closing', () async {
    final socket = _Socket(), media = _StallingMedia();
    final session = RemoteSession(
      connection: BrokerConnection(
        sessionId: 'latency-session',
        webSocketUrl: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/latency-session',
        ),
        options: const StreamOptions(),
      ),
      token: 'latency-session-token',
      media: media,
      dialer: (_, _) async => socket,
    );
    unawaited(session.start());
    await _settle();
    socket.incoming.add(
      jsonEncode({
        'type': 'stream.info',
        'codec': 'h264',
        'width': 1280,
        'height': 720,
      }),
    );
    await _settle();
    socket.incoming.add(_keyFrame());
    await _settle();
    await session.capture(true);
    media.stall = Completer<void>();
    socket.incoming.add(_keyFrame());
    await _settle();
    media.stall!.completeError(PlatformException(code: 'media_backpressure'));
    await _settle();
    expect(session.closed, isTrue);
    expect(socket.sent.last['type'], 'session.stop');
    expect(
      socket.sent.where((event) => event['type'] == 'input.reset'),
      isNotEmpty,
    );
    await media.events.close();
  });
}

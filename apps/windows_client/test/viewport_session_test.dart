import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, flush, packet;

class ResizeMedia extends SessionMedia {
  final dimensions = <List<int>>[];
  int _decoded = 0;
  @override
  Future<int> start(int width, int height) async {
    dimensions.add([width, height]);
    _decoded = 0;
    return 40 + dimensions.length;
  }

  @override
  Future<void> push(Uint8List packet) async {
    await super.push(packet);
    if (packet[0] == 1) _decoded++;
  }

  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    'decodedFrames': _decoded,
  };
}

class DeferredCaptureMedia extends ResizeMedia {
  Completer<void>? release;

  @override
  Future<bool> capture(bool enabled) async {
    final waiting = release;
    if (!enabled && waiting != null) await waiting.future;
    return super.capture(enabled);
  }
}

const startInfo = {
  'type': 'stream.info',
  'codec': 'h264',
  'width': 1280,
  'height': 720,
  'sourceWidth': 1280,
  'sourceHeight': 720,
  'epoch': 0,
  'fpsTarget': 60,
};
final connection = BrokerConnection(
  sessionId: 'resize-session',
  webSocketUrl: Uri(
    scheme: 'wss',
    host: 'cloud.example',
    path: '/api/v1/sessions/resize-session',
  ),
  epochMedia: true,
  viewportProvisioning: true,
);
Map<String, dynamic> resizeAck(String id) => {
  'type': 'stream.resizing',
  'requestId': id,
  'resizeId': 'matching-resize-id',
  'epoch': 1,
  'width': 1346,
  'height': 812,
  'fps': 60,
  'monitor': '0',
  'timeoutMs': 90000,
};
Map<String, dynamic> applied(String id) => {
  'type': 'stream.info',
  'codec': 'h264',
  'sessionId': 'resize-session',
  'requestId': id,
  'resizeId': 'matching-resize-id',
  'epoch': 1,
  'width': 1346,
  'height': 812,
  'sourceWidth': 1346,
  'sourceHeight': 812,
  'fpsTarget': 60,
};

void main() {
  test('a stale viewport is cancelled after deferred capture even after '
      'foreground return; the same size remains retryable', () async {
    final socket = SessionSocket(), media = DeferredCaptureMedia();
    final session = RemoteSession(
      connection: connection,
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    socket.incoming.add(jsonEncode(startInfo));
    await flush();
    socket.incoming.add(packet());
    await flush();
    var revision = 0, dispatched = 0;
    final expected = revision;
    media.release = Completer<void>();
    final cancelled = session.resizeViewport(
      1346,
      812,
      isCurrent: () => revision == expected,
      onDispatched: () => dispatched++,
    );
    await flush();
    // Pause, then return (or change geometry away and back) before the
    // original native capture release completes: equality is insufficient.
    revision += 2;
    media.release!.complete();
    expect(await cancelled, isFalse);
    expect(dispatched, 0);
    expect(session.resizing, isFalse);
    expect(socket.sent.where((m) => m['type'] == 'stream.resize'), isEmpty);
    expect(media.captures.where((enabled) => enabled), isEmpty);
    media.release = null;
    final retry = session.resizeViewport(
      1346,
      812,
      isCurrent: () => true,
      onDispatched: () => dispatched++,
    );
    await flush();
    expect(dispatched, 1);
    expect(
      socket.sent.where((m) => m['type'] == 'stream.resize'),
      hasLength(1),
    );
    await session.close();
    expect(await retry, isFalse);
    await running;
  });
  test('Epoch media is negotiated explicitly and does not loosen same-origin validation', () {
    final response = {
      'sessionId': 'resize-session',
      'webSocketUrl': 'wss://cloud.example/api/v1/sessions/resize-session',
      'mode': 'stream',
      'transport': 'media-epoch-v1',
      'resizeProtocol': 'epoch-v1',
      'viewportProvisioning': true,
    };
    final parsed = BrokerConnection.fromJson(
      response,
      Uri.parse('https://cloud.example'),
    );
    expect(parsed.epochMedia, true);
    expect(parsed.viewportProvisioning, true);
    for (final delta in [
      {'resizeProtocol': 'unknown'},
      {'webSocketUrl': 'wss://foreign.example/api/v1/sessions/resize-session'},
      {'viewportProvisioning': 'true'},
      {'transport': 'media-v1'},
    ]) {
      expect(
        () => BrokerConnection.fromJson({
          ...response,
          ...delta,
        }, Uri.parse('https://cloud.example')),
        throwsA(isA<BrokerException>()),
      );
    }
    final legacy = BrokerConnection.fromJson({
      'sessionId': 'resize-session',
      'webSocketUrl': response['webSocketUrl'],
    }, Uri.parse('https://cloud.example'));
    expect(legacy.epochMedia, false);
    expect(legacy.viewportProvisioning, false);
  });
  test('Real viewport handshake replaces media geometry, releases input and separates codec generations', () async {
    final socket = SessionSocket(), media = ResizeMedia();
    final session = RemoteSession(
      connection: connection,
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    socket.incoming.add(jsonEncode(startInfo));
    await flush();
    socket.incoming.add(packet());
    await flush();
    await session.capture(true);
    final result = session.resizeViewport(1346, 812);
    await flush();
    expect(session.resizing, true);
    expect(session.captured, false);
    final id =
        socket.sent.lastWhere((v) => v['type'] == 'stream.resize')['requestId']
            as String;
    expect(await session.resizeViewport(1548, 916), false);
    session.input({
      'type': 'input',
      'event': 'key',
      'vk': 91,
      'scan': 0,
      'down': true,
      'extended': true,
    });
    expect(socket.sent.where((v) => v['type'] == 'input'), isEmpty);
    socket.incoming.add(jsonEncode(resizeAck(id)));
    await flush();
    expect(media.stopped, true);
    expect(session.textureId, null);
    socket.incoming.add(packet());
    socket.incoming.add(jsonEncode(applied('retired-request')));
    await flush();
    expect(session.resizing, true);
    expect(media.dimensions, [
      [1280, 720],
    ]);
    socket.incoming.add(jsonEncode(applied(id)));
    await flush();
    expect(session.resizing, true);
    expect(session.width, 1346);
    expect(session.sourceWidth, 1346);
    expect(media.dimensions, [
      [1280, 720],
      [1346, 812],
    ]);
    final old = packet(), nonKey = packet(), fresh = packet();
    nonKey[1] = 0;
    nonKey[2] = 1;
    fresh[2] = 1;
    socket.incoming.add(old);
    socket.incoming.add(nonKey);
    socket.incoming.add(fresh);
    await flush();
    expect(await result, true);
    expect(media.packets.length, 2);
    expect(media.packets.last[2], 0);
    expect(media.packets.last[3], 0);
    expect(session.closed, false);
    await session.close();
    await running;
    await media.events.close();
  });
  test('Pre-dispatch refusal preserves the current decoder and settles the pending request', () async {
    final socket = SessionSocket(), media = ResizeMedia();
    final session = RemoteSession(
      connection: connection,
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    socket.incoming.add(jsonEncode(startInfo));
    await flush();
    final result = session.resizeViewport(1346, 812);
    await flush();
    final id = socket.sent.lastWhere(
      (v) => v['type'] == 'stream.resize',
    )['requestId'];
    socket.incoming.add(
      jsonEncode({
        'type': 'stream.resize.failed',
        'requestId': id,
        'unchanged': true,
        'reason': 'unsupported_display_mode',
      }),
    );
    expect(await result, false);
    expect(session.resizing, false);
    expect(session.textureId, 41);
    expect(media.stopped, false);
    expect(session.closed, false);
    await session.close();
    await running;
    await media.events.close();
  });
  test('An unchanged failure after dispatch cannot claim a stopped decoder is still connected', () async {
    final socket = SessionSocket(), media = ResizeMedia();
    final session = RemoteSession(
      connection: connection,
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    socket.incoming.add(jsonEncode(startInfo));
    await flush();
    final result = session.resizeViewport(1346, 812);
    await flush();
    final id =
        socket.sent.lastWhere((v) => v['type'] == 'stream.resize')['requestId']
            as String;
    socket.incoming.add(jsonEncode(resizeAck(id)));
    await flush();
    socket.incoming.add(
      jsonEncode({
        'type': 'stream.resize.failed',
        'requestId': id,
        'unchanged': true,
      }),
    );
    expect(await result, false);
    await running;
    expect(session.closed, true);
    expect(media.stopped, true);
    await media.events.close();
  });
  test('A scaled resize response fails instead of claiming the actual viewport was applied', () async {
    final socket = SessionSocket(), media = ResizeMedia();
    final session = RemoteSession(
      connection: connection,
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    socket.incoming.add(jsonEncode(startInfo));
    await flush();
    final result = session.resizeViewport(1346, 812);
    await flush();
    final id =
        socket.sent.lastWhere((v) => v['type'] == 'stream.resize')['requestId']
            as String;
    socket.incoming.add(jsonEncode(resizeAck(id)));
    await flush();
    socket.incoming.add(
      jsonEncode({...applied(id), 'width': 1280, 'height': 720}),
    );
    expect(await result, false);
    await running;
    expect(session.closed, true);
    expect(media.dimensions, [
      [1280, 720],
    ]);
    expect(session.diagnosticCode, 'invalid_stream_message');
    await media.events.close();
  });
  test('Disconnect settles the in-flight resize without reopening a decoder from late replies', () async {
    final socket = SessionSocket(), media = ResizeMedia();
    final session = RemoteSession(
      connection: connection,
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    socket.incoming.add(jsonEncode(startInfo));
    await flush();
    final result = session.resizeViewport(1346, 812);
    await flush();
    await session.close();
    expect(await result, false);
    await running;
    expect(session.closed, true);
    expect(media.dimensions, [
      [1280, 720],
    ]);
    await media.events.close();
  });
}

// A viewport resize keeps the previous picture and reports its stage timing
// (tuning backlog #1, 2026-11-02: the window went black for 0.5-2 s and the
// 780 ms p50 had no breakdown). Socket, decoder and native texture are
// fixtures: this proves the session's protocol and bookkeeping, not a
// measured field resize.
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart' show SessionSocket, flush, packet;
import 'viewport_session_test.dart'
    show ResizeMedia, applied, connection, resizeAck, startInfo;

/// The Windows runner's held picture, as a fixture.
class HoldMedia extends ResizeMedia implements HeldPictureMedia {
  HoldMedia({this.holdsPicture = true});
  @override
  final bool holdsPicture;
  final holding = <List<int>>[];
  final released = <int>[];

  @override
  Future<int> startHolding(int width, int height) async {
    holding.add([width, height]);
    return start(width, height);
  }

  @override
  Future<void> releaseHeld(int texture) async => released.add(texture);
}

const hostStages = {
  'suspendMs': 9,
  'displayMs': 420,
  'sdkMs': 230,
  'settleMs': 150,
  'confirmMs': 30,
  'startMs': 140,
  'captureMs': 60,
  'encoderMs': 25,
  'audioMs': 3,
};

Future<(RemoteSession, SessionSocket, Future<void>)> streaming(
  ResizeMedia media,
) async {
  final socket = SessionSocket();
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
  expect(session.textureId, 41);
  return (session, socket, running);
}

String requestId(SessionSocket socket) =>
    socket.sent.lastWhere((v) => v['type'] == 'stream.resize')['requestId']
        as String;

void main() {
  test('A resize keeps the previous picture until the new size has a frame, '
      'then lets it go after the view switched', () async {
    final media = HoldMedia();
    final (session, socket, running) = await streaming(media);
    final result = session.resizeViewport(1346, 812);
    await flush();
    final id = requestId(socket);
    socket.incoming.add(jsonEncode(resizeAck(id)));
    await flush();
    // The Host is changing the display: the old picture stays on screen.
    expect(session.resizing, isTrue);
    expect(media.stopped, isFalse);
    expect(session.textureId, 41);
    expect(session.heldTextureId, 41);
    expect(session.heldFrame, (1280, 720));
    socket.incoming.add(
      jsonEncode({
        ...applied(id),
        'resizeStages': {...hostStages, 'unknownMs': 5},
      }),
    );
    await flush();
    // The new decoder starts beside the held picture.
    expect(media.holding, [
      [1346, 812],
    ]);
    expect(session.textureId, 42);
    expect(session.heldTextureId, 41);
    final fresh = packet();
    fresh[2] = 1;
    socket.incoming.add(fresh);
    expect(await result, isTrue);
    expect(session.resizing, isFalse);
    expect(session.heldTextureId, isNull);
    expect(session.heldFrame, isNull);
    expect(media.released, isEmpty, reason: 'not before the view switched');
    await Future<void>.delayed(const Duration(milliseconds: 320));
    expect(media.released, [41]);
    expect(media.stopped, isFalse);
    await session.close();
    await running;
    await media.events.close();
  });

  test(
    'viewportResult carries every stage, client and Host, as fixed numbers',
    () async {
      final media = HoldMedia();
      final (session, socket, running) = await streaming(media);
      final result = session.resizeViewport(1346, 812);
      await flush();
      final id = requestId(socket);
      await Future<void>.delayed(const Duration(milliseconds: 30));
      socket.incoming.add(jsonEncode(resizeAck(id)));
      await Future<void>.delayed(const Duration(milliseconds: 30));
      socket.incoming.add(
        jsonEncode({
          ...applied(id),
          'resizeStages': {
            ...hostStages,
            'unknownMs': 5,
            'encoderMs': -1,
            'audioMs': 1.5,
          },
        }),
      );
      await flush();
      final fresh = packet();
      fresh[2] = 1;
      socket.incoming.add(fresh);
      expect(await result, isTrue);
      final record = ProductLog.instance.entries.lastWhere(
        (entry) => entry.event == LogEvent.viewportResult,
      );
      final values = record.values;
      expect(values['ok'], isTrue);
      for (final key in [
        'probeMs',
        'ackMs',
        'infoMs',
        'decoderMs',
        'frameMs',
        'elapsedMs',
      ]) {
        expect(values[key], isA<int>(), reason: key);
      }
      final ack = values['ackMs'] as int, info = values['infoMs'] as int;
      expect(ack, greaterThanOrEqualTo(25));
      expect(info, greaterThanOrEqualTo(ack + 25));
      expect(
        values['elapsedMs'] as int,
        greaterThanOrEqualTo(info),
        reason: 'request -> first new picture spans every later stage',
      );
      expect(values['hostSuspendMs'], 9);
      expect(values['hostDisplayMs'], 420);
      expect(values['hostSdkMs'], 230);
      expect(values['hostSettleMs'], 150);
      expect(values['hostConfirmMs'], 30);
      expect(values['hostRestartMs'], 140);
      expect(values['hostCaptureStartMs'], 60);
      // Invalid or unknown Host values never reach the record.
      expect(values.containsKey('hostEncoderStartMs'), isFalse);
      expect(values.containsKey('hostAudioStartMs'), isFalse);
      expect(values.containsKey('unknownMs'), isFalse);
      expect(values.keys.every(logFields.contains), isTrue);
      await session.close();
      await running;
      await media.events.close();
    },
  );

  test('A failed change after the acknowledgement lets the held picture go at once', () async {
    final media = HoldMedia();
    final (session, socket, running) = await streaming(media);
    final result = session.resizeViewport(1346, 812);
    await flush();
    final id = requestId(socket);
    socket.incoming.add(jsonEncode(resizeAck(id)));
    await flush();
    expect(session.heldTextureId, 41);
    socket.incoming.add(
      jsonEncode({
        'type': 'stream.resize.failed',
        'requestId': id,
        'unchanged': false,
        'reason': 'display_change_failed',
      }),
    );
    expect(await result, isFalse);
    await running;
    expect(session.closed, isTrue);
    expect(session.heldTextureId, isNull);
    expect(media.released, [41]);
    expect(media.stopped, isTrue);
    await media.events.close();
  });

  test('Engines that cannot hold a picture stop the old decoder at the acknowledgement as before', () async {
    final media = HoldMedia(holdsPicture: false);
    final (session, socket, running) = await streaming(media);
    final result = session.resizeViewport(1346, 812);
    await flush();
    final id = requestId(socket);
    socket.incoming.add(jsonEncode(resizeAck(id)));
    await flush();
    expect(media.stopped, isTrue);
    expect(session.textureId, isNull);
    expect(session.heldTextureId, isNull);
    socket.incoming.add(jsonEncode(applied(id)));
    await flush();
    expect(media.holding, isEmpty);
    expect(media.dimensions, [
      [1280, 720],
      [1346, 812],
    ]);
    final fresh = packet();
    fresh[2] = 1;
    socket.incoming.add(fresh);
    expect(await result, isTrue);
    expect(media.released, isEmpty);
    await session.close();
    await running;
    await media.events.close();
  });

  test('Host stage values: fixed names, integers within bounds only', () {
    expect(hostResizeStages(null), isEmpty);
    expect(hostResizeStages('x'), isEmpty);
    expect(
      hostResizeStages({
        'suspendMs': 0,
        'displayMs': 600000,
        'sdkMs': 600001,
        'settleMs': -3,
        'confirmMs': '30',
        'startMs': 2.5,
        'other': 1,
      }),
      {'hostSuspendMs': 0, 'hostDisplayMs': 600000},
    );
  });
}

import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart' show SessionSocket, SessionMedia, packet;
import 'stream_telemetry_test.dart' show validTelemetry;
import 'viewport_session_test.dart'
    show connection, startInfo, resizeAck, applied;

class TelemetryClock extends Fake implements Stopwatch {
  int now = 0;
  @override
  int get elapsedMicroseconds => now;
}

class TelemetryMedia extends SessionMedia {
  Completer<Map<String, dynamic>>? pending;
  int statsCalls = 0;
  Map<String, dynamic> report = {
    'decodedFrames': 4,
    'textureFrames': 3,
    'skippedDecodedFrames': 1,
    'decodeCalls': 4,
    'decodeTotalMs': 16.0,
    'decodeMaxMs': 5.2,
    'queueOldestMs': 0.0,
    'queuedPackets': 0,
    'error': 0,
    'audioError': 0,
  };
  @override
  Future<int> start(int width, int height) async => 40 + ++starts;
  @override
  Future<Map<String, dynamic>> stats() async {
    statsCalls++;
    return pending?.future ?? report;
  }
}

Future<void> settleControl(WidgetTester t) async {
  for (var i = 0; i < 5; i++) {
    await t.pump();
  }
}

Future<RemoteSession> openEpoch(
  WidgetTester t,
  SessionSocket socket,
  TelemetryMedia media,
  TelemetryClock clock,
) async {
  final current = RemoteSession(
    connection: connection,
    token: 'fixture-only',
    media: media,
    clock: clock,
    dialer: (_, _) async => socket,
  );
  unawaited(current.start());
  await t.pump();
  socket.incoming.add(jsonEncode(startInfo));
  await settleControl(t);
  socket.incoming.add(packet());
  await settleControl(t);
  expect(current.textureId, 41);
  return current;
}

Map<String, dynamic> scopedTelemetry({int epoch = 0}) => {
  ...validTelemetry(),
  'sessionId': connection.sessionId,
  'epoch': epoch,
};

void main() {
  for (final oldError in [false, true]) {
    testWidgets(
      'A delayed ${oldError ? 'failed' : 'successful'} decoder poll cannot affect the resized generation',
      (t) async {
        final socket = SessionSocket(),
            media = TelemetryMedia(),
            clock = TelemetryClock();
        final current = await openEpoch(t, socket, media, clock);
        final initialPolls = media.statsCalls;
        final delayed = media.pending = Completer<Map<String, dynamic>>();
        clock.now = 1000000;
        await t.pump(const Duration(seconds: 1));
        expect(media.statsCalls, initialPolls + 1);
        final resizing = current.resizeViewport(1346, 812);
        await settleControl(t);
        final id =
            socket.sent.lastWhere(
                  (event) => event['type'] == 'stream.resize',
                )['requestId']
                as String;
        socket.incoming.add(jsonEncode(resizeAck(id)));
        await settleControl(t);
        expect(current.textureId, isNull);
        socket.incoming.add(jsonEncode(applied(id)));
        await settleControl(t);
        expect(current.resizing, isTrue);
        expect(current.textureId, 42);
        if (oldError) {
          delayed.completeError(StateError('retired decoder'));
        } else {
          delayed.complete({
            ...media.report,
            'error': -1,
            'decodedFrames': 10000,
            'textureFrames': 9000,
          });
        }
        await settleControl(t);
        expect(current.closed, isFalse, reason: current.error);
        expect(current.decodedFps, isNull);
        expect(current.textureFps, isNull);
        expect(current.clientDecodeMs, isNull);
        media.pending = null;
        final fresh = packet()..[2] = 1;
        socket.incoming.add(fresh);
        await settleControl(t);
        clock.now = 2000000;
        await t.pump(const Duration(milliseconds: 60));
        await settleControl(t);
        expect(await resizing, isTrue);
        expect(current.closed, isFalse, reason: current.error);
        // Fast readiness checks do not consume the one-second measurement
        // window or invent a new statistics sample.
        await t.pump(const Duration(seconds: 1));
        await settleControl(t);
        expect(current.decodedFps, 4);
        expect(current.textureFps, 3);
        expect(current.clientDecodeMs, 4);
        expect(current.clientDecodeMaxMs, 5.2);
        expect(current.clientQueuePackets, 0);
        expect(current.skippedDecodedFrames, 1);
        await current.close();
        await media.events.close();
      },
    );
  }

  testWidgets(
    'Host timings need the current session and epoch and expire without inventing zeroes',
    (t) async {
      final socket = SessionSocket(),
          media = TelemetryMedia(),
          clock = TelemetryClock();
      final current = await openEpoch(t, socket, media, clock);
      for (final unscoped in [
        validTelemetry(),
        {...scopedTelemetry(), 'sessionId': 'other-session'},
        scopedTelemetry(epoch: 1),
        {...scopedTelemetry(), 'encodedFps': '60'},
      ]) {
        socket.incoming.add(jsonEncode(unscoped));
        await settleControl(t);
        expect(current.hostTelemetry, isNull);
        expect(current.closed, isFalse);
      }
      socket.incoming.add(jsonEncode(scopedTelemetry()));
      await settleControl(t);
      expect(current.hostTelemetryFresh, isTrue);
      expect(current.hostTelemetry!.pipeline!.captureMs, 3.5);
      clock.now = 6000000;
      expect(current.hostTelemetryFresh, isFalse);
      expect(
        current.hostTelemetry!.pipeline!.captureMs,
        3.5,
        reason:
            'Keep the measured sample; freshness controls its presentation.',
      );
      await current.close();
      expect(current.hostTelemetryFresh, isFalse);
      await media.events.close();
    },
  );

  testWidgets(
    'Applied bitrate remains a target and malformed optional timing does not close a valid session',
    (t) async {
      final socket = SessionSocket(),
          media = TelemetryMedia(),
          clock = TelemetryClock();
      final current = RemoteSession(
        connection: connection,
        token: 'fixture-only',
        media: media,
        clock: clock,
        dialer: (_, _) async => socket,
      );
      unawaited(current.start());
      await t.pump();
      socket.incoming.add(
        jsonEncode({
          ...startInfo,
          'bitrate': 12000000,
          'captureBackend': 'wgc-warp',
        }),
      );
      await settleControl(t);
      expect(current.appliedBitrate, 12000000);
      expect(current.captureBackend, 'wgc-warp');
      socket.incoming.add(
        jsonEncode({
          ...scopedTelemetry(),
          'pipeline': {'sampleCount': 'invalid'},
        }),
      );
      await settleControl(t);
      expect(current.closed, isFalse);
      expect(current.hostTelemetryFresh, isTrue);
      expect(current.hostTelemetry!.pipeline, isNull);
      expect(current.hostTelemetry!.bitrateKbps, 8560.4);
      expect(
        current.appliedBitrate,
        12000000,
        reason: 'Actual transfer rate must not replace the negotiated bitrate target.',
      );
      await current.close();
      await media.events.close();
    },
  );
}

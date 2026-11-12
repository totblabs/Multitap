// Fake relay/media/clock only. No native decoder, HWND, input or network use.
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_stats_report.dart';

import 'remote_session_test.dart' show SessionSocket;
import 'session_recovery_test.dart' show ManualClock;
import 'track_stream_test.dart' as f;

class _DualMedia extends f.TrackMedia {
  int first = 0, second = 0;
  @override
  Future<Map<String, dynamic>> stats() async => {
    ...f.trackStats(first: first, second: second),
    'textureFrames': second,
    'decodeCalls': first + second,
    'decodeTotalMs': (first + second) * 5.0,
    'decodeMaxMs': 17.0,
    'queueOldestMs': 80.0,
    'queuedPackets': 2,
    'appCpuPct10': 200,
    'systemCpuPct10': 300,
    // Rendering on hardware does not establish hardware decoding.
    'gpuHardware': true,
    'hardwareDecode': false,
    'decodeTier': 0,
    'decodeVendor': 0,
    // Android's decoder configuration; out of range is never reported.
    'decoderLowLatency': 23,
    'decoderVendorKeys': 16,
  };
}

void main() {
  testWidgets(
    'Dual aggregate stats reach the negotiated sink only after both outputs decode',
    (t) async {
      final socket = SessionSocket(),
          media = _DualMedia(),
          clock = ManualClock();
      final reports = <Map<String, Object>>[];
      final session = RemoteSession(
        connection: f.connection(),
        token: 'fixture-only-token',
        media: media,
        clock: clock,
        dialer: (_, _) async => socket,
        statsSink: reports.add,
      );
      final running = session.start();
      await t.pump();
      socket.incoming.add(jsonEncode(f.info()));
      await t.pump();
      var second = 0, answered = 0;
      Future<void> advance(int seconds, {required bool both}) async {
        for (var n = 0; n < seconds; ++n) {
          clock.now = ++second * 1000000;
          media.first += 60;
          if (both) media.second += 40;
          for (; answered < socket.sent.length; ++answered) {
            final sent = socket.sent[answered];
            if (sent['type'] == 'ping') {
              socket.incoming.add(
                jsonEncode({'type': 'pong', 'id': sent['id']}),
              );
            }
          }
          socket.incoming.add(f.packet(0, pts: clock.now));
          socket.incoming.add(f.packet(1, pts: clock.now));
          // The Host's per-output rates, in the exact shape the relay passes.
          socket.incoming.add(
            jsonEncode({
              'type': 'stream.tracks.stats',
              'sessionId': 'track-session',
              'topologyEpoch': 1,
              'measuredMs': 1000,
              'tracks': [
                {
                  'id': '0',
                  'epoch': 1,
                  'capturedFps': 60.0,
                  'encodedFps': 59.0,
                  'bitrateKbps': 4000.0,
                },
                {
                  'id': '1',
                  'epoch': 1,
                  'capturedFps': 45.0,
                  'encodedFps': 44.0,
                  'bitrateKbps': 3000.0,
                },
              ],
            }),
          );
          await t.pump(const Duration(seconds: 1));
          expect(session.closed, isFalse, reason: session.error);
        }
      }

      await advance(3, both: false);
      expect(session.frameReady, isFalse);
      expect(
        reports,
        isEmpty,
        reason: 'One decoded output cannot authorize aggregate readiness.',
      );
      await advance(14, both: true);
      expect(session.frameReady, isTrue);
      expect(reports, hasLength(1));
      final report = reports.single;
      expect(report['monitors'], 2);
      expect(report.keys.every(streamStatsFields.contains), isTrue);
      expect(
        report['decodeFps10'] as int,
        greaterThan(700),
        reason: 'The two decoder rates are summed, not reported as each monitor\'s rate.',
      );
      expect(
        report['textureFps10'] as int,
        inInclusiveRange(300, 500),
        reason: 'Only the fake primary Flutter texture counter is reported.',
      );
      expect(
        report['decodeFpsLow10'],
        400,
        reason: 'The slower output (40 fps) shows apart from the summed rate.',
      );
      expect(
        report['captureFps10'],
        1050,
        reason: 'The Host capture rates of both outputs reach the report.',
      );
      expect(report['encodeFps10'], 1030);
      expect(report['captureFpsLow10'], 450);
      expect(report['encodeFpsLow10'], 440);
      expect(session.capturedFps, 105);
      expect(report['decodeUs'], 5000);
      expect(report['queueMaxMs'], 80);
      expect(report['appCpuPct10'], 200);
      expect(report['gpuHardware'], true);
      expect(report.containsKey('hardwareDecode'), isFalse);
      // Reported (the broker keeps them): the decode adapter, from the
      // engine's own stats (0 here), never invented.
      expect(report['decodeTier'], 0);
      expect(report['decodeVendor'], 0);
      expect(report['decoderLowLatency'], 23);
      expect(report.containsKey('decoderVendorKeys'), isFalse);
      await advance(4, both: true);
      session.flushStats();
      expect(reports, hasLength(2));
      expect(reports.last['monitors'], 2);
      await session.close();
      await running;
      await media.events.close();
    },
  );
}

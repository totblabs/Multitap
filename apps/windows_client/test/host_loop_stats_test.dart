import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_loop_stats.dart';
import 'package:multitap_client/data/stream_stall.dart';
import 'package:multitap_client/data/stream_stats_report.dart';

Map<String, dynamic> hostStats({
  Map<String, Object?> stall = const {},
  Object? backend = 'wgc-warp',
  Object? interval = true,
}) => {
  'type': 'stream.stats',
  'capturedFps': 60,
  'encodedFps': 60,
  'bitrateKbps': 4000,
  if (backend != null) 'captureBackend': backend,
  if (interval != null) 'captureMinInterval': interval,
  'stall': {'encodeGapMs': 17, ...stall},
};

void main() {
  test('A Host reading becomes fixed row fields; older Hosts give none', () {
    final reading = HostLoopTally.reading(
      hostStats(
        stall: {
          'backgroundTicks': 19,
          'backgroundMs': 961,
          'secureHoldTicks': 1,
          'secureHoldMs': 16,
          'follows': 1,
          'followMs': 1500,
          'overshootMaxUs': 4200,
          'tailMaxUs': 12000,
          'pipeWriteMaxUs': 2500,
          'lossAnswers': 12,
          'lossKeys': 1,
        },
      ),
    );
    expect(reading, {
      'hostBackgroundTicks': 19,
      'hostBackgroundMs': 961,
      'hostSecureHoldTicks': 1,
      'hostSecureHoldMs': 16,
      'hostFollows': 1,
      'hostFollowMs': 1500,
      'hostOvershootMaxUs': 4200,
      'hostTailMaxUs': 12000,
      'hostPipeWriteMaxUs': 2500,
      'hostLossAnswers': 12,
      'hostLossKeys': 1,
      'hostCaptureBackend': 3,
      'hostCaptureMinInterval': 1,
    });
    // A released Host: no loop members, no backend.
    expect(HostLoopTally.reading(hostStats(backend: null, interval: null)), isEmpty);
    // Wrong types and bounds are dropped, never guessed.
    expect(
      HostLoopTally.reading(
        hostStats(
          stall: {'tailMaxUs': -1, 'followMs': 1.5, 'backgroundTicks': '3', 'pipeWriteMaxUs': 0x100000000},
          backend: 'dxgi',
          interval: 1,
        ),
      ),
      isEmpty,
    );
    // GDI has no update interval; a WGC capture that did not take it says 0.
    expect(HostLoopTally.reading(hostStats(backend: 'gdi', interval: true)), {'hostCaptureBackend': 1});
    expect(HostLoopTally.reading(hostStats(backend: 'wgc-d3d11', interval: false)), {'hostCaptureBackend': 2, 'hostCaptureMinInterval': 0});
  });

  test('A tally sums ticks and times, keeps the largest maxima and the latest backend', () {
    final tally = HostLoopTally();
    expect(tally.take(), isNull);
    tally.add(HostLoopTally.reading(hostStats(stall: {'backgroundTicks': 19, 'backgroundMs': 961, 'tailMaxUs': 9000, 'overshootMaxUs': 300})));
    tally.add(null);
    tally.add(HostLoopTally.reading(hostStats(stall: {'backgroundTicks': 1, 'backgroundMs': 50, 'tailMaxUs': 4000, 'overshootMaxUs': 5100}, backend: 'gdi')));
    expect(tally.take(), {
      'hostBackgroundTicks': 20,
      'hostBackgroundMs': 1011,
      'hostTailMaxUs': 9000,
      'hostOvershootMaxUs': 5100,
      'hostCaptureBackend': 1,
    });
    expect(tally.take(), isNull, reason: 'a take resets');
  });

  test('The field lists agree with each other and with both row kinds', () {
    expect(hostLoopStallFields, {for (final key in [...hostLoopSummed, ...hostLoopLargest]) hostLoopField(key)});
    expect(hostLoopFields.every(streamStatsFields.contains), isTrue);
    expect(hostLoopStallFields.every(streamStallFields.contains), isTrue);
  });

  test('A statistics window carries the Host loop of its samples', () {
    final window = StreamStatsWindow(firstPeriod: const Duration(seconds: 10));
    final tally = HostLoopTally();
    // Two Host readings arrive before the first sample, none before the second.
    tally.add(HostLoopTally.reading(hostStats(stall: {'secureNoticeTicks': 5, 'secureNoticeMs': 1000, 'pipeWriteMaxUs': 800})));
    tally.add(HostLoopTally.reading(hostStats(stall: {'secureNoticeTicks': 2, 'secureNoticeMs': 400, 'pipeWriteMaxUs': 2600})));
    window.add(StreamStatsSample(width: 1264, height: 760, decodedFps: 60, hostLoop: tally.take()), 0);
    window.add(StreamStatsSample(width: 1264, height: 760, decodedFps: 60, hostLoop: tally.take()), 1000000);
    tally.add(HostLoopTally.reading(hostStats(stall: {'secureNoticeTicks': 1, 'secureNoticeMs': 200, 'pipeWriteMaxUs': 100})));
    window.add(StreamStatsSample(width: 1264, height: 760, decodedFps: 60, hostLoop: tally.take()), 2000000);
    final report = window.take(10000000)!;
    expect(report['hostSecureNoticeTicks'], 8);
    expect(report['hostSecureNoticeMs'], 1600);
    expect(report['hostPipeWriteMaxUs'], 2600);
    expect(report['hostCaptureBackend'], 3);
    expect(report['hostCaptureMinInterval'], 1);
    expect(report.keys.every(streamStatsFields.contains), isTrue);
    window.add(const StreamStatsSample(width: 1264, height: 760, decodedFps: 60), 11000000);
    final next = window.take(21000000)!;
    expect(next.keys.where((key) => hostLoopFields.contains(key)), isEmpty, reason: 'a new window starts empty');
  });

  test('A stall row sums the covering windows\' skipped time and keeps their largest maxima', () {
    final watch = StallWatch();
    for (var us = 0; us <= 1000000; us += 16000) {
      watch.arrival(us);
    }
    watch.arrival(2000000);
    watch.host(hostStats(stall: {'secureHoldTicks': 30, 'secureHoldMs': 480, 'tailMaxUs': 700, 'pipeWriteMaxUs': 90000}), 1900000);
    watch.host(hostStats(stall: {'secureHoldTicks': 10, 'secureHoldMs': 160, 'tailMaxUs': 2100, 'pipeWriteMaxUs': 300, 'follows': 1, 'followMs': 600, 'lossAnswers': 3, 'lossKeys': 1}), 2900000);
    watch.engine([[1, 1, 1000, 0, 1, 8, 8, 3, 5, 2, 18000, 1, 0, 0]], null, 2000000);
    final event = watch.take(3300000).single;
    expect(event['hostWindows'], 2);
    expect(event['hostSecureHoldTicks'], 40);
    expect(event['hostSecureHoldMs'], 640);
    expect(event['hostFollows'], 1);
    expect(event['hostFollowMs'], 600);
    expect(event['hostTailMaxUs'], 2100);
    expect(event['hostPipeWriteMaxUs'], 90000);
    expect(event['hostLossAnswers'], 3);
    expect(event['hostLossKeys'], 1);
    expect(event.keys.every(streamStallFields.contains), isTrue);
  });
}

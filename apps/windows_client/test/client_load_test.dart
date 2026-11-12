import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/stream_stats_report.dart';
import 'package:multitap_client/ui/stream_session_stats.dart';

StreamStatsSample _sample({int? app, int? system, int? decode, int? render}) =>
    StreamStatsSample(
      width: 1264,
      height: 760,
      decodedFps: 60,
      appCpuPct10: app,
      systemCpuPct10: system,
      gpuDecodePct10: decode,
      gpu3dPct10: render,
    );

void main() {
  test('Client load is averaged per window as tenths of a percent', () {
    final window = StreamStatsWindow(firstPeriod: const Duration(seconds: 10));
    window.add(_sample(app: 100, system: 300, decode: 60, render: 20), 0);
    window.add(_sample(app: 140, system: 380, decode: 100, render: 40), 1000000);
    final report = window.take(10000000)!;
    expect(report['appCpuPct10'], 120);
    expect(report['systemCpuPct10'], 340);
    expect(report['gpuDecodePct10'], 80);
    expect(report['gpu3dPct10'], 30);
    expect(report.keys.every(streamStatsFields.contains), isTrue);
  });

  test('Unmeasured load is left out, never reported as zero', () {
    final window = StreamStatsWindow(firstPeriod: const Duration(seconds: 10));
    window.add(_sample(app: 55), 0);
    window.add(_sample(), 1000000);
    final report = window.take(10000000)!;
    expect(report['appCpuPct10'], 55);
    for (final key in ['systemCpuPct10', 'gpuDecodePct10', 'gpu3dPct10']) {
      expect(report.containsKey(key), isFalse, reason: key);
    }
    final next = StreamStatsWindow(firstPeriod: const Duration(seconds: 10))
      ..add(_sample(), 0);
    expect(next.take(10000000)!.containsKey('appCpuPct10'), isFalse);
  });

  test('Labels show the app and PC CPU and the decoder path', () {
    expect(appCpuLabel(120, 340), '12% (PC 전체 34%)');
    expect(appCpuLabel(125, null), '12.5%');
    expect(appCpuLabel(0, 5), '0% (PC 전체 0.5%)');
    expect(appCpuLabel(null, 340), '—');
    expect(gpuDecodeLabel(80, 1), '8% · GPU 디코더');
    expect(gpuDecodeLabel(null, 1), '알 수 없음 · GPU 디코더');
    expect(gpuDecodeLabel(null, 2), '알 수 없음 · GPU 디코더');
    expect(gpuDecodeLabel(0, 0), 'CPU 디코더');
    expect(gpuDecodeLabel(null, null), '알 수 없음');
  });
}
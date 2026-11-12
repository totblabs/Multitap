import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/stream_telemetry.dart';

Map<String, dynamic> validPipeline() => {
  'sampleCount': 60,
  'topologyAvgMs': .2,
  'captureAvgMs': 3.5,
  'convertAvgMs': 1.1,
  'encodeAvgMs': 4.4,
  'workAvgMs': 9.2,
  'workP95Ms': 11.5,
  'workMaxMs': 16.1,
};
Map<String, dynamic> validTelemetry() => {
  'type': 'stream.stats',
  'capturedFps': 58.2,
  'encodedFps': 58,
  'bitrateKbps': 8560.4,
  'measuredMs': 1012,
  'pipeline': validPipeline(),
};

void main() {
  test(
    'Host timing preserves separate stages and its measured sample window',
    () {
      final value = HostStreamTelemetry.parse(validTelemetry())!;
      expect(value.capturedFps, 58.2);
      expect(value.encodedFps, 58);
      expect(value.bitrateKbps, 8560.4);
      expect(value.measuredMs, 1012);
      expect(value.pipeline!.samples, 60);
      expect(value.pipeline!.captureMs, 3.5);
      expect(value.pipeline!.convertMs, 1.1);
      expect(value.pipeline!.encodeMs, 4.4);
      expect(value.pipeline!.p95Ms, 11.5);
    },
  );

  test(
    'Legacy FPS remains valid without claiming optional timings or zero work',
    () {
      final legacy = validTelemetry()
        ..remove('pipeline')
        ..remove('measuredMs');
      final value = HostStreamTelemetry.parse(legacy)!;
      expect(value.encodedFps, 58);
      expect(value.pipeline, isNull);
      expect(value.measuredMs, isNull);
    },
  );

  test(
    'Optional timing corruption does not discard a valid legacy FPS report',
    () {
      for (final corrupted in [
        null,
        '9ms',
        true,
        {...validPipeline(), 'captureAvgMs': double.nan},
        {...validPipeline(), 'workP95Ms': double.infinity},
        {...validPipeline(), 'encodeAvgMs': -1},
        {...validPipeline(), 'sampleCount': 257},
        {...validPipeline(), 'sampleCount': 1.5},
        {...validPipeline(), 'workMaxMs': 8},
        {...validPipeline(), 'workAvgMs': 2},
      ]) {
        final value = HostStreamTelemetry.parse({
          ...validTelemetry(),
          'pipeline': corrupted,
        })!;
        expect(value.pipeline, isNull, reason: '$corrupted');
        expect(value.encodedFps, 58);
      }
    },
  );

  test('A timing window must be bounded and positive before pipeline data is shown', () {
    for (final window in [null, 0, -1, '1000', true, double.nan, 120001]) {
      final value = HostStreamTelemetry.parse({
        ...validTelemetry(),
        'measuredMs': window,
      })!;
      expect(value.pipeline, isNull, reason: '$window');
    }
  });

  test('Malformed core measurements are rejected instead of becoming plausible zeroes', () {
    for (final key in ['capturedFps', 'encodedFps', 'bitrateKbps']) {
      for (final value in [null, '60', true, -1, double.nan, double.infinity]) {
        expect(
          HostStreamTelemetry.parse({...validTelemetry(), key: value}),
          isNull,
          reason: '$key=$value',
        );
      }
    }
    expect(
      HostStreamTelemetry.parse({...validTelemetry(), 'capturedFps': 241}),
      isNull,
    );
    // The bound is 1 Gbps since the 100 Mbps adaptive ceiling (a 1 s window
    // can exceed the target); beyond it the report is still rejected.
    expect(
      HostStreamTelemetry.parse({...validTelemetry(), 'bitrateKbps': 1000001}),
      isNull,
    );
    expect(
      HostStreamTelemetry.parse({...validTelemetry(), 'bitrateKbps': 150000})
          ?.bitrateKbps,
      150000,
    );
  });

  test('Host input timing is optional, bounded and ordered', () {
    final timing = {
      'samples': 12,
      'injectToCaptureP50Ms': 31.5,
      'injectToCaptureP95Ms': 70.25,
      'captureToEncodedP50Ms': 2.1,
      'captureToEncodedP95Ms': 6.4,
    };
    final value = HostStreamTelemetry.parse({
      ...validTelemetry(),
      'inputFrame': timing,
    })!;
    expect(value.inputFrame!.samples, 12);
    expect(value.inputFrame!.injectToCaptureP50Ms, 31.5);
    expect(value.inputFrame!.captureToEncodedP95Ms, 6.4);
    expect(HostStreamTelemetry.parse(validTelemetry())!.inputFrame, isNull);
    for (final corrupted in [
      null,
      'x',
      {...timing, 'samples': 0},
      {...timing, 'samples': 65},
      {...timing, 'injectToCaptureP50Ms': -1},
      {...timing, 'captureToEncodedP95Ms': 10001},
      {...timing, 'injectToCaptureP50Ms': 80},
      {...timing, 'captureToEncodedP50Ms': double.nan},
    ]) {
      final parsed = HostStreamTelemetry.parse({
        ...validTelemetry(),
        'inputFrame': corrupted,
      })!;
      expect(parsed.inputFrame, isNull, reason: '$corrupted');
      expect(parsed.encodedFps, 58);
    }
  });

  test('Host send path and relay forwarding are optional, bounded and ordered', () {
    final send = {
      'samples': 6,
      'queueP50Ms': 1.2,
      'queueP95Ms': 4.1,
      'writeP50Ms': .3,
      'writeP95Ms': 2.6,
    };
    final relay = {'samples': 6, 'p50Ms': .4, 'p95Ms': 1.9};
    final value = HostStreamTelemetry.parse({
      ...validTelemetry(),
      'inputSend': send,
      'relayForward': relay,
    })!;
    expect(value.inputSend!.firstP50Ms, 1.2);
    expect(value.inputSend!.secondP95Ms, 2.6);
    expect(value.relayForward!.firstP95Ms, 1.9);
    expect(value.relayForward!.secondP50Ms, isNull);
    final plain = HostStreamTelemetry.parse(validTelemetry())!;
    expect(plain.inputSend, isNull);
    expect(plain.relayForward, isNull);
    for (final corrupted in <Object?>[
      null,
      'x',
      {...send, 'samples': 0},
      {...send, 'samples': 65},
      {...send, 'queueP50Ms': -1},
      {...send, 'writeP95Ms': 10001},
      {...send, 'queueP50Ms': 5},
      {...send, 'writeP50Ms': double.nan},
    ]) {
      final parsed = HostStreamTelemetry.parse({
        ...validTelemetry(),
        'inputSend': corrupted,
        'relayForward': corrupted,
      })!;
      expect(parsed.inputSend, isNull, reason: '$corrupted');
      expect(parsed.encodedFps, 58);
    }
    expect(
      HostStreamTelemetry.parse({
        ...validTelemetry(),
        'relayForward': {...relay, 'p50Ms': 3},
      })!.relayForward,
      isNull,
    );
  });

  test('Independent-output Host rates are summed, with the slowest output apart', () {
    Map<String, dynamic> row(String id, num capture, num encode) => {
      'id': id,
      'epoch': 1,
      'capturedFps': capture,
      'encodedFps': encode,
      'bitrateKbps': 2500.5,
    };
    final value = HostStreamTelemetry.parseTracks({
      'type': 'stream.tracks.stats',
      'measuredMs': 1004,
      'tracks': [row('0', 59.8, 59.8), row('1', 31.2, 30)],
    })!;
    expect(value.capturedFps, closeTo(91, 1e-9));
    expect(value.encodedFps, closeTo(89.8, 1e-9));
    expect(value.bitrateKbps, 5001);
    expect(value.lowestCapturedFps, 31.2);
    expect(value.lowestEncodedFps, 30);
    expect(value.measuredMs, 1004);
    expect(value.pipeline, isNull);
    expect(HostStreamTelemetry.parse(validTelemetry())!.lowestCapturedFps, isNull);
    for (final tracks in <Object?>[
      null,
      <Object>[],
      [row('0', 60, 60), row('1', 60, 60), row('1', 60, 60)],
      [row('0', 241, 60)],
      [row('0', 60, -1)],
      [row('0', 60, double.nan)],
      ['row'],
    ]) {
      expect(
        HostStreamTelemetry.parseTracks({'tracks': tracks}),
        isNull,
        reason: '$tracks',
      );
    }
  });

  test('Rounded host stage averages tolerate display precision without hiding inconsistent totals', () {
    expect(
      HostPipelineMetrics.parse({...validPipeline(), 'workAvgMs': 9.23}),
      isNotNull,
    );
    expect(
      HostPipelineMetrics.parse({...validPipeline(), 'workAvgMs': 9.3}),
      isNull,
    );
    expect(
      HostPipelineMetrics.parse({...validPipeline(), 'workP95Ms': 16.2}),
      isNull,
    );
  });
}

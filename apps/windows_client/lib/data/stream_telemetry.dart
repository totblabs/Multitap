import 'encoder_telemetry.dart';

double? metricNumber(Object? value, double maximum) =>
    value is num && value.isFinite && value >= 0 && value <= maximum
    ? value.toDouble()
    : null;

class HostPipelineMetrics {
  const HostPipelineMetrics({
    required this.samples,
    required this.topologyMs,
    required this.captureMs,
    required this.convertMs,
    required this.encodeMs,
    required this.workMs,
    required this.p95Ms,
    required this.maxMs,
    this.topologyMaxMs,
    this.captureMaxMs,
    this.convertMaxMs,
    this.encodeMaxMs,
  });
  final int samples;
  final double topologyMs, captureMs, convertMs, encodeMs, workMs, p95Ms, maxMs;

  /// Per-stage maxima: which stage a slow frame spent its time
  /// in. Null from older hosts.
  final double? topologyMaxMs, captureMaxMs, convertMaxMs, encodeMaxMs;
  static HostPipelineMetrics? parse(Object? value) {
    if (value is! Map) return null;
    final count = value['sampleCount'];
    if (count is! int || count < 1 || count > 256) return null;
    final numbers = [
      for (final key in [
        'topologyAvgMs',
        'captureAvgMs',
        'convertAvgMs',
        'encodeAvgMs',
        'workAvgMs',
        'workP95Ms',
        'workMaxMs',
      ])
        metricNumber(value[key], 60000),
    ];
    if (numbers.any((n) => n == null)) return null;
    final n = numbers.cast<double>();
    if (n[4] > n[6] + .03 ||
        n[5] > n[6] + .03 ||
        (n[0] + n[1] + n[2] + n[3] - n[4]).abs() > .05) {
      return null;
    }
    return HostPipelineMetrics(
      samples: count,
      topologyMs: n[0],
      captureMs: n[1],
      convertMs: n[2],
      encodeMs: n[3],
      workMs: n[4],
      p95Ms: n[5],
      maxMs: n[6],
      topologyMaxMs: metricNumber(value['topologyMaxMs'], 60000),
      captureMaxMs: metricNumber(value['captureMaxMs'], 60000),
      convertMaxMs: metricNumber(value['convertMaxMs'], 60000),
      encodeMaxMs: metricNumber(value['encodeMaxMs'], 60000),
    );
  }
}

/// input-latency-v1 sessions: the host's share of recently
/// tagged inputs on the host clock, p50/p95 in ms. injectToCapture: the
/// injection -> start of the capture tick that got the changed frame (the
/// application and compositor reacting, plus capture pacing);
/// captureToEncoded: that tick -> its encoder output.
class HostInputFrameMetrics {
  const HostInputFrameMetrics({
    required this.samples,
    required this.injectToCaptureP50Ms,
    required this.injectToCaptureP95Ms,
    required this.captureToEncodedP50Ms,
    required this.captureToEncodedP95Ms,
  });
  final int samples;
  final double injectToCaptureP50Ms, injectToCaptureP95Ms;
  final double captureToEncodedP50Ms, captureToEncodedP95Ms;
  static HostInputFrameMetrics? parse(Object? value) {
    if (value is! Map) return null;
    final count = value['samples'];
    if (count is! int || count < 1 || count > 64) return null;
    final n = [
      for (final key in [
        'injectToCaptureP50Ms',
        'injectToCaptureP95Ms',
        'captureToEncodedP50Ms',
        'captureToEncodedP95Ms',
      ])
        metricNumber(value[key], 10000),
    ];
    if (n.any((v) => v == null) || n[0]! > n[1]! || n[2]! > n[3]!) return null;
    return HostInputFrameMetrics(
      samples: count,
      injectToCaptureP50Ms: n[0]!,
      injectToCaptureP95Ms: n[1]!,
      captureToEncodedP50Ms: n[2]!,
      captureToEncodedP95Ms: n[3]!,
    );
  }
}

/// input-latency-v1 sessions: the tagged packet after the encoder.
/// `inputSend` (host clock; absent from older Hosts): encoder output -> the start of
/// its WebSocket write (worker pipe, service send queue) and that write's
/// duration. `relayForward` (relay clock; absent from older brokers): the host's
/// complete message -> written toward this PC. p50/p95 in ms.
class SendPathMetrics {
  const SendPathMetrics({
    required this.samples,
    required this.firstP50Ms,
    required this.firstP95Ms,
    this.secondP50Ms,
    this.secondP95Ms,
  });
  final int samples;
  final double firstP50Ms, firstP95Ms;
  final double? secondP50Ms, secondP95Ms;

  static SendPathMetrics? _parse(Object? value, List<String> keys) {
    if (value is! Map) return null;
    final count = value['samples'];
    if (count is! int || count < 1 || count > 64) return null;
    final n = [for (final key in keys) metricNumber(value[key], 10000)];
    if (n.any((v) => v == null)) return null;
    for (var i = 0; i + 1 < n.length; i += 2) {
      if (n[i]! > n[i + 1]!) return null;
    }
    return SendPathMetrics(
      samples: count,
      firstP50Ms: n[0]!,
      firstP95Ms: n[1]!,
      secondP50Ms: n.length > 2 ? n[2] : null,
      secondP95Ms: n.length > 3 ? n[3] : null,
    );
  }

  /// Host: first = queue (output -> write start), second = write.
  static SendPathMetrics? host(Object? value) => _parse(value, const [
    'queueP50Ms',
    'queueP95Ms',
    'writeP50Ms',
    'writeP95Ms',
  ]);

  /// Relay: first = receipt -> written.
  static SendPathMetrics? relay(Object? value) =>
      _parse(value, const ['p50Ms', 'p95Ms']);
}

class HostStreamTelemetry {
  const HostStreamTelemetry({
    required this.capturedFps,
    required this.encodedFps,
    required this.bitrateKbps,
    this.measuredMs,
    this.pipeline,
    this.bitrateTarget,
    this.bitrateMax,
    this.automaticBitrate = false,
    this.networkDelayMs,
    this.inputFrame,
    this.inputSend,
    this.relayForward,
    this.encoder,
    this.legacyEncoder,
    this.lowestCapturedFps,
    this.lowestEncodedFps,
  });
  final double capturedFps, encodedFps, bitrateKbps;

  /// tracks-v1 only: the output with the fewest captured / encoded frames
  /// (the rates above sum the outputs). Null for a single stream.
  final double? lowestCapturedFps, lowestEncodedFps;
  final HostEncoderInfo? encoder;
  final String? legacyEncoder;
  final double? measuredMs;
  final HostPipelineMetrics? pipeline;

  /// Host share of tagged inputs; null from older hosts or without samples.
  final HostInputFrameMetrics? inputFrame;

  /// The tagged packet's host send path and relay forwarding (see
  /// [SendPathMetrics]); null from older hosts/brokers or without samples.
  final SendPathMetrics? inputSend, relayForward;

  /// The host encoder's current target (bits/s); with the automatic mode it
  /// follows the path. Optional: released hosts do not report it.
  final int? bitrateTarget, bitrateMax;
  final bool automaticBitrate;

  /// The host controller's standing one-way queueing delay after its send
  /// queue (ms); observational, reported only by an adaptive session.
  final double? networkDelayMs;
  static HostStreamTelemetry? parse(Map<String, dynamic> value) {
    final capture = metricNumber(value['capturedFps'], 240),
        encode = metricNumber(value['encodedFps'], 240),
        bitrate = metricNumber(value['bitrateKbps'], 1000000);
    if (capture == null || encode == null || bitrate == null) return null;
    final duration = metricNumber(value['measuredMs'], 120000);
    final target = value['bitrateTarget'], ceiling = value['bitrateMax'];
    return HostStreamTelemetry(
      capturedFps: capture,
      encodedFps: encode,
      bitrateKbps: bitrate,
      measuredMs: duration,
      pipeline: duration != null && duration > 0
          ? HostPipelineMetrics.parse(value['pipeline'])
          : null,
      bitrateTarget: target is int && target >= 500000 && target <= 100000000
          ? target
          : null,
      bitrateMax: ceiling is int && ceiling >= 500000 && ceiling <= 100000000
          ? ceiling
          : null,
      automaticBitrate: value['bitrateMode'] == 'auto',
      networkDelayMs: metricNumber(value['networkDelayMs'], 60000),
      inputFrame: HostInputFrameMetrics.parse(value['inputFrame']),
      inputSend: SendPathMetrics.host(value['inputSend']),
      relayForward: SendPathMetrics.relay(value['relayForward']),
      encoder: HostEncoderInfo.parse(value['encoderInfo']),
      legacyEncoder: const {'x264', 'mft'}.contains(value['encoder'])
          ? value['encoder'] as String
          : null,
    );
  }

  /// tracks-v1 `stream.tracks.stats` (the relay passes exactly id, epoch,
  /// capturedFps, encodedFps and bitrateKbps per output). Rates are summed
  /// over the outputs like the client's decoded rate, never one monitor's
  /// rate; the lowest output's rates show one that falls behind.
  static HostStreamTelemetry? parseTracks(Map<String, dynamic> value) {
    final rows = value['tracks'];
    if (rows is! List || rows.isEmpty || rows.length > 2) return null;
    var capture = 0.0, encode = 0.0, bitrate = 0.0;
    double? lowCapture, lowEncode;
    for (final row in rows) {
      if (row is! Map) return null;
      final c = metricNumber(row['capturedFps'], 240),
          e = metricNumber(row['encodedFps'], 240),
          b = metricNumber(row['bitrateKbps'], 1000000);
      if (c == null || e == null || b == null) return null;
      capture += c;
      encode += e;
      bitrate += b;
      if (lowCapture == null || c < lowCapture) lowCapture = c;
      if (lowEncode == null || e < lowEncode) lowEncode = e;
    }
    return HostStreamTelemetry(
      capturedFps: capture,
      encodedFps: encode,
      bitrateKbps: bitrate,
      measuredMs: metricNumber(value['measuredMs'], 120000),
      lowestCapturedFps: lowCapture,
      lowestEncodedFps: lowEncode,
    );
  }
}

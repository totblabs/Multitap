import '../data/encoder_telemetry.dart';
import '../data/input_latency.dart';
import '../data/remote_session.dart';
import '../models/stream_options.dart';
import 'session_stats_dialog.dart';
import '../l10n/l10n.dart';

/// input-latency-v1 rows: p50/p95 over the recent window, '측정 안 함' when the
/// connection's broker/host did not grant measurement, '—' before samples.
SessionMetricSection inputLatencySection(RemoteSession? session) {
  final latency = session?.inputLatency;
  final active = session != null && !session.closed;
  String row(LatencyPercentiles? value, {bool available = true}) {
    if (latency == null) return '—';
    if (!latency.supported || !available) return l10n.notMeasured2;
    if (!active || value == null) return '—';
    String ms(double v) => v.isFinite && v >= 0 ? v.toStringAsFixed(1) : '—';
    return l10n.p50P95Ms(ms(value.p50Ms), ms(value.p95Ms), value.samples);
  }

  return SessionMetricSection(
    l10n.inputLatency,
    [
      SessionMetric(l10n.inputHostApplied, row(latency?.toHost)),
      SessionMetric(
        l10n.inputScreenShown,
        row(latency?.toFrame, available: latency?.frameSupported == true),
      ),
      SessionMetric(l10n.hostReceivedApplied, row(latency?.hostApply)),
      SessionMetric(l10n.inputAckRoundTrip, row(latency?.roundTrip)),
    ],
    note: latency == null
        ? null
        : !latency.supported
        ? l10n.currentServerHostDoesntSupport
        : l10n.samplesFromLast30Seconds,
  );
}

List<SessionMetricSection> streamMetricSections(
  RemoteSession? session, {
  required StreamOptions options,
  required bool autoViewport,
  (int, int)? desired,
}) {
  String number(num? value, [int decimals = 1]) =>
      value != null && value.isFinite && value >= 0
      ? value.toStringAsFixed(decimals)
      : '—';
  String measured(num? value, String unit, [int decimals = 1]) {
    final formatted = number(value, decimals);
    return formatted == '—' ? formatted : '$formatted $unit';
  }

  String size(int? width, int? height) =>
      width == null || height == null || width <= 0 || height <= 0
      ? '—'
      : '$width × $height';
  final fresh = session?.hostTelemetryFresh == true;
  final clientActive = session != null && !session.closed && !session.resizing;
  // 모든 모니터 사용: the session sums the outputs' rates; show one output's
  // share, so 60 fps per monitor reads 60, not 120.
  final outputs =
      session != null && session.independentOutputs && session.tracks.length > 1
      ? session.tracks.length
      : 1;
  String rate(double? value) => measured(
    value == null ? null : value / outputs,
    outputs > 1 ? l10n.fpsPerScreen : 'fps',
  );
  final pipeline = fresh ? session?.hostTelemetry?.pipeline : null;
  // Automatic rate (bitrate-adaptive-v1): the host reports its current target
  // and ceiling in its own statistics.
  final telemetry = fresh ? session?.hostTelemetry : null;
  final encoder = telemetry?.encoder;
  final adaptive = telemetry?.automaticBitrate == true;
  String mbps(int? bits) => number(bits == null ? null : bits / 1000000);
  final backend = switch (session?.captureBackend) {
    'wgc-warp' => 'WGC · WARP',
    'wgc-d3d11' => 'WGC · D3D11',
    'gdi' => 'GDI',
    _ => '—',
  };
  return [
    SessionMetricSection(l10n.streaming, [
      SessionMetric(l10n.technology, StreamOptions.engineName),
      SessionMetric(
        l10n.status,
        session?.closed == true ? l10n.disconnected : session?.status ?? l10n.preparing4,
      ),
      SessionMetric(l10n.videoPath, session?.videoPathLabel ?? l10n.checking),
      SessionMetric(l10n.receivedSize, size(session?.width, session?.height)),
      SessionMetric(l10n.remoteOriginal, size(session?.sourceWidth, session?.sourceHeight)),
      // stream-size-v1: the Host encodes the source at its own size unless
      // something measured or declared cannot keep up; it says which.
      if (session != null && session.streamSize)
        SessionMetric(
          l10n.encodingScale,
          session.encodeScale == null
              ? '—'
              : '${number(session.encodeScale! / 10, 0)}% · ${session.encodeLimit?.label ?? '—'}',
        ),
      SessionMetric(
        l10n.autoResolution,
        session == null
            ? l10n.preparing4
            : !session.canResizeViewport
            ? l10n.notSupportedConnection
            : session.resizing
            ? l10n.applyingRemoteScreen
            : session.resizeError ?? l10n.use,
      ),
      SessionMetric(
        l10n.requestedScreen,
        autoViewport
            ? l10n.auto2(size(desired?.$1, desired?.$2))
            : size(options.maxWidth, options.maxHeight),
      ),
      SessionMetric(
        l10n.targetFps,
        l10n.requestedApplied(options.fps, session?.appliedFps ?? '—'),
      ),
      SessionMetric(
        l10n.targetBitrate,
        adaptive
            ? l10n.autoNowMaxMbps(mbps(telemetry?.bitrateTarget), mbps(telemetry?.bitrateMax))
            : l10n.requestedAppliedMbps(number(options.bitrate / 1000000), number(session?.appliedBitrate == null ? null : session!.appliedBitrate! / 1000000)),
      ),
      SessionMetric(l10n.captureMethod, backend),
      SessionMetric(
        l10n.hostEncoder,
        fresh
            ? hostEncoderLabel(telemetry?.encoder, telemetry?.legacyEncoder)
            : '—',
      ),
      if (encoder != null && encoder.adapterName != null)
        SessionMetric(
          encoder.hardwareVerified ? l10n.hostGpuUse : l10n.selectedHostGpu,
          encoder.adapterName!,
        ),
      if (fresh && hostEncoderFallback(telemetry?.encoder) != null)
        SessionMetric(l10n.encoderFallbackReason, hostEncoderFallback(telemetry?.encoder)!),
      SessionMetric(
        l10n.losslessSharpening,
        !fresh || session?.losslessTiles == null
            ? l10n.off5
            : session!.losslessTiles == 0
            ? l10n.waitingAppliesWhenScreenStill
            : l10n.active(session.losslessTiles!, session.losslessKbps == null ? '' : ' · ${number(session.losslessKbps! / 1000)} Mbps'),
      ),
      // cursor-v1: the remote shape is this PC's own cursor, not video.
      SessionMetric(
        l10n.mouseCursor,
        session == null
            ? '—'
            : session.cursorSeparated
            ? l10n.drawnPcCursorV1
            : l10n.video,
      ),
    ]),
    SessionMetricSection(l10n.framesNetwork, [
      SessionMetric(
        l10n.sourceCapture,
        fresh ? rate(session?.capturedFps) : '—',
      ),
      SessionMetric(
        l10n.hostEncoding,
        fresh ? rate(session?.encodedFps) : '—',
      ),
      SessionMetric(
        l10n.pcDecoding,
        rate(clientActive ? session.decodedFps : null),
      ),
      // gpuHardware: the H.264 decoder runs on a GPU; texturePath 1: the picture
      // reaches Flutter as a GPU surface instead of a CPU pixel buffer.
      SessionMetric(
        l10n.pcDecoder,
        !clientActive || session.gpuHardware == null
            ? '—'
            : session.gpuHardware!
            ? l10n.hardwareGpu(session.decodeTier == 2 ? l10n.otherAdapter : '', _vendor(session.decodeVendor))
            : l10n.softwareCpu,
      ),
      SessionMetric(
        l10n.frameDelivery,
        !clientActive || session.texturePath == null
            ? '—'
            : session.texturePath == 1
            ? l10n.gpuSurface
            : l10n.cpuPixelBuffer,
      ),
      SessionMetric(
        l10n.newTextureDelivery,
        measured(clientActive ? session.textureFps : null, 'fps'),
      ),
      SessionMetric(
        l10n.receiveRate,
        measured(clientActive ? session.receivedMbps : null, 'Mbps'),
      ),
      SessionMetric(
        'Host RTT',
        measured(clientActive ? session.rttMs : null, 'ms', 0),
      ),
      if (adaptive)
        SessionMetric(l10n.networkWait, measured(telemetry?.networkDelayMs, 'ms', 0)),
      SessionMetric(l10n.audio, session?.audioStatus ?? '—'),
    ], note: l10n.textureDeliveryMeasuredFlutterRender),
    SessionMetricSection(
      l10n.hostProcessing,
      [
        SessionMetric(l10n.displayLayoutCheck, measured(pipeline?.topologyMs, 'ms', 2)),
        SessionMetric(l10n.captureRead, measured(pipeline?.captureMs, 'ms', 2)),
        SessionMetric(l10n.colorConversion, measured(pipeline?.convertMs, 'ms', 2)),
        SessionMetric(l10n.encoding, measured(pipeline?.encodeMs, 'ms', 2)),
        SessionMetric(
          l10n.totalAvgP95,
          pipeline == null
              ? '—'
              : '${number(pipeline.workMs, 2)} / ${number(pipeline.p95Ms, 2)} ms',
        ),
        SessionMetric(l10n.processingMax, measured(pipeline?.maxMs, 'ms', 2)),
      ],
      note: pipeline == null
          ? session?.hostTelemetry != null && !fresh
                ? l10n.waitingRecentHostMeasurements
                : l10n.shownOnceHostReportsProcessing
          : l10n.lastSSampledNetworkWait(number(session?.hostTelemetry?.measuredMs == null ? null : session!.hostTelemetry!.measuredMs! / 1000), pipeline.samples),
    ),
    SessionMetricSection(l10n.pcProcessing, [
      SessionMetric(
        l10n.decodeAvg,
        measured(clientActive ? session.clientDecodeMs : null, 'ms', 2),
      ),
      SessionMetric(
        l10n.decodeMax,
        measured(clientActive ? session.clientDecodeMaxMs : null, 'ms', 2),
      ),
      SessionMetric(
        l10n.videoQueue,
        clientActive &&
                session.clientQueuePackets != null &&
                session.clientQueueMs != null
            ? l10n.ms(session.clientQueuePackets!, number(session.clientQueueMs))
            : '—',
      ),
      SessionMetric(
        l10n.replacedBeforeDelivery,
        clientActive && session.skippedDecodedFrames != null
            ? l10n.text33(session.skippedDecodedFrames!)
            : '—',
      ),
      SessionMetric(
        l10n.appCpu,
        clientActive
            ? appCpuLabel(session.appCpuPct10, session.systemCpuPct10)
            : '—',
      ),
      SessionMetric(
        l10n.gpuVideoDecoding,
        clientActive
            ? gpuDecodeLabel(session.gpuDecodePct10, session.decodeTier)
            : '—',
      ),
    ], note: l10n.averagesCoverRecentPollingWindow),
    inputLatencySection(session),
  ];
}

String _tenths(int tenths) {
  final whole = tenths ~/ 10, frac = tenths % 10;
  return frac == 0 ? '$whole%' : '$whole.$frac%';
}

/// "12% (PC 전체 34%)": this app's share of the whole PC's CPU, with the PC's
/// total beside it when the platform lets us read it. Tenths of a percent.
String appCpuLabel(int? appTenths, int? systemTenths) {
  if (appTenths == null) return '—';
  final app = _tenths(appTenths);
  return systemTenths == null ? app : l10n.pcTotal(app, _tenths(systemTenths));
}

/// "8% · GPU 디코더" / "알 수 없음 · GPU 디코더" / "CPU 디코더": the video
/// decode engine use of this app and the path decoding takes (decodeTier 0 is
/// the CPU decoder). A CPU decode has no GPU engine to read.
String gpuDecodeLabel(int? tenths, int? decodeTier) {
  if (decodeTier == 0) return l10n.cpuDecoder;
  final path = decodeTier == null ? '' : l10n.gpuDecoder;
  return tenths == null ? l10n.unknown6(path) : '${_tenths(tenths)}$path';
}

/// " · 0x8086" for a known PCI vendor ID, otherwise nothing.
String _vendor(int? id) =>
    id == null || id == 0 ? '' : ' · 0x${id.toRadixString(16).padLeft(4, '0')}';

import 'audio_stats.dart';
import 'gamepad.dart';
import 'stream_telemetry.dart';
import 'stream_observability.dart';

/// Diagnostic protocol the broker acknowledges before a client may send
/// `streamStats`; released brokers reject the whole batch otherwise.
const streamStatsProtocol = 'stream-stats-v1';

/// Receives one window's report; the broker API decides whether to send it.
typedef StreamStatsSink = void Function(Map<String, Object> values);

/// Integer/boolean fields of one `streamStats` diagnostic row. Rates are
/// scaled (fps ×10, times in µs) because the wire accepts integers only.
const streamStatsFields = <String>{
  'hostQueuePackets', 'hostQueueBytes', 'hostQueueOldestUs', 'hostWriteUs',
  'readbackUs', 'readbackMaxUs', 'serviceCpuPct10', 'workerCpuPct10',
  'relayQueueBytes', 'relayForwardUs', 'relayVideoQueuePackets',
  'relayVideoQueueOldestUs', 'clientInboxPackets', 'clientInboxBytes',
  'clientInboxOldestUs', 'inputGateWaitUs', 'inputGateWaitMaxUs',
  'inputGateWaitCount', 'inputGateQueuePackets',
  'samples',
  'windowMs',
  'width',
  'height',
  // Already accepted by released brokers. With independent outputs, decoded
  // FPS/counters sum the outputs; decode time is per call, queue age is shared,
  // and dimensions are the primary stream's. Texture FPS counts Flutter takes,
  // not the native companion's scan-out. Never read the sum as each monitor's Hz.
  'monitors',
  'srcWidth',
  'srcHeight',
  'decodeFps10',
  'decodeFpsMin10',
  // Independent outputs only: the window mean of the slowest output's
  // decoded / Host captured / Host encoded rate in each reading (x10), so a
  // monitor that falls behind shows apart from the summed rates. Brokers
  // before these fields drop them; the row itself stays valid.
  'decodeFpsLow10',
  'captureFpsLow10',
  'encodeFpsLow10',
  'textureFps10',
  'captureFps10',
  'encodeFps10',
  'receiveKbps',
  'bitrateKbps',
  'targetKbps',
  'auto',
  'networkDelayMs',
  'rttMs',
  'rttMaxMs',
  'videoGapMaxMs',
  'hostWorkUs',
  'hostP95Us',
  'hostMaxUs',
  'captureUs',
  'convertUs',
  'encodeUs',
  'decodeUs',
  'decodeMaxUs',
  'queueMaxMs',
  'skipped',
  'losslessTiles',
  'losslessKbps',
  'inputHostP50Us',
  'inputHostP95Us',
  'inputFrameP50Us',
  'inputFrameP95Us',
  'topologyMaxUs',
  'captureMaxUs',
  'convertMaxUs',
  'encodeMaxUs',
  // texture-pacing-v1: mean decoded -> taken by Flutter wait and Flutter's
  // texture upload (us), frames queued behind a waiting frame and frames
  // skipped in this window (counts; `skipped` above is the stream total).
  'textureWaitUs',
  'textureUploadUs',
  'queuedFrames',
  'skippedFrames',
  // How the client renders: hardware GPU (false = WARP / basic render
  // driver), texture path (0 = CPU pixel buffer, 1 = GPU surface), the
  // composition rate Flutter paces frames at, client in a Remote Desktop session.
  'gpuHardware',
  'texturePath',
  'displayHz',
  'remoteSession',
  // Where H.264 decodes (0 CPU, 1 hardware on the render adapter, 2 another
  // adapter) and its PCI vendor id. Brokers before these fields drop them.
  'decodeTier',
  'decodeVendor',
  // Android: the low-latency configuration the H.264 decoder accepted (bits:
  // 1 KEY_LOW_LATENCY, 2 real-time priority, 4 operating rate, 8 vendor keys,
  // 16 FEATURE_LowLatency declared) and the vendor key set its name matched
  // (0 none, 1 Qualcomm, 2 Exynos, 3 Kirin, 4 Amlogic, 5 MediaTek). Brokers
  // before these fields drop them.
  'decoderLowLatency',
  'decoderVendorKeys',
  // Backpressure that does not end the stream: video
  // packets that found the native decode queue full and waited, and audio
  // dropped to keep the playback queue within 200 ms (ms of 48 kHz audio).
  'mediaWaits',
  'audioDropMs',
  // Where input -> frame goes (latest rolling p50/p95, us). Host clock
  // (absent from older Hosts): injection -> capture tick of the changed frame, that
  // tick -> encoder output. This PC's clock: tagged packet arrival -> native
  // engine accepted it, accepted -> handed to the texture. The rest of
  // inputFrame is client -> host and host encoder -> this PC's arrival.
  'hostInputCaptureP50Us',
  'hostInputCaptureP95Us',
  'hostCaptureEncodedP50Us',
  'hostCaptureEncodedP95Us',
  'inputPushP50Us',
  'inputDecodeP50Us',
  // The tagged packet after the encoder (latest rolling p50/p95, us). Host
  // clock (absent from older Hosts): encoder output -> its WebSocket write start,
  // and that write. Relay clock (absent from older brokers): the host's complete
  // message -> written toward this PC. This PC's clock: the tag's arrival ->
  // the packet's arrival (its bytes after the first), and its size (bytes).
  'hostSendQueueP50Us',
  'hostSendQueueP95Us',
  'hostSendWriteP50Us',
  'hostSendWriteP95Us',
  'relayForwardP50Us',
  'relayForwardP95Us',
  'inputTransferP50Us',
  'inputTransferP95Us',
  'inputFrameBytes',
  // Per input (frame-timing-v1 tags): the host's tag write -> the packet's
  // arrival here, the uplink estimate and the host's durations removed.
  'inputDownlinkP50Us',
  'inputDownlinkP95Us',
  // Android input in the window (counts only, never content): touch mode
  // (0 direct, 1 trackpad), taps, right clicks, drags started, wheel steps,
  // pinches, keyboard opens, text-input-v1 commits and their UTF-16 units,
  // characters typed as key presses (Hosts without text-input-v1), characters
  // that could not be sent, PC keys pressed, hardware mouse button changes,
  // and whether the session negotiated text-input-v1. Brokers before these
  // fields drop them; the row itself stays valid.
  'touchMode',
  'taps',
  'rightClicks',
  'dragStarts',
  'wheelSteps',
  'pinches',
  'keyboardOpens',
  'textCommits',
  'textUnits',
  'textKeyChars',
  'textDropped',
  'keyPresses',
  'mouseButtons',
  'textInput',
  // Android picture sizing: the Windows display scale the Host applied
  // (display-scale-v1, percent; 0 = not applied), the one asked for, and
  // device pixels per remote pixel (absent while zoomed or fitted).
  'displayScale',
  'displayScaleWanted',
  'pixelScale',
  // Android direct touch: whether the session negotiated touch-input-v1,
  // touch frames sent in the window and the most contacts held at once
  // (counts only, never positions).
  'touchInput',
  'touchFrames',
  'touchContacts',
  // stream-size-v1: the Host's picture scale of the source (per mille), why
  // it is smaller (EncodeLimit code: 0 none, 1 decoder size, 2 decoder rate,
  // 3 client decode load, 4 Host encoder load, 5 administrator cap) and this
  // client's decoder busy share (per mille 0..1000, window mean: 203 is 20.3%,\n  // never above 1000; the share of wall time a frame was inside the decoder).
  'encodeScale',
  'encodeLimit',
  'decoderBusy',
  // client-load-v1: this app's CPU share of the whole PC, the PC's CPU, the
  // app's GPU video-decode and 3D engine use (tenths of a percent, 0..1000;
  // absent when not measured). Brokers before these fields drop them.
  'appCpuPct10',
  'systemCpuPct10',
  'gpuDecodePct10',
  'gpu3dPct10',
  // udp-media-v1 probe, no media (Phase 2): outcome (1 reachable, 2 socket
  // error, 3 name lookup failed, 4 BIND timeout, 5 no PONG; absent without a
  // grant or before the outcome), burst RTT p50/p95 and loss (%), index of the
  // relay that answered, keepalive PINGs sent and unanswered this session.
  'udpProbe',
  'udpRttP50Ms',
  'udpRttP95Ms',
  'udpLossPct',
  'udpRelayIndex',
  'udpKeepSent',
  'udpKeepLost',
  // Phase 3, only once the keys are done: the path video counts from (0 WebSocket,
  // 1 UDP), returns from UDP to WebSocket and datagrams given up as lost.
  'udpActivePath',
  'udpDirectPath',
  'udpDirectRttMs',
  'udpRelayRttMs',
  'udpFallbacks',
  'udpGaps',
  // Android leg: KEYFRAME requests sent, 1 s stretches on UDP with nothing
  // newer made whole (the Host's 271 cause 3) and the last one's wait (1
  // decoder queue full, 2 refused, 3 loss given up, 4 another epoch, 5 a
  // repair, 6 nothing of it arrived).
  'udpKeyRequests',
  'udpStalls',
  'udpStallCause',
  // 1 while the last reading of the native leg failed.
  'udpLegFailed',
  // audio-stats-v1 (audio_stats.dart): this client's jitter buffer and the
  // Host's capture and send path. Brokers before these fields drop them.
  ...audioStatsFields,
  // With sound on: the stream's audio format (1 PCM, 2 Opus audio-opus-v1)
  // and the native player's last error as an unsigned 32-bit status (0 none;
  // Windows: its HRESULT, Android: 0xffffffff for any failure).
  'audioFormat',
  'audioError',
  // audio-path-v1 (Android, audio_stats.dart): which decoder, its output
  // format, the track's start level and buffer, audio focus, the API level.
  ...audioPathFields,
  // Windows leg, lifetime: video that waited for a full decode queue, skips to
  // the next key frame, NACK datagrams (udpKeyRequests above: KEYFRAME sent).
  'udpQueueWaits',
  'udpQueueSkips',
  'udpNacks',
  // stream-stall-v1: displayed-video gaps of 250 ms or more in the window and
  // the longest (ms); each one is also its own streamStall row.
  'stalls',
  'stallMaxMs',
  // The frame rate the Host's capture loop paces at now (its encode-limit
  // controller may lower it below the requested rate before the picture size).
  'fpsTarget',
  // gamepad-v1 (gamepad.dart GamepadTally): granted, pads held, states sent
  // and refused, rumbles received. Brokers before these fields drop them.
  ...gamepadStatsFields,
};

/// One reading of the values the 연결 통계 dialog shows, taken on the
/// session's 1 s statistics tick.
class StreamStatsSample {
  const StreamStatsSample({
    this.observability,
    required this.width,
    required this.height,
    this.sourceWidth,
    this.sourceHeight,
    this.decodedFps,
    this.decodedFpsLowest,
    this.textureFps,
    this.receivedMbps,
    this.rttMs,
    this.clientDecodeMs,
    this.clientDecodeMaxMs,
    this.clientQueueMs,
    this.skippedDecodedFrames,
    this.appliedBitrate,
    this.losslessTiles,
    this.losslessKbps,
    this.host,
    this.inputHostP50Ms,
    this.inputHostP95Ms,
    this.inputFrameP50Ms,
    this.inputFrameP95Ms,
    this.inputPushP50Ms,
    this.inputDecodeP50Ms,
    this.inputTransferP50Ms,
    this.inputTransferP95Ms,
    this.inputFrameBytes,
    this.inputDownlinkP50Ms,
    this.inputDownlinkP95Ms,
    this.textureWaitMs,
    this.textureUploadMs,
    this.textureQueuedFrames,
    this.skippedFrames,
    this.gpuHardware,
    this.remoteSession,
    this.texturePath,
    this.displayHz,
    this.mediaWaits,
    this.videoGapMaxMs,
    this.audioDroppedFrames,
    this.encodeScale,
    this.encodeLimit,
    this.decoderBusy,
    this.appCpuPct10,
    this.systemCpuPct10,
    this.gpuDecodePct10,
    this.gpu3dPct10,
    this.decodeTier,
    this.decodeVendor,
    this.decoderLowLatency,
    this.decoderVendorKeys,
    this.udpProbe,
    this.audio,
    this.audioFormat,
    this.audioError,
    this.audioPath,
    this.stalls,
    this.stallMaxMs,
    this.fpsTarget,
  });

  /// audio-stats-v1 changes of this reading; null without audio.
  final AudioStatsReading? audio;

  /// See RemoteSession.audioFormat / audioErrorCode; null with sound off.
  final int? audioFormat, audioError;

  /// audio-path-v1 values of this reading (audioPathValues); the report
  /// carries the latest. Null or empty for an engine without them.
  final Map<String, Object>? audioPath;

  /// stream-stall-v1 of this reading: new stalls and the longest (ms).
  final int? stalls, stallMaxMs;

  /// The Host's applied capture rate (stream.stats fpsTarget).
  final int? fpsTarget;

  /// The session's UdpMedia.report at this reading: fixed numbers, already
  /// session totals, so the report carries the latest. Null without a grant.
  final Map<String, int>? udpProbe;

  /// Tenths of a percent (see RemoteSession.appCpuPct10).
  final Map<String, Object?>? observability;
  final int? appCpuPct10, systemCpuPct10, gpuDecodePct10, gpu3dPct10;

  /// stream-size-v1: the Host's latest picture scale (per mille) and its
  /// EncodeLimit code, and this client's decoder busy share (per mille).
  final int? encodeScale, encodeLimit, decoderBusy;

  /// Counts of this 1 s reading: packets that waited for a full native
  /// decode queue, and 48 kHz audio frames the playback queue dropped.
  final int? mediaWaits, audioDroppedFrames;

  /// Longest wait between two video packets in this 1 s reading.
  final double? videoGapMaxMs;

  /// texture-pacing-v1 values of this 1 s reading (means and counts).
  final double? textureWaitMs, textureUploadMs;
  final int? textureQueuedFrames, skippedFrames;

  /// Render facts of the client (see RemoteSession).
  final bool? gpuHardware, remoteSession;
  final int? texturePath, displayHz;

  /// Where H.264 decodes (0 CPU, 1 hardware on the render adapter, 2 on
  /// another one) and that adapter's PCI vendor id.
  final int? decodeTier, decodeVendor;

  /// Android: what the H.264 decoder was configured with (see RemoteSession).
  final int? decoderLowLatency, decoderVendorKeys;

  /// input-latency-v1 percentiles; already a rolling window of samples, so
  /// the report carries the latest values rather than an average of them.
  final double? inputHostP50Ms, inputHostP95Ms;
  final double? inputFrameP50Ms, inputFrameP95Ms;

  /// The client's share of input -> frame (InputLatencySummary.framePush /
  /// frameDecode p50).
  final double? inputPushP50Ms, inputDecodeP50Ms;

  /// The tagged packet's tag -> packet arrival (InputLatencySummary.transfer)
  /// and its size in bytes.
  final double? inputTransferP50Ms, inputTransferP95Ms, inputFrameBytes;

  /// InputLatencySummary.downlink p50/p95.
  final double? inputDownlinkP50Ms, inputDownlinkP95Ms;
  final int width, height;
  final int? sourceWidth, sourceHeight, appliedBitrate, skippedDecodedFrames;
  final int? losslessTiles;
  final double? decodedFps, textureFps, receivedMbps, rttMs;

  /// Independent outputs: the slowest output's decoded rate (decodedFps sums them).
  final double? decodedFpsLowest;
  final double? clientDecodeMs, clientDecodeMaxMs, clientQueueMs;
  final double? losslessKbps;

  /// Fresh host telemetry only; stale host values are not repeated.
  final HostStreamTelemetry? host;
}

class _Mean {
  double _sum = 0;
  int _count = 0;
  void add(double? value) {
    if (value == null || !value.isFinite || value < 0) return;
    _sum += value;
    _count++;
  }

  int? scaled(double factor) =>
      _count == 0 ? null : _bounded(_sum / _count * factor);
}

class _Max {
  double? _value;
  void add(double? value) {
    if (value == null || !value.isFinite || value < 0) return;
    if (_value == null || value > _value!) _value = value;
  }

  int? scaled(double factor) =>
      _value == null ? null : _bounded(_value! * factor);
}

class _Min {
  double? _value;
  void add(double? value) {
    if (value == null || !value.isFinite || value < 0) return;
    if (_value == null || value < _value!) _value = value;
  }

  int? scaled(double factor) =>
      _value == null ? null : _bounded(_value! * factor);
}

class _Sum {
  int? _value;
  void add(int? value) {
    if (value == null || value < 0) return;
    _value = (_value ?? 0) + value;
  }

  int? get value => _value == null ? null : _bounded(_value!.toDouble());
}

int _bounded(double value) => value.round().clamp(0, 0xffffffff);

/// Averages (and a few extremes) of a session's statistics over a reporting
/// window, so one diagnostic row stands for e.g. 30 s of streaming rather
/// than a single noisy 1 s reading. Holds no pixels, input or text.
class StreamStatsWindow {
  StreamStatsWindow({
    this.period = const Duration(seconds: 30),
    this.firstPeriod = const Duration(seconds: 10),
  });

  /// The first window is short so a brief session still leaves one report.
  final Duration period, firstPeriod;
  int _reports = 0;
  int? _startUs;
  int _samples = 0;
  StreamStatsSample? _last;
  var _decode = _Mean(), _texture = _Mean(), _capture = _Mean();
  var _encode = _Mean(), _receive = _Mean(), _rtt = _Mean(), _work = _Mean();
  var _captureMs = _Mean(), _convert = _Mean(), _encodeMs = _Mean();
  var _clientDecode = _Mean(), _lossless = _Mean(), _delay = _Mean();
  var _decodeMin = _Min();
  var _decodeLow = _Mean(), _captureLow = _Mean(), _encodeLow = _Mean();
  var _rttMax = _Max(), _p95 = _Max(), _hostMax = _Max(), _decodeMax = _Max();
  var _videoGapMax = _Max();
  var _queue = _Max(), _tiles = _Max();
  var _topologyMax = _Max(), _captureMax = _Max();
  var _convertMax = _Max(), _encodeMax = _Max();
  var _wait = _Mean(), _upload = _Mean();
  var _queued = _Sum(), _skipped = _Sum();
  var _mediaWaits = _Sum(), _audioDropped = _Sum();
  var _decoderBusy = _Mean();
  var _appCpu = _Mean(), _systemCpu = _Mean(), _gpuDecode = _Mean();
  var _gpu3d = _Mean();
  var _audio = AudioStatsWindow();
  var _stalls = _Sum(), _stallMax = _Max();
  int? _fpsTarget;
  double? _inputHost50, _inputHost95, _inputFrame50, _inputFrame95;
  double? _inputPush50, _inputDecode50;
  double? _transfer50, _transfer95, _frameBytes, _downlink50, _downlink95;
  HostInputFrameMetrics? _hostInput;
  SendPathMetrics? _hostSend, _relayForward;

  static int? _latest(double? ms) =>
      ms == null || !ms.isFinite || ms < 0 ? null : _bounded(ms * 1000);

  int get samples => _samples;

  /// Size of the window's samples; a new size starts a new window.
  (int, int)? get size => _last == null ? null : (_last!.width, _last!.height);

  void add(StreamStatsSample sample, int nowUs) {
    _startUs ??= nowUs;
    _samples++;
    _last = sample;
    _decode.add(sample.decodedFps);
    _decodeMin.add(sample.decodedFps);
    _decodeLow.add(sample.decodedFpsLowest);
    _texture.add(sample.textureFps);
    _receive.add(sample.receivedMbps);
    _rtt.add(sample.rttMs);
    _rttMax.add(sample.rttMs);
    _videoGapMax.add(sample.videoGapMaxMs);
    _clientDecode.add(sample.clientDecodeMs);
    _decodeMax.add(sample.clientDecodeMaxMs);
    _queue.add(sample.clientQueueMs);
    _tiles.add(sample.losslessTiles?.toDouble());
    _lossless.add(sample.losslessKbps);
    _wait.add(sample.textureWaitMs);
    _upload.add(sample.textureUploadMs);
    _queued.add(sample.textureQueuedFrames);
    _skipped.add(sample.skippedFrames);
    _mediaWaits.add(sample.mediaWaits);
    _audioDropped.add(sample.audioDroppedFrames);
    _audio.add(sample.audio);
    _decoderBusy.add(sample.decoderBusy?.toDouble());
    _appCpu.add(sample.appCpuPct10?.toDouble());
    _systemCpu.add(sample.systemCpuPct10?.toDouble());
    _gpuDecode.add(sample.gpuDecodePct10?.toDouble());
    _gpu3d.add(sample.gpu3dPct10?.toDouble());
    _stalls.add(sample.stalls);
    _stallMax.add(sample.stallMaxMs == null || sample.stallMaxMs == 0 ? null : sample.stallMaxMs!.toDouble());
    _fpsTarget = sample.fpsTarget ?? _fpsTarget;
    _inputHost50 = sample.inputHostP50Ms ?? _inputHost50;
    _inputHost95 = sample.inputHostP95Ms ?? _inputHost95;
    _inputFrame50 = sample.inputFrameP50Ms ?? _inputFrame50;
    _inputFrame95 = sample.inputFrameP95Ms ?? _inputFrame95;
    _inputPush50 = sample.inputPushP50Ms ?? _inputPush50;
    _inputDecode50 = sample.inputDecodeP50Ms ?? _inputDecode50;
    _transfer50 = sample.inputTransferP50Ms ?? _transfer50;
    _transfer95 = sample.inputTransferP95Ms ?? _transfer95;
    _frameBytes = sample.inputFrameBytes ?? _frameBytes;
    _downlink50 = sample.inputDownlinkP50Ms ?? _downlink50;
    _downlink95 = sample.inputDownlinkP95Ms ?? _downlink95;
    final host = sample.host;
    if (host != null) {
      _hostInput = host.inputFrame ?? _hostInput;
      _hostSend = host.inputSend ?? _hostSend;
      _relayForward = host.relayForward ?? _relayForward;
      _capture.add(host.capturedFps);
      _encode.add(host.encodedFps);
      _captureLow.add(host.lowestCapturedFps);
      _encodeLow.add(host.lowestEncodedFps);
      _delay.add(host.networkDelayMs);
      final pipeline = host.pipeline;
      if (pipeline != null) {
        _work.add(pipeline.workMs);
        _p95.add(pipeline.p95Ms);
        _hostMax.add(pipeline.maxMs);
        _captureMs.add(pipeline.captureMs);
        _convert.add(pipeline.convertMs);
        _encodeMs.add(pipeline.encodeMs);
        _topologyMax.add(pipeline.topologyMaxMs);
        _captureMax.add(pipeline.captureMaxMs);
        _convertMax.add(pipeline.convertMaxMs);
        _encodeMax.add(pipeline.encodeMaxMs);
      }
    }
  }

  bool due(int nowUs) =>
      _startUs != null &&
      nowUs - _startUs! >=
          (_reports == 0 ? firstPeriod : period).inMicroseconds;

  /// The window's report, or null before any sample; resets the window.
  Map<String, Object>? take(int nowUs) {
    final last = _last;
    final start = _startUs;
    if (last == null || start == null || _samples == 0) return null;
    final host = last.host;
    final values = <String, Object?>{
      if (last.observability != null)
        for (final entry in clientObservationBounds.entries)
          if (streamStatsFields.contains(entry.key) &&
              last.observability![entry.key] is int &&
              (last.observability![entry.key] as int) >= 0 &&
              (last.observability![entry.key] as int) <= entry.value)
            entry.key: last.observability![entry.key],
      'samples': _samples,
      'windowMs': _bounded((nowUs - start) / 1000),
      'width': last.width,
      'height': last.height,
      'srcWidth': last.sourceWidth,
      'srcHeight': last.sourceHeight,
      'decodeFps10': _decode.scaled(10),
      'decodeFpsMin10': _decodeMin.scaled(10),
      'decodeFpsLow10': _decodeLow.scaled(10),
      'captureFpsLow10': _captureLow.scaled(10),
      'encodeFpsLow10': _encodeLow.scaled(10),
      'textureFps10': _texture.scaled(10),
      'captureFps10': _capture.scaled(10),
      'encodeFps10': _encode.scaled(10),
      'receiveKbps': _receive.scaled(1000),
      'bitrateKbps': last.appliedBitrate == null
          ? null
          : _bounded(last.appliedBitrate! / 1000),
      'targetKbps': host?.bitrateTarget == null
          ? null
          : _bounded(host!.bitrateTarget! / 1000),
      'auto': host?.automaticBitrate,
      'networkDelayMs': _delay.scaled(1),
      'rttMs': _rtt.scaled(1),
      'rttMaxMs': _rttMax.scaled(1),
      'videoGapMaxMs': _videoGapMax.scaled(1),
      'hostWorkUs': _work.scaled(1000),
      'hostP95Us': _p95.scaled(1000),
      'hostMaxUs': _hostMax.scaled(1000),
      'captureUs': _captureMs.scaled(1000),
      'convertUs': _convert.scaled(1000),
      'encodeUs': _encodeMs.scaled(1000),
      'decodeUs': _clientDecode.scaled(1000),
      'decodeMaxUs': _decodeMax.scaled(1000),
      'queueMaxMs': _queue.scaled(1),
      'skipped': last.skippedDecodedFrames,
      'losslessTiles': _tiles.scaled(1),
      'losslessKbps': _lossless.scaled(1),
      'inputHostP50Us': _latest(_inputHost50),
      'inputHostP95Us': _latest(_inputHost95),
      'inputFrameP50Us': _latest(_inputFrame50),
      'inputFrameP95Us': _latest(_inputFrame95),
      'hostInputCaptureP50Us': _latest(_hostInput?.injectToCaptureP50Ms),
      'hostInputCaptureP95Us': _latest(_hostInput?.injectToCaptureP95Ms),
      'hostCaptureEncodedP50Us': _latest(_hostInput?.captureToEncodedP50Ms),
      'hostCaptureEncodedP95Us': _latest(_hostInput?.captureToEncodedP95Ms),
      'inputPushP50Us': _latest(_inputPush50),
      'inputDecodeP50Us': _latest(_inputDecode50),
      'hostSendQueueP50Us': _latest(_hostSend?.firstP50Ms),
      'hostSendQueueP95Us': _latest(_hostSend?.firstP95Ms),
      'hostSendWriteP50Us': _latest(_hostSend?.secondP50Ms),
      'hostSendWriteP95Us': _latest(_hostSend?.secondP95Ms),
      'relayForwardP50Us': _latest(_relayForward?.firstP50Ms),
      'relayForwardP95Us': _latest(_relayForward?.firstP95Ms),
      'inputTransferP50Us': _latest(_transfer50),
      'inputTransferP95Us': _latest(_transfer95),
      'inputFrameBytes': _frameBytes == null || !_frameBytes!.isFinite
          ? null
          : _bounded(_frameBytes!),
      'inputDownlinkP50Us': _latest(_downlink50),
      'inputDownlinkP95Us': _latest(_downlink95),
      'topologyMaxUs': _topologyMax.scaled(1000),
      'captureMaxUs': _captureMax.scaled(1000),
      'convertMaxUs': _convertMax.scaled(1000),
      'encodeMaxUs': _encodeMax.scaled(1000),
      'textureWaitUs': _wait.scaled(1000),
      'textureUploadUs': _upload.scaled(1000),
      'queuedFrames': _queued.value,
      'skippedFrames': _skipped.value,
      'gpuHardware': last.gpuHardware,
      'texturePath': last.texturePath,
      'displayHz': last.displayHz,
      'remoteSession': last.remoteSession,
      'decodeTier': last.decodeTier,
      'decodeVendor': last.decodeVendor,
      'decoderLowLatency': last.decoderLowLatency,
      'decoderVendorKeys': last.decoderVendorKeys,
      'mediaWaits': _mediaWaits.value,
      'audioDropMs': _audioDropped.value == null
          ? null
          : _bounded(_audioDropped.value! / 48),
      'encodeScale': last.encodeScale,
      'encodeLimit': last.encodeLimit,
      'decoderBusy': _decoderBusy.scaled(1),
      'appCpuPct10': _appCpu.scaled(1),
      'systemCpuPct10': _systemCpu.scaled(1),
      'gpuDecodePct10': _gpuDecode.scaled(1),
      'gpu3dPct10': _gpu3d.scaled(1),
      'stalls': _stalls.value,
      'stallMaxMs': _stallMax.scaled(1),
      'fpsTarget': _fpsTarget,
      'audioFormat': last.audioFormat,
      'audioError': last.audioError,
      ...?last.audioPath,
      ...?last.udpProbe,
      ..._audio.take(),
    };
    _reset();
    _reports++;
    return {
      for (final entry in values.entries)
        if (entry.value is bool ||
            (entry.value is int && (entry.value as int) >= 0))
          entry.key: entry.value as Object,
    };
  }

  void _reset() {
    _startUs = null;
    _samples = 0;
    _last = null;
    _decode = _Mean();
    _texture = _Mean();
    _capture = _Mean();
    _encode = _Mean();
    _receive = _Mean();
    _rtt = _Mean();
    _work = _Mean();
    _captureMs = _Mean();
    _convert = _Mean();
    _encodeMs = _Mean();
    _clientDecode = _Mean();
    _lossless = _Mean();
    _delay = _Mean();
    _decodeMin = _Min();
    _decodeLow = _Mean();
    _captureLow = _Mean();
    _encodeLow = _Mean();
    _rttMax = _Max();
    _videoGapMax = _Max();
    _p95 = _Max();
    _hostMax = _Max();
    _decodeMax = _Max();
    _queue = _Max();
    _tiles = _Max();
    _wait = _Mean();
    _upload = _Mean();
    _queued = _Sum();
    _skipped = _Sum();
    _mediaWaits = _Sum();
    _audioDropped = _Sum();
    _decoderBusy = _Mean();
    _appCpu = _Mean();
    _systemCpu = _Mean();
    _gpuDecode = _Mean();
    _gpu3d = _Mean();
    _audio = AudioStatsWindow();
    _stalls = _Sum();
    _stallMax = _Max();
    _fpsTarget = null;
    _inputHost50 = _inputHost95 = _inputFrame50 = _inputFrame95 = null;
    _inputPush50 = _inputDecode50 = null;
    _transfer50 = _transfer95 = _frameBytes = _downlink50 = _downlink95 = null;
    _hostInput = null;
    _hostSend = _relayForward = null;
    _topologyMax = _Max();
    _captureMax = _Max();
    _convertMax = _Max();
    _encodeMax = _Max();
  }
}

/// audio-stats-v1: the sound path's fixed numbers for the `streamStats`
/// diagnostic row. Client side: the native engine's jitter buffer (Windows
/// runner AudioJitterBuffer, Android AndroidAudioTrim). Host side: the
/// stream.stats members `audioCapture` (Host worker: WASAPI capture -> packet,
/// per packet) and `audioSend` (Host service: packets handed on and dropped,
/// and the WebSocket send queue wait of each audio packet written).
library;

/// Window field names (all non-negative integers).
const audioStatsFields = <String>{
  // This client: mean depth (buffered + in the device) and playback latency
  // estimate while sound played, the target at the end, underruns (the device
  // ran dry while the sound went on) and overruns (a backlog cut to the
  // target), frames repeated / dropped by the drift correction, and the mean
  // A/V offset (the picture handed to the texture minus the sound heard at
  // that moment): audioAvLagMs when the sound is behind, audioAvLeadMs when
  // ahead.
  'audioDepthMs', 'audioTargetMs', 'audioLatencyMs', 'audioUnderruns',
  'audioOverruns', 'audioInserted', 'audioTrimmed', 'audioAvLagMs',
  'audioAvLeadMs',
  // audio-opus-v1 only: 10 ms frames this client made up for lost or late
  // packets (Windows: Opus PLC; Android: the last frame repeated and faded).
  'audioConcealed',
  // The Host: capture -> packet mean/max (us), packets handed on / dropped,
  // WebSocket send-queue wait mean/max (us) of the audio written there.
  'hostAudioCaptureUs', 'hostAudioCaptureMaxUs', 'hostAudioSent',
  'hostAudioDropped', 'hostAudioQueueUs', 'hostAudioQueueMaxUs',
};

/// audio-path-v1 (2026-11-11): how the Android engine plays the sound, the
/// latest reading of the window (fixed numbers and flags, never audio
/// content). Brokers before these fields drop them; the row stays valid.
///  - audioDecoder: 0 none (PCM, or no sound), 1 the platform's software Opus
///    decoder, 2 another Opus decoder, 3 none could be used.
///  - audioOutRate / audioOutChannels / audioOutFloat: the output format that
///    decoder stated (converted to 48 kHz s16 stereo for the track).
///  - audioStartMs: the level the track waits for before it plays;
///    audioBufferMs its effective buffer; audioBufferFollows: the buffer
///    follows the jitter cap (before Android 12, or no start threshold);
///    audioFastTrack: Android gave it the low-latency mixer path.
///  - audioTrackUnderruns: the track's own underrun count (a silent VM too).
///  - audioTrackRestarts / audioTrackStuck: tracks made again this stream
///    because the platform lost one / one was written to and did not play.
///  - audioFocus: 0 none, 1 held, 2 waiting, 3 paused (a call), 4 lost;
///    audioMuted: focus mutes the track now.
///  - audioOpusUnusable: no Opus decoder of the device plays this app run (the
///    next connections ask for PCM); androidApi: the Android API level.
const audioPathFields = <String>{
  'audioDecoder', 'audioOutRate', 'audioOutChannels', 'audioOutFloat',
  'audioStartMs', 'audioBufferMs', 'audioBufferFollows', 'audioFastTrack',
  'audioTrackUnderruns', 'audioTrackRestarts', 'audioTrackStuck',
  'audioFocus', 'audioMuted', 'audioOpusUnusable', 'androidApi',
};

/// The [audioPathFields] of one native statistics reading, each within its
/// bound (anything else is left out); empty for an engine without them.
Map<String, Object> audioPathValues(Map<String, dynamic> stats) {
  int? bounded(Object? value, int maximum) => value is num &&
          value.isFinite &&
          value >= 0 &&
          value <= maximum
      ? value.round()
      : null;
  final values = <String, Object>{
    'audioDecoder': ?bounded(stats['audioDecoder'], 3),
    'audioOutRate': ?bounded(stats['audioOutRate'], 384000),
    'audioOutChannels': ?bounded(stats['audioOutChannels'], 8),
    'audioStartMs': ?bounded(stats['audioStartMs'], 60000),
    'audioTrackUnderruns': ?bounded(stats['audioTrackUnderruns'], 0xffffffff),
    'audioTrackRestarts': ?bounded(stats['audioTrackRestarts'], 0xffffffff),
    'audioTrackStuck': ?bounded(stats['audioTrackStuck'], 0xffffffff),
    'audioFocus': ?bounded(stats['audioFocus'], 4),
    'androidApi': ?bounded(stats['androidApi'], 1000),
  };
  // Only an engine that reports the path: its buffer, as audioBufferMs.
  if (values.isNotEmpty) {
    final buffer = bounded(stats['audioBufferMs'], 60000);
    if (buffer != null) values['audioBufferMs'] = buffer;
  }
  for (final key in [
    'audioOutFloat',
    'audioBufferFollows',
    'audioFastTrack',
    'audioMuted',
    'audioOpusUnusable',
  ]) {
    if (values.isNotEmpty && stats[key] is bool) values[key] = stats[key] as bool;
  }
  return values;
}

/// One 1 s statistics reading: the native engine's changes since the
/// previous reading, and the Host's stream.stats audio received meanwhile.
class AudioStatsReading {
  const AudioStatsReading({
    this.depthMs,
    this.latencyMs,
    this.targetMs,
    this.underruns = 0,
    this.overruns = 0,
    this.insertedFrames = 0,
    this.trimmedFrames = 0,
    this.concealed,
    this.avOffsetSumMs = 0,
    this.avOffsetSamples = 0,
    this.hostCapturePackets = 0,
    this.hostCaptureSumUs = 0,
    this.hostCaptureMaxUs,
    this.hostSent = 0,
    this.hostDropped = 0,
    this.hostQueuePackets = 0,
    this.hostQueueSumUs = 0,
    this.hostQueueMaxUs,
  });

  /// Null while no sound plays (priming, or the VM is silent).
  final double? depthMs, latencyMs;
  final double? targetMs;
  final int underruns, overruns, insertedFrames, trimmedFrames;

  /// Opus frames concealed; null when the engine does not count them (PCM).
  final int? concealed;
  final double avOffsetSumMs;
  final int avOffsetSamples;
  final int hostCapturePackets, hostSent, hostDropped, hostQueuePackets;
  final double hostCaptureSumUs, hostQueueSumUs;
  final double? hostCaptureMaxUs, hostQueueMaxUs;
}

double? _number(Object? value, double maximum) =>
    value is num && value.isFinite && value >= 0 && value <= maximum
    ? value.toDouble()
    : null;

int? _count(Object? value) =>
    value is int && value >= 0 && value <= 0xffffffff ? value : null;

/// A media engine whose native audio counters outlive one session: Android's
/// AndroidAudioTrim counts for the whole app (AndroidMedia.kt keeps one). The
/// sessions of such an engine share [audioCounterBaselines], so a new session
/// reports what happened since the previous session's last reading instead of
/// the app's totals again (fleet triage 2026-11-03: the first window of every
/// phone session re-sent them, 156 of preview.58's 195 underruns).
abstract interface class SharedAudioCounterMedia {
  Map<String, num> get audioCounterBaselines;
}

/// Turns cumulative native counters into per-reading changes and collects
/// the Host's audio members between readings (each stream.stats once, however
/// long the session keeps showing it).
class AudioStatsTracker {
  /// [baselines]: the native counters' last readings, shared by the sessions
  /// of a [SharedAudioCounterMedia] engine; otherwise this tracker's own.
  AudioStatsTracker({Map<String, num>? baselines})
    : _last = baselines ?? <String, num>{};
  final Map<String, num> _last;
  int _capturePackets = 0, _sent = 0, _dropped = 0, _queuePackets = 0;
  double _captureSumUs = 0, _queueSumUs = 0;
  double? _captureMaxUs, _queueMaxUs;

  /// A stream.stats of the current stream (already filtered by the session).
  void host(Map<String, dynamic> event) {
    final capture = event['audioCapture'];
    if (capture is Map) {
      final packets = _count(capture['packets']);
      final average = _number(capture['avgUs'], 60000000);
      final maximum = _number(capture['maxUs'], 60000000);
      if (packets != null &&
          packets > 0 &&
          average != null &&
          maximum != null &&
          average <= maximum) {
        _capturePackets += packets;
        _captureSumUs += average * packets;
        if (_captureMaxUs == null || maximum > _captureMaxUs!) {
          _captureMaxUs = maximum;
        }
      }
    }
    final send = event['audioSend'];
    if (send is Map) {
      final sent = _count(send['sent']), dropped = _count(send['dropped']);
      if (sent != null && dropped != null) {
        _sent += sent;
        _dropped += dropped;
      }
      final queued = _count(send['queuePackets']);
      final average = _number(send['queueAvgUs'], 60000000);
      final maximum = _number(send['queueMaxUs'], 60000000);
      if (queued != null &&
          queued > 0 &&
          average != null &&
          maximum != null &&
          average <= maximum) {
        _queuePackets += queued;
        _queueSumUs += average * queued;
        if (_queueMaxUs == null || maximum > _queueMaxUs!) {
          _queueMaxUs = maximum;
        }
      }
    }
  }

  int _delta(Map<String, dynamic> stats, String key) {
    final value = stats[key];
    if (value is! int || value < 0) return 0;
    final last = _last[key]?.toInt();
    _last[key] = value;
    // A restarted engine counts from zero again.
    return last == null || value < last ? value : value - last;
  }

  /// The native engine's statistics of this reading; null when it reports no audio.
  AudioStatsReading? read(Map<String, dynamic> stats) {
    if (stats['audioEnabled'] != true || !stats.containsKey('audioUnderruns')) {
      return null;
    }
    final playing = stats['audioPlaying'] == true;
    // The offset sum restarts with its sample count.
    final lastSamples = _last['audioAvOffsetSamples']?.toInt();
    final lastSum = _last['audioAvOffsetTotalMs']?.toDouble();
    final samples = _delta(stats, 'audioAvOffsetSamples');
    final total = stats['audioAvOffsetTotalMs'];
    final sum = total is num && total.isFinite ? total.toDouble() : null;
    final restarted =
        lastSamples == null || samples == stats['audioAvOffsetSamples'];
    if (sum != null) _last['audioAvOffsetTotalMs'] = sum;
    final offsetSum = sum == null || samples == 0
        ? 0.0
        : restarted || lastSum == null
        ? sum
        : sum - lastSum;
    final reading = AudioStatsReading(
      depthMs: playing ? _number(stats['audioDepthMs'], 60000) : null,
      latencyMs: playing ? _number(stats['audioLatencyMs'], 60000) : null,
      targetMs: _number(stats['audioTargetMs'], 60000),
      underruns: _delta(stats, 'audioUnderruns'),
      overruns: _delta(stats, 'audioOverruns'),
      insertedFrames: _delta(stats, 'audioInsertedFrames'),
      trimmedFrames: _delta(stats, 'audioTrimmedFrames'),
      concealed: stats['audioConcealed'] is int
          ? _delta(stats, 'audioConcealed')
          : null,
      avOffsetSamples: samples,
      avOffsetSumMs: offsetSum,
      hostCapturePackets: _capturePackets,
      hostCaptureSumUs: _captureSumUs,
      hostCaptureMaxUs: _captureMaxUs,
      hostSent: _sent,
      hostDropped: _dropped,
      hostQueuePackets: _queuePackets,
      hostQueueSumUs: _queueSumUs,
      hostQueueMaxUs: _queueMaxUs,
    );
    _capturePackets = _sent = _dropped = _queuePackets = 0;
    _captureSumUs = _queueSumUs = 0;
    _captureMaxUs = _queueMaxUs = null;
    return reading;
  }

  /// A new stream or engine: the Host members collected so far are dropped.
  /// The native baselines stay. Android's AndroidAudioTrim counts for the whole
  /// app, across sessions and picture changes, so forgetting them re-reported
  /// the app's total at every session start and resize (2026-11-03: one phone
  /// run sent 249 overruns for 31 real ones). An engine that really restarted
  /// reports less than its baseline, which [_delta] already takes as a restart.
  void reset() {
    _capturePackets = _sent = _dropped = _queuePackets = 0;
    _captureSumUs = _queueSumUs = 0;
    _captureMaxUs = _queueMaxUs = null;
  }
}

int _bounded(double value) => value.round().clamp(0, 0xffffffff);

/// One reporting window of audio readings.
class AudioStatsWindow {
  int _readings = 0, _playing = 0;
  double _depth = 0, _latency = 0;
  int _latencies = 0;
  double? _target;
  int _underruns = 0, _overruns = 0, _inserted = 0, _trimmed = 0;
  int? _concealed;
  double _avSum = 0;
  int _avSamples = 0;
  int _capturePackets = 0, _sent = 0, _dropped = 0, _queuePackets = 0;
  double _captureSum = 0, _queueSum = 0;
  double? _captureMax, _queueMax;
  bool _host = false;

  void add(AudioStatsReading? reading) {
    if (reading == null) return;
    _readings++;
    if (reading.depthMs != null) {
      _playing++;
      _depth += reading.depthMs!;
    }
    if (reading.latencyMs != null) {
      _latencies++;
      _latency += reading.latencyMs!;
    }
    _target = reading.targetMs ?? _target;
    _underruns += reading.underruns;
    _overruns += reading.overruns;
    _inserted += reading.insertedFrames;
    _trimmed += reading.trimmedFrames;
    if (reading.concealed != null) {
      _concealed = (_concealed ?? 0) + reading.concealed!;
    }
    _avSum += reading.avOffsetSumMs;
    _avSamples += reading.avOffsetSamples;
    _capturePackets += reading.hostCapturePackets;
    _captureSum += reading.hostCaptureSumUs;
    _sent += reading.hostSent;
    _dropped += reading.hostDropped;
    _queuePackets += reading.hostQueuePackets;
    _queueSum += reading.hostQueueSumUs;
    _host =
        _host ||
        reading.hostSent > 0 ||
        reading.hostDropped > 0 ||
        reading.hostCapturePackets > 0;
    if (reading.hostCaptureMaxUs != null &&
        (_captureMax == null || reading.hostCaptureMaxUs! > _captureMax!)) {
      _captureMax = reading.hostCaptureMaxUs;
    }
    if (reading.hostQueueMaxUs != null &&
        (_queueMax == null || reading.hostQueueMaxUs! > _queueMax!)) {
      _queueMax = reading.hostQueueMaxUs;
    }
  }

  /// The window's fields (absent without readings), then a fresh window.
  Map<String, int> take() {
    if (_readings == 0) return const {};
    final average = _avSamples == 0 ? null : _avSum / _avSamples;
    final values = <String, int>{
      if (_playing > 0) 'audioDepthMs': _bounded(_depth / _playing),
      if (_latencies > 0) 'audioLatencyMs': _bounded(_latency / _latencies),
      if (_target != null) 'audioTargetMs': _bounded(_target!),
      'audioUnderruns': _underruns,
      'audioOverruns': _overruns,
      'audioInserted': _inserted,
      'audioTrimmed': _trimmed,
      'audioConcealed': ?_concealed,
      if (average != null && average >= 0) 'audioAvLagMs': _bounded(average),
      if (average != null && average < 0) 'audioAvLeadMs': _bounded(-average),
      if (_capturePackets > 0)
        'hostAudioCaptureUs': _bounded(_captureSum / _capturePackets),
      if (_captureMax != null) 'hostAudioCaptureMaxUs': _bounded(_captureMax!),
      if (_host) 'hostAudioSent': _sent,
      if (_host) 'hostAudioDropped': _dropped,
      if (_queuePackets > 0)
        'hostAudioQueueUs': _bounded(_queueSum / _queuePackets),
      if (_queueMax != null) 'hostAudioQueueMaxUs': _bounded(_queueMax!),
    };
    _readings = _playing = _latencies = 0;
    _depth = _latency = 0;
    _target = null;
    _underruns = _overruns = _inserted = _trimmed = 0;
    _concealed = null;
    _avSum = 0;
    _avSamples = 0;
    _capturePackets = _sent = _dropped = _queuePackets = 0;
    _captureSum = _queueSum = 0;
    _captureMax = _queueMax = null;
    _host = false;
    return values;
  }
}

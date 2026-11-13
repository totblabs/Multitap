/// The 연결 통계 dialog's "이 기기 처리" rows, from the Android engine's `stats`
/// answer (AndroidMedia.kt): how the decoder was configured, how long a frame
/// takes from its arrival to the screen hand-over, what was skipped, the
/// audio buffer, the display rate, the Wi-Fi lock and the device's heat.
///
/// Only carries what the engine measured: fixed numbers and codes, no
/// content, no device identity. A value the engine did not report (an older
/// engine, an Android version without the API) reads '미측정'.
library;

import 'android_sharp_picture.dart' show androidSharpStatsLabel;
import '../l10n/l10n.dart';

/// Shown under the section.
String get androidEngineMetricsNote =>
    l10n.timesAverageMaximumOverLast;

String get _unmeasured => l10n.notMeasured;

num? _number(Object? value, [num max = 1e9]) =>
    value is num && value.isFinite && value >= 0 && value <= max ? value : null;

String _ms(num value) =>
    '${value >= 100 ? value.round() : value.toStringAsFixed(1)} ms';

/// "평균 4.2 ms · 최대 9.1 ms" of one RecentWindow pair.
String _recent(Object? mean, Object? max) {
  final average = _number(mean, 60000), highest = _number(max, 60000);
  if (average == null) return _unmeasured;
  return highest == null
      ? l10n.avg(_ms(average))
      : l10n.avgMax(_ms(average), _ms(highest));
}

/// AvcVendorKeys codes (AndroidMediaPacing.kt).
const _vendors = <int, String>{
  1: 'Qualcomm',
  2: 'Exynos',
  3: 'Kirin',
  4: 'Amlogic',
  5: 'MediaTek',
};

/// What the H.264 decoder accepted (avcTuningFlags bits: 1 KEY_LOW_LATENCY,
/// 2 real-time priority, 4 operating rate, 8 vendor keys).
String androidDecoderTuningLabel(
  Object? flags,
  Object? vendorKeys,
  Object? plainFallback,
) {
  if (flags is! int || flags < 0 || flags > 31) return l10n.preparing;
  if (plainFallback == true) return l10n.defaultFallbackAfterError;
  final parts = <String>[
    if (flags & 1 != 0) l10n.androidLowLatency,
    if (flags & 2 != 0) l10n.realTimePriority,
    if (flags & 8 != 0) l10n.lowLatencyKeys(_vendors[vendorKeys] ?? l10n.vendor),
  ];
  final on = flags & (1 | 8) != 0;
  if (parts.isEmpty) return l10n.offDefault;
  return '${on ? l10n.onLabel : l10n.off} (${parts.join(' · ')})';
}

/// PowerManager THERMAL_STATUS_* (0 none .. 6 shutdown).
Map<int, String> get _thermal => {
  0: l10n.normal,
  1: l10n.slightlyWarm,
  2: l10n.warm,
  3: l10n.hot,
  4: l10n.veryHot,
  5: l10n.critical,
  6: l10n.aboutShutDown,
};

String androidThermalLabel(Object? status, Object? highest) {
  final now = _thermal[status];
  if (now == null) return _unmeasured;
  final top = _thermal[highest];
  return top == null || highest == status ? now : l10n.highestConnection(now, top);
}

/// AndroidStreamWifi's `wifiLock` (0 not held, 1 low latency, 2 high
/// performance) and `wifiLockRefused`. Held means asked of Android: it applies
/// only on Wi-Fi, with the screen on.
String androidWifiLockLabel(Object? lock, Object? refused) => switch (lock) {
  1 => l10n.requested,
  2 => l10n.requestedHighPerformanceMode,
  0 => refused is int && refused > 0 ? l10n.refusedByDevice : l10n.notRequested,
  _ => _unmeasured,
};

/// The 연결 통계 dialog's "오디오" rows: the facts that decide a "no sound"
/// report, on the phone itself. [hostPerSecond]: audio packets per second the
/// Host reported sending lately (RemoteSession.hostAudioPerSecond, null before
/// its stream.stats said anything); the rest from the engine's `stats`
/// (AndroidMedia.kt): which decoder plays, audio focus and whether it mutes,
/// the device's media volume, and this stream's underruns and audio thread
/// errors. Empty with sound off.
List<(String, String)> androidAudioMetrics(
  Map<String, dynamic>? stats, {
  double? hostPerSecond,
}) {
  if (stats == null || stats['audioEnabled'] != true) return const [];
  int? count(Object? value) =>
      value is int && value >= 0 && value <= 0xffffffff ? value : null;
  final host = hostPerSecond == null || !hostPerSecond.isFinite
      ? _unmeasured
      : hostPerSecond.round() <= 0
      ? l10n.audioNoneSent
      : l10n.audioPacketsPerSecond(hostPerSecond.round());
  final decoder = switch (stats['audioDecoder']) {
    1 => l10n.audioOpusBuiltIn,
    2 => 'Opus',
    3 =>
      stats['audioOpusUnusable'] == true
          ? l10n.audioDecoderPcmNext
          : l10n.audioDecoderRetrying,
    0 => stats['audioOpus'] == true ? l10n.preparing : 'PCM',
    _ => _unmeasured,
  };
  final focus = switch (stats['audioFocus']) {
    0 => l10n.none,
    1 => l10n.audioFocusHeld,
    2 => l10n.audioFocusWaiting,
    3 => l10n.audioFocusPaused,
    4 => l10n.audioFocusLost,
    _ => _unmeasured,
  };
  final volume = count(stats['mediaVolume']), top = count(stats['mediaVolumeMax']);
  final level = volume == null || top == null || top == 0 || volume > top
      ? _unmeasured
      : volume == 0
      ? l10n.deviceVolumeSilent('0/$top')
      : '$volume/$top';
  final underruns = count(stats['audioStreamUnderruns']);
  final errors = count(stats['audioThreadErrors']) ?? 0;
  var playback = underruns == null
      ? _unmeasured
      : stats['audioPlaying'] == true
      ? l10n.audioPlayingUnderruns(underruns)
      : l10n.audioSilentUnderruns(underruns);
  if (underruns != null && errors > 0) {
    playback = l10n.audioThreadErrorsSuffix(playback, errors);
  }
  return [
    (l10n.audioFromHost, host),
    (l10n.audioDecoder, decoder),
    (l10n.audioFocus, focus),
    (l10n.deviceVolume, level),
    (l10n.audioPlayback, playback),
  ];
}

List<(String, String)> androidEngineMetrics(Map<String, dynamic>? stats) {
  if (stats == null) return const [];
  String count(Object? value) => value is int && value >= 0 ? '$value' : '—';
  final fallbacks = stats['decoderFallbacks'];
  final queued = _number(stats['audioDepthMs'], 60000),
      target = _number(stats['audioTargetMs'], 60000),
      buffer = _number(stats['audioBufferMs'], 60000),
      output = _number(stats['audioLatencyMs'], 60000);
  final hz = stats['displayHz'];
  return [
    (
      l10n.decoderLowLatency,
      androidDecoderTuningLabel(
        stats['decoderLowLatency'],
        stats['decoderVendorKeys'],
        stats['decoderPlainFallback'],
      ),
    ),
    (
      l10n.decoderFallback,
      fallbacks is int && fallbacks >= 0
          ? (fallbacks == 0 ? l10n.none : l10n.text(fallbacks))
          : _unmeasured,
    ),
    (l10n.decodeTime, _recent(stats['decodeRecentMs'], stats['decodeRecentMaxMs'])),
    (l10n.receivedDecoded, _recent(stats['frameArrivalMs'], stats['frameArrivalMaxMs'])),
    (
      l10n.decodedSentScreen,
      _recent(stats['presentWaitMs'], stats['presentWaitMaxMs']),
    ),
    (
      l10n.framesReplacedWaitingDropped,
      '${count(stats['skippedDecodedFrames'])} · ${count(stats['textureQueued'])} · ${count(stats['droppedPackets'])}',
    ),
    (
      l10n.audioBuffer,
      stats['audioEnabled'] != true || buffer == null
          ? _unmeasured
          : l10n.queuedTargetBuffer(queued == null ? '—' : _ms(queued), target == null ? '—' : _ms(target), _ms(buffer)),
    ),
    (
      l10n.audioOutput,
      switch (stats['audioFastTrack']) {
        true => output == null ? l10n.lowLatencyPath : l10n.lowLatencyPathLatency(_ms(output)),
        false => output == null ? l10n.normalPath : l10n.normalPathLatency(_ms(output)),
        _ => _unmeasured,
      },
    ),
    (l10n.displayRefreshRate, hz is int && hz > 0 && hz <= 1000 ? '$hz Hz' : _unmeasured),
    // 화면 › 해상도 › 선명하게: what was planned, and how often the automatic
    // size was used instead (the decoder's limits here, or the desktop's).
    (l10n.sharp, androidSharpStatsLabel(stats)),
    (
      l10n.wiFiLowLatency,
      androidWifiLockLabel(stats['wifiLock'], stats['wifiLockRefused']),
    ),
    (
      l10n.deviceHeat,
      androidThermalLabel(stats['thermalStatus'], stats['thermalStatusMax']),
    ),
  ];
}

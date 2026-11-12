import 'dart:math' as math;

import '../data/video_geometry.dart';
import '../l10n/l10n.dart';

/// Said wherever a desktop has no sound device: the Host can only send the
/// sound Windows plays on a device. Multitap does not add one: the
/// administrator adds an audio device in Proxmox.
String get audioDeviceMissingText =>
    l10n.desktopHasNoSoundDevice2;

/// The desktop tile's 다시 시작 필요 badge: a sound device was added in
/// Proxmox and takes effect on the desktop's next restart.
String get audioDevicePendingText => l10n.restartDesktopUseSoundDevice;

/// How the streaming bitrate is chosen. `auto` (bitrate-adaptive-v1) lets the
/// host follow what the relay path carries; `fixed` is the released behaviour.
enum StreamBitrateMode { fixed, auto }

class StreamOptions {
  // Product name; existing stream/media-epoch-v1 negotiation stays compatible.
  static const engineName = 'Multitap';

  /// Released brokers and hosts accept a fixed 4-20 Mbps only.
  static const minimumBitrate = 4000000,
      legacyMaximumBitrate = 20000000,
      adaptiveMaximumBitrate = 100000000;

  /// New sessions start at 60 fps with automatic bitrate: only changed
  /// frames are sent, and a GPU-less VM streams one 1920x1080 output at 60.
  /// Explicit choices keep their requested FPS.
  /// [negotiated] uses fixed 20 Mbps where automatic bitrate is unsupported.
  static const recommended = StreamOptions(
    fps: 60,
    bitrateMode: StreamBitrateMode.auto,
  );

  /// The encoder box is a codec safety ceiling, not a quality cap: a viewport
  /// stream is encoded at its own size and the Host scales only above its
  /// pixel budget (stream_encode_size.hpp). A 1920x1080 box would encode a
  /// 2960x760 window as 1920x492 and the picture would be upscaled, so it
  /// would look blurry. Same ceiling as the settings panel.
  static const codecMaxWidth = 3840, codecMaxHeight = 2160;

  const StreamOptions({
    this.maxWidth = codecMaxWidth,
    this.maxHeight = codecMaxHeight,
    this.fps = 60,
    // 20 Mbps (the released maximum): unchanged frames are not re-sent, so a
    // still desktop costs little while text stays sharp.
    // With the automatic mode this is the manual value kept for later.
    this.bitrate = 20000000,
    // Every connection the app makes asks for sound (no switch on the PC or
    // Android). False only in tests.
    this.audio = true,
    this.monitor = '0',
    this.bitrateMode = StreamBitrateMode.fixed,
  });
  final int maxWidth, maxHeight, fps, bitrate;
  final bool audio;
  final String monitor;
  final StreamBitrateMode bitrateMode;

  bool get automaticBitrate => bitrateMode == StreamBitrateMode.auto;

  StreamOptions _with({
    int? bitrate,
    StreamBitrateMode? mode,
    String? monitor,
  }) => StreamOptions(
    maxWidth: maxWidth,
    maxHeight: maxHeight,
    fps: fps,
    bitrate: bitrate ?? this.bitrate,
    audio: audio,
    monitor: monitor ?? this.monitor,
    bitrateMode: mode ?? bitrateMode,
  );

  /// The request this desktop's broker and host accept. [maximumBitrate] is
  /// the advertised bitrate-adaptive-v1 ceiling, or null for released peers.
  /// Automatic: the ceiling where supported (independent outputs stay fixed),
  /// otherwise the released fixed 20 Mbps. Manual: bounded by the ceiling.
  StreamOptions negotiated(
    int? maximumBitrate, {
    bool independentOutputs = false,
  }) {
    final supported =
        maximumBitrate != null && maximumBitrate >= legacyMaximumBitrate;
    final ceiling = supported
        ? math.min(maximumBitrate, adaptiveMaximumBitrate)
        : legacyMaximumBitrate;
    if (automaticBitrate && supported && !independentOutputs) {
      return _with(bitrate: ceiling);
    }
    return _with(
      bitrate: automaticBitrate
          ? legacyMaximumBitrate
          : bitrate.clamp(minimumBitrate, ceiling),
      mode: StreamBitrateMode.fixed,
    );
  }

  /// A window or one-monitor fullscreen session owns exactly output zero.
  /// Quality preferences never carry a previous dual/output-one selection into
  /// a new single-display connection.
  StreamOptions forSingleDisplay() =>
      monitor == '0' ? this : _with(monitor: '0');

  /// The same choices with the codec ceiling as the encoder box. Leaving
  /// 모든 모니터 needs it: that layout's box is one output's size (e.g.
  /// 1920x1080), which a single-output window would otherwise keep as a cap.
  StreamOptions withCodecCeiling() => withBox(codecMaxWidth, codecMaxHeight);

  /// The same choices with another encoder box. stream-size-v1 connections
  /// send two per-side ceilings of 3840 (the decoder limits carry the real
  /// ceiling and a portrait output may be 3840 rows tall).
  StreamOptions withBox(int width, int height) =>
      maxWidth == width && maxHeight == height
      ? this
      : StreamOptions(
          maxWidth: width,
          maxHeight: height,
          fps: fps,
          bitrate: bitrate,
          audio: audio,
          monitor: monitor,
          bitrateMode: bitrateMode,
        );

  StreamOptions forMonitors(List<RemoteMonitor> monitors) {
    final ids = monitors
        .map((m) => m.id)
        .where((id) => id == '0' || id == '1')
        .toSet();
    if (ids.isEmpty ||
        ids.contains(monitor) ||
        (monitor == 'all' && ids.length == 2)) {
      return this;
    }
    return _with(monitor: ids.contains('0') ? '0' : '1');
  }

  /// Wire form. Fixed options keep the released shape (no `bitrateMode`);
  /// send only [negotiated] options to a broker. A box taller than 2160 rows
  /// (two per-side ceilings up to 3840) goes only to a stream-size-v1 request
  /// (HttpBrokerApi.connect); released brokers keep 3840x2160.
  Map<String, Object> toJson() {
    if (maxWidth < 16 ||
        maxWidth > maximumGeometrySide ||
        maxWidth.isOdd ||
        maxHeight < 16 ||
        maxHeight > maximumGeometrySide ||
        maxHeight.isOdd ||
        ![30, 60].contains(fps) ||
        bitrate < minimumBitrate ||
        bitrate > adaptiveMaximumBitrate ||
        !['0', '1', 'all'].contains(monitor)) {
      throw ArgumentError('Invalid streaming options.');
    }
    return {
      'maxWidth': maxWidth,
      'maxHeight': maxHeight,
      'fps': fps,
      'bitrate': bitrate,
      'audio': audio,
      'monitor': monitor,
      if (automaticBitrate) 'bitrateMode': 'auto',
    };
  }

  factory StreamOptions.fromJson(Map<String, dynamic> data) {
    final mode = data['bitrateMode'];
    if (mode != null && mode != 'auto' && mode != 'fixed') {
      throw ArgumentError('Invalid streaming options.');
    }
    final result = StreamOptions(
      maxWidth: data['maxWidth'] as int,
      maxHeight: data['maxHeight'] as int,
      fps: data['fps'] as int,
      bitrate: data['bitrate'] as int,
      // The wire value as it is: the broker's echo of a request must match
      // what was asked. The app's
      // own choices always ask for sound (StreamSettingsBody, Android
      // AndroidStreamQuality), and the Windows client stores no choice.
      audio: data['audio'] as bool,
      monitor: data['monitor'] as String? ?? '0',
      bitrateMode: mode == 'auto'
          ? StreamBitrateMode.auto
          : StreamBitrateMode.fixed,
    );
    result.toJson();
    return result;
  }
}

/// Quality modes (backlog #99). A mode sets only the frame rate and the
/// bitrate: sound, decoding and 마우스 잠금 stay as they are. No 게임 mode
/// yet: the broker and Host take 30 or 60 fps only, every connection already
/// prefers UDP and the Host always encodes for low latency, so it would ask
/// for exactly what 작업 asks for.
enum StreamPreset {
  /// The new-connection default.
  work(StreamOptions.recommended),

  /// Mobile data and weak PCs: half the frames at a fixed 8 Mbps, one of
  /// Android's bitrate choices; fixed, so a released Host gets the same.
  saver(StreamOptions(fps: 30, bitrate: 8000000));

  const StreamPreset(this.options);
  String get label => switch (this) {
    StreamPreset.work => l10n.work,
    StreamPreset.saver => l10n.saver,
  };

  String get detail => switch (this) {
    StreamPreset.work => l10n.n60FpsAutoBitrate,
    StreamPreset.saver => l10n.n30Fps8MbpsUses,
  };

  /// Its frame rate and bitrate choice (the rest is not used).
  final StreamOptions options;

  /// The mode [choices] ask for on a desktop whose bitrate-adaptive-v1
  /// ceiling is [maximumBitrate] (null: released), or null (사용자 설정).
  /// Compared as negotiated, so 작업 still reads 작업 where it becomes a
  /// fixed 20 Mbps (a released Host, or independent outputs).
  static StreamPreset? of(StreamOptions choices, int? maximumBitrate) {
    final independent = choices.monitor == 'all';
    StreamOptions sent(StreamOptions value) =>
        value.negotiated(maximumBitrate, independentOutputs: independent);
    final asked = sent(choices);
    for (final preset in values) {
      final mode = sent(preset.options);
      if (mode.fps == asked.fps &&
          mode.bitrateMode == asked.bitrateMode &&
          mode.bitrate == asked.bitrate) {
        return preset;
      }
    }
    return null;
  }
}

class RemoteMonitor {
  const RemoteMonitor({
    required this.id,
    required this.width,
    required this.height,
    this.primary = false,
  });
  final String id;
  final int width, height;
  final bool primary;
}

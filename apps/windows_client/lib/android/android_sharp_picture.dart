/// 화면 › 해상도 › 선명하게 (Android): the automatic size is this screen's
/// logical size, which the panel scales up, so text is soft. This choice asks
/// for (up to) the panel's own pixels and raises Windows' scale by the same
/// factor, so everything keeps its size and only gets sharper. The automatic
/// size stays the default; this is remembered per kind of screen like the
/// other 해상도 choices.
///
/// The size is decided by the engine from what the device says about itself
/// (SharpPicture.kt: panel pixels, the automatic size, what its H.264 decoder
/// takes at the frame rate, memory, the connection's ceiling), never from a
/// device name. What cannot be had falls back to the automatic size and is
/// counted in the statistics ("이 기기 처리").
library;

import 'package:flutter/foundation.dart' show immutable;
import 'package:flutter/services.dart';

import '../data/stream_size.dart' show streamMaximumPixels, streamMaximumSide;
import '../l10n/l10n.dart';

/// What the person reads beside the choice (one line).
String get androidSharpLabel => l10n.sharp;
String get androidSharpDetail => l10n.usesMoreDataBattery;
String get androidSharpHint => l10n.getsScreensExactPixelsSo;

/// What the engine planned for one picture area.
@immutable
class AndroidSharpPlan {
  const AndroidSharpPlan({
    required this.width,
    required this.height,
    required this.baseWidth,
    required this.baseHeight,
    required this.scale,
    this.reason,
    this.fallback = false,
  });

  /// The size to ask for; the automatic size it was planned from.
  final int width, height, baseWidth, baseHeight;

  /// The Windows scale (percent) that keeps things the size they have at the
  /// automatic size and 100 %.
  final int scale;

  /// Why it is less than the panel's own pixels (a fixed code), or null.
  final String? reason;

  /// Nothing sharper than the automatic size could be had.
  final bool fallback;

  Size get size => Size(width.toDouble(), height.toDouble());

  /// Panel pixels per automatic pixel that this plan asks for.
  double get factor => baseWidth > 0 ? width / baseWidth : 1;

  /// Sharper than the automatic size.
  bool get gains => !fallback && width > baseWidth && height > baseHeight;

  static AndroidSharpPlan? parse(Object? value, Size base) {
    if (value is! Map) return null;
    final width = value['width'], height = value['height'];
    final scale = value['scale'], reason = value['reason'];
    final baseWidth = base.width.round(), baseHeight = base.height.round();
    if (width is! int ||
        height is! int ||
        scale is! int ||
        width < baseWidth ||
        height < baseHeight ||
        width.isOdd ||
        height.isOdd ||
        width > 32768 ||
        height > 32768 ||
        !const [100, 125, 150, 175, 200].contains(scale) ||
        (reason != null &&
            (reason is! String ||
                !RegExp(r'^[a-z_]{1,32}$').hasMatch(reason)))) {
      return null;
    }
    return AndroidSharpPlan(
      width: width,
      height: height,
      baseWidth: baseWidth,
      baseHeight: baseHeight,
      scale: scale,
      reason: reason as String?,
      fallback: value['fallback'] == true,
    );
  }
}

/// The engine's side of 선명하게 (AndroidMedia.kt, channel `multitap/remote`):
///   sharpPicture {panelWidth, panelHeight, baseWidth, baseHeight, fps,
///                 hardware, limitWidth, limitHeight, limitPixels}
///                             -> {width, height, scale, reason, fallback}
///   sharpPictureFallback {reason}   the Host did not give the planned picture
///   sharpPictureOff                 the choice is not in use
abstract interface class AndroidSharpPicture {
  /// The plan for a picture area of [panel] pixels whose automatic size is
  /// [base]; null when the engine cannot say (an older engine, a failed
  /// query), which is the automatic size.
  Future<AndroidSharpPlan?> plan({
    required Size panel,
    required Size base,
    required int fps,
    required bool portrait,
  });

  /// The Host refused the planned size (`host_refused`) or encodes less than
  /// it (`host_reduced`): counted with the engine's own fallbacks.
  void hostFallback(String reason);

  /// The choice is off (the statistics say so).
  void off();
}

class NativeAndroidSharpPicture implements AndroidSharpPicture {
  const NativeAndroidSharpPicture([
    this._channel = const MethodChannel('multitap/remote'),
  ]);
  final MethodChannel _channel;

  @override
  Future<AndroidSharpPlan?> plan({
    required Size panel,
    required Size base,
    required int fps,
    required bool portrait,
  }) async {
    if (panel.isEmpty || base.isEmpty || !panel.isFinite || !base.isFinite) {
      return null;
    }
    try {
      final value = await _channel
          .invokeMethod<Object?>('sharpPicture', {
            'panelWidth': panel.width.floor(),
            'panelHeight': panel.height.floor(),
            'baseWidth': base.width.round(),
            'baseHeight': base.height.round(),
            'fps': fps,
            'hardware': true,
            // An encoded picture's ceiling on this connection: the longer
            // side at most 3840 and one 4K frame's pixels; without
            // stream-size the released 3840x2160.
            'limitWidth': streamMaximumSide,
            'limitHeight': portrait ? streamMaximumSide : 2160,
            'limitPixels': streamMaximumPixels,
          })
          .timeout(const Duration(seconds: 3));
      return AndroidSharpPlan.parse(value, base);
    } catch (_) {
      return null;
    }
  }

  @override
  void hostFallback(String reason) {
    _channel
        .invokeMethod<void>('sharpPictureFallback', {'reason': reason})
        .catchError((Object _) {});
  }

  @override
  void off() {
    _channel.invokeMethod<void>('sharpPictureOff').catchError((Object _) {});
  }
}

/// Why 선명하게 gives less than the panel's own pixels, in a few words.
String androidSharpReasonLabel(Object? reason) => switch (reason) {
  'scale_limit' => l10n.windowsScale200Limit,
  'host_limit' || 'host_refused' || 'host_reduced' => l10n.desktopLimit,
  'decoder_size' || 'decoder_profile' => l10n.decoderSizeLimitDevice,
  'decoder_rate' => l10n.decoderSpeedLimitDevice,
  'memory_budget' => l10n.memoryDevice,
  'no_gain' => l10n.autoSizeAlreadyNativePixels,
  _ => l10n.couldntCheck,
};

/// The 연결 통계 row "선명하게" from the engine's `stats` answer.
String androidSharpStatsLabel(Map<String, dynamic>? stats) {
  if (stats == null || stats['sharpRequested'] is! bool) return l10n.notMeasured;
  final fallbacks = stats['sharpFallbacks'];
  final count = fallbacks is int && fallbacks > 0 ? fallbacks : 0;
  final fell = count == 0
      ? ''
      : l10n.backAutoSize(count, androidSharpReasonLabel(stats['sharpFallbackReason']));
  if (stats['sharpRequested'] != true) return l10n.off3(fell);
  final width = stats['sharpWidth'], height = stats['sharpHeight'];
  final reason = stats['sharpReason'];
  final size = width is int && height is int && width > 0 && height > 0
      ? '$width×$height'
      : '';
  final state = reason == null
      ? l10n.nativeDevicePixels(size.isEmpty ? '' : ' $size')
      : '${size.isEmpty ? l10n.lowered : size} (${androidSharpReasonLabel(reason)})';
  return l10n.onLabel3(state, fell);
}

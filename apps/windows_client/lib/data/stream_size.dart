/// stream-size-v1 (docs/remote-session-protocol.md "Stream sizes"): a viewport
/// stream is encoded at its source's own size. The Host reduces it only for
/// this client decoder's declared limits, its measured decode load (the
/// `stream.decoder` reports this client sends), the Host's own measured
/// encoder load or an administrator cap; the remote output may be portrait up
/// to 2160x3840 (longer side at most 3840, at most 8,294,400 pixels). Only a
/// connection whose broker granted the protocol (a Host that advertised it)
/// carries any of it; released peers keep today's 3840x2160 rules.
library;

import 'dart:math' as math;

import 'package:flutter/services.dart';

import 'video_geometry.dart';
import '../l10n/l10n.dart';

const streamSizeProtocol = 'stream-size-v1';
const streamSizeProtocolV2 = 'stream-size-v2';
const streamMaximumSide = 3840, streamMaximumPixels = 8294400;

/// The released single-output bound (3840x2160).
const releasedMaximumHeight = 2160;

/// An owned remote output size: even sides, at least 320x180, the longer side
/// at most 3840; [portrait] (granted stream-size-v1) allows up to 3840 rows.
bool ownedOutputSize(int width, int height, {bool portrait = false}) =>
    videoGeometry(width, height, minimumWidth: 320, minimumHeight: 180);

bool legacyOwnedOutputSize(int width, int height, {bool portrait = false}) =>
    width >= 320 &&
    height >= 180 &&
    width <= streamMaximumSide &&
    height <= (portrait ? streamMaximumSide : releasedMaximumHeight) &&
    width.isEven &&
    height.isEven &&
    width * height <= streamMaximumPixels;

/// An encoded picture size (stream.info width/height).
bool encodedPictureSize(int width, int height, {bool portrait = false}) =>
    videoGeometry(width, height);

/// The Host advertised stream-size-v1 (with the separate-v1 viewport encoder
/// ceilings it extends). A released broker drops the field.
bool streamSizeAdvertised(Object? capabilities) =>
    capabilities is Map &&
    ((capabilities['streamSizeProtocol'] == streamSizeProtocol &&
            capabilities['viewportEncoderLimitsProtocol'] == 'separate-v1') ||
        streamSizeV2Advertised(capabilities));

bool streamSizeV2Advertised(Object? capabilities) =>
    capabilities is Map &&
    (capabilities['streamSizeProtocol'] == streamSizeProtocolV2 ||
        (capabilities['streamSizeProtocols'] is List &&
            (capabilities['streamSizeProtocols'] as List).isNotEmpty &&
            (capabilities['streamSizeProtocols'] as List).length <= 2 &&
            (capabilities['streamSizeProtocols'] as List).toSet().length ==
                (capabilities['streamSizeProtocols'] as List).length &&
            (capabilities['streamSizeProtocols'] as List).every(
              (p) => p == streamSizeProtocol || p == streamSizeProtocolV2,
            ) &&
            (capabilities['streamSizeProtocols'] as List).contains(
              streamSizeProtocolV2,
            )));

int _macroblocks(int width, int height) =>
    ((width + 15) ~/ 16) * ((height + 15) ~/ 16);

/// What this client's decoder takes: sides, pixels per frame and pixels per
/// second. [maxPixelRate] 0: not declared (the Windows software decoder,
/// whose throughput is measured live instead).
class DecoderLimits {
  const DecoderLimits({
    required this.maxWidth,
    required this.maxHeight,
    required this.maxPixels,
    required this.maxPixelRate,
  });

  /// Unknown decoder: today's 3840x2160 and 4,194,304 pixels at 60 fps.
  static const safeDefault = DecoderLimits(
    maxWidth: 3840,
    maxHeight: releasedMaximumHeight,
    maxPixels: streamMaximumPixels,
    maxPixelRate: 4194304 * 60,
  );

  final int maxWidth, maxHeight, maxPixels, maxPixelRate;
  DecoderLimits get legacy => DecoderLimits(
    maxWidth: math.min(maxWidth, streamMaximumSide),
    maxHeight: math.min(maxHeight, streamMaximumSide),
    maxPixels: math.min(maxPixels, streamMaximumPixels),
    maxPixelRate: math.min(maxPixelRate, streamMaximumPixels * 60),
  );

  /// The wire bounds (stream-size.mjs validDecoderLimits).
  bool get valid =>
      maxWidth >= 16 &&
      maxWidth <= maximumGeometrySide &&
      maxHeight >= 16 &&
      maxHeight <= maximumGeometrySide &&
      maxPixels >= 256 &&
      maxPixels <= maximumWireInteger &&
      (maxPixelRate == 0 ||
          (maxPixelRate >= 256 && maxPixelRate <= maximumWireInteger));

  /// A native platform report ({maxWidth, maxHeight, maxPixels,
  /// maxPixelRate}), clamped to the protocol's bounds; null when unusable.
  static DecoderLimits? fromNative(Object? value) {
    if (value is! Map) return null;
    int? read(String key) {
      final number = value[key];
      return number is int ? number : null;
    }

    final width = read('maxWidth'), height = read('maxHeight');
    final pixels = read('maxPixels'), rate = read('maxPixelRate');
    if (width == null || height == null || pixels == null || rate == null) {
      return null;
    }
    final limits = DecoderLimits(
      maxWidth: width,
      maxHeight: height,
      maxPixels: pixels,
      maxPixelRate: rate,
    );
    return limits.valid ? limits : null;
  }

  Map<String, int> toJson() => {
    'maxWidth': maxWidth,
    'maxHeight': maxHeight,
    'maxPixels': maxPixels,
    'maxPixelRate': maxPixelRate,
  };

  /// A [width]x[height] picture at [fps] within these limits (whole
  /// macroblocks, as decoders and the broker count them).
  bool fits(int width, int height, int fps) {
    final pixels = _macroblocks(width, height) * 256;
    return width <= maxWidth &&
        height <= maxHeight &&
        pixels <= maxPixels &&
        (maxPixelRate == 0 || pixels * fps <= maxPixelRate);
  }

  @override
  bool operator ==(Object other) =>
      other is DecoderLimits &&
      other.maxWidth == maxWidth &&
      other.maxHeight == maxHeight &&
      other.maxPixels == maxPixels &&
      other.maxPixelRate == maxPixelRate;

  @override
  int get hashCode => Object.hash(maxWidth, maxHeight, maxPixels, maxPixelRate);
}

/// Why the Host's picture is smaller than its source (stream.info/stats
/// `encodeLimit`); the numeric code is the diagnostics value.
enum EncodeLimit {
  none('none', 0),
  decoderSize('decoder_size', 1),
  decoderRate('decoder_rate', 2),
  decoderLoad('decoder_load', 3),
  hostCpu('host_cpu', 4),
  adminCap('admin_cap', 5);

  const EncodeLimit(this.wire, this.code);
  final String wire;
  final int code;

  static EncodeLimit? parse(Object? value) {
    for (final limit in values) {
      if (limit.wire == value) return limit;
    }
    return null;
  }

  /// Short Korean reason for the statistics panel.
  String get label => switch (this) {
    none => l10n.originalSize,
    decoderSize => l10n.pcDecoderSizeLimit,
    decoderRate => l10n.pcDecoderSpeedLimit,
    decoderLoad => l10n.pcDecodingLoad,
    hostCpu => l10n.desktopEncodingLoad,
    adminCap => l10n.limitSetByAdministrator,
  };
}

/// One decoder report (`{"type":"stream.decoder","busy":..,"backlogMs":..}`):
/// the share of the last [elapsedMs] the decoder was busy (per mille, -1
/// when not measured) and the age of the oldest packet still waiting.
Map<String, Object> decoderReport({
  required double? decodeMs,
  required double elapsedMs,
  required double? backlogMs,
}) {
  final busy = decodeMs == null || !decodeMs.isFinite || elapsedMs <= 0
      ? -1
      : (decodeMs / elapsedMs * 1000).round().clamp(0, 1000);
  final backlog = backlogMs == null || !backlogMs.isFinite
      ? 0
      : backlogMs.round().clamp(0, 60000);
  return {'type': 'stream.decoder', 'busy': busy, 'backlogMs': backlog};
}

Future<DecoderLimits?>? _nativeLimits;

/// This device's decoder limits from its native media engine (once per
/// process): Windows, the Microsoft H.264 decoder the client uses; Android,
/// the MediaCodec AVC decoder it creates. Null when the engine cannot say
/// (no engine, an older engine, a failed query): the connect request then
/// declares [DecoderLimits.safeDefault].
Future<DecoderLimits?> nativeDecoderLimits() => _nativeLimits ??= () async {
  try {
    final value = await const MethodChannel('multitap/remote')
        .invokeMethod<Object?>('decoderLimits')
        .timeout(const Duration(seconds: 3));
    if (value is Map && value['exactValidation'] != null) return null;
    return DecoderLimits.fromNative(value);
  } catch (_) {
    return null;
  }
}();

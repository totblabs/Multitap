import 'dart:math' as math;

import 'package:flutter/services.dart';

import 'decode_settings.dart';
import 'stream_size.dart';
import 'video_geometry.dart';
import '../l10n/l10n.dart';

class StreamSourceSize {
  const StreamSourceSize(this.width, this.height);
  final int width, height;
}

class DecoderAdmission {
  const DecoderAdmission(this.limits, this.pictures, {this.hardwareDecode});
  final DecoderLimits limits;
  final List<StreamSourceSize> pictures;
  final bool? hardwareDecode;
}

class StreamCapacityException implements Exception {
  const StreamCapacityException(this.reason);
  final String reason;
}

typedef ExactStreamProbe = Future<Object?> Function(
  Map<String, Object> request,
);
typedef StreamAdmissionQuery = Future<DecoderAdmission> Function(
  List<StreamSourceSize>,
  int,
);

String streamCapacityMessage(String reason) => switch (reason) {
  'memory_budget' => l10n.notEnoughMemoryPrepareScreen,
  'insufficient_instances' => l10n.twoScreensCantShownOnce,
  'unsupported_rate' => l10n.screenSizeFrameRateCant,
  'unsupported_profile' => l10n.videoFormatCantShownHere,
  'unsupported_size' ||
  'decoder_unsupported' ||
  'gpu_unsupported' => l10n.screenSizeCantShownHere,
  _ => l10n.couldntCheckWhichScreenSizes,
};

const _capacityReasons = {
  'invalid_request',
  'unsupported_size',
  'unsupported_rate',
  'unsupported_profile',
  'insufficient_instances',
  'memory_budget',
  'capability_unknown',
  'engine_closed',
  'session_replaced',
  'decoder_unavailable',
  'decoder_unsupported',
  'gpu_unsupported',
  'resource_unavailable',
};
const _reasonAliases = {
  'stream_size_abi': 'invalid_request',
  'stream_memory_unknown': 'capability_unknown',
  'stream_memory_budget': 'memory_budget',
  'stream_texture_unknown': 'capability_unknown',
  'stream_texture_dimension': 'unsupported_size',
  'stream_texture_budget': 'memory_budget',
  'stream_decoder_unavailable': 'decoder_unavailable',
  'stream_decoder_type': 'decoder_unsupported',
};

/// Every returned picture was accepted by the actual selected decoder at the
/// requested FPS. Workspace sizes are always retained, including during fit.
/// This is type/resource admission, never proof of a decoded/displayed frame.
Future<DecoderAdmission> nativeStreamAdmission(
  List<StreamSourceSize> sources,
  int fps, {
  ExactStreamProbe? probe,
  bool? hardware,
}) async {
  if (sources.isEmpty ||
      sources.length > 2 ||
      ![30, 60].contains(fps) ||
      sources.any(
        (s) => !videoGeometry(
          s.width,
          s.height,
          minimumWidth: 320,
          minimumHeight: 180,
        ),
      )) {
    throw const StreamCapacityException('invalid_request');
  }
  final invoke =
      probe ??
      (Map<String, Object> request) =>
          const MethodChannel('multitap/remote')
              .invokeMethod<Object?>('validateStreamSize', request);
  final useHardware = hardware ?? DecodeSettings.hardwareDecode;
  var lower = 0.0, upper = 1.0, scale = 1.0;
  List<StreamSourceSize>? accepted;
  String lastReason = 'capability_unknown';
  final seen = <String>{};
  for (var attempt = 0; attempt < 12; attempt++) {
    final pictures = [
      for (final s in sources)
        StreamSourceSize(
          math.max(16, (s.width * scale / 2).floor() * 2),
          math.max(16, (s.height * scale / 2).floor() * 2),
        ),
    ];
    final identity = pictures.map((p) => '${p.width}x${p.height}').join(',');
    if (!seen.add(identity)) break;
    Object? response;
    try {
      response = await invoke({
        'schema': 1,
        'hardware': useHardware,
        'tracks': [
          for (var i = 0; i < sources.length; i++)
            {
              'width': pictures[i].width,
              'height': pictures[i].height,
              'fps': fps,
              'sourceWidth': sources[i].width,
              'sourceHeight': sources[i].height,
            },
        ],
      }).timeout(const Duration(seconds: 5));
    } catch (_) {
      throw const StreamCapacityException('capability_unknown');
    }
    if (response is! Map ||
        response['schema'] is! int ||
        response['schema'] != 1 ||
        response['validation'] != 'exact-candidate' ||
        response['accepted'] is! bool) {
      throw const StreamCapacityException('capability_unknown');
    }
    final tracks = response['tracks'];
    if (tracks is! List || tracks.length != pictures.length) {
      throw const StreamCapacityException('capability_unknown');
    }
    for (var i = 0; i < tracks.length; i++) {
      final t = tracks[i], p = pictures[i];
      if (t is! Map ||
          t['width'] is! int ||
          t['width'] != p.width ||
          t['height'] is! int ||
          t['height'] != p.height ||
          t['fps'] is! int ||
          t['fps'] != fps ||
          t['accepted'] is! bool ||
          (response['accepted'] == true && t['accepted'] != true)) {
        throw const StreamCapacityException('capability_unknown');
      }
    }
    if (response['accepted'] == true) {
      accepted = pictures;
      lower = scale;
      if (scale == 1.0 || upper - lower < 0.001) break;
      scale = (lower + upper) / 2;
    } else {
      final rawReason = response['reason'];
      final reason = _reasonAliases[rawReason] ?? rawReason;
      lastReason = reason is String && _capacityReasons.contains(reason)
          ? reason
          : 'capability_unknown';
      if (!{
        'unsupported_size',
        'unsupported_rate',
        'memory_budget',
        'decoder_unsupported',
        'gpu_unsupported',
      }.contains(lastReason)) {
        throw StreamCapacityException(lastReason);
      }
      upper = scale;
      scale = lower == 0 ? scale / 2 : (lower + upper) / 2;
    }
  }
  if (accepted == null) throw StreamCapacityException(lastReason);
  var width = 16, height = 16, pixels = 256;
  for (final p in accepted) {
    width = math.max(width, p.width);
    height = math.max(height, p.height);
    pixels = math.max(
      pixels,
      ((p.width + 15) ~/ 16) * ((p.height + 15) ~/ 16) * 256,
    );
  }
  if (pixels > maximumWireInteger ~/ fps) {
    throw const StreamCapacityException('invalid_request');
  }
  return DecoderAdmission(
    DecoderLimits(
      maxWidth: width,
      maxHeight: height,
      maxPixels: pixels,
      maxPixelRate: 0,
    ),
    List.unmodifiable(accepted),
    hardwareDecode: useHardware,
  );
}

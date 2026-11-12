import 'dart:math' as math;

import 'package:flutter/services.dart';

import 'track_stream.dart';
import 'video_geometry.dart';
import 'broker_api.dart';
import '../models/stream_options.dart';

class LocalDisplaySurface {
  const LocalDisplaySurface(this.id, this.bounds, this.primary, this.dpi);
  final String id;
  final Rect bounds;
  final bool primary;
  final int dpi;
}

/// Physical monitor rectangles measured by the PMv2 native window. The same
/// engine draws one separate decoder texture on each corresponding rectangle;
/// there is no encoded wide composite or cross-engine texture sharing.
class LocalDisplayLayout {
  LocalDisplayLayout._(this.bounds, this.surfaces, this.plan);
  final Rect bounds;
  final List<LocalDisplaySurface> surfaces;
  final TrackPlan plan;

  static const channel = MethodChannel('multitap/window');

  /// [covered] = the window must already cover both monitors exactly (the
  /// layout the connection uses). `false` only measures the monitors, before
  /// the window is spread over them: reading with the cover check first made
  /// every "모든 모니터" attempt fail before it reached the broker.
  static Future<LocalDisplayLayout> read({bool covered = true}) async {
    try {
      final value = await channel
          .invokeMapMethod<Object?, Object?>('monitorLayout', {
            'requireCover': covered,
          })
          .timeout(
            const Duration(seconds: 3),
            onTimeout: () =>
                throw const BrokerException('local_display_layout_unavailable'),
          );
      if (value == null) {
        throw const BrokerException('local_display_layout_unavailable');
      }
      return LocalDisplayLayout.fromMap(value);
    } on PlatformException {
      throw const BrokerException('local_display_layout_unavailable');
    } on FormatException {
      throw const BrokerException('local_display_layout_unsupported');
    }
  }

  /// Whether this PC has exactly two monitors the stream can pair, measured
  /// without the cover check: true when measured, false when Windows reports
  /// another count (the native side refuses anything but two) or a pair the
  /// stream cannot use, null when there is no answer (slow, or a preview
  /// without a native window). Only decides whether to offer the mode; the
  /// connection itself still reads [read] with the cover check.
  static Future<bool?> twoMonitors() async {
    try {
      final value = await channel
          .invokeMapMethod<Object?, Object?>('monitorLayout', {
            'requireCover': false,
          })
          .timeout(const Duration(seconds: 3));
      if (value == null) return null;
      LocalDisplayLayout.fromMap(value);
      return true;
    } on PlatformException {
      return false;
    } on FormatException {
      return false;
    } catch (_) {
      return null;
    }
  }

  factory LocalDisplayLayout.fromMap(Map<Object?, Object?> value) {
    int number(Object? value, int low, int high) {
      if (value is! int || value < low || value > high) {
        throw const FormatException('Invalid local display layout.');
      }
      return value;
    }

    final monitors = value['monitors'];
    if (monitors is! List || monitors.length != 2) {
      throw const FormatException('Exactly two local displays are required.');
    }
    final surfaces = <LocalDisplaySurface>[];
    for (final item in monitors) {
      if (item is! Map ||
          item['id'] != surfaces.length.toString() ||
          item['primary'] is! bool) {
        throw const FormatException('Invalid local display identity.');
      }
      final x = number(
            item['x'],
            minimumGeometryCoordinate,
            maximumGeometryCoordinate,
          ),
          y = number(
            item['y'],
            minimumGeometryCoordinate,
            maximumGeometryCoordinate,
          ),
          width = number(item['width'], 320, maximumGeometrySide),
          height = number(item['height'], 180, maximumGeometrySide),
          dpi = number(item['dpi'], 48, 960);
      if (width.isOdd || height.isOdd || !videoPlacement(width, height, x, y)) {
        throw const FormatException('Unsupported odd display size.');
      }
      surfaces.add(
        LocalDisplaySurface(
          item['id'] as String,
          Rect.fromLTWH(
            x.toDouble(),
            y.toDouble(),
            width.toDouble(),
            height.toDouble(),
          ),
          item['primary'] as bool,
          dpi,
        ),
      );
    }
    if (!surfaces.first.primary ||
        surfaces.last.primary ||
        surfaces[0].bounds.overlaps(surfaces[1].bounds)) {
      throw const FormatException('Ambiguous local display layout.');
    }
    final bounds = surfaces[0].bounds.expandToInclude(surfaces[1].bounds);
    if (number(value['width'], 320, maximumGeometrySide) != bounds.width ||
        number(value['height'], 180, maximumGeometrySide) != bounds.height ||
        number(
              value['x'],
              minimumGeometryCoordinate,
              maximumGeometryCoordinate,
            ) !=
            bounds.left ||
        number(
              value['y'],
              minimumGeometryCoordinate,
              maximumGeometryCoordinate,
            ) !=
            bounds.top) {
      throw const FormatException('Local display bounds changed.');
    }
    final primary = surfaces.first.bounds;
    final plan = TrackPlan(
      surfaces.map(
        (surface) => TrackViewport(
          surface.id,
          surface.bounds.width.toInt(),
          surface.bounds.height.toInt(),
          (surface.bounds.left - primary.left).toInt(),
          (surface.bounds.top - primary.top).toInt(),
        ),
      ),
    );
    return LocalDisplayLayout._(bounds, List.unmodifiable(surfaces), plan);
  }

  StreamOptions options(StreamOptions quality) => StreamOptions(
    maxWidth: surfaces.map((s) => s.bounds.width.toInt()).reduce(math.max),
    maxHeight: surfaces.map((s) => s.bounds.height.toInt()).reduce(math.max),
    fps: quality.fps,
    bitrate: quality.bitrate,
    audio: quality.audio,
    monitor: 'all',
  );

  /// Current Host source rectangles must still pair with these physical local
  /// outputs. Encoded-picture sizes and per-monitor UI DPI do not rescale the
  /// source coordinate system; compare relative to output0 on both machines.
  bool matchesSources(List<StreamTrack> tracks) {
    if (tracks.length != surfaces.length) {
      return false;
    }
    final byId = {for (final track in tracks) track.id: track};
    if (byId.length != tracks.length || byId['0'] == null) {
      return false;
    }
    final primary = surfaces.first.bounds, remote = byId['0']!;
    for (final surface in surfaces) {
      final track = byId[surface.id];
      if (track == null ||
          track.width != surface.bounds.width ||
          track.height != surface.bounds.height ||
          track.left - remote.left != surface.bounds.left - primary.left ||
          track.top - remote.top != surface.bounds.top - primary.top) {
        return false;
      }
    }
    return true;
  }

  bool matchesViewport(Size logical, double pixelRatio) =>
      (logical.width * pixelRatio - bounds.width).abs() < 1.5 &&
      (logical.height * pixelRatio - bounds.height).abs() < 1.5;

  /// Resolve the whole window's pointer position, including a held drag whose
  /// Flutter route still belongs to the surface where the button went down.
  /// Empty space between differently arranged monitors has no remote target.
  ({String track, double x, double y})? pointAt(
    Offset logical,
    double pixelRatio,
  ) {
    if (!logical.dx.isFinite ||
        !logical.dy.isFinite ||
        !pixelRatio.isFinite ||
        pixelRatio <= 0) {
      return null;
    }
    final physical = logical * pixelRatio + bounds.topLeft;
    for (final surface in surfaces) {
      if (surface.bounds.contains(physical)) {
        return (
          track: surface.id,
          x: (physical.dx - surface.bounds.left) / surface.bounds.width,
          y: (physical.dy - surface.bounds.top) / surface.bounds.height,
        );
      }
    }
    return null;
  }
}

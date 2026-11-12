import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../data/video_geometry.dart';

enum RemoteViewMode { fitNoUpscale, fit, pixels }

/// Request actual physical pixels, never a logical-DPI size or a stretched image.
/// Use four-pixel Windows source widths and even NV12 heights. The signed
/// display path rejects 1582x952 while 1580/1584x952 are accepted.
/// Keep the sub-four-pixel edge outside the real source; never invent source
/// geometry by stretching. A canvas larger than 3840x2160 (a window spanning
/// monitors) requests the largest supported size with the window's shape and
/// the view scales it; a window below 320x180 keeps the current resolution.
/// [portrait] (stream-size-v1): a tall canvas (a portrait monitor) may use up
/// to 3840 rows; the longer side stays at most 3840 and one 4K frame's pixels.
Size? viewportResolution(
  Size logical,
  double pixelRatio, {
  bool portrait = false,
}) {
  if (!logical.width.isFinite ||
      !logical.height.isFinite ||
      !pixelRatio.isFinite ||
      pixelRatio <= 0) {
    return null;
  }
  var physicalWidth = _wholePixels(logical.width * pixelRatio);
  var physicalHeight = _wholePixels(logical.height * pixelRatio);
  if (physicalWidth <= 0 || physicalHeight <= 0) return null;
  if (!physicalWidth.isFinite ||
      !physicalHeight.isFinite ||
      physicalWidth > maximumGeometrySide ||
      physicalHeight > maximumGeometrySide) {
    return null;
  }
  final width = (physicalWidth / 4).floor() * 4;
  final height = (physicalHeight / 2).floor() * 2;
  if (!videoGeometry(width, height, minimumWidth: 320, minimumHeight: 180)) {
    return null;
  }
  return Size(width.toDouble(), height.toDouble());
}

/// A logical size times the pixel ratio is a whole number of physical pixels
/// up to the rounding of Flutter's own division: at 175 % a 2560-pixel window
/// is 1462.857... logical pixels, which multiplies back to 2559.9999999999995
/// and must not floor to a 2556-pixel request.
double _wholePixels(double value) {
  final nearest = value.roundToDouble();
  return (value - nearest).abs() < 1e-6 ? nearest : value;
}

/// How the remote picture's texture is sampled. Nearest only where every
/// texture pixel lands on exactly one device pixel: the texture is shown 1:1.
/// A scaled view, or a Host-downscaled video under a larger source picture (a
/// texture smaller than the picture), is filtered: nearest would upscale it
/// blocky. Linear sampling of an aligned 1:1 texture is exact as well.
FilterQuality remoteTextureFilter({
  required Size texture,
  required Size shown,
  required double pixelRatio,
}) {
  bool same(double logical, double pixels) =>
      (logical * pixelRatio - pixels).abs() < 0.5;
  return same(shown.width, texture.width) && same(shown.height, texture.height)
      ? FilterQuality.none
      : FilterQuality.low;
}

Size remoteDisplaySize(
  Size frame,
  Size available,
  double pixelRatio,
  RemoteViewMode mode,
) {
  final native = Size(frame.width / pixelRatio, frame.height / pixelRatio);
  if (mode == RemoteViewMode.pixels) return native;
  var scale = math.min(
    available.width / native.width,
    available.height / native.height,
  );
  if (mode == RemoteViewMode.fitNoUpscale) scale = math.min(scale, 1.0);
  return Size(native.width * scale, native.height * scale);
}

class RemoteViewport extends StatefulWidget {
  const RemoteViewport({
    super.key,
    required this.frameSize,
    required this.mode,
    required this.captured,
    required this.builder,
  });
  final Size frameSize;
  final RemoteViewMode mode;
  final bool captured;
  final Widget Function(Size) builder;
  @override
  State<RemoteViewport> createState() => _RemoteViewportState();
}

class _RemoteViewportState extends State<RemoteViewport> {
  final _horizontal = ScrollController(), _vertical = ScrollController();
  @override
  void dispose() {
    _horizontal.dispose();
    _vertical.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, constraints) {
      final available = constraints.biggest;
      final display = remoteDisplaySize(
        widget.frameSize,
        available,
        MediaQuery.devicePixelRatioOf(context),
        widget.mode,
      );
      final ratio = MediaQuery.devicePixelRatioOf(context);
      final frame = SizedBox(
        key: const Key('remote-image-bounds'),
        width: display.width,
        height: display.height,
        child: widget.builder(display),
      );
      if (widget.mode != RemoteViewMode.pixels) {
        // Centred on whole physical pixels: a half-pixel offset (the request
        // rounds to 4x2 pixels) would make the GPU blend every pixel with its
        // neighbour, blurring text and icons even at 1:1.
        double snap(double value) =>
            (math.max(0.0, value) * ratio).floorToDouble() / ratio;
        return Stack(
          children: [
            Positioned(
              left: snap((available.width - display.width) / 2),
              top: snap((available.height - display.height) / 2),
              child: frame,
            ),
          ],
        );
      }
      final contentWidth = math.max(display.width, available.width),
          contentHeight = math.max(display.height, available.height);
      final physics = widget.captured
          ? const NeverScrollableScrollPhysics()
          : const ClampingScrollPhysics();
      return ClipRect(
        child: Scrollbar(
          controller: _vertical,
          thumbVisibility: true,
          notificationPredicate: (notification) =>
              notification.metrics.axis == Axis.vertical,
          child: Scrollbar(
            controller: _horizontal,
            thumbVisibility: true,
            scrollbarOrientation: ScrollbarOrientation.bottom,
            notificationPredicate: (notification) =>
                notification.metrics.axis == Axis.horizontal,
            child: SingleChildScrollView(
              controller: _horizontal,
              scrollDirection: Axis.horizontal,
              physics: physics,
              child: SizedBox(
                width: contentWidth,
                height: available.height,
                child: SingleChildScrollView(
                  controller: _vertical,
                  physics: physics,
                  child: SizedBox(
                    width: contentWidth,
                    height: contentHeight,
                    child: Center(child: frame),
                  ),
                ),
              ),
            ),
          ),
        ),
      );
    },
  );
}

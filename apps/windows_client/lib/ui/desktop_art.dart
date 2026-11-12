import 'package:flutter/material.dart';

import 'korean_text.dart';

/// The tile picture: the desktop's last-frame [thumbnail] when one exists,
/// otherwise a plain Windows-like blue wallpaper. The placeholder is never
/// dressed up as a screen (no fake taskbar, window mark or per-VM colours,
/// which read as a strange green "preview").
class DesktopArt extends StatelessWidget {
  const DesktopArt({
    super.key,
    required this.seed,
    this.label = '',
    this.dimmed = false,
    this.snapshot = false,
    this.mark = false,
    this.thumbnail,
    this.fit = BoxFit.cover,
  });

  final String seed;

  /// Optional caption drawn over the picture (unused by the tiles, which
  /// already show the operating system beside the name).
  final String label;

  /// Off, waiting or failing desktops are shown greyed and darkened.
  final bool dimmed;

  /// An old last frame of a running desktop: slightly dimmed and
  /// desaturated so it reads as a stored picture, not a live view. [dimmed]
  /// already covers it.
  final bool snapshot;

  // Saturation .45 (Rec. 709 luma), then brightness .8.
  static const _snapshotFilter = ColorFilter.matrix([
    .4535, .3147, .0318, 0, 0, //
    .0935, .6747, .0318, 0, 0, //
    .0935, .3147, .3918, 0, 0, //
    0, 0, 0, 1, 0,
  ]);

  /// Kept for callers; the placeholder no longer draws a mark.
  final bool mark;

  /// The last frame of this desktop, when the client has one.
  final ImageProvider? thumbnail;
  final BoxFit fit;

  // Dark base, mid tone, bloom: one calm Windows-like blue for every VM.
  static const _palettes = [
    [Color(0xFF071434), Color(0xFF0D2F86), Color(0xFF7FB0FF)],
  ];

  static int _hash(String value) {
    var hash = 0x811C9DC5;
    for (final unit in value.codeUnits) {
      hash = ((hash ^ unit) * 0x01000193) & 0xFFFFFFFF;
    }
    return hash;
  }

  @override
  Widget build(BuildContext context) {
    final hash = _hash(seed);
    final colors = _palettes[hash % _palettes.length];
    Widget art = thumbnail == null
        ? CustomPaint(
            painter: _ArtPainter(colors, hash),
            child: const SizedBox.expand(),
          )
        : Image(
            image: thumbnail!,
            fit: fit,
            filterQuality: FilterQuality.medium,
            gaplessPlayback: true,
            errorBuilder: (_, _, _) => CustomPaint(
              painter: _ArtPainter(colors, hash),
              child: const SizedBox.expand(),
            ),
          );
    if (dimmed) {
      art = ColorFiltered(
        colorFilter: const ColorFilter.matrix([
          0.16, 0.32, 0.06, 0, 0, //
          0.16, 0.32, 0.06, 0, 0, //
          0.16, 0.32, 0.06, 0, 0, //
          0, 0, 0, 1, 0,
        ]),
        child: art,
      );
    } else if (snapshot && thumbnail != null) {
      art = ColorFiltered(colorFilter: _snapshotFilter, child: art);
    }
    return ClipRect(
      child: LayoutBuilder(
        builder: (context, bounds) => Stack(
          fit: StackFit.expand,
          children: [
            art,
            if (label.isNotEmpty)
              Positioned(
                left: 14,
                bottom: 12,
                right: 14,
                child: KeepAllText(
                  label,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontFamily: 'Pretendard',
                    fontFamilyFallback: const ['Malgun Gothic'],
                    fontSize: 12,
                    fontWeight: FontWeight.w600,
                    letterSpacing: .2,
                    color: Colors.white.withValues(alpha: dimmed ? .42 : .72),
                    shadows: const [
                      Shadow(color: Color(0x66000000), blurRadius: 6),
                    ],
                  ),
                ),
              ),
          ],
        ),
      ),
    );
  }
}

class _ArtPainter extends CustomPainter {
  const _ArtPainter(this.colors, this.seed);
  final List<Color> colors;
  final int seed;

  @override
  void paint(Canvas canvas, Size size) {
    final rect = Offset.zero & size;
    canvas.clipRect(rect);
    canvas.drawRect(
      rect,
      Paint()
        ..shader = LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [colors[0], colors[1], colors[0]],
          stops: const [0, .55, 1],
        ).createShader(rect),
    );
    // Two soft blooms, placed per VM so neighbouring tiles differ.
    final dx = .52 + ((seed >> 4) % 26) / 100;
    final dy = .40 + ((seed >> 9) % 18) / 100;
    void bloom(Offset center, double radius, double strength) =>
        canvas.drawCircle(
          center,
          radius,
          Paint()
            ..shader = RadialGradient(
              colors: [
                colors[2].withValues(alpha: strength),
                colors[2].withValues(alpha: strength * .32),
                colors[2].withValues(alpha: 0),
              ],
              stops: const [0, .42, 1],
            ).createShader(Rect.fromCircle(center: center, radius: radius)),
        );
    bloom(
      Offset(size.width * dx, size.height * dy),
      size.shortestSide * .82,
      .78,
    );
    bloom(
      Offset(size.width * (dx - .26), size.height * (dy + .2)),
      size.shortestSide * .6,
      .3,
    );
    // A thin light ribbon, like a Windows wallpaper petal.
    final ribbon = Path()
      ..moveTo(size.width * (dx - .34), size.height * (dy + .32))
      ..quadraticBezierTo(
        size.width * dx,
        size.height * (dy - .38),
        size.width * (dx + .3),
        size.height * (dy - .05),
      );
    canvas.drawPath(
      ribbon,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = size.shortestSide * .05
        ..strokeCap = StrokeCap.round
        ..color = Colors.white.withValues(alpha: .1)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 6),
    );
  }

  @override
  bool shouldRepaint(_ArtPainter old) =>
      old.seed != seed || old.colors != colors;
}

import 'package:flutter/material.dart';

import '../data/appearance_settings.dart';
import 'app_theme.dart';
import 'korean_text.dart';

/// The chosen scene (theme menu), as night in the dark appearance and day in
/// the light one.
class SceneBackground extends StatelessWidget {
  const SceneBackground({super.key, required this.child});
  final Widget child;

  @override
  Widget build(BuildContext context) => Stack(
    fit: StackFit.expand,
    children: [
      Image.asset(
        (AppearanceScope.maybeOf(context)?.scene ?? BackgroundScene.peaks)
            .asset(dark: AppPalette.of(context).dark),
        key: const Key('scene-background-image'),
        fit: BoxFit.cover,
        gaplessPlayback: true,
        errorBuilder: (_, _, _) => const DecoratedBox(
          decoration: BoxDecoration(
            gradient: LinearGradient(
              begin: Alignment.topCenter,
              end: Alignment.bottomCenter,
              colors: [Color(0xFF293341), Color(0xFF625947), Color(0xFF15201E)],
            ),
          ),
        ),
      ),
      DecoratedBox(
        key: const Key('scene-appearance-overlay'),
        decoration: BoxDecoration(
          gradient: LinearGradient(
            begin: Alignment.topLeft,
            end: Alignment.bottomRight,
            colors: AppPalette.of(context).sceneOverlay,
          ),
        ),
      ),
      child,
    ],
  );
}

/// The Multitap mark: a power strip (three outlets and its cord), shared with
/// the website and the native icons. The master geometry is
/// tools/icons/src/multitap-mark.svg, and multitap-mark-small.svg at 32 px and
/// below (larger holes, thicker cord).
///
/// On its own it is the bare mark in one colour ([color], or the amber of the
/// theme). With [tile] it is the app icon: the amber tile with the dark mark.
class ProductMark extends StatelessWidget {
  const ProductMark({
    super.key,
    this.size = 32,
    this.color,
    this.tile = false,
    this.shadows,
  });
  final double size;

  /// The colour of the bare mark. Defaults to the amber of the theme: the
  /// amber itself on a dark surface, the deep amber ink on a light one.
  final Color? color;
  final bool tile;

  /// A soft halo behind the bare mark (the light sign-in sets it over the sky).
  final List<Shadow>? shadows;

  static const amber = Color(0xFFF5A623);

  /// The deep amber of the wordmark on a light surface ("Proxmox VE" there).
  static const deepAmber = Color(0xFF66360A);

  /// The mark on the amber tile.
  static const tileInk = Color(0xFF1E1503);

  @override
  Widget build(BuildContext context) => SizedBox.square(
    dimension: size,
    child: CustomPaint(
      painter: _MarkPainter(
        color: tile
            ? tileInk
            : color ?? AppPalette.of(context).amberInk,
        tile: tile,
        small: size <= 32,
        shadows: tile ? null : shadows,
      ),
    ),
  );
}

class _MarkPainter extends CustomPainter {
  const _MarkPainter({
    required this.color,
    required this.tile,
    required this.small,
    this.shadows,
  });
  final Color color;
  final bool tile;
  final bool small;
  final List<Shadow>? shadows;

  // One path each, filled non-zero: the bar clockwise, the three holes the
  // other way, the cord (an outline, not a stroke) clockwise. viewBox 120.
  static Path _strip({
    required Rect bar,
    required double hole,
    required List<double> holes,
    required double cordWidth,
    required double cordStart,
    required double cordEnd,
  }) {
    const cordX = 8.0, bend = 12.0;
    final r = bar.height / 2, cy = bar.center.dy, h = cordWidth / 2;
    final path = Path()
      ..moveTo(bar.left + r, bar.top)
      ..lineTo(bar.right - r, bar.top)
      ..arcToPoint(
        Offset(bar.right - r, bar.bottom),
        radius: Radius.circular(r),
      )
      ..lineTo(bar.left + r, bar.bottom)
      ..arcToPoint(Offset(bar.left + r, bar.top), radius: Radius.circular(r))
      ..close();
    for (final x in holes) {
      path
        ..moveTo(x + hole, cy)
        ..arcToPoint(
          Offset(x - hole, cy),
          radius: Radius.circular(hole),
          clockwise: false,
        )
        ..arcToPoint(
          Offset(x + hole, cy),
          radius: Radius.circular(hole),
          clockwise: false,
        )
        ..close();
    }
    return path
      ..moveTo(cordStart, cy + h)
      ..lineTo(cordX + bend, cy + h)
      ..arcToPoint(
        Offset(cordX + h, cy + bend),
        radius: Radius.circular(bend - h),
        clockwise: false,
      )
      ..lineTo(cordX + h, cordEnd)
      ..arcToPoint(Offset(cordX - h, cordEnd), radius: Radius.circular(h))
      ..lineTo(cordX - h, cy + bend)
      ..arcToPoint(
        Offset(cordX + bend, cy - h),
        radius: Radius.circular(bend + h),
      )
      ..lineTo(cordStart, cy - h)
      ..close();
  }

  // tools/icons/src/multitap-mark.svg.
  static final _large = _strip(
    bar: const Rect.fromLTRB(24, 38, 112, 84),
    hole: 7.5,
    holes: const [46, 68, 90],
    cordWidth: 10,
    cordStart: 26,
    cordEnd: 97,
  );
  // tools/icons/src/multitap-mark-small.svg.
  static final _small = _strip(
    bar: const Rect.fromLTRB(20, 34, 114, 86),
    hole: 9,
    holes: const [43, 67, 91],
    cordWidth: 13,
    cordStart: 24,
    cordEnd: 98,
  );

  @override
  void paint(Canvas canvas, Size size) {
    canvas.save();
    if (tile) {
      // The app icon: a 160-unit tile, the mark placed so that its weight sits
      // on the centre (tools/icons/src/windows/tile-256.svg).
      canvas.scale(size.width / 160, size.height / 160);
      canvas.drawRRect(
        RRect.fromLTRBR(0, 0, 160, 160, const Radius.circular(36)),
        Paint()..color = ProductMark.amber,
      );
      if (small) {
        canvas.translate(19, 16);
        canvas.scale(1.05);
      } else {
        canvas.translate(20, 17);
      }
    } else {
      canvas.scale(size.width / 120, size.height / 120);
    }
    final path = small ? _small : _large;
    for (final shadow in shadows ?? const <Shadow>[]) {
      canvas.drawPath(
        path,
        Paint()
          ..color = shadow.color
          ..maskFilter = MaskFilter.blur(
            BlurStyle.normal,
            // In the units of the path: the canvas is scaled to 120.
            shadow.blurSigma * 120 / size.width,
          ),
      );
    }
    canvas.drawPath(path, Paint()..color = color);
    canvas.restore();
  }

  @override
  bool shouldRepaint(_MarkPainter oldDelegate) =>
      oldDelegate.color != color ||
      oldDelegate.tile != tile ||
      oldDelegate.small != small ||
      oldDelegate.shadows != shadows;
}

class Brand extends StatelessWidget {
  const Brand({super.key, this.onDark = false, this.scale = 1});
  final bool onDark;

  /// Multiplies the wordmark's 22 px size (the sign-in uses a larger mark).
  final double scale;

  // The ink of the wordmark on a light surface.
  static const _lightPrimary = Color(0xFF17243B);
  // The light sign-in places the wordmark directly over a bright sky (about
  // #6DB7F9-#8DC7FC), so the secondary parts use deep tones (>= 4.5:1 there)
  // and the same soft halo as the hero text. On the near-white workspace
  // header the halo is invisible.
  static const _lightSecondary = Color(0xFF2D4352);
  static const _halo = [Shadow(color: Color(0xCCFFFFFF), blurRadius: 6)];

  @override
  Widget build(BuildContext context) {
    final halo = onDark ? null : _halo;
    final size = 22 * scale;
    // The product amber; its deep tone keeps >= 4.5:1 over the daylight sky.
    final amber = onDark ? ProductMark.amber : ProductMark.deepAmber;
    final words = Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        // One weight and one colour for the whole word. The key is older
        // than the name.
        KeepAllText(
          'Multitap',
          key: const Key('brand-vdi'),
          maxLines: 1,
          style: TextStyle(
            fontFamily: 'Pretendard',
            fontSize: size,
            height: 1.1,
            letterSpacing: -.04 * size,
            fontWeight: FontWeight.w700,
            color: onDark ? Colors.white : _lightPrimary,
            shadows: halo,
          ),
        ),
        SizedBox(height: 1.5 * scale),
        // A third-party tool for Proxmox VE, not a Proxmox product: "for", and
        // the name in Multitap's own amber, not Proxmox's orange (the notice
        // is in 클라이언트 정보).
        Text.rich(
          key: const Key('brand-for'),
          TextSpan(
            text: 'for ',
            children: [
              TextSpan(
                text: 'Proxmox VE',
                style: TextStyle(
                  fontWeight: onDark ? FontWeight.w600 : FontWeight.w700,
                  color: amber,
                ),
              ),
            ],
          ),
          style: TextStyle(
            fontFamily: 'Pretendard',
            fontSize: 9 * (scale > 1 ? (1 + (scale - 1) * .45) : scale),
            height: 1.1,
            letterSpacing: .35,
            fontWeight: onDark ? FontWeight.w400 : FontWeight.w500,
            color: onDark ? const Color(0xFFB5C5DC) : _lightSecondary,
            shadows: halo,
          ),
        ),
      ],
    );
    // The bare mark beside the word, in the amber of "Proxmox VE".
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        ProductMark(size: 30 * scale, color: amber, shadows: halo),
        SizedBox(width: 9 * scale),
        words,
      ],
    );
  }
}

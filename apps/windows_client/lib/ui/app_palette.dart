import 'package:flutter/material.dart';

/// ToTB Link's dark/amber palette, adapted for both readable appearances.
class AppPalette {
  const AppPalette(this.dark);
  final bool dark;
  static AppPalette of(BuildContext context) =>
      AppPalette(Theme.of(context).brightness == Brightness.dark);
  Color get canvas => dark ? const Color(0xFF0E1418) : const Color(0xFFF3F5F6);
  Color get surface => dark ? const Color(0xFF141C21) : Colors.white;
  Color get subtle => dark ? const Color(0xFF1A252B) : const Color(0xFFF4F6F8);
  Color get line => dark ? const Color(0xFF2B3941) : const Color(0xFFDCE3E7);
  Color get ink => dark ? const Color(0xFFE9F1F3) : const Color(0xFF19262D);

  /// Secondary ink, between [ink] and [muted]: links, disclosure labels and
  /// text buttons that are not the surface's main action (amber is kept for
  /// that action, the current selection and the brand).
  Color get ink2 => dark ? const Color(0xFFC6CDD2) : const Color(0xFF36414A);
  Color get muted => dark ? const Color(0xFF9AADB6) : const Color(0xFF596C76);
  Color get amber => const Color(0xFFF5A623);
  Color get amberInk => dark ? amber : const Color(0xFF865000);

  /// The keyboard-focus ring and a focused field's edge. The amber itself is
  /// 2.0:1 on white, so the light theme takes the deep amber (6.6:1).
  Color get focus => amberInk;
  Color get blue => dark ? const Color(0xFF78B9F7) : const Color(0xFF286CB2);
  Color get green => dark ? const Color(0xFF62E3A9) : const Color(0xFF147652);
  Color get red => dark ? const Color(0xFFFF8A7A) : const Color(0xFFB8404B);
  Color get purple => dark ? const Color(0xFFB8A2F5) : const Color(0xFF7956B4);

  /// The session stage: what is behind a stream, a Console picture or a
  /// session card before there is a picture. Always dark, in both themes.
  static const stage = Color(0xFF0B0E10);

  // The soft fill and the edge of a failure box and of a "Host side / in
  // progress" box, so every such box has one strength.
  Color get redSoft => red.withValues(alpha: dark ? .12 : .08);
  Color get redLine => red.withValues(alpha: dark ? .34 : .30);
  Color get blueSoft => blue.withValues(alpha: dark ? .13 : .09);
  Color get blueLine => blue.withValues(alpha: dark ? .30 : .28);
  // Frosted header over the scene; the scene stays faintly visible through
  // the blur behind it.
  Color get sceneChrome =>
      dark ? const Color(0xD60E1418) : const Color(0xE0FAFCFD);

  /// Frosted VM card surface and its hairline edge (ToTB Link's raised
  /// panels: near-opaque slate in dark, near-opaque white in light).
  Color get card => dark ? const Color(0xE8141C21) : const Color(0xF2FFFFFF);
  Color get cardEdge =>
      dark ? const Color(0x1FFFFFFF) : const Color(0x1F0E2A3A);
  List<Color> get sceneOverlay => dark
      ? const [Color(0xB50E1418), Color(0x60141C21), Color(0xAA0E1418)]
      : const [Colors.transparent, Colors.transparent, Colors.transparent];

  // Launcher ("one-click tiles"): flat charcoal or pale grey surfaces. Only
  // the action under the pointer or keyboard focus turns amber.
  Color get launcher =>
      dark ? const Color(0xFF0F1215) : const Color(0xFFEDEFF1);
  Color get tile => dark ? const Color(0xFF171B1F) : Colors.white;
  Color get tileWell =>
      dark ? const Color(0xFF1D2226) : const Color(0xFFF3F5F6);
  Color get tileLine =>
      dark ? const Color(0xFF262C32) : const Color(0xFFDCE1E5);
  Color get quiet => dark ? const Color(0xFF22282D) : const Color(0xFFEDF0F2);

  /// A segmented control: a grey groove ([segmentTrack]) and the chosen
  /// segment raised out of it ([segmentOn], the lightest step in either
  /// theme). No amber: a choice among equals is not the main action.
  Color get segmentTrack =>
      dark ? const Color(0xFF1D2226) : const Color(0xFFE8ECEF);
  Color get segmentOn => dark ? const Color(0xFF2E363C) : Colors.white;

  // A toast: a dark card in both themes, with a hairline that keeps its
  // edge on a dark window.
  Color get toast => dark ? const Color(0xFF262D33) : const Color(0xFF1F2A31);
  static const toastInk = Color(0xFFF1F4F6);
  static const toastEdge = Color(0x24FFFFFF);

  // Launcher surfaces over the scenic background: a translucent fill and a
  // hairline edge, never a live backdrop blur (costly on GPU-less clients).
  Color get headerGlass =>
      dark ? const Color(0xD90E1215) : const Color(0xE6F7F9FA);
  Color get tileGlass =>
      dark ? const Color(0xEB161A1E) : const Color(0xF0FFFFFF);
  Color get glassEdge =>
      dark ? const Color(0x24FFFFFF) : const Color(0x330E2A3A);
  Color get pillGlass =>
      dark ? const Color(0xB30E1215) : const Color(0xD9FFFFFF);

  /// A soft halo that keeps text readable directly over the scene.
  List<Shadow> get sceneHalo => [
    Shadow(
      color: dark ? const Color(0x99000000) : const Color(0xCCFFFFFF),
      blurRadius: 10,
    ),
  ];

  /// A small count or keycap chip on the launcher background.
  Color get chip => dark ? const Color(0xFF22282D) : const Color(0xFFDFE3E7);

  /// The faintest colour that still reads as text or as a small mark: 4.9:1
  /// on the tile, 4.6:1 on white.
  Color get faintText =>
      dark ? const Color(0xFF7E8991) : const Color(0xFF6B767F);

  /// The label and icon of a control that is switched off (3.1:1: never for
  /// text that has to be read).
  Color get disabled =>
      dark ? const Color(0xFF5E6A73) : const Color(0xFF8A949C);

  /// The older name of [disabled]. New code says [disabled] for a control
  /// that is off and [faintText] for faint text.
  Color get faint => disabled;
  Color get hover => dark ? const Color(0x13FFFFFF) : const Color(0x110F1419);
  Color get shadow => dark ? const Color(0x99000000) : const Color(0x33101820);
  Color get onAmber => const Color(0xFF241600);
  Color get amberSoft => amber.withValues(alpha: dark ? .15 : .18);
  Color get amberLine =>
      dark ? const Color(0x99F5A623) : const Color(0x99D68A0A);

  // Readiness tones: green ready, yellow attention, grey unconfirmed.
  Color get warn => dark ? const Color(0xFFFFD24A) : const Color(0xFF8F6200);
  Color get warnSoft =>
      dark ? const Color(0x17FFD24A) : const Color(0x29E8B014);
  Color get warnLine =>
      dark ? const Color(0x42FFD24A) : const Color(0x52AA7800);
  Color get okSoft => green.withValues(alpha: .11);
  Color get okLine => green.withValues(alpha: .28);
}

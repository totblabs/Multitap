import 'package:flutter/material.dart';

import 'app_menu.dart';
import 'app_palette.dart';
export 'app_palette.dart';

/// The most a toast is wide (app_toast.dart narrows it in a small window).
const appToastWidth = 420.0;

/// A text style in the product's UI family. Button `textStyle` values replace
/// the theme style instead of merging with it, so they name the family too.
TextStyle uiText({
  double? size,
  FontWeight? weight,
  Color? color,
  double? height,
}) => TextStyle(
  fontFamily: 'Pretendard',
  fontFamilyFallback: const ['Malgun Gothic', 'Segoe UI', 'Segoe UI Symbol', 'sans-serif'],
  fontSize: size,
  fontWeight: weight,
  color: color,
  height: height,
);

ThemeData buildAppTheme({Brightness brightness = Brightness.light}) {
  final palette = AppPalette(brightness == Brightness.dark);
  return ThemeData(
    brightness: brightness,
    useMaterial3: true,
    fontFamily: 'Pretendard',
    fontFamilyFallback: const ['Malgun Gothic', 'Segoe UI', 'sans-serif'],
    scaffoldBackgroundColor: palette.canvas,
    // Every menu shares the power-menu geometry (see AppMenu).
    popupMenuTheme: AppMenu.popupTheme(palette),
    menuTheme: MenuThemeData(style: AppMenu.menuStyle(palette)),
    menuButtonTheme: MenuButtonThemeData(style: AppMenu.itemStyle(palette)),
    // Any dialog not built from AppDialog still gets its surface and type.
    dialogTheme: DialogThemeData(
      backgroundColor: palette.tile,
      surfaceTintColor: Colors.transparent,
      titleTextStyle: uiText(
        size: 17,
        weight: FontWeight.w600,
        color: palette.ink,
      ),
      contentTextStyle: uiText(size: 13.5, height: 1.6, color: palette.muted),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(18),
        side: BorderSide(color: palette.tileLine),
      ),
    ),
    // A toast is a small card at the bottom centre, never a bar as wide as
    // the window (1,888 px in a 1920 px window). showAppToast
    // (app_toast.dart) adds the result's icon and fits narrow windows and
    // large text; this is what a plain SnackBar gets.
    snackBarTheme: SnackBarThemeData(
      behavior: SnackBarBehavior.floating,
      width: appToastWidth,
      backgroundColor: palette.toast,
      contentTextStyle: uiText(
        size: 13,
        height: 1.5,
        color: AppPalette.toastInk,
      ),
      actionTextColor: palette.amber,
      elevation: 6,
      insetPadding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: const BorderSide(color: AppPalette.toastEdge),
      ),
    ),
    progressIndicatorTheme: ProgressIndicatorThemeData(
      color: palette.amber,
      linearTrackColor: palette.quiet,
      circularTrackColor: Colors.transparent,
      linearMinHeight: 4,
      borderRadius: BorderRadius.circular(2),
    ),
    switchTheme: SwitchThemeData(
      materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
      // On: a thumb that stands out from the green track. White on the dark
      // theme's light green was 1.6:1; the charcoal is 11:1 (white on the
      // light theme's deep green stays, 5.6:1).
      thumbColor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.disabled)
            ? palette.disabled
            : states.contains(WidgetState.selected)
            ? (palette.dark ? palette.launcher : Colors.white)
            : palette.muted,
      ),
      trackColor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.selected)
            ? palette.green.withValues(
                alpha: states.contains(WidgetState.disabled) ? .4 : 1,
              )
            : palette.quiet,
      ),
      trackOutlineColor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.selected)
            ? Colors.transparent
            : palette.tileLine,
      ),
    ),
    checkboxTheme: CheckboxThemeData(
      fillColor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.selected)
            ? palette.amber
            : Colors.transparent,
      ),
      checkColor: WidgetStatePropertyAll(palette.onAmber),
      side: BorderSide(color: palette.muted, width: 1.5),
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(5)),
    ),
    sliderTheme: SliderThemeData(
      trackHeight: 4,
      activeTrackColor: palette.amber,
      inactiveTrackColor: palette.quiet,
      thumbColor: palette.amber,
      overlayColor: palette.amber.withValues(alpha: .14),
      tickMarkShape: SliderTickMarkShape.noTickMark,
      valueIndicatorColor: palette.dark
          ? const Color(0xFF262D33)
          : const Color(0xFF1F2A31),
      valueIndicatorTextStyle: uiText(size: 12, color: Colors.white),
    ),
    // A grey groove; the chosen segment is the one raised out of it, with
    // the full ink. No amber: amber is the main action, and a choice among
    // equals is not one.
    segmentedButtonTheme: SegmentedButtonThemeData(
      style: ButtonStyle(
        minimumSize: const WidgetStatePropertyAll(Size(0, 38)),
        visualDensity: VisualDensity.compact,
        textStyle: WidgetStatePropertyAll(
          uiText(size: 13, weight: FontWeight.w600),
        ),
        backgroundColor: WidgetStateProperty.resolveWith(
          (states) => states.contains(WidgetState.selected)
              ? palette.segmentOn
              : palette.segmentTrack,
        ),
        foregroundColor: WidgetStateProperty.resolveWith(
          (states) => states.contains(WidgetState.disabled)
              ? palette.disabled
              : states.contains(WidgetState.selected)
              ? palette.ink
              : palette.muted,
        ),
        overlayColor: WidgetStateProperty.resolveWith(
          (states) => states.contains(WidgetState.pressed)
              ? palette.ink.withValues(alpha: .10)
              : states.contains(WidgetState.hovered) ||
                    states.contains(WidgetState.focused)
              ? palette.hover
              : Colors.transparent,
        ),
        // SegmentedButton resolves this once for the whole control (frame
        // and dividers) and calls the control "selected" whenever any
        // segment is chosen. A colour for the selected state therefore
        // frames every segment, chosen or not (an amber frame around all).
        // The one state that does belong to the whole control is keyboard
        // focus: its ring, as on the dialog buttons.
        side: WidgetStateProperty.resolveWith(
          (states) =>
              states.contains(WidgetState.focused) &&
                  FocusManager.instance.highlightMode ==
                      FocusHighlightMode.traditional
              ? BorderSide(color: palette.focus, width: 2)
              : BorderSide(color: palette.tileLine),
        ),
        shape: WidgetStatePropertyAll(
          RoundedRectangleBorder(borderRadius: BorderRadius.circular(10)),
        ),
      ),
    ),
    chipTheme: ChipThemeData(
      backgroundColor: Colors.transparent,
      selectedColor: palette.amberSoft,
      checkmarkColor: palette.amberInk,
      side: BorderSide(color: palette.tileLine),
      labelStyle: uiText(size: 13, weight: FontWeight.w600, color: palette.ink),
      padding: const EdgeInsets.symmetric(horizontal: 6),
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(10)),
    ),
    scrollbarTheme: ScrollbarThemeData(
      thickness: const WidgetStatePropertyAll(6),
      radius: const Radius.circular(6),
      thumbColor: WidgetStatePropertyAll(palette.muted.withValues(alpha: .38)),
    ),
    colorScheme: ColorScheme.fromSeed(
      brightness: brightness,
      seedColor: palette.amber,
      primary: palette.amber,
      onPrimary: const Color(0xFF1E1503),
      surface: palette.surface,
      onSurface: palette.ink,
      outline: palette.line,
    ),
    textTheme: TextTheme(
      headlineLarge: TextStyle(
        fontSize: 32,
        fontWeight: FontWeight.w600,
        letterSpacing: -0.3,
        color: palette.ink,
      ),
      headlineMedium: TextStyle(
        fontSize: 26,
        fontWeight: FontWeight.w600,
        letterSpacing: -0.25,
        color: palette.ink,
      ),
      titleLarge: TextStyle(
        fontSize: 20,
        fontWeight: FontWeight.w600,
        letterSpacing: -0.2,
        color: palette.ink,
      ),
      titleMedium: TextStyle(
        fontSize: 15,
        fontWeight: FontWeight.w600,
        color: palette.ink,
      ),
      bodyLarge: TextStyle(fontSize: 14, height: 1.6, color: palette.ink),
      bodyMedium: TextStyle(fontSize: 13, height: 1.55, color: palette.ink),
      bodySmall: TextStyle(fontSize: 12, height: 1.5, color: palette.muted),
      labelLarge: const TextStyle(fontSize: 13, fontWeight: FontWeight.w600),
    ),
    // Fields: a filled well, a hairline edge and an amber focus ring, with the
    // 12 px corners of the dialog buttons.
    inputDecorationTheme: InputDecorationTheme(
      filled: true,
      fillColor: palette.tileWell,
      isDense: true,
      contentPadding: const EdgeInsets.symmetric(horizontal: 14, vertical: 14),
      hintStyle: uiText(size: 13, color: palette.muted),
      labelStyle: uiText(size: 13, color: palette.muted),
      // Material draws a floating label at 75 %: this reads as 11.25 px.
      floatingLabelStyle: uiText(size: 15, color: palette.muted),
      errorStyle: uiText(size: 12, color: palette.red),
      border: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: BorderSide(color: palette.tileLine),
      ),
      enabledBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: BorderSide(color: palette.tileLine),
      ),
      disabledBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: BorderSide(color: palette.tileLine.withValues(alpha: .6)),
      ),
      // Amber on the dark theme; on white it is 2.0:1, so the light theme
      // uses the deep amber (6.6:1).
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: BorderSide(color: palette.focus, width: 1.5),
      ),
    ),
    // One button geometry everywhere (see AppButtons): 42 px, 12 px corners.
    filledButtonTheme: FilledButtonThemeData(
      style: FilledButton.styleFrom(
        backgroundColor: palette.amber,
        foregroundColor: palette.onAmber,
        disabledBackgroundColor: palette.quiet,
        disabledForegroundColor: palette.disabled,
        minimumSize: const Size(0, 42),
        padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 12),
        textStyle: uiText(size: 14, weight: FontWeight.w600),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      ),
    ),
    outlinedButtonTheme: OutlinedButtonThemeData(
      style: OutlinedButton.styleFrom(
        foregroundColor: palette.ink,
        disabledForegroundColor: palette.disabled,
        side: BorderSide(color: palette.tileLine),

        minimumSize: const Size(0, 42),
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
        textStyle: uiText(size: 14, weight: FontWeight.w600),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
      ),
    ),
    textButtonTheme: TextButtonThemeData(
      style: TextButton.styleFrom(
        foregroundColor: palette.amberInk,
        textStyle: uiText(size: 13.5, weight: FontWeight.w600),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(10)),
      ),
    ),
    dividerTheme: DividerThemeData(color: palette.line, space: 1),
    tooltipTheme: const TooltipThemeData(
      waitDuration: Duration(milliseconds: 450),
    ),
  );
}

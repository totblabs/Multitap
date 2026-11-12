import 'package:flutter/material.dart';

import '../data/appearance_settings.dart';
import 'app_menu.dart';
import 'app_theme.dart';
import 'app_toast.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// Applies and remembers a theme; a failed save says so instead of silently
/// reverting.
Future<void> applyThemeChoice(
  BuildContext context,
  AppAppearance appearance,
  ThemeMode value,
) async {
  try {
    await appearance.select(value);
  } catch (_) {
    if (context.mounted) {
      showAppToast(
        context,
        l10n.couldntSaveThemeChooseAgain,
        kind: AppToastKind.failed,
      );
    }
  }
}

/// Applies and remembers a background scene.
Future<void> applySceneChoice(
  BuildContext context,
  AppAppearance appearance,
  BackgroundScene value,
) async {
  try {
    await appearance.selectScene(value);
  } catch (_) {
    if (context.mounted) {
      showAppToast(
        context,
        l10n.couldntSaveBackgroundChooseAgain,
        kind: AppToastKind.failed,
      );
    }
  }
}

/// "언어 / Language" (docs/i18n.md): applies at once everywhere in this
/// window and is remembered with the appearance; a failed save says so.
Future<void> applyLanguageChoice(
  BuildContext context,
  AppLanguage value,
) async {
  try {
    await AppLanguageController.instance.select(value);
  } catch (_) {
    if (context.mounted) {
      showAppToast(context, l10n.languageSaveFailed, kind: AppToastKind.failed);
    }
  }
}

/// A language choice's name: 자동 in the app's language, the others in
/// their own language so either can be found from the other.
String languageChoiceLabel(AppLanguage language) => switch (language) {
  AppLanguage.auto => l10n.auto,
  AppLanguage.ko => l10n.languageNameKorean,
  AppLanguage.en => l10n.languageNameEnglish,
};

/// Its own header button for 언어 / Language: inside the theme menu alone it
/// is not found.
class LanguageSwitch extends StatelessWidget {
  const LanguageSwitch({super.key, this.iconColor});
  final Color? iconColor;

  @override
  Widget build(BuildContext context) {
    final palette = AppPalette.of(context);
    return PopupMenuButton<AppLanguage>(
      key: const Key('language-menu'),
      tooltip: l10n.language,
      color: palette.surface,
      icon: Icon(Icons.translate_rounded, color: iconColor ?? palette.muted, size: 20),
      onSelected: (value) => applyLanguageChoice(context, value),
      itemBuilder: (_) => [
        for (final language in AppLanguage.values)
          AppMenu.item<AppLanguage>(
            palette,
            key: Key('language-${language.name}'),
            value: language,
            icon: Icons.translate_rounded,
            color: palette.muted,
            label: languageChoiceLabel(language),
            trailing: AppLanguageController.instance.choice == language
                ? AppMenu.check(palette)
                : const SizedBox(width: 15),
          ),
      ],
    );
  }
}

class AppearanceSwitch extends StatelessWidget {
  const AppearanceSwitch({
    super.key,
    this.compact = false,
    this.iconColor,
    this.scenes = true,
    this.languages = true,
  });
  final bool compact;

  /// Offers the language choice too (a screen with its own [LanguageSwitch]
  /// leaves it out).
  final bool languages;

  /// Offers the scenic background choice (screens without the scene omit it).
  final bool scenes;

  /// The compact button's icon colour (amber by default); a grouped header
  /// passes its neutral icon colour.
  final Color? iconColor;

  @override
  Widget build(BuildContext context) {
    final appearance = AppearanceScope.maybeOf(context);
    if (appearance == null) return const SizedBox.shrink();
    final palette = AppPalette.of(context);
    Future<void> select(ThemeMode value) =>
        applyThemeChoice(context, appearance, value);
    Future<void> selectScene(BackgroundScene value) =>
        applySceneChoice(context, appearance, value);

    if (compact) {
      // Theme (day/night of the scene) and the scene itself.
      return PopupMenuButton<Object>(
        key: const Key('appearance-menu'),
        tooltip: scenes ? l10n.themeBackground : l10n.theme2,
        enabled: !appearance.saving,
        color: palette.surface,
        icon: Icon(
          appearance.mode == ThemeMode.dark
              ? Icons.dark_mode_outlined
              : Icons.light_mode_outlined,
          color: iconColor ?? palette.amber,
          size: 20,
        ),
        onSelected: (value) => value is ThemeMode
            ? select(value)
            : value is AppLanguage
            ? applyLanguageChoice(context, value)
            : selectScene(value as BackgroundScene),
        itemBuilder: (_) => [
          for (final mode in [ThemeMode.dark, ThemeMode.light])
            AppMenu.item<Object>(
              palette,
              key: Key('theme-${mode.name}'),
              value: mode,
              icon: mode == ThemeMode.dark
                  ? Icons.dark_mode_rounded
                  : Icons.light_mode_rounded,
              color: palette.amber,
              label: mode == ThemeMode.dark ? l10n.dark : l10n.light,
              trailing: appearance.mode == mode
                  ? AppMenu.check(palette)
                  : const SizedBox(width: 15),
            ),
          if (scenes) const PopupMenuDivider(height: 9),
          if (scenes)
            for (final scene in BackgroundScene.values)
              AppMenu.item<Object>(
                palette,
                key: Key('scene-${scene.name}'),
                value: scene,
                icon: _sceneIcon(scene),
                color: palette.blue,
                label: l10n.background2(scene.label),
                trailing: appearance.scene == scene
                    ? AppMenu.check(palette)
                    : const SizedBox(width: 15),
              ),
          if (languages) const PopupMenuDivider(height: 9),
          if (languages)
          for (final language in AppLanguage.values)
            AppMenu.item<Object>(
              palette,
              key: Key('language-${language.name}'),
              value: language,
              icon: Icons.translate_rounded,
              color: palette.muted,
              label: language == AppLanguage.auto
                  ? '${l10n.language} · ${languageChoiceLabel(language)}'
                  : languageChoiceLabel(language),
              trailing: AppLanguageController.instance.choice == language
                  ? AppMenu.check(palette)
                  : const SizedBox(width: 15),
            ),
        ],
      );
    }
    return SegmentedButton<ThemeMode>(
      key: const Key('appearance-selection'),
      segments: [
        ButtonSegment(
          value: ThemeMode.dark,
          icon: Icon(Icons.dark_mode_rounded, size: 18),
          label: KeepAllText(l10n.dark),
        ),
        ButtonSegment(
          value: ThemeMode.light,
          icon: Icon(Icons.light_mode_rounded, size: 18),
          label: KeepAllText(l10n.light),
        ),
      ],
      selected: {appearance.mode},
      onSelectionChanged: appearance.saving
          ? null
          : (values) => select(values.single),
    );
  }
}

IconData _sceneIcon(BackgroundScene scene) => switch (scene) {
  BackgroundScene.peaks => Icons.terrain_rounded,
  BackgroundScene.meadow => Icons.landscape_rounded,
};

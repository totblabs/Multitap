import 'dart:ui' show Locale, PlatformDispatcher;

import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart';
import 'package:intl/intl.dart' show DateFormat;

import 'app_localizations.dart';

export 'app_localizations.dart';

/// The "언어 / Language" app setting (docs/i18n.md). [auto] follows the
/// system: Korean when its locale starts with `ko`, English otherwise.
enum AppLanguage {
  auto,
  ko,
  en;

  static AppLanguage parse(Object? value) => AppLanguage.values.firstWhere(
    (language) => language.name == value,
    orElse: () => AppLanguage.auto,
  );
}

/// The chosen language for the whole app. One per process: the Windows
/// client, each remote session window, the Host settings app and the Android
/// app each read the stored choice when they start.
class AppLanguageController extends ChangeNotifier {
  AppLanguageController._();
  static final instance = AppLanguageController._();

  AppLanguage _choice = AppLanguage.auto;
  Future<void> Function(AppLanguage)? _save;

  /// For tests: the language "auto" resolves to instead of the platform's.
  @visibleForTesting
  static String? debugSystemLanguage;

  AppLanguage get choice => _choice;

  /// `ko` or `en`.
  String get code => switch (_choice) {
    AppLanguage.ko => 'ko',
    AppLanguage.en => 'en',
    AppLanguage.auto => resolveSystemLanguage(
      debugSystemLanguage ?? PlatformDispatcher.instance.locale.languageCode,
    ),
  };

  Locale get locale => Locale(code);

  /// The stored choice at start-up; [save] keeps later changes.
  void configure(
    AppLanguage choice, {
    Future<void> Function(AppLanguage)? save,
  }) {
    _save = save;
    if (choice == _choice) return;
    _choice = choice;
    notifyListeners();
  }

  /// The setting changed: applies at once, then is stored.
  Future<void> select(AppLanguage choice) async {
    if (choice == _choice) return;
    _choice = choice;
    notifyListeners();
    await _save?.call(choice);
  }

  /// The system locale changed (only matters for [AppLanguage.auto]).
  void systemLocaleChanged() {
    if (_choice == AppLanguage.auto) notifyListeners();
  }

  @visibleForTesting
  void debugReset([AppLanguage choice = AppLanguage.auto]) {
    _choice = choice;
    _save = null;
  }
}

/// Korean when [languageCode] starts with `ko`, English otherwise.
String resolveSystemLanguage(String? languageCode) =>
    (languageCode ?? '').toLowerCase().startsWith('ko') ? 'ko' : 'en';

final _cache = <String, AppLocalizations>{};

/// The app's strings in the chosen language, usable anywhere (data layer
/// included). Widgets rebuild when the language changes (ThemedApp).
AppLocalizations get l10n {
  final code = AppLanguageController.instance.code;
  return _cache[code] ??= lookupAppLocalizations(Locale(code));
}

/// "11월 12일" / "Nov 12" (docs/i18n.md: dates by locale).
String formatMonthDay(DateTime local) =>
    AppLanguageController.instance.code == 'ko'
    ? l10n.text10(local.month, local.day)
    : DateFormat.MMMd('en_US').format(local);

/// "6:05 PM" for English; the Korean "오후 6:05" is built where it is shown.
String formatClock(DateTime local) => DateFormat.jm('en_US').format(local);

/// The value of `Accept-Language` sent to the Multitap server, so the few
/// human sentences it sends come in the same language.
String get acceptLanguage => AppLanguageController.instance.code == 'ko'
    ? 'ko-KR,ko;q=0.9,en;q=0.5'
    : 'en';

/// Marks every element below [context] for rebuild, so text read through
/// [l10n] (not through an inherited widget) follows a language change
/// without losing any state.
void rebuildAllText(BuildContext context) {
  void rebuild(Element element) {
    element.markNeedsBuild();
    element.visitChildren(rebuild);
  }

  (context as Element).visitChildren(rebuild);
}

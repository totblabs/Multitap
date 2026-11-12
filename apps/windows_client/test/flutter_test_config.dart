import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/l10n/l10n.dart';

/// Every test runs in Korean, the source text the existing tests pin,
/// unless it switches the language itself (test/i18n/*). The test platform
/// reports en-US, so "auto" is pointed at Korean here and reset after each
/// test.
Future<void> testExecutable(FutureOr<void> Function() testMain) async {
  AppLanguageController.debugSystemLanguage = 'ko';
  setUp(() {
    AppLanguageController.debugSystemLanguage = 'ko';
    AppLanguageController.instance.debugReset();
  });
  await testMain();
}

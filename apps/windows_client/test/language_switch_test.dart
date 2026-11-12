import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/l10n/l10n.dart';
import 'package:multitap_client/ui/appearance_switch.dart';

// The language is not offered only inside the theme menu: LanguageSwitch is
// its own header button with the three choices.
void main() {
  testWidgets('the language button lists 자동, 한국어, English and applies a choice', (
    tester,
  ) async {
    await tester.pumpWidget(
      const MaterialApp(
        home: Scaffold(body: Center(child: LanguageSwitch())),
      ),
    );
    expect(find.byKey(const Key('language-menu')), findsOneWidget);
    await tester.tap(find.byKey(const Key('language-menu')));
    await tester.pumpAndSettle();
    for (final language in AppLanguage.values) {
      expect(find.byKey(Key('language-${language.name}')), findsOneWidget);
    }
    expect(find.text('한국어'), findsOneWidget);
    expect(find.text('English'), findsOneWidget);
    await tester.tap(find.byKey(const Key('language-en')));
    await tester.pumpAndSettle();
    expect(AppLanguageController.instance.choice, AppLanguage.en);
    expect(tester.takeException(), isNull);
  });
}

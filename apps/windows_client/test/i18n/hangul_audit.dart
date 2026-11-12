import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

final _hangul = RegExp(r'[가-힣ᄀ-ᇿ㄰-㆏]');

/// Words that stay Korean in the English app on purpose: the language
/// choice names itself in its own language.
const languageNames = {'한국어'};

/// Every on-screen text (Text/RichText, tooltips) that still holds Hangul
/// once the [allow]ed data values (desktop names, server text) and
/// [languageNames] are taken out of it.
List<String> hangulOnScreen(
  WidgetTester tester, {
  Set<String> allow = const {},
}) {
  final found = <String>{};
  void check(String? text) {
    if (text == null) return;
    var rest = text.replaceAll('\u2060', '');
    for (final value in [...allow, ...languageNames]) {
      rest = rest.replaceAll(value, '');
    }
    if (_hangul.hasMatch(rest)) found.add(text.replaceAll('\u2060', ''));
  }

  for (final element in find.byType(RichText).evaluate()) {
    check((element.widget as RichText).text.toPlainText());
  }
  for (final element in find.byType(Tooltip).evaluate()) {
    final tooltip = element.widget as Tooltip;
    check(tooltip.message ?? tooltip.richMessage?.toPlainText());
  }
  return found.toList()..sort();
}

/// Fails with the Korean texts found on screen.
void expectNoHangul(
  WidgetTester tester,
  String where, {
  Set<String> allow = const {},
}) {
  expect(hangulOnScreen(tester, allow: allow), isEmpty, reason: where);
}

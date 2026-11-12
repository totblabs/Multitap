import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/korean_text.dart';
import 'package:multitap_client/ui/release_notes.dart';

const _notes = '# 주요 변경\n- 연결 준비 안내\n  - 작은 화면 조정\n\n안내:\n설치 후 다시 열어 주세요.';
const _lines = ['주요 변경', '연결 준비 안내', '작은 화면 조정', '안내:', '설치 후 다시 열어 주세요.'];

Future<void> _show(WidgetTester tester, Widget child) => tester.pumpWidget(
  MaterialApp(
    theme: buildAppTheme(brightness: Brightness.dark),
    home: Scaffold(body: SizedBox(width: 220, child: child)),
  ),
);

void main() {
  testWidgets('default release notes retain ordinary Text and the explicit '
      'false layout', (tester) async {
    await _show(tester, const ChangelogText(_notes));
    expect(find.byType(KeepAllText), findsNothing);
    final before = [for (final line in _lines) tester.getRect(find.text(line))];
    final bulletRows = find.descendant(
      of: find.byType(ChangelogText),
      matching: find.byType(Row),
    );
    expect(bulletRows, findsNWidgets(2));
    await _show(tester, const ChangelogText(_notes, keepWords: false));
    expect(find.byType(KeepAllText), findsNothing);
    expect([
      for (final line in _lines) tester.getRect(find.text(line)),
    ], before);
  });

  testWidgets('opt-in keeps heading, nested bullets and paragraph source '
      'text while rendering Korean word joiners', (tester) async {
    final semantics = tester.ensureSemantics();
    try {
      await _show(tester, const ChangelogText(_notes, keepWords: true));
      expect(find.byType(KeepAllText), findsNWidgets(_lines.length));
      for (final line in _lines) {
        final text = find.text(line);
        expect(text, findsOneWidget);
        expect(tester.widget<KeepAllText>(text).data, line);
        final rich = find.descendant(of: text, matching: find.byType(RichText));
        expect(
          tester.widget<RichText>(rich).text.toPlainText(),
          contains('\u2060'),
        );
        expect(find.bySemanticsLabel(line), findsOneWidget);
      }
      expect(
        find.descendant(
          of: find.byType(ChangelogText),
          matching: find.byType(Row),
        ),
        findsNWidgets(2),
      );
      expect(tester.takeException(), isNull);
    } finally {
      semantics.dispose();
    }
  });
}

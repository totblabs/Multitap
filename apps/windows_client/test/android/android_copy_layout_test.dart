import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_launcher.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/ui/app_theme.dart';

import 'android_launcher_test.dart' show MemoryStore, signedIn, vm;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    final font = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      font.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await font.load();
  });
  tearDown(
    () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null),
  );

  testWidgets('a changed password reads as done on the sign-in card, a failure as a failure', (
    tester,
  ) async {
    for (final done in [true, false]) {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(
            body: AndroidSignInView(
              origin: Uri.parse('https://vdi.example'),
              id: TextEditingController(),
              password: TextEditingController(),
              remember: false,
              busy: false,
              version: null,
              error: done ? '비밀번호를 바꿨어요. 새 비밀번호로 로그인해 주세요.' : '로그인하지 못했어요.',
              errorIsDone: done,
              onServer: () {},
              onIdChanged: (_) {},
              onRemember: (_) {},
              onLogin: () {},
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byIcon(Icons.check_circle_outline_rounded), done ? findsOneWidget : findsNothing);
      expect(find.byIcon(Icons.error_outline_rounded), done ? findsNothing : findsOneWidget);
    }
  });

  for (final size in [const Size(320, 720), const Size(800, 390)]) {
    testWidgets('sign-in action and footer stay readable at $size, text 200%', (
      tester,
    ) async {
      tester.view.physicalSize = size;
      tester.view.devicePixelRatio = 1;
      tester.platformDispatcher.textScaleFactorTestValue = 2;
      addTearDown(tester.view.reset);
      addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
      final id = TextEditingController(), password = TextEditingController();
      var configured = 0;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(
            body: AndroidSignInView(
              origin: null,
              id: id,
              password: password,
              remember: false,
              busy: false,
              version: '0.1.0-preview.999',
              onServer: () => configured++,
              onIdChanged: (_) {},
              onRemember: (_) {},
              onLogin: () {},
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final action = find.text('서버 설정');
      await tester.ensureVisible(action);
      await tester.pumpAndSettle();
      final paragraph = tester.renderObject<RenderParagraph>(
        find.descendant(of: action, matching: find.byType(RichText)).first,
      );
      final single = TextPainter(
        text: paragraph.text,
        textScaler: paragraph.textScaler,
        textDirection: TextDirection.ltr,
      )..layout();
      expect(paragraph.size.height, lessThanOrEqualTo(single.height + .5));
      single.dispose();
      await tester.tap(action);
      expect(configured, 1);
      final version = find.byKey(const Key('login-version'));
      await tester.ensureVisible(version);
      await tester.pumpAndSettle();
      expect(find.text('0.1.0-preview.999'), findsOneWidget);
      for (final text in [version, find.text('Android · 스트리밍')]) {
        final render = tester.renderObject<RenderParagraph>(
          find.descendant(of: text, matching: find.byType(RichText)).first,
        );
        expect(render.didExceedMaxLines, isFalse);
      }
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
      id.dispose();
      password.dispose();
    });
  }

  testWidgets(
    'card titles and VM management preserve Korean words at text 200%',
    (tester) async {
      tester.view.physicalSize = const Size(360, 800);
      tester.view.devicePixelRatio = 1;
      tester.platformDispatcher.textScaleFactorTestValue = 2;
      addTearDown(tester.view.reset);
      addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
      await signedIn(
        tester,
        [vm(name: '작업 데스크톱'), vm(id: 'pve-150', name: '개발 환경')],
        store: MemoryStore(
          origin: 'https://vdi.example',
          username: 'tester',
          prefs: {'launcher.view': 'docking'},
        ),
      );
      for (final (id, name, first, last) in [
        ('pve-149', '작업 데스크톱', '데', '톱'),
        ('pve-150', '개발 환경', '환', '경'),
      ]) {
        final label = find.byKey(Key('desktop-name-$id'));
        await tester.ensureVisible(label);
        await tester.pumpAndSettle();
        expect(tester.widget<Text>(label).data, name);
        final paragraph = tester.renderObject<RenderParagraph>(
          find.descendant(of: label, matching: find.byType(RichText)).first,
        );
        final text = paragraph.text.toPlainText();
        final boxes = paragraph.getBoxesForSelection(
          TextSelection(
            baseOffset: text.indexOf(first),
            extentOffset: text.indexOf(last) + 1,
          ),
        );
        expect(boxes, isNotEmpty);
        expect(
          boxes.map((b) => b.top).toSet(),
          hasLength(1),
          reason: '$name must preserve its Korean word on a single line',
        );
      }
      final more = find.byKey(const Key('desktop-more-pve-149'));
      await tester.ensureVisible(more);
      await tester.pumpAndSettle();
      expect(more.hitTestable(), findsOneWidget);
      await tester.tap(more);
      await tester.pumpAndSettle();
      final details = find.byKey(const Key('details-pve-149'));
      expect(details.hitTestable(), findsOneWidget);
      final actionLabel = find.descendant(
        of: details,
        matching: find.text('연결 준비 점검'),
      );
      expect(actionLabel, findsOneWidget);
      final paragraph = tester.renderObject<RenderParagraph>(
        find.descendant(of: actionLabel, matching: find.byType(RichText)).first,
      );
      final text = paragraph.text.toPlainText();
      for (final word in ['연결', '준비', '점검']) {
        final start = text.indexOf(word[0]);
        expect(start, greaterThanOrEqualTo(0), reason: word);
        final end = text.indexOf(word[word.length - 1], start) + 1;
        final boxes = paragraph.getBoxesForSelection(
          TextSelection(baseOffset: start, extentOffset: end),
        );
        expect(boxes, isNotEmpty, reason: word);
        expect(
          boxes.map((box) => box.top).toSet(),
          hasLength(1),
          reason: 'management action preserves $word on one line',
        );
      }
      await tester.tap(details);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('readiness-verdict')), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );
}

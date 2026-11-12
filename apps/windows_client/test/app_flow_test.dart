import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';

void main() {
  Future<void> account(WidgetTester tester, String item) async {
    await tester.tap(find.byKey(const Key('workspace-account')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(Key(item)));
  }

  Future<void> confirmWorkspaceSignOut(WidgetTester tester) async {
    await account(tester, 'sign-out');
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('confirm-sign-out')));
    await tester.pumpAndSettle();
  }

  Future<void> start(
    WidgetTester tester, {
    Size size = const Size(1440, 900),
  }) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = size;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MultitapApp(
        enableDemo: true,
        serverOrigin: Uri.parse('https://cloud.example.com'),
      ),
    );
    await tester.pumpAndSettle();
  }

  Future<void> preview(WidgetTester tester) async {
    await tester.ensureVisible(find.byKey(const Key('open-preview')));
    await tester.tap(find.byKey(const Key('open-preview')));
    await tester.pumpAndSettle();
  }

  testWidgets(
    'App starts at login and preview is explicitly separate from authentication',
    (tester) async {
      await start(tester);
      expect(find.byKey(const Key('card-sample-office')), findsNothing);
      expect(find.byKey(const Key('login-submit')), findsOneWidget);
      await tester.enterText(
        find.byKey(const Key('sign-in-id')),
        'preview-only',
      );
      await tester.enterText(
        find.byKey(const Key('sign-in-password')),
        'not-a-real-password',
      );
      await preview(tester);
      expect(find.text('데모 · 서버 미연결'), findsOneWidget);
      expect(find.text('예시'), findsNWidgets(2));
      expect(
        find.descendant(
          of: find.byKey(const Key('card-sample-lab')),
          matching: find.text('꺼짐'),
        ),
        findsOneWidget,
      );
      expect(find.text('예시 할당 목록 · 실제 연결 전'), findsOneWidget);
      expect(find.text('연결 가능'), findsNothing);
      expect(find.text('연결하기'), findsNothing);
      expect(find.byKey(const Key('card-sample-office')), findsOneWidget);
      expect(find.text('추가'), findsNothing);
      expect(find.text('편집'), findsNothing);
      expect(find.text('삭제'), findsNothing);
      expect(find.byType(TextField), findsNothing);
      await confirmWorkspaceSignOut(tester);
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        isEmpty,
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        isEmpty,
      );
    },
  );

  testWidgets(
    'Client info shows server-managed desktops and exposes no editable settings',
    (tester) async {
      await start(tester);
      await preview(tester);
      await account(tester, 'open-settings');
      await tester.pumpAndSettle();
      // Multitap / 서버 / 오픈소스 라이선스 sections; the
      // preview's 서버 section says it is not connected to a real server.
      expect(find.byKey(const Key('app-info-facts')), findsOneWidget);
      await tester.tap(find.text('서버').last);
      await tester.pumpAndSettle();
      expect(find.text('데모 모드'), findsOneWidget);
      expect(find.byType(TextField), findsNothing);
      expect(find.byType(TextFormField), findsNothing);
      await tester.tap(find.byKey(const Key('close-settings')));
      await tester.pumpAndSettle();
      expect(find.text('내 데스크톱'), findsOneWidget);
    },
  );

  testWidgets(
    'Session preview reports disconnected state and no fabricated metrics',
    (tester) async {
      await start(tester);
      await preview(tester);
      expect(
        tester
            .widget<FilledButton>(find.byKey(const Key('start-sample-lab')))
            .onPressed,
        isNull,
      );
      await tester.tap(find.byKey(const Key('open-sample-office')));
      await tester.pumpAndSettle();
      expect(find.text('연결되지 않음'), findsOneWidget);
      expect(find.text('비활성'), findsOneWidget);
      expect(find.text('—'), findsNWidgets(2));
      await tester.tap(find.byKey(const Key('close-session-preview')));
      await tester.pumpAndSettle();
      expect(find.text('내 데스크톱'), findsOneWidget);
    },
  );

  testWidgets(
    'Compact Windows layout supports login, assigned desktops and client info',
    (tester) async {
      await start(tester, size: const Size(900, 600));
      expect(tester.takeException(), isNull);
      await preview(tester);
      expect(tester.takeException(), isNull);
      await account(tester, 'open-settings');
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
      await tester.tap(find.byKey(const Key('close-settings')));
      await tester.pumpAndSettle();
      await confirmWorkspaceSignOut(tester);
      expect(tester.takeException(), isNull);
    },
  );
}

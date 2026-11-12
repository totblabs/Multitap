import 'dart:io';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/session_activity.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/app_info_dialog.dart';
import 'package:multitap_client/ui/desktop_organizer.dart';
import 'package:multitap_client/ui/launcher_view_menu.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';
import 'package:multitap_client/ui/workspace_header.dart';

import 'auth_flow_test.dart' show TestLoginMemory;

Future<void> _fonts(WidgetTester tester) => tester.runAsync(() async {
  final loader = FontLoader('Pretendard');
  for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
    loader.addFont(
      Future.value(
        ByteData.sublistView(
          await File('assets/fonts/Pretendard-$weight.otf').readAsBytes(),
        ),
      ),
    );
  }
  await loader.load();
});

Finder _key(String value) => find.byKey(Key(value));

Future<void> _mount(
  WidgetTester tester,
  Widget child, {
  required Size size,
  double scale = 1,
  Brightness brightness = Brightness.dark,
}) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = size;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: brightness),
      builder: (_, child) => MediaQuery(
        data: MediaQueryData(size: size, textScaler: TextScaler.linear(scale)),
        child: child!,
      ),
      home: Scaffold(body: child),
    ),
  );
  await tester.pump();
}

void main() {
  for (final size in [
    const Size(1280, 800),
    const Size(760, 640),
    const Size(360, 640),
  ]) {
    for (final scale in [1.0, 2.0]) {
      testWidgets(
        'header actions reach the right edge with real idle $size $scale',
        (tester) async {
          await _fonts(tester);
          var now = DateTime.utc(2026, 11, 6);
          final activity = SessionActivity(clock: () => now);
          SessionAccess policy(bool enabled) => SessionAccess(
            serverTime: now,
            idleDeadline: enabled ? now.add(const Duration(minutes: 30)) : null,
            idleTimeout: const Duration(minutes: 30),
            warning: const Duration(minutes: 1),
            idleEnabled: enabled,
          );
          activity.start(
            policy(true),
            record: () async => policy(true),
            onTerminal: (_) {},
            setIdle: (value) async => policy(value),
          );
          addTearDown(activity.dispose);
          var updates = 0;
          await _mount(
            tester,
            Column(
              children: [
                WorkspaceHeader(
                  side: 16,
                  columnWidth: 1400,
                  isDemo: false,
                  accountLabel: '아주 긴 사용자 이름이 있는 테스트 계정',
                  server: 'synthetic.example.com',
                  version: '1.0.19',
                  update: '1.0.19',
                  hostUpdates: 5,
                  sessionAccess: activity,
                  onUpdates: () => updates++,
                  onClientInfo: () {},
                  onLog: () {},
                  onChangePassword: () {},
                  onSignOut: () {},
                ),
              ],
            ),
            size: size,
            scale: scale,
          );
          expect(tester.takeException(), isNull);
          final account = tester.getRect(_key('workspace-account'));
          final update = tester.getRect(_key('check-client-updates'));
          final idle = tester.getRect(_key('session-idle-badge'));
          expect(
            account.right,
            closeTo(size.width - 16, .1),
            reason: 'No unused trailing flex allocation',
          );
          expect(update.right, lessThanOrEqualTo(idle.left));
          expect(idle.right, lessThanOrEqualTo(account.left));
          expect((account.center.dy - update.center.dy).abs(), lessThan(.1));
          expect(account.height, greaterThanOrEqualTo(36));
          expect(update.height, greaterThanOrEqualTo(36));
          final paragraph = tester.renderObject<RenderParagraph>(
            find.descendant(
              of: find.text('30:00'),
              matching: find.byType(RichText),
            ),
          );
          expect(paragraph.didExceedMaxLines, isFalse);
          final textBoxes = paragraph.getBoxesForSelection(
            TextSelection(
              baseOffset: 0,
              extentOffset: paragraph.text.toPlainText().length,
            ),
          );
          expect(textBoxes.map((box) => box.top).toSet(), hasLength(1));
          final beforeWidth = idle.width;
          now = now.add(const Duration(seconds: 1));
          await tester.pump(const Duration(seconds: 1));
          expect(
            tester.getRect(_key('session-idle-badge')).width,
            closeTo(beforeWidth, .1),
          );
          expect(find.text('29:59'), findsOneWidget);
          await tester.tap(_key('check-client-updates'));
          await tester.pump();
          expect(updates, 1);
          await activity.setIdleEnabled(false);
          await tester.pump();
          expect(find.text('꺼짐'), findsOneWidget);
          expect(
            tester.getRect(_key('workspace-account')).right,
            closeTo(size.width - 16, .1),
          );
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox.shrink());
        },
      );
    }
  }

  for (final brightness in Brightness.values) {
    testWidgets(
      'server chooser scrolls all actions at 760 and 200% $brightness',
      (tester) async {
        await _fonts(tester);
        await _mount(
          tester,
          SignInScreen(
            onLogin: (_, _) async => SignInResult.rejected,
            serverOrigin: Uri.parse('https://cloud.example.com'),
            onServerChanged: (_) async {},
            loginMemory: TestLoginMemory(),
          ),
          size: const Size(760, 640),
          scale: 2,
          brightness: brightness,
        );
        await tester.pumpAndSettle();
        await tester.ensureVisible(_key('open-server-settings'));
        await tester.tap(_key('open-server-settings'));
        await tester.pumpAndSettle();
        expect(tester.takeException(), isNull);
        expect(_key('broker-scheme-https'), findsOneWidget);
        expect(_key('broker-scheme-http'), findsOneWidget);
        await tester.ensureVisible(_key('cancel-server-settings'));
        await tester.pumpAndSettle();
        expect(_key('cancel-server-settings').hitTestable(), findsOneWidget);
        expect(
          tester.getRect(_key('cancel-server-settings')).bottom,
          lessThanOrEqualTo(640),
        );
        await tester.tap(_key('cancel-server-settings'));
        await tester.pumpAndSettle();
        expect(_key('sign-in-id'), findsOneWidget);
        expect(_key('remember-password'), findsOneWidget);
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'compact preview and refresh retain a visible keyboard focus border',
    (tester) async {
      await _fonts(tester);
      final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
      await mouse.addPointer(location: const Offset(1, 1));
      addTearDown(mouse.removePointer);
      for (final touch in [false, true]) {
        await _mount(
          tester,
          Row(
            children: [
              LauncherHeaderButton(
                key: const Key('refresh-desktops'),
                icon: Icons.refresh,
                tooltip: '새로고침',
                onPressed: () {},
                compactChrome: true,
                height: touch ? 48 : 36,
              ),
              LauncherViewMenu(
                controller: MenuController(),
                view: LauncherView.cards,
                showPreviews: true,
                onSelected: (_) {},
                onPreviews: (_) {},
                compactChrome: true,
              ),
            ],
          ),
          size: const Size(760, 640),
        );
        for (final target in ['refresh-desktops', 'desktop-previews']) {
          await tester.sendKeyEvent(LogicalKeyboardKey.tab);
          await tester.pump();
          final material = tester.widget<Material>(
            find
                .descendant(of: _key(target), matching: find.byType(Material))
                .first,
          );
          final shape = material.shape! as RoundedRectangleBorder;
          expect(
            shape.side.style,
            BorderStyle.solid,
            reason: '$target keeps focus for compact Chrome, touch=$touch',
          );
          expect(shape.side.width, greaterThanOrEqualTo(1.5));
        }
        await tester.pumpWidget(const SizedBox.shrink());
      }
    },
  );

  for (final scale in [1.0, 2.0]) {
    testWidgets(
      'login information footer has a real target and opens information at $scale',
      (tester) async {
        await _fonts(tester);
        await _mount(
          tester,
          SignInScreen(
            onLogin: (_, _) async => SignInResult.rejected,
            serverOrigin: Uri.parse('https://cloud.example.com'),
            onServerChanged: (_) async {},
            loginMemory: TestLoginMemory(),
          ),
          size: const Size(1280, 800),
          scale: scale,
        );
        await tester.pumpAndSettle();
        final footer = _key('login-app-info');
        await tester.ensureVisible(footer);
        await tester.pumpAndSettle();
        expect(footer.hitTestable(), findsOneWidget);
        expect(tester.getSize(footer).height, greaterThanOrEqualTo(32));
        expect(tester.getRect(footer).right, lessThanOrEqualTo(1280));
        expect(tester.getRect(footer).left, greaterThanOrEqualTo(0));
        await tester.tap(footer);
        await tester.pumpAndSettle();
        expect(find.byType(AppInfoDialog), findsOneWidget);
        expect(tester.takeException(), isNull);
      },
    );
  }
}

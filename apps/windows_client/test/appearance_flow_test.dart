import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';
import 'package:multitap_client/ui/workspace_shell.dart';
import 'package:multitap_client/ui/components.dart';

import 'auth_flow_test.dart' show TestBroker, TestLoginMemory, assignedVm;
import 'remote_session_test.dart' show SessionSocket, info, packet;
import 'session_recovery_test.dart'
    show RecoveryMedia, machine, mockPresentation, selectedOptions, session;

void main() {
  Future<void> start(
    WidgetTester tester,
    AppAppearance appearance,
    TestBroker api, {
    TestLoginMemory? memory,
  }) async {
    tester.view.physicalSize = const Size(1440, 900);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MultitapApp(
        api: api,
        serverOrigin: Uri.parse('https://configured.example.com'),
        loginMemory: memory ?? TestLoginMemory(),
        appearance: appearance,
      ),
    );
    await tester.pumpAndSettle();
  }

  Future<void> login(WidgetTester tester) async {
    await tester.enterText(find.byKey(const Key('sign-in-id')), '테마사용자');
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'fixture-password',
    );
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
  }

  testWidgets(
    'Light workspace keeps the daylight scene, a light header and a header update action',
    (tester) async {
      final appearance = AppAppearance(mode: ThemeMode.light);
      addTearDown(appearance.dispose);
      final api = TestBroker();
      await start(tester, appearance, api);
      await login(tester);
      final header = tester.widget<Container>(
        find.byKey(const Key('workspace-header')),
      );
      expect(header.color!.computeLuminance(), greaterThan(.85));
      // Signed-in workspaces have no footer strip; only the demo keeps one.
      expect(find.byKey(const Key('workspace-footer')), findsNothing);
      // The launcher keeps the full-window daylight scene, unshaded in light.
      expect(find.byType(SceneBackground), findsOneWidget);
      final overlay = tester.widget<DecoratedBox>(
        find.byKey(const Key('scene-appearance-overlay')),
      );
      final colors =
          ((overlay.decoration as BoxDecoration).gradient as LinearGradient)
              .colors;
      expect(colors.every((color) => color.a == 0), isTrue);
      expect(
        tester.widget<Image>(find.byType(Image).first).image,
        isA<AssetImage>().having(
          (image) => image.assetName,
          'asset',
          'assets/backgrounds/alpine-dusk-day.jpg',
        ),
      );
      expect(tester.widget<Brand>(find.byType(Brand)).onDark, isFalse);
      expect(
        tester
            .widget<IconButton>(find.byKey(const Key('check-client-updates')))
            .onPressed,
        isNotNull,
      );
      // The account menu chooses the scene (2026-10-26: theme and scene moved
      // there from their own header button); the light theme shows its day.
      expect(find.byKey(const Key('appearance-menu')), findsNothing);
      await tester.tap(find.byKey(const Key('workspace-account')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('scene-meadow')));
      await tester.pumpAndSettle();
      expect(appearance.scene, BackgroundScene.meadow);
      expect(
        (tester
                    .widget<Image>(
                      find.byKey(const Key('scene-background-image')),
                    )
                    .image
                as AssetImage)
            .assetName,
        'assets/backgrounds/alpine-daylight.png',
      );
      final before = tester.element(find.byType(WorkspaceShell));
      // 다크 in the same open menu: the app and the menu itself turn dark in
      // place, and the menu stays open.
      await tester.tap(find.byKey(const Key('theme-dark')));
      await tester.pumpAndSettle();
      expect(appearance.mode, ThemeMode.dark);
      expect(
        Theme.of(tester.element(find.byKey(const Key('sign-out')))).brightness,
        Brightness.dark,
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('sign-out')), findsNothing);
      // The same place at night.
      expect(
        (tester
                    .widget<Image>(
                      find.byKey(const Key('scene-background-image')),
                    )
                    .image
                as AssetImage)
            .assetName,
        'assets/backgrounds/alpine-daylight-night.jpg',
      );
      expect(
        tester
            .widget<Container>(find.byKey(const Key('workspace-header')))
            .color!
            .computeLuminance(),
        lessThan(.05),
      );
      expect(tester.element(find.byType(WorkspaceShell)), same(before));
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Changing appearance preserves sign-in fields and server selection',
    (tester) async {
      final appearance = AppAppearance();
      addTearDown(appearance.dispose);
      final api = TestBroker();
      final memory = TestLoginMemory();
      await start(tester, appearance, api, memory: memory);
      final before = tester.state(find.byType(SignInScreen));
      final loads = memory.loads;
      await tester.enterText(find.byKey(const Key('sign-in-id')), '한글사용자');
      await tester.enterText(
        find.byKey(const Key('sign-in-password')),
        'secret',
      );
      expect(
        Theme.of(tester.element(find.byType(SignInScreen))).brightness,
        Brightness.dark,
      );
      await appearance.select(ThemeMode.light);
      await tester.pumpAndSettle();
      expect(
        Theme.of(tester.element(find.byType(SignInScreen))).brightness,
        Brightness.light,
      );
      expect(tester.state(find.byType(SignInScreen)), same(before));
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        '한글사용자',
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        'secret',
      );
      expect(
        tester.widget<SignInScreen>(find.byType(SignInScreen)).serverOrigin,
        Uri.parse('https://configured.example.com'),
      );
      expect(memory.loads, loads);
      expect(api.logins, 0);
      expect(api.logouts, 0);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Authenticated workspace and confirmed logout keep the selected appearance',
    (tester) async {
      final appearance = AppAppearance();
      addTearDown(appearance.dispose);
      final api = TestBroker()..assigned = [assignedVm];
      await start(tester, appearance, api);
      await login(tester);
      final workspaceElement = tester.element(find.byType(WorkspaceShell));
      final original = tester.widget<WorkspaceShell>(
        find.byType(WorkspaceShell),
      );
      await appearance.select(ThemeMode.light);
      await tester.pumpAndSettle();
      final after = tester.widget<WorkspaceShell>(find.byType(WorkspaceShell));
      expect(
        tester.element(find.byType(WorkspaceShell)),
        same(workspaceElement),
      );
      expect(after.displayName, original.displayName);
      expect(after.machines.single.id, assignedVm.id);
      expect(api.logins, 1);
      expect(api.logouts, 0);
      expect(
        Theme.of(tester.element(find.byType(WorkspaceShell))).brightness,
        Brightness.light,
      );
      await tester.tap(find.byKey(const Key('workspace-account')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('sign-out')));
      await tester.pumpAndSettle();
      final confirmation = ModalRoute.of(tester.element(find.text('로그아웃할까요?')));
      await appearance.select(ThemeMode.dark);
      await tester.pumpAndSettle();
      expect(
        ModalRoute.of(tester.element(find.text('로그아웃할까요?'))),
        same(confirmation),
      );
      expect(api.logouts, 0);
      await tester.tap(find.byKey(const Key('confirm-sign-out')));
      await tester.pumpAndSettle();
      expect(api.logouts, 1);
      expect(find.byType(SignInScreen), findsOneWidget);
      expect(
        Theme.of(tester.element(find.byType(SignInScreen))).brightness,
        Brightness.dark,
      );
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Appearance rebuild preserves an active fake streaming session and pushed route',
    (tester) async {
      mockPresentation();
      final appearance = AppAppearance();
      addTearDown(appearance.dispose);
      final api = TestBroker()..assigned = [assignedVm];
      await start(tester, appearance, api);
      await login(tester);
      final socket = SessionSocket();
      final media = RecoveryMedia();
      final current = session(socket, media);
      var loads = 0;
      final navigator = Navigator.of(
        tester.element(find.byType(WorkspaceShell)),
      );
      final route = MaterialPageRoute<void>(
        builder: (_) => RemoteView(
          machine: machine,
          initialOptions: selectedOptions,
          load: () async {
            loads++;
            return current;
          },
        ),
      );
      unawaited(navigator.push(route));
      for (var i = 0; i < 8; i++) {
        await tester.pump(const Duration(milliseconds: 50));
      }
      socket.incoming.add(jsonEncode(info));
      await tester.pump();
      socket.incoming.add(packet());
      for (var i = 0; i < 8; i++) {
        await tester.pump(const Duration(milliseconds: 1));
      }
      expect(current.closed, isFalse, reason: current.error);
      expect(current.receivedBytes, greaterThan(0));
      final state = tester.state(find.byType(RemoteView));
      final starts = media.starts;
      await appearance.select(ThemeMode.light);
      await tester.pump(const Duration(milliseconds: 20));
      expect(tester.state(find.byType(RemoteView)), same(state));
      expect(
        ModalRoute.of(tester.element(find.byType(RemoteView))),
        same(route),
      );
      expect(current.closed, isFalse, reason: current.error);
      expect(media.starts, starts);
      expect(loads, 1);
      expect(api.logins, 1);
      expect(api.logouts, 0);
      navigator.pop();
      await tester.pumpAndSettle();
      expect(find.byType(WorkspaceShell), findsOneWidget);
      expect(api.logouts, 0);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );
}

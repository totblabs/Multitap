import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/components.dart';
import 'package:multitap_client/ui/korean_text.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

const _origin = 'https://private-broker.example.com';
const _machine = DesktopMachine(
  id: 'pve-149',
  name: 'TESTER',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows 11',
  accent: Colors.blue,
  powerState: 'running',
  powerStale: false,
  powerCanForceStop: true,
  hostOnline: true,
);

Future<void> _show(
  WidgetTester t, {
  required VoidCallback onSignOut,
  VoidCallback? onRefresh,
  Size size = const Size(1440, 900),
  double scale = 1,
  GlobalKey? boundary,
  Future<String?> Function()? checkClientUpdate,
  AppAppearance? appearance,
  bool remoteWindows = false,
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = size;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  // The shipped app always provides the appearance scope, so the header also
  // carries the theme menu in these layout checks.
  if (appearance == null) {
    appearance = AppAppearance();
    addTearDown(appearance.dispose);
  }
  await t.pumpWidget(
    RepaintBoundary(
      key: boundary,
      child: AppearanceScope(
        controller: appearance,
        child: MaterialApp(
          debugShowCheckedModeBanner: false,
          theme: buildAppTheme(),
          builder: (_, child) => MediaQuery(
            data: MediaQueryData(
              size: size,
              textScaler: TextScaler.linear(scale),
            ),
            child: child!,
          ),
          home: WorkspaceShell(
            onSignOut: onSignOut,
            onRefresh: onRefresh ?? () {},
            displayName: '백종헌',
            serverOrigin: _origin,
            machines: const [_machine],
            onConnect: (_, {options = const StreamOptions()}) async =>
                throw StateError('UI fixture does not connect'),
            onPower: (_, _) async {},
            checkClientUpdate: checkClientUpdate,
            onOpenWindow: remoteWindows ? (_, _, _) async => null : null,
          ),
        ),
      ),
    ),
  );
  await t.pumpAndSettle();
}

Future<void> _account(WidgetTester t, String item) async {
  await t.tap(find.byKey(const Key('workspace-account')));
  await t.pumpAndSettle();
  await t.tap(find.byKey(Key(item)));
}

void main() {
  testWidgets(
    'Workspace has one compact header with the version and no footer strip',
    (t) async {
      var refreshes = 0;
      await _show(t, onSignOut: () {}, onRefresh: () => refreshes++);
      expect(find.text('My Cloud Desktop'), findsNothing);
      expect(find.text('내 데스크톱'), findsOneWidget);
      expect(find.text('할당된 VM과 연결 상태를 확인하세요.'), findsNothing);
      expect(find.text(_origin), findsNothing);
      expect(find.text('1대'), findsNothing);
      expect(find.byKey(const Key('workspace-footer')), findsNothing);
      expect(find.byKey(const Key('workspace-management-note')), findsNothing);
      expect(
        find.descendant(
          of: find.byKey(const Key('workspace-header')),
          matching: find.byKey(const Key('check-client-updates')),
        ),
        findsOneWidget,
      );
      expect(find.byTooltip('앱 업데이트 확인'), findsOneWidget);
      // The version is shown once, in the account menu, not beside the brand.
      expect(find.byKey(const Key('client-version-info')), findsNothing);
      expect(
        t.getTopLeft(find.byKey(const Key('card-pve-149'))).dy,
        lessThan(210),
      );
      expect(t.getTopLeft(find.byKey(const Key('card-pve-149'))).dx, 100);
      expect(
        t.getTopLeft(find.byKey(const Key('refresh-desktops'))).dy,
        lessThan(100),
      );
      await t.tap(find.byKey(const Key('refresh-desktops')));
      await t.pump();
      expect(refreshes, 1);
      await _account(t, 'open-settings');
      await t.pumpAndSettle();
      expect(find.byKey(const Key('app-info-facts')), findsOneWidget);
      expect(find.text('빌드'), findsOneWidget);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'The readiness check asks the broker again on opening and shows when',
    (t) async {
      var refreshes = 0;
      await _show(t, onSignOut: () {}, onRefresh: () => refreshes++);
      await t.tap(find.byKey(const Key('desktop-more-pve-149')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('details-pve-149')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('readiness-dialog')), findsOneWidget);
      expect(refreshes, 1, reason: 'Opening the check is a real check.');
      expect(
        t.widget<Text>(find.byKey(const Key('readiness-checked'))).data,
        startsWith('점검 시각 '),
      );
      expect(find.textContaining('마지막으로 보고한'), findsNothing);
      await t.tap(find.byKey(const Key('readiness-check')));
      await t.pump();
      expect(refreshes, 2);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'A newer client on the server puts a badge on the header update button',
    (t) async {
      final badge = find.byKey(const Key('client-update-badge'));
      // Nothing to install: a quiet icon like the other header actions.
      await _show(t, onSignOut: () {}, checkClientUpdate: () async => null);
      expect(badge, findsNothing);
      expect(
        t.widget(find.byKey(const Key('check-client-updates'))),
        isA<IconButton>(),
      );
      expect(find.byTooltip('앱 업데이트 확인'), findsOneWidget);
      await t.pumpWidget(const SizedBox.shrink());
      var checks = 0;
      await _show(
        t,
        onSignOut: () {},
        checkClientUpdate: () async {
          checks++;
          return '0.4.99';
        },
      );
      // A newer client: an amber "새 버전" button with a dot on its corner.
      expect(badge, findsOneWidget);
      expect(find.text('새 버전 v0.4.99'), findsOneWidget);
      final button = t.getRect(find.byKey(const Key('check-client-updates')));
      final dot = t.getRect(badge);
      expect(dot.center.dx, greaterThan(button.right - 8));
      expect(dot.center.dy, lessThan(button.top + 8));
      // Checked again on the next interval while signed in.
      await t.pump(const Duration(minutes: 10));
      expect(checks, 2);
      await t.pumpWidget(const SizedBox.shrink());
      // A narrow header keeps the badge on the icon button.
      await _show(
        t,
        onSignOut: () {},
        size: const Size(560, 800),
        checkClientUpdate: () async => '0.4.99',
      );
      expect(badge, findsOneWidget);
      expect(find.byTooltip('앱 새 버전 v0.4.99'), findsOneWidget);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'A release published while open shows on returning to the window or 새로고침',
    (t) async {
      final badge = find.byKey(const Key('client-update-badge'));
      String? latest;
      var checks = 0;
      var refreshes = 0;
      await _show(
        t,
        onSignOut: () {},
        onRefresh: () => refreshes++,
        checkClientUpdate: () async {
          checks++;
          return latest;
        },
      );
      expect(checks, 1);
      expect(badge, findsNothing);
      latest = '0.4.99';
      // Coming back within a minute of the last read does not read again.
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await t.pump();
      expect(checks, 1);
      expect(badge, findsNothing);
      await t.pump(const Duration(minutes: 2));
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await t.pumpAndSettle();
      expect(checks, 2);
      expect(badge, findsOneWidget);
      // 새로고침 reads at once, and still refreshes the list.
      latest = null;
      await t.tap(find.byKey(const Key('refresh-desktops')));
      await t.pumpAndSettle();
      expect(checks, 3);
      expect(refreshes, 1);
      expect(badge, findsNothing);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'The header keeps the update control and the account, in one geometry on the page column',
    (t) async {
      await _show(t, onSignOut: () {}, checkClientUpdate: () async => '0.4.99');
      final header = find.byKey(const Key('workspace-header'));
      final update = find.byKey(const Key('check-client-updates'));
      final account = find.byKey(const Key('workspace-account'));
      // The log and the theme/scene moved into the account menu.
      for (final moved in const ['open-client-log', 'appearance-menu']) {
        expect(find.byKey(Key(moved)), findsNothing, reason: moved);
      }
      expect(find.descendant(of: header, matching: update), findsOneWidget);
      expect(find.descendant(of: header, matching: account), findsOneWidget);
      // One control height, centred on one line.
      expect(t.getSize(update).height, 36);
      expect(t.getSize(account).height, 36);
      expect(t.getCenter(update).dy, t.getCenter(account).dy);
      // The brand starts where the title starts; the account ends where
      // 새로고침 ends (the page column, not the window edge).
      expect(
        t.getTopLeft(find.byType(Brand)).dx,
        t.getTopLeft(find.byKey(const Key('workspace-title'))).dx,
      );
      expect(
        t.getTopRight(account).dx,
        t.getTopRight(find.byKey(const Key('refresh-desktops'))).dx,
      );
      // A full name is not repeated as a tooltip.
      expect(find.byTooltip('백종헌'), findsNothing);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'The account menu opens under its button with the log, theme and scene, and works from the keyboard',
    (t) async {
      final appearance = AppAppearance();
      addTearDown(appearance.dispose);
      await _show(t, onSignOut: () {}, appearance: appearance);
      final account = find.byKey(const Key('workspace-account'));
      await t.tap(account);
      await t.pumpAndSettle();
      // Right-aligned under the button (the identity card stands the
      // panel's 6 px padding inside it); identity first, 로그아웃 last.
      final identity = t.getRect(find.byKey(const Key('account-identity')));
      expect(identity.right + 6, t.getRect(account).right);
      expect(identity.top, greaterThan(t.getRect(account).bottom));
      // Who and where: the server host and the running build.
      expect(
        t.widget<Text>(find.byKey(const Key('account-server'))).data,
        startsWith('private-broker.example.com · '),
      );
      double top(String key) => t.getTopLeft(find.byKey(Key(key))).dy;
      final order = [
        'open-settings',
        'open-client-log',
        'theme-dark',
        'scene-peaks',
        'sign-out',
      ].map(top).toList();
      expect(order, orderedEquals([...order]..sort()));
      expect(
        t.getRect(find.byKey(const Key('sign-out'))).bottom,
        greaterThan(t.getRect(find.byKey(const Key('scene-meadow'))).bottom),
      );
      // A scene choice applies at once and keeps the menu open.
      await t.tap(find.byKey(const Key('scene-meadow')));
      await t.pumpAndSettle();
      expect(appearance.scene, BackgroundScene.meadow);
      expect(find.byKey(const Key('sign-out')), findsOneWidget);
      // 로그 보기 opens the log viewer and closes the menu.
      await t.tap(find.byKey(const Key('open-client-log')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('close-product-log')), findsOneWidget);
      expect(find.byKey(const Key('sign-out')), findsNothing);
      await t.tap(find.byKey(const Key('close-product-log')));
      await t.pumpAndSettle();
      // Keyboard: Enter opens with the first row focused, the arrows move
      // through the rows, Escape closes and returns to the button.
      final button = Focus.of(
        t.element(find.descendant(of: account, matching: find.byType(Row))),
      );
      button.requestFocus();
      await t.pump();
      await t.sendKeyEvent(LogicalKeyboardKey.enter);
      await t.pumpAndSettle();
      bool focused(String key) => Focus.of(
        t.element(
          find
              .descendant(
                of: find.byKey(Key(key)),
                matching: find.byType(KeepAllText),
              )
              .first,
        ),
      ).hasPrimaryFocus;
      expect(focused('open-settings'), isTrue);
      await t.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await t.pumpAndSettle();
      expect(focused('open-client-log'), isTrue);
      await t.sendKeyEvent(LogicalKeyboardKey.escape);
      await t.pumpAndSettle();
      expect(find.byKey(const Key('sign-out')), findsNothing);
      expect(button.hasPrimaryFocus, isTrue);
      // Opened by a click, ↓ still enters the menu (not the page behind it).
      await t.tap(account);
      await t.pumpAndSettle();
      await t.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await t.pumpAndSettle();
      expect(focused('open-settings'), isTrue);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('Logout requires confirmation and cancel retains the workspace', (
    t,
  ) async {
    var signOuts = 0;
    await _show(t, onSignOut: () => signOuts++, remoteWindows: true);
    await _account(t, 'sign-out');
    await t.pumpAndSettle();
    expect(signOuts, 0);
    expect(find.text('로그아웃할까요?'), findsOneWidget);
    final copy = find.text('열린 원격 창도 함께 닫혀요.\n데스크톱과 실행 중인 작업은 그대로 유지돼요.');
    final rich = find.descendant(of: copy, matching: find.byType(RichText));
    final paragraph = t.renderObject<RenderParagraph>(rich);
    final rendered = paragraph.text.toPlainText();
    final boundary = rendered.indexOf('\n');
    final before = paragraph.getBoxesForSelection(
      TextSelection(baseOffset: boundary - 1, extentOffset: boundary),
    );
    final after = paragraph.getBoxesForSelection(
      TextSelection(baseOffset: boundary + 1, extentOffset: boundary + 2),
    );
    expect(
      after.single.top,
      greaterThan(before.single.top),
      reason: 'The preserved-work statement starts on its own rendered line.',
    );
    await t.tap(find.byKey(const Key('cancel-sign-out')));
    await t.pumpAndSettle();
    expect(signOuts, 0);
    expect(find.byType(WorkspaceShell), findsOneWidget);
    await _account(t, 'sign-out');
    await t.pumpAndSettle();
    await t.tap(find.byKey(const Key('confirm-sign-out')));
    await t.pumpAndSettle();
    expect(signOuts, 1);
  });

  testWidgets('Workspace remains usable in narrow windows and large text', (
    t,
  ) async {
    for (final spec in [
      (1440.0, 900.0, 1.0),
      (640.0, 600.0, 1.25),
      (320.0, 700.0, 2.0),
    ]) {
      await _show(
        t,
        onSignOut: () {},
        size: Size(spec.$1, spec.$2),
        scale: spec.$3,
      );
      expect(t.takeException(), isNull);
      for (final action in const [
        'workspace-account',
        'refresh-desktops',
        'check-client-updates',
      ]) {
        expect(
          find.byKey(Key(action)).hitTestable(),
          findsOneWidget,
          reason: '$action at ${spec.$1}',
        );
      }
      // The account stays identifiable even when reduced to an initial: the
      // name beside the monogram, or as its tooltip.
      final account = find.byKey(const Key('workspace-account'));
      final named = find
          .descendant(of: account, matching: find.text('백종헌'))
          .evaluate()
          .isNotEmpty;
      expect(
        named ||
            find
                .ancestor(of: account, matching: find.byTooltip('백종헌'))
                .evaluate()
                .isNotEmpty,
        isTrue,
        reason: 'account at ${spec.$1}',
      );
      await t.tap(account);
      await t.pumpAndSettle();
      for (final item in const [
        'open-settings',
        'open-client-log',
        'theme-dark',
        'theme-light',
        'scene-peaks',
        'scene-meadow',
        'sign-out',
      ]) {
        expect(
          find.byKey(Key(item)).hitTestable(),
          findsOneWidget,
          reason: '$item at ${spec.$1}',
        );
      }
      await t.sendKeyEvent(LogicalKeyboardKey.escape);
      await t.pumpAndSettle();
      expect(find.byKey(const Key('sign-out')), findsNothing);
      if (spec.$1 >= 640) {
        expect(
          t.getSize(find.byKey(const Key('workspace-header'))).height,
          lessThan(80),
          reason: '${spec.$1} header stays on one row',
        );
      }
      await _account(t, 'sign-out');
      await t.pumpAndSettle();
      expect(
        find.byKey(const Key('confirm-sign-out')).hitTestable(),
        findsOneWidget,
      );
      expect(
        find.byKey(const Key('cancel-sign-out')).hitTestable(),
        findsOneWidget,
      );
      expect(t.takeException(), isNull);
      await t.tap(find.byKey(const Key('cancel-sign-out')));
      await t.pumpAndSettle();
    }
  });

  // ---- review 2026-11-12 ---------------------------------------------------

  Widget shell({
    List<DesktopMachine> machines = const [_machine],
    bool busy = false,
    String? error,
    String? signOutError,
    int listGeneration = 1,
    VoidCallback? onSignOut,
    VoidCallback? onRefresh,
    Future<String?> Function()? checkClientUpdate,
    Future<RemoteSession> Function(String, {StreamOptions options})? onConnect,
  }) => MaterialApp(
    debugShowCheckedModeBanner: false,
    theme: buildAppTheme(),
    home: WorkspaceShell(
      onSignOut: onSignOut ?? () {},
      onRefresh: onRefresh ?? () {},
      displayName: '백종헌',
      serverOrigin: _origin,
      machines: machines,
      busy: busy,
      error: error,
      signOutError: signOutError,
      listGeneration: listGeneration,
      onConnect:
          onConnect ??
          (_, {options = const StreamOptions()}) async =>
              throw StateError('UI fixture does not connect'),
      onPower: (_, _) async {},
      checkClientUpdate: checkClientUpdate,
    ),
  );

  void window(WidgetTester t) {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = const Size(1440, 900);
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
  }

  testWidgets(
    'A failed sign-out is its own line whose 다시 시도 signs out again, never the list error',
    (t) async {
      window(t);
      var signOuts = 0, refreshes = 0;
      await t.pumpWidget(
        shell(
          signOutError: '로그아웃하지 못했어요. 네트워크를 확인하고 다시 시도하세요.',
          onSignOut: () => signOuts++,
          onRefresh: () => refreshes++,
        ),
      );
      await t.pumpAndSettle();
      expect(find.byKey(const Key('workspace-error')), findsNothing);
      expect(find.byKey(const Key('workspace-error-card')), findsNothing);
      expect(find.text('목록을 불러오지 못했어요'), findsNothing);
      expect(
        t.widget<Text>(find.byKey(const Key('workspace-sign-out-error'))).data,
        '로그아웃하지 못했어요. 네트워크를 확인하고 다시 시도하세요.',
      );
      final before = refreshes;
      await t.tap(find.byKey(const Key('workspace-sign-out-retry')));
      await t.pump();
      expect(signOuts, 1);
      expect(refreshes, before, reason: 'the list is not reloaded');
      // The desktops stay listed under it.
      expect(find.text('TESTER'), findsWidgets);
    },
  );

  testWidgets(
    'A failed catalog read keeps the 새 버전 button as it was',
    (t) async {
      window(t);
      final badge = find.byKey(const Key('client-update-badge'));
      var fail = false, checks = 0;
      await t.pumpWidget(
        shell(
          checkClientUpdate: () async {
            checks++;
            if (fail) throw const SocketException('offline');
            return '0.4.99';
          },
        ),
      );
      await t.pumpAndSettle();
      expect(badge, findsOneWidget);
      fail = true;
      await t.pump(const Duration(minutes: 10));
      await t.pump();
      expect(checks, greaterThan(1));
      expect(badge, findsOneWidget, reason: 'nothing new was learned');
      expect(find.text('새 버전 v0.4.99'), findsOneWidget);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'The readiness check shows when the list was last read, not a pushed sample or a failed read, and its header follows the verdict',
    (t) async {
      window(t);
      const starting = DesktopMachine(
        id: 'pve-149',
        name: 'TESTER',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.awaitingHost,
        specification: 'Windows 11',
        accent: Colors.blue,
        powerState: 'running',
        powerStale: false,
        hostOnline: false,
      );
      await t.pumpWidget(shell(machines: const [starting]));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('stream-state-pve-149')));
      await t.pumpAndSettle();
      String checked() =>
          t.widget<Text>(find.byKey(const Key('readiness-checked'))).data!;
      String clock() =>
          RegExp(r'\d\d:\d\d:\d\d').firstMatch(checked())!.group(0)!;
      final first = clock();
      expect(checked(), startsWith('점검 시각 '));
      // The verdict is a warning ("Host가 응답하지 않아요"): so is the header.
      expect(
        t
            .widget<AppToneIcon>(find.byKey(const Key('readiness-header-icon')))
            .tone,
        AppTone.warn,
      );
      // A pushed CPU/RAM sample: a new list object, the same generation.
      await t.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 1100)),
      );
      await t.pumpWidget(
        shell(machines: [starting.withResources(starting.resources)]),
      );
      await t.pumpAndSettle();
      expect(clock(), first, reason: 'a pushed sample is not a check');
      // A failed read empties the list: the old rows keep the old time.
      await t.pumpWidget(shell(machines: const [], error: '목록 오류'));
      await t.pumpAndSettle();
      expect(checked(), '점검하지 못했어요 · 마지막 결과 $first');
      // A real read: the time moves on.
      await t.pumpWidget(
        shell(machines: const [starting], listGeneration: 2),
      );
      await t.pumpAndSettle();
      expect(checked(), startsWith('점검 시각 '));
      expect(clock(), isNot(first));
    },
  );

  testWidgets(
    'A desktop whose state changed while the question was open says so instead of doing nothing',
    (t) async {
      window(t);
      var connects = 0;
      Future<RemoteSession> connect(
        String _, {
        StreamOptions options = const StreamOptions(),
      }) async {
        connects++;
        throw StateError('UI fixture does not connect');
      }
      await t.pumpWidget(shell(onConnect: connect));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('open-pve-149')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('connect-confirm-dialog')), findsOneWidget);
      // The Host went away meanwhile.
      const away = DesktopMachine(
        id: 'pve-149',
        name: 'TESTER',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.awaitingHost,
        specification: 'Windows 11',
        accent: Colors.blue,
        powerState: 'running',
        powerStale: false,
        hostOnline: false,
      );
      await t.pumpWidget(
        shell(machines: const [away], listGeneration: 2, onConnect: connect),
      );
      await t.pump();
      await t.tap(find.byKey(const Key('confirm-connect-pve-149')));
      await t.pumpAndSettle();
      expect(connects, 0);
      expect(find.text('연결 준비 상태가 바뀌었어요. 목록을 다시 확인하세요.'), findsOneWidget);
    },
  );
}

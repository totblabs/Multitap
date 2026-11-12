import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

void main() {
  Future<void> show(
    WidgetTester tester,
    List<String> actions, {
    String state = 'running',
    bool stale = false,
    bool busy = false,
    bool? canForceStop,
    String? powerAction,
    Future<void> Function(String, String)? powerHandler,
  }) async {
    tester.view.physicalSize = const Size(1400, 1000);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: WorkspaceShell(
          onSignOut: () {},
          onRefresh: () {},
          displayName: '사용자',
          serverOrigin: 'https://cloud.example.com',
          onPower:
              powerHandler ??
              (id, action) async {
                actions.add('$id:$action');
              },
          machines: [
            DesktopMachine(
              id: 'pve-149',
              name: 'TESTER',
              description: '',
              kind: MachineKind.vm,
              status: MachineStatus.awaitingHost,
              specification: 'Windows',
              accent: Colors.blue,
              powerState: state,
              powerStale: stale,
              powerBusy: busy,
              // What a current broker reports: stop is allowed for a running
              // idle VM (and over this account's own unfinished shutdown).
              powerCanForceStop:
                  canForceStop ?? (state == 'running' && !stale && !busy),
              powerAction: powerAction,
            ),
          ],
        ),
      ),
    );
    if (busy) {
      await tester.pump(const Duration(milliseconds: 200));
    } else {
      await tester.pumpAndSettle();
    }
  }

  Future<void> openMenu(WidgetTester tester, {bool settle = true}) async {
    await tester.tap(find.byKey(const Key('desktop-power-pve-149')));
    if (settle) {
      await tester.pumpAndSettle();
    } else {
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
    }
  }

  MenuItemButton item(WidgetTester tester, String action) =>
      tester.widget<MenuItemButton>(find.byKey(Key('power-$action-pve-149')));

  List<String> offered(WidgetTester tester) => [
    for (final action in ['start', 'reboot', 'shutdown', 'stop', 'reset'])
      if (find.byKey(Key('power-$action-pve-149')).evaluate().isNotEmpty)
        action,
  ];

  double top(WidgetTester tester, String action) =>
      tester.getTopLeft(find.byKey(Key('power-$action-pve-149'))).dy;

  String? tooltipOf(WidgetTester tester, String action) => tester
      .widgetList<Tooltip>(
        find.ancestor(
          of: find.byKey(Key('power-$action-pve-149')),
          matching: find.byType(Tooltip),
        ),
      )
      .firstOrNull
      ?.message;

  testWidgets(
    'a running VM offers 종료, 다시 시작 and, apart in red, 정지 in that order',
    (tester) async {
      await show(tester, []);
      expect(
        find.descendant(
          of: find.byKey(const Key('card-pve-149')),
          matching: find.text('실행 중'),
        ),
        findsOneWidget,
      );
      await openMenu(tester);
      expect(offered(tester), ['reboot', 'shutdown', 'stop']);
      for (final action in ['shutdown', 'reboot', 'stop']) {
        expect(item(tester, action).onPressed, isNotNull, reason: action);
      }
      expect(find.text('종료'), findsOneWidget);
      expect(find.text('다시 시작'), findsOneWidget);
      expect(find.text('정지'), findsOneWidget);
      expect(find.text('정상 종료'), findsNothing);
      expect(find.textContaining('강제'), findsNothing);
      expect(top(tester, 'shutdown'), lessThan(top(tester, 'reboot')));
      expect(top(tester, 'reboot'), lessThan(top(tester, 'stop')));
      // A divider separates 정지 from the others.
      final dividers = find.byType(Divider);
      final lines = [
        for (var i = 0; i < dividers.evaluate().length; i++)
          tester.getTopLeft(dividers.at(i)).dy,
      ];
      expect(
        lines.where(
          (y) => y > top(tester, 'reboot') && y < top(tester, 'stop'),
        ),
        hasLength(1),
      );
      // 정지 reads red: its label and its icon.
      final palette = AppPalette.of(
        tester.element(find.byKey(const Key('power-stop-pve-149'))),
      );
      final label = tester.widget<RichText>(
        find
            .descendant(
              of: find.byKey(const Key('power-stop-pve-149')),
              matching: find.byType(RichText),
            )
            .last,
      );
      expect(label.text.style?.color, palette.red);
      final icon = tester.widget<Icon>(
        find.descendant(
          of: find.byKey(const Key('power-stop-pve-149')),
          matching: find.byIcon(Icons.power_off_rounded),
        ),
      );
      expect(icon.color, palette.red);
    },
  );

  testWidgets('종료 asks first with Proxmox wording; cancel sends nothing', (
    tester,
  ) async {
    final actions = <String>[];
    await show(tester, actions);
    await openMenu(tester);
    await tester.tap(find.byKey(const Key('power-shutdown-pve-149')));
    await tester.pumpAndSettle();
    expect(find.text('데스크톱을 종료할까요?'), findsOneWidget);
    expect(find.textContaining('Windows를 정상적으로 끄도록 요청해요.'), findsOneWidget);
    expect(actions, isEmpty);
    await tester.tap(find.byKey(const Key('cancel-power-shutdown-pve-149')));
    await tester.pumpAndSettle();
    expect(actions, isEmpty);
    await openMenu(tester);
    await tester.tap(find.byKey(const Key('power-shutdown-pve-149')));
    await tester.pumpAndSettle();
    await tester.tap(find.widgetWithText(FilledButton, '종료'));
    await tester.pumpAndSettle();
    expect(actions, ['pve-149:shutdown']);
    expect(find.text('TESTER 종료 작업이 완료됐어요.'), findsOneWidget);
  });

  testWidgets(
    '정지 asks in the danger tone, Enter stays on 취소, and only 정지 sends stop',
    (tester) async {
      final actions = <String>[];
      await show(tester, actions);
      await openMenu(tester);
      await tester.tap(find.byKey(const Key('power-stop-pve-149')));
      await tester.pumpAndSettle();
      expect(find.text('데스크톱을 정지할까요?'), findsOneWidget);
      expect(
        find.text(
          '전원을 바로 끊어요. 저장하지 않은 작업이 사라질 수 있어요. Windows가 응답하지 않을 때만 쓰세요.',
        ),
        findsOneWidget,
      );
      final confirm = find.byKey(const Key('confirm-power-stop-pve-149'));
      expect(
        find.descendant(of: confirm, matching: find.text('정지')),
        findsOneWidget,
      );
      // The danger button: the palette's red, and Enter does not confirm.
      final palette = AppPalette.of(tester.element(confirm));
      final button = tester.widget<FilledButton>(confirm);
      expect(button.style?.backgroundColor?.resolve({}), palette.red);
      await tester.sendKeyEvent(LogicalKeyboardKey.enter);
      await tester.pumpAndSettle();
      expect(find.text('데스크톱을 정지할까요?'), findsNothing);
      expect(actions, isEmpty);
      await openMenu(tester);
      await tester.tap(find.byKey(const Key('power-stop-pve-149')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('confirm-power-stop-pve-149')));
      await tester.pumpAndSettle();
      expect(actions, ['pve-149:stop']);
      expect(find.text('TESTER 정지 작업이 완료됐어요.'), findsOneWidget);
    },
  );

  testWidgets('stopped VM shows every power action and enables only 켜기', (
    tester,
  ) async {
    final actions = <String>[];
    await show(tester, actions, state: 'stopped');
    expect(
      find.descendant(
        of: find.byKey(const Key('card-pve-149')),
        matching: find.text('꺼짐'),
      ),
      findsOneWidget,
    );
    await openMenu(tester);
    expect(offered(tester), ['start', 'reboot', 'shutdown', 'stop']);
    expect(top(tester, 'start'), lessThan(top(tester, 'shutdown')));
    for (final action in ['reboot', 'shutdown', 'stop']) {
      expect(item(tester, action).onPressed, isNull, reason: action);
    }
    await tester.tap(find.byKey(const Key('power-start-pve-149')));
    await tester.pumpAndSettle();
    // 켜기 asks like the other power actions.
    expect(find.text('데스크톱을 켤까요?'), findsOneWidget);
    expect(actions, isEmpty);
    await tester.tap(find.byKey(const Key('confirm-power-start-pve-149')));
    await tester.pumpAndSettle();
    expect(actions, ['pve-149:start']);
  });

  testWidgets('stale and in-progress states cannot issue power commands', (
    tester,
  ) async {
    final actions = <String>[];
    await show(tester, actions, stale: true);
    expect(find.text('확인 중'), findsOneWidget);
    await openMenu(tester);
    for (final action in offered(tester)) {
      expect(item(tester, action).onPressed, isNull, reason: action);
    }
    await tester.tapAt(const Offset(10, 990));
    await tester.pumpAndSettle();
    await show(tester, actions, busy: true);
    expect(find.text('전원 작업 중'), findsOneWidget);
    await openMenu(tester, settle: false);
    expect(offered(tester), ['reboot', 'shutdown', 'stop']);
    for (final action in offered(tester)) {
      expect(item(tester, action).onPressed, isNull, reason: action);
    }
    expect(tooltipOf(tester, 'stop'), '전원 작업이 진행 중이에요.');
    expect(actions, isEmpty);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets(
    'without the broker stop permission (an older broker) 정지 stays greyed with the reason',
    (tester) async {
      final actions = <String>[];
      await show(tester, actions, canForceStop: false);
      await openMenu(tester);
      expect(offered(tester), ['reboot', 'shutdown', 'stop']);
      expect(item(tester, 'shutdown').onPressed, isNotNull);
      expect(item(tester, 'stop').onPressed, isNull);
      expect(tooltipOf(tester, 'stop'), contains('정지를 쓸 수 없어요'));
      expect(offered(tester), isNot(contains('reset')));
      expect(actions, isEmpty);
    },
  );

  testWidgets(
    'while its own 종료 has not finished, 정지 stays available (the broker replaces the shutdown)',
    (tester) async {
      final actions = <String>[];
      await show(
        tester,
        actions,
        busy: true,
        canForceStop: true,
        powerAction: 'shutdown',
      );
      expect(find.text('종료하는 중'), findsWidgets);
      expect(find.text('Windows가 응답하지 않으면 전원 메뉴에서 정지할 수 있어요.'), findsOneWidget);
      await openMenu(tester, settle: false);
      for (final action in ['reboot', 'shutdown']) {
        expect(item(tester, action).onPressed, isNull, reason: action);
      }
      expect(item(tester, 'stop').onPressed, isNotNull);
      await tester.tap(find.byKey(const Key('power-stop-pve-149')));
      // The menu runs its action after the next frame; then the dialog opens.
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.text('데스크톱을 정지할까요?'), findsOneWidget);
      await tester.tap(find.byKey(const Key('confirm-power-stop-pve-149')));
      await tester.pump(const Duration(milliseconds: 300));
      expect(actions, ['pve-149:stop']);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );

  testWidgets('a stop in progress reads 정지하는 중', (tester) async {
    await show(
      tester,
      [],
      busy: true,
      canForceStop: false,
      powerAction: 'stop',
    );
    expect(find.text('정지하는 중'), findsWidgets);
    expect(find.text('데스크톱을 정지하는 중이에요'), findsWidgets);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets(
    'An older superseded power request does not show a false failure toast',
    (tester) async {
      await show(
        tester,
        [],
        powerHandler: (_, _) async {
          throw const BrokerException('power_superseded');
        },
      );
      await openMenu(tester);
      await tester.tap(find.byKey(const Key('power-reboot-pve-149')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('confirm-power-reboot-pve-149')));
      await tester.pumpAndSettle();
      expect(find.byType(SnackBar), findsNothing);
    },
  );
}

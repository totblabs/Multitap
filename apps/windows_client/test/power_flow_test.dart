import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'dart:async';

import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestBroker;
import 'power_api_test.dart' show taskId;

BrokerDesktop desktop({
  String state = 'stopped',
  bool stale = false,
  bool taskBusy = false,
  bool canForceStop = false,
}) => BrokerDesktop(
  id: 'qa-vm',
  name: 'QA VM',
  kind: 'vm',
  description: '',
  os: 'Windows',
  status: 'host_offline',
  canConnect: false,
  power: BrokerPowerSnapshot(
    state: state,
    stale: stale,
    checkedAt: DateTime.utc(2026, 10, 22),
  ),
  powerBusy: taskBusy,
  powerCanForceStop: canForceStop,
);

class PowerBroker extends TestBroker {
  int submitted = 0, polls = 0, reads = 0;
  String? action;
  bool failTask = false;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    reads++;
    return super.desktops(token);
  }

  @override
  Future<BrokerPowerResult> requestPower(
    String token,
    String id,
    String action,
  ) async {
    if (token != 'test-token' || id != 'qa-vm') {
      throw StateError('Unexpected fixture identity.');
    }
    submitted++;
    this.action = action;
    return BrokerPowerResult(
      power: const BrokerPowerSnapshot(state: 'stopped', stale: false),
      task: const BrokerPowerTask(
        id: taskId,
        desktopId: 'qa-vm',
        action: 'start',
        status: 'submitted',
      ),
    );
  }

  @override
  Future<BrokerPowerResult> power(
    String token,
    String id, {
    String? taskId,
  }) async {
    polls++;
    if (taskId == null) throw StateError('Task ID is required.');
    assigned = [desktop(state: failTask ? 'stopped' : 'running')];
    return BrokerPowerResult(
      power: BrokerPowerSnapshot(
        state: failTask ? 'stopped' : 'running',
        stale: false,
      ),
      task: BrokerPowerTask(
        id: taskId,
        desktopId: 'qa-vm',
        action: 'start',
        status: 'stopped',
        exitStatus: failTask ? 'ERROR' : 'OK',
      ),
    );
  }
}

class ForcedStopBroker extends PowerBroker {
  final delayedShutdown = Completer<BrokerPowerResult>();
  final actions = <String>[];
  int shutdownPolls = 0;
  static const forcedId = '514c03e4-5595-4d77-a075-f6a61cfc4e8b';
  @override
  Future<BrokerPowerResult> requestPower(
    String token,
    String id,
    String action,
  ) async {
    actions.add(action);
    return BrokerPowerResult(
      power: const BrokerPowerSnapshot(state: 'running', stale: false),
      canForceStop: action == 'shutdown',
      task: BrokerPowerTask(
        id: action == 'shutdown' ? taskId : forcedId,
        desktopId: id,
        action: action,
        status: 'submitted',
      ),
    );
  }

  @override
  Future<BrokerPowerResult> power(
    String token,
    String id, {
    String? taskId,
  }) async {
    if (taskId != forcedId) {
      shutdownPolls++;
      return delayedShutdown.future;
    }
    assigned = [desktop(state: 'stopped')];
    return BrokerPowerResult(
      power: const BrokerPowerSnapshot(state: 'stopped', stale: false),
      task: BrokerPowerTask(
        id: forcedId,
        desktopId: id,
        action: 'stop',
        status: 'stopped',
        exitStatus: 'OK',
      ),
    );
  }
}

void main() {
  Future<void> login(WidgetTester tester, PowerBroker api) async {
    tester.view.physicalSize = const Size(1440, 900);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MultitapApp(
        api: api,
        serverOrigin: Uri.parse('https://cloud.example.com'),
      ),
    );
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('sign-in-id')), 'qa-user');
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'qa-password',
    );
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
  }

  testWidgets(
    '정지 replaces the clients own unfinished 종료 when the broker allows it; the shutdown polling ends quietly',
    (tester) async {
      final api = ForcedStopBroker()
        ..assigned = [desktop(state: 'running', canForceStop: true)];
      await login(tester, api);
      var workspace = tester.widget<WorkspaceShell>(
        find.byType(WorkspaceShell),
      );
      expect(workspace.machines.single.canStop, isTrue);
      final shutdown = workspace.onPower!('qa-vm', 'shutdown');
      await tester.pump();
      await tester.pump(const Duration(seconds: 1));
      expect(api.shutdownPolls, 1);
      // The broker's answer allows the stop over this account's shutdown.
      workspace = tester.widget<WorkspaceShell>(find.byType(WorkspaceShell));
      expect(workspace.machines.single.powerBusy, isTrue);
      expect(workspace.machines.single.canStop, isTrue);
      expect(workspace.machines.single.canShutdown, isFalse);
      await expectLater(
        workspace.onPower!('qa-vm', 'reboot'),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'power_in_progress',
          ),
        ),
      );
      final stop = workspace.onPower!('qa-vm', 'stop');
      await tester.pump();
      await tester.pump(const Duration(seconds: 1));
      await tester.pump();
      await stop;
      expect(api.actions, ['shutdown', 'stop']);
      // The old shutdown poll answers late; its request ends as superseded
      // (the shell shows nothing for it) and changes nothing.
      api.delayedShutdown.complete(
        const BrokerPowerResult(
          power: BrokerPowerSnapshot(state: 'stopped', stale: false),
          task: BrokerPowerTask(
            id: taskId,
            desktopId: 'qa-vm',
            action: 'shutdown',
            status: 'superseded',
          ),
        ),
      );
      await expectLater(
        shutdown,
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'power_superseded',
          ),
        ),
      );
      await tester.pumpAndSettle();
      workspace = tester.widget<WorkspaceShell>(find.byType(WorkspaceShell));
      expect(api.actions, ['shutdown', 'stop']);
      expect(workspace.machines.single.powerState, 'stopped');
      expect(workspace.machines.single.powerBusy, isFalse);
      expect(workspace.machines.single.canStop, isFalse);
    },
  );
  testWidgets(
    'The tile ⏻ menu stops a running VM with one confirmed stop request',
    (tester) async {
      final api = PowerBroker()
        ..assigned = [desktop(state: 'running', canForceStop: true)];
      await login(tester, api);
      await tester.tap(find.byKey(const Key('desktop-power-qa-vm')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('power-stop-qa-vm')));
      await tester.pumpAndSettle();
      expect(api.submitted, 0, reason: '정지 asks first.');
      expect(find.text('데스크톱을 정지할까요?'), findsOneWidget);
      await tester.tap(find.byKey(const Key('confirm-power-stop-qa-vm')));
      await tester.pump();
      expect(api.submitted, 1);
      expect(api.action, 'stop');
      await tester.pump(const Duration(seconds: 1));
      await tester.pumpAndSettle();
    },
  );
  testWidgets(
    'Without the broker stop permission (an older broker) the app refuses stop before sending',
    (tester) async {
      final api = PowerBroker()..assigned = [desktop(state: 'running')];
      await login(tester, api);
      final workspace = tester.widget<WorkspaceShell>(
        find.byType(WorkspaceShell),
      );
      expect(workspace.machines.single.canStop, isFalse);
      expect(workspace.machines.single.canShutdown, isTrue);
      await expectLater(
        workspace.onPower!('qa-vm', 'stop'),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'invalid_power_action',
          ),
        ),
      );
      for (final action in ['reset', 'suspend']) {
        await expectLater(
          workspace.onPower!('qa-vm', action),
          throwsA(
            isA<BrokerException>().having(
              (e) => e.code,
              'code',
              'invalid_power_action',
            ),
          ),
        );
      }
      expect(api.submitted, 0);
    },
  );
  testWidgets(
    'The tile power button reboots a running VM through the signed-in app (one confirmed request)',
    (tester) async {
      final api = PowerBroker()..assigned = [desktop(state: 'running')];
      await login(tester, api);
      await tester.tap(find.byKey(const Key('desktop-power-qa-vm')));
      await tester.pumpAndSettle();
      final reboot = tester.widget<MenuItemButton>(
        find.byKey(const Key('power-reboot-qa-vm')),
      );
      expect(reboot.onPressed, isNotNull, reason: 'Reboot must be offered.');
      await tester.tap(find.byKey(const Key('power-reboot-qa-vm')));
      await tester.pumpAndSettle();
      expect(api.submitted, 0, reason: 'Reboot asks first.');
      await tester.tap(find.byKey(const Key('confirm-power-reboot-qa-vm')));
      await tester.pump();
      expect(api.submitted, 1);
      expect(api.action, 'reboot');
      await tester.pump(const Duration(seconds: 1));
      await tester.pumpAndSettle();
    },
  );
  testWidgets(
    'A dialog opening over an open tile menu or account menu closes that menu first',
    (tester) async {
      final api = PowerBroker()..assigned = [desktop(state: 'running')];
      await login(tester, api);
      await tester.tap(find.byKey(const Key('desktop-power-qa-vm')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('power-reboot-qa-vm')), findsOneWidget);
      final context = tester.element(find.byType(WorkspaceShell));
      unawaited(
        showDialog<void>(
          context: context,
          builder: (_) => const AlertDialog(key: Key('fixture-dialog')),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('fixture-dialog')), findsOneWidget);
      expect(find.byKey(const Key('power-reboot-qa-vm')), findsNothing);
      Navigator.of(tester.element(find.byKey(const Key('fixture-dialog')))).pop();
      await tester.pumpAndSettle();
      // The account menu is closed the same way.
      await tester.tap(find.byKey(const Key('workspace-account')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('sign-out')), findsOneWidget);
      unawaited(
        showDialog<void>(
          context: context,
          builder: (_) => const AlertDialog(key: Key('fixture-dialog')),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('fixture-dialog')), findsOneWidget);
      expect(find.byKey(const Key('sign-out')), findsNothing);
      expect(tester.takeException(), isNull);
    },
  );
  testWidgets(
    'A ready running VM (host connected, one virtual monitor) reboots from the tile power button',
    (tester) async {
      final api = PowerBroker()
        ..assigned = [
          BrokerDesktop(
            id: 'qa-vm',
            name: 'QA VM',
            kind: 'vm',
            description: '',
            os: 'Windows',
            status: 'ready',
            canConnect: true,
            hostConnected: true,
            virtualMonitorControl: true,
            display: const VirtualDisplayState(
              supported: true,
              activeCount: 1,
              stale: false,
            ),
            monitors: const [
              RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
            ],
            power: BrokerPowerSnapshot(
              state: 'running',
              stale: false,
              checkedAt: DateTime.utc(2026, 10, 22),
            ),
          ),
        ];
      await login(tester, api);
      await tester.tap(find.byKey(const Key('desktop-power-qa-vm')));
      await tester.pumpAndSettle();
      expect(
        tester
            .widget<MenuItemButton>(find.byKey(const Key('power-reboot-qa-vm')))
            .onPressed,
        isNotNull,
      );
      await tester.tap(find.byKey(const Key('power-reboot-qa-vm')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('confirm-power-reboot-qa-vm')));
      await tester.pump();
      expect(api.submitted, 1);
      expect(api.action, 'reboot');
      await tester.pump(const Duration(seconds: 1));
      await tester.pumpAndSettle();
    },
  );
  testWidgets(
    'A fresh stopped VM is powered on from 켜기 after one question, once, without connecting',
    (t) async {
      final api = PowerBroker()..assigned = [desktop()];
      await login(t, api);
      expect(find.text('켜기'), findsWidgets);
      expect(find.text('켜고 연결'), findsNothing);
      await t.tap(find.byKey(const Key('start-qa-vm')));
      await t.pumpAndSettle();
      expect(find.text('데스크톱을 켤까요?'), findsOneWidget);
      expect(api.submitted, 0, reason: 'The main action asks once first.');
      await t.tap(find.byKey(const Key('confirm-power-start-qa-vm')));
      await t.pump();
      expect(api.submitted, 1);
      expect(api.action, 'start');
      await t.pump(const Duration(seconds: 1));
      await t.pumpAndSettle();
      expect(api.submitted, 1);
      expect(
        t
            .widget<WorkspaceShell>(find.byType(WorkspaceShell))
            .machines
            .single
            .powerState,
        'running',
        reason: 'The state follows the broker without pressing 새로고침.',
      );
      expect(find.byKey(const Key('cancel-start-qa-vm')), findsNothing);
    },
  );  testWidgets(
    'App submits once, prevents duplicates while pending, polls completion and refreshes actual power state',
    (tester) async {
      final api = PowerBroker()..assigned = [desktop()];
      await login(tester, api);
      var workspace = tester.widget<WorkspaceShell>(
        find.byType(WorkspaceShell),
      );
      expect(workspace.machines.single.canStart, isTrue);
      final operation = workspace.onPower!('qa-vm', 'start');
      await tester.pump();
      workspace = tester.widget<WorkspaceShell>(find.byType(WorkspaceShell));
      expect(workspace.machines.single.powerBusy, isTrue);
      await expectLater(
        workspace.onPower!('qa-vm', 'start'),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'power_in_progress',
          ),
        ),
      );
      await tester.pump(const Duration(seconds: 1));
      await tester.pumpAndSettle();
      await operation;
      workspace = tester.widget<WorkspaceShell>(find.byType(WorkspaceShell));
      expect(api.submitted, 1);
      expect(api.polls, 1);
      expect(api.reads, greaterThan(1));
      expect(workspace.machines.single.powerState, 'running');
      expect(workspace.machines.single.powerBusy, isFalse);
      expect(workspace.machines.single.canShutdown, isTrue);
    },
  );
  testWidgets(
    'Stale state cannot submit and failed tasks never return success',
    (tester) async {
      final api = PowerBroker()..assigned = [desktop(stale: true)];
      await login(tester, api);
      expect(find.byKey(const Key('start-qa-vm')), findsNothing);
      expect(find.text('VM 켜기'), findsNothing);
      var workspace = tester.widget<WorkspaceShell>(
        find.byType(WorkspaceShell),
      );
      await expectLater(
        workspace.onPower!('qa-vm', 'start'),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'power_status_unavailable',
          ),
        ),
      );
      expect(api.submitted, 0);
      api.assigned = [desktop()];
      workspace.onRefresh();
      await tester.pumpAndSettle();
      workspace = tester.widget<WorkspaceShell>(find.byType(WorkspaceShell));
      api.failTask = true;
      final failed = expectLater(
        workspace.onPower!('qa-vm', 'start'),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'power_task_failed',
          ),
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(seconds: 1));
      await tester.pumpAndSettle();
      await failed;
      expect(api.submitted, 1);
      expect(api.polls, 1);
    },
  );
}

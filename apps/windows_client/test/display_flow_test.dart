import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestBroker;
import 'display_api_test.dart' show displayResponse;
import 'power_api_test.dart' show taskId;
import 'helpers/pump_work.dart';

VirtualDisplayState state({
  String status = 'pending',
  int active = 2,
  bool stale = true,
  bool busy = true,
}) => VirtualDisplayState.fromJson(
  displayResponse(
        status: status,
        active: active,
        stale: stale,
        busy: busy,
      )['display']
      as Map<String, dynamic>,
);
BrokerDesktop desktop({
  bool capability = true,
  VirtualDisplayState? display,
  int count = 2,
}) => BrokerDesktop(
  id: 'qa-vm',
  name: 'QA VM',
  kind: 'vm',
  description: '',
  os: 'Windows',
  status: 'ready',
  canConnect: true,
  hostConnected: true,
  virtualMonitorControl: capability,
  display:
      display ??
      VirtualDisplayState(
        supported: true,
        activeCount: count,
        checkedAt: DateTime.utc(2026, 10, 22),
        stale: false,
      ),
  monitors: [
    const RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
    if (count == 2) const RemoteMonitor(id: '1', width: 1920, height: 1080),
  ],
  power: const BrokerPowerSnapshot(state: 'running', stale: false),
);

class DisplayBroker extends TestBroker {
  int submitted = 0, polls = 0;
  bool unknown = false, delayedTopology = false;
  bool stalled = false;
  @override
  Future<VirtualDisplayState> configureDisplays(
    String token,
    String id,
    int count,
  ) async {
    if (token != 'test-token' || id != 'qa-vm' || count != 1) {
      throw StateError('Unexpected display request fixture.');
    }
    submitted++;
    final pending = state();
    assigned = [desktop(display: pending)];
    if (stalled) return Completer<VirtualDisplayState>().future;
    return pending;
  }

  @override
  Future<VirtualDisplayState> display(
    String token,
    String id, {
    String? operationId,
  }) async {
    if (operationId != taskId) {
      throw StateError('Unexpected operation fixture.');
    }
    polls++;
    final result = state(
      status: unknown ? 'unknown' : 'succeeded',
      active: 1,
      stale: unknown,
      busy: unknown,
    );
    assigned = [
      desktop(display: result, count: delayedTopology && polls == 1 ? 2 : 1),
    ];
    return result;
  }
}

Future<void> login(WidgetTester tester, DisplayBroker api) async {
  tester.view.physicalSize = const Size(1440, 1000);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(
    MultitapApp(api: api, serverOrigin: Uri.parse('https://cloud.example.com')),
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

WorkspaceShell workspace(WidgetTester tester) =>
    tester.widget<WorkspaceShell>(find.byType(WorkspaceShell));

void main() {
  testWidgets(
    'An unresponsive display command times out without success and leaves observed pending state blocked',
    (tester) async {
      final api = DisplayBroker()
        ..assigned = [desktop()]
        ..stalled = true;
      await login(tester, api);
      final result = expectLater(
        workspace(tester).onConfigureDisplays!('qa-vm', 1),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'display_operation_timeout',
          ),
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(seconds: 30));
      await tester.pump();
      await result;
      expect(api.submitted, 1);
      expect(api.polls, 0);
      expect(api.connects, 0);
      expect(workspace(tester).machines.single.display.busy, isTrue);
      expect(workspace(tester).machines.single.display.activeCount, 2);
    },
  );
  testWidgets(
    'A confirmed count change blocks connections and power until the real topology matches; the launcher menu has no count',
    (tester) async {
      final api = DisplayBroker()
        ..assigned = [desktop()]
        ..delayedTopology = true;
      await login(tester, api);
      // ⋯ has no virtual-monitor count: connecting prepares
      // it, and the session toolbar asks before changing it
      // (display_remote_test).
      await tester.tap(find.byKey(const Key('desktop-more-qa-vm')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('details-qa-vm')), findsOneWidget);
      expect(find.byKey(const Key('display-menu-qa-vm')), findsNothing);
      expect(find.byKey(const Key('display-count-qa-vm-1')), findsNothing);
      await tester.tapAt(const Offset(4, 990));
      await tester.pumpAndSettle();
      expect(api.submitted, 0);

      // The session toolbar's confirmed change is this app command.
      DesktopMachine? applied;
      unawaited(
        workspace(
          tester,
        ).onConfigureDisplays!('qa-vm', 1).then((value) => applied = value),
      );
      await tester.pump();
      expect(api.submitted, 1);
      expect(workspace(tester).machines.single.isReady, isFalse);
      expect(workspace(tester).machines.single.canShutdown, isFalse);
      for (final call in [
        () => workspace(tester).onConfigureDisplays!('qa-vm', 1),
        () => workspace(tester).onConnect!('qa-vm'),
        () => workspace(tester).onPower!('qa-vm', 'reboot'),
      ]) {
        await expectLater(
          call(),
          throwsA(
            isA<BrokerException>().having(
              (e) => e.code,
              'code',
              'display_busy',
            ),
          ),
        );
      }
      await tester.pump(const Duration(seconds: 1));
      await tester.pump();
      // ACK alone is insufficient: the list still advertises two monitors,
      // so the change is unfinished and connecting is still refused.
      expect(api.polls, 1);
      expect(applied, isNull);
      await expectLater(
        workspace(tester).onConnect!('qa-vm'),
        throwsA(
          isA<BrokerException>().having((e) => e.code, 'code', 'display_busy'),
        ),
      );
      await tester.pump(const Duration(seconds: 1));
      await tester.pumpAndSettle();
      expect(api.polls, 2);
      expect(api.submitted, 1);
      expect(api.connects, 0);
      expect(applied?.display.activeCount, 1);
      expect(workspace(tester).machines.single.display.activeCount, 1);
      expect(workspace(tester).machines.single.monitors, hasLength(1));
    },
  );
  testWidgets(
    'Legacy host stays disabled and unknown result never returns success or reconnects',
    (tester) async {
      final api = DisplayBroker()..assigned = [desktop(capability: false)];
      await login(tester, api);
      expect(workspace(tester).machines.single.canConfigureDisplays, isFalse);
      await expectLater(
        workspace(tester).onConfigureDisplays!('qa-vm', 1),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'display_not_supported',
          ),
        ),
      );
      expect(api.submitted, 0);
      api.assigned = [desktop()];
      api.unknown = true;
      workspace(tester).onRefresh();
      await tester.pumpAndSettle();
      final operation = expectLater(
        workspace(tester).onConfigureDisplays!('qa-vm', 1),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'display_operation_unknown',
          ),
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(seconds: 1));
      await pumpWork(tester);
      await operation;
      expect(api.connects, 0);
      expect(workspace(tester).machines.single.display.stale, isTrue);
      expect(workspace(tester).machines.single.display.busy, isTrue);
      expect(workspace(tester).machines.single.canConfigureDisplays, isFalse);
    },
  );
}

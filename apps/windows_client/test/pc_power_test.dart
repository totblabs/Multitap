import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestBroker;
import 'helpers/pump_work.dart';

// Local PC power (pc-power-v1) in the Windows client: parsing, the offered
// actions and the ⏻ flows against a recording broker. Nothing here reaches a
// PC; the broker's answers are fixtures.
const pcId = 'pc-a1234567-89ab-cdef-8123-0123456789ab';
const taskId = '6f1b4f7e-2c9d-4b8a-9e3f-0123456789ab';

Map<String, dynamic> pcJson({
  bool online = true,
  Object? pcPower = const {
    'protocol': 'pc-power-v1',
    'actions': ['shutdown', 'reboot'],
    'wake': true,
    'busy': false,
  },
}) => {
  'id': pcId,
  'name': 'TOWER-PC',
  'kind': 'pc',
  'description': '',
  'os': 'Windows',
  'status': online ? 'ready' : 'host_offline',
  'canConnect': online,
  'localPcApproved': true,
  'host': online ? {'connected': true} : {'connected': false},
  'pcPower': ?pcPower,
};

BrokerDesktop pc({
  bool online = true,
  Set<String> actions = const {'shutdown', 'reboot'},
  bool wake = true,
  bool supported = true,
}) => BrokerDesktop(
  id: pcId,
  name: 'TOWER-PC',
  kind: 'pc',
  description: '',
  os: 'Windows',
  status: online ? 'ready' : 'host_offline',
  canConnect: false,
  localPcApproved: true,
  hostConnected: online,
  pcPower: supported
      ? PcPowerState(supported: true, actions: actions, wake: wake)
      : PcPowerState.absent,
);

class PcBroker extends TestBroker {
  final requested = <String>[];
  String finalExit = 'OK';
  String finalStatus = 'stopped';
  @override
  Future<BrokerPowerResult> requestPower(
    String token,
    String id,
    String action,
  ) async {
    requested.add(action);
    return BrokerPowerResult(
      power: const BrokerPowerSnapshot(state: 'running', stale: false),
      task: BrokerPowerTask(
        id: taskId,
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
  }) async => BrokerPowerResult(
    power: const BrokerPowerSnapshot(state: 'running', stale: false),
    task: BrokerPowerTask(
      id: taskId!,
      desktopId: id,
      action: requested.last,
      status: finalStatus,
      exitStatus: finalStatus == 'stopped' ? finalExit : null,
    ),
  );
}

void main() {
  test('pcPower parses leniently; a VM never carries it', () {
    final online = BrokerDesktop.fromJson(pcJson());
    expect(online.pcPower.supported, isTrue);
    expect(online.pcPower.actions, {'shutdown', 'reboot'});
    expect(online.pcPower.wake, isTrue);
    expect(
      BrokerDesktop.fromJson(
        pcJson(
          pcPower: {
            'protocol': 'pc-power-v1',
            'actions': ['start', 'stop', 'format', 7],
            'wake': false,
          },
        ),
      ).pcPower.actions,
      {'start'},
      reason: 'Only start, shutdown and reboot are ever offered.',
    );
    for (final bad in <Object?>[
      null,
      'yes',
      {'protocol': 'pc-power-v2', 'actions': <String>[], 'wake': true},
      {'protocol': 'pc-power-v1', 'actions': 'shutdown', 'wake': true},
      {
        'protocol': 'pc-power-v1',
        'actions': <String>[],
        'wake': 'true',
      },
    ]) {
      expect(
        BrokerDesktop.fromJson(pcJson(pcPower: bad)).pcPower.supported,
        isFalse,
      );
    }
    final busy = BrokerDesktop.fromJson(
      pcJson(
        pcPower: {
          'protocol': 'pc-power-v1',
          'actions': <String>[],
          'wake': true,
          'busy': true,
          'task': {
            'id': taskId,
            'desktopId': pcId,
            'action': 'reboot',
            'status': 'submitted',
            'exitStatus': null,
            'submittedAt': '2026-11-02T00:00:00.000Z',
            'checkedAt': null,
          },
        },
      ),
    );
    expect(busy.powerBusy, isTrue);
    expect(busy.powerTask?.action, 'reboot');
    final vm = BrokerDesktop.fromJson({
      ...pcJson(),
      'id': 'pve-170',
      'kind': 'vm',
    });
    expect(vm.pcPower.supported, isFalse);
  });

  test('A PC offers 종료/다시 시작 online, 켜기 offline with a wake address, never 정지', () {
    final online = machineFromBroker(pc());
    expect(
      [online.canShutdown, online.canReboot, online.canStart, online.canStop],
      [true, true, false, false],
    );
    expect(online.pcPowerRefusal('start'), 'invalid_power_state');
    expect(online.pcPowerRefusal('stop'), 'invalid_power_action');
    expect(online.pcPowerRefusal('reboot'), isNull);
    expect(online.pcPowerRefusal('reboot', pending: true), 'power_in_progress');
    final old = machineFromBroker(pc(actions: const {}));
    expect([old.canShutdown, old.canReboot], [false, false]);
    expect(old.pcPowerRefusal('reboot'), 'invalid_power_state');
    final offline = machineFromBroker(
      pc(online: false, actions: const {'start'}),
    );
    expect([offline.canStart, offline.canShutdown], [true, false]);
    final unreachable = machineFromBroker(
      pc(online: false, actions: const {}, wake: false),
    );
    expect(unreachable.pcPowerRefusal('start'), 'pc_wake_unavailable');
    final released = machineFromBroker(pc(supported: false));
    expect(released.pcPowerRefusal('reboot'), 'invalid_power_action');
  });

  Future<void> login(WidgetTester tester, PcBroker api) async {
    tester.view.physicalSize = const Size(1440, 900);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MultitapApp(api: api, serverOrigin: Uri.parse('https://cloud.example.com')),
    );
    await pumpWork(tester);
    await tester.enterText(find.byKey(const Key('sign-in-id')), 'qa-user');
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'qa-password',
    );
    await tester.tap(find.byKey(const Key('login-submit')));
    await pumpWork(tester);
  }

  testWidgets(
    'An online PC restarts from ⏻ after the same unsaved-work question; there is no 정지',
    (tester) async {
      final api = PcBroker()..assigned = [pc()];
      await login(tester, api);
      await tester.tap(find.byKey(const Key('desktop-power-$pcId')));
      await pumpWork(tester);
      expect(find.byKey(const Key('power-stop-$pcId')), findsNothing);
      expect(find.byKey(const Key('power-start-$pcId')), findsNothing);
      expect(find.byKey(const Key('power-shutdown-$pcId')), findsOneWidget);
      await tester.tap(find.byKey(const Key('power-reboot-$pcId')));
      await pumpWork(tester);
      expect(find.text('로컬 PC를 다시 시작할까요?'), findsOneWidget);
      expect(find.text('저장하지 않은 작업이 있으면 먼저 저장하세요.'), findsOneWidget);
      expect(api.requested, isEmpty, reason: 'It asks first.');
      await tester.tap(find.byKey(const Key('confirm-power-reboot-$pcId')));
      await tester.pump();
      expect(api.requested, ['reboot']);
      await tester.pump(const Duration(seconds: 1));
      await pumpWork(tester);
      expect(find.textContaining('Windows가 다시 시작 요청을 받았어요'), findsOneWidget);
    },
  );

  testWidgets('A Windows refusal keeps its fixed reason', (tester) async {
    final api = PcBroker()
      ..assigned = [pc()]
      ..finalExit = 'machine_locked';
    await login(tester, api);
    final workspace = tester.widget<WorkspaceShell>(find.byType(WorkspaceShell));
    final failed = expectLater(
      workspace.onPower!(pcId, 'shutdown'),
      throwsA(
        isA<BrokerException>()
            .having((e) => e.code, 'code', 'machine_locked')
            .having((e) => e.message, 'message', contains('잠겨')),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(seconds: 1));
    await pumpWork(tester);
    await failed;
    expect(api.requested, ['shutdown']);
  });

  testWidgets('A Host that announced nothing gets no ⏻ while online', (
    tester,
  ) async {
    final api = PcBroker()..assigned = [pc(actions: const {})];
    await login(tester, api);
    expect(find.byKey(const Key('desktop-power-$pcId')), findsNothing);
  });

  testWidgets(
    '켜기 without a wake address explains why in one line, with what WOL needs behind 자세히',
    (tester) async {
      final api = PcBroker()
        ..assigned = [pc(online: false, actions: const {}, wake: false)];
      await login(tester, api);
      await tester.tap(find.byKey(const Key('desktop-power-$pcId')));
      await pumpWork(tester);
      expect(find.byKey(const Key('power-shutdown-$pcId')), findsNothing);
      await tester.tap(find.byKey(const Key('power-start-$pcId')));
      await pumpWork(tester);
      expect(find.text('로컬 PC를 켜지 못했어요'), findsOneWidget);
      expect(find.text('서버가 이 로컬 PC의 랜카드 정보를 아직 받지 못했어요.'), findsOneWidget);
      expect(find.textContaining('Wake-on-LAN'), findsNothing);
      await tester.tap(find.byKey(const Key('pc-wake-details')));
      await pumpWork(tester);
      expect(find.textContaining('Wake-on-LAN'), findsOneWidget);
      expect(api.requested, isEmpty, reason: 'Nothing is sent.');
    },
  );

  testWidgets('켜기 that times out shows the wake help instead of a generic error', (
    tester,
  ) async {
    final api = PcBroker()
      ..assigned = [pc(online: false, actions: const {'start'})]
      ..finalExit = 'wake_timeout';
    await login(tester, api);
    await tester.tap(find.byKey(const Key('desktop-power-$pcId')));
    await pumpWork(tester);
    await tester.tap(find.byKey(const Key('power-start-$pcId')));
    await pumpWork(tester);
    expect(find.text('로컬 PC를 켤까요?'), findsOneWidget);
    await tester.tap(find.byKey(const Key('confirm-power-start-$pcId')));
    await tester.pump();
    expect(api.requested, ['start']);
    await tester.pump(const Duration(seconds: 1));
    await pumpWork(tester);
    expect(find.text('로컬 PC를 켜지 못했어요'), findsOneWidget);
    expect(find.text('켜기 신호를 보냈지만 4분 안에 켜지지 않았어요.'), findsOneWidget);
  });
}

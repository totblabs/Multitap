// The broker's hostMissingAfterUpdate on the launcher card (a Host update
// ends install_failed, the Host never comes back, and the card must say
// more than offline). Widget/model checks only: no broker, Host
// or VM is contacted. Every machine comes through the list JSON path.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';
import 'package:multitap_client/ui/readiness_dialog.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

const headline = 'Host가 업데이트 후 다시 연결되지 않았어요';
const guidance = 'Console로 데스크톱을 열고 Host 설정 → 진단을 확인하세요.';
const pcId = 'pc-b1234567-89ab-cdef-8123-0123456789ab';

/// One desktop list entry as the broker sends it.
Map<String, dynamic> listed(
  String id, {
  String kind = 'vm',
  String power = 'running',
  bool host = false,
  Object? missing,
  Map<String, dynamic>? operation,
}) => {
  'id': id,
  'name': 'WorkPC',
  'kind': kind,
  'description': '',
  'os': 'Windows 11',
  'status': host ? 'ready' : 'host_offline',
  'canConnect': false,
  'connectionModes': {'stream': false, 'rdp': false},
  if (kind == 'pc') 'localPcApproved': true,
  if (kind == 'vm') ...{
    'powerState': power,
    'powerCheckedAt': '2026-11-03T06:00:00.000Z',
    'powerStale': false,
    'powerBusy': false,
    'vmState': host
        ? 'host_wait'
        : power == 'running'
        ? 'host_lost'
        : 'off',
    'vmStateReason': 'host_disconnected',
    'vmStateRevision': 7,
  },
  'host': host
      ? {
          'connected': true,
          'version': '0.2.145',
          'capabilities': {'interactiveSession': true},
        }
      : {'connected': false},
  'hostUpdate': {
    'protocol': 'host-update-v1',
    'connected': host,
    'supported': host,
    'version': host ? '0.2.145' : null,
    'latestVersion': '0.2.146',
    'newer': host,
    'available': false,
    'sessionActive': false,
    'operation': operation,
  },
  'hostMissingAfterUpdate': ?missing,
};

const failedAttempt = {
  'result': 'failed',
  'code': 'install_failed',
  'fromVersion': '0.2.145',
  'targetVersion': '0.2.146',
};

DesktopMachine machine(Map<String, dynamic> json) =>
    machineFromBroker(BrokerDesktop.fromJson(json));

Widget shell(List<DesktopMachine> machines) => MaterialApp(
  theme: buildAppTheme(brightness: Brightness.dark),
  home: MediaQuery(
    data: const MediaQueryData(size: Size(1440, 900)),
    child: WorkspaceShell(
      onSignOut: () {},
      onRefresh: () {},
      displayName: '백종헌',
      serverOrigin: 'https://vdi.example.com',
      localMonitors: 1,
      machines: machines,
      launcherMemory: SessionLauncherMemory(),
      onConnect: (_, {options = const StreamOptions()}) async =>
          throw StateError('detached windows only'),
      onPower: (_, _) async {},
      onConfigureDisplays: (_, _) async => throw StateError('fixture only'),
      onOpenWindow: (_, _, _) async =>
          const SessionWindowOpenResult(opened: false),
    ),
  ),
);

Future<void> show(WidgetTester t, List<DesktopMachine> machines) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = const Size(1440, 900);
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  await t.pumpWidget(shell(machines));
  await t.pump();
  await t.pump(const Duration(milliseconds: 300));
}

String? textOf(WidgetTester t, String key) =>
    t.widget<Text>(find.byKey(Key(key))).data;

// Never a claim that something removed the Host, never exclusions or
// switching protection off.
final claims = RegExp('백신|바이러스가|격리했|삭제했|차단했|제외|끄세요|끄고|해제하세요');

void main() {
  testWidgets(
    'A running VM whose Host did not come back after an update says so, with the steps behind 자세히',
    (t) async {
      // The failed update ended four minutes ago: its own red line would still
      // show for six more minutes; the guidance replaces it.
      final ended = DateTime.now().toUtc().subtract(const Duration(minutes: 4));
      await show(t, [
        machine(
          listed(
            'pve-247',
            missing: failedAttempt,
            operation: {
              'status': 'failed',
              'code': 'install_failed',
              'targetVersion': '0.2.146',
              'checkedAt': ended.toIso8601String(),
            },
          ),
        ),
        machine(listed('pve-248')),
      ]);
      expect(textOf(t, 'status-pve-247'), headline);
      expect(textOf(t, 'status-detail-pve-247'), guidance);
      expect(find.byKey(const Key('host-update-status-pve-247')), findsNothing);
      // The other Host only dropped: the released text, unchanged.
      expect(textOf(t, 'status-pve-248'), 'Host 연결이 끊겼어요');
      // One card design: the same size as any other card.
      expect(
        t.getSize(find.byKey(const Key('card-pve-247'))),
        t.getSize(find.byKey(const Key('card-pve-248'))),
      );
      await t.tap(find.byKey(const Key('stream-state-pve-247')));
      await t.pump(const Duration(milliseconds: 300));
      expect(find.byType(ReadinessDialog), findsOneWidget);
      final steps = textOf(t, 'readiness-detail-1')!;
      expect(steps, startsWith('Host 0.2.146 업데이트 실패 후 응답을 확인하지 못했어요.'));
      expect(steps, contains('Host 설정 → 진단'));
      expect(steps, isNot(contains('보호 기록')));
      expect(steps, isNot(contains('복원')));
      expect(steps, isNot(contains('다시 설치')));
      for (final widget in t.widgetList<Text>(find.byType(Text))) {
        expect(widget.data ?? '', isNot(matches(claims)));
      }
    },
  );

  test('The card reads the field only where the Host is really away', () {
    DesktopReadiness of(Map<String, dynamic> json) =>
        DesktopReadiness.of(machine(json));
    final shown = of(listed('pve-247', missing: failedAttempt));
    expect(shown.phase, TilePhase.attention);
    expect(shown.headline, headline);
    expect(shown.detail, guidance);
    expect(shown.primary, ReadinessAction.refresh);
    // A completed update or one without an answer names itself in the steps.
    expect(
      of(listed('pve-247', missing: {...failedAttempt, 'result': 'completed'}))
          .checks[1]
          .detail,
      startsWith('Host 0.2.146 업데이트 완료 보고 뒤 응답을 확인하지 못했어요.'),
    );
    // A PC has no Proxmox power: its card says the same.
    expect(
      of(listed(pcId, kind: 'pc', missing: failedAttempt)).headline,
      headline,
    );
    // A list entry without it (a released broker) keeps the released texts.
    expect(of(listed('pve-247')).headline, 'Host 연결이 끊겼어요');
    // Off is off; a connected Host is back.
    expect(
      of(listed('pve-247', power: 'stopped', missing: failedAttempt)).headline,
      '꺼져 있어요',
    );
    expect(
      of(listed('pve-247', host: true, missing: failedAttempt)).headline,
      isNot(headline),
    );
    // Anything but the broker's fixed shape is ignored.
    for (final bad in <Object>[
      true,
      'missing',
      {'result': 'rejected', 'targetVersion': '0.2.146'},
      {'result': 'failed', 'targetVersion': 'latest'},
    ]) {
      expect(
        of(listed('pve-247', missing: bad)).headline,
        'Host 연결이 끊겼어요',
        reason: '$bad',
      );
    }
  });
}

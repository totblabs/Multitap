import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/vm_resource_bars.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'pc_power_test.dart' show pcJson, pcId;

// A physical PC's card: CPU and memory from its own Host (broker "source":
// "host"), rendered by the same strip as a VM's, from the broker's list JSON.
const _gib = 1024 * 1024 * 1024;

Map<String, Object?> hostSample({int? uptime = 5400}) => {
  'source': 'host',
  'measuredBy': 'host',
  'checkedAt': DateTime.now().toUtc().toIso8601String(),
  'stale': false,
  'cpuUsagePercent': 37.5,
  'vcpus': 8,
  'memoryUsedBytes': 4 * _gib,
  'memoryTotalBytes': 16 * _gib,
  'uptimeSeconds': uptime,
};

DesktopMachine machine({bool online = true, Object? resources}) =>
    machineFromBroker(
      BrokerDesktop.fromJson({
        ...pcJson(online: online),
        'resources': online ? resources ?? hostSample() : null,
      }),
    );

Future<void> _launcher(WidgetTester t, List<DesktopMachine> machines) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = const Size(1440, 1600);
  addTearDown(t.view.reset);
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: WorkspaceShell(
        onSignOut: () {},
        onRefresh: () {},
        machines: machines,
        displayName: 'tester',
        serverOrigin: 'https://cloud.example.com',
        onConnect: (_, {options = const StreamOptions()}) async =>
            throw StateError('fixture only'),
        onPower: (_, _) async {},
      ),
    ),
  );
  await t.pumpAndSettle();
}

Finder _in(String key, Finder matching) =>
    find.descendant(of: find.byKey(Key(key)), matching: matching);

void main() {
  test('a Host-measured PC sample is read like a Proxmox one', () {
    final sample = VmResources.fromJson(hostSample());
    expect(sample.measured, isTrue);
    expect(sample.measuredByHost, isTrue);
    expect(sample.cpuUsagePercent, 37.5);
    expect(sample.vcpus, 8);
    expect(sample.memoryFraction, .25);
    expect(sample.uptimeSeconds, 5400);
    expect(
      VmResources.fromJson({...hostSample(), 'source': 'pc'}).measured,
      isFalse,
    );
    // The broker sends no VM power fields for a PC: its Host decides.
    expect(machine().powerState, isNot('running'));
    expect(machine().resourcesRunning, isTrue);
    expect(machine(online: false).resourcesRunning, isFalse);
  });

  testWidgets(
    'a connected PC card shows live CPU and memory and its uptime; an offline one does not; same card size as a VM',
    (t) async {
      final vm = DesktopMachine(
        id: 'pve-149',
        name: 'VM 149',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.ready,
        specification: 'Windows 11 Pro',
        accent: Colors.blue,
        hostOnline: true,
        interactiveSession: true,
        virtualDisplayReady: true,
        powerState: 'running',
        powerStale: false,
        resources: VmResources.fromJson({...hostSample(), 'source': 'proxmox'}),
      );
      await _launcher(t, [machine(), vm]);
      final key = 'resources-$pcId';
      expect(_in(key, find.text('CPU 8코어')), findsOneWidget);
      expect(_in(key, find.text('38%')), findsOneWidget);
      expect(_in(key, find.text('메모리 16 GB')), findsOneWidget);
      expect(_in(key, find.text('25%')), findsOneWidget);
      // Gauges, not the hairline rail of a desktop without a measurement.
      expect(_in(key, find.byKey(const ValueKey('gauge'))), findsNWidgets(2));
      expect(t.widget<Text>(find.byKey(const Key('uptime-$pcId'))).data, '1시간');
      final tooltip = t
          .widgetList<Tooltip>(_in(key, find.byType(Tooltip)))
          .first
          .message!;
      expect(tooltip, startsWith('로컬 PC에서 측정'));
      expect(tooltip, isNot(contains('Proxmox')));
      expect(
        t.getSize(find.byKey(Key('card-$pcId'))),
        t.getSize(find.byKey(const Key('card-pve-149'))),
      );
      expect(
        t.getSize(find.byKey(Key(key))),
        t.getSize(find.byKey(const Key('resources-pve-149'))),
      );
      expect(t.takeException(), isNull);

      await _launcher(t, [machine(online: false), vm]);
      expect(_in(key, find.textContaining('%')), findsNothing);
      expect(_in(key, find.byKey(const ValueKey('gauge'))), findsNothing);
      expect(find.byKey(Key('uptime-$pcId')), findsNothing);
      expect(
        t.widgetList<Tooltip>(_in(key, find.byType(Tooltip))).first.message,
        '켜져 있을 때 사용량을 보여줘요.',
      );
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('a connected PC waits for its first sample with 측정 중', (t) async {
    final waiting = machine(resources: const <String, Object?>{});
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: Scaffold(
          body: SizedBox(
            width: 300,
            child: VmResourceStrip(
              sample: waiting.resources,
              running: waiting.resourcesRunning,
              pc: true,
            ),
          ),
        ),
      ),
    );
    expect(find.text('측정 중'), findsNWidgets(2));
    await t.pump(VmResourceStrip.measuringFor);
    await t.pumpAndSettle();
    expect(find.text('측정 중'), findsNothing);
  });
}

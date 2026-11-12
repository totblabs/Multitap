import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'android/android_launcher_test.dart' show signedIn;

const _gib = 1024 * 1024 * 1024;

VmResources _sample({
  double? cpu = 23,
  int? used = 5583457484,
  int? uptime = 11520,
  bool stale = false,
}) => VmResources(
  cpuUsagePercent: cpu,
  vcpus: 4,
  memoryUsedBytes: used,
  memoryTotalBytes: 8 * _gib,
  uptimeSeconds: uptime,
  checkedAt: DateTime.now().toUtc(),
  receivedAt: DateTime.now().toUtc(),
  stale: stale,
);

DesktopMachine _vm(
  String id, {
  String power = 'running',
  VmResources? resources,
  DesktopUse use = DesktopUse.idle,
}) => DesktopMachine(
  id: id,
  name: 'VM $id',
  description: '',
  kind: MachineKind.vm,
  status: power == 'stopped' ? MachineStatus.poweredOff : MachineStatus.ready,
  specification: 'Windows 11 Pro',
  accent: Colors.blue,
  hostOnline: power == 'running',
  interactiveSession: true,
  virtualDisplayReady: true,
  powerState: power,
  powerStale: false,
  resources: resources ?? _sample(),
  use: use,
);

Future<void> _launcher(
  WidgetTester t,
  List<DesktopMachine> machines, {
  Brightness brightness = Brightness.dark,
  double width = 1440,
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = Size(width, 1600);
  addTearDown(t.view.reset);
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: brightness),
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
  testWidgets(
    'a running tile shows CPU and memory use, the uptime beside 실행 중 and keeps its size',
    (t) async {
      await _launcher(t, [
        _vm('on'),
        _vm(
          'off',
          power: 'stopped',
          resources: _sample(cpu: null, used: null, uptime: null),
        ),
        _vm('stale', resources: _sample(stale: true)),
      ]);
      // Live: the configured size as the label, the share in use as the value.
      expect(_in('resources-on', find.text('CPU 4코어')), findsOneWidget);
      expect(_in('resources-on', find.text('23%')), findsOneWidget);
      expect(_in('resources-on', find.text('메모리 8 GB')), findsOneWidget);
      expect(_in('resources-on', find.text('65%')), findsOneWidget);
      expect(_in('power-state-on', find.text('실행 중')), findsOneWidget);
      expect(find.byKey(const Key('uptime-on')), findsOneWidget);
      expect(t.widget<Text>(find.byKey(const Key('uptime-on'))).data, '3시간');
      // Off or stale: the size alone in the same place, no share, no uptime.
      for (final id in ['off', 'stale']) {
        expect(_in('resources-$id', find.text('CPU 4코어')), findsOneWidget);
        expect(_in('resources-$id', find.text('메모리 8 GB')), findsOneWidget);
        expect(_in('resources-$id', find.textContaining('%')), findsNothing);
        expect(find.byKey(Key('uptime-$id')), findsNothing);
      }
      final sizes = [
        for (final id in ['on', 'off', 'stale'])
          t.getSize(find.byKey(Key('card-$id'))),
      ];
      expect(sizes.toSet().length, 1);
      final strips = [
        for (final id in ['on', 'off', 'stale'])
          t.getSize(find.byKey(Key('resources-$id'))),
      ];
      expect(strips.toSet().length, 1);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'a running VM says 측정 중 until its first sample, for at most 20 s, at the same size',
    (t) async {
      final lapsed = VmResources(
        cpuUsagePercent: 23,
        vcpus: 4,
        memoryUsedBytes: 5583457484,
        memoryTotalBytes: 8 * _gib,
        receivedAt: DateTime.now().toUtc().subtract(
          const Duration(seconds: 40),
        ),
      );
      final machines = [
        _vm('on'),
        _vm('wait', resources: const VmResources()),
        _vm(
          'off',
          power: 'stopped',
          resources: _sample(cpu: null, used: null, uptime: null),
        ),
        _vm('lapsed', resources: lapsed),
      ];
      await _launcher(t, machines);
      expect(_in('resources-wait', find.text('측정 중')), findsNWidgets(2));
      for (final id in ['on', 'off', 'lapsed']) {
        expect(_in('resources-$id', find.text('측정 중')), findsNothing);
      }
      final sizes = {
        for (final id in ['on', 'wait', 'off', 'lapsed'])
          t.getSize(find.byKey(Key('card-$id'))),
      };
      expect(sizes, hasLength(1));
      final strips = {
        for (final id in ['on', 'wait', 'off', 'lapsed'])
          t.getSize(find.byKey(Key('resources-$id'))),
      };
      expect(strips, hasLength(1));
      // No sample for 20 s is no measurement: the quiet rail alone.
      await t.pump(const Duration(seconds: 19));
      expect(_in('resources-wait', find.text('측정 중')), findsNWidgets(2));
      await t.pump(const Duration(seconds: 2));
      await t.pumpAndSettle();
      expect(_in('resources-wait', find.text('측정 중')), findsNothing);
      // A new start waits again; its first sample replaces the words.
      await _launcher(t, [_vm('wait', power: 'stopped'), ...machines.skip(2)]);
      await _launcher(t, [_vm('wait', resources: const VmResources())]);
      expect(_in('resources-wait', find.text('측정 중')), findsNWidgets(2));
      await _launcher(t, [_vm('wait')]);
      expect(_in('resources-wait', find.text('측정 중')), findsNothing);
      expect(_in('resources-wait', find.text('23%')), findsOneWidget);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('a new sample shows at once, in its load tone', (t) async {
    await _launcher(t, [_vm('vm')]);
    await _launcher(t, [_vm('vm', resources: _sample(cpu: 83))]);
    // Settled on the new value, in the high-load tone.
    expect(_in('resources-vm', find.text('83%')), findsOneWidget);
    await _launcher(t, [_vm('vm', resources: _sample(cpu: 23))]);
    final p = AppPalette(true);
    final value = t.widget<Text>(_in('resources-vm', find.text('23%')));
    expect(value.style?.color, p.ink);
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: WorkspaceShell(
          onSignOut: () {},
          onRefresh: () {},
          machines: [_vm('vm', resources: _sample(cpu: 97))],
          displayName: 'tester',
          serverOrigin: 'https://cloud.example.com',
        ),
      ),
    );
    // The same frame: easing kept the whole window redrawing between samples.
    final high = t.widget<Text>(_in('resources-vm', find.text('97%')));
    expect(high.style?.color, p.red);
  });

  testWidgets(
    'a connection that holds the VM is said on the tile, never whose',
    (t) async {
      await _launcher(t, [
        _vm('mine', use: DesktopUse.mine),
        _vm('other', use: DesktopUse.other),
        _vm('idle'),
      ]);
      expect(
        t.widget<Text>(find.byKey(const Key('status-detail-mine'))).data,
        '내 계정으로 연결돼 있어요',
      );
      expect(
        t.widget<Text>(find.byKey(const Key('status-detail-other'))).data,
        '다른 사용자가 사용 중이에요',
      );
      expect(find.byKey(const Key('desktop-in-use-mine')), findsOneWidget);
      expect(find.byKey(const Key('desktop-in-use-other')), findsOneWidget);
      expect(find.byKey(const Key('desktop-in-use-idle')), findsNothing);
      expect(
        t.widget<Text>(find.byKey(const Key('status-detail-idle'))).data,
        startsWith('기본 연결'),
      );
      // Still connectable: the broker decides (an administrator may take over).
      expect(
        t.widget<FilledButton>(find.byKey(const Key('open-other'))).onPressed,
        isNotNull,
      );
    },
  );

  testWidgets(
    'the Android tile (the Windows one) shows idle metrics and current in-use status',
    (t) async {
      BrokerDesktop desktop(
        String id, {
        String power = 'running',
        DesktopUse use = DesktopUse.idle,
        String? useAccount,
        VmResources? resources,
      }) => BrokerDesktop(
        id: id,
        name: 'VM $id',
        kind: 'vm',
        description: '',
        os: 'Windows 11 · 4 vCPU',
        status: power == 'stopped' ? 'powered_off' : 'online',
        canConnect: power == 'running',
        canStream: power == 'running',
        hostConnected: power == 'running',
        power: BrokerPowerSnapshot(
          state: power,
          stale: false,
          checkedAt: DateTime.utc(2026, 10, 26),
        ),
        resources: resources ?? _sample(),
        use: use,
        useAccount: useAccount,
      );
      await signedIn(t, [
        desktop('a'),
        desktop(
          'b',
          power: 'stopped',
          resources: _sample(cpu: null, used: null, uptime: null),
        ),
      ]);
      // The tile keeps the CPU and memory strip and the status card in every
      // state, as on Windows.
      expect(_in('resources-a', find.text('CPU 4코어')), findsOneWidget);
      expect(_in('resources-a', find.text('23%')), findsOneWidget);
      expect(_in('resources-a', find.text('메모리 8 GB')), findsOneWidget);
      expect(_in('resources-a', find.text('65%')), findsOneWidget);
      expect(_in('power-state-a', find.text('실행 중')), findsOneWidget);
      expect(_in('card-b', find.textContaining('%')), findsNothing);
      expect(_in('card-b', find.text('꺼져 있어요')), findsOneWidget);
      expect(find.byKey(const Key('uptime-a')), findsOneWidget);
      expect(t.widget<Text>(find.byKey(const Key('uptime-a'))).data, '3시간');
      expect(find.byKey(const Key('uptime-b')), findsNothing);
      expect(
        t.getSize(find.byKey(const Key('card-a'))),
        t.getSize(find.byKey(const Key('card-b'))),
      );
      // The broker names the account: the card says who.
      await t.pumpWidget(const SizedBox.shrink());
      await t.pumpAndSettle();
      await signedIn(t, [desktop('a', use: DesktopUse.other, useAccount: 'alice')]);
      expect(_in('card-a', find.text('alice 님이 사용 중이에요')), findsOneWidget);
      expect(find.text('다른 사용자가 사용 중이에요'), findsNothing);
      expect(t.takeException(), isNull);

      // A VM in use: its status card says so.
      await t.pumpWidget(const SizedBox.shrink());
      await t.pumpAndSettle();
      await signedIn(t, [
        desktop('a', use: DesktopUse.other),
        desktop(
          'b',
          power: 'stopped',
          resources: _sample(cpu: null, used: null, uptime: null),
        ),
      ]);
      expect(_in('card-a', find.text('다른 사용자가 사용 중이에요')), findsOneWidget);
      // The Windows tile (on Android too): the use in the status
      // card with its sign, the CPU and memory strip kept.
      expect(find.byKey(const Key('desktop-in-use-a')), findsOneWidget);
      expect(find.byKey(const Key('resources-a')), findsOneWidget);
      expect(_in('card-b', find.textContaining('%')), findsNothing);
      expect(find.byKey(const Key('uptime-a')), findsOneWidget);
      expect(find.byKey(const Key('uptime-b')), findsNothing);
      expect(
        t.getSize(find.byKey(const Key('card-a'))),
        t.getSize(find.byKey(const Key('card-b'))),
      );
      // The broker names the account: the card says who.
      await t.pumpWidget(const SizedBox.shrink());
      await t.pumpAndSettle();
      await signedIn(t, [desktop('a', use: DesktopUse.other, useAccount: 'alice')]);
      expect(_in('card-a', find.text('alice 님이 사용 중이에요')), findsOneWidget);
      expect(find.text('다른 사용자가 사용 중이에요'), findsNothing);
      expect(t.takeException(), isNull);
    },
  );
}

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/virtual_display.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';

const zero = RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true);
const one = RemoteMonitor(id: '1', width: 1280, height: 720);

VirtualDisplayState observed({
  int? count = 2,
  bool stale = false,
  bool busy = false,
  bool supported = true,
  bool missingTime = false,
  DateTime? checkedAt,
  VirtualDisplayOperation? operation,
}) => VirtualDisplayState(
  supported: supported,
  activeCount: count,
  stale: stale,
  busy: busy,
  checkedAt: missingTime ? null : checkedAt ?? DateTime.utc(2026, 10, 31),
  operation: operation,
);

DesktopMachine machine({
  VirtualDisplayState? display,
  List<RemoteMonitor> monitors = const [zero, one],
  bool? hostOnline = true,
  bool? displayReady = true,
  bool editable = true,
  bool powerBusy = false,
  MachineStatus status = MachineStatus.ready,
  SessionCleanupState cleanup = const SessionCleanupState(),
}) => DesktopMachine(
  id: 'pve-245',
  name: 'fixture',
  description: '',
  kind: MachineKind.vm,
  status: status,
  specification: '',
  accent: Colors.blue,
  hostOnline: hostOnline,
  virtualDisplayReady: displayReady,
  interactiveSession: true,
  powerState: 'running',
  powerStale: false,
  powerBusy: powerBusy,
  display: display ?? observed(),
  monitors: monitors,
  virtualMonitorControl: editable,
  sessionCleanup: cleanup,
);

void main() {
  test(
    'counts the full owned topology, independently of its order or view',
    () {
      expect(machine().verifiedOwnedDisplayCount, 2);
      expect(machine(monitors: const [one, zero]).verifiedOwnedDisplayCount, 2);
      expect(
        machine(
          display: observed(count: 1),
          monitors: const [zero],
        ).verifiedOwnedDisplayCount,
        1,
      );
    },
  );

  test('a known topology does not grant permission to change it', () {
    final m = machine(display: observed(supported: false), editable: false);
    expect(m.verifiedOwnedDisplayCount, 2);
    expect(m.canConfigureDisplays, isFalse);
    final check = DesktopReadiness.of(m).checks[2];
    expect(check.state, CheckState.ok);
    expect(check.detail, startsWith('2개 준비됨'));
  });

  test('broker freshness wins over the local clock', () {
    for (final date in [DateTime.utc(2000), DateTime.utc(2100)]) {
      expect(
        machine(display: observed(checkedAt: date)).verifiedOwnedDisplayCount,
        2,
      );
      expect(
        machine(display: observed(checkedAt: date, stale: true))
            .verifiedOwnedDisplayCount,
        isNull,
      );
    }
  });

  final unavailable = <String, DesktopMachine Function()>{
    'missing time': () => machine(display: observed(missingTime: true)),
    'stale': () => machine(display: observed(stale: true)),
    'busy': () => machine(display: observed(busy: true)),
    'pending with retained two': () => machine(display: observed().pending()),
    'offline': () => machine(hostOnline: false),
    'unknown host': () => machine(hostOnline: null),
    'display not ready': () => machine(displayReady: false),
    'unknown display readiness': () => machine(displayReady: null),
    'power pending': () => machine(powerBusy: true),
    'powered off': () => machine(status: MachineStatus.poweredOff),
    'cleanup pending': () => machine(
      cleanup: const SessionCleanupState(pending: true, workerQuiescent: true),
    ),
    'no count': () => machine(display: observed(count: null)),
    'zero count': () => machine(display: observed(count: 0)),
    'unsupported count': () => machine(display: observed(count: 3)),
    'missing output': () => machine(monitors: const [zero]),
    'extra output': () => machine(display: observed(count: 1)),
    'duplicate ID': () => machine(monitors: const [zero, zero]),
    'foreign ID': () => machine(
      monitors: const [
        zero,
        RemoteMonitor(id: '2', width: 1920, height: 1080),
      ],
    ),
    'missing owned zero': () =>
        machine(display: observed(count: 1), monitors: const [one]),
  };
  for (final entry in unavailable.entries) {
    test('${entry.key} never substitutes a count', () {
      expect(entry.value().verifiedOwnedDisplayCount, isNull);
    });
  }

  test('stale metadata cannot describe a ready count in the checklist', () {
    for (final state in [observed(stale: true), observed(missingTime: true)]) {
      final check = DesktopReadiness.of(
        machine(display: state),
      ).checks[2];
      expect(check.state, CheckState.unknown);
      expect(check.detail, '화면 개수를 아직 받지 못했어요');
      expect(check.action, ReadinessAction.refresh);
      expect(check.detail, isNot(contains('2개 준비됨')));
    }
  });

  test('unknown operation stays unknown despite a fresh observed count', () {
    const id = 'ea6f0efc-7db1-4b6b-9290-e3aa21c0d91d';
    const operation = VirtualDisplayOperation(
      id: id,
      status: 'unknown',
      requestedCount: 1,
      error: 'host_disconnected',
    );
    final m = machine(display: observed(operation: operation));
    expect(m.verifiedOwnedDisplayCount, 2);
    expect(m.display.operation?.status, 'unknown');
    expect(m.display.applied(2, id), isFalse);
  });

  test('invalid timestamp is rejected before a topology can be shown', () {
    expect(
      () => VirtualDisplayState.fromJson({
        'activeCount': 2,
        'stale': false,
        'checkedAt': 'not-a-timestamp',
      }),
      throwsFormatException,
    );
  });
}

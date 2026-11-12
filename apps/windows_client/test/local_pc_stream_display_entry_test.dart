// Actual PC model and entry helpers. No platform, display or network access.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/console_preparation.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/models/stream_options.dart';

DesktopMachine _pc({
  String id = 'pc-11111111-1111-4111-8111-111111111111',
  int? count = 1,
  bool online = true,
  bool control = true,
  bool supported = true,
  bool stale = false,
  bool busy = false,
  bool powerBusy = false,
  String powerState = 'running',
  bool cleanup = false,
  MachineStatus status = MachineStatus.ready,
  List<RemoteMonitor>? monitors,
}) => DesktopMachine(
  id: id,
  name: 'Synthetic local PC',
  description: '',
  kind: MachineKind.pc,
  localPcApproved: true,
  status: status,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: online,
  interactiveSession: true,
  virtualDisplayReady: true,
  virtualMonitorControl: control,
  powerState: powerState,
  powerStale: false,
  powerBusy: powerBusy,
  powerCanForceStop: true,
  consoleRelayProtocol: 'rfb-none-v1',
  consolePreparation: const ConsolePreparationState(
    supported: true,
    available: true,
  ),
  sessionCleanup: SessionCleanupState(pending: cleanup),
  display: VirtualDisplayState(
    supported: supported,
    activeCount: count,
    stale: stale,
    busy: busy,
    checkedAt: DateTime.utc(2026, 10, 29),
  ),
  monitors:
      monitors ??
      [
        for (var n = 0; n < (count ?? 0); n++)
          RemoteMonitor(id: '$n', width: 1920, height: 1080, primary: n == 0),
      ],
);

Matcher _code(String expected) => throwsA(
  isA<BrokerException>().having((error) => error.code, 'code', expected),
);

void main() {
  test('approved PC uses actual display capability without VM power or console control', () {
    final machine = _pc();
    expect(machine.canConfigureDisplays, isTrue);
    expect(_pc(powerState: 'stopped').canStart, isFalse);
    expect(machine.canShutdown, isFalse);
    expect(machine.canReboot, isFalse);
    expect(machine.canStop, isFalse);
    expect(machine.canRecoverConsole, isFalse);
    expect(machine.emergencyConsoleSupported, isFalse);
  });

  testWidgets(
    'ready PC finishes display-entry observation in the first turn without refresh',
    (tester) async {
      final machine = _pc();
      var current = true, refreshes = 0;
      DesktopMachine? observed;
      Object? failure;
      final done = Completer<void>();
      unawaited(
        waitForStreamDisplayControl(
          machine,
          refresh: () async {
            refreshes++;
            return machine;
          },
          current: () => current,
          maxWait: const Duration(seconds: 1),
        ).then<void>(
          (value) {
            observed = value;
            done.complete();
          },
          onError: (Object error, StackTrace stack) {
            failure = error;
            done.complete();
          },
        ),
      );
      await tester.pump();
      final firstTurn = observed;
      // The unchanged baseline enters its 500ms loop. Retire and drain it before
      // asserting, so its intended failure cannot leave a widget timer behind.
      current = false;
      await tester.pump(const Duration(seconds: 1));
      await done.future;
      expect(firstTurn, same(machine));
      expect(failure, isNull);
      expect(refreshes, 0);
    },
  );

  test('fresh PC count is accepted without any display mutation', () async {
    final machine = _pc();
    var changes = 0;
    expect(streamDisplayCountReady(machine, 1), isTrue);
    final fresh = await prepareStreamDisplayCount(machine, 1, (_) async {
      changes++;
      return machine;
    });
    expect(fresh, same(machine));
    expect(changes, 0);
  });

  test(
    'uncounted capable PC requires the normal exact display callback',
    () async {
      final machine = _pc(count: null), ready = _pc();
      expect(machine.canConfigureDisplays, isTrue);
      expect(streamDisplayCountReady(machine, 1), isFalse);
      final gate = Completer<DesktopMachine>(), counts = <int>[];
      final result = prepareStreamDisplayCount(machine, 1, (count) {
        counts.add(count);
        return gate.future;
      });
      var completed = false;
      unawaited(
        result.then<void>((_) {
          completed = true;
        }),
      );
      await Future<void>.value();
      expect(counts, [1]);
      expect(completed, isFalse);
      gate.complete(ready);
      expect(await result, same(ready));
    },
  );

  test(
    'PC capability, freshness, cleanup and busy guards remain required',
    () async {
      for (final blocked in [
        _pc(online: false),
        _pc(control: false),
        _pc(supported: false),
        _pc(stale: true),
        _pc(busy: true),
        _pc(powerBusy: true),
        _pc(cleanup: true),
      ]) {
        expect(blocked.canConfigureDisplays, isFalse);
        expect(streamDisplayCountReady(blocked, 1), isFalse);
      }
      expect(
        _pc(status: MachineStatus.poweredOff).canConfigureDisplays,
        isFalse,
      );
      var changes = 0;
      Future<DesktopMachine> configure(int count) async {
        changes++;
        return _pc();
      }

      await expectLater(
        prepareStreamDisplayCount(_pc(online: false), 1, configure),
        _code('host_offline'),
      );
      await expectLater(
        prepareStreamDisplayCount(_pc(control: false), 1, configure),
        _code('display_not_supported'),
      );
      await expectLater(
        prepareStreamDisplayCount(_pc(supported: false), 1, configure),
        _code('display_not_supported'),
      );
      await expectLater(
        prepareStreamDisplayCount(_pc(cleanup: true), 1, configure),
        _code('session_cleanup_pending'),
      );
      expect(changes, 0);
    },
  );

  test(
    'PC display completion still requires exact identity and fresh owned count',
    () async {
      for (final response in [
        _pc(id: 'pc-22222222-2222-4222-8222-222222222222'),
        _pc(stale: true),
        _pc(count: 2),
        _pc(
          monitors: const [RemoteMonitor(id: '1', width: 1920, height: 1080)],
        ),
      ]) {
        await expectLater(
          prepareStreamDisplayCount(_pc(count: null), 1, (_) async => response),
          _code('display_operation_unknown'),
        );
      }
    },
  );
}

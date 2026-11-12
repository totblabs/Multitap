// Broker snapshots and Flutter entry policy; no installed Host or Windows
// topology is changed. Exact Host worker exit is covered in native fixtures.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/data/console_resume.dart';
import 'package:multitap_client/models/console_preparation.dart';

import 'console_resume_test.dart' show RecoveryApi;

Map<String, dynamic> snapshot({Object? cleanup, bool console = false}) => {
  'id': 'pve-149',
  'name': 'TESTER',
  'kind': 'vm',
  'description': '',
  'os': 'Windows',
  'status': 'ready',
  'canConnect': true,
  'connectionModes': {'stream': true},
  'sessionCleanup': ?cleanup,
  'host': {
    'connected': true,
    'capabilities': {
      'interactiveSession': !console,
      'virtualDisplayReady': true,
      'virtualMonitorControl': true,
      'monitors': [
        {'id': '0', 'width': 1920, 'height': 1080, 'primary': true},
      ],
    },
  },
  'display': {
    'supported': true,
    'activeCount': 1,
    'stale': false,
    'busy': false,
  },
  if (console)
    'consolePreparation': {
      'supported': true,
      'available': true,
      'busy': false,
      'reason': 'rdp_session_disconnected',
    },
};
Map<String, Object> cleanup({bool quiet = false, String phase = 'pending'}) => {
  'pending': true,
  'phase': phase,
  'workerQuiescent': quiet,
  'checkedAt': '2026-10-25T00:00:00.000Z',
};
DesktopMachine machine({Object? state, bool console = false}) =>
    machineFromBroker(
      BrokerDesktop.fromJson(snapshot(cleanup: state, console: console)),
    );

void main() {
  test(
    'cached ready/count capabilities cannot bypass pending Host cleanup',
    () async {
      final value = machine(state: cleanup());
      expect(value.isReady, false);
      expect(value.canConfigureDisplays, false);
      expect(streamDisplayCountReady(value, 1), false);
      var changes = 0;
      await expectLater(
        prepareStreamDisplayCount(value, 1, (_) async {
          changes++;
          return machine();
        }),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'session_cleanup_pending',
          ),
        ),
      );
      expect(changes, 0);
    },
  );
  test('model updates preserve cleanup; retired capture permits only emergency control', () {
    // A fresh broker timestamp: this checks the normal short wait. Stalled
    // waits are covered in session_cleanup_ux_test.dart.
    final value = machine(
      state: {
        ...cleanup(quiet: true),
        'checkedAt': DateTime.now().toUtc().toIso8601String(),
      },
      console: true,
    );
    expect(value.canRecoverConsole, true);
    for (final changed in [
      value,
      value.withDisplay(value.display),
      value.withResources(value.resources),
      value.withPower(
        state: 'running',
        checkedAt: null,
        stale: false,
        busy: false,
      ),
    ]) {
      expect(changed.isReady, false);
      expect(changed.canConfigureDisplays, false);
      expect(changed.streamReadinessBadge, '화면 복원 중');
    }
    expect(machine(state: cleanup(), console: true).canRecoverConsole, false);
  });
  test('legacy absent state and exact completed state remain compatible', () {
    expect(machine().isReady, true);
    expect(machine(state: {'pending': false}).canConfigureDisplays, true);
  });
  test('malformed cleanup cannot silently become readiness', () {
    for (final state in [
      true,
      {},
      {'pending': 'false'},
      {'pending': false, 'workerQuiescent': true},
      cleanup()..remove('workerQuiescent'),
      cleanup(phase: 'success'),
      {...cleanup(), 'checkedAt': 'invalid'},
      {...cleanup(), 'workerQuiescent': 1},
    ]) {
      expect(
        () => BrokerDesktop.fromJson(snapshot(cleanup: state)),
        throwsFormatException,
      );
    }
  });
  testWidgets(
    'fresh pending state waits even when stale cached capabilities say ready',
    (t) async {
      var reads = 0;
      DesktopMachine? result;
      unawaited(
        waitForStreamDisplayControl(
          machine(state: cleanup()),
          refresh: () async {
            reads++;
            return machine();
          },
          current: () => true,
        ).then((value) => result = value),
      );
      await t.pump();
      expect(result, null);
      expect(reads, 0);
      await t.pump(const Duration(milliseconds: 500));
      expect(reads, 1);
      expect(result?.isReady, true);
    },
  );
  test('waiting yields to confirmed console recovery only after capture exit proof', () async {
    final value = machine(state: cleanup(quiet: true), console: true);
    expect(
      await waitForStreamDisplayControl(
        value,
        refresh: () async {
          fail('Do not poll past an actionable explicit console return.');
        },
        current: () => true,
      ),
      same(value),
    );
    expect(value.isReady, false);
  });
  testWidgets(
    'new remote window cannot connect until fresh cleanup barrier clears',
    (t) async {
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      for (final name in ['multitap/window', 'multitap/window/events']) {
        messenger.setMockMethodCallHandler(
          MethodChannel(name),
          (_) async => false,
        );
        addTearDown(
          () => messenger.setMockMethodCallHandler(MethodChannel(name), null),
        );
      }
      var reads = 0, connects = 0, changes = 0;
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(),
            prepareDisplaysOnEntry: true,
            onRefreshDesktop: () async =>
                ++reads == 1 ? machine(state: cleanup()) : machine(),
            onConfigureDisplays: (_) async {
              changes++;
              return machine();
            },
            onReconnect: (_) async {
              connects++;
              throw const BrokerException('host_maintenance');
            },
            load: () async => throw StateError('Unprepared connection'),
          ),
        ),
      );
      await t.pump();
      await t.pump();
      expect(connects, 0);
      expect(changes, 0);
      await t.pump(const Duration(milliseconds: 500));
      await t.pumpAndSettle();
      expect(reads, 2);
      expect(connects, 1);
      expect(changes, 0);
    },
  );

  testWidgets(
    'cleanup deadline cannot be extended by polling or a late HTTP result',
    (t) async {
      final pending = machine(state: cleanup());
      final blocked = Completer<DesktopMachine>();
      var reads = 0;
      Object? error;
      unawaited(
        waitForStreamDisplayControl(
          pending,
          refresh: () {
            reads++;
            return reads < 89 ? Future.value(pending) : blocked.future;
          },
          current: () => true,
        ).catchError((Object value) {
          error = value;
          return pending;
        }),
      );
      for (var n = 0; n < 89; n++) {
        await t.pump(const Duration(milliseconds: 500));
      }
      expect(reads, 89);
      expect(error, null);
      await t.pump(const Duration(milliseconds: 600));
      expect(
        error,
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'session_cleanup_pending',
        ),
      );
      blocked.complete(machine());
      await t.pump();
      await t.pump(const Duration(seconds: 10));
      expect(reads, 89);
    },
  );

  test(
    'confirmed console return waits for cleanup even after its own success ACK',
    () async {
      final api = RecoveryApi();
      var reads = 0;
      final operation = const ConsolePreparationState(
        supported: true,
        reason: 'console_ready',
        operation: ConsolePreparationOperation(
          id: '11111111-1111-4111-8111-111111111111',
          status: 'succeeded',
          attempted: true,
        ),
      );
      // The console-operation proof remains distinct from stream cleanup.
      final bound = operation.operation!;
      api.start = () async => ConsolePreparationState(
        supported: true,
        reason: 'console_ready',
        operation: bound,
      );
      await ConsoleResume(
        api: api,
        authorized: () => true,
        delay: (_) async {},
        loadDesktops: () async {
          final data = snapshot(
            cleanup: ++reads == 1 ? cleanup(quiet: true) : {'pending': false},
          );
          data['consolePreparation'] = {
            'supported': true,
            'available': false,
            'busy': false,
            'reason': 'console_ready',
            'operation': {
              'id': bound.id,
              'status': 'succeeded',
              'attempted': true,
              'error': null,
            },
          };
          return [BrokerDesktop.fromJson(data)];
        },
      ).run('fixture-token', 'pve-149');
      expect(reads, 2);
      expect(api.posts, 1);
      expect(api.polls, 0);
    },
  );
}

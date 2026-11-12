// The fixed fields come from an actual Host48 public-relay stop observation.
// Replay uses fake time and no connection, input or Windows display changes.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/models/stream_options.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final evidence = jsonDecode(
    File('../../server/test/fixtures/host48-stop-readiness.json')
        .readAsStringSync(),
  ) as Map<String, dynamic>;
  final samples = (evidence['samples'] as List).cast<Map<String, dynamic>>();

  DesktopMachine machine(int index, {bool broker31 = false}) {
    final sample = samples[index];
    final capabilities = sample['capabilities'] as Map<String, dynamic>;
    // Keep the recorded Broker30 result intact. This separate projection is
    // the Broker31 admission policy also tested through real HTTP/WS routes.
    final pending = broker31
        ? capabilities['virtualMonitorControl'] != true
        : sample['cleanupPending'] == true;
    return machineFromBroker(
      BrokerDesktop.fromJson({
        'id': 'pve-149',
        'name': 'TESTER',
        'description': '',
        'kind': 'vm',
        'os': 'Windows',
        'status': 'ready',
        'canConnect': true,
        'connectionModes': {'stream': capabilities['streaming']},
        'powerState': 'running',
        'powerCheckedAt': sample['display']['checkedAt'],
        'powerStale': false,
        'powerBusy': false,
        'display': sample['display'],
        'sessionCleanup': pending
            ? {
                'pending': true,
                'workerQuiescent': true,
                'phase': 'awaiting_capabilities',
                'checkedAt': sample['display']['checkedAt'],
              }
            : {'pending': false},
        'consolePreparation': {
          'supported': true,
          'available': false,
          'busy': false,
          'reason': 'console_ready',
        },
        'host': {'connected': true, 'capabilities': capabilities},
      }),
    );
  }

  testWidgets(
    'recorded zero-output gap reproduces Client44 count preflight failure',
    (t) async {
      expect(evidence['hostVersion'], '0.2.48');
      expect(machine(2).streamPreparation, StreamPreparation.display);
      var reads = 0, mutations = 0;
      final result =
          waitForStreamDisplayControl(
            machine(1),
            refresh: () async {
              reads++;
              return machine(2);
            },
            current: () => true,
          ).then(
            (fresh) => prepareStreamDisplayCount(fresh, 1, (_) async {
              mutations++;
              return machine(4);
            }),
          );
      final expected = expectLater(
        result,
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'display_not_supported',
          ),
        ),
      );
      await t.pump(const Duration(milliseconds: 501));
      await expected;
      expect(reads, 1);
      expect(mutations, 0);
    },
  );

  for (final count in [1, 2]) {
    testWidgets(
      'Broker31 cleanup projection keeps Client44 count $count read-only until fresh outputs return',
      (t) async {
        var reads = 0, mutations = 0, completed = false;
        int? requestedCount;
        bool? readyAtMutation;
        final result =
            waitForStreamDisplayControl(
              machine(1, broker31: true),
              refresh: () async =>
                  machine(++reads == 1 ? 2 : 4, broker31: true),
              current: () => true,
            ).then(
              (fresh) => prepareStreamDisplayCount(fresh, count, (
                requested,
              ) async {
                readyAtMutation = fresh.canConfigureDisplays;
                requestedCount = requested;
                mutations++;
                // Only this mock operation creates the requested second output.
                return DesktopMachine(
                  id: fresh.id,
                  name: fresh.name,
                  description: fresh.description,
                  kind: fresh.kind,
                  status: fresh.status,
                  specification: fresh.specification,
                  accent: fresh.accent,
                  hostOnline: true,
                  virtualMonitorControl: true,
                  display: const VirtualDisplayState(
                    supported: true,
                    activeCount: 2,
                    stale: false,
                  ),
                  monitors: [
                    fresh.monitors.single,
                    const RemoteMonitor(id: '1', width: 1280, height: 720),
                  ],
                );
              }),
            );
        unawaited(result.then((_) => completed = true));
        await t.pump(const Duration(milliseconds: 501));
        expect(reads, 1);
        expect(completed, isFalse);
        expect(mutations, 0);
        await t.pump(const Duration(milliseconds: 501));
        final fresh = await result;
        expect(fresh.display.activeCount, count);
        expect(reads, 2);
        expect(mutations, count == 1 ? 0 : 1);
        expect(requestedCount, count == 1 ? isNull : 2);
        expect(readyAtMutation, count == 1 ? isNull : isTrue);
      },
    );
  }
}

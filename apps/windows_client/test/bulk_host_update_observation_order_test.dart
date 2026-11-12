import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/bulk_host_updates.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/ui/bulk_host_update_dialog.dart';

import 'bulk_host_updates_test.dart' show FakeBulkApi, bulkMachine, bulkRelease;

void main() {
  for (final scenario in [
    'stale-success',
    'stale-error',
    'normal-success',
    'normal-error',
    'disposed',
  ]) {
    testWidgets(
      'bulk observation order $scenario keeps current assignment ownership',
      (t) async {
        final machine = bulkMachine('pve-149'), next = bulkMachine('pve-245');
        final live = ValueNotifier<List<DesktopMachine>>([machine]);
        final refresh = Completer<List<DesktopMachine>>();
        final api = FakeBulkApi();
        late BulkHostUpdateController controller;
        await t.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: BulkHostUpdateDialog(
                api: api,
                token: 'synthetic-token',
                server: 'https://fixture.invalid',
                assignedMachines: [machine],
                liveMachines: live,
                refreshAssigned: () => refresh.future,
                loadRelease: () async => bulkRelease,
                controllerFactory: (release, assigned) =>
                    controller = BulkHostUpdateController(
                      api: api,
                      token: 'synthetic-token',
                      release: release,
                      assignedMachines: assigned,
                    ),
              ),
            ),
          ),
        );
        try {
          await t.pumpAndSettle();
          expect(controller.rows.single.eligible, true);
          if (scenario.startsWith('stale')) {
            live.value = [];
            await t.pump();
            expect(controller.rows.single.assigned, false);
          }
          if (scenario == 'disposed') await t.pumpWidget(const SizedBox());
          if (scenario.endsWith('error')) {
            refresh.completeError(StateError('synthetic delayed read failed'));
          } else {
            refresh.complete(scenario == 'normal-success' ? [next] : [machine]);
          }
          await t.pump();
          await t.pump();
          if (scenario.startsWith('stale')) {
            expect(live.value, isEmpty);
            expect(
              controller.rows.single.assigned,
              false,
              reason: 'A late refresh must not resurrect an assignment removed by a newer push.',
            );
            expect(controller.rows.single.eligible, false);
            expect(controller.selectedCount, 0);
            expect(
              find.byKey(const Key('bulk-host-observation-failed')),
              findsNothing,
              reason: 'An older failed refresh must not replace a successful newer live observation.',
            );
          } else if (scenario == 'normal-success') {
            expect(
              controller.rows.singleWhere((r) => r.id == machine.id).assigned,
              false,
            );
            expect(
              controller.rows.singleWhere((r) => r.id == next.id).eligible,
              true,
            );
          } else if (scenario == 'normal-error') {
            expect(controller.rows.single.assigned, true);
            expect(
              find.byKey(const Key('bulk-host-observation-failed')),
              findsOneWidget,
            );
          }
          expect(
            api.calls,
            isEmpty,
            reason: 'This regression never submits an update.',
          );
          expect(t.takeException(), isNull);
        } finally {
          await t.pumpWidget(const SizedBox());
          live.dispose();
        }
      },
    );
  }
}

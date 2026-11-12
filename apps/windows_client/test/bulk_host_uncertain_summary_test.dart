import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/bulk_host_updates.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/bulk_host_update_dialog.dart';

import 'bulk_host_update_dialog_test.dart' show loadBulkFonts, confirmBulk;
import 'bulk_host_updates_test.dart';

void main() {
  for (final kind in ['uncertain', 'needsCheck', 'active', 'completed']) {
    testWidgets('visible bulk summary honestly represents $kind', (t) async {
      await loadBulkFonts(t);
      t.view.devicePixelRatio = 1;
      t.view.physicalSize = const Size(1100, 850);
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      final api = FakeBulkApi()
        ..handler = (_, _) async {
          if (kind == 'uncertain') {
            throw TimeoutException('synthetic transport only');
          }
          if (kind == 'active') {
            return bulkState(operation: bulkOperation('waiting'));
          }
          return bulkState(
            version: bulkTarget,
            operation: bulkOperation(
              'completed',
              code: kind == 'needsCheck' ? 'result_unreported' : '',
            ),
          );
        };
      var reads = 0;
      late BulkHostUpdateController controller;
      final machines = [bulkMachine('pve-fixture')];
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: Builder(
            builder: (context) => Scaffold(
              body: TextButton(
                key: const Key('open-summary-fixture'),
                onPressed: () => unawaited(
                  showDialog<void>(
                    context: context,
                    builder: (_) => BulkHostUpdateDialog(
                      api: api,
                      token: 'in-memory-token',
                      server: 'https://fixture.invalid',
                      assignedMachines: machines,
                      refreshAssigned: () async {
                        reads++;
                        return machines;
                      },
                      loadRelease: () async => bulkRelease,
                      controllerFactory: (release, assigned) =>
                          controller = BulkHostUpdateController(
                            api: api,
                            token: 'in-memory-token',
                            release: release,
                            assignedMachines: assigned,
                            minimumRequestInterval: Duration.zero,
                          ),
                    ),
                  ),
                ),
                child: const Text('open'),
              ),
            ),
          ),
        ),
      );
      await t.tap(find.byKey(const Key('open-summary-fixture')));
      await t.pumpAndSettle();
      await confirmBulk(t);
      await t.pumpAndSettle();
      expect(api.calls, hasLength(1));
      if (kind == 'uncertain' || kind == 'needsCheck') {
        expect(controller.finished, isTrue);
        expect(controller.checkingCount, 1);
        expect(controller.hasServerWork, kind == 'uncertain');
        expect(find.textContaining('모두 끝났어요'), findsNothing);
        expect(find.textContaining('진행 상태 확인 필요'), findsOneWidget);
      } else if (kind == 'active') {
        expect(controller.finished, isFalse);
        expect(find.textContaining('진행 중 1대'), findsOneWidget);
      } else {
        expect(controller.finished, isTrue);
        expect(controller.hasServerWork, isFalse);
        expect(find.textContaining('모두 끝났어요'), findsOneWidget);
      }
      final before = reads;
      await t.pump(const Duration(seconds: 4));
      await t.pumpAndSettle();
      expect(reads > before, kind == 'uncertain' || kind == 'active');
      expect(
        api.calls,
        hasLength(1),
        reason: 'Observation must not replay an ambiguous POST',
      );
      await t.tap(find.byKey(const Key('bulk-host-close')));
      await t.pumpAndSettle();
      expect(t.takeException(), isNull);
    });
  }
}

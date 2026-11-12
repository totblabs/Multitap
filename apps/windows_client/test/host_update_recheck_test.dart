// A Host update is checked again at the moment of the request (the state may
// have changed while a batch was queued or a confirmation was open), and its
// guidance names only what is known.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/bulk_host_updates.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_update_diagnostics_dialog.dart';
import 'package:multitap_client/ui/host_update_dialog.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

/// An update that can be installed now, with someone connected. [current]:
/// the Host already runs the latest version, so nothing is offered.
Map<String, Object?> updateView({bool current = false}) => {
  'protocol': 'host-update-v1',
  'connected': true,
  'supported': true,
  'version': current ? '1.0.12' : '1.0.0',
  'latestVersion': '1.0.12',
  'newer': !current,
  'available': !current,
  'sessionActive': true,
};

DesktopMachine fixtureMachine({MachineKind kind = MachineKind.vm}) =>
    DesktopMachine(
      id: 'fixture',
      name: 'Fixture',
      description: '',
      kind: kind,
      status: MachineStatus.ready,
      specification: 'Windows',
      accent: Colors.amber,
      powerState: 'running',
      powerStale: false,
      hostOnline: true,
      hostUpdate: HostUpdateState.fromJson(updateView()),
    );

final release = ProductRelease(
  product: 'host',
  version: '1.0.12',
  filename: 'Multitap-Host-1.0.12-windows-x64.exe',
  size: 1,
  sha256: 'a' * 64,
  url: Uri.parse('https://fixture.invalid/unused.exe'),
  changelog: 'fixture',
);

DesktopMachine assigned(String id, {bool current = false}) => DesktopMachine(
  id: id,
  name: id,
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.amber,
  hostUpdate: HostUpdateState.fromJson(updateView(current: current)),
);

class RecordingApi implements HostUpdateBrokerApi {
  final calls = <({String id, bool disconnect})>[];
  Future<HostUpdateState> Function()? respond;
  @override
  Future<HostUpdateState> requestHostUpdate(
    String token,
    String desktopId,
    String version, {
    bool disconnectSessions = false,
  }) async {
    calls.add((id: desktopId, disconnect: disconnectSessions));
    return respond == null
        ? const HostUpdateState(
            present: true,
            operation: HostUpdateOperation(
              status: 'accepted',
              targetVersion: '1.0.12',
            ),
          )
        : await respond!();
  }
}

BulkHostUpdateController controller(
  RecordingApi api,
  List<DesktopMachine> machines,
) => BulkHostUpdateController(
  api: api,
  token: 'fixture-only',
  release: release,
  assignedMachines: machines,
  minimumRequestInterval: Duration.zero,
);

void main() {
  test('an update view is offered and an absent clipboard policy allows nothing', () {
    expect(HostUpdateState.fromJson(updateView()).offered, isTrue);
    expect(HostUpdateState.fromJson(updateView(current: true)).offered, isFalse);
    expect(ClipboardPolicy.fromJson(null).any, isFalse);
  });
  test('fixed refusal names no obsolete bootstrap version', () {
    expect(
      const BrokerException('host_update_unsupported').message,
      isNot(contains('0.2.59')),
    );
  });
  test(
    'unknown missing Host does not diagnose quarantine or restore files',
    () {
      final missing = HostMissingAfterUpdate.fromJson({
        'result': 'unknown',
        'targetVersion': '1.0.12',
      })!;
      expect(missing.steps, isNot(contains('보호 기록')));
      expect(missing.steps, isNot(contains('복원')));
      expect(missing.steps, isNot(contains('다시 설치')));
    },
  );
  test('a Host that stops being offered while queued is rechecked before POST', () async {
    final api = RecordingApi(), gate = Completer<HostUpdateState>();
    api.respond = () => gate.future;
    final c = controller(api, [assigned('first'), assigned('second')]);
    addTearDown(c.dispose);
    c.selectAll(true);
    final pending = c.submitSelected();
    await Future<void>.delayed(Duration.zero);
    expect(api.calls.length, 1);
    c.synchronize([assigned('first'), assigned('second', current: true)]);
    gate.complete(
      const HostUpdateState(
        present: true,
        operation: HostUpdateOperation(
          status: 'accepted',
          targetVersion: '1.0.12',
        ),
      ),
    );
    await pending;
    expect(api.calls.map((e) => e.id), ['first']);
    expect(api.calls.single.disconnect, isTrue);
  });
  for (final kind in MachineKind.values) {
    testWidgets('diagnostic guidance names the actual device ${kind.name}', (
      tester,
    ) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: HostUpdateDiagnosticsDialog(
            machine: fixtureMachine(kind: kind),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(
        find.textContaining(
          kind == MachineKind.pc ? '로컬 PC의 Host 설정' : '데스크톱을 열고 Host 설정',
        ),
        findsOneWidget,
      );
      expect(find.textContaining('보호 기록'), findsNothing);
      await tester.pumpWidget(const SizedBox());
    });
  }
  testWidgets(
    'an update no longer offered after the confirmation opened sends no request',
    (tester) async {
      tester.view.physicalSize = const Size(1440, 900);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final appearance = AppAppearance();
      addTearDown(appearance.dispose);
      final machines = ValueNotifier<List<DesktopMachine>>([
        assigned('changing'),
      ]);
      addTearDown(machines.dispose);
      var requests = 0;
      await tester.pumpWidget(
        AppearanceScope(
          controller: appearance,
          child: MaterialApp(
            theme: buildAppTheme(),
            home: ValueListenableBuilder(
              valueListenable: machines,
              builder: (context, list, _) => WorkspaceShell(
                onSignOut: () {},
                onRefresh: () {},
                displayName: 'fixture',
                serverOrigin: 'https://fixture.invalid',
                machines: list,
                checkClientUpdate: () async => null,
                onHostUpdate:
                    (id, version, {required disconnectSessions}) async {
                      requests++;
                    },
                loadHostRelease: (_) async => release,
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('desktop-more-changing')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('host-update-changing')));
      await tester.pumpAndSettle();
      expect(find.byType(HostUpdateConfirmDialog), findsOneWidget);
      machines.value = [assigned('changing', current: true)];
      await tester.pump();
      await tester.tap(find.byKey(const Key('host-update-now')));
      await tester.pumpAndSettle();
      expect(requests, 0);
      expect(find.byType(HostUpdateConfirmDialog), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
    },
  );
}

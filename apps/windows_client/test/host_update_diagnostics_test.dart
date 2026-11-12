import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/host_update.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';
import 'package:multitap_client/ui/desktop_tile.dart';
import 'package:multitap_client/ui/host_update_diagnostics_dialog.dart';

const machine = DesktopMachine(
  id: 'pve-247',
  name: 'WorkPC',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: true,
  interactiveSession: true,
  virtualDisplayReady: true,
  powerState: 'running',
  powerStale: false,
  hostUpdate: HostUpdateState(
    present: true,
    connected: true,
    supported: true,
    version: '0.2.93',
    latestVersion: '0.2.94',
    newer: true,
    available: true,
    operation: HostUpdateOperation(
      status: 'failed',
      targetVersion: '0.2.94',
      code: 'install_failed',
    ),
  ),
);

void main() {
  for (final brightness in Brightness.values) {
    for (final scale in [1.0, 2.0]) {
      testWidgets(
        'failed Host update has a usable diagnostics popup ${brightness.name}/$scale',
        (tester) async {
          tester.view.devicePixelRatio = 1;
          tester.view.physicalSize = const Size(960, 640);
          addTearDown(tester.view.resetDevicePixelRatio);
          addTearDown(tester.view.resetPhysicalSize);
          var details = 0;
          await tester.pumpWidget(
            MaterialApp(
              theme: buildAppTheme(brightness: brightness),
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(context)
                    .copyWith(textScaler: TextScaler.linear(scale)),
                child: child!,
              ),
              home: Scaffold(
                body: Center(
                  child: SizedBox(
                    width: 400,
                    child: DesktopStatusArea(
                      machine: machine,
                      readiness: DesktopReadiness.of(
                        machine,
                      ),
                      mode: null,
                      progress: null,
                      preview: false,
                      onDetails: () => details++,
                    ),
                  ),
                ),
              ),
            ),
          );
          final action = find.byKey(
            const Key('host-update-diagnostics-pve-247'),
          );
          expect(action.hitTestable(), findsOneWidget);
          await tester.tap(action);
          await tester.pumpAndSettle();
          expect(find.byType(HostUpdateDiagnosticsDialog), findsOneWidget);
          expect(find.text('install_failed'), findsOneWidget);
          expect(find.text('0.2.93'), findsOneWidget);
          expect(find.text('0.2.94'), findsOneWidget);
          expect(find.textContaining('자세한 원인을 알 수 없어요'), findsOneWidget);
          expect(
            find
                .byKey(const Key('close-host-update-diagnostics'))
                .hitTestable(),
            findsOneWidget,
          );
          expect(
            details,
            0,
            reason: 'Opening update diagnostics never connects or invokes VM readiness actions.',
          );
          expect(tester.takeException(), isNull);
          await tester.tap(
            find.byKey(const Key('close-host-update-diagnostics')),
          );
          await tester.pumpAndSettle();
          expect(find.byType(HostUpdateDiagnosticsDialog), findsNothing);
          expect(action.hitTestable(), findsOneWidget);
        },
      );
    }
  }
}

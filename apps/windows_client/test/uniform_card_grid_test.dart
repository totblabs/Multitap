import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/virtual_display.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

void main() {
  testWidgets(
    'assigned cards keep equal size across power and failure states, rows and text scales',
    (tester) async {
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      tester.view.devicePixelRatio = 1;
      final machines = [
        for (var i = 0; i < 4; i++)
          DesktopMachine(
            id: 'vm-$i',
            name: i == 3 ? '이름이 긴 Windows 데스크톱' : 'VM $i',
            description: '',
            specification: 'Windows 11',
            kind: MachineKind.vm,
            status: i == 0
                ? MachineStatus.ready
                : i == 1
                ? MachineStatus.poweredOff
                : MachineStatus.awaitingHost,
            accent: Colors.blue,
            powerState: i == 1 ? 'stopped' : 'running',
            powerStale: i == 3,
            resources: VmResources(
              vcpus: 4,
              memoryTotalBytes: 8589934592,
              cpuUsagePercent: i == 1 ? null : 3.4,
              memoryUsedBytes: i == 1 ? null : 9 * 1024 * 1024 * 1024,
              stale: i == 3,
            ),
            hostOnline: i == 0 || i == 2,
            virtualMonitorControl: i == 0,
            display: VirtualDisplayState(
              supported: i == 0,
              activeCount: i == 0 ? 2 : 0,
              stale: false,
            ),
          ),
      ];
      var opens = 0;
      for (final width in [1440.0, 800.0, 400.0]) {
        tester.view.physicalSize = Size(width, 1000);
        for (final scale in [1.0, 1.75, 2.0]) {
          await tester.pumpWidget(
            MaterialApp(
              theme: buildAppTheme(brightness: Brightness.light),
              home: MediaQuery(
                data: MediaQueryData(
                  size: Size(width, 1000),
                  textScaler: TextScaler.linear(scale),
                ),
                child: WorkspaceShell(
                  onSignOut: () {},
                  onRefresh: () {},
                  machines: machines,
                  displayName: 'tester',
                  serverOrigin: 'https://cloud.example.com',
                  onConnect: (_, {options = const StreamOptions()}) async {
                    opens++;
                    throw StateError('fixture only');
                  },
                  onPower: (_, _) async {},
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          tester.widget<CustomScrollView>(
            find.byKey(const Key('workspace-scroll')),
          ).controller!.jumpTo(0);
          await tester.pump(const Duration(milliseconds: 250));
          final sizes = <Size>[];
          for (final vm in machines) {
            final card = find.byKey(Key('card-${vm.id}'));
            await tester.scrollUntilVisible(card, 400,
              scrollable: find.descendant(
                of: find.byKey(const Key('workspace-scroll')),
                matching: find.byType(Scrollable),
              ).first);
            sizes.add(tester.getSize(card));
          }
          for (final size in sizes) {
            expect(size, sizes.first, reason: 'width=$width scale=$scale');
          }
          if (scale == 1) {
            // The body holds the name row, the CPU and memory strip
            // (2026-10-26, about 30 px), the status card and the buttons.
            expect(
              sizes.first.height,
              lessThanOrEqualTo(sizes.first.width * 3 / 5 + 230),
              reason: 'No large reserved gap beneath the connection actions.',
            );
          }
          expect(
            tester.takeException(),
            isNull,
            reason: 'width=$width scale=$scale',
          );
        }
      }
      expect(opens, 0);
    },
  );
}

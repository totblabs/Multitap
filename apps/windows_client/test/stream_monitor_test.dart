import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';

void main() {
  testWidgets(
    'A single active connector 1 remains monitor 1 in settings and connection options',
    (tester) async {
      const monitors = [RemoteMonitor(id: '1', width: 1584, height: 884)];
      const initial = StreamOptions(fps: 30, bitrate: 8000000, audio: false);
      final actual = initial.forMonitors(monitors);
      expect(actual.monitor, '1');
      expect(actual.fps, 30);
      expect(actual.bitrate, 8000000);
      expect(actual.audio, false);
      expect(
        const StreamOptions(monitor: 'all').forMonitors(monitors).monitor,
        '1',
      );
      StreamOptions? result;
      await tester.pumpWidget(
        MaterialApp(
          home: Builder(
            builder: (context) => TextButton(
              onPressed: () async => result = await showDialog<StreamOptions>(
                context: context,
                builder: (_) => const StreamSettingsDialog(
                  machine: DesktopMachine(
                    id: 'fixture',
                    name: 'Fixture',
                    description: '',
                    kind: MachineKind.vm,
                    status: MachineStatus.ready,
                    specification: '',
                    accent: Colors.blue,
                    monitors: monitors,
                  ),
                  initialOptions: initial,
                ),
              ),
              child: const Text('open'),
            ),
          ),
        ),
      );
      await tester.tap(find.text('open'));
      await tester.pumpAndSettle();
      // No virtual-monitor choice (2026-10-26): the request keeps the
      // VM's one active output.
      expect(find.textContaining('가상 화면'), findsNothing);
      await tester.tap(find.byKey(const Key('apply-stream-settings')));
      await tester.pumpAndSettle();
      expect(result!.monitor, '1');
      expect(result!.fps, 30);
      // The settings always ask for sound (2026-11-11).
      expect(result!.audio, true);
      expect(tester.takeException(), isNull);
    },
  );
}

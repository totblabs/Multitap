// Actual popup routes with memory-only Drive/USB dependencies.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/drive_panel.dart';
import 'package:multitap_client/ui/session_menu.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';
import 'package:multitap_client/ui/usb_panel.dart';

import 'drive_session_test.dart' show FakeFiles, driveUrl, sessionId;
import 'usb_redirection_test.dart' show FakeBackend;
import 'helpers/pump_work.dart';

const _machine = DesktopMachine(
  id: 'pve-149',
  name: 'SYNTHETIC VM',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: '',
  accent: Colors.amber,
);

void main() {
  for (final feature in ['drive', 'usb', 'settings']) {
    for (final beforeBuild in [false, true]) {
      testWidgets(
        'compact $feature fallback retires with popup scope beforeBuild=$beforeBuild',
        (tester) async {
          tester.view.physicalSize = const Size(320, 640);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          final popups = SessionPopups();
          var dials = 0;
          final drive = DriveSession(
            url: driveUrl,
            token: 'synthetic-token',
            sessionId: sessionId,
            files: FakeFiles(),
            dialer: (_, _) async {
              dials++;
              throw StateError('No network in fixture');
            },
          );
          final usb = UsbSession(
            url: Uri.parse('wss://fixture.invalid/usb'),
            token: 'synthetic-token',
            sessionId: sessionId,
            backend: FakeBackend(),
            dialer: (_, _) async {
              dials++;
              throw StateError('No network in fixture');
            },
          );
          late BuildContext context;
          await tester.pumpWidget(
            MaterialApp(
              theme: buildAppTheme(brightness: Brightness.dark),
              home: Scaffold(
                body: Builder(
                  builder: (value) {
                    context = value;
                    return const SizedBox.expand();
                  },
                ),
              ),
            ),
          );
          final Future<Object?> result;
          switch (feature) {
            case 'drive':
              result = showDrivePanel(
                context,
                drive: drive,
                anchor: const Rect.fromLTWH(140, 8, 40, 40),
                popups: popups,
              ).then<Object?>((_) => null);
            case 'usb':
              result = showUsbPanel(
                context,
                usb: usb,
                anchor: const Rect.fromLTWH(140, 8, 40, 40),
                popups: popups,
              ).then<Object?>((_) => null);
            default:
              result = showStreamSettingsPanel(
                context,
                anchor: const Rect.fromLTWH(140, 8, 40, 40),
                machine: _machine,
                initialOptions: const StreamOptions(),
                popups: popups,
              );
          }
          if (beforeBuild) {
            popups.closeAll();
          } else {
            await pumpWork(tester);
            expect(popups.isEmpty, isFalse);
            expect(find.byType(Dialog), findsOneWidget);
            popups.closeAll();
          }
          await tester.pumpAndSettle();
          expect(await result, isNull);
          expect(popups.isEmpty, isTrue);
          expect(find.byType(Dialog), findsNothing);
          expect(dials, 0);
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.runAsync(() async {
            await drive.close();
            await usb.close();
          });
        },
      );
    }
  }
}

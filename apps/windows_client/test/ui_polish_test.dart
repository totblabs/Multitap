// Widget checks for the light wordmark, page title fit and stream-settings
// labels. Render-level only; not a native-window or real-session check.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/components.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

double _contrast(Color a, Color b) {
  final l = [a.computeLuminance(), b.computeLuminance()]..sort();
  return (l.last + .05) / (l.first + .05);
}

const _machine = DesktopMachine(
  id: 'pve-149',
  name: 'TESTER',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows 11',
  accent: Colors.blue,
  powerState: 'running',
  powerStale: false,
  hostOnline: true,
);

void main() {
  testWidgets('light wordmark stays readable over the daylight sky', (t) async {
    // Sampled behind the sign-in wordmark in the daylight scene.
    const skies = [Color(0xFF6DB7F9), Color(0xFF8DC7FC)];
    for (final dark in [false, true]) {
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(
            brightness: dark ? Brightness.dark : Brightness.light,
          ),
          home: Brand(onDark: dark),
        ),
      );
      final vdi = t.widget<Text>(find.byKey(const Key('brand-vdi'))).style!;
      final line = t.widget<Text>(find.byKey(const Key('brand-for')));
      final proxmox = (line.textSpan! as TextSpan).children!.single;
      expect(line.textSpan!.toPlainText(), 'for Proxmox VE');
      // The product's own amber, not Proxmox's orange (#E57000).
      final name = (proxmox as TextSpan).style!.color!;
      expect(name, isNot(const Color(0xFFE57000)));
      if (dark) {
        expect(name, const Color(0xFFF5A623));
        expect(vdi.shadows, isNull);
        continue;
      }
      for (final sky in skies) {
        for (final color in [vdi.color!, line.style!.color!, name]) {
          expect(_contrast(color, sky), greaterThanOrEqualTo(4.5));
        }
      }
      expect(vdi.shadows, isNotEmpty);
      expect(line.style!.shadows, isNotEmpty);
    }
  });

  testWidgets('page title scales down instead of truncating', (t) async {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = const Size(320, 700);
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: MediaQuery(
          data: const MediaQueryData(
            size: Size(320, 700),
            textScaler: TextScaler.linear(2),
          ),
          child: WorkspaceShell(
            onSignOut: () {},
            onRefresh: () {},
            machines: const [_machine],
            displayName: 'tester',
            serverOrigin: 'https://cloud.example.com',
          ),
        ),
      ),
    );
    await t.pumpAndSettle();
    final title = find.text('내 데스크톱');
    expect(t.widget<Text>(title).overflow, isNot(TextOverflow.ellipsis));
    final box = t.getRect(find.byKey(const Key('workspace-title')));
    final text = t.getRect(title);
    expect(text.width, lessThanOrEqualTo(box.width + .5));
    expect(text.left, greaterThanOrEqualTo(box.left - .5));
    expect(t.takeException(), isNull);
  });

  testWidgets('stream settings labels share one style', (t) async {
    for (final brightness in Brightness.values) {
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: brightness),
          home: const StreamSettingsDialog(
            machine: _machine,
            initialOptions: StreamOptions(),
          ),
        ),
      );
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('stream-advanced')));
      await t.pumpAndSettle();
      final styles = [
        t.widget<Text>(find.byKey(const Key('stream-fps-label'))).style!,
        t.widget<Text>(find.byKey(const Key('stream-bitrate-label'))).style!,
        t.widget<Text>(find.byKey(const Key('stream-hardware-label'))).style!,
      ];
      for (final style in styles) {
        expect(style, styles.first);
      }
      expect(styles.first.color, AppPalette(brightness == Brightness.dark).ink);
      expect(find.text('영상 전송률  20 Mbps'), findsOneWidget);
      // Each appearance starts from a fresh collapsed dialog state.
      await t.pumpWidget(const SizedBox.shrink());
      await t.pump();
    }
  });
}

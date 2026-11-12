import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/themed_app.dart';
import 'package:multitap_client/ui/vm_resource_bars.dart';

Map<String, Object?> _json({String? measuredBy}) => {
  'source': 'proxmox',
  'checkedAt': DateTime.now().toUtc().toIso8601String(),
  'cpuUsagePercent': 12.5,
  'vcpus': 4,
  'memoryUsedBytes': 3 * 1024 * 1024 * 1024,
  'memoryTotalBytes': 8 * 1024 * 1024 * 1024,
  'stale': false,
  'measuredBy': ?measuredBy,
};

Future<String> _tooltip(WidgetTester t, VmResources sample) async {
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: Scaffold(
        body: Center(
          child: SizedBox(
            width: 300,
            child: VmResourceStrip(sample: sample, running: true),
          ),
        ),
      ),
    ),
  );
  return t.widget<Tooltip>(find.byType(Tooltip)).message!;
}

void main() {
  test('measuredBy host is read; a released broker (no field) means Proxmox', () {
    expect(VmResources.fromJson(_json(measuredBy: 'host')).measuredByHost, isTrue);
    expect(VmResources.fromJson(_json()).measuredByHost, isFalse);
    expect(VmResources.fromJson(_json(measuredBy: 'other')).measuredByHost, isFalse);
    expect(VmResources.fromJson(_json(measuredBy: 'host')).asStale().measuredByHost, isTrue);
  });

  testWidgets('a new sample is one frame: nothing keeps the window redrawing', (t) async {
    final theme = buildAppTheme(brightness: Brightness.dark);
    Future<void> show(double percent) => t.pumpWidget(
      MaterialApp(
        theme: theme,
        home: Scaffold(
          body: Center(
            child: SizedBox(
              width: 300,
              child: VmResourceStrip(
                sample: VmResources.fromJson({
                  ..._json(),
                  'cpuUsagePercent': percent,
                }),
                running: true,
              ),
            ),
          ),
        ),
      ),
    );
    await show(12);
    await t.pumpAndSettle();
    await show(80);
    expect(t.binding.hasScheduledFrame, isFalse);
    expect(find.text('80%'), findsOneWidget);
  });

  testWidgets('rebuilding the app is no theme change: nothing keeps drawing', (t) async {
    final appearance = AppAppearance();
    addTearDown(appearance.dispose);
    Future<void> show(String text) => t.pumpWidget(
      ThemedApp(appearance: appearance, home: Scaffold(body: Text(text))),
    );
    await show('one');
    await t.pumpAndSettle();
    await show('two');
    expect(find.text('two'), findsOneWidget);
    expect(t.binding.hasScheduledFrame, isFalse);
  });

  testWidgets('the CPU/memory tooltip names where the numbers were measured', (t) async {
    final host = await _tooltip(t, VmResources.fromJson(_json(measuredBy: 'host')));
    expect(host, startsWith('데스크톱 안에서 측정'));
    expect(host, isNot(contains('Windows 작업 관리자')));
    final proxmox = await _tooltip(t, VmResources.fromJson(_json()));
    expect(proxmox, startsWith('Proxmox 기준'));
    expect(proxmox, contains('Windows 작업 관리자'));
  });
}
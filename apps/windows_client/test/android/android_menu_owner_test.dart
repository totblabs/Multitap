// The tile menus on Android are the Windows tile's (DesktopMenu, by finger,
// 2026-11-03): an open menu belongs to its tile. It goes with the tile, a
// row whose permission is gone is greyed in place and does nothing, and a
// dialog opened meanwhile stays.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_menu.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';
import 'package:multitap_client/ui/desktop_tile.dart';

import 'android_launcher_test.dart' show vm;

typedef Entry = ({
  DesktopMachine machine,
  DesktopReadiness readiness,
  DesktopActions actions,
});

Entry entry(
  String id,
  List<String> calls, {
  bool shutdown = true,
  bool quality = true,
}) {
  final machine = machineFromBroker(vm(id: id, name: id));
  return (
    machine: machine,
    readiness: DesktopReadiness.of(machine),
    actions: DesktopActions(
      primary: () => calls.add('connect:$id'),
      showPower: true,
      shutdown: shutdown ? () => calls.add('shutdown:$id') : null,
      reboot: () => calls.add('reboot:$id'),
      quality: quality ? () => calls.add('quality:$id') : null,
      details: () => calls.add('details:$id'),
    ),
  );
}

Future<void> mount(
  WidgetTester tester,
  ValueNotifier<List<Entry>> entries,
) async {
  tester.view.physicalSize = const Size(500, 1000);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: Scaffold(
        body: SingleChildScrollView(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: ValueListenableBuilder<List<Entry>>(
              valueListenable: entries,
              builder: (_, value, _) => Column(
                children: [
                  for (final e in value)
                    Padding(
                      key: ValueKey(e.machine.id),
                      padding: const EdgeInsets.only(bottom: 24),
                      child: DesktopTile(
                        machine: e.machine,
                        readiness: e.readiness,
                        actions: e.actions,
                        mode: null,
                        touch: true,
                      ),
                    ),
                ],
              ),
            ),
          ),
        ),
      ),
    ),
  );
  await tester.pumpAndSettle();
}

Future<void> tap(WidgetTester tester, String key) async {
  final finder = find.byKey(Key(key));
  await tester.ensureVisible(finder);
  await tester.pumpAndSettle();
  await tester.tap(finder);
  await tester.pumpAndSettle();
}

Future<void> finish(
  WidgetTester tester,
  ValueNotifier<List<Entry>> entries,
) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pumpAndSettle();
  entries.dispose();
}

bool enabled(WidgetTester tester, String key) =>
    tester.widget<MenuItemButton>(find.byKey(Key(key))).onPressed != null;

void main() {
  testWidgets('reordering keeps every tile and its main button, and connects '
      'nothing', (tester) async {
    final calls = <String>[];
    final a = entry('pve-101', calls), b = entry('pve-102', calls);
    final entries = ValueNotifier([a, b]);
    await mount(tester, entries);
    expect(find.byKey(const Key('open-pve-101')), findsOneWidget);
    entries.value = [b, a];
    await tester.pumpAndSettle();
    final first = tester.getTopLeft(find.byKey(const Key('card-pve-102'))).dy;
    final second = tester.getTopLeft(find.byKey(const Key('card-pve-101'))).dy;
    final stayed = find.byKey(const Key('open-pve-101')).evaluate().length;
    await finish(tester, entries);
    expect(first, lessThan(second));
    expect(stayed, 1);
    expect(calls, isEmpty);
  });

  testWidgets('an open power menu goes with its desktop', (tester) async {
    final calls = <String>[];
    final entries = ValueNotifier([
      entry('pve-101', calls),
      entry('pve-102', calls),
    ]);
    await mount(tester, entries);
    await tap(tester, 'desktop-power-pve-101');
    expect(find.byKey(const Key('power-shutdown-pve-101')), findsOneWidget);
    entries.value = [entry('pve-102', calls)];
    await tester.pumpAndSettle();
    final stale = find
        .byKey(const Key('power-shutdown-pve-101'))
        .evaluate()
        .isNotEmpty;
    await finish(tester, entries);
    expect([stale, calls], [false, <String>[]]);
  });

  testWidgets('a lost permission greys its open row in place; a tap sends '
      'nothing', (tester) async {
    final calls = <String>[];
    final entries = ValueNotifier([entry('pve-101', calls)]);
    await mount(tester, entries);
    await tap(tester, 'desktop-power-pve-101');
    expect(enabled(tester, 'power-shutdown-pve-101'), isTrue);
    entries.value = [entry('pve-101', calls, shutdown: false)];
    await tester.pumpAndSettle();
    expect(enabled(tester, 'power-shutdown-pve-101'), isFalse);
    await tester.tap(find.byKey(const Key('power-shutdown-pve-101')));
    await tester.pumpAndSettle();
    await finish(tester, entries);
    expect(calls, isEmpty);
  });

  testWidgets('an ordinary refresh does not dismiss the same VM menu', (
    tester,
  ) async {
    final calls = <String>[];
    final entries = ValueNotifier([entry('pve-101', calls)]);
    await mount(tester, entries);
    await tap(tester, 'desktop-power-pve-101');
    entries.value = [entry('pve-101', calls)];
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('power-shutdown-pve-101')), findsOneWidget);
    await tap(tester, 'power-shutdown-pve-101');
    expect(calls, ['shutdown:pve-101']);
    await finish(tester, entries);
  });

  testWidgets('▾ is named for what it opens; losing the quality permission '
      'greys 화질 옵션 without an action', (tester) async {
    final calls = <String>[];
    final entries = ValueNotifier([entry('pve-101', calls)]);
    await mount(tester, entries);
    expect(
      find.descendant(
        of: find.byKey(const Key('stream-modes-pve-101')),
        matching: find.byTooltip('연결 옵션'),
      ),
      findsOneWidget,
    );
    await tap(tester, 'stream-modes-pve-101');
    expect(enabled(tester, 'stream-settings-pve-101'), isTrue);
    entries.value = [entry('pve-101', calls, quality: false)];
    await tester.pumpAndSettle();
    expect(enabled(tester, 'stream-settings-pve-101'), isFalse);
    await tester.tap(find.byKey(const Key('stream-settings-pve-101')));
    await tester.pumpAndSettle();
    expect(calls, isEmpty);
    await finish(tester, entries);
  });

  testWidgets('removal before the first menu frame cannot leave an orphan', (
    tester,
  ) async {
    final calls = <String>[];
    final entries = ValueNotifier([
      entry('pve-101', calls),
      entry('pve-102', calls),
    ]);
    await mount(tester, entries);
    await tester.tap(find.byKey(const Key('desktop-power-pve-101')));
    entries.value = [entry('pve-102', calls)];
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('power-shutdown-pve-101')), findsNothing);
    expect(calls, isEmpty);
    await finish(tester, entries);
  });

  testWidgets('a VM menu going away leaves a newer dialog open', (
    tester,
  ) async {
    final calls = <String>[];
    final entries = ValueNotifier([
      entry('pve-101', calls),
      entry('pve-102', calls),
    ]);
    await mount(tester, entries);
    await tap(tester, 'desktop-power-pve-101');
    unawaited(
      showDialog<void>(
        context: tester.element(find.byType(Scaffold)),
        builder: (_) => const AlertDialog(title: Text('new dialog')),
      ),
    );
    await tester.pumpAndSettle();
    entries.value = [entry('pve-102', calls)];
    await tester.pumpAndSettle();
    final stale = find
        .byKey(const Key('power-shutdown-pve-101'))
        .evaluate()
        .isNotEmpty;
    final keptDialog = find.text('new dialog').evaluate().length == 1;
    await finish(tester, entries);
    expect(stale, isFalse);
    expect(keptDialog, isTrue);
    expect(calls, isEmpty);
  });
}

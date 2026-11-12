import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/session_activity.dart';
import 'package:multitap_client/models/desktop_organize.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_organizer.dart';
import 'package:multitap_client/ui/session_access_controls.dart';

import 'android_launcher_test.dart' show vm;

class _Activity extends SessionActivity {
  Duration left = const Duration(minutes: 20);
  @override bool get idleSupported => true;
  @override bool get idleEnabled => true;
  @override Duration get remaining => left;
  void tick(Duration value) { left = value; notifyListeners(); }
}

void main() {
  for (final dark in [false, true]) {
    testWidgets('desktop default chrome retains original shapes and geometry $dark', (t) async {
      final controller = TextEditingController();
      final focus = FocusNode();
      addTearDown(controller.dispose);
      addTearDown(focus.dispose);
      final activity = _Activity();
      addTearDown(activity.dispose);
      await t.pumpWidget(MaterialApp(
        theme: buildAppTheme(brightness: dark ? Brightness.dark : Brightness.light),
        home: Scaffold(body: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          DesktopSearchField(controller: controller, focusNode: focus,
            onSubmitted: (_) {}, onLeave: () {}, onOpen: () {}, open: false, width: 200),
          DesktopStatusFilter(selected: DesktopStatus.all,
            counts: const DesktopStatusCounts(4, 3, 1), onSelected: (_) {}),
          DesktopKindFilter(selected: DesktopKind.all,
            machines: [machineFromBroker(vm())], onSelected: (_) {}),
          DesktopFilterReset(onPressed: () {}),
          LauncherHeaderButton(key: const Key('default-header'),
            icon: Icons.refresh, label: 'Refresh', tooltip: 'Refresh', onPressed: () {}),
          SessionIdleBadge(activity: activity),
        ])),
      ));
      await t.pumpAndSettle();
      final p = AppPalette(dark);
      for (final key in ['status-all', 'kind-all']) {
        final material = t.widget<Material>(find.byKey(Key(key)));
        expect(material.shape, isA<StadiumBorder>());
        expect(material.color, p.amber);
      }
      final search = t.widget<IconButton>(find.byKey(const Key('desktop-search-open')));
      expect(search.style!.shape!.resolve({}), isA<CircleBorder>());
      expect(t.getSize(find.byKey(const Key('desktop-search-open'))), const Size(34, 34));
      final reset = t.widget<TextButton>(find.byKey(const Key('desktop-filter-reset')));
      expect(reset.style!.shape!.resolve({}), isA<StadiumBorder>());
      final header = t.widget<Material>(find.descendant(
        of: find.byKey(const Key('default-header')), matching: find.byType(Material)).first);
      expect(header.shape, isA<StadiumBorder>());
      expect(header.color, p.pillGlass);
      final badge = t.widget<Container>(find.byKey(const Key('session-idle-badge')));
      final decoration = badge.decoration! as BoxDecoration;
      expect(decoration.color, p.quiet);
      expect(decoration.border, Border.all(color: p.tileLine));
      expect(decoration.borderRadius, BorderRadius.circular(9));
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
    });
  }
  testWidgets('flat real countdown retains warning emphasis and passive behavior', (t) async {
    final activity = _Activity();
    addTearDown(activity.dispose);
    await t.pumpWidget(MaterialApp(theme: buildAppTheme(brightness: Brightness.dark),
      home: Scaffold(body: SessionIdleBadge(activity: activity, compact: true, flat: true))));
    final p = AppPalette(true);
    BoxDecoration decoration() => t.widget<Container>(find.byKey(
      const Key('session-idle-badge'))).decoration! as BoxDecoration;
    expect(decoration().color, Colors.transparent);
    expect(decoration().border, isNull);
    expect(find.text('20:00'), findsOneWidget);
    activity.tick(const Duration(seconds: 59));
    await t.pump();
    expect(find.text('0:59'), findsOneWidget);
    expect(decoration().color, p.amberSoft);
    expect(decoration().border, Border.all(color: p.amberLine));
    expect(t.takeException(), isNull);
    await t.pumpWidget(const SizedBox.shrink());
  });
}

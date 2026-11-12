// Previous scene names remain readable preferences, never product layouts.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/ui/desktop_tile.dart';

import 'launcher_view_test.dart' show showLauncherView, viewMachine, viewOrigin;

void main() {
  for (final legacy in [
    LauncherView.explorer,
    LauncherView.monitorWall,
    LauncherView.dock,
  ]) {
    testWidgets('$legacy is restored as cards with no scene selector', (
      t,
    ) async {
      final fixture = await showLauncherView(
        t,
        view: legacy,
        machines: [viewMachine(101), viewMachine(102)],
      );
      expect(find.byType(DesktopTile), findsNWidgets(2));
      expect(find.byType(DesktopRow), findsNothing);
      expect(find.byKey(const Key('desktop-view')), findsNothing);
      expect(find.byKey(const Key('scene-navigation')), findsNothing);
      expect(find.byKey(const Key('scene-preview-panel')), findsNothing);
      expect(find.byKey(const Key('scene-desktop-dock')), findsNothing);
      for (final label in ['리스트형', '미니', '탐색기형', '모니터월', '도킹형']) {
        expect(find.text(label), findsNothing);
      }
      expect((await fixture.memory.load(viewOrigin)).view, LauncherView.cards);
      expect(fixture.opens, isEmpty);
      expect(t.takeException(), isNull);
    });
    testWidgets(
      '$legacy preserves remembered desktop focus in the lazy card grid',
      (t) async {
        final memory = SessionLauncherMemory();
        await memory.save(
          viewOrigin,
          LauncherPreferences(view: legacy, lastDesktop: 'pve-200'),
        );
        final fixture = await showLauncherView(
          t,
          memory: memory,
          machines: [for (var i = 101; i <= 200; i++) viewMachine(i)],
        );
        expect(find.byKey(const Key('card-pve-200')), findsOneWidget);
        expect(find.byType(DesktopTile).evaluate().length, lessThan(25));
        expect(
          FocusManager.instance.primaryFocus?.debugLabel,
          'desktop-pve-200',
        );
        expect(fixture.opens, isEmpty);
        expect(t.takeException(), isNull);
      },
    );
  }
}

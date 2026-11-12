// Real shared card/caller widgets. VM power, broker, Host and native windows
// are replaced by callbacks; the tests never change a running desktop.
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_menu.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';
import 'package:multitap_client/ui/desktop_tile.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'desktop_tile_test.dart' show vm;

Future<void> _platform(TargetPlatform platform, Future<void> Function() body) async {
  final previous = debugDefaultTargetPlatformOverride;
  debugDefaultTargetPlatformOverride = platform;
  try { await body(); } finally { debugDefaultTargetPlatformOverride = previous; }
}

Future<void> _card(
  WidgetTester tester, {
  required TargetPlatform platform,
  required DesktopMachine machine,
  required DesktopActions actions,
  StartProgress? progress,
  VoidCallback? parentTap,
}) async {
  await tester.pumpWidget(MaterialApp(
    theme: buildAppTheme(),
    home: Scaffold(body: Center(child: GestureDetector(
      onTap: parentTap,
      child: SizedBox(width: 360, height: 530, child: DesktopTile(
        machine: machine,
        readiness: DesktopReadiness.of(machine),
        actions: actions,
        mode: SessionDisplayMode.windowed,
        progress: progress,
        pictureHeight: 200,
        touch: platform == TargetPlatform.android,
      )),
    ))),
  ));
  if (progress == null) {
    await tester.pumpAndSettle();
  } else {
    await tester.pump(const Duration(milliseconds: 200));
  }
}

void main() {
  for (final platform in [TargetPlatform.windows, TargetPlatform.android]) {
    testWidgets('${platform.name}: off center power icon and footer call the same primary once', (tester) async {
      await _platform(platform, () async {
        var primary = 0, menuStart = 0, parent = 0;
        final machine = vm('off-center', state: 'stopped');
        await _card(tester, platform: platform, machine: machine,
          actions: DesktopActions(primary: () => primary++, start: () => menuStart++, menu: false),
          parentTap: () => parent++);
        final picture = find.byType(DesktopThumbnailView);
        expect(picture, findsOneWidget);
        await tester.tapAt(tester.getCenter(picture));
        await tester.pump();
        expect(primary, 1, reason: 'The displayed central power control uses the footer handler.');
        expect(menuStart, 0, reason: 'Do not bypass the primary confirmation/pending policy.');
        expect(parent, 0, reason: 'One gesture must not bubble into a second card action.');
        await tester.tap(find.byKey(const Key('start-off-center')));
        await tester.pump();
        expect(primary, 2);
        expect(menuStart, 0);
        expect(parent, 0);
      });
    });

    testWidgets('${platform.name}: the off preview background shares the same wake action', (tester) async {
      await _platform(platform, () async {
        var primary = 0;
        await _card(tester, platform: platform, machine: vm('off-picture', state: 'stopped'),
          actions: DesktopActions(primary: () => primary++, menu: false));
        final rect = tester.getRect(find.byType(DesktopThumbnailView));
        await tester.tapAt(Offset(rect.left + 20, rect.center.dy));
        await tester.pump();
        expect(primary, 1, reason: 'The main preview and central symbol have one wake target.');
        await tester.tap(find.byKey(const Key('start-off-picture')));
        await tester.pump();
        expect(primary, 2);
      });
    });

    testWidgets('${platform.name}: missing primary permission and pending power never wake from preview', (tester) async {
      await _platform(platform, () async {
        var forbidden = 0;
        final machine = vm('off-disabled', state: 'stopped');
        // A menu start callback may exist for an explanation or a different
        // guarded route. It cannot substitute for a disabled main callback.
        await _card(tester, platform: platform, machine: machine,
          actions: DesktopActions(start: () => forbidden++, menu: false));
        final picture = find.byType(DesktopThumbnailView);
        await tester.tapAt(tester.getCenter(picture));
        final rect = tester.getRect(picture);
        await tester.tapAt(Offset(rect.left + 20, rect.center.dy));
        await tester.pump();
        expect(tester.widget<FilledButton>(find.byKey(const Key('start-off-disabled'))).onPressed, isNull);
        expect(forbidden, 0);
        await _card(tester, platform: platform, machine: machine,
          actions: DesktopActions(primary: () => forbidden++, start: () => forbidden++, menu: false),
          progress: const StartProgress(stage: 0, elapsed: Duration(seconds: 2)));
        await tester.tapAt(tester.getCenter(find.byType(DesktopThumbnailView)));
        await tester.pump();
        expect(find.byKey(const Key('start-off-disabled')), findsNothing);
        expect(forbidden, 0);
      });
    });

    testWidgets('${platform.name}: ready play and footer connect; ready background stays unchanged', (tester) async {
      await _platform(platform, () async {
        var connects = 0, starts = 0;
        await _card(tester, platform: platform, machine: vm('ready-card'),
          actions: DesktopActions(primary: () => connects++, start: () => starts++, menu: false));
        final picture = find.byType(DesktopThumbnailView);
        if (platform == TargetPlatform.windows) {
          final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
          await mouse.addPointer(location: Offset.zero);
          await mouse.moveTo(tester.getCenter(picture));
          await tester.pumpAndSettle();
          addTearDown(mouse.removePointer);
        }
        await tester.tap(find.byKey(const Key('play-ready-card')));
        await tester.pump();
        expect(connects, 1);
        await tester.tap(find.byKey(const Key('open-ready-card')));
        await tester.pump();
        expect(connects, 2);
        final rect = tester.getRect(picture);
        await tester.tapAt(Offset(rect.left + 20, rect.center.dy));
        await tester.pump();
        expect(connects, 2, reason: 'This fix does not make the running preview a second connect surface.');
        expect(starts, 0);
      });
    });
  }

  testWidgets('Real launcher center wake and footer use the same single confirmation and guarded pending request', (tester) async {
    await _platform(TargetPlatform.windows, () async {
      tester.view.physicalSize = const Size(1440, 1000);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final calls = <String>[];
      final opened = <String>[];
      final power = Completer<void>();
      final powerPending = ValueNotifier<bool>(false);
      addTearDown(powerPending.dispose);
      final memory = SessionLauncherMemory();
      final machine = vm('guarded-off', state: 'stopped');
      await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: ValueListenableBuilder<bool>(
        valueListenable: powerPending,
        builder: (context, pending, _) => WorkspaceShell(
        onSignOut: () {}, onRefresh: () {}, displayName: 'Fixture',
        serverOrigin: 'https://fixture.invalid', localMonitors: 1,
        machines: [machine], launcherMemory: memory, busy: pending,
        onConnect: (_, {options = const StreamOptions()}) async => throw StateError('No stream in power fixture'),
        onPower: (id, action) {
          calls.add('$id:$action');
          // The real parent publishes its pending state to this presentation
          // caller. A held callback without that input would be a different UI.
          powerPending.value = true;
          return power.future;
        },
        onOpenWindow: (desktop, mode, options) async {
          opened.add(desktop.id); return const SessionWindowOpenResult(opened: true);
        },
      ))));
      await tester.pumpAndSettle();
      final card = find.byKey(const Key('card-guarded-off'));
      final picture = find.descendant(of: card, matching: find.byType(DesktopThumbnailView));
      await tester.tapAt(tester.getCenter(picture));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('confirm-power-start-guarded-off')), findsOneWidget);
      expect(calls, isEmpty);
      expect(opened, isEmpty);
      await tester.tap(find.byKey(const Key('cancel-power-start-guarded-off')));
      await tester.pumpAndSettle();
      expect(calls, isEmpty);
      await tester.tap(find.byKey(const Key('start-guarded-off')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('confirm-power-start-guarded-off')), findsOneWidget);
      await tester.tap(find.byKey(const Key('confirm-power-start-guarded-off')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      await tester.pump();
      expect(powerPending.value, isTrue);
      expect(tester.widget<DesktopTile>(find.byType(DesktopTile)).actions.primary, isNull);
      expect(calls, ['guarded-off:start']);
      await tester.tapAt(tester.getCenter(picture));
      await tester.pump();
      expect(calls, ['guarded-off:start'], reason: 'A pending request cannot be started again from the preview.');
      expect(find.byKey(const Key('confirm-power-start-guarded-off')), findsNothing);
      expect(opened, isEmpty, reason: 'Wake never implicitly connects.');
      power.complete();
      powerPending.value = false;
      await tester.pumpAndSettle();
      expect(calls, ['guarded-off:start']);
      expect(opened, isEmpty);
    });
  });
}

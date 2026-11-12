// A remote window spanning two local monitors (모든 모니터 사용) keeps its
// session bar and every popup on the primary monitor. The
// native window and monitors are fixtures; this is layout only.
import 'dart:async';
import 'dart:ui' show DisplayFeature;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/app_menu.dart';
import 'package:multitap_client/ui/primary_region.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_status_panel.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';

import 'stream_display_entry_test.dart' show entryMachine;

/// Two local monitors in physical pixels (dpr 1): the primary first, as the
/// native layout reports it.
typedef _Layout = ({String name, Rect primary, Rect secondary});

const List<_Layout> _layouts = [
  // Primary on the left, a taller secondary on the right set higher.
  (
    name: 'primary left, taller right',
    primary: Rect.fromLTWH(0, 0, 1280, 720),
    secondary: Rect.fromLTWH(1280, -180, 1920, 1080),
  ),
  // Primary on the right, a smaller secondary on the left set lower.
  (
    name: 'primary right, smaller left',
    primary: Rect.fromLTWH(0, 0, 1920, 1080),
    secondary: Rect.fromLTWH(-1280, 200, 1280, 720),
  ),
];

Map<String, Object?> _native(_Layout layout) {
  final bounds = layout.primary.expandToInclude(layout.secondary);
  Map<String, Object?> monitor(String id, Rect rect, bool primary) => {
    'id': id,
    'x': rect.left.toInt(),
    'y': rect.top.toInt(),
    'width': rect.width.toInt(),
    'height': rect.height.toInt(),
    'primary': primary,
    'dpi': 96,
  };
  return {
    'x': bounds.left.toInt(),
    'y': bounds.top.toInt(),
    'width': bounds.width.toInt(),
    'height': bounds.height.toInt(),
    'monitors': [
      monitor('0', layout.primary, true),
      monitor('1', layout.secondary, false),
    ],
  };
}

void _inside(WidgetTester tester, Finder finder, Rect region, String what) {
  final rect = tester.getRect(finder);
  expect(
    region.inflate(.5).contains(rect.topLeft) &&
        region.inflate(.5).contains(rect.bottomRight),
    isTrue,
    reason: '$what $rect must lie inside the primary monitor $region',
  );
}

void main() {
  test('Display features leave exactly the primary part of the window', () {
    for (final layout in _layouts) {
      final bounds = layout.primary.expandToInclude(layout.secondary);
      final region = layout.primary.shift(-bounds.topLeft);
      final media = MediaQueryData(
        size: bounds.size,
        displayFeatures: <DisplayFeature>[
          ...PrimaryRegionScope.featuresFor(region, bounds.size),
        ],
      );
      final screens = DisplayFeatureSubScreen.subScreensInBounds(
        Offset.zero & bounds.size,
        DisplayFeatureSubScreen.avoidBounds(media),
      ).toList();
      expect(screens, [region], reason: layout.name);
      expect(
        AnchoredPanelRoute.areaFor(media, region.topCenter & const Size(1, 1)),
        region,
      );
    }
  });

  for (final layout in _layouts) {
    testWidgets(
      'Handle, bar, menu, dialog, panel, snack bar and status card stay on the primary monitor (${layout.name})',
      (tester) async {
        final bounds = layout.primary.expandToInclude(layout.secondary);
        final region = layout.primary.shift(-bounds.topLeft);
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize = bounds.size;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final messenger =
            TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
        const window = MethodChannel('multitap/window'),
            events = MethodChannel('multitap/window/events');
        messenger.setMockMethodCallHandler(window, (call) async {
          if (call.method == 'monitorLayout') return _native(layout);
          return true;
        });
        messenger.setMockMethodCallHandler(events, (_) async => null);
        addTearDown(() {
          messenger.setMockMethodCallHandler(window, null);
          messenger.setMockMethodCallHandler(events, null);
        });
        // The output count never arrives: the window stays connecting, with
        // the layout read and the window spread over both monitors. The
        // connecting card spins forever, so animations are waited out with
        // bounded pumps: pumpAndSettle would run on until the count times
        // out, and a failed connection closes the bar's menus.
        Future<void> settle() async {
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 400));
        }

        final pending = Completer<DesktopMachine>();
        await tester.pumpWidget(
          MaterialApp(
            builder: PrimaryRegionScope.builder,
            navigatorObservers: [MenuDismissObserver()],
            home: RemoteView(
              machine: entryMachine(1),
              prepareDisplaysOnEntry: true,
              initialDisplayMode: SessionDisplayMode.allMonitors,
              load: () async => fail('No composite stream for all monitors.'),
              onConfigureDisplays: (_) => pending.future,
              onConnectTracks: (_, _) async => fail('Not connected here.'),
            ),
          ),
        );
        for (var i = 0; i < 4; i++) {
          await tester.pump();
        }
        await tester.pump(const Duration(milliseconds: 50));
        // The resting handle and the connecting card.
        _inside(tester, find.byKey(const Key('session-tab')), region, 'Handle');
        expect(
          (tester.getCenter(find.byKey(const Key('session-tab'))).dx -
                  region.center.dx)
              .abs(),
          lessThan(1),
          reason: 'Centred on the primary monitor, not on the seam.',
        );
        _inside(
          tester,
          find.byType(SessionStatusPanel),
          region,
          'Connecting card',
        );
        // The open bar and its quality menu.
        await tester.tap(find.byKey(const Key('expand-session-toolbar')));
        await tester.pump();
        _inside(
          tester,
          find.byKey(const Key('session-command-bar')),
          region,
          'Bar',
        );
        // Dragged far to one side, the bar stays on the primary monitor.
        await tester.drag(
          find.byKey(const Key('drag-session-toolbar')),
          const Offset(-5000, 0),
        );
        await tester.pump();
        _inside(
          tester,
          find.byKey(const Key('session-command-bar')),
          region,
          'Moved bar',
        );
        await tester.drag(
          find.byKey(const Key('drag-session-toolbar')),
          const Offset(10000, 0),
        );
        await tester.pump();
        _inside(
          tester,
          find.byKey(const Key('session-command-bar')),
          region,
          'Moved bar',
        );
        // The handle under the bar stays with it.
        _inside(
          tester,
          find.byKey(const Key('session-toolbar-fold')),
          region,
          'Fold handle',
        );
        // The chip opens the 연결 panel under itself (v2; the quality menu
        // is only for a bar without a session panel).
        await tester.tap(find.byKey(const Key('session-quality')));
        await settle();
        expect(
          find.byKey(const Key('session-connection-panel')),
          findsOneWidget,
        );
        _inside(
          tester,
          find.byKey(const Key('anchored-panel')),
          region,
          'Connection panel',
        );
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await settle();
        expect(find.byKey(const Key('session-connection-panel')), findsNothing);
        final context = tester.element(find.byType(RemoteView));
        // The disconnect question (any AppDialog) is centred on the primary.
        unawaited(confirmDisconnect(context, machine: 'TESTER'));
        await settle();
        _inside(tester, find.byType(Dialog), region, 'Disconnect question');
        expect(
          (tester.getCenter(find.byType(Dialog)).dx - region.center.dx).abs(),
          lessThan(1),
        );
        await tester.tap(find.text('취소'));
        await settle();
        // The settings panel under the bar.
        final bar = tester.getRect(
          find.byKey(const Key('session-command-bar')),
        );
        unawaited(
          showStreamSettingsPanel(
            context,
            anchor: Rect.fromLTWH(bar.right - 60, bar.top, 40, bar.height),
            machine: entryMachine(2),
            initialOptions: const StreamOptions(monitor: 'all'),
          ),
        );
        await settle();
        _inside(
          tester,
          find.byKey(const Key('anchored-panel')),
          region,
          'Settings panel',
        );
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await settle();
        // A snack bar floats inside the primary monitor's part.
        ScaffoldMessenger.of(context)
            .showSnackBar(const SnackBar(content: Text('알림')));
        await settle();
        // The visible bar (its outer box spans the slot with its insets).
        _inside(
          tester,
          find
              .descendant(
                of: find.byType(SnackBar),
                matching: find.byType(Material),
              )
              .first,
          region,
          'Snack bar',
        );
        pending.complete(entryMachine(1));
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump(const Duration(seconds: 1));
      },
    );
  }

  testWidgets(
    'One monitor (or an unknown layout) leaves dialogs centred on the window',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1600, 900);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      late BuildContext context;
      await tester.pumpWidget(
        MaterialApp(
          builder: PrimaryRegionScope.builder,
          home: Builder(
            builder: (c) {
              context = c;
              return const SizedBox.expand();
            },
          ),
        ),
      );
      unawaited(confirmDisconnect(context, machine: 'TESTER'));
      await tester.pumpAndSettle();
      expect(tester.getCenter(find.byType(Dialog)).dx, closeTo(800, 1));
      // Published, the region moves it; withdrawn, it is centred again.
      PrimaryRegionScope.maybeOf(context)!.value = const Rect.fromLTWH(
        1000,
        0,
        600,
        900,
      );
      await tester.pumpAndSettle();
      expect(tester.getCenter(find.byType(Dialog)).dx, closeTo(1300, 1));
      PrimaryRegionScope.maybeOf(context)!.value = null;
      await tester.pumpAndSettle();
      expect(tester.getCenter(find.byType(Dialog)).dx, closeTo(800, 1));
    },
  );
}

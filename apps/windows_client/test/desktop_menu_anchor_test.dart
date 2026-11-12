import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_menu.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';
import 'package:multitap_client/ui/desktop_tile.dart';

const targetId = 'pc-12345678-1234-4234-8234-123456789abc';
const target = DesktopMachine(
  id: targetId,
  name: 'TOWER-PC',
  description: '',
  kind: MachineKind.pc,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.blue,
  localPcApproved: true,
  powerState: 'running',
  powerStale: false,
  hostOnline: true,
  interactiveSession: true,
  virtualDisplayReady: true,
);

class Fixture {
  final scroll = ScrollController();
  var actions = 0;
  late final DesktopActions callbacks = DesktopActions(
    primary: () => actions++,
    stream: (_) => actions++,
    quality: () => actions++,
    details: () => actions++,
    favorite: false,
    toggleFavorite: () => actions++,
  );

  Widget surface({
    Size size = const Size(1200, 900),
    double scale = 1,
    bool row = false,
    bool dense = false,
  }) {
    final readiness = DesktopReadiness.of(target, server: 'fixture.invalid');
    Widget tile(String id) {
      final value = id == 'target'
          ? target
          : DesktopMachine(
              id: id == 'upper-left' ? 'pve-149' : 'pve-170',
              name: id,
              description: '',
              kind: MachineKind.vm,
              status: MachineStatus.ready,
              specification: 'Windows',
              accent: Colors.blue,
              powerState: 'running',
              powerStale: false,
              hostOnline: true,
              interactiveSession: true,
              virtualDisplayReady: true,
            );
      return SizedBox(
        width: 420,
        child: DesktopTile(
          key: ValueKey(id),
          machine: value,
          readiness: DesktopReadiness.of(value, server: 'fixture.invalid'),
          actions: callbacks,
          mode: SessionDisplayMode.windowed,
        ),
      );
    }

    return MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: MediaQuery(
        data: MediaQueryData(size: size, textScaler: TextScaler.linear(scale)),
        child: Scaffold(
          body: SingleChildScrollView(
            key: const Key('fixture-scroll'),
            controller: scroll,
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  if (!row)
                    Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        tile('upper-left'),
                        const SizedBox(width: 20),
                        tile('upper-right'),
                      ],
                    )
                  else
                    const SizedBox(height: 800),
                  const SizedBox(height: 20),
                  if (row)
                    DesktopRow(
                      key: const ValueKey('target'),
                      machine: target,
                      readiness: readiness,
                      actions: callbacks,
                      mode: SessionDisplayMode.windowed,
                      dense: dense,
                    )
                  else
                    tile('target'),
                  const SizedBox(height: 900),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

Finder targetWidget(bool row) => find.byKey(const ValueKey('target'));
Finder trigger(String prefix, {bool row = false}) => find.descendant(
  of: targetWidget(row),
  matching: find.byKey(Key('$prefix-$targetId')),
);

Future<void> show(
  WidgetTester t,
  Fixture fixture, {
  Size size = const Size(1200, 900),
  double scale = 1,
  bool row = false,
  bool dense = false,
}) async {
  t.view.physicalSize = size;
  t.view.devicePixelRatio = 1;
  await t.pumpWidget(
    fixture.surface(size: size, scale: scale, row: row, dense: dense),
  );
  await t.pump();
  // Put the second row's footer near the bottom: the old full-card anchor
  // flips its popup above the whole card, into the upper row.
  final footer = t.getRect(trigger('stream-modes', row: row));
  fixture.scroll.jumpTo(
    math.max(0, fixture.scroll.offset + footer.bottom - size.height + 30),
  );
  await t.pumpAndSettle();
}

Rect popupItems(WidgetTester t) => t
    .getRect(find.byKey(const Key('stream-mode-windowed')))
    .expandToInclude(
      t.getRect(find.byKey(const Key('stream-settings-$targetId'))),
    );

void expectAtTrigger(WidgetTester t, Rect button, Size viewport) {
  final popup = popupItems(t);
  expect(popup.left, greaterThanOrEqualTo(0));
  expect(popup.top, greaterThanOrEqualTo(0));
  expect(popup.right, lessThanOrEqualTo(viewport.width));
  expect(popup.bottom, lessThanOrEqualTo(viewport.height));
  expect(popup.bottom, lessThanOrEqualTo(button.top + 2));
  expect(
    button.top - popup.bottom,
    lessThan(24),
    reason: 'Flip above the clicked button, not above the entire card.',
  );
  expect(popup.right, greaterThan(button.left));
  expect(popup.left, lessThan(button.right));
}

void main() {
  testWidgets(
    'Bottom local-PC card arrow flips beside its own footer rather than into the upper row',
    (t) async {
      final fixture = Fixture();
      addTearDown(fixture.scroll.dispose);
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      await show(t, fixture);
      final button = t.getRect(trigger('stream-modes'));
      await t.tap(trigger('stream-modes'));
      await t.pumpAndSettle();
      expectAtTrigger(t, button, const Size(1200, 900));
      expect(
        find.byKey(const Key('emergency-console-$targetId')),
        findsNothing,
      );
      expect(fixture.actions, 0);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Scroll and window resize dismiss old menus; reopen uses the new footer at 200 percent',
    (t) async {
      final fixture = Fixture();
      addTearDown(fixture.scroll.dispose);
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      await show(t, fixture);
      await t.tap(trigger('stream-modes'));
      await t.pumpAndSettle();
      final move = fixture.scroll.animateTo(
        fixture.scroll.offset + 40,
        duration: const Duration(milliseconds: 80),
        curve: Curves.linear,
      );
      await t.pumpAndSettle();
      await move;
      expect(find.byKey(const Key('stream-mode-windowed')), findsNothing);
      await show(t, fixture, size: const Size(960, 900), scale: 2);
      var button = t.getRect(trigger('stream-modes'));
      await t.tap(trigger('stream-modes'));
      await t.pumpAndSettle();
      expectAtTrigger(t, button, const Size(960, 900));
      await show(t, fixture, size: const Size(1366, 900), scale: 2);
      expect(find.byKey(const Key('stream-mode-windowed')), findsNothing);
      button = t.getRect(trigger('stream-modes'));
      await t.tap(trigger('stream-modes'));
      await t.pumpAndSettle();
      expectAtTrigger(t, button, const Size(1366, 900));
      expect(fixture.actions, 0);
      expect(t.takeException(), isNull);
    },
  );

  for (final dense in [false, true]) {
    testWidgets(
      'List/mini footer anchor follows its current row near the viewport edge (dense=$dense)',
      (t) async {
        final fixture = Fixture();
        addTearDown(fixture.scroll.dispose);
        addTearDown(t.view.resetPhysicalSize);
        addTearDown(t.view.resetDevicePixelRatio);
        await show(t, fixture, row: true, dense: dense);
        final button = t.getRect(trigger('stream-modes', row: true));
        await t.tap(trigger('stream-modes', row: true));
        await t.pumpAndSettle();
        expectAtTrigger(t, button, const Size(1200, 900));
        await t.sendKeyEvent(LogicalKeyboardKey.escape);
        await t.pumpAndSettle();
        expect(find.byKey(const Key('stream-mode-windowed')), findsNothing);
        expect(fixture.actions, 0);
        expect(t.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'One click switches footer to same-card management; outside click only dismisses',
    (t) async {
      final fixture = Fixture();
      addTearDown(fixture.scroll.dispose);
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      await show(t, fixture);
      // Keep both controls exposed. At the bottom edge the upward popup
      // legitimately covers the management button, so it cannot be clicked.
      fixture.scroll.jumpTo(fixture.scroll.offset + 330);
      await t.pumpAndSettle();
      await t.tap(trigger('stream-modes'));
      await t.pumpAndSettle();
      await t.tap(trigger('desktop-more'));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('stream-mode-windowed')), findsNothing);
      expect(find.byKey(const Key('details-$targetId')), findsOneWidget);
      await t.tapAt(const Offset(1180, 870));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('details-$targetId')), findsNothing);
      expect(fixture.actions, 0);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Right-click anchors to the actual scrolled pointer point, not the card origin',
    (t) async {
      final fixture = Fixture();
      addTearDown(fixture.scroll.dispose);
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      await show(t, fixture);
      final point =
          t.getRect(trigger('stream-modes')).topLeft - const Offset(60, 5);
      final mouse = await t.startGesture(
        point,
        kind: PointerDeviceKind.mouse,
        buttons: kSecondaryMouseButton,
      );
      await mouse.up();
      await t.pumpAndSettle();
      final last = t.getRect(find.byKey(const Key('details-$targetId')));
      expect(point.dy - last.bottom, inInclusiveRange(0, 24));
      expect(last.left, greaterThanOrEqualTo(0));
      expect(last.right, lessThanOrEqualTo(1200));
      expect(fixture.actions, 0);
      expect(t.takeException(), isNull);
    },
  );
}

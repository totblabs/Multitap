// One card on both platforms: the Android launcher renders the Windows launcher's tile
// (DesktopTile) in the Windows grid (desktopTileGrid), adapted only for
// touch: 48 dp targets and the ▶ without a hover. Both real launchers are
// rendered over fixtures: no broker, VM or device.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_tile.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'android_launcher_test.dart' show signedIn, vm;

const _ids = ['pve-101', 'pve-102', 'pve-103'];

/// Running and ready, stopped, running and ready: every tile state's main
/// button key, as the Windows tile names it.
const _main = {
  'pve-101': 'open-pve-101',
  'pve-102': 'start-pve-102',
  'pve-103': 'open-pve-103',
};

List<BrokerDesktop> _desktops() => [
  vm(id: 'pve-101', name: '문서 작업용 데스크톱'),
  vm(id: 'pve-102', name: '개발용 VM', power: 'stopped', stream: false),
  vm(id: 'pve-103', name: '검증 VM'),
];

/// What holds on either platform: every tile is a [DesktopTile] with the
/// Windows tile's parts, in a grid of [desktopTileGrid]'s columns, one size
/// for every state and the main buttons on one line.
void _expectTiles(
  WidgetTester t, {
  required bool touch,
  required Size viewport,
  required double textScale,
}) {
  final tiles = t.widgetList<DesktopTile>(find.byType(DesktopTile)).toList();
  expect(tiles.map((tile) => tile.machine.id), _ids);
  for (final tile in tiles) {
    expect(tile.touch, touch, reason: tile.machine.id);
  }
  for (final id in _ids) {
    for (final part in const [
      'card',
      'power-state',
      'desktop-name',
      'resources',
      'status',
      'desktop-power',
      'desktop-more',
    ]) {
      expect(find.byKey(Key('$part-$id')), findsOneWidget, reason: '$part-$id');
    }
    expect(find.byKey(Key(_main[id]!)), findsOneWidget, reason: _main[id]);
  }
  final cards = [for (final id in _ids) t.getRect(find.byKey(Key('card-$id')))];
  // The first row is full (three tiles, at most three columns): its span
  // is the grid's width.
  final columns = cards.where((r) => (r.top - cards.first.top).abs() < .5);
  final width = columns.last.right - cards.first.left;
  final grid = desktopTileGrid(
    width,
    TextScaler.linear(textScale),
    viewport: viewport,
    touch: touch,
  );
  expect(columns.length, grid.columns, reason: 'columns at $width');
  for (final (index, card) in cards.indexed) {
    expect(card.width, closeTo(grid.width, .5), reason: _ids[index]);
    expect(card.height, closeTo(grid.height, .5), reason: _ids[index]);
    final button = t.getRect(find.byKey(Key(_main[_ids[index]]!)));
    expect(
      button.top - card.top,
      closeTo(
        t.getRect(find.byKey(Key(_main[_ids.first]!))).top - cards.first.top,
        .5,
      ),
      reason: 'the footer of ${_ids[index]}',
    );
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  for (final (size, textScale) in const [
    (Size(360, 800), 1.0),
    (Size(412, 915), 1.0),
    (Size(412, 915), 1.6),
    (Size(915, 412), 1.0),
    (Size(932, 704), 1.0),
    (Size(1280, 800), 1.0),
    (Size(1280, 800), 1.5),
  ]) {
    testWidgets(
      'Android ${size.width.toInt()}x${size.height.toInt()} text $textScale: '
      'the Windows tile in the Windows grid, by finger',
      (t) async {
        await signedIn(t, _desktops());
        t.view.physicalSize = size;
        t.view.devicePixelRatio = 1;
        t.platformDispatcher.textScaleFactorTestValue = textScale;
        addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
        await t.pumpAndSettle();
        expect(t.takeException(), isNull);
        _expectTiles(t, touch: true, viewport: size, textScale: textScale);
        for (final tile in t.widgetList<DesktopTile>(
          find.byType(DesktopTile),
        )) {
          // One full screen: no window modes on the button or in ▾.
          expect(tile.mode, isNull);
        }
        // 48 dp targets.
        for (final key in [
          'desktop-power-pve-101',
          'desktop-more-pve-101',
          'open-pve-101',
          'stream-modes-pve-101',
        ]) {
          final rect = t.getRect(find.byKey(Key(key)));
          expect(rect.height, greaterThanOrEqualTo(47.99), reason: key);
          if (key != 'open-pve-101') {
            expect(rect.width, greaterThanOrEqualTo(47.99), reason: key);
          }
        }
        // No hover by finger: the ▶ is there to tap.
        expect(
          find.byKey(const Key('play-pve-101')).hitTestable(),
          findsOneWidget,
        );
        await t.pumpWidget(const SizedBox.shrink());
        await t.pumpAndSettle();
      },
    );
  }

  testWidgets('Android: ▾ holds 화질 옵션 and 긴급 콘솔 as the Windows ▾ (no '
      'window modes); ⋯ manages; a greyed row says why under its label', (
    t,
  ) async {
    await signedIn(t, [vm(id: 'pve-101', canStop: false)]);
    await t.ensureVisible(find.byKey(const Key('stream-modes-pve-101')));
    await t.pumpAndSettle();
    await t.tap(find.byKey(const Key('stream-modes-pve-101')));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('stream-settings-pve-101')), findsOneWidget);
    expect(find.byKey(const Key('stream-mode-windowed')), findsNothing);
    expect(find.byKey(const Key('stream-mode-fullscreen')), findsNothing);
    expect(
      t.getSize(find.byKey(const Key('stream-settings-pve-101'))).height,
      greaterThanOrEqualTo(48),
    );
    await t.tapAt(Offset.zero);
    await t.pumpAndSettle();
    await t.tap(find.byKey(const Key('desktop-more-pve-101')));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('details-pve-101')), findsOneWidget);
    expect(find.byKey(const Key('stream-settings-pve-101')), findsNothing);
    await t.tapAt(Offset.zero);
    await t.pumpAndSettle();
    // ⏻: 정지 is greyed here (a server without it), its reason in place.
    await t.tap(find.byKey(const Key('desktop-power-pve-101')));
    await t.pumpAndSettle();
    expect(
      t.getSize(find.byKey(const Key('power-shutdown-pve-101'))).height,
      greaterThanOrEqualTo(48),
    );
    expect(
      find.descendant(
        of: find.byKey(const Key('power-stop-pve-101')),
        matching: find.byKey(const Key('power-stop-pve-101-reason')),
      ),
      findsOneWidget,
    );
    expect(find.textContaining('정지를 쓸 수 없어요'), findsOneWidget);
    await t.pumpWidget(const SizedBox.shrink());
    await t.pumpAndSettle();
  });

  for (final (size, textScale) in const [
    (Size(700, 900), 1.0),
    (Size(1000, 900), 1.0),
    (Size(1440, 900), 1.0),
    (Size(1440, 900), 1.5),
    (Size(1200, 460), 1.0),
  ]) {
    testWidgets(
      'Windows ${size.width.toInt()}x${size.height.toInt()} text $textScale: '
      'the same tile and grid rule, by mouse',
      (t) async {
        t.view.devicePixelRatio = 1;
        t.view.physicalSize = size;
        addTearDown(t.view.reset);
        final appearance = AppAppearance();
        addTearDown(appearance.dispose);
        await t.pumpWidget(
          AppearanceScope(
            controller: appearance,
            child: MaterialApp(
              theme: buildAppTheme(),
              builder: (_, child) => MediaQuery(
                data: MediaQueryData(
                  size: size,
                  textScaler: TextScaler.linear(textScale),
                ),
                child: child!,
              ),
              home: WorkspaceShell(
                onSignOut: () {},
                onRefresh: () {},
                displayName: 'tester',
                serverOrigin: 'https://vdi.example',
                machines: [
                  for (final desktop in _desktops()) machineFromBroker(desktop),
                ],
                onConnect: (_, {options = const StreamOptions()}) async =>
                    throw StateError('fixture does not connect'),
                onPower: (_, _) async {},
              ),
            ),
          ),
        );
        await t.pumpAndSettle();
        expect(t.takeException(), isNull);
        _expectTiles(t, touch: false, viewport: size, textScale: textScale);
        await t.pumpWidget(const SizedBox.shrink());
        await t.pumpAndSettle();
      },
    );
  }
}

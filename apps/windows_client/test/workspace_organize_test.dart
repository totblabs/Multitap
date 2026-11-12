import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart' show BrokerException;
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

const _origin = 'https://private-broker.example.com';

DesktopMachine _vm(
  String id,
  String name, {
  List<String> tags = const [],
  String? pool,
  bool? favorite,
  DateTime? used,
  String power = 'running',
}) => DesktopMachine(
  id: id,
  name: name,
  description: '',
  kind: MachineKind.vm,
  status: power == 'stopped' ? MachineStatus.poweredOff : MachineStatus.ready,
  specification: 'Windows 11',
  accent: Colors.blue,
  powerState: power,
  powerStale: false,
  powerCanForceStop: true,
  hostOnline: power == 'running',
  organize: DesktopOrganize(
    tags: tags,
    pool: pool,
    favorite: favorite ?? false,
    lastUsedAt: used,
    supported: favorite != null,
  ),
);

DesktopMachine _pc(String id, String name, {String power = 'running'}) =>
    DesktopMachine(
      id: id,
      name: name,
      description: '',
      kind: MachineKind.pc,
      status: power == 'stopped'
          ? MachineStatus.poweredOff
          : MachineStatus.ready,
      specification: 'Windows 11',
      accent: Colors.teal,
      powerState: power,
      powerStale: false,
      hostOnline: power == 'running',
    );

/// The five VMs and one local PC.
List<DesktopMachine> _mixed() => [..._fleet(), _pc('pc-gaming', 'TOWER-PC')];

/// Five desktops with tags, a pool and one favourite.
List<DesktopMachine> _fleet() => [
  _vm('pve-101', '사무실 PC', tags: ['업무'], pool: '영업팀', favorite: false),
  _vm('pve-149', 'Dev Box', tags: ['dev'], favorite: true),
  _vm('pve-150', 'Lab', tags: ['dev', '업무'], favorite: false, power: 'stopped'),
  _vm('pve-160', '민수 데스크톱', favorite: false),
  _vm('pve-170', 'Zeta', pool: '영업팀', favorite: false),
];

final _searchInput = find.byKey(const Key('desktop-search-input'));

class _Host {
  _Host(this.machines);
  final ValueNotifier<List<DesktopMachine>> machines;
  final favorites = <(String, bool)>[];
  Future<void> Function(String, bool)? onFavorite;
}

Future<_Host> _show(
  WidgetTester t,
  List<DesktopMachine> machines, {
  Size size = const Size(1440, 1400),
  double scale = 1,
  LauncherMemory? memory,
  bool favorites = true,
  Brightness brightness = Brightness.light,
  Future<void> Function(String, bool)? onFavorite,
  bool busy = false,
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = size;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  final host = _Host(ValueNotifier(machines));
  addTearDown(host.machines.dispose);
  await t.pumpWidget(
    MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildAppTheme(brightness: brightness),
      builder: (_, child) => MediaQuery(
        data: MediaQueryData(size: size, textScaler: TextScaler.linear(scale)),
        child: child!,
      ),
      home: ValueListenableBuilder<List<DesktopMachine>>(
        valueListenable: host.machines,
        builder: (_, list, _) => WorkspaceShell(
          onSignOut: () {},
          onRefresh: () {},
          displayName: 'tester',
          serverOrigin: _origin,
          machines: list,
          busy: busy,
          launcherMemory: memory,
          onConnect: (_, {options = const StreamOptions()}) async =>
              throw StateError('UI fixture does not connect'),
          onPower: (_, _) async {},
          onFavorite: favorites
              ? (id, value) {
                  host.favorites.add((id, value));
                  return (onFavorite ?? (_, _) async {})(id, value);
                }
              : null,
        ),
      ),
    ),
  );
  if (busy) {
    // The list read's progress bar and spinner never settle.
    await t.pump();
    await t.pump(const Duration(milliseconds: 300));
  } else {
    await t.pumpAndSettle();
  }
  return host;
}

Finder _tile(String id) => find.byKey(Key('tile-$id'));

/// Visible tile IDs in reading order.
List<String> _order(WidgetTester t) {
  final found = <(String, Offset)>[];
  for (final element
      in find
          .byWidgetPredicate(
            (w) => w.key is Key && '${w.key}'.startsWith("[<'tile-"),
          )
          .evaluate()) {
    final key = (element.widget.key as ValueKey<String>).value;
    found.add((key.substring(5), t.getTopLeft(find.byWidget(element.widget))));
  }
  found.sort((a, b) {
    // A hovered or focused tile floats a few pixels: compare rows, not pixels.
    final y = (a.$2.dy / 60).round().compareTo((b.$2.dy / 60).round());
    return y != 0 ? y : a.$2.dx.compareTo(b.$2.dx);
  });
  return [for (final (id, _) in found) id];
}

Future<void> _type(WidgetTester t, String text) async {
  await t.enterText(_searchInput, text);
  await t.pumpAndSettle();
}

Future<void> _tapStatus(WidgetTester t, String name) async {
  final target = find.byKey(Key('status-$name'));
  await t.ensureVisible(target);
  await t.pumpAndSettle();
  var visible = t.getRect(target);
  final row = find.byKey(const Key('desktop-status-row'));
  if (row.evaluate().isNotEmpty) {
    visible = visible.intersect(t.getRect(row));
  }
  expect(
    visible.isEmpty,
    false,
    reason: 'The status segment must be reachable',
  );
  // A segment can be wider than its viewport at 200% text. Its un-clipped
  // centre can hit the sort menu instead; exercise the actually visible part.
  await t.tapAt(visible.center);
  await t.pumpAndSettle();
}

Future<void> _key(
  WidgetTester t,
  LogicalKeyboardKey key, {
  String? character,
  bool control = false,
}) async {
  if (control) await t.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
  await t.sendKeyDownEvent(key, character: character);
  await t.sendKeyUpEvent(key);
  if (control) await t.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
  await t.pumpAndSettle();
}

Future<void> _openMore(WidgetTester t, String id) async {
  await t.tap(find.byKey(Key('desktop-more-$id')));
  await t.pumpAndSettle();
}

void main() {
  group('search', () {
    testWidgets('finds by name, VM ID, tag, pool and 초성', (t) async {
      await _show(t, _fleet());
      expect(_order(t).length, 5);
      await _type(t, 'dev');
      expect(_order(t), ['pve-149', 'pve-150']);
      await _type(t, '149');
      expect(_order(t), ['pve-149']);
      await _type(t, '#16');
      expect(_order(t), ['pve-160']);
      await _type(t, '업무');
      expect(_order(t), ['pve-101', 'pve-150']);
      await _type(t, '영업');
      expect(_order(t), ['pve-101', 'pve-170']);
      await _type(t, 'ㅅㅁㅅ');
      expect(_order(t), ['pve-101']);
      await _type(t, 'ㅁㅅ');
      expect(_order(t), ['pve-101', 'pve-160']);
      expect(find.byKey(const Key('desktop-count')), findsOneWidget);
      expect(
        t.widget<Text>(find.byKey(const Key('desktop-count'))).data,
        '2/5',
      );
      expect(t.takeException(), isNull);
    });

    testWidgets('no match says so and 검색 지우기 brings the list back', (t) async {
      await _show(t, _fleet());
      await _type(t, '없는이름');
      expect(_order(t), isEmpty);
      expect(find.text("'없는이름'에 맞는 데스크톱이 없어요"), findsOneWidget);
      await t.tap(find.byKey(const Key('no-match-clear')));
      await t.pumpAndSettle();
      expect(_order(t).length, 5);
      expect(find.byKey(const Key('no-match-desktops')), findsNothing);
      expect(t.widget<TextField>(_searchInput).controller!.text, isEmpty);
    });

    testWidgets('Enter with one result moves focus to the tile, not connect', (
      t,
    ) async {
      await _show(t, _fleet());
      await _type(t, 'zeta');
      await t.testTextInput.receiveAction(TextInputAction.search);
      await t.pumpAndSettle();
      final focus = FocusManager.instance.primaryFocus;
      expect(focus?.debugLabel, 'desktop-pve-170');
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
    });

    testWidgets('Ctrl+F and / focus the box; Esc clears, then leaves', (
      t,
    ) async {
      await _show(t, _fleet());
      await _key(t, LogicalKeyboardKey.keyF, control: true);
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'desktop-search');
      await _type(t, 'lab');
      expect(_order(t), ['pve-150']);
      await _key(t, LogicalKeyboardKey.escape);
      expect(t.widget<TextField>(_searchInput).controller!.text, isEmpty);
      expect(_order(t).length, 5);
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'desktop-search');
      await _key(t, LogicalKeyboardKey.escape);
      expect(
        FocusManager.instance.primaryFocus?.debugLabel,
        isNot('desktop-search'),
      );
      // A slash opens it again and is not typed.
      await _key(t, LogicalKeyboardKey.slash, character: '/');
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'desktop-search');
      expect(t.widget<TextField>(_searchInput).controller!.text, isEmpty);
    });

    testWidgets('typing on the launcher starts a search with that character', (
      t,
    ) async {
      await _show(t, _fleet());
      await t.tap(_tile('pve-101'));
      _tile('pve-101');
      FocusManager.instance.primaryFocus?.unfocus();
      await t.pumpAndSettle();
      await _key(t, LogicalKeyboardKey.keyZ, character: 'z');
      expect(t.widget<TextField>(_searchInput).controller!.text, 'z');
      expect(_order(t), ['pve-170']);
      // Typing in the box is the box's own business, not a restart.
      await t.enterText(_searchInput, 'zeta');
      await t.pumpAndSettle();
      expect(t.widget<TextField>(_searchInput).controller!.text, 'zeta');
    });

    testWidgets('Space on a focused tile does not start a search', (t) async {
      await _show(t, _fleet());
      await _key(t, LogicalKeyboardKey.space, character: ' ');
      expect(find.byKey(const Key('no-match-desktops')), findsNothing);
      expect(t.widget<TextField>(_searchInput).controller!.text, isEmpty);
    });

    testWidgets('a refresh while searching keeps the query and the filter', (
      t,
    ) async {
      final host = await _show(t, _fleet());
      await _type(t, 'dev');
      await t.tap(find.byKey(const Key('chip-tag:업무')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-150']);
      host.machines.value = _fleet();
      await t.pumpAndSettle();
      expect(t.widget<TextField>(_searchInput).controller!.text, 'dev');
      expect(_order(t), ['pve-150']);
      // A changed VM state pushes through the same way.
      host.machines.value = [
        for (final m in _fleet())
          m.id == 'pve-150'
              ? _vm('pve-150', 'Lab', tags: ['dev', '업무'], favorite: false)
              : m,
      ];
      await t.pumpAndSettle();
      expect(_order(t), ['pve-150']);
    });

    testWidgets('a list refresh keeps the focused tile and the scroll', (
      t,
    ) async {
      final host = await _show(t, _fleet(), size: const Size(1000, 500));
      _tile('pve-149');
      FocusManager.instance.primaryFocus?.unfocus();
      final scroll = find.byType(SingleChildScrollView).first;
      await t.drag(scroll, const Offset(0, -150));
      await t.pumpAndSettle();
      final offset = t
          .state<ScrollableState>(find.byType(Scrollable).first)
          .position
          .pixels;
      host.machines.value = _fleet();
      await t.pumpAndSettle();
      expect(
        t.state<ScrollableState>(find.byType(Scrollable).first).position.pixels,
        offset,
      );
    });
  });

  group('분류', () {
    testWidgets('a chip filters, ANDs with the search and the status', (
      t,
    ) async {
      await _show(t, _fleet());
      // 즐겨찾기, pools and tags are chips; 전체 and the states are not.
      expect([
        for (final key in const ['favorites', 'pool:영업팀', 'tag:dev', 'tag:업무'])
          find.byKey(Key('chip-$key')).evaluate().isNotEmpty,
      ], everyElement(isTrue));
      for (final gone in const ['all', 'running', 'stopped']) {
        expect(find.byKey(Key('chip-$gone')), findsNothing);
      }
      await t.tap(find.byKey(const Key('chip-tag:dev')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-149', 'pve-150']);
      await _type(t, 'lab');
      expect(_order(t), ['pve-150']);
      await t.tap(find.byKey(const Key('status-stopped')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-150']);
      await _type(t, 'zeta');
      // Nothing in 꺼짐 + dev matches: the message offers the way back.
      expect(find.byKey(const Key('no-match-all')), findsOneWidget);
      await t.tap(find.byKey(const Key('no-match-all')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-170']);
      await t.tap(find.byKey(const Key('chip-pool:영업팀')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-170']);
    });

    testWidgets('pressing the chosen chip again lets go of it', (t) async {
      await _show(t, _fleet());
      await t.tap(find.byKey(const Key('chip-tag:dev')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-149', 'pve-150']);
      await t.tap(find.byKey(const Key('chip-tag:dev')));
      await t.pumpAndSettle();
      expect(_order(t).length, 5);
      expect(find.byKey(const Key('desktop-filter-reset')), findsNothing);
    });

    testWidgets('no favourites, tags or pools: no chip row at all', (t) async {
      await _show(t, [
        _vm('pve-1', 'A', favorite: false),
        _vm('pve-2', 'B', favorite: false),
        _vm('pve-3', 'C', favorite: false),
        _vm('pve-4', 'D', favorite: false),
      ]);
      expect(find.byKey(const Key('desktop-chips')), findsNothing);
      expect(find.byKey(const Key('chip-all')), findsNothing);
      expect(find.byKey(const Key('chip-running')), findsNothing);
      expect(find.byKey(const Key('chip-favorites')), findsNothing);
      // The status filter and the search are there regardless.
      expect(find.byKey(const Key('desktop-status')), findsOneWidget);
      expect(find.byKey(const Key('desktop-search')), findsOneWidget);
      expect(find.byKey(const Key('desktop-sort')), findsOneWidget);
    });

    testWidgets('three plain desktops: status filter and a search icon only', (
      t,
    ) async {
      await _show(t, [
        _vm('pve-1', 'A', favorite: false),
        _vm('pve-2', 'B', favorite: false),
        _vm('pve-3', 'C', favorite: false),
      ]);
      expect(find.byKey(const Key('desktop-chips')), findsNothing);
      expect(find.byKey(const Key('desktop-sort')), findsNothing);
      // The row never disappears just because there are few desktops.
      expect(find.byKey(const Key('desktop-status')), findsOneWidget);
      expect(find.byKey(const Key('desktop-search-open')), findsOneWidget);
      expect(find.byKey(const Key('desktop-search')), findsNothing);
      // The shortcut still works.
      await _key(t, LogicalKeyboardKey.keyF, control: true);
      expect(find.byKey(const Key('desktop-search')), findsOneWidget);
      await _type(t, 'b');
      expect(_order(t), ['pve-2']);
    });

    testWidgets('a tag or a favourite makes even two desktops filterable', (
      t,
    ) async {
      await _show(t, [
        _vm('pve-1', 'A', tags: ['x'], favorite: false),
        _vm('pve-2', 'B', favorite: false),
      ]);
      expect(find.byKey(const Key('desktop-chips')), findsOneWidget);
      expect(find.byKey(const Key('desktop-sort')), findsOneWidget);
    });

    testWidgets('the row scrolls sideways in a narrow window with big text', (
      t,
    ) async {
      final machines = [
        for (var i = 0; i < 9; i++)
          _vm(
            'pve-${100 + i}',
            'Desktop $i',
            tags: ['아주긴태그이름-$i'],
            pool: '풀-$i',
            favorite: i == 0,
          ),
      ];
      await _show(t, machines, size: const Size(360, 780), scale: 2);
      expect(t.takeException(), isNull);
      final row = find.byKey(const Key('desktop-chips'));
      expect(row, findsOneWidget);
      final scrollable = find.descendant(
        of: row,
        matching: find.byType(Scrollable),
      );
      final state = t.state<ScrollableState>(scrollable);
      expect(state.position.maxScrollExtent, greaterThan(0));
      expect(t.getSize(row).height, lessThan(80));
      // The last chip is reachable by scrolling the row.
      await t.drag(row, const Offset(-3000, 0));
      await t.pumpAndSettle();
      expect(state.position.pixels, greaterThan(1000));
      expect(t.takeException(), isNull);
      // The search opens as a full-width field without overflow.
      await t.tap(find.byKey(const Key('desktop-search-open')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('desktop-search')), findsOneWidget);
      expect(t.takeException(), isNull);
    });
  });
  group('search box design', () {
    /// The widgets under [root] that draw a border of their own.
    List<Widget> bordered(WidgetTester t, Finder root) => [
      for (final element
          in find
              .descendant(of: root, matching: find.byType(DecoratedBox))
              .evaluate())
        if (((element.widget as DecoratedBox).decoration is BoxDecoration) &&
            ((element.widget as DecoratedBox).decoration as BoxDecoration)
                    .border !=
                null)
          element.widget,
    ];

    BoxDecoration pill(WidgetTester t) =>
        t.widget<Container>(find.byKey(const Key('desktop-search'))).decoration
            as BoxDecoration;

    for (final dark in [false, true]) {
      final name = dark ? 'dark' : 'light';
      final palette = AppPalette(dark);
      final brightness = dark ? Brightness.dark : Brightness.light;

      testWidgets('$name: one rounded field, nothing nested inside', (t) async {
        await _show(t, _fleet(), brightness: brightness);
        final search = find.byKey(const Key('desktop-search'));
        expect(search, findsOneWidget);
        // The container itself carries the only border...
        final decoration = pill(t);
        expect(decoration.border, isNotNull);
        expect(decoration.borderRadius, BorderRadius.circular(10));
        expect((decoration.border as Border).top.width, 1);
        expect((decoration.border as Border).top.color, palette.glassEdge);
        // ...no other decorated box inside it has one (the one found is the
        // container's own decoration)...
        expect(bordered(t, search), hasLength(1));
        // ...and the text field draws no border, fill or outline of its own.
        final field = find.descendant(
          of: search,
          matching: find.byType(InputDecorator),
        );
        expect(field, findsOneWidget);
        final inner = t.widget<InputDecorator>(field).decoration;
        for (final border in [
          inner.border,
          inner.enabledBorder,
          inner.focusedBorder,
          inner.disabledBorder,
          inner.errorBorder,
          inner.focusedErrorBorder,
        ]) {
          expect(border, InputBorder.none);
          expect(border, isNot(isA<OutlineInputBorder>()));
          expect(border, isNot(isA<UnderlineInputBorder>()));
        }
        expect(inner.filled, isFalse);
        expect(inner.contentPadding, EdgeInsets.zero);
        expect(inner.isDense, isTrue);
        expect(find.byType(OutlineInputBorder), findsNothing);
        expect(t.takeException(), isNull);
      });

      testWidgets('$name: the amber ring is on the container only', (t) async {
        await _show(t, _fleet(), brightness: brightness);
        expect((pill(t).border as Border).top.color, palette.glassEdge);
        expect(pill(t).boxShadow, anyOf(isNull, isEmpty));
        await t.tap(find.byKey(const Key('desktop-search')));
        await t.pumpAndSettle();
        expect(
          FocusManager.instance.primaryFocus?.debugLabel,
          'desktop-search',
        );
        expect((pill(t).border as Border).top.color, palette.amberLine);
        expect(pill(t).boxShadow, isNotEmpty);
        // Still exactly one bordered thing, and the field inside stays bare.
        expect(
          bordered(t, find.byKey(const Key('desktop-search'))),
          hasLength(1),
        );
        final inner = t
            .widget<InputDecorator>(
              find.descendant(
                of: find.byKey(const Key('desktop-search')),
                matching: find.byType(InputDecorator),
              ),
            )
            .decoration;
        expect(inner.focusedBorder, InputBorder.none);
        // Leaving the box puts the quiet edge back.
        FocusManager.instance.primaryFocus?.unfocus();
        await t.pumpAndSettle();
        expect((pill(t).border as Border).top.color, palette.glassEdge);
      });
    }

    testWidgets('placeholder 검색, a faint Ctrl F keycap only while idle', (
      t,
    ) async {
      await _show(t, _fleet());
      final field = t.widget<TextField>(_searchInput);
      expect(field.decoration!.hintText, '검색');
      expect(field.decoration!.hintText, isNot(contains('Ctrl')));
      final hint = find.byKey(const Key('desktop-search-hint'));
      expect(hint, findsOneWidget);
      expect(
        find.descendant(of: hint, matching: find.text('Ctrl F')),
        findsOneWidget,
      );
      // The keycap is not a second bordered box.
      expect(
        (t.widget<Container>(hint).decoration as BoxDecoration).border,
        isNull,
      );
      // Focused: the keycap steps aside.
      await t.tap(_searchInput);
      await t.pumpAndSettle();
      expect(hint, findsNothing);
      // Typing: the clear button takes the place.
      await _type(t, 'dev');
      expect(find.byKey(const Key('desktop-search-clear')), findsOneWidget);
      expect(hint, findsNothing);
      // Left idle with text: still no keycap (the clear button is there).
      FocusManager.instance.primaryFocus?.unfocus();
      await t.pumpAndSettle();
      expect(hint, findsNothing);
      expect(find.byKey(const Key('desktop-search-clear')), findsOneWidget);
    });

    testWidgets('the × clears with a hand cursor, a tooltip and Esc too', (
      t,
    ) async {
      await _show(t, _fleet());
      expect(find.byKey(const Key('desktop-search-clear')), findsNothing);
      await _type(t, 'dev');
      final clear = find.byKey(const Key('desktop-search-clear'));
      expect(clear, findsOneWidget);
      expect(
        find.descendant(
          of: clear,
          matching: find.byWidgetPredicate(
            (w) => w is Tooltip && w.message == '지우기',
          ),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: clear,
          matching: find.byWidgetPredicate(
            (w) => w is MouseRegion && w.cursor == SystemMouseCursors.click,
          ),
        ),
        findsOneWidget,
      );
      await t.tap(clear);
      await t.pumpAndSettle();
      expect(t.widget<TextField>(_searchInput).controller!.text, isEmpty);
      expect(_order(t).length, 5);
      // The box keeps the focus after the clear button.
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'desktop-search');
      await _type(t, 'lab');
      await _key(t, LogicalKeyboardKey.escape);
      expect(t.widget<TextField>(_searchInput).controller!.text, isEmpty);
      expect(find.byKey(const Key('desktop-search-clear')), findsNothing);
    });

    testWidgets('the collapsed magnifier names the shortcut and grows out', (
      t,
    ) async {
      await _show(t, [
        _vm('pve-1', 'A', favorite: false),
        _vm('pve-2', 'B', favorite: false),
      ]);
      final open = find.byKey(const Key('desktop-search-open'));
      expect(t.widget<IconButton>(open).tooltip, '검색 (Ctrl+F)');
      expect(t.getSize(open), const Size(34, 34));
      await t.tap(open);
      await t.pump();
      await t.pump(const Duration(milliseconds: 90));
      final mid = t.getSize(find.byKey(const Key('desktop-search')));
      expect(mid.width, greaterThan(34));
      expect(mid.width, lessThan(240));
      expect(mid.height, 34);
      await t.pumpAndSettle();
      expect(
        t.getSize(find.byKey(const Key('desktop-search'))),
        const Size(240, 34),
      );
      expect(t.takeException(), isNull);
      // Leaving it empty folds it back into the magnifier.
      FocusManager.instance.primaryFocus?.unfocus();
      await t.pumpAndSettle();
      expect(open, findsOneWidget);
      expect(find.byKey(const Key('desktop-search')), findsNothing);
      expect(t.getSize(open), const Size(34, 34));
    });

    testWidgets('same height as the neighbouring header buttons', (t) async {
      // 2026-11-03: 새로고침 was shorter and 미리보기 taller and square.
      for (final (size, scale, expected) in [
        (const Size(1440, 1400), 1.0, 34.0),
        (const Size(1440, 1400), 1.5, 37.3),
        (const Size(1440, 1400), 2.0, 46.4),
        (const Size(420, 900), 1.0, 34.0),
        (const Size(420, 900), 2.0, 46.4),
      ]) {
        await t.pumpWidget(const SizedBox());
        await _show(t, _mixed(), size: size, scale: scale);
        final heights = {
          for (final key in const [
            'desktop-kind',
            'desktop-status',
            'desktop-sort',
            'desktop-previews',
            'refresh-desktops',
          ])
            key: t.getSize(find.byKey(Key(key))).height,
          // The search is a magnifier or the open box, by width.
          'search': t
              .getSize(
                find.byKey(const Key('desktop-search')).evaluate().isEmpty
                    ? find.byKey(const Key('desktop-search-open'))
                    : find.byKey(const Key('desktop-search')),
              )
              .height,
        };
        expect(heights.values.map((h) => h.toStringAsFixed(1)).toSet(), {
          expected.toStringAsFixed(1),
        }, reason: '$size ×$scale $heights');
        expect(t.takeException(), isNull);
      }
    });

    testWidgets('미리보기 is a toggle that shows whether it is on', (t) async {
      await _show(t, _fleet());
      final button = find.byKey(const Key('desktop-previews'));
      Material material() => t.widget<Material>(
        find.descendant(of: button, matching: find.byType(Material)).first,
      );
      final palette = AppPalette(false);
      expect(material().color, palette.amberSoft);
      expect(
        find.byWidgetPredicate(
          (w) => w is Tooltip && w.message == '화면 미리보기 켜짐 · 누르면 꺼요',
        ),
        findsOneWidget,
      );
      bool? toggled() => t
          .widget<Semantics>(
            find.descendant(
              of: button,
              matching: find.byWidgetPredicate(
                (w) => w is Semantics && w.properties.button == true,
              ),
            ),
          )
          .properties
          .toggled;
      expect(toggled(), isTrue);
      await t.tap(button);
      await t.pumpAndSettle();
      expect(material().color, palette.pillGlass);
      expect(toggled(), isFalse);
      expect(
        find.byWidgetPredicate(
          (w) => w is Tooltip && w.message == '화면 미리보기 꺼짐 · 누르면 켜요',
        ),
        findsOneWidget,
      );
    });

    testWidgets('새로고침 keeps its shape and label while the list is read', (
      t,
    ) async {
      await _show(t, _fleet());
      final refresh = find.byKey(const Key('refresh-desktops'));
      final idle = t.getSize(refresh);
      expect(find.byKey(const Key('header-button-busy')), findsNothing);
      await t.pumpWidget(const SizedBox());
      await _show(t, _fleet(), busy: true);
      expect(t.getSize(refresh), idle);
      expect(
        find.descendant(of: refresh, matching: find.text('새로고침')),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: refresh,
          matching: find.byKey(const Key('header-button-busy')),
        ),
        findsOneWidget,
      );
      // Not faded out: the same fill as when idle.
      expect(
        t
            .widget<Material>(
              find
                  .descendant(of: refresh, matching: find.byType(Material))
                  .first,
            )
            .color,
        AppPalette(false).pillGlass,
      );
    });

    testWidgets('a short title line keeps 미리보기 and 새로고침 as round icons', (
      t,
    ) async {
      await _show(t, _fleet(), size: const Size(360, 800));
      for (final key in const ['desktop-previews', 'refresh-desktops']) {
        final size = t.getSize(find.byKey(Key(key)));
        expect(size.width, size.height, reason: key);
      }
      expect(find.text('새로고침'), findsNothing);
      expect(
        find.byWidgetPredicate((w) => w is Tooltip && w.message == '목록 새로고침'),
        findsOneWidget,
      );
      // Both sit on the title line, inside the window.
      final title = t.getRect(find.byKey(const Key('workspace-title')));
      for (final key in const ['desktop-previews', 'refresh-desktops']) {
        final r = t.getRect(find.byKey(Key(key)));
        expect(r.center.dy, closeTo(title.center.dy, 12), reason: key);
        expect(r.right, lessThanOrEqualTo(360), reason: key);
      }
      expect(t.takeException(), isNull);
    });
  });

  group('구분 filter', () {
    Finder segment(String name) => find.byKey(Key('kind-$name'));
    String count(WidgetTester t, String name) =>
        t.widget<Text>(find.byKey(Key('kind-count-$name'))).data!;
    Color? fill(WidgetTester t, String name) =>
        t.widget<Material>(segment(name)).color;

    testWidgets('only with both VMs and local PCs; counts like 상태', (t) async {
      await _show(t, _fleet());
      expect(find.byKey(const Key('desktop-kind')), findsNothing);
      await t.pumpWidget(const SizedBox());
      await _show(t, [_pc('pc-1', 'A'), _pc('pc-2', 'B')]);
      expect(find.byKey(const Key('desktop-kind')), findsNothing);
      await t.pumpWidget(const SizedBox());
      await _show(t, _mixed());
      expect(find.byKey(const Key('desktop-kind')), findsOneWidget);
      expect(
        [count(t, 'all'), count(t, 'vm'), count(t, 'pc')],
        ['6', '5', '1'],
      );
      expect(find.text('로컬 PC'), findsWidgets);
      expect(fill(t, 'all'), AppPalette(false).amberSoft);
      // Beside the status filter, on the title line of a wide window.
      final kind = t.getRect(find.byKey(const Key('desktop-kind')));
      final status = t.getRect(find.byKey(const Key('desktop-status')));
      expect(kind.right, lessThan(status.left));
      expect(kind.center.dy, closeTo(status.center.dy, 1));
    });

    testWidgets('each filter counts what the other one leaves', (t) async {
      String status(String name) =>
          t.widget<Text>(find.byKey(Key('status-count-$name'))).data!;
      await _show(t, _mixed());
      await t.tap(segment('pc'));
      await t.pumpAndSettle();
      // 로컬 PC: one running PC, so 상태 reads 1 · 1 · 0, not the fleet's 6 · 5 · 1.
      expect(
        [status('all'), status('running'), status('stopped')],
        ['1', '1', '0'],
      );
      await t.tap(segment('all'));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('status-stopped')));
      await t.pumpAndSettle();
      // 꺼짐: only the stopped VM, so 구분 reads 1 · 1 · 0.
      expect(
        [count(t, 'all'), count(t, 'vm'), count(t, 'pc')],
        ['1', '1', '0'],
      );
      expect(t.takeException(), isNull);
    });

    testWidgets('ANDs with 상태 and the search; 필터 해제 clears it', (t) async {
      await _show(t, _mixed());
      await t.tap(segment('pc'));
      await t.pumpAndSettle();
      expect(_order(t), ['pc-gaming']);
      expect(
        t.widget<Text>(find.byKey(const Key('desktop-count'))).data,
        '1/6',
      );
      expect(fill(t, 'pc'), AppPalette(false).amberSoft);
      await t.tap(segment('vm'));
      await t.pumpAndSettle();
      expect(_order(t).length, 5);
      await t.tap(find.byKey(const Key('status-stopped')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-150']);
      // No local PC is off, so 로컬 PC counts 0 under 꺼짐 and can't be chosen
      // into an empty list.
      expect(count(t, 'pc'), '0');
      await t.tap(segment('pc'));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-150']);
      await t.tap(find.byKey(const Key('desktop-filter-reset')));
      await t.pumpAndSettle();
      expect(_order(t).length, 6);
      expect(fill(t, 'all'), AppPalette(false).amberSoft);
      await _type(t, 'gaming');
      expect(_order(t), ['pc-gaming']);
      await t.tap(segment('vm'));
      await t.pumpAndSettle();
      expect(_order(t), isEmpty);
    });

    testWidgets('remembered per account like the other filters', (t) async {
      final memory = SessionLauncherMemory();
      await _show(t, _mixed(), memory: memory);
      await t.tap(segment('pc'));
      await t.pumpAndSettle();
      expect((await memory.load(_origin)).organizeFor('tester').kind, 'pc');
      await t.pumpWidget(const SizedBox());
      await _show(t, _mixed(), memory: memory);
      expect(fill(t, 'pc'), AppPalette(false).amberSoft);
      expect(_order(t), ['pc-gaming']);
      // Without local PCs the filter is gone and counts as 전체 again.
      await t.pumpWidget(const SizedBox());
      await _show(t, _fleet(), memory: memory);
      expect(_order(t).length, 5);
      expect((await memory.load(_origin)).organizeFor('tester').kind, 'all');
    });

    testWidgets('narrow windows put it with 상태 under the title, scrolling', (
      t,
    ) async {
      for (final scale in [1.0, 2.0]) {
        await t.pumpWidget(const SizedBox());
        await _show(t, _mixed(), size: const Size(360, 800), scale: scale);
        final title = t.getRect(find.byKey(const Key('workspace-title')));
        final kind = t.getRect(find.byKey(const Key('desktop-kind')));
        expect(kind.top, greaterThan(title.bottom - 1));
        expect(
          find.descendant(
            of: find.byKey(const Key('desktop-status-row')),
            matching: find.byKey(const Key('desktop-kind')),
          ),
          findsOneWidget,
        );
        final target = segment('pc');
        await t.ensureVisible(target);
        await t.pumpAndSettle();
        final visible = t
            .getRect(target)
            .intersect(t.getRect(find.byKey(const Key('desktop-status-row'))));
        await t.tapAt(visible.center);
        await t.pumpAndSettle();
        expect(_order(t), ['pc-gaming'], reason: '×$scale');
        expect(t.takeException(), isNull);
      }
    });
  });

  group('상태 filter', () {
    Finder segment(String name) => find.byKey(Key('status-$name'));
    Color? fill(WidgetTester t, String name) =>
        t.widget<Material>(segment(name)).color;
    String count(WidgetTester t, String name) =>
        t.widget<Text>(find.byKey(Key('status-count-$name'))).data!;
    Finder tooltip(String message) =>
        find.byWidgetPredicate((w) => w is Tooltip && w.message == message);
    FocusNode? focusOf(WidgetTester t, String name) => t
        .widget<InkWell>(
          find.descendant(of: segment(name), matching: find.byType(InkWell)),
        )
        .focusNode;

    testWidgets('전체 · 실행 중 · 꺼짐 with live counts, 전체 selected', (t) async {
      await _show(t, _fleet());
      expect(
        [count(t, 'all'), count(t, 'running'), count(t, 'stopped')],
        ['5', '4', '1'],
      );
      final palette = AppPalette(false);
      expect(fill(t, 'all'), palette.amberSoft);
      expect(fill(t, 'running'), Colors.transparent);
      expect(fill(t, 'stopped'), Colors.transparent);
      for (final label in const ['전체', '실행 중', '꺼짐']) {
        expect(find.text(label), findsWidgets);
      }
    });

    testWidgets('single selection filters the list and keeps the counts', (
      t,
    ) async {
      await _show(t, _fleet());
      await t.tap(segment('running'));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-149', 'pve-101', 'pve-160', 'pve-170']);
      expect(fill(t, 'running'), AppPalette(false).amberSoft);
      expect(fill(t, 'all'), Colors.transparent);
      expect(
        t.widget<Text>(find.byKey(const Key('desktop-count'))).data,
        '4/5',
      );
      // The counts describe the fleet, not the filtered view.
      expect(
        [count(t, 'all'), count(t, 'running'), count(t, 'stopped')],
        ['5', '4', '1'],
      );
      await t.tap(segment('stopped'));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-150']);
      expect(fill(t, 'running'), Colors.transparent);
      expect(
        t.widget<Text>(find.byKey(const Key('desktop-count'))).data,
        '1/5',
      );
      // 전체 clears the status filter.
      await t.tap(segment('all'));
      await t.pumpAndSettle();
      expect(_order(t).length, 5);
      expect(t.widget<Text>(find.byKey(const Key('desktop-count'))).data, '5');
    });

    testWidgets('hand cursor and a tooltip on every available segment', (
      t,
    ) async {
      await _show(t, _fleet());
      for (final name in const ['all', 'running', 'stopped']) {
        expect(
          find.descendant(
            of: find
                .ancestor(of: segment(name), matching: find.byType(MouseRegion))
                .first,
            matching: find.byKey(Key('status-$name')),
          ),
          findsOneWidget,
        );
        final region = t.widget<MouseRegion>(
          find
              .ancestor(of: segment(name), matching: find.byType(MouseRegion))
              .first,
        );
        expect(region.cursor, SystemMouseCursors.click, reason: name);
      }
      expect(tooltip('모든 데스크톱 보기'), findsOneWidget);
      expect(tooltip('실행 중인 데스크톱만 보기'), findsOneWidget);
      expect(tooltip('꺼진 데스크톱만 보기'), findsOneWidget);
    });

    testWidgets('a segment without members stays, disabled, saying why', (
      t,
    ) async {
      // Everything runs: nothing is 꺼짐.
      await _show(t, [
        _vm('pve-1', 'A', favorite: false),
        _vm('pve-2', 'B', favorite: false),
      ]);
      expect(segment('stopped'), findsOneWidget);
      expect(count(t, 'stopped'), '0');
      expect(tooltip('꺼진 데스크톱이 없어요'), findsOneWidget);
      expect(tooltip('꺼진 데스크톱만 보기'), findsNothing);
      final ink = t.widget<InkWell>(
        find.descendant(of: segment('stopped'), matching: find.byType(InkWell)),
      );
      expect(ink.onTap, isNull);
      final region = t.widget<MouseRegion>(
        find
            .ancestor(
              of: segment('stopped'),
              matching: find.byType(MouseRegion),
            )
            .first,
      );
      expect(region.cursor, SystemMouseCursors.basic);
      await t.tap(segment('stopped'));
      await t.pumpAndSettle();
      expect(_order(t).length, 2);
      expect(fill(t, 'all'), AppPalette(false).amberSoft);
      expect(fill(t, 'stopped'), Colors.transparent);
      // And the mirror image: all powered off, nothing is 실행 중.
      await t.pumpWidget(const SizedBox());
      await _show(t, [
        _vm('pve-1', 'A', favorite: false, power: 'stopped'),
        _vm('pve-2', 'B', favorite: false, power: 'stopped'),
      ]);
      expect(tooltip('실행 중인 데스크톱이 없어요'), findsOneWidget);
      expect(
        t
            .widget<InkWell>(
              find.descendant(
                of: segment('running'),
                matching: find.byType(InkWell),
              ),
            )
            .onTap,
        isNull,
      );
      expect(segment('stopped'), findsOneWidget);
    });

    testWidgets('left/right arrows move the choice, Home/End jump', (t) async {
      await _show(t, _fleet());
      focusOf(t, 'all')!.requestFocus();
      await t.pump();
      await _key(t, LogicalKeyboardKey.arrowRight);
      expect(fill(t, 'running'), AppPalette(false).amberSoft);
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'status-running');
      await _key(t, LogicalKeyboardKey.arrowRight);
      expect(fill(t, 'stopped'), AppPalette(false).amberSoft);
      expect(_order(t), ['pve-150']);
      // The end stays put.
      await _key(t, LogicalKeyboardKey.arrowRight);
      expect(fill(t, 'stopped'), AppPalette(false).amberSoft);
      await _key(t, LogicalKeyboardKey.arrowLeft);
      expect(fill(t, 'running'), AppPalette(false).amberSoft);
      await _key(t, LogicalKeyboardKey.home);
      expect(fill(t, 'all'), AppPalette(false).amberSoft);
      await _key(t, LogicalKeyboardKey.end);
      expect(fill(t, 'stopped'), AppPalette(false).amberSoft);
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'status-stopped');
      // Enter chooses the focused segment too.
      focusOf(t, 'running')!.requestFocus();
      await t.pump();
      await _key(t, LogicalKeyboardKey.enter);
      expect(fill(t, 'running'), AppPalette(false).amberSoft);
    });

    testWidgets('arrows skip a segment that cannot be chosen', (t) async {
      await _show(t, [
        _vm('pve-1', 'A', favorite: false),
        _vm('pve-2', 'B', favorite: false),
      ]);
      focusOf(t, 'all')!.requestFocus();
      await t.pump();
      await _key(t, LogicalKeyboardKey.arrowRight);
      expect(fill(t, 'running'), AppPalette(false).amberSoft);
      await _key(t, LogicalKeyboardKey.arrowRight);
      // 꺼짐 is disabled: the choice stays on 실행 중.
      expect(fill(t, 'running'), AppPalette(false).amberSoft);
      await _key(t, LogicalKeyboardKey.end);
      expect(fill(t, 'running'), AppPalette(false).amberSoft);
      expect(focusOf(t, 'stopped')!.canRequestFocus, isFalse);
    });

    testWidgets('typing while a segment has focus still starts a search', (
      t,
    ) async {
      await _show(t, _fleet());
      focusOf(t, 'all')!.requestFocus();
      await t.pump();
      await _key(t, LogicalKeyboardKey.keyZ, character: 'z');
      expect(t.widget<TextField>(_searchInput).controller!.text, 'z');
      expect(_order(t), ['pve-170']);
    });

    testWidgets('ANDs with the 분류 chip and the search; counter follows', (
      t,
    ) async {
      await _show(t, _fleet());
      await t.tap(segment('running'));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('chip-tag:업무')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-101']);
      expect(
        t.widget<Text>(find.byKey(const Key('desktop-count'))).data,
        '1/5',
      );
      await t.tap(segment('stopped'));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-150']);
      await _type(t, 'lab');
      expect(_order(t), ['pve-150']);
      await _type(t, 'dev box');
      expect(_order(t), isEmpty);
      // Changing the chip keeps the status filter.
      await _type(t, '');
      await t.tap(find.byKey(const Key('chip-tag:dev')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-150']);
      expect(fill(t, 'stopped'), AppPalette(false).amberSoft);
    });

    testWidgets('empty result names the conditions; 필터 해제 gets out', (t) async {
      await _show(t, _fleet());
      await t.tap(segment('stopped'));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('chip-pool:영업팀')));
      await t.pumpAndSettle();
      expect(_order(t), isEmpty);
      expect(find.text('조건에 맞는 데스크톱이 없어요'), findsOneWidget);
      expect(find.text("'꺼짐' · '영업팀' 조건 안에서 찾았어요."), findsOneWidget);
      expect(find.byKey(const Key('no-match-clear')), findsNothing);
      expect(
        t.widget<Text>(find.byKey(const Key('desktop-count'))).data,
        '0/5',
      );
      await t.tap(find.byKey(const Key('no-match-all')));
      await t.pumpAndSettle();
      expect(_order(t).length, 5);
      expect(fill(t, 'all'), AppPalette(false).amberSoft);
      expect(find.byKey(const Key('desktop-filter-reset')), findsNothing);
    });

    testWidgets('empty result with a search offers both ways back', (t) async {
      await _show(t, _fleet());
      await t.tap(segment('stopped'));
      await t.pumpAndSettle();
      await _type(t, 'zeta');
      expect(find.text("'zeta'에 맞는 데스크톱이 없어요"), findsOneWidget);
      expect(find.text("'꺼짐' 조건 안에서 찾았어요."), findsOneWidget);
      await t.tap(find.byKey(const Key('no-match-clear')));
      await t.pumpAndSettle();
      // The search is gone, the status filter stays.
      expect(_order(t), ['pve-150']);
      expect(fill(t, 'stopped'), AppPalette(false).amberSoft);
    });

    testWidgets('필터 해제 shows only while a filter is active', (t) async {
      await _show(t, _fleet());
      final reset = find.byKey(const Key('desktop-filter-reset'));
      expect(reset, findsNothing);
      await t.tap(segment('running'));
      await t.pumpAndSettle();
      expect(reset, findsOneWidget);
      expect(find.text('필터 해제'), findsOneWidget);
      await t.tap(find.byKey(const Key('chip-tag:dev')));
      await t.pumpAndSettle();
      await _type(t, 'dev');
      expect(_order(t), ['pve-149']);
      await t.tap(reset);
      await t.pumpAndSettle();
      // Status and chip are cleared; the search text is its own business.
      expect(reset, findsNothing);
      expect(fill(t, 'all'), AppPalette(false).amberSoft);
      expect(_order(t), ['pve-149', 'pve-150']);
      expect(t.widget<TextField>(_searchInput).controller!.text, 'dev');
    });

    testWidgets('visible with one desktop; gone with none', (t) async {
      await _show(t, [_vm('pve-1', 'A', favorite: false)]);
      expect(find.byKey(const Key('desktop-status')), findsOneWidget);
      expect(
        [count(t, 'all'), count(t, 'running'), count(t, 'stopped')],
        ['1', '1', '0'],
      );
      await t.pumpWidget(const SizedBox());
      await _show(t, const []);
      expect(find.byKey(const Key('desktop-status')), findsNothing);
      expect(find.byKey(const Key('empty-desktops')), findsOneWidget);
    });

    testWidgets('a live power change moves the counts and the list', (t) async {
      final host = await _show(t, _fleet());
      await t.tap(segment('stopped'));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-150']);
      host.machines.value = [
        for (final m in _fleet())
          m.id == 'pve-160'
              ? _vm('pve-160', '민수 데스크톱', favorite: false, power: 'stopped')
              : m,
      ];
      await t.pumpAndSettle();
      expect([count(t, 'running'), count(t, 'stopped')], ['3', '2']);
      expect(_order(t), ['pve-150', 'pve-160']);
    });

    testWidgets('the selected segment reads in dark too', (t) async {
      await _show(t, _fleet(), brightness: Brightness.dark);
      await t.tap(segment('running'));
      await t.pumpAndSettle();
      expect(fill(t, 'running'), AppPalette(true).amberSoft);
      expect(t.takeException(), isNull);
    });
  });

  group('header layout', () {
    Rect rect(WidgetTester t, String key) => t.getRect(find.byKey(Key(key)));

    testWidgets('wide: title left; status, search, sort, refresh on one line', (
      t,
    ) async {
      await _show(t, _fleet(), size: const Size(1440, 900));
      final title = rect(t, 'workspace-title');
      final status = rect(t, 'desktop-status');
      final search = rect(t, 'desktop-search');
      final sort = rect(t, 'desktop-sort');
      final refresh = rect(t, 'refresh-desktops');
      for (final other in [status, search, sort, refresh]) {
        expect(other.center.dy, closeTo(title.center.dy, 12));
      }
      expect(title.right, lessThan(status.left));
      expect(status.right, lessThan(search.left));
      expect(search.right, lessThan(sort.left));
      expect(sort.right, lessThan(refresh.left));
      expect(t.takeException(), isNull);
    });

    testWidgets('medium: the search is a magnifier on the title line', (
      t,
    ) async {
      await _show(t, _fleet(), size: const Size(900, 900));
      expect(find.byKey(const Key('desktop-search-open')), findsOneWidget);
      expect(
        rect(t, 'desktop-status').center.dy,
        closeTo(rect(t, 'workspace-title').center.dy, 12),
      );
      await t.tap(find.byKey(const Key('desktop-search-open')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('desktop-search')), findsOneWidget);
      expect(t.takeException(), isNull);
    });

    testWidgets('narrow: the status filter and search sit under the title', (
      t,
    ) async {
      await _show(t, _fleet(), size: const Size(420, 800));
      final title = rect(t, 'workspace-title');
      final status = rect(t, 'desktop-status');
      expect(status.top, greaterThan(title.bottom - 1));
      expect(rect(t, 'desktop-sort').center.dy, closeTo(status.center.dy, 1));
      expect(
        rect(t, 'desktop-search-open').center.dy,
        closeTo(status.center.dy, 1),
      );
      expect(t.takeException(), isNull);
      // Opening the search takes a row below the filter; the filter stays.
      await t.tap(find.byKey(const Key('desktop-search-open')));
      await t.pumpAndSettle();
      final search = rect(t, 'desktop-search');
      expect(search.top, greaterThan(status.bottom - 1));
      expect(search.left, closeTo(16, 1));
      expect(search.right, closeTo(420 - 16, 1));
      expect(find.byKey(const Key('desktop-status')), findsOneWidget);
      // The × closes an empty box and clears a filled one.
      expect(find.byKey(const Key('desktop-search-close')), findsOneWidget);
      await _type(t, 'dev');
      expect(find.byKey(const Key('desktop-search-close')), findsNothing);
      await t.tap(find.byKey(const Key('desktop-search-clear')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('desktop-search')), findsOneWidget);
      await t.tap(find.byKey(const Key('desktop-search-close')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('desktop-search')), findsNothing);
      expect(find.byKey(const Key('desktop-search-open')), findsOneWidget);
      expect(t.takeException(), isNull);
    });

    for (final (size, scale) in [
      (const Size(300, 700), 1.0),
      (const Size(300, 700), 2.0),
      (const Size(360, 780), 2.0),
      (const Size(600, 800), 2.0),
      (const Size(1000, 800), 1.6),
      (const Size(1440, 900), 2.0),
    ]) {
      testWidgets('no overflow at ${size.width.toInt()} dp, text ×$scale', (
        t,
      ) async {
        for (final machines in [
          _fleet(),
          [
            for (var i = 0; i < 10; i++)
              _vm(
                'pve-${200 + i}',
                'VM $i',
                favorite: i == 3,
                tags: [if (i.isEven) '아주긴태그이름'],
                power: i == 7 ? 'stopped' : 'running',
              ),
          ],
        ]) {
          await t.pumpWidget(const SizedBox());
          await _show(t, machines, size: size, scale: scale);
          expect(t.takeException(), isNull, reason: '${machines.length}');
          // Everything stays inside the window sideways.
          for (final key in const [
            'desktop-status',
            'desktop-sort',
            'refresh-desktops',
          ]) {
            final r = rect(t, key);
            expect(r.left, greaterThanOrEqualTo(-1), reason: key);
            if (key != 'desktop-status') {
              expect(r.right, lessThanOrEqualTo(size.width + 1), reason: key);
            }
          }
          // Open the search, type, pick a status and look again.
          await _key(t, LogicalKeyboardKey.keyF, control: true);
          await _type(t, 'vm');
          // Enlarged text can clip this segment in its horizontal row. Bring
          // it into that viewport rather than clicking through its clip at
          // an unrelated neighboring control.
          await _tapStatus(t, 'running');
          expect(t.takeException(), isNull);
          final search = rect(t, 'desktop-search');
          expect(search.right, lessThanOrEqualTo(size.width + 1));
          expect(search.height, lessThan(90));
        }
      });
    }

    testWidgets('the status row scrolls sideways at 300 dp, text ×2', (
      t,
    ) async {
      await _show(t, _fleet(), size: const Size(300, 700), scale: 2);
      final row = find.byKey(const Key('desktop-status-row'));
      final state = t.state<ScrollableState>(
        find.descendant(of: row, matching: find.byType(Scrollable)),
      );
      expect(state.position.maxScrollExtent, greaterThan(0));
      await t.drag(row, const Offset(-400, 0));
      await t.pumpAndSettle();
      expect(state.position.pixels, greaterThan(0));
      // The last segment is reachable and works.
      await _tapStatus(t, 'stopped');
      expect(_order(t), ['pve-150']);
      expect(t.takeException(), isNull);
    });

    testWidgets('enlarged text grows search and status together', (t) async {
      await _show(t, _fleet(), size: const Size(1600, 900), scale: 1.6);
      await _key(t, LogicalKeyboardKey.keyF, control: true);
      final heights = {
        t.getSize(find.byKey(const Key('desktop-search'))).height,
        t.getSize(find.byKey(const Key('desktop-status'))).height,
        t.getSize(find.byKey(const Key('desktop-sort'))).height,
      };
      expect(heights.length, 1, reason: '$heights');
      expect(heights.single, greaterThan(34));
    });
  });

  group('status memory', () {
    testWidgets('comes back for this account only, search text does not', (
      t,
    ) async {
      final memory = SessionLauncherMemory();
      await _show(t, _fleet(), memory: memory);
      await t.tap(find.byKey(const Key('status-stopped')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('chip-tag:dev')));
      await t.pumpAndSettle();
      await _type(t, 'lab');
      final saved = (await memory.load(_origin)).organizeFor('tester');
      expect((saved.chip, saved.status), ('tag:dev', 'stopped'));
      expect((await memory.load(_origin)).organizeFor('other').status, 'all');

      await t.pumpWidget(const SizedBox());
      await _show(t, _fleet(), memory: memory);
      expect(_order(t), ['pve-150']);
      expect(t.widget<TextField>(_searchInput).controller!.text, isEmpty);
      expect(
        t.widget<Material>(find.byKey(const Key('status-stopped'))).color,
        AppPalette(false).amberSoft,
      );
      expect(find.byKey(const Key('desktop-filter-reset')), findsOneWidget);
    });

    testWidgets('a stale remembered status falls back to 전체 silently', (
      t,
    ) async {
      final memory = SessionLauncherMemory();
      await memory.save(
        _origin,
        const LauncherPreferences().withOrganize(
          'tester',
          const OrganizePreference(status: 'stopped', sort: 'recent'),
        ),
      );
      // Nothing is powered off any more.
      await _show(t, [
        _vm('pve-1', 'A', favorite: false),
        _vm('pve-2', 'B', favorite: false),
      ], memory: memory);
      expect(_order(t).length, 2);
      expect(find.byKey(const Key('no-match-desktops')), findsNothing);
      expect(find.byKey(const Key('desktop-filter-reset')), findsNothing);
      final saved = (await memory.load(_origin)).organizeFor('tester');
      expect(saved.status, 'all');
      expect(saved.sort, 'recent');
    });

    testWidgets('an older 실행 중 / 꺼짐 chip is read as the status', (t) async {
      final memory = SessionLauncherMemory();
      await memory.save(
        _origin,
        const LauncherPreferences().withOrganize(
          'tester',
          const OrganizePreference(chip: 'stopped'),
        ),
      );
      await _show(t, _fleet(), memory: memory);
      expect(_order(t), ['pve-150']);
      expect(
        t.widget<Material>(find.byKey(const Key('status-stopped'))).color,
        AppPalette(false).amberSoft,
      );
      await t.tap(find.byKey(const Key('status-all')));
      await t.pumpAndSettle();
      final saved = (await memory.load(_origin)).organizeFor('tester');
      expect((saved.chip, saved.status), ('all', 'all'));
    });
  });
  group('sort', () {
    testWidgets('the menu orders by number, name, recency and state', (
      t,
    ) async {
      final machines = [
        _vm(
          'pve-3',
          'Charlie',
          favorite: false,
          used: DateTime.utc(2026, 1, 1),
        ),
        _vm('pve-1', 'Bravo', favorite: false, power: 'stopped'),
        _vm('pve-2', 'alpha', favorite: false, used: DateTime.utc(2026, 5, 1)),
        _vm('pve-4', 'Delta', favorite: false),
      ];
      await _show(t, machines);
      expect(_order(t), ['pve-3', 'pve-1', 'pve-2', 'pve-4']);
      Future<void> choose(String sort) async {
        await t.tap(find.byKey(const Key('desktop-sort')));
        await t.pumpAndSettle();
        await t.tap(find.byKey(Key('sort-$sort')));
        await t.pumpAndSettle();
      }

      await choose('name');
      expect(_order(t), ['pve-2', 'pve-1', 'pve-3', 'pve-4']);
      await choose('recent');
      expect(_order(t), ['pve-2', 'pve-3', 'pve-1', 'pve-4']);
      await choose('running');
      expect(_order(t), ['pve-3', 'pve-2', 'pve-4', 'pve-1']);
      await choose('number');
      expect(_order(t), ['pve-3', 'pve-1', 'pve-2', 'pve-4']);
    });

    testWidgets('favourites stay first under every sort', (t) async {
      await _show(t, [
        _vm('pve-1', 'Alpha', favorite: false),
        _vm('pve-2', 'Bravo', favorite: true),
        _vm('pve-3', 'Charlie', favorite: false),
        _vm('pve-4', 'Delta', favorite: true, power: 'stopped'),
      ]);
      expect(_order(t), ['pve-2', 'pve-4', 'pve-1', 'pve-3']);
      await t.tap(find.byKey(const Key('desktop-sort')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('sort-running')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-2', 'pve-4', 'pve-1', 'pve-3']);
      await t.tap(find.byKey(const Key('desktop-sort')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('sort-name')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-2', 'pve-4', 'pve-1', 'pve-3']);
    });
  });

  group('favourites', () {
    testWidgets('the ⋯ menu toggles at once and keeps the answer', (t) async {
      final answer = Completer<void>();
      final host = await _show(
        t,
        _fleet(),
        onFavorite: (_, _) => answer.future,
      );
      expect(find.byKey(const Key('favorite-star-pve-101')), findsNothing);
      await _openMore(t, 'pve-101');
      expect(find.text('즐겨찾기에 추가'), findsOneWidget);
      await t.tap(find.byKey(const Key('favorite-pve-101')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 50));
      // Optimistic: the star and the order change before the answer.
      expect(find.byKey(const Key('favorite-star-pve-101')), findsOneWidget);
      expect(_order(t).take(2), ['pve-101', 'pve-149']);
      expect(host.favorites, [('pve-101', true)]);
      // A second press while it is being sent does nothing more.
      await _openMore(t, 'pve-101');
      expect(find.text('즐겨찾기에서 빼기'), findsOneWidget);
      await t.tap(
        find.byKey(const Key('favorite-pve-101')),
        warnIfMissed: false,
      );
      await t.pump();
      expect(host.favorites.length, 1);
      answer.complete();
      await t.pumpAndSettle();
    });

    testWidgets('a failure rolls back and says so', (t) async {
      final answer = Completer<void>();
      final host = await _show(
        t,
        _fleet(),
        onFavorite: (_, _) => answer.future,
      );
      await _openMore(t, 'pve-160');
      await t.tap(find.byKey(const Key('favorite-pve-160')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 50));
      expect(find.byKey(const Key('favorite-star-pve-160')), findsOneWidget);
      answer.completeError(const BrokerException('network_error'));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('favorite-star-pve-160')), findsNothing);
      expect(find.text('즐겨찾기를 바꾸지 못했어요. 잠시 후 다시 시도하세요.'), findsOneWidget);
      expect(host.favorites, [('pve-160', true)]);
      expect(_order(t).first, 'pve-149');
    });

    testWidgets('a 404 explains that the VM is no longer assigned', (t) async {
      await _show(
        t,
        _fleet(),
        onFavorite: (_, _) async =>
            throw const BrokerException('desktop_not_found', 404),
      );
      await _openMore(t, 'pve-149');
      expect(find.text('즐겨찾기에서 빼기'), findsOneWidget);
      await t.tap(find.byKey(const Key('favorite-pve-149')));
      await t.pumpAndSettle();
      expect(find.text('이 데스크톱은 더 이상 내게 배정돼 있지 않아요.'), findsOneWidget);
      expect(find.byKey(const Key('favorite-star-pve-149')), findsOneWidget);
    });

    testWidgets('a broker without favourites shows no star, item or chip', (
      t,
    ) async {
      await _show(t, [
        _vm('pve-1', 'A', tags: ['x']),
        _vm('pve-2', 'B'),
        _vm('pve-3', 'C'),
        _vm('pve-4', 'D'),
      ]);
      await _openMore(t, 'pve-1');
      expect(find.byKey(const Key('favorite-pve-1')), findsNothing);
      expect(find.byKey(const Key('details-pve-1')), findsOneWidget);
      expect(find.byKey(const Key('chip-favorites')), findsNothing);
      expect(find.byKey(const Key('chip-tag:x')), findsOneWidget);
    });

    testWidgets('no handler (demo) hides the menu item too', (t) async {
      await _show(t, _fleet(), favorites: false);
      await _openMore(t, 'pve-101');
      expect(find.byKey(const Key('favorite-pve-101')), findsNothing);
    });
  });

  group('memory', () {
    testWidgets('chip and sort come back for this account only', (t) async {
      final memory = SessionLauncherMemory();
      await _show(t, _fleet(), memory: memory);
      await t.tap(find.byKey(const Key('chip-tag:dev')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('desktop-sort')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('sort-name')));
      await t.pumpAndSettle();
      await _type(t, 'dev');
      final saved = (await memory.load(_origin)).organizeFor('tester');
      expect(saved.chip, 'tag:dev');
      expect(saved.sort, 'name');
      expect((await memory.load(_origin)).organizeFor('other').chip, 'all');

      // Later launch: same chip and sort, but an empty search.
      await t.pumpWidget(const SizedBox());
      await _show(t, _fleet(), memory: memory);
      expect(_order(t), ['pve-149', 'pve-150']);
      expect(t.widget<TextField>(_searchInput).controller!.text, isEmpty);
      final selected = t.widget<Material>(
        find.byKey(const Key('chip-tag:dev')),
      );
      expect(selected.color, isNot(Colors.transparent));
    });

    testWidgets('a remembered chip that no longer exists falls back to 전체', (
      t,
    ) async {
      final memory = SessionLauncherMemory();
      await memory.save(
        _origin,
        const LauncherPreferences().withOrganize(
          'tester',
          const OrganizePreference(chip: 'tag:사라진태그', sort: 'recent'),
        ),
      );
      await _show(t, _fleet(), memory: memory);
      expect(_order(t).length, 5);
      expect(find.byKey(const Key('no-match-desktops')), findsNothing);
      final saved = (await memory.load(_origin)).organizeFor('tester');
      expect(saved.chip, 'all');
      // The sort is not touched by the fall back.
      expect(saved.sort, 'recent');
    });

    testWidgets('a status losing its last member falls back to 전체', (t) async {
      final memory = SessionLauncherMemory();
      final host = await _show(t, _fleet(), memory: memory);
      await t.tap(find.byKey(const Key('status-stopped')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-150']);
      expect(
        (await memory.load(_origin)).organizeFor('tester').status,
        'stopped',
      );
      host.machines.value = [
        for (final m in _fleet())
          m.id == 'pve-150'
              ? _vm('pve-150', 'Lab', tags: ['dev', '업무'], favorite: false)
              : m,
      ];
      await t.pumpAndSettle();
      expect(_order(t).length, 5);
      expect(find.byKey(const Key('no-match-desktops')), findsNothing);
      final saved = (await memory.load(_origin)).organizeFor('tester');
      expect((saved.chip, saved.status), ('all', 'all'));
    });

    testWidgets('a chip losing its last member falls back to 전체', (t) async {
      final memory = SessionLauncherMemory();
      final host = await _show(t, _fleet(), memory: memory);
      await t.tap(find.byKey(const Key('chip-tag:업무')));
      await t.pumpAndSettle();
      expect(_order(t), ['pve-101', 'pve-150']);
      host.machines.value = [
        for (final m in _fleet())
          _vm(
            m.id,
            m.name,
            tags: m.organize.tags.where((x) => x != '업무').toList(),
            pool: m.organize.pool,
            favorite: m.organize.favorite,
          ),
      ];
      await t.pumpAndSettle();
      expect(_order(t).length, 5);
      expect((await memory.load(_origin)).organizeFor('tester').chip, 'all');
    });

    testWidgets('list rows (many desktops) filter and star too', (t) async {
      final machines = [
        for (var i = 0; i < 10; i++)
          _vm(
            'pve-${200 + i}',
            'VM $i',
            favorite: i == 3,
            tags: [if (i.isEven) 'even'],
          ),
      ];
      await _show(t, machines);
      expect(find.byKey(const Key('card-pve-203')), findsOneWidget);
      expect(find.byKey(const Key('favorite-star-pve-203')), findsOneWidget);
      await _type(t, 'vm 3');
      expect(find.byKey(const Key('card-pve-203')), findsOneWidget);
      expect(find.byKey(const Key('card-pve-204')), findsNothing);
    });
  });
}

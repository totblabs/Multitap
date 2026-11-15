// Desktop groups (sections, the card's 그룹 menu, the header's ⋯) and the
// 보통 / 작게 card size, on the Windows launcher and its broker calls. All
// brokers, files and HTTP are synthetic.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_watch.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/login_memory.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/desktop_organize.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_tile.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestBroker, TestLoginMemory, fixtureOrigin;

const _origin = 'https://private-broker.example.com';

DesktopMachine _vm(
  String id,
  String name, {
  String? group,
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
  organize: DesktopOrganize(supported: true, group: group),
);

const _a = DesktopGroup(id: 'g1', name: '개발');
const _b = DesktopGroup(id: 'g2', name: '영업');

/// pve-2 and pve-4 in 개발, pve-1 in 영업, pve-3 in none.
List<DesktopMachine> _fleet() => [
  _vm('pve-1', 'Alpha', group: 'g2'),
  _vm('pve-2', 'Bravo', group: 'g1'),
  _vm('pve-3', 'Charlie'),
  _vm('pve-4', 'Delta', group: 'g1', power: 'stopped'),
];

/// The shell's broker side, as the server keeps it: the whole list saved in
/// order (new entries get an id, a group left out is deleted and its
/// desktops are in none) and one desktop's group.
class _Host {
  _Host(List<DesktopMachine> machines, List<DesktopGroup>? groups)
    : machines = ValueNotifier(machines),
      groups = ValueNotifier(groups);
  final ValueNotifier<List<DesktopMachine>> machines;
  final ValueNotifier<List<DesktopGroup>?> groups;
  final saved = <List<DesktopGroupDraft>>[];
  final sets = <(String, String?)>[];
  Object? failSet, failSave;
  var _next = 10;

  Future<List<DesktopGroup>> save(List<DesktopGroupDraft> drafts) async {
    saved.add(drafts);
    if (failSave case final error?) throw error;
    final list = [
      for (final draft in drafts)
        DesktopGroup(id: draft.id ?? 'g${_next++}', name: draft.name),
    ];
    final ids = {for (final group in list) group.id};
    groups.value = list;
    machines.value = [
      for (final m in machines.value)
        ids.contains(m.organize.group)
            ? m
            : m.withOrganize(m.organize.withGroup(null)),
    ];
    return list;
  }

  Future<void> set(String id, String? group) async {
    sets.add((id, group));
    if (failSet case final error?) throw error;
    machines.value = [
      for (final m in machines.value)
        m.id == id ? m.withOrganize(m.organize.withGroup(group)) : m,
    ];
  }
}

Future<_Host> _show(
  WidgetTester t, {
  List<DesktopMachine>? machines,
  List<DesktopGroup>? groups = const [_a, _b],
  bool supported = true,
  LauncherMemory? memory,
  Size size = const Size(1440, 2400),
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = size;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  final host = _Host(machines ?? _fleet(), groups);
  addTearDown(host.machines.dispose);
  addTearDown(host.groups.dispose);
  await t.pumpWidget(
    MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildAppTheme(),
      home: ValueListenableBuilder<List<DesktopGroup>?>(
        valueListenable: host.groups,
        builder: (_, groups, _) => ValueListenableBuilder<List<DesktopMachine>>(
          valueListenable: host.machines,
          builder: (_, list, _) => WorkspaceShell(
            onSignOut: () {},
            onRefresh: () {},
            displayName: 'tester',
            serverOrigin: _origin,
            machines: list,
            launcherMemory: memory,
            onConnect: (_, {options = const StreamOptions()}) async =>
                throw StateError('UI fixture does not connect'),
            onPower: (_, _) async {},
            onFavorite: (_, _) async {},
            groups: supported ? groups : null,
            onSaveGroups: supported ? host.save : null,
            onSetGroup: supported ? host.set : null,
          ),
        ),
      ),
    ),
  );
  await t.pumpAndSettle();
  return host;
}

/// Visible tile ids in reading order.
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
    final y = (a.$2.dy / 60).round().compareTo((b.$2.dy / 60).round());
    return y != 0 ? y : a.$2.dx.compareTo(b.$2.dx);
  });
  return [for (final (id, _) in found) id];
}

/// Section headers top to bottom.
List<String> _sections(WidgetTester t) {
  final found = <(String, double)>[];
  for (final element
      in find
          .byWidgetPredicate(
            (w) => w.key is ValueKey<String> &&
                (w.key as ValueKey<String>).value.startsWith('section-header-'),
          )
          .evaluate()) {
    final key = (element.widget.key as ValueKey<String>).value;
    found.add((
      key.substring('section-header-'.length),
      t.getTopLeft(find.byWidget(element.widget)).dy,
    ));
  }
  found.sort((a, b) => a.$2.compareTo(b.$2));
  return [for (final (key, _) in found) key];
}

String _count(WidgetTester t, String key) =>
    t.widget<Text>(find.byKey(Key('section-count-$key'))).data!;

Future<void> _tap(WidgetTester t, String key) async {
  await t.tap(find.byKey(Key(key)));
  await t.pumpAndSettle();
}

Future<void> _openGroups(WidgetTester t, String id) async {
  await t.ensureVisible(find.byKey(Key('desktop-more-$id')));
  await t.pumpAndSettle();
  await _tap(t, 'desktop-more-$id');
  await _tap(t, 'group-menu-$id');
}

Future<void> _type(WidgetTester t, String text) async {
  await t.enterText(find.byKey(const Key('desktop-search-input')), text);
  await t.pumpAndSettle();
}

void main() {
  group('model', () {
    test('groups and each desktop\'s group come from the list read', () {
      expect(DesktopGroup.listFromJson(null), isNull);
      expect(DesktopGroup.listFromJson({'g1': 'x'}), isNull);
      expect(DesktopGroup.listFromJson(const []), isEmpty);
      expect(
        DesktopGroup.listFromJson([
          {'id': 'g1', 'name': ' 개발 '},
          {'id': 'g1', 'name': 'again'},
          {'id': '', 'name': 'x'},
          {'id': 'g3', 'name': ''},
          'junk',
          {'id': 'g2', 'name': '영업'},
        ]),
        [const DesktopGroup(id: 'g1', name: '개발'), _b],
      );
      // A saved list's answer must be whole.
      expect(
        DesktopGroup.listFromJson([
          {'id': 'g1', 'name': '개발'},
          {'name': 'no id'},
        ], strict: true),
        isNull,
      );
      expect(DesktopOrganize.fromJson({'group': 'g1'}).group, 'g1');
      expect(DesktopOrganize.fromJson({'group': null}).group, isNull);
      expect(DesktopOrganize.fromJson({'group': 7}).group, isNull);
      expect(DesktopOrganize.fromJson(const {}).group, isNull);
      final organize = DesktopOrganize.fromJson({
        'group': 'g1',
        'favorite': true,
      });
      expect(organize.withFavorite(false).group, 'g1');
      expect(organize.withGroup(null).favorite, isTrue);
      expect(DesktopGroup.validName('a' * 40), isTrue);
      expect(DesktopGroup.validName('a' * 41), isFalse);
      expect(DesktopGroup.validName('   '), isFalse);
      expect(DesktopGroup.validName('a\nb'), isFalse);
    });

    test('sections: the account\'s order, then 기타 while it has a desktop', () {
      final all = _fleet();
      expect(desktopSections(all, all, null, filtering: false), isNull);
      expect(desktopSections(all, all, const [], filtering: false), isNull);
      final sections = desktopSections(
        all,
        all,
        const [_a, _b],
        filtering: false,
      )!;
      expect([for (final s in sections) s.key], [
        'group:g1',
        'group:g2',
        'other',
      ]);
      expect([for (final s in sections) s.machines.map((m) => m.id).join()], [
        'pve-2pve-4',
        'pve-1',
        'pve-3',
      ]);
      // A desktop in a group the list no longer has is in 기타.
      final stale = [_vm('pve-9', 'Old', group: 'gone'), ...all];
      expect(
        desktopSections(stale, stale, const [_a], filtering: false)!.last.total,
        3,
      );
      // No desktop outside a group: no 기타. An empty group stays.
      final grouped = [_vm('pve-1', 'A', group: 'g1')];
      expect(
        [
          for (final s in desktopSections(
            grouped,
            grouped,
            const [_a, _b],
            filtering: false,
          )!)
            (s.key, s.total),
        ],
        [('group:g1', 1), ('group:g2', 0)],
      );
      // Filtering: only the sections with a shown card, counted against all.
      final shown = [all[1]];
      final filtered = desktopSections(
        shown,
        all,
        const [_a, _b],
        filtering: true,
      )!;
      expect([for (final s in filtered) (s.key, s.machines.length, s.total)], [
        ('group:g1', 1, 2),
      ]);
    });
  });

  group('memory', () {
    late Directory dir;
    setUp(() => dir = Directory.systemTemp.createTempSync('totb-groups'));
    tearDown(() => dir.deleteSync(recursive: true));

    test('the folded sections and the card size round-trip', () async {
      final file = File('${dir.path}/launcher.json');
      final memory = FileLauncherMemory(file);
      expect((await memory.load('https://s')).smallCards, isFalse);
      await memory.save(
        'https://s',
        const LauncherPreferences(smallCards: true).withOrganize(
          'tester',
          const OrganizePreference(folded: ['group:g1', 'other']),
        ),
      );
      final back = await memory.load('https://s');
      expect(back.smallCards, isTrue);
      expect(back.organizeFor('tester').folded, ['group:g1', 'other']);
      expect(back.organizeFor('someone').folded, isEmpty);
      final entry = (jsonDecode(await file.readAsString()) as Map)['https://s'];
      expect(entry['cards'], 'small');
      // Other changes keep both.
      final opened = back.opened('pve-1', back.modeFor('pve-1'));
      expect(opened.smallCards, isTrue);
      expect(back.withView(back.view, showPreviews: false).smallCards, isTrue);
      // A file before them, or nonsense, reads as 보통 and nothing folded.
      await file.writeAsString(
        '{"https://s":{"cards":7,"organize":{"t":{"chip":"all","sort":"number","folded":"x"}}}}',
      );
      final old = await memory.load('https://s');
      expect(old.smallCards, isFalse);
      expect(old.organizeFor('t').folded, isEmpty);
    });
  });

  group('broker calls', () {
    test('the list read carries the groups; an older broker none', () async {
      final desktop = {
        'id': 'pve-1',
        'name': 'Alpha',
        'kind': 'vm',
        'description': '',
        'os': 'Windows 11',
        'status': 'online',
        'canConnect': true,
        'favorite': false,
        'group': 'g1',
      };
      final client = _ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [desktop],
            'groups': [
              {'id': 'g1', 'name': '개발'},
            ],
          },
        ))
        ..responses.add((
          200,
          {
            'desktops': [
              {...desktop}..remove('group'),
            ],
          },
        ));
      final api = HttpBrokerApi(
        origin: Uri.parse('https://cloud.example.com'),
        client: client,
      );
      final list = await readDesktopList(api, 'qa-token');
      expect(list.groups, [const DesktopGroup(id: 'g1', name: '개발')]);
      expect(list.desktops.single.organize.group, 'g1');
      final old = await readDesktopList(api, 'qa-token');
      expect(old.groups, isNull);
      expect(old.desktops.single.organize.group, isNull);
      expect(client.requests.map((r) => r.uri.path), [
        '/api/v1/desktops',
        '/api/v1/desktops',
      ]);
    });

    test('the whole list is one PUT in its order; new entries have no id', () async {
      final client = _ApiClient()
        ..responses.add((
          200,
          {
            'groups': [
              {'id': 'g2', 'name': '영업'},
              {'id': 'g9', 'name': '새 그룹'},
            ],
          },
        ))
        ..responses.add((
          400,
          {
            'error': {'code': 'duplicate_group_name', 'message': 'x'},
          },
        ))
        ..responses.add((200, {'groups': 'nope'}));
      final api = HttpBrokerApi(
        origin: Uri.parse('https://cloud.example.com'),
        client: client,
      );
      final saved = await api.saveDesktopGroups('qa-token', [
        (id: 'g2', name: '영업'),
        (id: null, name: ' 새 그룹 '),
      ]);
      expect(saved, [_b, const DesktopGroup(id: 'g9', name: '새 그룹')]);
      final request = client.requests.single;
      expect(request.method, 'PUT');
      expect(request.uri.path, '/api/v1/desktop-groups');
      expect(jsonDecode(request.body.toString()), {
        'groups': [
          {'id': 'g2', 'name': '영업'},
          {'name': '새 그룹'},
        ],
      });
      expect(
        request.headers.values[HttpHeaders.authorizationHeader],
        'Bearer qa-token',
      );
      await expectLater(
        api.saveDesktopGroups('qa-token', [(id: null, name: '영업')]),
        throwsA(
          isA<BrokerException>()
              .having((e) => e.code, 'code', 'duplicate_group_name')
              .having((e) => e.status, 'status', 400),
        ),
      );
      // A malformed answer is not taken as saved.
      await expectLater(
        api.saveDesktopGroups('qa-token', const []),
        throwsA(isA<BrokerException>()),
      );
      // More than 30 is refused before sending.
      await expectLater(
        api.saveDesktopGroups('qa-token', [
          for (var i = 0; i < 31; i++) (id: null, name: 'g$i'),
        ]),
        throwsA(isA<BrokerException>()),
      );
      expect(client.requests.length, 3);
    });

    test('one desktop\'s group is a PUT of the id or null', () async {
      final client = _ApiClient()
        ..responses.add((200, {'group': 'g1'}))
        ..responses.add((200, {'group': null}))
        ..responses.add((
          404,
          {
            'error': {'code': 'desktop_not_found', 'message': 'x'},
          },
        ))
        ..responses.add((200, {'group': 5}));
      final api = HttpBrokerApi(
        origin: Uri.parse('https://cloud.example.com'),
        client: client,
      );
      expect(await api.setDesktopGroup('qa-token', 'pve-1', 'g1'), 'g1');
      expect(await api.setDesktopGroup('qa-token', 'pve-1', null), isNull);
      expect(client.requests[0].method, 'PUT');
      expect(client.requests[0].uri.path, '/api/v1/desktops/pve-1/group');
      expect(jsonDecode(client.requests[0].body.toString()), {'group': 'g1'});
      expect(jsonDecode(client.requests[1].body.toString()), {'group': null});
      await expectLater(
        api.setDesktopGroup('qa-token', 'pve-1', 'g1'),
        throwsA(isA<BrokerException>().having((e) => e.status, 'status', 404)),
      );
      await expectLater(
        api.setDesktopGroup('qa-token', 'pve-1', 'g1'),
        throwsA(isA<BrokerException>()),
      );
      await expectLater(
        api.setDesktopGroup('qa-token', '../x', 'g1'),
        throwsA(isA<BrokerException>()),
      );
      expect(client.requests.length, 4);
    });
  });

  group('sections', () {
    testWidgets('one per group in the account\'s order, then 기타', (t) async {
      await _show(t);
      expect(_sections(t), ['group:g1', 'group:g2', 'other']);
      expect(_order(t), ['pve-2', 'pve-4', 'pve-1', 'pve-3']);
      expect(find.text('개발'), findsOneWidget);
      expect(find.text('기타'), findsOneWidget);
      expect(_count(t, 'group:g1'), '2');
      expect(_count(t, 'group:g2'), '1');
      expect(_count(t, 'other'), '1');
      // Each header sits above its own cards.
      final header = t.getRect(find.byKey(const Key('section-group:g2')));
      expect(t.getRect(find.byKey(const Key('tile-pve-1'))).top,
          greaterThan(header.bottom));
      expect(t.getRect(find.byKey(const Key('tile-pve-4'))).bottom,
          lessThan(header.top));
      // 기타 has nothing to rename or delete.
      expect(find.byKey(const Key('section-more-other')), findsNothing);
      expect(find.byKey(const Key('section-more-group:g1')), findsOneWidget);
      expect(t.takeException(), isNull);
    });

    testWidgets('기타 only while it has a card; an empty group is shown', (
      t,
    ) async {
      await _show(
        t,
        machines: [_vm('pve-1', 'Alpha', group: 'g1')],
      );
      expect(_sections(t), ['group:g1', 'group:g2']);
      expect(_count(t, 'group:g2'), '0');
      expect(find.text('기타'), findsNothing);
    });

    testWidgets('search, filters and sort apply inside each section; a '
        'section without a match hides while filtering', (t) async {
      await _show(t);
      await _type(t, 'delta');
      expect(_sections(t), ['group:g1']);
      expect(_order(t), ['pve-4']);
      expect(_count(t, 'group:g1'), '1/2');
      await t.tap(find.byKey(const Key('desktop-search-clear')));
      await t.pumpAndSettle();
      expect(_sections(t), ['group:g1', 'group:g2', 'other']);
      await _tap(t, 'status-stopped');
      expect(_sections(t), ['group:g1']);
      expect(_order(t), ['pve-4']);
      await _tap(t, 'status-all');
      // The sort applies within a section: 실행 중 먼저 puts Bravo first, by
      // name Bravo before Delta either way; 이름순 across sections keeps
      // each card in its own section.
      await _tap(t, 'desktop-sort');
      await _tap(t, 'sort-name');
      expect(_order(t), ['pve-2', 'pve-4', 'pve-1', 'pve-3']);
      expect(t.takeException(), isNull);
    });

    testWidgets('a header is a button that folds its section, remembered', (
      t,
    ) async {
      final memory = SessionLauncherMemory();
      final handle = t.ensureSemantics();
      await _show(t, memory: memory);
      final header = find.byKey(const Key('section-group:g1'));
      expect(
        t.getSemantics(header),
        isSemantics(
          label: '개발 · 데스크톱 2',
          isButton: true,
          hasTapAction: true,
          hasExpandedState: true,
          isExpanded: true,
        ),
      );
      await t.tap(header);
      await t.pumpAndSettle();
      expect(_order(t), ['pve-1', 'pve-3']);
      expect(_sections(t), ['group:g1', 'group:g2', 'other']);
      expect(
        t.getSemantics(header),
        isSemantics(
          label: '개발 · 데스크톱 2',
          isButton: true,
          hasTapAction: true,
          hasExpandedState: true,
          isExpanded: false,
        ),
      );
      expect(
        (await memory.load(_origin)).organizeFor('tester').folded,
        ['group:g1'],
      );
      // A new launcher with the same memory keeps it folded.
      await t.pumpWidget(const SizedBox());
      await _show(t, memory: memory);
      expect(_order(t), ['pve-1', 'pve-3']);
      await t.tap(header);
      await t.pumpAndSettle();
      expect(_order(t), ['pve-2', 'pve-4', 'pve-1', 'pve-3']);
      expect((await memory.load(_origin)).organizeFor('tester').folded, isEmpty);
      handle.dispose();
    });
  });

  group('card menu', () {
    testWidgets('그룹 lists the groups with the current one checked, moves '
        'the card and takes it out', (t) async {
      final handle = t.ensureSemantics();
      final host = await _show(t);
      await _openGroups(t, 'pve-1');
      final current = t.widget<MenuItemButton>(
        find.byKey(const Key('group-g2-pve-1')),
      );
      expect(current.trailingIcon, isA<Icon>());
      // A screen reader hears which one is current, not only the check.
      expect(current.semanticsLabel, '영업, 지금 그룹');
      expect(
        t.getSemantics(find.byKey(const Key('group-g2-pve-1'))),
        isSemantics(label: '영업, 지금 그룹'),
      );
      handle.dispose();
      expect(
        t.widget<MenuItemButton>(find.byKey(const Key('group-g1-pve-1')))
            .trailingIcon,
        isNull,
      );
      expect(find.byKey(const Key('group-none-pve-1')), findsOneWidget);
      expect(find.byKey(const Key('group-new-pve-1')), findsOneWidget);
      await _tap(t, 'group-g1-pve-1');
      expect(host.sets, [('pve-1', 'g1')]);
      expect(_order(t), ['pve-1', 'pve-2', 'pve-4', 'pve-3']);
      expect(_count(t, 'group:g1'), '3');
      // 영업 is empty now but stays (it is the account's group).
      expect(_count(t, 'group:g2'), '0');
      await _openGroups(t, 'pve-1');
      await _tap(t, 'group-none-pve-1');
      expect(host.sets.last, ('pve-1', null));
      expect(_count(t, 'other'), '2');
      // A desktop in none has no 그룹에서 빼기.
      await _openGroups(t, 'pve-3');
      expect(find.byKey(const Key('group-none-pve-3')), findsNothing);
      expect(t.takeException(), isNull);
    });

    testWidgets('a refused move says why and the card stays where it was', (
      t,
    ) async {
      final host = await _show(t)
        ..failSet = const BrokerException('unknown_group', 400);
      await _openGroups(t, 'pve-3');
      await _tap(t, 'group-g1-pve-3');
      expect(host.sets, [('pve-3', 'g1')]);
      expect(find.text('그 그룹은 이제 없어요.'), findsOneWidget);
      expect(_count(t, 'other'), '1');
      expect(_order(t).last, 'pve-3');
    });

    testWidgets('새 그룹… asks for a name, saves the list with it last and '
        'puts the card in it', (t) async {
      final host = await _show(t);
      await _openGroups(t, 'pve-3');
      await _tap(t, 'group-new-pve-3');
      expect(find.byKey(const Key('group-name-dialog')), findsOneWidget);
      expect(find.text('새 그룹'), findsOneWidget);
      // Empty and taken names are said in the dialog; nothing is sent.
      await _tap(t, 'group-name-save');
      expect(find.text('그룹 이름을 입력하세요.'), findsOneWidget);
      await t.enterText(find.byKey(const Key('group-name-input')), ' 개발 ');
      await _tap(t, 'group-name-save');
      expect(find.text('같은 이름의 그룹이 있어요.'), findsOneWidget);
      expect(host.saved, isEmpty);
      await t.enterText(find.byKey(const Key('group-name-input')), '테스트');
      // Enter in the field is the main button.
      await t.testTextInput.receiveAction(TextInputAction.done);
      await t.pumpAndSettle();
      expect(find.byKey(const Key('group-name-dialog')), findsNothing);
      expect(host.saved.single, [
        (id: 'g1', name: '개발'),
        (id: 'g2', name: '영업'),
        (id: null, name: '테스트'),
      ]);
      expect(host.sets, [('pve-3', 'g10')]);
      expect(_sections(t), ['group:g1', 'group:g2', 'group:g10']);
      expect(find.text('테스트'), findsOneWidget);
      expect(_count(t, 'group:g10'), '1');
      expect(t.takeException(), isNull);
    });

    testWidgets('a name the broker refuses keeps the dialog open with why', (
      t,
    ) async {
      final host = await _show(t)
        ..failSave = const BrokerException('too_many_groups', 400);
      await _openGroups(t, 'pve-3');
      await _tap(t, 'group-new-pve-3');
      await t.enterText(find.byKey(const Key('group-name-input')), '새것');
      await _tap(t, 'group-name-save');
      expect(find.byKey(const Key('group-name-dialog')), findsOneWidget);
      expect(find.text('그룹은 30개까지 만들 수 있어요.'), findsOneWidget);
      expect(host.sets, isEmpty);
      await _tap(t, 'group-name-cancel');
      expect(find.byKey(const Key('group-name-dialog')), findsNothing);
    });

    testWidgets('30 groups: 새 그룹… is greyed', (t) async {
      await _show(
        t,
        groups: [
          for (var i = 0; i < 30; i++) DesktopGroup(id: 'g$i', name: 'G$i'),
        ],
      );
      await _openGroups(t, 'pve-3');
      expect(
        t.widget<MenuItemButton>(find.byKey(const Key('group-new-pve-3')))
            .onPressed,
        isNull,
      );
    });
  });

  group('header menu', () {
    testWidgets('이름 바꾸기, 위로·아래로 옮기기 and 그룹 삭제 save the whole '
        'list', (t) async {
      final host = await _show(t);
      // The first cannot move up, the last not down.
      await _tap(t, 'section-more-group:g1');
      expect(
        t.widget<MenuItemButton>(find.byKey(const Key('section-up-group:g1')))
            .onPressed,
        isNull,
      );
      await _tap(t, 'section-rename-group:g1');
      expect(
        t.widget<TextField>(find.byKey(const Key('group-name-input'))).controller!.text,
        '개발',
      );
      await t.enterText(find.byKey(const Key('group-name-input')), '개발팀');
      await _tap(t, 'group-name-save');
      expect(host.saved.last, [
        (id: 'g1', name: '개발팀'),
        (id: 'g2', name: '영업'),
      ]);
      expect(find.text('개발팀'), findsOneWidget);

      await _tap(t, 'section-more-group:g2');
      expect(
        t.widget<MenuItemButton>(find.byKey(const Key('section-down-group:g2')))
            .onPressed,
        isNull,
      );
      await _tap(t, 'section-up-group:g2');
      expect(host.saved.last, [
        (id: 'g2', name: '영업'),
        (id: 'g1', name: '개발팀'),
      ]);
      expect(_sections(t), ['group:g2', 'group:g1', 'other']);
      expect(_order(t), ['pve-1', 'pve-2', 'pve-4', 'pve-3']);

      await _tap(t, 'section-more-group:g2');
      await _tap(t, 'section-down-group:g2');
      expect(_sections(t), ['group:g1', 'group:g2', 'other']);

      // 그룹 삭제 asks first and says the cards go to 기타.
      await _tap(t, 'section-more-group:g1');
      await _tap(t, 'section-delete-group:g1');
      expect(find.text("'개발팀' 그룹을 삭제할까요?"), findsOneWidget);
      expect(
        find.text("이 그룹의 카드는 '기타'로 옮겨져요. 데스크톱은 그대로예요."),
        findsOneWidget,
      );
      final before = host.saved.length;
      await _tap(t, 'cancel-group-delete');
      expect(host.saved.length, before);
      await _tap(t, 'section-more-group:g1');
      await _tap(t, 'section-delete-group:g1');
      await _tap(t, 'confirm-group-delete');
      expect(host.saved.last, [(id: 'g2', name: '영업')]);
      expect(_sections(t), ['group:g2', 'other']);
      expect(_count(t, 'other'), '3');
      // Nothing else is gone: every desktop is still listed.
      expect(_order(t).toSet(), {'pve-1', 'pve-2', 'pve-3', 'pve-4'});
      expect(t.takeException(), isNull);
    });

    testWidgets('a move that is not saved says so', (t) async {
      final host = await _show(t)
        ..failSave = const BrokerException('network_error');
      await _tap(t, 'section-more-group:g2');
      await _tap(t, 'section-up-group:g2');
      expect(host.saved, hasLength(1));
      expect(
        find.text('그룹을 바꾸지 못했어요. 잠시 후 다시 시도하세요.'),
        findsOneWidget,
      );
      expect(_sections(t), ['group:g1', 'group:g2', 'other']);
    });
  });

  group('an older broker', () {
    testWidgets('without groups the list and menus are what they were', (
      t,
    ) async {
      await _show(t, supported: false);
      expect(_sections(t), isEmpty);
      expect(find.text('기타'), findsNothing);
      expect(_order(t), ['pve-1', 'pve-2', 'pve-3', 'pve-4']);
      await _tap(t, 'desktop-more-pve-1');
      expect(find.byKey(const Key('group-menu-pve-1')), findsNothing);
      expect(find.byKey(const Key('favorite-pve-1')), findsOneWidget);
    });

    testWidgets('a broker with groups but none made yet: the plain list and '
        '그룹 → 새 그룹… only', (t) async {
      await _show(t, groups: const []);
      expect(_sections(t), isEmpty);
      expect(_order(t), ['pve-1', 'pve-2', 'pve-3', 'pve-4']);
      await _openGroups(t, 'pve-1');
      expect(find.byKey(const Key('group-new-pve-1')), findsOneWidget);
      expect(find.byKey(const Key('group-none-pve-1')), findsNothing);
    });
  });

  group('card size', () {
    testWidgets('작게 draws the same cards smaller, more to a row, and is '
        'remembered', (t) async {
      final memory = SessionLauncherMemory();
      await _show(t, supported: false, memory: memory);
      final normal = t.getRect(find.byKey(const Key('card-pve-1')));
      final normalRow = _order(t)
          .where(
            (id) =>
                (t.getRect(find.byKey(Key('card-$id'))).top - normal.top)
                    .abs() <
                2,
          )
          .length;
      final parts = [
        for (final key in ['card', 'tile', 'desktop-more', 'resources'])
          '$key-pve-1',
      ];
      for (final key in parts) {
        expect(find.byKey(Key(key)), findsOneWidget, reason: key);
      }
      final size = find.byKey(const Key('desktop-card-size'));
      expect(size, findsOneWidget);
      expect(find.text('작게'), findsOneWidget);
      await t.tap(size);
      await t.pumpAndSettle();
      final small = t.getRect(find.byKey(const Key('card-pve-1')));
      expect(small.width, lessThan(normal.width * .85));
      // The same proportions: the card is the normal one, scaled.
      expect(
        small.width / small.height,
        closeTo(
          normal.width / normal.height,
          normal.width / normal.height * .25,
        ),
      );
      final smallRow = _order(t)
          .where(
            (id) =>
                (t.getRect(find.byKey(Key('card-$id'))).top - small.top)
                    .abs() <
                2,
          )
          .length;
      expect(smallRow, greaterThan(normalRow));
      for (final key in parts) {
        expect(find.byKey(Key(key)), findsOneWidget, reason: key);
      }
      expect(find.byType(DesktopTileScale), findsNWidgets(4));
      expect((await memory.load(_origin)).smallCards, isTrue);
      // A new launcher with the same memory is small at once.
      await t.pumpWidget(const SizedBox());
      await _show(t, supported: false, memory: memory);
      expect(
        t.getRect(find.byKey(const Key('card-pve-1'))).width,
        closeTo(small.width, 1),
      );
      // A card's buttons still work drawn small: ⋯ opens its menu.
      await _tap(t, 'desktop-more-pve-1');
      expect(find.byKey(const Key('details-pve-1')), findsOneWidget);
      await t.tapAt(const Offset(4, 4));
      await t.pumpAndSettle();
      await t.tap(size);
      await t.pumpAndSettle();
      expect(
        t.getRect(find.byKey(const Key('card-pve-1'))).width,
        closeTo(normal.width, 1),
      );
      expect((await memory.load(_origin)).smallCards, isFalse);
      expect(t.takeException(), isNull);
    });

    test('the small grid is the normal rule on a wider page, drawn smaller', () {
      const viewport = Size(1440, 900);
      final normal = desktopTileGrid(
        1240,
        TextScaler.noScaling,
        viewport: viewport,
      );
      final small = desktopTileGrid(
        1240,
        TextScaler.noScaling,
        viewport: viewport,
        small: true,
      );
      expect(normal.columns, 3);
      expect(normal.scale, 1);
      expect(normal.extent, normal.height);
      expect(small.columns, greaterThan(normal.columns));
      expect(small.scale, desktopSmallTileScale);
      expect(small.extent, closeTo(small.height * small.scale, .001));
      expect(small.gap, desktopTileGap * desktopSmallTileScale);
      // On screen the columns and gaps fill the same width.
      expect(
        small.width * small.scale * small.columns +
            small.gap * (small.columns - 1),
        closeTo(1240, .5),
      );
    });
  });

  group('the app', () {
    testWidgets('the launcher sends group changes to the broker and shows '
        'its answer', (t) async {
      t.view.physicalSize = const Size(1440, 2000);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      final broker = _GroupBroker();
      final memory = TestLoginMemory()
        ..entries[fixtureOrigin.origin] = const RememberedLogin(
          username: 'alice',
          password: 'synthetic-password',
        );
      await t.pumpWidget(
        MultitapApp(
          api: broker,
          serverOrigin: fixtureOrigin,
          loginMemory: memory,
          desktopWatchFactory: (_, _) => _Watch(),
        ),
      );
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      expect(broker.reads, greaterThan(0));
      expect(_sections(t), ['group:g1', 'other']);
      expect(_order(t), ['pve-2', 'pve-1']);

      await _openGroups(t, 'pve-1');
      await _tap(t, 'group-new-pve-1');
      await t.enterText(find.byKey(const Key('group-name-input')), '영업');
      await _tap(t, 'group-name-save');
      expect(broker.saved.single, [
        (id: 'g1', name: '개발'),
        (id: null, name: '영업'),
      ]);
      expect(broker.sets, [('pve-1', 'g2')]);
      expect(_sections(t), ['group:g1', 'group:g2']);
      expect(_order(t), ['pve-2', 'pve-1']);

      await _tap(t, 'section-more-group:g1');
      await _tap(t, 'section-delete-group:g1');
      await _tap(t, 'confirm-group-delete');
      expect(broker.saved.last, [(id: 'g2', name: '영업')]);
      expect(_sections(t), ['group:g2', 'other']);
      expect(_order(t), ['pve-1', 'pve-2']);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox());
      await t.pumpAndSettle();
    });

    testWidgets('an older broker shows the plain list', (t) async {
      t.view.physicalSize = const Size(1440, 2000);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      final broker = TestBroker()..assigned = _GroupBroker().inventory();
      final memory = TestLoginMemory()
        ..entries[fixtureOrigin.origin] = const RememberedLogin(
          username: 'alice',
          password: 'synthetic-password',
        );
      await t.pumpWidget(
        MultitapApp(
          api: broker,
          serverOrigin: fixtureOrigin,
          loginMemory: memory,
          desktopWatchFactory: (_, _) => _Watch(),
        ),
      );
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      expect(_sections(t), isEmpty);
      expect(_order(t), ['pve-1', 'pve-2']);
      await _tap(t, 'desktop-more-pve-1');
      expect(find.byKey(const Key('group-menu-pve-1')), findsNothing);
      await t.pumpWidget(const SizedBox());
      await t.pumpAndSettle();
    });
  });
}

class _Watch extends ChangeNotifier implements DesktopUpdates {
  @override
  bool get connected => true;
  @override
  Stream<void> get changes => const Stream<void>.empty();
  @override
  void start() {}
  @override
  Future<void> close() async {}
}

/// A broker with groups that answers as the server does.
class _GroupBroker extends TestBroker implements DesktopGroupBrokerApi {
  var groups = [const DesktopGroup(id: 'g1', name: '개발')];
  final placed = <String, String?>{'pve-1': null, 'pve-2': 'g1'};
  final saved = <List<DesktopGroupDraft>>[];
  final sets = <(String, String?)>[];
  int reads = 0;
  var _next = 2;

  List<BrokerDesktop> inventory() => [
    for (final (id, name) in [('pve-1', 'Alpha'), ('pve-2', 'Bravo')])
      BrokerDesktop.fromJson({
        'id': id,
        'name': name,
        'kind': 'vm',
        'description': '',
        'os': 'Windows 11',
        'status': 'online',
        'canConnect': true,
        'favorite': false,
        'group': placed[id],
      }),
  ];

  @override
  Future<List<BrokerDesktop>> desktops(String token) async =>
      (await desktopList(token)).desktops;

  @override
  Future<BrokerDesktopList> desktopList(String token) async {
    reads++;
    return BrokerDesktopList(inventory(), groups: groups);
  }

  @override
  Future<List<DesktopGroup>> saveDesktopGroups(
    String token,
    List<DesktopGroupDraft> drafts,
  ) async {
    saved.add(drafts);
    groups = [
      for (final draft in drafts)
        DesktopGroup(id: draft.id ?? 'g${_next++}', name: draft.name),
    ];
    final ids = {for (final group in groups) group.id};
    placed.updateAll((_, group) => ids.contains(group) ? group : null);
    return groups;
  }

  @override
  Future<String?> setDesktopGroup(
    String token,
    String desktopId,
    String? group,
  ) async {
    sets.add((desktopId, group));
    placed[desktopId] = group;
    return group;
  }
}

class _Headers extends Fake implements HttpHeaders {
  final values = <String, Object>{};
  @override
  ContentType? contentType;
  @override
  void set(String name, Object value, {bool preserveHeaderCase = false}) {
    values[name] = value;
  }
}

class _Response extends Fake implements HttpClientResponse {
  _Response(this.statusCode, this.value);
  @override
  final int statusCode;
  final Map<String, dynamic> value;
  @override
  HttpHeaders get headers => _Headers();
  @override
  Stream<List<int>> timeout(
    Duration timeLimit, {
    void Function(EventSink<List<int>>)? onTimeout,
  }) => Stream<List<int>>.value(
    utf8.encode(jsonEncode(value)),
  ).timeout(timeLimit, onTimeout: onTimeout);
  @override
  StreamSubscription<List<int>> listen(
    void Function(List<int>)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => Stream<List<int>>.value(utf8.encode(jsonEncode(value))).listen(
    onData,
    onError: onError,
    onDone: onDone,
    cancelOnError: cancelOnError,
  );
}

class _Request extends Fake implements HttpClientRequest {
  _Request(this.method, this.uri, this.response);
  @override
  final String method;
  @override
  final Uri uri;
  final _Response response;
  @override
  final _Headers headers = _Headers();
  @override
  bool followRedirects = true;
  final body = StringBuffer();
  @override
  void write(Object? value) => body.write(value);
  @override
  Future<HttpClientResponse> close() async => response;
}

class _ApiClient extends Fake implements HttpClient {
  final responses = <(int, Map<String, dynamic>)>[];
  final requests = <_Request>[];
  @override
  Duration? connectionTimeout;
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    final response = responses.removeAt(0);
    final request = _Request(method, uri, _Response(response.$1, response.$2));
    requests.add(request);
    return request;
  }

  @override
  void close({bool force = false}) {}
}

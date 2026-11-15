// Desktop groups and the 보통 / 작게 card size on the Android launcher. The
// broker and the device preferences are synthetic.

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/desktop_organize.dart';
import 'package:multitap_client/ui/desktop_tile.dart' show DesktopTileScale;

import 'android_launcher_test.dart' show FakeBroker, MemoryStore;
import '../helpers/pump_work.dart';

BrokerDesktop _vm(String id, String name, {String? group}) => BrokerDesktop(
  id: id,
  name: name,
  kind: 'vm',
  description: '',
  os: 'Windows 11 · 4 vCPU',
  status: 'online',
  canConnect: true,
  canStream: true,
  hostConnected: true,
  power: BrokerPowerSnapshot(
    state: 'running',
    stale: false,
    checkedAt: DateTime.utc(2026, 11, 15),
  ),
  organize: DesktopOrganize(supported: true, group: group),
);

/// A broker with desktop groups that answers as the server does.
class _GroupBroker extends FakeBroker implements DesktopGroupBrokerApi {
  _GroupBroker()
    : super([
        _vm('pve-1', 'Alpha', group: 'g2'),
        _vm('pve-2', 'Bravo', group: 'g1'),
        _vm('pve-3', 'Charlie'),
        _vm('pve-4', 'Delta', group: 'g1'),
      ]);
  var groups = [
    const DesktopGroup(id: 'g1', name: '개발'),
    const DesktopGroup(id: 'g2', name: '영업'),
  ];
  final saved = <List<DesktopGroupDraft>>[];
  final sets = <(String, String?)>[];
  var _next = 3;

  @override
  Future<BrokerDesktopList> desktopList(String token) async {
    calls.add('desktops');
    return BrokerDesktopList(inventory, groups: groups);
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
    inventory = [
      for (final desktop in inventory)
        ids.contains(desktop.organize.group)
            ? desktop
            : _vm(desktop.id, desktop.name),
    ];
    return groups;
  }

  @override
  Future<String?> setDesktopGroup(
    String token,
    String desktopId,
    String? group,
  ) async {
    sets.add((desktopId, group));
    inventory = [
      for (final desktop in inventory)
        desktop.id == desktopId
            ? _vm(desktop.id, desktop.name, group: group)
            : desktop,
    ];
    return group;
  }
}

Future<MemoryStore> _signIn(
  WidgetTester tester,
  FakeBroker broker, {
  MemoryStore? store,
}) async {
  tester.view.physicalSize = const Size(1080, 9000);
  tester.view.devicePixelRatio = 3;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  final fixture =
      store ?? MemoryStore(origin: 'https://vdi.example', username: 'tester');
  fixture.install();
  await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
  await pumpWork(tester);
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'secret-pass',
  );
  await _tap(tester, 'login-submit');
  return fixture;
}

Future<void> _tap(WidgetTester tester, String key) async {
  final target = find.byKey(Key(key));
  await tester.ensureVisible(target);
  await pumpWork(tester);
  await tester.tap(target);
  await pumpWork(tester);
}

List<String> _order(WidgetTester t) {
  final found = <(String, Offset)>[];
  for (final element
      in find
          .byWidgetPredicate(
            (w) =>
                w.key is ValueKey<String> &&
                (w.key as ValueKey<String>).value.startsWith('slot-'),
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

List<String> _sections(WidgetTester t) {
  final found = <(String, double)>[];
  for (final element
      in find
          .byWidgetPredicate(
            (w) =>
                w.key is ValueKey<String> &&
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

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  testWidgets('sections in the account\'s order; a header folds its section, '
      'remembered for this account', (tester) async {
    final handle = tester.ensureSemantics();
    final store = await _signIn(tester, _GroupBroker());
    expect(_sections(tester), ['group:g1', 'group:g2', 'other']);
    expect(_order(tester), ['pve-2', 'pve-4', 'pve-1', 'pve-3']);
    expect(_count(tester, 'group:g1'), '2');
    expect(find.text('기타'), findsOneWidget);
    final header = find.byKey(const Key('section-group:g1'));
    expect(tester.getSize(header).height, greaterThanOrEqualTo(48));
    expect(
      tester.getSemantics(header),
      isSemantics(
        label: '개발 · 데스크톱 2',
        isButton: true,
        hasExpandedState: true,
        isExpanded: true,
      ),
    );
    await _tap(tester, 'section-group:g1');
    expect(_order(tester), ['pve-1', 'pve-3']);
    expect(
      tester.getSemantics(header),
      isSemantics(hasExpandedState: true, isExpanded: false),
    );
    expect(
      store.prefs['organize.https://vdi.example|tester'],
      contains('"folded":["group:g1"]'),
    );
    // A new launch keeps it folded; another account does not.
    await tester.pumpWidget(const SizedBox());
    await _signIn(tester, _GroupBroker(), store: store);
    expect(_order(tester), ['pve-1', 'pve-3']);
    await tester.pumpWidget(const SizedBox());
    store.username = 'other';
    await _signIn(tester, _GroupBroker(), store: store);
    expect(_order(tester), ['pve-2', 'pve-4', 'pve-1', 'pve-3']);
    handle.dispose();
  });

  testWidgets('the search hides a section without a match', (tester) async {
    await _signIn(tester, _GroupBroker());
    await _tap(tester, 'desktop-search-open');
    await tester.enterText(
      find.byKey(const Key('desktop-search-input')),
      'delta',
    );
    await pumpWork(tester);
    expect(_sections(tester), ['group:g1']);
    expect(_count(tester, 'group:g1'), '1/2');
    expect(_order(tester), ['pve-4']);
  });

  testWidgets('the card ⋯ 그룹 moves the card; 새 그룹… makes one and puts the '
      'card in it', (tester) async {
    final broker = _GroupBroker();
    await _signIn(tester, broker);
    await _tap(tester, 'desktop-more-pve-3');
    await _tap(tester, 'group-menu-pve-3');
    expect(find.byKey(const Key('group-none-pve-3')), findsNothing);
    await _tap(tester, 'group-g2-pve-3');
    expect(broker.sets, [('pve-3', 'g2')]);
    expect(_sections(tester), ['group:g1', 'group:g2']);
    expect(_count(tester, 'group:g2'), '2');

    await _tap(tester, 'desktop-more-pve-3');
    await _tap(tester, 'group-menu-pve-3');
    await _tap(tester, 'group-new-pve-3');
    expect(find.byKey(const Key('group-name-dialog')), findsOneWidget);
    await tester.enterText(find.byKey(const Key('group-name-input')), '현장');
    await _tap(tester, 'group-name-save');
    expect(broker.saved.single, [
      (id: 'g1', name: '개발'),
      (id: 'g2', name: '영업'),
      (id: null, name: '현장'),
    ]);
    expect(broker.sets.last, ('pve-3', 'g3'));
    expect(_sections(tester), ['group:g1', 'group:g2', 'group:g3']);
    expect(_order(tester), ['pve-2', 'pve-4', 'pve-1', 'pve-3']);

    await _tap(tester, 'desktop-more-pve-3');
    await _tap(tester, 'group-menu-pve-3');
    await _tap(tester, 'group-none-pve-3');
    expect(broker.sets.last, ('pve-3', null));
    expect(_sections(tester), ['group:g1', 'group:g2', 'group:g3', 'other']);
    expect(tester.takeException(), isNull);
  });

  testWidgets('the header ⋯ renames, moves and deletes a group', (
    tester,
  ) async {
    final broker = _GroupBroker();
    await _signIn(tester, broker);
    await _tap(tester, 'section-more-group:g1');
    expect(
      tester.getSize(find.byKey(const Key('section-rename-group:g1'))).height,
      greaterThanOrEqualTo(48),
    );
    await _tap(tester, 'section-rename-group:g1');
    await tester.enterText(find.byKey(const Key('group-name-input')), '개발팀');
    await _tap(tester, 'group-name-save');
    expect(broker.saved.last, [
      (id: 'g1', name: '개발팀'),
      (id: 'g2', name: '영업'),
    ]);
    expect(find.text('개발팀'), findsOneWidget);

    await _tap(tester, 'section-more-group:g1');
    await _tap(tester, 'section-down-group:g1');
    expect(broker.saved.last, [
      (id: 'g2', name: '영업'),
      (id: 'g1', name: '개발팀'),
    ]);
    expect(_sections(tester), ['group:g2', 'group:g1', 'other']);

    await _tap(tester, 'section-more-group:g1');
    await _tap(tester, 'section-delete-group:g1');
    expect(find.text("'개발팀' 그룹을 삭제할까요?"), findsOneWidget);
    await _tap(tester, 'confirm-group-delete');
    expect(broker.saved.last, [(id: 'g2', name: '영업')]);
    expect(_sections(tester), ['group:g2', 'other']);
    expect(_count(tester, 'other'), '3');
    expect(tester.takeException(), isNull);
  });

  testWidgets('an older broker: no sections and no 그룹 row', (tester) async {
    await _signIn(
      tester,
      FakeBroker([
        _vm('pve-1', 'Alpha', group: 'g2'),
        _vm('pve-2', 'Bravo'),
      ]),
    );
    expect(_sections(tester), isEmpty);
    expect(find.text('기타'), findsNothing);
    expect(_order(tester), ['pve-1', 'pve-2']);
    await _tap(tester, 'desktop-more-pve-1');
    expect(find.byKey(const Key('group-menu-pve-1')), findsNothing);
    expect(find.byKey(const Key('details-pve-1')), findsOneWidget);
  });

  testWidgets('작게 before 새로고침 draws the same cards smaller and is '
      'remembered on this device', (tester) async {
    final store = await _signIn(tester, _GroupBroker());
    final button = find.byKey(const Key('desktop-card-size'));
    expect(button, findsOneWidget);
    final refresh = tester.getRect(find.byKey(const Key('refresh-desktops')));
    final previews = tester.getRect(find.byKey(const Key('desktop-previews')));
    final rect = tester.getRect(button);
    // In the title row before 새로고침, as tall as 미리보기 (a 48 dp target).
    expect(rect.right, lessThan(refresh.left));
    expect(rect.center.dy, closeTo(refresh.center.dy, .5));
    expect(rect.height, closeTo(previews.height, .5));
    expect(rect.height, greaterThanOrEqualTo(48));
    final normal = tester.getRect(find.byKey(const Key('card-pve-2')));
    await _tap(tester, 'desktop-card-size');
    expect(store.prefs['launcher.cards'], 'small');
    final small = tester.getRect(find.byKey(const Key('card-pve-2')));
    expect(small.width, lessThan(normal.width * .85));
    expect(
      small.width / small.height,
      closeTo(normal.width / normal.height, normal.width / normal.height * .25),
    );
    expect(find.byType(DesktopTileScale), findsNWidgets(4));
    for (final key in ['tile-pve-2', 'desktop-more-pve-2', 'resources-pve-2']) {
      expect(find.byKey(Key(key)), findsOneWidget, reason: key);
    }
    await tester.pumpWidget(const SizedBox());
    await _signIn(tester, _GroupBroker(), store: store);
    expect(
      tester.getRect(find.byKey(const Key('card-pve-2'))).width,
      closeTo(small.width, 1),
    );
    await _tap(tester, 'desktop-card-size');
    expect(store.prefs['launcher.cards'], 'normal');
    expect(
      tester.getRect(find.byKey(const Key('card-pve-2'))).width,
      closeTo(normal.width, 1),
    );
    expect(tester.takeException(), isNull);
  });
}

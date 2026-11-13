import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/desktop_organize.dart';

import 'android_launcher_test.dart' show FakeBroker, MemoryStore;
import '../helpers/pump_work.dart';

BrokerDesktop _vm(
  String id,
  String name, {
  List<String> tags = const [],
  String? pool,
  bool? favorite,
  DateTime? used,
  String power = 'running',
  String kind = 'vm',
}) => BrokerDesktop(
  id: id,
  name: name,
  kind: kind,
  description: '',
  os: 'Windows 11 · 4 vCPU',
  status: power == 'stopped' ? 'powered_off' : 'online',
  canConnect: true,
  canStream: true,
  hostConnected: power == 'running',
  power: BrokerPowerSnapshot(
    state: power,
    stale: false,
    checkedAt: DateTime.utc(2026, 10, 26),
  ),
  powerCanForceStop: power == 'running',
  organize: DesktopOrganize(
    tags: tags,
    pool: pool,
    favorite: favorite ?? false,
    lastUsedAt: used,
    supported: favorite != null,
  ),
);

List<BrokerDesktop> _fleet() => [
  _vm('pve-101', '사무실 PC', tags: ['업무'], pool: '영업팀', favorite: false),
  _vm('pve-149', 'Dev Box', tags: ['dev'], favorite: true),
  _vm('pve-150', 'Lab', tags: ['dev', '업무'], favorite: false, power: 'stopped'),
  _vm('pve-160', '민수 데스크톱', favorite: false),
];

/// The four VMs and one local PC.
List<BrokerDesktop> _mixed() => [
  ..._fleet(),
  _vm('pc-gaming', 'TOWER-PC', favorite: false, kind: 'pc'),
];

class _FavoriteBroker extends FakeBroker implements FavoriteBrokerApi {
  _FavoriteBroker(super.inventory);
  Completer<bool>? answer;
  final sent = <(String, bool)>[];

  @override
  Future<bool> setFavorite(String token, String desktopId, bool favorite) {
    sent.add((desktopId, favorite));
    return answer?.future ?? Future.value(favorite);
  }
}

Future<void> _signIn(
  WidgetTester tester,
  FakeBroker broker, {
  MemoryStore? store,
  double scale = 1,
  Size size = const Size(1080, 9000),
}) async {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 3;
  tester.platformDispatcher.textScaleFactorTestValue = scale;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
  final fixture =
      store ?? MemoryStore(origin: 'https://vdi.example', username: 'tester');
  // These legacy cases exercise the card arrangement; default-view coverage is separate.
  fixture.prefs.putIfAbsent('launcher.view', () => 'cards');
  fixture.install();
  await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
  await pumpWork(tester);
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'secret-pass',
  );
  await _tapVisible(tester, find.byKey(const Key('login-submit')));
  await pumpWork(tester);
}

Finder _card(String id) => find.byKey(Key('card-$id'));

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

final _input = find.byKey(const Key('desktop-search-input'));

/// The chip row scrolls sideways: bring the chip into view, then press it.
Future<void> _chip(WidgetTester tester, String id) async {
  final chip = find.byKey(Key('chip-$id'));
  await tester.ensureVisible(chip);
  await pumpWork(tester);
  await _tapVisible(tester, chip);
  await pumpWork(tester);
}

Future<void> _status(WidgetTester tester, String id) async {
  final segment = find.byKey(Key('status-$id'));
  await tester.ensureVisible(segment);
  await pumpWork(tester);
  await _tapVisible(tester, segment);
  await pumpWork(tester);
}

String _statusCount(WidgetTester tester, String id) =>
    tester.widget<Text>(find.byKey(Key('status-count-$id'))).data!;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  testWidgets('the search icon opens a box; name, ID, tag, pool and 초성', (
    tester,
  ) async {
    await _signIn(tester, FakeBroker(_fleet()));
    expect(_order(tester).length, 4);
    expect(find.byKey(const Key('desktop-search')), findsNothing);
    await _tapVisible(tester, find.byKey(const Key('desktop-search-open')));
    await pumpWork(tester);
    expect(find.byKey(const Key('desktop-search')), findsOneWidget);
    Future<void> type(String text) async {
      await tester.enterText(_input, text);
      await pumpWork(tester);
    }

    await type('dev');
    expect(_order(tester), ['pve-149', 'pve-150']);
    await type('#16');
    expect(_order(tester), ['pve-160']);
    await type('업무');
    expect(_order(tester), ['pve-101', 'pve-150']);
    await type('영업');
    expect(_order(tester), ['pve-101']);
    await type('ㅁㅅㄷㅅㅋㅌ');
    expect(_order(tester), ['pve-160']);
    expect(
      tester.widget<Text>(find.byKey(const Key('desktop-count'))).data,
      '1/4',
    );
    await type('없는이름');
    expect(_order(tester), isEmpty);
    expect(find.text("'없는이름'에 맞는 데스크톱이 없어요"), findsOneWidget);
    await _tapVisible(tester, find.byKey(const Key('no-match-clear')));
    await pumpWork(tester);
    expect(_order(tester).length, 4);
    // Cleared and closed: the magnifier is back.
    expect(find.byKey(const Key('desktop-search-open')), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  testWidgets('system back closes an open search before leaving', (
    tester,
  ) async {
    await _signIn(tester, FakeBroker(_fleet()));
    await _tapVisible(tester, find.byKey(const Key('desktop-search-open')));
    await pumpWork(tester);
    await tester.enterText(_input, 'lab');
    await pumpWork(tester);
    expect(_order(tester), ['pve-150']);
    await tester.binding.handlePopRoute();
    await pumpWork(tester);
    expect(find.byKey(const Key('desktop-search')), findsNothing);
    expect(_order(tester).length, 4);
  });

  testWidgets('chips filter, AND with the search, and sort orders the list', (
    tester,
  ) async {
    await _signIn(tester, FakeBroker(_fleet()));
    await _chip(tester, 'tag:dev');
    expect(_order(tester), ['pve-149', 'pve-150']);
    await _chip(
      tester,
      'tag:dev',
    ); // pressing the chosen chip again releases it
    // The favourite leads in every sort.
    expect(_order(tester).first, 'pve-149');
    await _tapVisible(tester, find.byKey(const Key('desktop-sort')));
    await pumpWork(tester);
    await _tapVisible(tester, find.byKey(const Key('sort-name')));
    await pumpWork(tester);
    expect(_order(tester), ['pve-149', 'pve-150', 'pve-160', 'pve-101']);
    await _tapVisible(tester, find.byKey(const Key('desktop-sort')));
    await pumpWork(tester);
    await _tapVisible(tester, find.byKey(const Key('sort-running')));
    await pumpWork(tester);
    expect(_order(tester), ['pve-149', 'pve-101', 'pve-160', 'pve-150']);
  });

  testWidgets('the chip and sort are remembered for this account', (
    tester,
  ) async {
    final store = MemoryStore(
      origin: 'https://vdi.example',
      username: 'tester',
    );
    await _signIn(tester, FakeBroker(_fleet()), store: store);
    await _chip(tester, 'pool:영업팀');
    await _tapVisible(tester, find.byKey(const Key('desktop-sort')));
    await pumpWork(tester);
    await _tapVisible(tester, find.byKey(const Key('sort-name')));
    await pumpWork(tester);
    final key = 'organize.https://vdi.example|tester';
    expect(store.prefs[key], contains('"chip":"pool:영업팀"'));
    expect(store.prefs[key], contains('"sort":"name"'));
    expect(store.prefs.keys.any((k) => k.contains('desktop-search')), false);

    // A new launch with the same preferences restores both.
    await tester.pumpWidget(const SizedBox());
    await _signIn(tester, FakeBroker(_fleet()), store: store);
    expect(_order(tester), ['pve-101']);
  });

  testWidgets('a remembered chip that is gone falls back to 전체', (
    tester,
  ) async {
    final store = MemoryStore(
      origin: 'https://vdi.example',
      username: 'tester',
      prefs: {
        'organize.https://vdi.example|tester':
            '{"chip":"tag:사라짐","sort":"recent"}',
      },
    );
    await _signIn(tester, FakeBroker(_fleet()), store: store);
    expect(_order(tester).length, 4);
    expect(find.byKey(const Key('no-match-desktops')), findsNothing);
  });

  testWidgets('three plain desktops: only the search icon', (tester) async {
    await _signIn(
      tester,
      FakeBroker([
        _vm('pve-1', 'A', favorite: false),
        _vm('pve-2', 'B', favorite: false),
        _vm('pve-3', 'C', favorite: false),
      ]),
    );
    expect(find.byKey(const Key('desktop-chips')), findsNothing);
    expect(find.byKey(const Key('desktop-sort')), findsNothing);
    expect(find.byKey(const Key('desktop-search-open')), findsOneWidget);
  });

  group('status filter', () {
    testWidgets('always visible with fleet counts, independent of search', (
      tester,
    ) async {
      await _signIn(tester, FakeBroker(_fleet()));
      expect(find.byKey(const Key('desktop-status')), findsOneWidget);
      expect(
        [
          for (final id in ['all', 'running', 'stopped'])
            _statusCount(tester, id),
        ],
        ['4', '3', '1'],
      );
      await _status(tester, 'stopped');
      expect(_order(tester), ['pve-150']);
      expect(
        tester.widget<Text>(find.byKey(const Key('desktop-count'))).data,
        '1/4',
      );
      await _tapVisible(tester, find.byKey(const Key('desktop-search-open')));
      await pumpWork(tester);
      await tester.enterText(_input, 'no match');
      await pumpWork(tester);
      expect(_order(tester), isEmpty);
      expect(find.byKey(const Key('desktop-status')), findsOneWidget);
      expect(
        [
          for (final id in ['all', 'running', 'stopped'])
            _statusCount(tester, id),
        ],
        ['4', '3', '1'],
      );
      expect(tester.takeException(), isNull);
    });

    testWidgets('an empty segment explains why on touch and cannot select', (
      tester,
    ) async {
      await _signIn(tester, FakeBroker([_vm('pve-1', 'A')]));
      final segment = find.byKey(const Key('status-stopped'));
      expect(segment, findsOneWidget);
      expect(_statusCount(tester, 'stopped'), '0');
      expect(
        tester
            .widget<InkWell>(
              find.descendant(of: segment, matching: find.byType(InkWell)),
            )
            .onTap,
        isNull,
      );
      final tooltip = find.byWidgetPredicate(
        (widget) => widget is Tooltip && widget.message == '꺼진 데스크톱이 없어요',
      );
      expect(tooltip, findsOneWidget);
      expect(
        tester.widget<Tooltip>(tooltip).triggerMode,
        TooltipTriggerMode.manual,
      );
      await _status(tester, 'stopped');
      expect(find.text('꺼진 데스크톱이 없어요'), findsOneWidget);
      expect(find.byType(SnackBar), findsOneWidget);
      expect(_order(tester), ['pve-1']);
      expect(tester.widget<Material>(segment).color, Colors.transparent);
      expect(tester.takeException(), isNull);
    });

    testWidgets('status, tag and search AND; reset keeps the search', (
      tester,
    ) async {
      await _signIn(tester, FakeBroker(_fleet()));
      await _status(tester, 'running');
      await _chip(tester, 'tag:dev');
      expect(_order(tester), ['pve-149']);
      await _tapVisible(tester, find.byKey(const Key('desktop-search-open')));
      await pumpWork(tester);
      await tester.enterText(_input, 'lab');
      await pumpWork(tester);
      expect(_order(tester), isEmpty);
      final reset = find.byKey(const Key('desktop-filter-reset'));
      await tester.ensureVisible(reset);
      await pumpWork(tester);
      await _tapVisible(tester, reset);
      await pumpWork(tester);
      expect(_order(tester), ['pve-150']);
      expect(tester.widget<TextField>(_input).controller!.text, 'lab');
      expect(reset, findsNothing);
    });

    testWidgets('status restores for its server and account; search does not', (
      tester,
    ) async {
      final store = MemoryStore(
        origin: 'https://vdi.example',
        username: 'tester',
      );
      await _signIn(tester, FakeBroker(_fleet()), store: store);
      await _status(tester, 'stopped');
      expect(
        store.prefs['organize.https://vdi.example|tester'],
        contains('"status":"stopped"'),
      );
      await _tapVisible(tester, find.byKey(const Key('desktop-search-open')));
      await pumpWork(tester);
      await tester.enterText(_input, 'lab');
      await pumpWork(tester);

      await tester.pumpWidget(const SizedBox());
      await _signIn(tester, FakeBroker(_fleet()), store: store);
      expect(_order(tester), ['pve-150']);
      expect(find.byKey(const Key('desktop-search')), findsNothing);
      await tester.pumpWidget(const SizedBox());
      store.username = 'other';
      await _signIn(tester, FakeBroker(_fleet()), store: store);
      expect(_order(tester).length, 4);
      expect(find.byKey(const Key('desktop-filter-reset')), findsNothing);
      await tester.pumpWidget(const SizedBox());
      store.origin = 'https://another.example';
      store.username = 'tester';
      await _signIn(tester, FakeBroker(_fleet()), store: store);
      expect(_order(tester).length, 4);
      expect(find.byKey(const Key('desktop-filter-reset')), findsNothing);
    });

    testWidgets('a status losing its last member returns to all and saves it', (
      tester,
    ) async {
      final store = MemoryStore(
        origin: 'https://vdi.example',
        username: 'tester',
      );
      final broker = FakeBroker(_fleet());
      await _signIn(tester, broker, store: store);
      await _status(tester, 'stopped');
      broker.inventory = [_vm('pve-101', '사무실 PC'), _vm('pve-150', 'Lab')];
      await _tapVisible(tester, find.byKey(const Key('refresh-desktops')));
      await pumpWork(tester);
      expect(_order(tester), ['pve-101', 'pve-150']);
      expect(
        [
          for (final id in ['all', 'running', 'stopped'])
            _statusCount(tester, id),
        ],
        ['2', '2', '0'],
      );
      expect(find.byKey(const Key('desktop-filter-reset')), findsNothing);
      expect(
        store.prefs['organize.https://vdi.example|tester'],
        contains('"status":"all"'),
      );
      expect(tester.takeException(), isNull);
    });

    testWidgets('a legacy stopped chip migrates to the separate status', (
      tester,
    ) async {
      final store = MemoryStore(
        origin: 'https://vdi.example',
        username: 'tester',
        prefs: {
          'organize.https://vdi.example|tester':
              '{"chip":"stopped","sort":"name"}',
        },
      );
      await _signIn(tester, FakeBroker(_fleet()), store: store);
      expect(_order(tester), ['pve-150']);
      await _status(tester, 'all');
      expect(_order(tester), ['pve-149', 'pve-150', 'pve-160', 'pve-101']);
      expect(
        store.prefs['organize.https://vdi.example|tester'],
        allOf(contains('"chip":"all"'), contains('"status":"all"')),
      );
    });

    for (final theme in ['dark', 'light']) {
      testWidgets('$theme: status scrolls at 300 dp with text scale 2', (
        tester,
      ) async {
        await _signIn(
          tester,
          FakeBroker(_fleet()),
          store: MemoryStore(
            origin: 'https://vdi.example',
            username: 'tester',
            prefs: {'appearance.theme': theme},
          ),
          scale: 2,
          size: const Size(900, 6000),
        );
        expect(tester.takeException(), isNull);
        final row = find.byKey(const Key('desktop-status-row'));
        final scroll = tester.state<ScrollableState>(
          find.descendant(of: row, matching: find.byType(Scrollable)),
        );
        expect(scroll.position.maxScrollExtent, greaterThan(0));
        await tester.drag(row, const Offset(-500, 0));
        await pumpWork(tester);
        expect(scroll.position.pixels, greaterThan(0));
        await _status(tester, 'stopped');
        expect(_order(tester), ['pve-150']);
        await _tapVisible(tester, find.byKey(const Key('desktop-search-open')));
        await pumpWork(tester);
        final search = find.byKey(const Key('desktop-search'));
        expect(search, findsOneWidget);
        expect(find.byKey(const Key('desktop-status')), findsOneWidget);
        expect(tester.getRect(search).right, lessThanOrEqualTo(300));
        expect(tester.getSize(search).height, tester.getSize(row).height);
        await tester.enterText(_input, 'lab');
        await pumpWork(tester);
        expect(_order(tester), ['pve-150']);
        expect(tester.takeException(), isNull);
      });
    }
  });

  group('구분 filter', () {
    Future<void> kind(WidgetTester tester, String id) async {
      final segment = find.byKey(Key('kind-$id'));
      await tester.ensureVisible(segment);
      await pumpWork(tester);
      await _tapVisible(tester, segment);
      await pumpWork(tester);
    }

    String count(WidgetTester tester, String id) =>
        tester.widget<Text>(find.byKey(Key('kind-count-$id'))).data!;
    const saved = 'organize.https://vdi.example|tester';

    testWidgets('only with both kinds; ANDs with 상태 and the search; '
        '필터 해제 clears it', (tester) async {
      await _signIn(tester, FakeBroker(_fleet()));
      expect(find.byKey(const Key('desktop-kind')), findsNothing);
      await tester.pumpWidget(const SizedBox());
      await _signIn(tester, FakeBroker(_mixed()));
      expect(find.byKey(const Key('desktop-kind')), findsOneWidget);
      expect(
        [
          for (final id in ['all', 'vm', 'pc']) count(tester, id),
        ],
        ['5', '4', '1'],
      );
      await kind(tester, 'pc');
      expect(_order(tester), ['pc-gaming']);
      expect(
        tester.widget<Text>(find.byKey(const Key('desktop-count'))).data,
        '1/5',
      );
      await kind(tester, 'vm');
      expect(_order(tester).length, 4);
      await _status(tester, 'stopped');
      expect(_order(tester), ['pve-150']);
      // No local PC is off: 로컬 PC counts 0 under 꺼짐 and isn't chosen.
      expect(count(tester, 'pc'), '0');
      await kind(tester, 'pc');
      expect(_order(tester), ['pve-150']);
      await _tapVisible(tester, find.byKey(const Key('desktop-filter-reset')));
      await pumpWork(tester);
      expect(_order(tester).length, 5);
      await _tapVisible(tester, find.byKey(const Key('desktop-search-open')));
      await pumpWork(tester);
      await tester.enterText(_input, 'gaming');
      await pumpWork(tester);
      expect(_order(tester), ['pc-gaming']);
      await kind(tester, 'vm');
      expect(_order(tester), isEmpty);
      expect(tester.takeException(), isNull);
    });

    testWidgets(
      'remembered for its server and account with the other filters',
      (tester) async {
        final store = MemoryStore(
          origin: 'https://vdi.example',
          username: 'tester',
        );
        await _signIn(tester, FakeBroker(_mixed()), store: store);
        await kind(tester, 'pc');
        expect(
          store.prefs[saved],
          allOf(contains('"kind":"pc"'), contains('"status":"all"')),
        );
        await tester.pumpWidget(const SizedBox());
        await _signIn(tester, FakeBroker(_mixed()), store: store);
        expect(_order(tester), ['pc-gaming']);
        await tester.pumpWidget(const SizedBox());
        store.username = 'other';
        await _signIn(tester, FakeBroker(_mixed()), store: store);
        expect(_order(tester).length, 5);
        // Without local PCs the filter is gone and counts as 전체 again.
        await tester.pumpWidget(const SizedBox());
        store.username = 'tester';
        await _signIn(tester, FakeBroker(_fleet()), store: store);
        expect(find.byKey(const Key('desktop-kind')), findsNothing);
        expect(_order(tester).length, 4);
        expect(store.prefs[saved], contains('"kind":"all"'));
      },
    );

    for (final scale in [1.0, 2.0]) {
      testWidgets('a phone puts 상태 under 구분, not behind the edge (×$scale)', (
        tester,
      ) async {
        await _signIn(tester, FakeBroker(_mixed()), scale: scale);
        expect(tester.takeException(), isNull);
        final row = find.byKey(const Key('desktop-status-row'));
        final kindRect = tester.getRect(find.byKey(const Key('desktop-kind')));
        final status = tester.getRect(find.byKey(const Key('desktop-status')));
        expect(
          find.descendant(
            of: row,
            matching: find.byKey(const Key('desktop-kind')),
          ),
          findsOneWidget,
        );
        expect(status.top, greaterThanOrEqualTo(kindRect.bottom));
        expect(status.left, closeTo(kindRect.left, 1));
        final target = find.byKey(const Key('kind-pc'));
        await tester.ensureVisible(target);
        await pumpWork(tester);
        final visible = tester.getRect(target).intersect(tester.getRect(row));
        await tester.tapAt(visible.center);
        await pumpWork(tester);
        expect(_order(tester), ['pc-gaming']);
        expect(tester.takeException(), isNull);
      });
    }
  });

  testWidgets('favourite: optimistic star, rollback with a notice on failure', (
    tester,
  ) async {
    final broker = _FavoriteBroker(_fleet())..answer = Completer<bool>();
    await _signIn(tester, broker);
    await _tapVisible(tester, find.byKey(const Key('desktop-more-pve-160')));
    await pumpWork(tester);
    expect(find.text('즐겨찾기에 추가'), findsOneWidget);
    await _tapVisible(tester, find.byKey(const Key('favorite-pve-160')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.byKey(const Key('favorite-star-pve-160')), findsOneWidget);
    expect(_order(tester).take(2), ['pve-149', 'pve-160']);
    broker.answer!.completeError(const BrokerException('network_error'));
    await pumpWork(tester);
    expect(find.byKey(const Key('favorite-star-pve-160')), findsNothing);
    expect(find.text('즐겨찾기를 바꾸지 못했어요. 잠시 후 다시 시도하세요.'), findsOneWidget);
    expect(broker.sent, [('pve-160', true)]);
  });

  testWidgets('favourite: the broker answer is kept and can be removed', (
    tester,
  ) async {
    final broker = _FavoriteBroker(_fleet());
    await _signIn(tester, broker);
    await _tapVisible(tester, find.byKey(const Key('desktop-more-pve-149')));
    await pumpWork(tester);
    expect(find.text('즐겨찾기에서 빼기'), findsOneWidget);
    await _tapVisible(tester, find.byKey(const Key('favorite-pve-149')));
    await pumpWork(tester);
    expect(find.byKey(const Key('favorite-star-pve-149')), findsNothing);
    expect(broker.sent, [('pve-149', false)]);
    expect(_order(tester).first, 'pve-101');
  });

  testWidgets('a broker without favourites offers none', (tester) async {
    await _signIn(
      tester,
      FakeBroker([
        _vm('pve-1', 'A', tags: ['x']),
        _vm('pve-2', 'B'),
      ]),
    );
    await _tapVisible(tester, find.byKey(const Key('desktop-more-pve-1')));
    await pumpWork(tester);
    expect(find.byKey(const Key('favorite-pve-1')), findsNothing);
    expect(find.byKey(const Key('details-pve-1')), findsOneWidget);
    // An old broker's FakeBroker is not a FavoriteBrokerApi either way.
  });

  testWidgets('phone width with text scale 2.0 keeps cards and bar intact', (
    tester,
  ) async {
    await _signIn(
      tester,
      FakeBroker([
        for (var i = 0; i < 6; i++)
          _vm(
            'pve-${100 + i}',
            'Desktop $i',
            tags: ['아주긴태그이름-$i'],
            pool: '풀-$i',
            favorite: i == 0,
          ),
      ]),
      scale: 2,
      size: const Size(1080, 9000),
    );
    expect(tester.takeException(), isNull);
    final chips = find.byKey(const Key('desktop-chips'));
    expect(chips, findsOneWidget);
    expect(tester.getSize(chips).height, lessThan(90));
    final heights = {
      for (final id in ['pve-100', 'pve-101', 'pve-102'])
        tester.getSize(_card(id)).height,
    };
    expect(heights.length, 1);
    await _tapVisible(tester, find.byKey(const Key('desktop-search-open')));
    await pumpWork(tester);
    expect(find.byKey(const Key('desktop-search')), findsOneWidget);
    await tester.enterText(_input, '풀-3');
    await pumpWork(tester);
    expect(_order(tester), ['pve-103']);
    expect(tester.takeException(), isNull);
  });
}

Future<void> _tapVisible(WidgetTester tester, Finder target) async {
  await tester.ensureVisible(target);
  await pumpWork(tester);
  await tester.tap(target);
}

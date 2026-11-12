// Release history ("변경 내역") over in-process fixtures of the server's notes
// history and download catalog. No HTTPS request is made and nothing is
// downloaded or installed.
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/host_update.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_update_dialog.dart';
import 'package:multitap_client/ui/product_update_dialog.dart';
import 'package:multitap_client/ui/release_notes.dart';

const _origin = 'https://releases.example:18443';

Map<String, Object?> releaseEntry(
  String version, {
  String product = 'client',
  String format = 'installer',
  String? changelog,
  String? releasedAt,
  String sha = 'a',
}) {
  final file =
      'Multitap${product == 'host' ? '-Host' : ''}-$version-windows-x64.exe';
  return {
    'product': product,
    'version': version,
    'filename': file,
    'size': 31457280,
    'sha256': sha * 64,
    'format': format,
    'url': '/downloads/$file',
    'changelog': ?changelog,
    'releasedAt': ?releasedAt,
  };
}

Map<String, Object?> note(String version, {String? changelog, String? date}) =>
    {'version': version, 'changelog': ?changelog, 'releasedAt': ?date};

/// The broker's notes history (`/api/v1/releases/history`, newest first,
/// paged by `before`) and its download catalog. [legacy] answers the history
/// like a released broker (404); [failPages] fails history pages after the
/// first that many.
class HistoryFixture implements UpdateTransport {
  HistoryFixture(
    this.notes, {
    this.catalog = const [],
    this.legacy = false,
    this.failPages,
    this.raw,
  });
  final List<Map<String, Object?>> notes;
  final List<Object?> catalog;
  final bool legacy;
  int? failPages;

  /// A literal history body instead of [notes].
  final Object? raw;
  final requests = <Uri>[];
  bool closed = false;

  int get historyRequests =>
      requests.where((u) => u.path == '/api/v1/releases/history').length;

  UpdateResponse _json(Object value, [int status = 200]) {
    final bytes = utf8.encode(jsonEncode(value));
    return UpdateResponse(status, bytes.length, Stream.value(bytes));
  }

  @override
  Future<UpdateResponse> get(Uri uri) async {
    requests.add(uri);
    expect(uri.origin, Uri.parse(_origin).origin);
    if (uri.path == '/api/v1/downloads') return _json({'releases': catalog});
    expect(uri.path, '/api/v1/releases/history');
    if (legacy) return _json({'error': {'code': 'not_found'}}, 404);
    if (raw != null) return _json(raw!);
    if (failPages != null && historyRequests > failPages!) {
      return _json({'error': {'code': 'internal_error'}}, 500);
    }
    final limit = int.parse(uri.queryParameters['limit']!);
    final before = uri.queryParameters['before'];
    var start = 0;
    if (before != null) {
      start = notes.indexWhere(
        (n) => compareProductVersions(n['version'] as String, before) < 0,
      );
      if (start < 0) start = notes.length;
    }
    final page = notes.skip(start).take(limit).toList();
    final more = start + limit < notes.length;
    return _json({
      'product': uri.queryParameters['product'],
      'total': notes.length,
      'releases': page,
      'next': more ? page.last['version'] : null,
    });
  }

  @override
  void close() => closed = true;
}

/// 150 client versions, 0.4.149 (newest) down to 0.4.0, every fifth without
/// notes and every seventh without a date.
List<Map<String, Object?>> manyNotes([int count = 150]) => [
  for (var i = count - 1; i >= 0; i--)
    note(
      '0.4.$i',
      changelog: i % 5 == 0 ? null : '- 버전 $i의 변경\n- 두 번째 항목 $i',
      date: i % 7 == 0
          ? null
          : DateTime.utc(2026, 1, 1).add(Duration(days: i)).toIso8601String(),
    ),
];

Future<void> settle(WidgetTester t) async {
  for (var i = 0; i < 12; i++) {
    await t.pump(const Duration(milliseconds: 30));
    await t.runAsync(() => Future<void>.delayed(Duration.zero));
  }
  await t.pumpAndSettle();
}

void main() {
  group('data', () {
    test('history pages walk the server history with before/next', () async {
      final fixture = HistoryFixture(manyNotes());
      final api = ProductUpdates(_origin, transport: fixture);
      final first = await api.historyPage('client', limit: 40);
      expect(first.releases, hasLength(40));
      expect(first.releases.first.version, '0.4.149');
      expect(first.next, '0.4.110');
      expect(first.total, 150);
      expect(first.releases.first.changelog, contains('버전 149'));
      expect(first.releases.first.releasedAt, DateTime.utc(2026, 5, 30));
      expect(fixture.requests.single.queryParameters, {
        'product': 'client',
        'limit': '40',
      });
      final seen = [...first.releases.map((n) => n.version)];
      String? next = first.next;
      while (next != null) {
        final page = await api.historyPage('client', before: next, limit: 40);
        expect(fixture.requests.last.queryParameters['before'], next);
        seen.addAll(page.releases.map((n) => n.version));
        next = page.next;
      }
      expect(seen, hasLength(150));
      expect(seen.toSet(), hasLength(150));
      expect(seen.last, '0.4.0');
      expect(
        () => api.historyPage('android-client'),
        throwsA(isA<UpdateFailure>()),
      );
      expect(
        () => api.historyPage('client', limit: 101),
        throwsA(isA<UpdateFailure>()),
      );
    });

    test('invalid, repeated, unordered or newer entries are left out', () async {
      final fixture = HistoryFixture(
        const [],
        raw: {
          'releases': [
            note('0.4.60', changelog: '- 정상'),
            note('0.4.59', changelog: 'BELL\u0007'),
            note('0.4.58', date: '2026-02-30T00:00:00.000Z'),
            note('0.4', changelog: 'BAD VERSION'),
            note('0.4.57', changelog: '- 정상 2', date: '2026-10-01T00:00:00.000Z'),
            note('0.4.57', changelog: 'DUPLICATE'),
            note('0.4.70', changelog: 'OUT OF ORDER'),
            {'version': 7, 'changelog': 'NOT A STRING'},
            'not an object',
          ],
          'next': '0.4.57',
          'total': 9,
        },
      );
      final page = await ProductUpdates(
        _origin,
        transport: fixture,
      ).historyPage('client');
      expect(page.releases.map((n) => n.version), ['0.4.60', '0.4.57']);
      expect(page.releases.last.changelog, '- 정상 2');
      expect(page.next, '0.4.57');
      expect(page.total, 9);
      // A cursor that does not move older ends the list.
      final stuck = await ProductUpdates(
        _origin,
        transport: HistoryFixture(
          const [],
          raw: {
            'releases': [note('0.4.10')],
            'next': '0.4.11',
          },
        ),
      ).historyPage('client');
      expect(stuck.next, isNull);
      await expectLater(
        ProductUpdates(
          _origin,
          transport: HistoryFixture(
            const [],
            raw: {
              'releases': [for (var i = 200; i > 99; i--) note('0.4.$i')],
            },
          ),
        ).historyPage('client'),
        throwsA(isA<UpdateFailure>()),
      );
    });

    test('a released broker without the history gives its catalog notes', () async {
      final fixture = HistoryFixture(
        const [],
        legacy: true,
        catalog: [
          releaseEntry('0.4.53', changelog: '- 새 디자인'),
          releaseEntry('0.4.52', changelog: '- 마지막 화면'),
          releaseEntry('0.2.6', product: 'host', changelog: 'HOST ONLY'),
          releaseEntry('0.4.49', format: 'archive', changelog: 'ARCHIVE'),
        ],
      );
      final page = await ProductUpdates(
        _origin,
        transport: fixture,
      ).historyPage('client');
      expect(page.releases.map((n) => n.version), ['0.4.53', '0.4.52']);
      expect(page.next, isNull);
      expect(fixture.requests.map((u) => u.path), [
        '/api/v1/releases/history',
        '/api/v1/downloads',
      ]);
      // A later page never falls back: it fails.
      await expectLater(
        ProductUpdates(
          _origin,
          transport: fixture,
        ).historyPage('client', before: '0.4.52'),
        throwsA(isA<UpdateFailure>()),
      );
    });

    test('the catalog history keeps validated installers only', () async {
      final releases = await ProductUpdates(
        _origin,
        transport: HistoryFixture(
          const [],
          catalog: [
            releaseEntry('0.4.53', changelog: '- a'),
            releaseEntry('0.4.48', changelog: 'INVALID\u0007NOTE'),
            releaseEntry('0.4.46', changelog: 'DUPLICATE ONE'),
            releaseEntry('0.4.46', changelog: 'DUPLICATE TWO', sha: 'b'),
          ],
        ),
      ).history('client');
      expect(releases.map((r) => r.version), ['0.4.53']);
    });
  });

  Future<HistoryFixture> openPanel(
    WidgetTester t, {
    List<Map<String, Object?>>? notes,
    HistoryFixture? fixture,
    String current = '0.4.147',
    Size size = const Size(1280, 860),
    String product = 'client',
  }) async {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = size;
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    final transport = fixture ?? HistoryFixture(notes ?? manyNotes());
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: Scaffold(
          body: Builder(
            builder: (context) => TextButton(
              onPressed: () => showReleaseHistory(
                context,
                server: _origin,
                product: product,
                currentVersion: current,
                api: ProductUpdates(_origin, transport: transport),
              ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await t.tap(find.text('open'));
    await settle(t);
    return transport;
  }

  testWidgets('the sidebar lists versions with dates and marks; a row shows its notes', (
    t,
  ) async {
    final fixture = await openPanel(t);
    expect(find.byKey(const Key('release-history-dialog')), findsOneWidget);
    expect(find.byKey(const Key('release-history-sidebar')), findsOneWidget);
    expect(
      find.text('Multitap Client · 현재 0.4.147 · 버전 150개'),
      findsOneWidget,
    );
    expect(fixture.historyRequests, 1);
    // Newest first and selected; newer versions and the running one marked.
    expect(find.text('v0.4.149'), findsNWidgets(2), reason: 'row + detail');
    expect(find.byKey(const Key('release-new-0.4.149')), findsWidgets);
    expect(find.byKey(const Key('release-new-0.4.148')), findsOneWidget);
    expect(
      find.descendant(
        of: find.byKey(const Key('release-version-0.4.147')),
        matching: find.byKey(const Key('release-current')),
      ),
      findsOneWidget,
    );
    expect(find.byKey(const Key('release-new-0.4.146')), findsNothing);
    expect(find.text('2026-05-30'), findsOneWidget);
    expect(find.text('버전 149의 변경'), findsOneWidget);
    expect(find.text('1 / 150'), findsOneWidget);
    // A click shows that version's notes on the right.
    await t.tap(find.byKey(const Key('release-version-0.4.146')));
    await t.pumpAndSettle();
    expect(find.text('버전 146의 변경'), findsOneWidget);
    expect(find.text('버전 149의 변경'), findsNothing);
    expect(
      t.widget<Text>(find.byKey(const Key('release-detail-version'))).data,
      'v0.4.146',
    );
    // A version without notes or date says so.
    await t.tap(find.byKey(const Key('release-version-0.4.145')));
    await t.pumpAndSettle();
    expect(find.text('이 버전의 변경 내역이 등록되지 않았어요.'), findsOneWidget);
    expect(find.text('2026-05-26 · 내역 없음'), findsOneWidget);
    await t.tap(find.byKey(const Key('release-version-0.4.140')));
    await t.pumpAndSettle();
    expect(find.text('배포일 정보 없음'), findsWidgets);
    expect(t.takeException(), isNull);
    await t.tap(find.byKey(const Key('close-release-history')));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('release-history-dialog')), findsNothing);
    expect(fixture.closed, isFalse, reason: 'A lent connection stays open.');
  });

  testWidgets('arrow keys, Home and End move the selection and keep it in view', (
    t,
  ) async {
    await openPanel(t);
    String selected() =>
        t.widget<Text>(find.byKey(const Key('release-detail-version'))).data!;
    await t.sendKeyEvent(LogicalKeyboardKey.arrowDown);
    await t.pumpAndSettle();
    expect(selected(), 'v0.4.148');
    await t.sendKeyEvent(LogicalKeyboardKey.arrowDown);
    await t.sendKeyEvent(LogicalKeyboardKey.arrowUp);
    await t.pumpAndSettle();
    expect(selected(), 'v0.4.148');
    await t.sendKeyEvent(LogicalKeyboardKey.pageDown);
    await t.pumpAndSettle();
    expect(selected(), 'v0.4.138');
    await t.sendKeyEvent(LogicalKeyboardKey.end);
    await settle(t);
    // End reaches the last loaded row and asks for the next page.
    expect(selected(), 'v0.4.110');
    expect(find.byKey(const Key('release-version-0.4.110')).hitTestable(), findsOneWidget);
    await t.sendKeyEvent(LogicalKeyboardKey.home);
    await t.pumpAndSettle();
    expect(selected(), 'v0.4.149');
    expect(find.byKey(const Key('release-version-0.4.149')).hitTestable(), findsOneWidget);
    // The step buttons do the same.
    await t.tap(find.byKey(const Key('release-older')));
    await t.pumpAndSettle();
    expect(selected(), 'v0.4.148');
    await t.tap(find.byKey(const Key('release-newer')));
    await t.pumpAndSettle();
    expect(selected(), 'v0.4.149');
    expect(
      t.widget<IconButton>(find.byKey(const Key('release-newer'))).onPressed,
      isNull,
    );
  });

  testWidgets('scrolling the sidebar to its end loads older pages until the first version', (
    t,
  ) async {
    final fixture = await openPanel(t);
    expect(fixture.historyRequests, 1);
    for (var i = 0; i < 12 && fixture.historyRequests < 4; i++) {
      await t.drag(
        find.byKey(const Key('release-history-sidebar')),
        const Offset(0, -2400),
      );
      await settle(t);
    }
    expect(fixture.historyRequests, 4);
    expect(
      fixture.requests
          .where((u) => u.path == '/api/v1/releases/history')
          .map((u) => u.queryParameters['before']),
      [null, '0.4.110', '0.4.70', '0.4.30'],
    );
    await t.drag(
      find.byKey(const Key('release-history-sidebar')),
      const Offset(0, -20000),
    );
    await settle(t);
    expect(find.byKey(const Key('release-version-0.4.0')), findsOneWidget);
    expect(find.byKey(const Key('release-history-loading-more')), findsNothing);
    expect(fixture.historyRequests, 4, reason: 'no request after the oldest');
  });

  testWidgets('전체 보기 streams every note under sticky version headers', (t) async {
    final fixture = await openPanel(t);
    await t.tap(find.byKey(const Key('release-history-view-all')));
    await settle(t);
    expect(find.byKey(const Key('release-history-all')), findsOneWidget);
    expect(find.byKey(const Key('release-history-sidebar')), findsNothing);
    expect(find.byKey(const Key('release-all-header-0.4.149')), findsOneWidget);
    expect(find.text('버전 149의 변경'), findsOneWidget);
    expect(find.text('버전 148의 변경'), findsOneWidget);
    final top = t.getTopLeft(find.byKey(const Key('release-history-all'))).dy;
    // Scroll into the second version: its header stays pinned on top.
    final second = t
        .getTopLeft(find.byKey(const Key('release-all-header-0.4.148')))
        .dy;
    await t.drag(
      find.byKey(const Key('release-history-all')),
      Offset(0, -(second - top) - 30),
    );
    await t.pumpAndSettle();
    expect(
      t.getTopLeft(find.byKey(const Key('release-all-header-0.4.148'))).dy,
      moreOrLessEquals(top, epsilon: .5),
    );
    // Reading on loads the older pages into the same scroll.
    for (var i = 0; i < 40 && fixture.historyRequests < 4; i++) {
      await t.sendKeyEvent(LogicalKeyboardKey.end);
      await settle(t);
    }
    expect(fixture.historyRequests, 4);
    await t.sendKeyEvent(LogicalKeyboardKey.end);
    await settle(t);
    expect(find.byKey(const Key('release-history-end')), findsOneWidget);
    expect(find.byKey(const Key('release-all-header-0.4.0')), findsOneWidget);
    // Back to 버전별 keeps the loaded list.
    await t.tap(find.byKey(const Key('release-history-view-versions')));
    await settle(t);
    expect(find.byKey(const Key('release-history-sidebar')), findsOneWidget);
    expect(fixture.historyRequests, 4);
    expect(t.takeException(), isNull);
  });

  testWidgets('a narrow window shows one timeline without the view switch', (
    t,
  ) async {
    await openPanel(t, size: const Size(620, 820));
    expect(find.byKey(const Key('release-history-all')), findsOneWidget);
    expect(find.byKey(const Key('release-history-sidebar')), findsNothing);
    expect(find.byKey(const Key('release-history-view')), findsNothing);
    expect(find.byKey(const Key('release-all-header-0.4.149')), findsOneWidget);
    expect(find.text('버전 149의 변경'), findsOneWidget);
    expect(t.takeException(), isNull);
  });

  testWidgets('loading, failure with retry, an empty server and a failed later page', (
    t,
  ) async {
    final failing = HistoryFixture(manyNotes(), failPages: 0);
    await openPanel(t, fixture: failing);
    expect(find.byKey(const Key('release-history-error')), findsOneWidget);
    failing.failPages = null;
    await t.tap(find.byKey(const Key('release-history-retry')));
    await settle(t);
    expect(find.byKey(const Key('release-history-error')), findsNothing);
    expect(find.text('v0.4.149'), findsWidgets);
    await t.tap(find.byKey(const Key('close-release-history')));
    await t.pumpAndSettle();

    await openPanel(t, fixture: HistoryFixture(const []));
    expect(find.byKey(const Key('release-history-empty')), findsOneWidget);
    expect(find.text('이 서버에 등록된 변경 내역이 없어요.'), findsOneWidget);
    await t.tap(find.byKey(const Key('close-release-history')));
    await t.pumpAndSettle();

    final later = HistoryFixture(manyNotes(), failPages: 1);
    await openPanel(t, fixture: later);
    for (var i = 0; i < 6; i++) {
      await t.drag(
        find.byKey(const Key('release-history-sidebar')),
        const Offset(0, -4000),
      );
      await settle(t);
    }
    expect(find.byKey(const Key('release-history-more-retry')), findsOneWidget);
    final failedRequests = later.historyRequests;
    later.failPages = null;
    await t.tap(find.byKey(const Key('release-history-more-retry')));
    await settle(t);
    expect(later.historyRequests, failedRequests + 1);
    expect(find.byKey(const Key('release-history-more-retry')), findsNothing);
    expect(find.text('v0.4.109'), findsOneWidget);
  });

  testWidgets('the update panel opens the history beside the new notes', (
    t,
  ) async {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = const Size(1200, 900);
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    final transport = HistoryFixture(
      manyNotes(60),
      catalog: [
        releaseEntry(
          '0.4.59',
          changelog: '연결 확인 창을 새 디자인으로 정리했습니다.\n\n- 확인 창 모양 통일',
          releasedAt: '2026-10-25T05:00:00.000Z',
        ),
      ],
    );
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: Scaffold(
          body: Builder(
            builder: (context) => TextButton(
              onPressed: () => showDialog<void>(
                context: context,
                builder: (_) => ProductUpdateDialog(
                  server: _origin,
                  product: 'client',
                  currentVersion: '0.4.59',
                  api: ProductUpdates(_origin, transport: transport),
                ),
              ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await t.tap(find.text('open'));
    await settle(t);
    expect(find.text('최신 버전이에요'), findsOneWidget);
    expect(find.text('확인 창 모양 통일'), findsOneWidget);
    await t.tap(find.byKey(const Key('open-release-history')));
    await settle(t);
    expect(find.byKey(const Key('release-history-dialog')), findsOneWidget);
    expect(find.byKey(const Key('release-current')), findsWidgets);
    expect(transport.historyRequests, 1);
    expect(t.takeException(), isNull);
  });

  testWidgets('the remote Host update opens the Host history', (t) async {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = const Size(1200, 900);
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    final transport = HistoryFixture([
      note('0.2.67', changelog: '- Host 최신'),
      note('0.2.66', changelog: '- Host 이전'),
    ]);
    const machine = DesktopMachine(
      id: 'pve-149',
      name: 'TESTER',
      description: '',
      kind: MachineKind.vm,
      status: MachineStatus.ready,
      specification: 'Windows 11 Pro',
      accent: Colors.blue,
      hostUpdate: HostUpdateState(
        present: true,
        connected: true,
        supported: true,
        version: '0.2.66',
        latestVersion: '0.2.67',
        newer: true,
        available: true,
      ),
    );
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: Scaffold(
          body: Builder(
            builder: (context) => TextButton(
              onPressed: () => showDialog<void>(
                context: context,
                builder: (_) => HostUpdateConfirmDialog(
                  machine: machine,
                  server: _origin,
                  loadRelease: () async => null,
                  api: ProductUpdates(_origin, transport: transport),
                ),
              ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await t.tap(find.text('open'));
    await settle(t);
    expect(find.byKey(const Key('host-update-dialog')), findsOneWidget);
    await t.tap(find.byKey(const Key('open-host-release-history')));
    await settle(t);
    expect(find.byKey(const Key('release-history-dialog')), findsOneWidget);
    expect(find.text('Multitap Host · 현재 0.2.66 · 버전 2개'), findsOneWidget);
    expect(find.byKey(const Key('release-new-0.2.67')), findsWidgets);
    expect(find.text('Host 최신'), findsOneWidget);
    expect(
      transport.requests.single.queryParameters['product'],
      'host',
    );
    // Closing the history keeps the Host question open.
    await t.tap(find.byKey(const Key('close-release-history')));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('host-update-dialog')), findsOneWidget);
  });

  testWidgets('bullets and paragraphs render as separate readable rows', (
    t,
  ) async {
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: const Scaffold(
          body: ChangelogText(
            '첫 문단입니다.\n\n- 첫째 항목\n- 둘째 항목\n  - 하위 항목\n\n끝 문단입니다.',
          ),
        ),
      ),
    );
    for (final line in ['첫 문단입니다.', '첫째 항목', '둘째 항목', '하위 항목', '끝 문단입니다.']) {
      expect(find.text(line), findsOneWidget, reason: line);
    }
    expect(find.textContaining('- '), findsNothing);
    expect(
      t.getTopLeft(find.text('하위 항목')).dx,
      greaterThan(t.getTopLeft(find.text('둘째 항목')).dx),
    );
  });
}

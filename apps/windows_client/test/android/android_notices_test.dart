import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_launcher.dart'
    show androidTheme;
import 'package:multitap_client/android/android_notices.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/data/notices.dart';
import 'package:multitap_client/ui/app_theme.dart';

import 'package:multitap_client/data/broker_api.dart' show BrokerException;

import '../notices_test.dart'
    show FakeNotices, FakeRemovableNotices, base, hex, notice;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore, vm;

// 공지 on Android (lib/android/android_notices.dart): the layer above the
// pages (launcher and stream), the header's bell, and the notification of a
// notice that arrives while the app is not in front. Widget behaviour; the
// native notification itself (AndroidNotices.kt) is not run here.

const _pen = MethodChannel('multitap/pen-input');

/// The Android test broker with notice-v1.
class NoticeBroker extends FakeBroker implements NoticeBrokerApi {
  NoticeBroker(super.inventory);
  final feed = FakeNotices();
  @override
  Future<List<BrokerNotice>> notices(String token) => feed.notices(token);
  @override
  Future<void> markNoticesSeen(String token, List<String> ids) =>
      feed.markNoticesSeen(token, ids);
}

/// The layer over a page that counts the touches that reach it and holds the
/// keyboard focus.
Future<({AndroidNoticeHost host, List<Offset> touches, List<BrokerNotice> posted})>
layer(
  WidgetTester t,
  NoticeCenter center, {
  Size size = const Size(360, 800),
  EdgeInsets insets = const EdgeInsets.only(top: 32, bottom: 16),
  bool streaming = false,
}) async {
  t.view.physicalSize = size;
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  final host = AndroidNoticeHost()
    ..bind(center)
    ..streaming = streaming;
  addTearDown(host.dispose);
  final touches = <Offset>[], posted = <BrokerNotice>[];
  await t.pumpWidget(
    MaterialApp(
      theme: androidTheme(buildAppTheme(brightness: Brightness.dark)),
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(context).copyWith(padding: insets),
        child: AndroidNoticeLayer(
          host: host,
          post: (notice) async {
            posted.add(notice);
            return true;
          },
          child: child!,
        ),
      ),
      home: Focus(
        autofocus: true,
        debugLabel: 'page',
        child: Listener(
          behavior: HitTestBehavior.opaque,
          onPointerDown: (event) => touches.add(event.position),
          child: const SizedBox.expand(),
        ),
      ),
    ),
  );
  await settle(t);
  return (host: host, touches: touches, posted: posted);
}

final _open = <NoticeCenter>[];

/// A widget test that ends with its widgets gone and its notices closed.
void noticeTest(String description, Future<void> Function(WidgetTester t) body) =>
    testWidgets(description, (t) async {
      try {
        await body(t);
      } finally {
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
        await t.pumpWidget(const SizedBox.shrink());
        for (final center in _open) {
          center.dispose();
        }
        _open.clear();
        await t.pump();
      }
    });

Future<void> settle(WidgetTester t) async {
  await t.pump();
  await t.pump(const Duration(milliseconds: 200));
  await t.pump(const Duration(milliseconds: 200));
}

Future<NoticeCenter> started(WidgetTester t, FakeNotices api) async {
  final center = NoticeCenter(api: api, token: 't', clock: () => base);
  _open.add(center);
  center.start();
  await t.pump();
  return center;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  noticeTest('launcher: the banner sits under the header, takes only its own touches and no focus', (
    t,
  ) async {
    final api = FakeNotices()..list = [notice(1)];
    final center = await started(t, api);
    final f = await layer(t, center);
    final banner = t.getRect(find.byKey(const Key('notice-banner')));
    expect(banner.top, 32 + 64, reason: 'under the status bar and the header');
    expect(banner.left, greaterThanOrEqualTo(12));
    expect(banner.right, lessThanOrEqualTo(360 - 12));
    expect(banner.width, lessThanOrEqualTo(360));
    expect(FocusManager.instance.primaryFocus?.debugLabel, 'page');
    await t.tapAt(const Offset(180, 700));
    await t.tapAt(Offset(180, banner.bottom + 8));
    expect(f.touches.length, 2, reason: 'everything outside the card passes');
    await t.tapAt(banner.center);
    await settle(t);
    expect(f.touches.length, 2);
    expect(find.byKey(const Key('notice-banner')), findsNothing);
    expect(api.seen, [
      [hex(1)],
    ]);
    expect(FocusManager.instance.primaryFocus?.debugLabel, 'page');
    expect(f.posted, isEmpty, reason: 'in front: no notification');
  });

  noticeTest('stream: the banner keeps clear of the strip the floating buttons rest in, on every edge', (
    t,
  ) async {
    final api = FakeNotices()..list = [notice(1)];
    final center = await started(t, api);
    for (final (size, insets) in const [
      (Size(915, 412), EdgeInsets.only(top: 24, left: 32, bottom: 16)),
      (Size(360, 800), EdgeInsets.only(top: 32, bottom: 16)),
      (Size(1280, 800), EdgeInsets.only(top: 24, bottom: 16)),
    ]) {
      await layer(t, center, size: size, insets: insets, streaming: true);
      final banner = t.getRect(find.byKey(const Key('notice-banner')));
      // The buttons are about 48 dp wide with their margin, against an edge.
      const strip = 64.0;
      final strips = [
        Rect.fromLTWH(insets.left, 0, strip, size.height), // left edge
        Rect.fromLTWH(size.width - insets.right - strip, 0, strip, size.height),
        Rect.fromLTWH(0, insets.top, size.width, strip), // top edge
        Rect.fromLTWH(0, size.height - insets.bottom - strip, size.width, strip),
      ];
      for (final area in strips) {
        expect(banner.overlaps(area), isFalse, reason: '$size $area');
      }
      expect(banner.top, insets.top + androidNoticeStreamInset);
      expect(banner.width, greaterThan(180), reason: 'still wide enough to read');
      expect(t.takeException(), isNull, reason: '$size');
    }
    // Over a stream the card is the fixed dark one.
    final box = t.widget<DecoratedBox>(
      find.descendant(
        of: find.byKey(const Key('notice-banner')),
        matching: find.byType(DecoratedBox),
      ).first,
    );
    expect((box.decoration as BoxDecoration).color!.computeLuminance(), lessThan(.05));
  });

  noticeTest('not in front: a notice that arrives is posted once as a notification and its banner waits', (
    t,
  ) async {
    final api = FakeNotices();
    final center = await started(t, api);
    final f = await layer(t, center, streaming: true);
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    api.list = [notice(1, kind: 'maintenance', scheduled: const Duration(minutes: 1), lifetime: const Duration(hours: 1))];
    center.signal();
    await t.pump();
    expect(f.posted.map((n) => n.id), [hex(1)]);
    // More changes of the same notice post nothing again.
    center.signal();
    await t.pump();
    await t.pump(const Duration(minutes: 3));
    expect(f.posted.length, 1);
    expect(center.banner?.id, hex(1), reason: 'the banner did not spend its time unseen');
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await settle(t);
    expect(find.byKey(const Key('notice-banner')), findsOneWidget);
    expect(find.textContaining('점검 예고'), findsOneWidget);
    // In front again: the next one is a banner only.
    api.list = [notice(2), ...api.list];
    center.signal();
    await t.pump();
    expect(f.posted.length, 1);
  });

  testWidgets('the notification goes through the existing native channel and never fails the app', (
    t,
  ) async {
    final calls = <MethodCall>[];
    Object? answer = true;
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, (
      call,
    ) async {
      calls.add(call);
      return answer;
    });
    addTearDown(
      () => messenger.setMockMethodCallHandler(
        AndroidSessionKeepalive.channel,
        null,
      ),
    );
    Future<bool> post(BrokerNotice value) async =>
        (await t.runAsync(() => androidPostNotice(value)))!;
    expect(await post(notice(1)), isTrue);
    expect(calls.single.method, 'notice');
    final sent = calls.single.arguments as Map;
    expect(sent['title'], '오늘 오후 6시 서버 점검 안내');
    expect(sent['body'], '저장하고 연결을 끊어 주세요.');
    expect(sent['id'], inInclusiveRange(0, 1023));
    expect(sent.keys.toSet(), {'id', 'title', 'body'}, reason: 'text only: nothing to open');
    expect(
      await post(
        notice(2, kind: 'maintenance', scheduled: const Duration(hours: 1)),
      ),
      isTrue,
    );
    expect((calls.last.arguments as Map)['title'], '점검 예고 · 오늘 오후 6시 서버 점검 안내');
    // Not allowed, an unexpected answer, an older native part: false, no error.
    answer = false;
    expect(await post(notice(1)), isFalse);
    answer = 'started';
    expect(await post(notice(1)), isFalse);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
    expect(await post(notice(1)), isFalse);
  });

  testWidgets('the app: after sign-in the header has the bell, the banner rises, and signing out takes both away', (
    t,
  ) async {
    messenger.setMockMethodCallHandler(_pen, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? true : null,
    );
    addTearDown(() {
      messenger.setMockMethodCallHandler(_pen, null);
      messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
    });
    t.view.physicalSize = const Size(1080, 2400);
    t.view.devicePixelRatio = 3;
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    final store = MemoryStore(origin: 'https://vdi.example', username: 'tester');
    store.prefs.putIfAbsent('launcher.view', () => 'cards');
    store.install();
    final now = DateTime.now();
    final broker = NoticeBroker([vm()]);
    broker.feed.list = [
      BrokerNotice(
        id: hex(1),
        kind: NoticeKind.general,
        title: '새 프린터 드라이버를 설치했어요',
        body: '인쇄가 안 되면 다시 연결해 주세요.',
        sentAt: now.subtract(const Duration(hours: 2)),
        expiresAt: now.add(const Duration(hours: 20)),
      ),
    ];
    await t.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('notice-bell')), findsNothing, reason: 'nobody is signed in');
    await t.enterText(find.byKey(const Key('sign-in-password')), 'secret-pass');
    await t.ensureVisible(find.byKey(const Key('login-submit')));
    await t.tap(find.byKey(const Key('login-submit')));
    await t.pumpAndSettle();
    // Signed in and still signed in: the layer did not rebuild the pages.
    expect(find.byKey(const Key('workspace-header')), findsOneWidget);
    expect(broker.feed.tokens, {'not-persisted'});
    expect(find.byKey(const Key('notice-bell')), findsOneWidget);
    expect(find.byKey(const Key('notice-bell-count')), findsOneWidget);
    expect(find.byKey(const Key('notice-banner')), findsOneWidget);
    expect(find.text('새 프린터 드라이버를 설치했어요'), findsOneWidget);
    expect(t.takeException(), isNull);
    final bell = t.getRect(find.byKey(const Key('notice-bell')));
    final header = t.getRect(find.byKey(const Key('workspace-header')));
    expect(header.contains(bell.center), isTrue);
    expect(bell.width, greaterThanOrEqualTo(48), reason: 'a full touch target');
    expect(
      bell.right,
      lessThanOrEqualTo(t.getRect(find.byKey(const Key('android-update-check'))).left + .5),
      reason: 'left of the update button',
    );
    // The list behind the bell: opening it is seeing.
    await t.tap(find.byKey(const Key('notice-bell')));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('notice-list')), findsOneWidget);
    // This test broker answers like an older server: nothing to remove with.
    expect(find.byTooltip('지우기'), findsNothing);
    expect(find.byKey(const Key('notice-remove-all')), findsNothing);
    expect(broker.feed.seen, [
      [hex(1)],
    ]);
    await t.tapAt(const Offset(20, 700));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('notice-bell-count')), findsNothing);
    expect(find.byKey(const Key('notice-banner')), findsNothing);
    await t.pumpWidget(const SizedBox.shrink());
    await t.pumpAndSettle();
  });

  // ---- 지우기 in the Android list (the header's bell).
  Future<void> bell(
    WidgetTester t,
    NoticeCenter center, {
    Size size = const Size(360, 800),
    Brightness brightness = Brightness.dark,
  }) async {
    t.view.physicalSize = size;
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    await t.pumpWidget(
      MaterialApp(
        theme: androidTheme(buildAppTheme(brightness: brightness)),
        home: Scaffold(
          body: Align(
            alignment: Alignment.topRight,
            child: Padding(
              padding: const EdgeInsets.only(top: 40, right: 100),
              child: AndroidNoticeBell(center: center),
            ),
          ),
        ),
      ),
    );
    await settle(t);
    await t.tap(find.byKey(const Key('notice-bell')));
    await settle(t);
    expect(find.byKey(const Key('notice-list')), findsOneWidget);
  }

  noticeTest('Android list: 지우기 on each row and 모두 지우기 are full touch targets and remove after one question', (
    t,
  ) async {
    final api = FakeRemovableNotices()
      ..list = [notice(3), notice(2), notice(1)];
    final center = await started(t, api);
    await bell(t, center);
    final panel = t.getRect(find.byKey(const Key('anchored-panel')));
    expect(panel.left, greaterThanOrEqualTo(0));
    expect(panel.right, lessThanOrEqualTo(360));
    for (final n in [1, 2, 3]) {
      final finder = find.byKey(ValueKey('notice-remove-${hex(n)}'));
      expect(t.widget<IconButton>(finder).tooltip, '지우기');
      final box = t.getRect(finder);
      expect(box.width, greaterThanOrEqualTo(48));
      expect(box.height, greaterThanOrEqualTo(48));
      expect(box.right, lessThanOrEqualTo(panel.right));
    }
    final all = t.getRect(find.byKey(const Key('notice-remove-all')));
    expect(all.height, greaterThanOrEqualTo(48));
    expect(all.right, lessThanOrEqualTo(panel.right));
    expect(find.text('모두 지우기'), findsOneWidget);
    await t.tap(find.byKey(ValueKey('notice-remove-${hex(2)}')));
    await t.pumpAndSettle();
    expect(find.text('이 공지를 지울까요?'), findsOneWidget);
    // The question's buttons are touch targets too.
    expect(t.getRect(find.byKey(const Key('notice-remove-accept'))).height, greaterThanOrEqualTo(48));
    expect(api.removed, isEmpty, reason: 'nothing before the answer');
    await t.tap(find.byKey(const Key('notice-remove-accept')));
    await t.pump();
    expect(find.byKey(ValueKey('notice-row-${hex(2)}')), findsNothing);
    await t.pumpAndSettle();
    expect(find.byKey(const Key('notice-remove-confirm')), findsNothing);
    expect(api.removed, [
      [hex(2)],
    ]);
    // 모두 지우기 while the send fails: empty here, sent again later.
    api.removeError = const BrokerException('network_error');
    await t.tap(find.byKey(const Key('notice-remove-all')));
    await t.pumpAndSettle();
    expect(find.text('공지 2개를 모두 지울까요?'), findsOneWidget);
    await t.tap(find.byKey(const Key('notice-remove-accept')));
    await settle(t);
    expect(find.text('받은 공지가 없어요.'), findsOneWidget);
    expect(find.byKey(const Key('notice-remove-all')), findsNothing);
    expect(api.removed.last, [hex(3), hex(1)]);
    api.removeError = null;
    center.signal();
    await settle(t);
    expect(api.removed.length, 3);
    expect(api.list, isEmpty);
    expect(t.takeException(), isNull);
    await t.tapAt(const Offset(20, 700));
    await settle(t);
  });

  noticeTest('Android list with an older broker (server 1.0.45): neither control is there', (
    t,
  ) async {
    final api = FakeRemovableNotices(offered: false)
      ..list = [notice(2), notice(1)];
    final center = await started(t, api);
    await bell(t, center);
    expect(find.byKey(ValueKey('notice-row-${hex(1)}')), findsOneWidget);
    expect(find.byTooltip('지우기'), findsNothing);
    expect(find.byKey(const Key('notice-remove-all')), findsNothing);
    expect(find.text('모두 지우기'), findsNothing);
    await t.tapAt(const Offset(20, 700));
    await settle(t);
    expect(api.removed, isEmpty);
  });
}

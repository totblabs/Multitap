import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/notices.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/notice_widgets.dart';
import 'package:multitap_client/ui/primary_region.dart';
import 'package:multitap_client/ui/session_window.dart';
import 'package:multitap_client/ui/themed_app.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'notices_test.dart'
    show FakeNotices, FakeRemovableNotices, hex, wire;
import 'session_recovery_test.dart' show mockPresentation;
import 'session_window_lifecycle_test.dart' show launch;

// 공지 on screen (lib/ui/notice_widgets.dart): the launcher's banner and bell,
// the list behind the bell, and the banner over a stream.

DesktopMachine _vm(
  String id,
  String name,
  String state, {
  int monitors = 1,
  StreamPreparation? preparation,
}) => DesktopMachine(
  id: id,
  name: name,
  description: '',
  kind: MachineKind.vm,
  status: state == 'stopped'
      ? MachineStatus.poweredOff
      : preparation == null
      ? MachineStatus.ready
      : MachineStatus.awaitingHost,
  specification: 'Windows 11 Pro',
  accent: Colors.blue,
  hostOnline: state != 'stopped',
  interactiveSession: true,
  virtualDisplayReady: preparation == null,
  streamPreparation: preparation,
  powerState: state,
  powerStale: false,
  resources: VmResources(
    cpuUsagePercent: state == 'stopped' ? null : 7.4,
    vcpus: 4,
    memoryUsedBytes: state == 'stopped' ? null : 5583457484,
    memoryTotalBytes: 8589934592,
    checkedAt: DateTime.utc(2026, 10, 25, 4),
    stale: false,
  ),
  virtualMonitorControl: true,
  display: VirtualDisplayState(
    supported: true,
    activeCount: monitors,
    stale: false,
  ),
  monitors: [
    const RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
    if (monitors > 1) const RemoteMonitor(id: '1', width: 1920, height: 1080),
  ],
);

final _desktops = [
  _vm('pve-149', 'TESTER', 'running', monitors: 2),
  _vm('pve-151', '개발용 VM', 'stopped'),
  _vm(
    'pve-152',
    '디자인 VM',
    'running',
    monitors: 2,
    preparation: StreamPreparation.display,
  ),
];

final _open = <NoticeCenter>[];

/// A widget test that ends with its widgets gone and its notices closed, so
/// none of their timers outlives it.
void noticeTest(String description, Future<void> Function(WidgetTester t) body) =>
    testWidgets(description, (t) async {
      try {
        await body(t);
      } finally {
        await t.pumpWidget(const SizedBox.shrink());
        for (final center in _open) {
          center.dispose();
        }
        _open.clear();
        await t.pump();
      }
    });

/// A banner has risen or gone: the read, the new frame and its short motion.
Future<void> settle(WidgetTester t) async {
  await t.pump();
  await t.pump(const Duration(milliseconds: 200));
  await t.pump(const Duration(milliseconds: 200));
}

/// A notice around the real time (the widgets say "5분 뒤" by the real clock).
BrokerNotice live(
  int n, {
  String title = '오늘 오후 6시 서버 점검 안내',
  String body = '저장하고 연결을 끊어 주세요.',
  bool maintenance = false,
  Duration sent = Duration.zero,
  Duration? scheduled,
}) {
  final now = DateTime.now();
  return BrokerNotice(
    id: hex(n),
    kind: maintenance ? NoticeKind.maintenance : NoticeKind.general,
    title: title,
    body: body,
    sentAt: now.add(sent),
    expiresAt: now.add(const Duration(hours: 20)),
    scheduledAt: scheduled == null ? null : now.add(scheduled),
  );
}

Future<NoticeCenter> started(
  WidgetTester t,
  FakeNotices api, {
  NoticeGreeting greeting = NoticeGreeting.unseen,
}) async {
  final center = NoticeCenter(
    api: api,
    token: 'fixture-token',
    greeting: greeting,
  );
  _open.add(center);
  center.start();
  await t.pump();
  return center;
}

Future<void> launcher(
  WidgetTester t,
  NoticeCenter? center, {
  Size size = const Size(1280, 800),
  double scale = 1,
  bool active = true,
  bool demo = false,
  ThemeMode mode = ThemeMode.dark,
}) async {
  t.view.physicalSize = size;
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  t.platformDispatcher.textScaleFactorTestValue = scale;
  addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
  final appearance = AppAppearance(mode: mode);
  addTearDown(appearance.dispose);
  await t.pumpWidget(
    ThemedApp(
      appearance: appearance,
      home: WorkspaceShell(
        onSignOut: () {},
        onRefresh: () {},
        displayName: '백종현',
        serverOrigin: 'https://vdi.example.com',
        machines: _desktops,
        isDemo: demo,
        onConnect: (_, {options = const StreamOptions()}) async =>
            throw StateError('fixture only'),
        onPower: (_, _) async {},
        notices: center,
        noticesActive: active,
      ),
    ),
  );
  await t.pump(const Duration(milliseconds: 300));
}

/// What stands for the remote view: it owns the keyboard focus and counts the
/// keys and clicks that reach it.
class _Remote extends StatefulWidget {
  const _Remote({required this.keys, required this.clicks});
  final List<LogicalKeyboardKey> keys;
  final List<Offset> clicks;
  @override
  State<_Remote> createState() => _RemoteState();
}

class _RemoteState extends State<_Remote> {
  final node = FocusNode(debugLabel: 'remote-view');
  @override
  void dispose() {
    node.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Focus(
    focusNode: node,
    autofocus: true,
    onKeyEvent: (_, event) {
      if (event is KeyDownEvent) widget.keys.add(event.logicalKey);
      return KeyEventResult.handled;
    },
    child: Listener(
      behavior: HitTestBehavior.opaque,
      onPointerDown: (event) => widget.clicks.add(event.position),
      child: const ColoredBox(
        color: Color(0xFF123456),
        child: SizedBox.expand(),
      ),
    ),
  );
}

/// The banner as a session window lays it out, over [_Remote].
Future<void> stream(
  WidgetTester t,
  NoticeCenter center,
  List<LogicalKeyboardKey> keys,
  List<Offset> clicks, {
  Size size = const Size(1280, 720),
  Rect? primary,
}) async {
  t.view.physicalSize = size;
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  await t.pumpWidget(
    MaterialApp(
      builder: PrimaryRegionScope.builder,
      home: Builder(
        builder: (context) {
          // 모든 모니터: the remote view publishes the primary monitor's part.
          WidgetsBinding.instance.addPostFrameCallback(
            (_) => PrimaryRegionScope.maybeOf(context)?.value = primary,
          );
          return Stack(
            fit: StackFit.expand,
            children: [
              _Remote(keys: keys, clicks: clicks),
              NoticeCorner(
                top: 44,
                child: NoticeBanner(center: center, onStream: true),
              ),
            ],
          );
        },
      ),
    ),
  );
  await t.pump(const Duration(milliseconds: 300));
}

class _SessionApi extends Fake implements BrokerApi, NoticeBrokerApi {
  final notices_ = FakeNotices();
  // No desktop: the window stays on its "could not load" panel, which is
  // enough content for a notice to rise over.
  @override
  Future<List<BrokerDesktop>> desktops(String token) async => [];
  @override
  Future<List<BrokerNotice>> notices(String token) => notices_.notices(token);
  @override
  Future<void> markNoticesSeen(String token, List<String> ids) =>
      notices_.markNoticesSeen(token, ids);
  @override
  void close() {}
}

class _Guard implements WindowCloseGuard {
  @override
  void attach(Future<void> Function() value) {}
  @override
  Future<bool> update({required bool busy, required bool dirty}) async => true;
  @override
  Future<bool> close() async => true;
  @override
  void dispose() {}
}

void main() {
  noticeTest('launcher: a notice rises as a banner, the bell counts what is unread, and the list is untouched', (
    t,
  ) async {
    final api = FakeNotices()
      ..list = [
        live(2, title: '방금 온 공지', sent: const Duration(minutes: -1)),
        live(1, sent: const Duration(hours: -2, minutes: -1)),
      ];
    final center = await started(t, api);
    await launcher(t, center);
    await settle(t);
    expect(find.byKey(const Key('notice-banner')), findsOneWidget);
    expect(find.text('오늘 오후 6시 서버 점검 안내'), findsOneWidget);
    expect(find.text('저장하고 연결을 끊어 주세요.'), findsOneWidget);
    expect(find.text('방금 온 공지'), findsNothing, reason: 'the oldest unseen notice is read first');
    expect(find.text('공지 1개 더 있어요.'), findsOneWidget);
    expect(
      t.widget<Text>(find.byKey(const Key('notice-age'))).data,
      '2시간 전',
    );
    expect(find.byKey(const Key('notice-bell')), findsOneWidget);
    expect(
      find.descendant(
        of: find.byKey(const Key('notice-bell-count')),
        matching: find.text('2'),
      ),
      findsOneWidget,
    );
    // The desktop list is as it was: same title, same tiles, nothing moved.
    expect(find.text('내 데스크톱'), findsOneWidget);
    expect(find.byKey(const Key('workspace-scroll')), findsOneWidget);
    final scroll = t.getRect(find.byKey(const Key('workspace-scroll')));
    final banner = t.getRect(find.byKey(const Key('notice-banner')));
    expect(scroll.contains(banner.topLeft), isTrue, reason: 'it floats over the list');
    expect(banner.bottom, scroll.bottom - 16, reason: 'in the bottom-right corner');
    expect(banner.top, greaterThan(400), reason: 'clear of the title row and its filters');
    expect(banner.right, lessThanOrEqualTo(1280 - 16));
    expect(banner.width, lessThanOrEqualTo(380));
    expect(t.takeException(), isNull);
  });

  noticeTest('launcher: the banner takes no keyboard focus when it arrives or when it is clicked', (
    t,
  ) async {
    final api = FakeNotices();
    final center = await started(t, api);
    await launcher(t, center);
    expect(find.byKey(const Key('notice-banner')), findsNothing);
    // The keyboard is somewhere in the launcher.
    await t.sendKeyEvent(LogicalKeyboardKey.tab);
    await t.pump();
    final focused = FocusManager.instance.primaryFocus;
    expect(focused?.context, isNotNull);
    api.list = [live(1)];
    center.signal();
    await settle(t);
    expect(find.byKey(const Key('notice-banner')), findsOneWidget);
    expect(FocusManager.instance.primaryFocus, same(focused));
    // Nothing inside the banner can be focused at all.
    expect(
      find.ancestor(
        of: find.byKey(const Key('notice-banner')),
        matching: find.byType(ExcludeFocus),
      ),
      findsWidgets,
    );
    await t.tap(find.byKey(const Key('notice-banner')));
    await settle(t);
    expect(FocusManager.instance.primaryFocus, same(focused));
    expect(find.byKey(const Key('notice-banner')), findsNothing);
    expect(api.seen, [
      [hex(1)],
    ]);
    expect(find.byKey(const Key('notice-bell-count')), findsNothing);
  });

  noticeTest('launcher: a banner goes down by itself, stays unread in the bell, and the list marks it seen', (
    t,
  ) async {
    final api = FakeNotices()..list = [live(1)];
    final center = await started(t, api);
    await launcher(t, center);
    await settle(t);
    expect(find.byKey(const Key('notice-banner')), findsOneWidget);
    await t.pump(const Duration(seconds: 8));
    expect(find.byKey(const Key('notice-banner')), findsOneWidget);
    await t.pump(const Duration(seconds: 4));
    await settle(t);
    expect(find.byKey(const Key('notice-banner')), findsNothing);
    expect(api.seen, isEmpty, reason: 'gone by itself is not seen');
    expect(find.byKey(const Key('notice-bell-count')), findsOneWidget);
    expect(
      t.widget<IconButton>(find.byKey(const Key('notice-bell'))).tooltip,
      '공지 1개',
    );
    await t.tap(find.byKey(const Key('notice-bell')));
    await t.pump();
    await t.pump(const Duration(milliseconds: 300));
    expect(find.byKey(const Key('notice-list')), findsOneWidget);
    expect(find.byKey(const Key('notice-list-count')), findsOneWidget);
    expect(find.byKey(ValueKey('notice-row-${hex(1)}')), findsOneWidget);
    expect(find.byKey(const Key('notice-unread-dot')), findsOneWidget);
    expect(find.text('오늘 오후 6시 서버 점검 안내'), findsOneWidget);
    expect(api.seen, [
      [hex(1)],
    ], reason: 'opening the list is seeing it');
    // Closed with a click outside; the count is gone.
    await t.tapAt(const Offset(40, 700));
    await t.pump();
    await t.pump(const Duration(milliseconds: 300));
    expect(find.byKey(const Key('notice-list')), findsNothing);
    expect(find.byKey(const Key('notice-bell-count')), findsNothing);
    expect(
      t.widget<IconButton>(find.byKey(const Key('notice-bell'))).tooltip,
      '공지',
    );
    // Opened again: still listed, without the dot.
    await t.tap(find.byKey(const Key('notice-bell')));
    await t.pump();
    await t.pump(const Duration(milliseconds: 300));
    expect(find.byKey(ValueKey('notice-row-${hex(1)}')), findsOneWidget);
    expect(find.byKey(const Key('notice-unread-dot')), findsNothing);
    await t.tapAt(const Offset(40, 700));
    await t.pump(const Duration(milliseconds: 300));
  });

  noticeTest('launcher: the pointer on a banner keeps it up; with no notices the list says so', (
    t,
  ) async {
    final api = FakeNotices()..list = [live(1)];
    final center = await started(t, api);
    await launcher(t, center);
    await settle(t);
    final mouse = await t.createGesture(kind: PointerDeviceKind.mouse);
    await mouse.addPointer(location: Offset.zero);
    addTearDown(mouse.removePointer);
    await mouse.moveTo(t.getCenter(find.byKey(const Key('notice-banner'))));
    await t.pump(const Duration(seconds: 30));
    expect(find.byKey(const Key('notice-banner')), findsOneWidget);
    await mouse.moveTo(const Offset(40, 700));
    await t.pump(const Duration(seconds: 3));
    await settle(t);
    expect(find.byKey(const Key('notice-banner')), findsNothing);
    // Withdrawn meanwhile: the list is empty and says so.
    api.list = [];
    center.signal();
    await t.pump();
    await t.tap(find.byKey(const Key('notice-bell')));
    await t.pump();
    await t.pump(const Duration(milliseconds: 300));
    expect(find.byKey(const Key('notice-list-empty')), findsOneWidget);
    expect(find.text('받은 공지가 없어요.'), findsOneWidget);
    await t.tapAt(const Offset(40, 700));
    await t.pump(const Duration(milliseconds: 300));
  });

  noticeTest('launcher: 점검 예고 shows its time and stays longer; in the tray the banner waits', (
    t,
  ) async {
    final api = FakeNotices()
      ..list = [
        live(
          1,
          maintenance: true,
          scheduled: const Duration(minutes: 4, seconds: 50),
        ),
      ];
    final center = await started(t, api);
    await launcher(t, center, active: false);
    await settle(t);
    // The window rests in the tray: nothing counts down.
    await t.pump(const Duration(minutes: 2));
    expect(find.byKey(const Key('notice-banner')), findsOneWidget);
    // A hidden app draws no frame: the banner itself knows it is unseen.
    await launcher(t, center);
    await settle(t);
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    await t.pump(const Duration(minutes: 2));
    expect(center.banner, isNotNull, reason: 'hidden, its time does not run');
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await settle(t);
    expect(
      t.widget<Text>(find.byKey(const Key('notice-kind'))).data,
      '점검 예고 · 5분 뒤',
    );
    await t.pump(const Duration(seconds: 25));
    expect(
      find.byKey(const Key('notice-banner')),
      findsOneWidget,
      reason: 'an ordinary notice would be gone by now',
    );
    await t.pump(const Duration(seconds: 10));
    await settle(t);
    expect(find.byKey(const Key('notice-banner')), findsNothing);
    expect(find.byKey(const Key('notice-bell-count')), findsOneWidget);
  });

  noticeTest('launcher: no bell and no banner without notices (the demo, an older server)', (
    t,
  ) async {
    await launcher(t, null);
    expect(find.byKey(const Key('notice-bell')), findsNothing);
    expect(find.byKey(const Key('notice-banner')), findsNothing);
    final api = FakeNotices()..list = [live(1)];
    final center = await started(t, api);
    await launcher(t, center, demo: true);
    await settle(t);
    expect(find.byKey(const Key('notice-bell')), findsNothing);
    expect(find.byKey(const Key('notice-banner')), findsNothing);
  });

  noticeTest('launcher: a narrow window and large text hold a long notice without overflow', (
    t,
  ) async {
    final api = FakeNotices()
      ..list = [
        live(
          1,
          title: '가' * 60,
          body: List.filled(12, '아주 긴 줄을 가진 공지 내용이에요 ' * 3).join('\n'),
        ),
      ];
    final center = await started(t, api);
    for (final (size, scale) in const [
      (Size(360, 640), 1.0),
      (Size(560, 700), 2.0),
      (Size(1280, 800), 1.5),
    ]) {
      await launcher(t, center, size: size, scale: scale);
      await settle(t);
      expect(t.takeException(), isNull, reason: '$size x$scale');
      final banner = t.getRect(find.byKey(const Key('notice-banner')));
      expect(banner.left, greaterThanOrEqualTo(0));
      expect(banner.right, lessThanOrEqualTo(size.width));
      expect(find.byKey(const Key('notice-bell')), findsOneWidget);
      await t.tap(find.byKey(const Key('notice-bell')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 300));
      expect(t.takeException(), isNull, reason: 'list $size x$scale');
      final list = t.getRect(find.byKey(const Key('anchored-panel')));
      expect(list.left, greaterThanOrEqualTo(0));
      expect(list.right, lessThanOrEqualTo(size.width));
      expect(list.bottom, lessThanOrEqualTo(size.height));
      await t.tapAt(Offset(4, size.height - 4));
      await t.pump(const Duration(milliseconds: 300));
      // Raise it again for the next size.
      api.list = [live(size.width.toInt(), title: '가' * 60)];
      center.signal();
      await settle(t);
    }
  });

  noticeTest('stream: the banner is in the top-right corner, and the keyboard and every click outside it stay with the remote view', (
    t,
  ) async {
    final api = FakeNotices();
    final center = await started(t, api);
    final keys = <LogicalKeyboardKey>[], clicks = <Offset>[];
    await stream(t, center, keys, clicks);
    final remote = FocusManager.instance.primaryFocus;
    expect(remote?.debugLabel, 'remote-view');
    api.list = [live(1)];
    center.signal();
    await settle(t);
    final banner = t.getRect(find.byKey(const Key('notice-banner')));
    expect(banner.right, 1280 - 16);
    expect(banner.top, 44);
    // Above every page there is no Material: the banner brings its own text
    // defaults (without them Flutter underlines text in yellow).
    for (final text in t.widgetList<RichText>(
      find.descendant(
        of: find.byKey(const Key('notice-banner')),
        matching: find.byType(RichText),
      ),
    )) {
      expect(text.text.style?.decoration, isNot(TextDecoration.underline));
      if (text.text.style?.fontFamily == 'MaterialIcons') continue; // an icon
      expect(text.text.style?.fontFamily, 'Pretendard');
    }
    expect(
      DefaultTextStyle.of(
        t.element(find.byKey(const Key('notice-title'))),
      ).style.decoration,
      isNot(TextDecoration.underline),
    );
    expect(banner.width, lessThanOrEqualTo(380));
    // The keyboard never left the remote view.
    expect(FocusManager.instance.primaryFocus, same(remote));
    await t.sendKeyEvent(LogicalKeyboardKey.keyA);
    await t.sendKeyEvent(LogicalKeyboardKey.enter);
    await t.sendKeyEvent(LogicalKeyboardKey.escape);
    expect(keys, [
      LogicalKeyboardKey.keyA,
      LogicalKeyboardKey.enter,
      LogicalKeyboardKey.escape,
    ]);
    expect(find.byKey(const Key('notice-banner')), findsOneWidget, reason: 'no key closes it or is taken by it');
    // Clicks beside, below and left of the card reach the remote view.
    for (final point in [
      const Offset(100, 100),
      Offset(banner.left - 6, banner.center.dy),
      Offset(banner.center.dx, banner.bottom + 6),
      Offset(banner.center.dx, banner.top - 6),
    ]) {
      await t.tapAt(point);
    }
    expect(clicks.length, 4);
    // A click on the card closes it and is not passed on.
    await t.tapAt(banner.center);
    await settle(t);
    expect(clicks.length, 4);
    expect(find.byKey(const Key('notice-banner')), findsNothing);
    expect(api.seen, [
      [hex(1)],
    ]);
    expect(FocusManager.instance.primaryFocus, same(remote));
    // Where the card was, clicks reach the remote view again.
    await t.tapAt(banner.center);
    expect(clicks.length, 5);
  });

  noticeTest('stream: over a dark surface of its own; full screen and 모든 모니터 keep it on the primary monitor', (
    t,
  ) async {
    final api = FakeNotices()..list = [live(1)];
    final center = await started(t, api);
    final keys = <LogicalKeyboardKey>[], clicks = <Offset>[];
    // Full screen on one monitor.
    await stream(t, center, keys, clicks, size: const Size(1920, 1080));
    await settle(t);
    var banner = t.getRect(find.byKey(const Key('notice-banner')));
    expect(banner.right, 1920 - 16);
    expect(banner.top, 44);
    final box = t.widget<DecoratedBox>(
      find.descendant(
        of: find.byKey(const Key('notice-banner')),
        matching: find.byType(DecoratedBox),
      ).first,
    );
    final surface = (box.decoration as BoxDecoration).color!;
    expect(surface.a, greaterThan(.9), reason: 'readable over any picture');
    expect(surface.computeLuminance(), lessThan(.05));
    // 모든 모니터: one window over two monitors, the primary on the left.
    await stream(
      t,
      center,
      keys,
      clicks,
      size: const Size(3840, 1080),
      primary: const Rect.fromLTWH(0, 0, 1920, 1080),
    );
    await settle(t);
    banner = t.getRect(find.byKey(const Key('notice-banner')));
    expect(banner.right, 1920 - 16, reason: 'not on the seam, not on the other monitor');
    expect(banner.left, greaterThan(1920 - 16 - 381));
    // The primary on the right, and lower than the other.
    await stream(
      t,
      center,
      keys,
      clicks,
      size: const Size(3840, 1200),
      primary: const Rect.fromLTWH(1920, 120, 1920, 1080),
    );
    await settle(t);
    banner = t.getRect(find.byKey(const Key('notice-banner')));
    expect(banner.right, 3840 - 16);
    expect(banner.top, 120 + 44);
    // A small window: never wider than the window.
    await stream(t, center, keys, clicks, size: const Size(320, 240));
    await settle(t);
    banner = t.getRect(find.byKey(const Key('notice-banner')));
    expect(banner.left, greaterThanOrEqualTo(16));
    expect(banner.right, 320 - 16);
    expect(t.takeException(), isNull);
  });

  noticeTest('a session window reads its own notices and raises a fresh one over its content', (
    t,
  ) async {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    mockPresentation();
    messenger.setMockMethodCallHandler(
      SessionWindows.channel,
      (_) async => null,
    );
    addTearDown(
      () => messenger.setMockMethodCallHandler(SessionWindows.channel, null),
    );
    final api = _SessionApi();
    final now = DateTime.now();
    BrokerNotice at(int n, Duration sent) => BrokerNotice.parse({
      ...wire(n, title: n == 2 ? '방금 온 공지' : '어제 온 공지'),
      'sentAt': now.add(sent).toUtc().toIso8601String(),
      'expiresAt': now.add(const Duration(hours: 20)).toUtc().toIso8601String(),
    })!;
    api.notices_.list = [
      at(2, const Duration(minutes: -1)),
      at(1, const Duration(hours: -3)),
    ];
    await t.pumpWidget(
      MaterialApp(
        builder: PrimaryRegionScope.builder,
        home: SessionWindow(
          launch: SessionWindowLaunch.fromMap(launch().toMap()),
          api: api,
          guard: _Guard(),
        ),
      ),
    );
    await settle(t);
    expect(find.text('원격 창 닫기'), findsOneWidget);
    expect(api.notices_.tokens, {'fixture-token'}, reason: 'its own read');
    expect(find.byKey(const Key('notice-banner')), findsOneWidget);
    expect(find.text('방금 온 공지'), findsOneWidget);
    expect(find.text('어제 온 공지'), findsNothing, reason: 'the launcher raised that one');
    await t.tap(find.byKey(const Key('notice-banner')));
    await settle(t);
    expect(api.notices_.seen, [
      [hex(2)],
    ]);
    expect(find.byKey(const Key('notice-banner')), findsNothing);
    // The window's own controls still work under where the banner was.
    expect(find.text('원격 창 닫기'), findsOneWidget);
  });

  // ---- 지우기 in the list.
  Future<void> openList(WidgetTester t) async {
    await t.tap(find.byKey(const Key('notice-bell')));
    await settle(t);
    expect(find.byKey(const Key('notice-list')), findsOneWidget);
  }

  Finder removeOf(int n) => find.byKey(ValueKey('notice-remove-${hex(n)}'));
  // The one question before a removal.
  Future<void> answer(WidgetTester t, {bool accept = true}) async {
    await t.pumpAndSettle();
    expect(find.byKey(const Key('notice-remove-confirm')), findsOneWidget);
    await t.tap(find.byKey(Key(accept ? 'notice-remove-accept' : 'notice-remove-cancel')));
    await t.pump();
  }

  noticeTest('list: each row has a small 지우기 and the header 모두 지우기; a row goes after one question', (
    t,
  ) async {
    final api = FakeRemovableNotices()
      ..list = [
        live(3, title: '셋째 공지'),
        live(2, title: '둘째 공지', sent: const Duration(minutes: -5)),
        live(1, title: '첫 공지', sent: const Duration(hours: -1)),
      ];
    final center = await started(t, api);
    await launcher(t, center);
    await settle(t);
    await openList(t);
    final panel = t.getRect(find.byKey(const Key('anchored-panel')));
    for (final n in [1, 2, 3]) {
      final button = t.widget<IconButton>(removeOf(n));
      expect(button.tooltip, '지우기');
      final box = t.getRect(removeOf(n));
      expect(box.size, const Size(28, 28), reason: 'small, for a pointer');
      final row = t.getRect(find.byKey(ValueKey('notice-row-${hex(n)}')));
      expect(box.left, greaterThanOrEqualTo(row.right), reason: 'beside the text, not over it');
      expect(box.right, lessThanOrEqualTo(panel.right));
      expect(box.top - row.top, lessThan(12), reason: 'at the row\'s top-right');
    }
    final all = find.byKey(const Key('notice-remove-all'));
    expect(t.widget<TextButton>(all).child, isA<Text>());
    expect(find.descendant(of: all, matching: find.text('모두 지우기')), findsOneWidget);
    expect(t.getRect(all).right, greaterThan(panel.right - 24), reason: 'at the right of the header');
    expect(t.getRect(all).center.dy, closeTo(t.getRect(find.byKey(const Key('notice-list-count'))).center.dy, 3));
    expect(t.widget<Text>(find.byKey(const Key('notice-list-count'))).data, '3');
    expect(find.textContaining('삭제'), findsNothing, reason: 'the word is 지우기');
    // One row: the question names it; 취소 keeps it and sends nothing.
    await t.tap(removeOf(2));
    await t.pumpAndSettle();
    expect(find.text('이 공지를 지울까요?'), findsOneWidget);
    expect(
      find.descendant(of: find.byKey(const Key('notice-remove-confirm')), matching: find.text('둘째 공지')),
      findsOneWidget,
    );
    await answer(t, accept: false);
    await t.pumpAndSettle();
    expect(find.text('둘째 공지'), findsOneWidget);
    expect(api.removed, isEmpty, reason: 'cancel sends nothing');
    await t.tap(removeOf(2));
    await answer(t);
    await t.pumpAndSettle();
    expect(find.text('둘째 공지'), findsNothing, reason: 'gone once 지우기 is pressed');
    expect(find.byKey(const Key('notice-list')), findsOneWidget, reason: 'the list stays open');
    await t.pumpAndSettle();
    expect(find.byKey(const Key('notice-remove-confirm')), findsNothing);
    expect(find.byKey(const Key('notice-list')), findsOneWidget, reason: 'the list stays open');
    expect(t.widget<Text>(find.byKey(const Key('notice-list-count'))).data, '2');
    expect(api.removed, [
      [hex(2)],
    ]);
    expect(find.text('첫 공지'), findsOneWidget);
    expect(find.text('셋째 공지'), findsOneWidget);
    // The rest, one by one: after the last the list says it is empty and the controls are gone.
    await t.tap(removeOf(3));
    await answer(t);
    await t.pumpAndSettle();
    await t.tap(removeOf(1));
    await answer(t);
    await settle(t);
    expect(find.text('받은 공지가 없어요.'), findsOneWidget);
    expect(find.byKey(const Key('notice-remove-all')), findsNothing);
    expect(find.byKey(const Key('notice-list-count')), findsNothing);
    expect(api.removed.length, 3);
    expect(t.takeException(), isNull);
    await t.tapAt(const Offset(40, 700));
    await settle(t);
    expect(find.byKey(const Key('notice-bell-count')), findsNothing);
  });

  noticeTest('list: 모두 지우기 asks once with the count, then empties it in one request; a failed send still leaves it empty and is sent again', (
    t,
  ) async {
    final api = FakeRemovableNotices()
      ..list = [live(2, title: '둘째 공지'), live(1, title: '첫 공지')]
      ..removeError = const BrokerException('network_error');
    final center = await started(t, api);
    await launcher(t, center);
    await settle(t);
    await openList(t);
    await t.tap(find.byKey(const Key('notice-remove-all')));
    await t.pumpAndSettle();
    expect(find.text('공지 2개를 모두 지울까요?'), findsOneWidget);
    await answer(t, accept: false);
    await t.pumpAndSettle();
    expect(find.text('둘째 공지'), findsOneWidget);
    expect(api.removed, isEmpty, reason: 'cancel sends nothing');
    await t.tap(find.byKey(const Key('notice-remove-all')));
    await answer(t);
    await settle(t);
    expect(find.text('받은 공지가 없어요.'), findsOneWidget);
    expect(find.byKey(const Key('notice-remove-confirm')), findsNothing);
    expect(api.removed, [
      [hex(2), hex(1)],
    ]);
    // The send failed and the broker still lists both: they stay gone here,
    // no error is shown for a tidy-up, and the next read sends it again.
    expect(find.textContaining('못'), findsNothing);
    api.removeError = null;
    center.signal();
    await settle(t);
    expect(find.text('받은 공지가 없어요.'), findsOneWidget);
    expect(api.removed.length, 2);
    expect(api.list, isEmpty);
    await t.tapAt(const Offset(40, 700));
    await settle(t);
  });

  noticeTest('list: the bell count follows a removal made on another device', (
    t,
  ) async {
    final api = FakeRemovableNotices()
      ..list = [live(2, title: '둘째 공지'), live(1, title: '첫 공지')];
    final center = await started(t, api);
    await launcher(t, center);
    await settle(t);
    expect(
      find.descendant(
        of: find.byKey(const Key('notice-bell-count')),
        matching: find.text('2'),
      ),
      findsOneWidget,
    );
    // Removed on the phone: the broker's next answer has one left.
    api.list = [api.list.first];
    center.signal();
    await settle(t);
    expect(
      find.descendant(
        of: find.byKey(const Key('notice-bell-count')),
        matching: find.text('1'),
      ),
      findsOneWidget,
    );
    expect(find.byKey(const Key('notice-banner')), findsOneWidget);
    expect(find.text('첫 공지'), findsNothing, reason: 'its banner went too');
  });

  noticeTest('list with an older broker (server 1.0.45): no 지우기 and no 모두 지우기 at all', (
    t,
  ) async {
    // The answer does not offer it, whatever this client could send.
    for (final api in [
      FakeRemovableNotices(offered: false),
      FakeNotices(),
    ]) {
      api.list = [live(2, title: '둘째 공지'), live(1, title: '첫 공지')];
      final center = await started(t, api);
      await launcher(t, center);
      await settle(t);
      await openList(t);
      expect(find.text('둘째 공지'), findsOneWidget);
      expect(find.byTooltip('지우기'), findsNothing);
      expect(removeOf(1), findsNothing);
      expect(find.byKey(const Key('notice-remove-all')), findsNothing);
      expect(find.text('모두 지우기'), findsNothing);
      // The header keeps its plain shape.
      expect(t.widget<Text>(find.byKey(const Key('notice-list-count'))).data, '2');
      await t.tapAt(const Offset(40, 700));
      await settle(t);
      await t.pumpWidget(const SizedBox.shrink());
    }
  });

  noticeTest('list: the controls fit a narrow window and large text, and the banner has none', (
    t,
  ) async {
    final api = FakeRemovableNotices()
      ..list = [
        live(
          2,
          title: '가' * 60,
          body: List.filled(6, '아주 긴 줄을 가진 공지 내용이에요 ' * 3).join('\n'),
        ),
        live(1, title: '첫 공지', maintenance: true, scheduled: const Duration(hours: 2)),
      ];
    final center = await started(t, api);
    for (final (size, scale) in const [
      (Size(360, 640), 1.0),
      (Size(560, 700), 2.0),
      (Size(1280, 800), 1.0),
    ]) {
      await launcher(t, center, size: size, scale: scale);
      await settle(t);
      // The banner is as it was: closing it means seen, never removed. (It
      // is up until the list is opened the first time.)
      if (size.width == 360) {
        expect(find.byKey(const Key('notice-banner')), findsOneWidget);
      }
      expect(
        find.descendant(
          of: find.byKey(const Key('notice-banner')),
          matching: find.byTooltip('지우기'),
        ),
        findsNothing,
      );
      await openList(t);
      expect(t.takeException(), isNull, reason: '$size x$scale');
      final panel = t.getRect(find.byKey(const Key('anchored-panel')));
      // The long first row may fill the list: the second is built when scrolled to.
      expect(removeOf(2), findsOneWidget);
      for (final finder in [
        if (removeOf(1).evaluate().isNotEmpty) removeOf(1),
        removeOf(2),
        find.byKey(const Key('notice-remove-all')),
      ]) {
        final box = t.getRect(finder);
        expect(box.left, greaterThanOrEqualTo(panel.left), reason: '$size x$scale');
        expect(box.right, lessThanOrEqualTo(panel.right), reason: '$size x$scale');
      }
      await t.tapAt(Offset(4, size.height - 4));
      await settle(t);
    }
    expect(api.removed, isEmpty, reason: 'looking removes nothing');
    expect(api.seen, isNotEmpty);
  });
}

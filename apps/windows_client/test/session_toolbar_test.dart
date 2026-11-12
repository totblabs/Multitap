import 'dart:async';
import 'dart:convert';
import 'dart:ui';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/remote_viewport.dart';
import 'package:multitap_client/ui/session_panels.dart' show SessionKeycaps;
import 'package:multitap_client/ui/session_toolbar.dart';
import 'package:multitap_client/ui/primary_region.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;

const _bar = Key('session-command-bar');
const _handle = Key('expand-session-toolbar');

/// The folded handle's painted tongue: its size is the handle's size.
const _tab = Key('session-tab');

/// The resting handle's content (a dot and a chevron); absent while the
/// handle shows the latency under the pointer or a status tab.
const _grip = Key('session-handle-grip');

/// The same handle under the open bar: it folds it.
const _fold = Key('session-toolbar-fold');
const _overflow = Key('session-toolbar-overflow');
const _pin = Key('pin-session-toolbar');

// SessionToolbarState's own sizes (lib/ui/session_toolbar.dart: _restSize,
// _lipSize, _tabHeight, _square and _caption by SessionChrome.button).
const _restSize = Size(74, 15);
const _lipSize = Size(72, 14);
const _tabHeight = 25.0;
const _disconnectSize = Size(38, SessionChrome.button);
const _windowButtonSize = Size(34, SessionChrome.button);

SessionToolbarMenu _menu(
  String label, {
  IconData icon = Icons.desktop_windows_outlined,
  List<SessionMenuItem>? items,
  SessionMenuPresenter? presenter,
}) => SessionToolbarMenu(
  label: label,
  icon: icon,
  onInteract: () {},
  presenter: presenter,
  items: items ?? [SessionMenuItem('$label 현재 설정', onSelected: () {})],
);

/// The bar's own state: 바 열어 두기 has no button on the bar (v2), so a
/// bar-only test keeps it open as the session's 설정 panel does.
SessionToolbarState _state(WidgetTester tester) =>
    tester.state<SessionToolbarState>(find.byType(SessionToolbar));

/// A bar whose folded handle is the subject: re-pumped with other values it
/// keeps its state (and its latency timer).
Widget _statusBar({
  double? latencyMs,
  bool connected = true,
  bool gameMouse = false,
  bool resuming = false,
}) => MaterialApp(
  home: Scaffold(
    body: SessionToolbar(
      title: 'TESTER',
      modeLabel: '스트리밍',
      connected: connected,
      latencyMs: latencyMs,
      gameMouse: gameMouse,
      resuming: resuming,
      onInteract: () {},
      actions: [_menu('화면')],
    ),
  ),
);

/// A row of a session panel (its key is on the SessionPanelRow).
Finder _panelRow(String key) => find.byKey(Key(key));

/// Puts a mouse over the resting grip, which grows into the latency tab.
Future<TestGesture> hoverHandle(WidgetTester tester) async {
  final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
  await mouse.addPointer(location: Offset.zero);
  await mouse.moveTo(tester.getCenter(find.byKey(_handle)));
  await tester.pump();
  return mouse;
}

/// Opens the bar from its resting handle (a click; hover never opens it).
Future<void> openBar(WidgetTester tester) async {
  if (find.byKey(_bar).evaluate().isEmpty) {
    await tester.tap(find.byKey(_handle));
    await tester.pump();
  }
  expect(find.byKey(_bar), findsOneWidget);
}

void main() {
  testWidgets(
    'commands follow the shared order, then the window buttons, then disconnect',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1440, 900);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetPhysicalSize);
      final anchors = <Rect>[];
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              title: 'Guide ordering',
              connected: true,
              modeLabel: '스트리밍',
              onInteract: () {},
              onMinimize: () {},
              onRestore: () {},
              onDisconnect: () {},
              actions: [
                // Deliberately first in the input list: the bar orders its
                // commands by the shared vocabulary (SessionTool).
                SessionToolbarMenu.command(
                  key: const Key('ordered-guide'),
                  label: '사용안내',
                  icon: Icons.help_outline_rounded,
                  rightGroup: true,
                  onInteract: () {},
                  onPressed: () {},
                  onPressedAt: (anchor) async {
                    anchors.add(anchor);
                    return false;
                  },
                ),
                _menu('화면'),
              ],
            ),
          ),
        ),
      );
      await openBar(tester);
      await tester.pump();
      final guide = find.byKey(const Key('ordered-guide'));
      double left(Finder finder) => tester.getTopLeft(finder).dx;
      // chip | 화면 · 사용안내 | — ❐ | 연결 끊기 (v2): every command sits
      // before the window's own buttons, the danger action last.
      expect(
        left(find.byKey(const Key('session-quality'))),
        lessThan(left(find.byTooltip('화면'))),
      );
      expect(left(find.byTooltip('화면')), lessThan(left(guide)));
      expect(
        left(guide),
        lessThan(left(find.byKey(const Key('session-minimize')))),
      );
      expect(
        left(find.byKey(const Key('session-minimize'))),
        lessThan(left(find.byKey(const Key('session-restore')))),
      );
      expect(
        left(find.byKey(const Key('session-restore'))),
        lessThan(left(find.byKey(const Key('session-disconnect')))),
      );
      // 바 열어 두기 is a switch in the session's 설정 panel (or a row of
      // the narrow 메뉴), never a button of the wide bar.
      expect(find.byKey(_pin), findsNothing);
      expect(find.byKey(const Key('session-fullscreen')), findsNothing);
      // Every command opens something under itself: no chevron on each.
      expect(
        find.descendant(
          of: find.byKey(_bar),
          matching: find.byIcon(Icons.expand_more_rounded),
        ),
        findsNothing,
      );
      await tester.tap(guide);
      await tester.pump();
      expect(anchors, hasLength(1));
      // Its panel drops from the bar's bottom edge.
      expect(
        anchors.single.bottom,
        closeTo(tester.getRect(find.byKey(_bar)).bottom, .01),
      );
      expect(find.byKey(_bar), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets('scaled anchored command uses the visible button corners', (
    tester,
  ) async {
    Rect? presented;
    const key = Key('scaled-toolbar-command');
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: Scaffold(
          body: Padding(
            padding: const EdgeInsets.only(left: 80, top: 40),
            child: Align(
              alignment: Alignment.topLeft,
              child: Transform.scale(
                scale: .5,
                alignment: Alignment.topLeft,
                child: SessionToolbarMenu.command(
                  key: key,
                  label: '설정',
                  icon: Icons.tune_rounded,
                  denseLabel: true,
                  commandBar: true,
                  onInteract: () {},
                  onPressed: () {},
                  onPressedAt: (anchor) async {
                    presented = anchor;
                    return false;
                  },
                ),
              ),
            ),
          ),
        ),
      ),
    );
    final target = find.byKey(key);
    final visible = Rect.fromPoints(
      tester.getTopLeft(target),
      tester.getBottomRight(target),
    );
    await tester.tapAt(visible.center);
    await tester.pump();
    expect(presented, isNotNull);
    expect(presented!.left, closeTo(visible.left, .01));
    expect(presented!.top, closeTo(visible.top, .01));
    expect(presented!.width, closeTo(visible.width, .01));
    expect(
      presented!.height,
      closeTo(visible.height + SessionChrome.belowButton, .01),
    );
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets('session menu converts window anchor into its nested overlay', (
    tester,
  ) async {
    late BuildContext ownedContext;
    final popups = SessionPopups();
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: Scaffold(
          body: Padding(
            padding: const EdgeInsets.fromLTRB(180, 100, 80, 80),
            child: HeroControllerScope.none(
              child: Navigator(
                onGenerateRoute: (_) => MaterialPageRoute<void>(
                  builder: (context) => Material(
                    child: Builder(
                      builder: (context) {
                        ownedContext = context;
                        return const SizedBox.expand();
                      },
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    );
    final overlay =
        Overlay.of(ownedContext).context.findRenderObject()! as RenderBox;
    final origin = overlay.localToGlobal(Offset.zero);
    final anchor = (origin + const Offset(20, 20)) & const Size(40, 36);
    final chosen = showSessionMenu(ownedContext, anchor, [
      SessionMenuItem(
        'Owned nested item',
        key: const Key('owned-nested-item'),
        onSelected: () {},
      ),
    ], popups: popups);
    await tester.pumpAndSettle();
    final item = find.byKey(const Key('owned-nested-item'));
    final painted = Rect.fromPoints(
      tester.getTopLeft(item),
      tester.getBottomRight(item),
    );
    expect(painted.left, closeTo(anchor.left, .01));
    expect(painted.top, greaterThanOrEqualTo(anchor.bottom + 6));
    expect(painted.top, lessThan(anchor.bottom + 30));
    expect(popups.isEmpty, isFalse);
    popups.closeAll();
    await tester.pumpAndSettle();
    expect(await chosen, isNull);
    expect(popups.isEmpty, isTrue);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets(
    'labelled Windows combo and menu stay in the primary region at DPR 2',
    (tester) async {
      tester.view.devicePixelRatio = 2;
      tester.view.physicalSize = const Size(2560, 1440);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      const region = Rect.fromLTWH(640, 0, 640, 720);
      late ValueNotifier<Rect?> regionValue;
      await tester.pumpWidget(
        MaterialApp(
          builder: PrimaryRegionScope.builder,
          theme: buildAppTheme(),
          home: Builder(
            builder: (context) {
              regionValue = PrimaryRegionScope.maybeOf(context)!;
              return Stack(
                children: [
                  Positioned.fromRect(
                    rect: region,
                    child: SessionToolbar(
                      title: 'SYNTHETIC VM',
                      connected: true,
                      latencyMs: 12,
                      onInteract: () {},
                      actions: [
                        _menu('화면'),
                        _menu('공유 폴더'),
                        _menu('USB'),
                        _menu('설정'),
                      ],
                    ),
                  ),
                ],
              );
            },
          ),
        ),
      );
      regionValue.value = region;
      await tester.pump();
      await openBar(tester);
      final combo = find.byWidgetPredicate(
        (widget) => widget is SessionToolbarMenu && widget.label == '화면',
      );
      expect(combo, findsOneWidget);
      final comboRect = Rect.fromPoints(
        tester.getTopLeft(combo),
        tester.getBottomRight(combo),
      );
      expect(region.contains(comboRect.center), isTrue);
      await tester.tapAt(comboRect.center, kind: PointerDeviceKind.mouse);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));
      final item = find.text('화면 현재 설정');
      expect(item, findsOneWidget);
      final rect = Rect.fromPoints(
        tester.getTopLeft(item),
        tester.getBottomRight(item),
      );
      expect(rect.left, greaterThanOrEqualTo(region.left));
      expect(rect.right, lessThanOrEqualTo(region.right));
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'The resting handle shows a hand cursor and the open bar a grab cursor',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              title: 'TESTER',
              connected: true,
              onInteract: () {},
              actions: const [],
            ),
          ),
        ),
      );
      await tester.pump();
      MouseCursor cursorAbove(Key key) => tester
          .widgetList<MouseRegion>(
            find.ancestor(
              of: find.byKey(key),
              matching: find.byType(MouseRegion),
            ),
          )
          .map((region) => region.cursor)
          .firstWhere((cursor) => cursor != MouseCursor.defer);
      expect(cursorAbove(_handle), SystemMouseCursors.click);
      await openBar(tester);
      expect(cursorAbove(_bar), SystemMouseCursors.grab);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Missing, zero, negative or nonfinite latency never becomes a fabricated RTT',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      for (final latency in [null, 0.0, -2.0, double.nan, double.infinity]) {
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: SessionToolbar(
                key: UniqueKey(),
                title: 'TESTER',
                modeLabel: '스트리밍',
                connected: true,
                latencyMs: latency,
                onInteract: () {},
                actions: const [],
              ),
            ),
          ),
        );
        await tester.pump();
        // The handle never invents a number, at rest or under the pointer.
        expect(find.byKey(_handle), findsOneWidget);
        expect(find.byKey(_grip), findsOneWidget);
        expect(find.textContaining('ms'), findsNothing);
        final mouse = await hoverHandle(tester);
        expect(find.text('연결됨'), findsOneWidget);
        expect(find.textContaining('ms'), findsNothing);
        await mouse.removePointer();
        await tester.pump();
        // Nor does an unmeasured latency ever count as poor.
        expect(_state(tester).poorSustained, isFalse);
        expect(_state(tester).handleStatus, SessionHandleStatus.normal);
        await openBar(tester);
        expect(find.text('연결됨'), findsOneWidget);
        expect(find.textContaining('ms'), findsNothing);
        expect(find.text('좋음'), findsNothing);
        expect(find.textContaining('RTT'), findsNothing);
      }
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              key: UniqueKey(),
              title: 'TESTER',
              connected: true,
              latencyMs: .2,
              onInteract: () {},
              actions: const [],
            ),
          ),
        ),
      );
      await tester.pump();
      final mouse = await hoverHandle(tester);
      expect(find.text('<1 ms'), findsOneWidget);
      await mouse.removePointer();
      await tester.pump();
      await openBar(tester);
      expect(find.text('<1 ms'), findsOneWidget);
      // The word is only in the tooltip; the chip shows bars and the number.
      expect(find.text('좋음'), findsNothing);
      expect(find.byTooltip('연결 품질 · 좋음'), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets('The open bar keeps its width when the latency gains a digit', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(900, 600);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final widths = <double>[], controls = <double>[], folds = <double>[];
    for (final latency in [7.0, 70.0, 700.0]) {
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              key: UniqueKey(),
              title: 'TESTER',
              connected: true,
              latencyMs: latency,
              onInteract: () {},
              onDisconnect: () {},
              actions: const [],
            ),
          ),
        ),
      );
      await tester.pump();
      await openBar(tester);
      widths.add(tester.getSize(find.byKey(_bar)).width);
      controls.add(
        tester.getTopLeft(find.byKey(const Key('session-disconnect'))).dx -
            tester.getTopLeft(find.byKey(_bar)).dx,
      );
      folds.add(
        tester.getTopLeft(find.byKey(_fold)).dx -
            tester.getTopLeft(find.byKey(_bar)).dx,
      );
    }
    expect(widths[1], widths[0]);
    expect(widths[2], widths[0]);
    // The controls beside the number stay put too, and the handle under it.
    expect(controls[1], controls[0]);
    expect(controls[2], controls[0]);
    expect(folds[1], folds[0]);
    expect(folds[2], folds[0]);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets(
    'While waiting the bar shows a waiting ring, not weak-signal bars, and a quiet disconnect',
    (tester) async {
      tester.view.physicalSize = const Size(1440, 900);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      Future<Color?> disconnectColor(bool connected) async {
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: SessionToolbar(
                key: UniqueKey(),
                title: 'TESTER',
                connected: connected,
                onInteract: () {},
                onDisconnect: () {},
                actions: const [],
              ),
            ),
          ),
        );
        await tester.pump();
        await openBar(tester);
        final button = tester.widget<ButtonStyleButton>(
          find.byKey(const Key('session-disconnect')),
        );
        return button.style?.foregroundColor?.resolve({});
      }

      final waiting = await disconnectColor(false);
      expect(
        find.byKey(const Key('session-waiting-indicator')),
        findsOneWidget,
      );
      expect(find.text('연결 대기'), findsWidgets);
      final connected = await disconnectColor(true);
      expect(find.byKey(const Key('session-waiting-indicator')), findsNothing);
      expect(waiting, isNot(connected), reason: 'Red only once connected.');
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Collapsed by default to a small top-centre handle; a click opens the labelled bar and its handle folds it',
    (tester) async {
      tester.view.physicalSize = const Size(1440, 900);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      for (final latency in <double?>[12, null]) {
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: SessionToolbar(
                key: ValueKey(latency),
                title: 'TESTER',
                modeLabel: '스트리밍',
                latencyMs: latency,
                connected: true,
                onInteract: () {},
                onMinimize: () {},
                onRestore: () {},
                onDisconnect: () {},
                actions: [
                  for (final label in ['화면', '클립보드', '스트리밍 설정']) _menu(label),
                ],
              ),
            ),
          ),
        );
        await tester.pump();
        // At rest: a 74 × 15 tongue hanging from the top edge, centred, with
        // a dot in the latency colour and no text.
        expect(find.byKey(_bar), findsNothing);
        expect(find.byKey(_fold), findsNothing);
        final grip = tester.getRect(find.byKey(_tab));
        expect(grip.size, _restSize);
        expect(grip.top, 0);
        expect(grip.center.dx, closeTo(720, 1));
        expect(find.byKey(_grip), findsOneWidget);
        expect(find.textContaining('ms'), findsNothing);
        expect(
          find.byTooltip(
            'TESTER · 스트리밍 · ${latency != null ? '지연 약 12 ms' : '연결됨'} · 눌러서 툴바 열기',
          ),
          findsOneWidget,
        );
        // Under the pointer it grows in place into the latency tab: same
        // centre, flush with the top edge, wide enough for "999 ms" so the
        // number never moves it; moving within it changes nothing.
        final mouse = await hoverHandle(tester);
        final tab = tester.getRect(find.byKey(_tab));
        expect(tab.height, _tabHeight);
        expect(tab.width, greaterThanOrEqualTo(_restSize.width));
        expect(tab.top, 0);
        expect(tab.center.dx, closeTo(720, 1));
        expect(find.byKey(_grip), findsNothing);
        expect(find.text(latency != null ? '12 ms' : '연결됨'), findsOneWidget);
        await mouse.moveTo(tab.bottomLeft + const Offset(4, -3));
        await tester.pump();
        await mouse.moveTo(tab.topRight + const Offset(-4, 3));
        await tester.pump();
        expect(tester.getRect(find.byKey(_tab)), tab);
        expect(find.byKey(_bar), findsNothing, reason: 'Hover never opens.');
        await mouse.moveTo(const Offset(720, 400));
        await tester.pump();
        expect(tester.getRect(find.byKey(_tab)).size, _restSize);
        expect(find.byKey(_grip), findsOneWidget);
        await mouse.removePointer();
        // Idle time changes nothing: no timer opens or hides anything.
        await tester.pump(const Duration(seconds: 10));
        expect(find.byKey(_bar), findsNothing);
        expect(find.byKey(_handle), findsOneWidget);
        expect(tester.getRect(find.byKey(_tab)).size, _restSize);
        await tester.tap(find.byKey(_handle));
        await tester.pump();
        for (final label in ['화면', '클립보드', '스트리밍 설정']) {
          expect(find.text(label), findsOneWidget);
          expect(find.byTooltip(label), findsOneWidget);
          expect(
            tester
                .getSize(
                  find.descendant(
                    of: find.byTooltip(label),
                    matching: find.byType(TextButton),
                  ),
                )
                .height,
            SessionChrome.button,
          );
        }
        // Labelled commands carry no chevron of their own.
        expect(
          find.descendant(
            of: find.byKey(_bar),
            matching: find.byIcon(Icons.expand_more_rounded),
          ),
          findsNothing,
        );
        expect(find.text('TESTER'), findsNothing);
        expect(find.text(latency != null ? '12 ms' : '연결됨'), findsOneWidget);
        // The window's own buttons and 연결 끊기 are icons.
        expect(find.text('연결 끊기'), findsNothing);
        expect(find.byTooltip('연결 끊기'), findsOneWidget);
        expect(
          tester.getSize(find.byKey(const Key('session-disconnect'))),
          _disconnectSize,
        );
        expect(find.byTooltip('최소화'), findsOneWidget);
        expect(find.byTooltip('창 모드로'), findsOneWidget);
        expect(
          tester.getSize(find.byKey(const Key('session-minimize'))),
          _windowButtonSize,
        );
        expect(
          tester.getSize(find.byKey(const Key('session-restore'))),
          _windowButtonSize,
        );
        // Neither a pin nor the old full screen button is on the bar.
        expect(find.byKey(_pin), findsNothing);
        expect(find.byKey(const Key('session-fullscreen')), findsNothing);
        expect(find.byKey(_overflow), findsNothing);
        // The bar hangs from the top edge.
        final bar = tester.getRect(find.byKey(_bar));
        expect(bar.height, SessionChrome.barHeight);
        expect(bar.top, 0);
        expect(bar.center.dx, closeTo(720, 1));
        final logicalWidth =
            tester.view.physicalSize.width / tester.view.devicePixelRatio;
        expect(bar.left, greaterThanOrEqualTo(0));
        expect(bar.right, lessThanOrEqualTo(logicalWidth));
        // The same handle hangs under the bar's centre.
        final lip = tester.getRect(find.byKey(_fold));
        expect(lip.size, _lipSize);
        expect(lip.center.dx, closeTo(bar.center.dx, .01));
        expect(lip.top, closeTo(bar.bottom - 1, .01));
        expect(find.byTooltip('툴바 접기 (Esc)'), findsOneWidget);
        // It stays open while read, then the handle under it folds it.
        await tester.pump(const Duration(seconds: 10));
        expect(find.byKey(_bar), findsOneWidget);
        await tester.tap(find.byKey(_fold));
        await tester.pump();
        expect(find.byKey(_bar), findsNothing);
        expect(find.byKey(_fold), findsNothing);
        expect(find.byKey(_handle), findsOneWidget);
        expect(tester.takeException(), isNull);
      }
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'At rest the handle is a dot in the latency colour: green when good, and a fair latency never grows a tab',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      const chrome = SessionChrome.forDark(true);
      Color dotColor() {
        final dot = tester.widget<Container>(
          find.descendant(
            of: find.byKey(_grip),
            matching: find.byType(Container),
          ),
        );
        return (dot.decoration! as BoxDecoration).color!;
      }

      await tester.pumpWidget(_statusBar(latencyMs: 12));
      await tester.pump();
      final state = _state(tester);
      expect(tester.getSize(find.byKey(_tab)), _restSize);
      expect(find.byKey(_grip), findsOneWidget);
      expect(dotColor(), chrome.good);
      expect(find.textContaining('ms'), findsNothing);
      expect(state.handleStatus, SessionHandleStatus.normal);
      expect(state.poorSustained, isFalse);
      // Fair (40 ms up to 80 ms) only colours the dot, however long it lasts.
      for (final fair in [SessionChrome.goodMs, 55.0, 79.0]) {
        await tester.pumpWidget(_statusBar(latencyMs: fair));
        await tester.pump(SessionToolbar.poorHold + const Duration(seconds: 1));
        expect(tester.getSize(find.byKey(_tab)), _restSize, reason: '$fair');
        expect(find.byKey(_grip), findsOneWidget, reason: '$fair');
        expect(dotColor(), chrome.fair, reason: '$fair');
        expect(find.textContaining('ms'), findsNothing, reason: '$fair');
        expect(find.textContaining('느림'), findsNothing, reason: '$fair');
        expect(state.handleStatus, SessionHandleStatus.normal);
        expect(state.poorSustained, isFalse);
      }
      // Good again: green again.
      await tester.pumpWidget(_statusBar(latencyMs: 12));
      await tester.pump();
      expect(dotColor(), chrome.good);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'A poor latency shows on the handle only once it lasted, and goes only once better lasted',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      expect(SessionToolbar.poorHold, const Duration(seconds: 5));
      final slow = find.textContaining('느림');
      await tester.pumpWidget(_statusBar(latencyMs: 12));
      await tester.pump();
      final state = _state(tester);
      // A spike shorter than the hold never shows.
      await tester.pumpWidget(_statusBar(latencyMs: 142));
      await tester.pump(const Duration(seconds: 4));
      expect(state.poorSustained, isFalse);
      expect(slow, findsNothing);
      await tester.pumpWidget(_statusBar(latencyMs: 12));
      await tester.pump(SessionToolbar.poorHold);
      expect(state.poorSustained, isFalse);
      expect(state.handleStatus, SessionHandleStatus.normal);
      expect(slow, findsNothing);
      expect(find.byKey(_grip), findsOneWidget);
      // Poor for the whole hold (80 ms is poor already): the tab shows the
      // number. A second poor sample does not start the wait again.
      await tester.pumpWidget(_statusBar(latencyMs: 142));
      await tester.pump(const Duration(seconds: 4));
      expect(state.poorSustained, isFalse);
      expect(find.byKey(_grip), findsOneWidget);
      await tester.pumpWidget(_statusBar(latencyMs: SessionChrome.fairMs));
      await tester.pump(const Duration(seconds: 2));
      expect(state.poorSustained, isTrue);
      expect(state.handleStatus, SessionHandleStatus.poor);
      expect(find.text('80 ms · 느림'), findsOneWidget);
      expect(find.byKey(_grip), findsNothing);
      final tab = tester.getRect(find.byKey(_tab));
      expect(tab.height, _tabHeight);
      expect(tab.top, 0);
      expect(tab.center.dx, closeTo(450, 1));
      expect(find.byKey(_bar), findsNothing, reason: 'It never opens itself.');
      // The number follows the measurement.
      await tester.pumpWidget(_statusBar(latencyMs: 142));
      await tester.pump();
      expect(find.text('142 ms · 느림'), findsOneWidget);
      expect(tester.getRect(find.byKey(_tab)), tab);
      // A dip shorter than the hold never hides it.
      await tester.pumpWidget(_statusBar(latencyMs: 20));
      await tester.pump(const Duration(seconds: 4));
      expect(state.poorSustained, isTrue);
      // The tab keeps its place and reads the number as it is now.
      expect(slow, findsNothing);
      expect(find.text('20 ms'), findsOneWidget);
      expect(find.byKey(_grip), findsNothing);
      expect(tester.getRect(find.byKey(_tab)), tab);
      await tester.pumpWidget(_statusBar(latencyMs: 142));
      await tester.pump(SessionToolbar.poorHold);
      expect(state.poorSustained, isTrue);
      expect(find.text('142 ms · 느림'), findsOneWidget);
      // Better for the whole hold: back to the resting handle.
      await tester.pumpWidget(_statusBar(latencyMs: 20));
      await tester.pump(const Duration(seconds: 4));
      expect(state.poorSustained, isTrue);
      await tester.pump(const Duration(seconds: 2));
      expect(state.poorSustained, isFalse);
      expect(state.handleStatus, SessionHandleStatus.normal);
      expect(slow, findsNothing);
      expect(find.byKey(_grip), findsOneWidget);
      expect(tester.getSize(find.byKey(_tab)), _restSize);
      // Not connected clears it at once, and nothing stays pending.
      await tester.pumpWidget(_statusBar(latencyMs: 142));
      await tester.pump(SessionToolbar.poorHold);
      expect(state.poorSustained, isTrue);
      await tester.pumpWidget(_statusBar(latencyMs: 142, connected: false));
      expect(state.poorSustained, isFalse);
      expect(state.handleStatus, SessionHandleStatus.normal);
      expect(slow, findsNothing);
      expect(find.byKey(_grip), findsOneWidget);
      await tester.pump(SessionToolbar.poorHold * 2);
      expect(state.poorSustained, isFalse);
      // Connected again, the wait starts anew.
      await tester.pumpWidget(_statusBar(latencyMs: 142));
      await tester.pump(const Duration(seconds: 4));
      expect(state.poorSustained, isFalse);
      await tester.pump(const Duration(seconds: 2));
      expect(state.poorSustained, isTrue);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'While 마우스 잠금 is on the handle shows the keys that release it, before resuming and poor',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      void expectMouseLock() {
        expect(find.text('마우스 잠금'), findsOneWidget);
        expect(find.byIcon(Icons.lock_rounded), findsOneWidget);
        expect(find.byType(SessionKeycaps), findsOneWidget);
        expect(SessionKeycaps.mouseLock, ['Ctrl', 'Shift', 'Alt', 'Z']);
        for (final key in SessionKeycaps.mouseLock) {
          expect(
            find.descendant(
              of: find.byType(SessionKeycaps),
              matching: find.text(key),
            ),
            findsOneWidget,
            reason: key,
          );
        }
        expect(find.text('로 풀기'), findsOneWidget);
        expect(find.byKey(_grip), findsNothing);
        // A tab, shown without the pointer, inside the window.
        final tab = tester.getRect(find.byKey(_tab));
        expect(tab.height, _tabHeight);
        expect(tab.top, 0);
        expect(tab.center.dx, closeTo(450, 1));
        expect(tab.left, greaterThanOrEqualTo(0));
        expect(tab.right, lessThanOrEqualTo(900));
        expect(find.byKey(_bar), findsNothing);
      }

      // On its own, with a good connection.
      await tester.pumpWidget(_statusBar(latencyMs: 12, gameMouse: true));
      await tester.pump();
      final state = _state(tester);
      expect(state.handleStatus, SessionHandleStatus.mouseLock);
      expectMouseLock();
      // Off again: the resting handle.
      await tester.pumpWidget(_statusBar(latencyMs: 12));
      await tester.pump();
      expect(state.handleStatus, SessionHandleStatus.normal);
      expect(find.text('마우스 잠금'), findsNothing);
      expect(find.byKey(_grip), findsOneWidget);
      // With the connection coming back and a latency that stayed poor, it
      // still is the one thing the handle says.
      await tester.pumpWidget(
        _statusBar(latencyMs: 142, gameMouse: true, resuming: true),
      );
      await tester.pump(SessionToolbar.poorHold);
      expect(state.poorSustained, isTrue);
      expect(state.handleStatus, SessionHandleStatus.mouseLock);
      expectMouseLock();
      expect(find.text('다시 연결 중'), findsNothing);
      expect(find.textContaining('느림'), findsNothing);
      // Then resuming, then poor.
      await tester.pumpWidget(_statusBar(latencyMs: 142, resuming: true));
      await tester.pump();
      expect(state.handleStatus, SessionHandleStatus.resuming);
      expect(find.text('다시 연결 중'), findsOneWidget);
      expect(find.text('마우스 잠금'), findsNothing);
      expect(find.textContaining('느림'), findsNothing);
      await tester.pumpWidget(_statusBar(latencyMs: 142));
      await tester.pump();
      expect(state.handleStatus, SessionHandleStatus.poor);
      expect(find.text('142 ms · 느림'), findsOneWidget);
      expect(find.text('다시 연결 중'), findsNothing);
      expect(find.text('마우스 잠금'), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'While the connection is coming back the handle says so; a click still opens the bar',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(_statusBar(latencyMs: 12));
      await tester.pump();
      final state = _state(tester);
      expect(find.text('다시 연결 중'), findsNothing);
      await tester.pumpWidget(_statusBar(latencyMs: 12, resuming: true));
      await tester.pump();
      expect(state.handleStatus, SessionHandleStatus.resuming);
      expect(find.text('다시 연결 중'), findsOneWidget);
      expect(find.byKey(_grip), findsNothing);
      expect(find.textContaining('ms'), findsNothing);
      final tab = tester.getRect(find.byKey(_tab));
      expect(tab.height, _tabHeight);
      expect(tab.top, 0);
      expect(tab.center.dx, closeTo(450, 1));
      // Idle time neither opens nor hides it.
      await tester.pump(const Duration(seconds: 10));
      expect(find.text('다시 연결 중'), findsOneWidget);
      expect(find.byKey(_bar), findsNothing);
      // The tab is the handle: a click opens the bar.
      await tester.tap(find.byKey(_handle));
      await tester.pump();
      expect(find.byKey(_bar), findsOneWidget);
      expect(find.text('다시 연결 중'), findsNothing);
      await tester.tap(find.byKey(_fold));
      await tester.pump();
      expect(find.text('다시 연결 중'), findsOneWidget);
      // Back: the resting handle.
      await tester.pumpWidget(_statusBar(latencyMs: 12));
      await tester.pump();
      expect(state.handleStatus, SessionHandleStatus.normal);
      expect(find.text('다시 연결 중'), findsNothing);
      expect(find.byKey(_grip), findsOneWidget);
      expect(tester.getSize(find.byKey(_tab)), _restSize);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'By finger the handle keeps its look inside a 44 dp hit area; a tap anywhere in it opens the bar',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      Rect? bounds;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              title: 'TESTER',
              connected: true,
              latencyMs: 12,
              touch: true,
              onInteract: () {},
              onBounds: (value) => bounds = value,
              actions: [_menu('화면')],
            ),
          ),
        ),
      );
      await tester.pump();
      final tab = tester.getRect(find.byKey(_tab));
      expect(tab.size, _restSize);
      expect(tab.top, 0);
      expect(tab.center.dx, closeTo(450, 1));
      // The area that takes touches (SessionToolbarState._touchHit).
      expect(bounds!.size, const Size(88, 44));
      expect(bounds!.top, 0);
      expect(bounds!.center.dx, closeTo(450, 1));
      // No hover by finger: a pointer over it does not grow the tab.
      final mouse = await hoverHandle(tester);
      expect(tester.getRect(find.byKey(_tab)), tab);
      expect(find.byKey(_grip), findsOneWidget);
      expect(find.textContaining('ms'), findsNothing);
      await mouse.removePointer();
      await tester.pump();
      // Below the tongue, still inside the hit area.
      await tester.tapAt(bounds!.bottomCenter - const Offset(0, 6));
      await tester.pump();
      expect(find.byKey(_bar), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Hover never opens the handle or releases remote input; a click does both',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      var interactions = 0, below = 0;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: Stack(
              children: [
                Positioned.fill(
                  child: MouseRegion(
                    onHover: (_) => below++,
                    child: const ColoredBox(color: Colors.black),
                  ),
                ),
                SessionToolbar(
                  title: 'TESTER',
                  connected: true,
                  latencyMs: 55,
                  onInteract: () => interactions++,
                  actions: [_menu('화면')],
                ),
              ],
            ),
          ),
        ),
      );
      await tester.pump();
      final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
      await mouse.addPointer(location: const Offset(60, 300));
      await tester.pump();
      final hoversBefore = below;
      // Along the top edge next to the handle: the picture keeps its hover.
      await mouse.moveTo(const Offset(80, 1));
      await tester.pump();
      expect(below, greaterThan(hoversBefore));
      await mouse.moveTo(tester.getCenter(find.byKey(_handle)));
      await tester.pump(const Duration(seconds: 2));
      expect(find.byKey(_bar), findsNothing, reason: 'Hover never opens it.');
      expect(interactions, 0, reason: 'Passing over it keeps remote input.');
      await tester.tap(find.byKey(_handle));
      await tester.pump();
      expect(find.byKey(_bar), findsOneWidget);
      expect(interactions, greaterThan(0));
      await mouse.removePointer();
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Dragging moves the bar along the top edge only and menus use the moved anchor',
    (tester) async {
      tester.view.physicalSize = const Size(1200, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      Rect? bounds, anchor;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              title: 'TESTER',
              modeLabel: '스트리밍',
              connected: true,
              onInteract: () {},
              onBounds: (value) => bounds = value,
              actions: [
                _menu(
                  '화면',
                  presenter: (value, _) async {
                    anchor = value;
                    return null;
                  },
                ),
              ],
            ),
          ),
        ),
      );
      await tester.pump();
      await openBar(tester);
      await tester.pump();
      final drag = await tester.startGesture(
        tester.getTopLeft(find.byKey(_bar)) +
            const Offset(30, SessionChrome.barHeight / 2),
      );
      await drag.moveBy(const Offset(30, 20));
      await tester.pump();
      final before = bounds!;
      await drag.moveBy(const Offset(20, 10));
      await drag.moveBy(const Offset(20, 10));
      await tester.pump();
      expect(bounds!.left - before.left, closeTo(40, 0.1));
      expect(bounds!.top, before.top, reason: 'The bar stays on the top edge.');
      await drag.moveBy(const Offset(2000, 2000));
      await drag.up();
      await tester.pump();
      expect(bounds!.right, lessThanOrEqualTo(1200));
      expect(bounds!.top, 0);
      expect(bounds!.height, SessionChrome.barHeight);
      // The handle under the bar moves with it.
      expect(
        tester.getCenter(find.byKey(_fold)).dx,
        closeTo(bounds!.center.dx, .01),
      );
      await tester.tap(find.byTooltip('화면'));
      await tester.pump();
      expect(anchor, isNotNull);
      expect(bounds!.contains(anchor!.center), isTrue);
      expect(anchor!.right, lessThanOrEqualTo(1200));
      // Folded, the handle keeps the moved position.
      await tester.tap(find.byKey(_fold));
      await tester.pump();
      expect(bounds!.right, lessThanOrEqualTo(1200));
      expect(bounds!.center.dx, greaterThan(600));
      expect(bounds!.top, 0);
      expect(bounds!.size, _restSize);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'All menu groups remain bounded at narrow widths and double text size',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      for (final width in [200.0, 360.0, 640.0, 1920.0]) {
        tester.view.physicalSize = Size(width, 600);
        await tester.pumpWidget(
          MaterialApp(
            home: MediaQuery(
              data: MediaQueryData(
                size: Size(width, 600),
                textScaler: const TextScaler.linear(2),
              ),
              child: Scaffold(
                body: SessionToolbar(
                  key: ValueKey(width),
                  title: '아주 긴 한글 작업 공간 이름',
                  modeLabel: '스트리밍',
                  connected: false,
                  onInteract: () {},
                  onMinimize: () {},
                  onRestore: () {},
                  onDisconnect: () {},
                  actions: [
                    for (final label in ['화면', '클립보드', '스트리밍 설정']) _menu(label),
                  ],
                ),
              ),
            ),
          ),
        );
        await tester.pump();
        expect(
          tester.getRect(find.byKey(_tab)).right,
          lessThanOrEqualTo(width),
        );
        expect(tester.getRect(find.byKey(_tab)).left, greaterThanOrEqualTo(0));
        await openBar(tester);
        final bar = tester.getRect(find.byKey(_bar));
        expect(bar.left, greaterThanOrEqualTo(0));
        expect(bar.right, lessThanOrEqualTo(width));
        expect(bar.height, SessionChrome.barHeight);
        // Too narrow for three labelled commands at this text size: one 메뉴.
        expect(
          find.byKey(_overflow),
          width < 1920 ? findsOneWidget : findsNothing,
          reason: '$width',
        );
        expect(tester.takeException(), isNull);
      }
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Quick session commands stay reachable through the narrow 메뉴 and fold the bar after use',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      for (final width in [1440.0, 320.0, 200.0]) {
        tester.view.physicalSize = Size(width, 600);
        final calls = <String>[];
        Rect? reported;
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: SessionToolbar(
                key: ValueKey(width),
                title: '한글 VM · 긴 작업 공간 이름',
                connected: true,
                modeLabel: '스트리밍',
                onInteract: () => calls.add('local'),
                onBounds: (value) => reported = value,
                onMinimize: () => calls.add('minimize'),
                onRestore: () => calls.add('restore'),
                onDisconnect: () => calls.add('disconnect'),
                actions: [
                  _menu(
                    '화면',
                    items: [SessionMenuItem('실제 화면 설정', onSelected: () {})],
                  ),
                ],
              ),
            ),
          ),
        );
        await tester.pump();
        await openBar(tester);
        await tester.pump();
        expect(tester.takeException(), isNull);
        expect(reported!.right, lessThanOrEqualTo(width));
        expect(reported!.size, tester.getSize(find.byKey(_bar)));
        // Wide, the window's buttons are on the bar; narrow, in the 메뉴.
        expect(
          find.byKey(_overflow),
          width > 1000 ? findsNothing : findsOneWidget,
          reason: '$width',
        );
        for (final command in ['minimize', 'restore', 'disconnect']) {
          await openBar(tester);
          if (find.byKey(Key('session-$command')).evaluate().isEmpty) {
            await tester.tap(find.byKey(_overflow));
            await tester.pumpAndSettle();
            // The window's rows have their own caption.
            expect(find.text('창'), findsOneWidget, reason: '$width');
          }
          await tester.tap(find.byKey(Key('session-$command')));
          await tester.pumpAndSettle();
          expect(calls.where((value) => value == command).length, 1);
          // A used command folds the unpinned bar back to its handle.
          expect(find.byKey(_bar), findsNothing, reason: command);
          expect(find.byKey(_handle), findsOneWidget);
        }
        expect(
          calls.where((value) => value == 'local').length,
          greaterThanOrEqualTo(3),
        );
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      }
    },
  );

  testWidgets(
    'The window buttons sit side by side before disconnect, run their callbacks and fold an unpinned bar',
    (tester) async {
      tester.view.physicalSize = const Size(1200, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final calls = <String>[];
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              title: 'TESTER',
              connected: true,
              fullscreen: true,
              onInteract: () {},
              onMinimize: () => calls.add('minimize'),
              onRestore: () => calls.add('restore'),
              onDisconnect: () => calls.add('disconnect'),
              actions: [_menu('화면')],
            ),
          ),
        ),
      );
      await openBar(tester);
      final minimize = find.byKey(const Key('session-minimize')),
          restore = find.byKey(const Key('session-restore')),
          disconnect = find.byKey(const Key('session-disconnect'));
      expect(tester.getSize(minimize), _windowButtonSize);
      expect(tester.getSize(restore), _windowButtonSize);
      expect(find.byTooltip('최소화'), findsOneWidget);
      expect(find.byTooltip('창 모드로'), findsOneWidget);
      expect(find.byKey(const Key('session-fullscreen')), findsNothing);
      expect(find.byKey(_overflow), findsNothing);
      // As on a title bar: — then ❐, after the commands, before the one
      // danger action.
      expect(
        tester.getRect(find.byTooltip('화면')).right,
        lessThan(tester.getRect(minimize).left),
      );
      expect(
        tester.getRect(minimize).right,
        closeTo(tester.getRect(restore).left, .01),
      );
      expect(
        tester.getRect(restore).right,
        lessThan(tester.getRect(disconnect).left),
      );
      expect(tester.getRect(minimize).top, tester.getRect(restore).top);
      await tester.tap(minimize);
      await tester.pump();
      expect(calls, ['minimize']);
      expect(find.byKey(_bar), findsNothing, reason: 'Used: it folds.');
      await openBar(tester);
      await tester.tap(restore);
      await tester.pump();
      expect(calls, ['minimize', 'restore']);
      expect(find.byKey(_bar), findsNothing, reason: 'Used: it folds.');
      // Kept open, it stays.
      await openBar(tester);
      _state(tester).setPinned(true);
      await tester.pump();
      await tester.tap(minimize);
      await tester.pump();
      await tester.tap(restore);
      await tester.pump();
      expect(calls, ['minimize', 'restore', 'minimize', 'restore']);
      expect(find.byKey(_bar), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'In a bar too narrow for them the window buttons move into the 메뉴 under 창',
    (tester) async {
      tester.view.physicalSize = const Size(300, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final calls = <String>[];
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              title: 'TESTER',
              connected: true,
              fullscreen: true,
              onInteract: () {},
              onMinimize: () => calls.add('minimize'),
              onRestore: () => calls.add('restore'),
              onDisconnect: () {},
              actions: [
                _menu(
                  '화면',
                  items: [
                    SessionMenuItem(
                      '창 모드',
                      key: const Key('mode-window'),
                      onSelected: () => calls.add('mode'),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ),
      );
      await openBar(tester);
      final minimize = find.byKey(const Key('session-minimize')),
          restore = find.byKey(const Key('session-restore'));
      expect(minimize, findsNothing);
      expect(restore, findsNothing);
      expect(find.byKey(_overflow), findsOneWidget);
      // The chip and the danger action never leave the bar.
      expect(find.byKey(const Key('session-quality')), findsOneWidget);
      expect(find.byKey(const Key('session-disconnect')), findsOneWidget);
      final bar = tester.getRect(find.byKey(_bar));
      expect(bar.left, greaterThanOrEqualTo(0));
      expect(bar.right, lessThanOrEqualTo(300));
      await tester.tap(find.byKey(_overflow));
      await tester.pumpAndSettle();
      // 화면's rows, 바 열어 두기 under its own caption (the last group is
      // not 설정), then the window's rows: — before ❐.
      expect(find.text('화면'), findsOneWidget);
      expect(find.text('툴바'), findsOneWidget);
      expect(find.text('창'), findsOneWidget);
      expect(find.text('최소화'), findsOneWidget);
      expect(find.text('창 모드로'), findsOneWidget);
      final rows = [
        find.text('화면'),
        find.byKey(const Key('mode-window')),
        find.text('툴바'),
        find.byKey(_pin),
        find.text('창'),
        minimize,
        restore,
      ];
      for (var i = 1; i < rows.length; i++) {
        expect(
          tester.getTopLeft(rows[i]).dy,
          greaterThan(tester.getTopLeft(rows[i - 1]).dy),
          reason: 'Row $i is below row ${i - 1}.',
        );
      }
      await tester.tap(restore);
      await tester.pumpAndSettle();
      expect(calls, ['restore']);
      expect(find.byKey(_bar), findsNothing, reason: 'Used: it folds.');
      await openBar(tester);
      await tester.tap(find.byKey(_overflow));
      await tester.pumpAndSettle();
      await tester.tap(minimize);
      await tester.pumpAndSettle();
      expect(calls, ['restore', 'minimize']);
      expect(find.byKey(_bar), findsNothing, reason: 'Used: it folds.');
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'In a narrow window the labelled commands become one 메뉴 that lists their entries and 바 열어 두기',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final used = <String>[];
      Widget bar() => MaterialApp(
        home: Scaffold(
          body: SessionToolbar(
            title: 'TESTER',
            connected: true,
            latencyMs: 12,
            onInteract: () {},
            onDisconnect: () {},
            actions: [
              // One row for a command of its own.
              SessionToolbarMenu.command(
                key: const Key('group-screen'),
                label: '화면',
                icon: Icons.fullscreen_rounded,
                onInteract: () {},
                onPressed: () => used.add('screen'),
              ),
              // A command that stands for a group: its caption and rows.
              SessionToolbarMenu.command(
                key: const Key('group-share'),
                label: '폴더·USB',
                icon: Icons.folder_shared_rounded,
                onInteract: () {},
                onPressed: () => used.add('share'),
                overflow: [
                  SessionMenuItem(
                    '공유 폴더',
                    key: const Key('row-drives'),
                    icon: Icons.folder_shared_rounded,
                    value: '1개 연결됨',
                    onSelected: () => used.add('drives'),
                  ),
                  SessionMenuItem(
                    'USB 장치',
                    key: const Key('row-usb'),
                    icon: Icons.usb_rounded,
                    onSelected: () => used.add('usb'),
                  ),
                ],
              ),
              // A menu: its caption and items.
              _menu(
                '설정',
                icon: Icons.settings_rounded,
                items: [
                  SessionMenuItem(
                    '화질',
                    key: const Key('row-quality'),
                    onSelected: () => used.add('quality'),
                  ),
                ],
              ),
            ],
          ),
        ),
      );
      const labels = ['화면', '폴더·USB', '설정'];
      // Wide: three labelled buttons, no 메뉴 and no pin on the bar.
      tester.view.physicalSize = const Size(1200, 600);
      await tester.pumpWidget(bar());
      await openBar(tester);
      expect(find.byKey(_overflow), findsNothing);
      for (final label in labels) {
        expect(find.text(label), findsOneWidget);
        expect(find.byTooltip(label), findsOneWidget);
      }
      expect(find.byKey(_pin), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
      // Narrow: one labelled 메뉴 in their place.
      tester.view.physicalSize = const Size(360, 600);
      await tester.pumpWidget(bar());
      await openBar(tester);
      final menu = find.byKey(_overflow);
      expect(menu, findsOneWidget);
      expect(
        find.descendant(of: menu, matching: find.text('메뉴')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: menu, matching: find.byIcon(Icons.menu_rounded)),
        findsOneWidget,
      );
      expect(find.byTooltip('메뉴 · 화면, 설정 등'), findsOneWidget);
      expect(tester.getSize(menu).height, SessionChrome.button);
      for (final label in labels) {
        expect(find.text(label), findsNothing);
        expect(find.byTooltip(label), findsNothing);
      }
      expect(find.byKey(const Key('group-screen')), findsNothing);
      expect(find.byKey(const Key('group-share')), findsNothing);
      // chip | 메뉴 | 연결 끊기, inside the window.
      double left(Finder finder) => tester.getTopLeft(finder).dx;
      expect(
        left(find.byKey(const Key('session-quality'))),
        lessThan(left(menu)),
      );
      expect(
        left(menu),
        lessThan(left(find.byKey(const Key('session-disconnect')))),
      );
      final narrow = tester.getRect(find.byKey(_bar));
      expect(narrow.left, greaterThanOrEqualTo(0));
      expect(narrow.right, lessThanOrEqualTo(360));
      final state = _state(tester);
      Future<void> openMenu() async {
        await openBar(tester);
        await tester.tap(menu);
        await tester.pumpAndSettle();
      }

      await openMenu();
      // Each action's rows in the bar's order, then 바 열어 두기 as the last
      // row of 설정 (no caption of its own), and no window rows.
      final rows = [
        find.byKey(const Key('group-screen')),
        find.text('폴더·USB'),
        find.byKey(const Key('row-drives')),
        find.byKey(const Key('row-usb')),
        find.text('설정'),
        find.byKey(const Key('row-quality')),
        find.byKey(_pin),
      ];
      for (final row in rows) {
        expect(row, findsOneWidget);
      }
      for (var i = 1; i < rows.length; i++) {
        expect(
          tester.getTopLeft(rows[i]).dy,
          greaterThan(tester.getTopLeft(rows[i - 1]).dy),
          reason: 'Row $i is below row ${i - 1}.',
        );
      }
      expect(find.text('화면'), findsOneWidget);
      expect(find.text('공유 폴더'), findsOneWidget);
      expect(find.text('1개 연결됨'), findsOneWidget);
      expect(find.text('USB 장치'), findsOneWidget);
      expect(find.text('화질'), findsOneWidget);
      expect(find.text('툴바 열어 두기'), findsOneWidget);
      expect(find.text('툴바'), findsNothing);
      expect(find.text('창'), findsNothing);
      // The group's own onPressed is for the wide bar only.
      expect(find.byKey(const Key('group-share')), findsNothing);
      Finder pinCheck() => find.descendant(
        of: find.byKey(_pin),
        matching: find.byIcon(Icons.check_rounded),
      );
      expect(pinCheck(), findsNothing);
      // A row runs its command and folds the unpinned bar.
      await tester.tap(find.byKey(const Key('row-usb')));
      await tester.pumpAndSettle();
      expect(used, ['usb']);
      expect(find.byKey(_bar), findsNothing);
      // 바 열어 두기 switches in place and keeps the bar open.
      await openMenu();
      await tester.tap(find.byKey(_pin));
      await tester.pumpAndSettle();
      expect(state.pinned, isTrue);
      expect(state.isOpen, isTrue);
      expect(find.byKey(_bar), findsOneWidget);
      expect(used, ['usb']);
      // Kept open, a row no longer folds it.
      await openMenu();
      expect(pinCheck(), findsOneWidget);
      await tester.tap(find.byKey(const Key('row-drives')));
      await tester.pumpAndSettle();
      expect(used, ['usb', 'drives']);
      expect(find.byKey(_bar), findsOneWidget);
      // Switched off in the same row: still open, until the next command.
      await openMenu();
      await tester.tap(find.byKey(_pin));
      await tester.pumpAndSettle();
      expect(state.pinned, isFalse);
      expect(find.byKey(_bar), findsOneWidget);
      await openMenu();
      expect(pinCheck(), findsNothing);
      await tester.tap(find.byKey(const Key('group-screen')));
      await tester.pumpAndSettle();
      expect(used, ['usb', 'drives', 'screen']);
      expect(find.byKey(_bar), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'In the narrow 메뉴 a greyed command still says why, as on the bar',
    (tester) async {
      tester.view.physicalSize = const Size(260, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              title: 'Overflow QA',
              connected: true,
              onInteract: () {},
              onDisconnect: () {},
              actions: [
                SessionToolbarMenu.command(
                  key: const Key('remote-drives'),
                  label: '공유 폴더',
                  icon: Icons.folder_off_rounded,
                  onPressed: null,
                  onInteract: () {},
                  disabledReason: 'VM에서 WebClient 서비스를 켜 주세요',
                ),
              ],
            ),
          ),
        ),
      );
      await tester.pump();
      await openBar(tester);
      expect(find.byKey(const Key('remote-drives')), findsNothing);
      expect(
        find.descendant(of: find.byKey(_overflow), matching: find.text('메뉴')),
        findsOneWidget,
      );
      await tester.tap(find.byKey(_overflow));
      await tester.pumpAndSettle();
      expect(find.text('공유 폴더'), findsOneWidget);
      expect(
        find.byTooltip('공유 폴더 · VM에서 WebClient 서비스를 켜 주세요'),
        findsOneWidget,
      );
      // 바 열어 두기 follows under its own caption.
      expect(find.text('툴바'), findsOneWidget);
      expect(find.byKey(_pin), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Keyboard focus and a native popup keep the bar open; Escape folds it',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      var pending = Completer<int?>();
      var selected = false;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              title: 'Keyboard QA',
              onInteract: () {},
              actions: [
                _menu(
                  '화면',
                  presenter: (_, _) => pending.future,
                  items: [
                    SessionMenuItem(
                      '지원 화면 동작',
                      onSelected: () => selected = true,
                    ),
                  ],
                ),
              ],
            ),
          ),
        ),
      );
      await openBar(tester);
      // The quality chip, then the screen menu.
      await tester.sendKeyEvent(LogicalKeyboardKey.tab);
      await tester.sendKeyEvent(LogicalKeyboardKey.tab);
      await tester.pump(const Duration(seconds: 4));
      expect(find.byKey(_bar), findsOneWidget);
      await tester.sendKeyEvent(LogicalKeyboardKey.enter);
      await tester.pump();
      await tester.pump(const Duration(seconds: 4));
      expect(find.byKey(_bar), findsOneWidget);
      pending.complete(0);
      await tester.pumpAndSettle();
      expect(selected, isTrue);
      // Using a menu folds the bar.
      expect(find.byKey(_bar), findsNothing);
      // Escape folds it for keyboard users.
      pending = Completer<int?>();
      await openBar(tester);
      await tester.sendKeyEvent(LogicalKeyboardKey.tab);
      await tester.pump();
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pump();
      expect(find.byKey(_bar), findsNothing);
      expect(find.byKey(_handle), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Large text remains bounded while session commands keep their tooltip labels',
    (tester) async {
      tester.view.physicalSize = const Size(640, 400);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MaterialApp(
          home: MediaQuery(
            data: const MediaQueryData(textScaler: TextScaler.linear(2)),
            child: Scaffold(
              body: SessionToolbar(
                title: '한글 작업 공간',
                connected: true,
                onInteract: () {},
                actions: [_menu('화면')],
              ),
            ),
          ),
        ),
      );
      await tester.pump();
      await openBar(tester);
      expect(tester.takeException(), isNull);
      expect(find.byTooltip('화면'), findsOneWidget);
      // The bar and the handle under it keep their own heights.
      expect(tester.getSize(find.byKey(_bar)).height, SessionChrome.barHeight);
      expect(tester.getSize(find.byKey(_fold)), _lipSize);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Switching between bar menus takes one click; a menu\'s own button only closes it',
    (tester) async {
      tester.view.physicalSize = const Size(1200, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      var panels = 0;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              title: 'TESTER',
              connected: true,
              latencyMs: 12,
              onInteract: () {},
              onDisconnect: () {},
              actions: [
                _menu(
                  '화면',
                  items: [
                    SessionMenuItem(
                      '창 모드',
                      key: const Key('mode-window'),
                      onSelected: () {},
                    ),
                  ],
                ),
                SessionToolbarMenu.command(
                  key: const Key('settings-command'),
                  label: '스트리밍 설정',
                  icon: Icons.tune_rounded,
                  onInteract: () {},
                  onPressed: () {},
                  onPressedAt: (_) async {
                    panels++;
                    return false;
                  },
                ),
              ],
            ),
          ),
        ),
      );
      await openBar(tester);
      final chipMenu = find.text('왕복 지연 · Host 응답');
      await tester.tap(find.byKey(const Key('session-quality')));
      await tester.pumpAndSettle();
      expect(chipMenu, findsOneWidget);
      // One click on another button closes the chip menu and opens its own.
      await tester.tap(find.byTooltip('화면'), warnIfMissed: false);
      await tester.pumpAndSettle();
      expect(chipMenu, findsNothing);
      expect(find.byKey(const Key('mode-window')), findsOneWidget);
      // And back again, still one click.
      await tester.tap(
        find.byKey(const Key('session-quality')),
        warnIfMissed: false,
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('mode-window')), findsNothing);
      expect(chipMenu, findsOneWidget);
      // The button that opened a menu closes it, without reopening.
      await tester.tap(
        find.byKey(const Key('session-quality')),
        warnIfMissed: false,
      );
      await tester.pumpAndSettle();
      expect(chipMenu, findsNothing);
      // From a menu straight to a command.
      await tester.tap(find.byTooltip('화면'));
      await tester.pumpAndSettle();
      await tester.tap(
        find.byKey(const Key('settings-command')),
        warnIfMissed: false,
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('mode-window')), findsNothing);
      expect(panels, 1);
      expect(find.byKey(_bar), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'The quality chip lists measured values with what they measure and opens statistics',
    (tester) async {
      tester.view.physicalSize = const Size(1200, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      var stats = 0;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(
            body: SessionToolbar(
              title: 'TESTER',
              connected: true,
              latencyMs: 92,
              onInteract: () {},
              qualityItems: () => [
                const SessionMenuItem.info('화면 · PC 디코딩', '59.8 fps'),
                SessionMenuItem(
                  '연결 정보·실측 통계…',
                  key: const Key('quality-stats'),
                  onSelected: () => stats++,
                ),
              ],
              actions: [_menu('화면')],
            ),
          ),
        ),
      );
      await tester.pump();
      await openBar(tester);
      expect(
        find.byTooltip('연결 품질 · 느림'),
        findsOneWidget,
        reason: '80 ms or more is poor.',
      );
      expect(find.text('느림'), findsNothing);
      await tester.tap(find.byKey(const Key('session-quality')));
      await tester.pumpAndSettle();
      expect(find.text('연결 품질'), findsOneWidget);
      expect(find.text('왕복 지연 · Host 응답'), findsOneWidget);
      expect(find.text('92 ms'), findsNWidgets(2));
      expect(find.text('59.8 fps'), findsOneWidget);
      await tester.tap(find.byKey(const Key('quality-stats')));
      await tester.pumpAndSettle();
      expect(stats, 1);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'onQualityAt opens the session\'s own panel in place of the quality menu; true folds an unpinned bar',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final anchors = <Rect>[], menus = <bool>[];
      var used = false, items = 0;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              title: 'TESTER',
              connected: true,
              latencyMs: 12,
              onInteract: () {},
              onMenuChanged: menus.add,
              qualityItems: () {
                items++;
                return const [SessionMenuItem.info('화면 · PC 디코딩', '59.8 fps')];
              },
              onQualityAt: (anchor) async {
                anchors.add(anchor);
                return used;
              },
              actions: [_menu('화면')],
            ),
          ),
        ),
      );
      await openBar(tester);
      final chip = find.byKey(const Key('session-quality'));
      final chipRect = tester.getRect(chip);
      await tester.tap(chip);
      await tester.pumpAndSettle();
      // The caller's panel, never the quality menu.
      expect(anchors, hasLength(1));
      expect(items, 0);
      expect(find.text('연결 품질'), findsNothing);
      expect(find.text('왕복 지연 · Host 응답'), findsNothing);
      expect(find.text('59.8 fps'), findsNothing);
      // The anchor is the chip, down to the bar's bottom edge.
      final anchor = anchors.single;
      expect(anchor.left, closeTo(chipRect.left, .01));
      expect(anchor.top, closeTo(chipRect.top, .01));
      expect(anchor.width, closeTo(chipRect.width, .01));
      expect(
        anchor.height,
        closeTo(chipRect.height + SessionChrome.belowButton, .01),
      );
      // The session hears of it as of any bar menu.
      expect(menus, [true, false]);
      // false: nothing was used, the bar stays open.
      expect(find.byKey(_bar), findsOneWidget);
      // true: it folds as after a command.
      used = true;
      await tester.tap(chip);
      await tester.pumpAndSettle();
      expect(anchors, hasLength(2));
      expect(find.byKey(_bar), findsNothing);
      expect(find.byKey(_handle), findsOneWidget);
      // Unless it is kept open.
      await openBar(tester);
      _state(tester).setPinned(true);
      await tester.pump();
      await tester.tap(chip);
      await tester.pumpAndSettle();
      expect(anchors, hasLength(3));
      expect(find.byKey(_bar), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets('The handle under the bar folds it, also when it is kept open', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(900, 600);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    var commands = 0, interactions = 0;
    final opened = <bool>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: SessionToolbar(
            title: 'TESTER',
            connected: true,
            onInteract: () => interactions++,
            onOpenChanged: opened.add,
            actions: [
              SessionToolbarMenu.command(
                key: const Key('fold-command'),
                label: '설정',
                icon: Icons.settings_rounded,
                onInteract: () {},
                onPressed: () => commands++,
              ),
            ],
          ),
        ),
      ),
    );
    await tester.pump();
    final state = _state(tester);
    expect(state.isOpen, isFalse);
    expect(state.pinned, isFalse);
    expect(find.byKey(_fold), findsNothing);
    await openBar(tester);
    expect(state.isOpen, isTrue);
    expect(opened, [true]);
    // An unpinned bar.
    interactions = 0;
    await tester.tap(find.byKey(_fold));
    await tester.pump();
    expect(interactions, greaterThan(0));
    expect(state.isOpen, isFalse);
    expect(find.byKey(_bar), findsNothing);
    expect(find.byKey(_fold), findsNothing);
    expect(find.byKey(_handle), findsOneWidget);
    expect(opened, [true, false]);
    // Kept open: a command leaves it, and the handle is still there.
    await openBar(tester);
    state.setPinned(true);
    await tester.pump();
    expect(state.pinned, isTrue);
    expect(
      find.byKey(_fold),
      findsOneWidget,
      reason: '바 열어 두기 only stops the bar folding by itself.',
    );
    await tester.tap(find.byKey(const Key('fold-command')));
    await tester.pump();
    expect(commands, 1);
    expect(state.isOpen, isTrue);
    // A pinned bar that was folded is not open, and is still pinned.
    await tester.tap(find.byKey(_fold));
    await tester.pump();
    expect(state.isOpen, isFalse);
    expect(state.pinned, isTrue);
    expect(find.byKey(_bar), findsNothing);
    expect(find.byKey(_handle), findsOneWidget);
    // Opened again it is kept open as before.
    await openBar(tester);
    await tester.tap(find.byKey(const Key('fold-command')));
    await tester.pump();
    expect(commands, 2);
    expect(state.isOpen, isTrue);
    expect(find.byKey(_bar), findsOneWidget);
    // fold() (a used command, Android's Back) leaves a pinned bar alone
    // and folds an unpinned one; open() opens it.
    state.fold();
    await tester.pump();
    expect(state.isOpen, isTrue);
    state.setPinned(false);
    await tester.pump();
    expect(state.isOpen, isTrue);
    state.fold();
    await tester.pump();
    expect(state.isOpen, isFalse);
    state.open();
    await tester.pump();
    expect(state.isOpen, isTrue);
    expect(find.byKey(_bar), findsOneWidget);
    expect(opened.last, isTrue);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets(
    'A bar kept open stays open after use and still folds by its handle; released, the next command folds it',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      Rect? bounds;
      var restored = 0;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionToolbar(
              title: 'QA VM',
              onInteract: () {},
              onBounds: (value) => bounds = value,
              onRestore: () => restored++,
              actions: [
                IconButton(
                  key: const Key('qa-action'),
                  onPressed: () {},
                  icon: const Icon(Icons.info_outline),
                ),
              ],
            ),
          ),
        ),
      );
      await tester.pump();
      expect(find.byKey(const Key('qa-action')), findsNothing);
      await openBar(tester);
      expect(find.byKey(const Key('qa-action')), findsOneWidget);
      // 바 열어 두기 has no button on the bar; a window button the caller
      // did not pass is not there either.
      expect(find.byKey(_pin), findsNothing);
      expect(find.byKey(const Key('session-minimize')), findsNothing);
      final state = _state(tester);
      state.setPinned(true);
      await tester.pump();
      expect(state.pinned, isTrue);
      expect(find.byKey(_fold), findsOneWidget);
      await tester.tap(find.byKey(const Key('session-restore')));
      await tester.pump(const Duration(seconds: 5));
      expect(restored, 1);
      expect(find.byKey(const Key('qa-action')), findsOneWidget);
      await tester.drag(
        find.byKey(const Key('drag-session-toolbar')),
        const Offset(-2000, 2000),
      );
      await tester.pump();
      expect(bounds!.left, greaterThanOrEqualTo(0));
      expect(bounds!.top, 0);
      expect(bounds!.right, lessThanOrEqualTo(900));
      // Its handle folds it all the same; opened again it is still kept
      // open.
      await tester.tap(find.byKey(_fold));
      await tester.pump();
      expect(find.byKey(const Key('qa-action')), findsNothing);
      expect(state.isOpen, isFalse);
      expect(state.pinned, isTrue);
      await openBar(tester);
      await tester.tap(find.byKey(const Key('session-restore')));
      await tester.pump();
      expect(restored, 2);
      expect(find.byKey(const Key('qa-action')), findsOneWidget);
      // Unpinned, the next command folds it again.
      state.setPinned(false);
      await tester.pump();
      expect(find.byKey(const Key('qa-action')), findsOneWidget);
      await tester.tap(find.byKey(const Key('session-restore')));
      await tester.pump();
      expect(restored, 3);
      expect(find.byKey(const Key('qa-action')), findsNothing);
      expect(find.byKey(_handle), findsOneWidget);
    },
  );

  testWidgets(
    'A held remote press makes the toolbar ignore the pointer and folds an open bar',
    (tester) async {
      tester.view.physicalSize = const Size(900, 600);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      var interactions = 0;
      Widget bar({required bool held}) => MaterialApp(
        home: Scaffold(
          body: SessionToolbar(
            title: 'TESTER',
            connected: true,
            pointerHeld: held,
            onInteract: () => interactions++,
            actions: [_menu('화면')],
          ),
        ),
      );
      IgnorePointer gate() => tester.widget<IgnorePointer>(
        find.byKey(const Key('session-toolbar-pointer')),
      );
      await tester.pumpWidget(bar(held: false));
      expect(gate().ignoring, isFalse);
      await openBar(tester);
      interactions = 0;
      await tester.pumpWidget(bar(held: true));
      expect(
        find.byKey(_bar),
        findsNothing,
        reason: 'A click into the remote folds it.',
      );
      expect(gate().ignoring, isTrue);
      // A button-held pointer arriving over the bar changes nothing.
      await tester.pumpWidget(bar(held: false));
      await openBar(tester);
      interactions = 0;
      final drag = await tester.createGesture(kind: PointerDeviceKind.mouse);
      await drag.addPointer(location: const Offset(450, 400));
      await drag.down(const Offset(450, 400));
      await drag.moveTo(tester.getCenter(find.byKey(_bar)));
      await tester.pump();
      expect(interactions, 0, reason: 'A drag from elsewhere is not a visit.');
      await drag.up();
      await tester.pump();
      await drag.moveBy(const Offset(2, 0));
      await tester.pump();
      expect(interactions, 1, reason: 'Once the button is up it counts.');
      await drag.removePointer();
      await tester.pump();
      // Kept open, a press in the picture does not fold it; while the press
      // is held neither the bar nor the handle under it takes the pointer.
      _state(tester).setPinned(true);
      await tester.pump();
      await tester.pumpWidget(bar(held: true));
      expect(find.byKey(_bar), findsOneWidget);
      expect(gate().ignoring, isTrue);
      await tester.tap(find.byKey(_fold), warnIfMissed: false);
      await tester.pump();
      expect(
        find.byKey(_bar),
        findsOneWidget,
        reason: 'The held press owns the pointer.',
      );
      await tester.pumpWidget(bar(held: false));
      await tester.tap(find.byKey(_fold));
      await tester.pump();
      expect(find.byKey(_bar), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Remote viewport uses the full client area and toolbar hover releases input without forwarding toolbar clicks',
    (tester) async {
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      const window = MethodChannel('multitap/window');
      const events = MethodChannel('multitap/window/events');
      messenger.setMockMethodCallHandler(window, (_) async => false);
      messenger.setMockMethodCallHandler(events, (_) async => null);
      addTearDown(() {
        messenger.setMockMethodCallHandler(window, null);
        messenger.setMockMethodCallHandler(events, null);
      });
      tester.view.physicalSize = const Size(1440, 900);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final socket = SessionSocket(), media = SessionMedia();
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'qa-session',
          webSocketUrl: Uri.parse(
            'wss://cloud.example.com/api/v1/sessions/qa-session',
          ),
        ),
        token: 'fixture-only-token',
        media: media,
        dialer: (_, _) async => socket,
      );
      final boundary = GlobalKey();
      await tester.pumpWidget(
        RepaintBoundary(
          key: boundary,
          child: MaterialApp(
            debugShowCheckedModeBanner: false,
            theme: buildAppTheme(),
            home: RemoteView(
              machine: const DesktopMachine(
                id: 'qa',
                name: 'QA VM',
                description: '',
                kind: MachineKind.vm,
                status: MachineStatus.ready,
                specification: 'Windows',
                accent: Colors.blue,
              ),
              load: () async => session,
              onReconnect: (_) async => session,
            ),
          ),
        ),
      );
      await tester.pump();
      socket.incoming.add(jsonEncode(info));
      await tester.pump();
      await tester.pump();
      socket.incoming.add(packet());
      await tester.pump();
      await tester.pump();
      session.clipboard = ClipboardSession(
        url: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/qa-session/clipboard',
        ),
        token: 'fixture-only',
        sessionId: 'qa-session',
      );
      session.notifyListeners();
      await tester.pump();
      expect(
        tester.getSize(find.byType(RemoteViewport)),
        const Size(1440, 900),
      );
      // Sharing that has not paired yet shows no clipboard command.
      await openBar(tester);
      expect(find.byKey(const Key('stream-menu-클립보드')), findsNothing);
      // 바 열어 두기 is a switch in the 설정 panel, not a button of the bar.
      expect(find.byKey(_pin), findsNothing);
      // Kept open (as that switch does), the bar stays while the remote is
      // used.
      _state(tester).setPinned(true);
      await tester.pump(Duration.zero);
      await tester.tapAt(const Offset(720, 450));
      await tester.pump(Duration.zero);
      expect(session.captured, isTrue);
      expect(find.byKey(_bar), findsOneWidget);
      final before = socket.sent
          .where((event) => event['type'] == 'input')
          .length;
      final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
      await mouse.addPointer(location: const Offset(900, 500));
      await mouse.moveTo(
        tester.getCenter(find.byKey(const Key('drag-session-toolbar'))),
      );
      await tester.pump();
      await tester.pump();
      expect(session.captured, isFalse);
      expect(media.captures.last, isFalse);
      // The chip opens the 연결 panel; statistics are its last row.
      await tester.tap(find.byKey(const Key('session-quality')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('session-connection-panel')), findsOneWidget);
      expect(find.text('왕복 지연 · Host 응답'), findsOneWidget);
      expect(find.byKey(const Key('quality-value-resolution')), findsOneWidget);
      await tester.tap(_panelRow('quality-stats'));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('session-connection-panel')), findsNothing);
      expect(find.text('연결 통계'), findsOneWidget);
      expect(
        socket.sent.where((event) => event['type'] == 'input').length,
        before,
      );
      await mouse.removePointer();
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    },
  );
}

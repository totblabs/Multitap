import 'dart:ui' show DisplayFeature, DisplayFeatureState, DisplayFeatureType;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_toolbar.dart';
import 'package:multitap_client/ui/session_menu.dart';

import 'android_launcher_test.dart' show MemoryStore;

class _Host {
  final remoteTaps = <Offset>[];
  final hold = AndroidPointerHold();
  final barKey = GlobalKey<AndroidSessionBarState>();
  final chosen = <String>[];
  final popups = SessionPopups();
  final moves = <(AndroidBubbleEdge, double)>[];
  int screen = 0, disconnects = 0, quickKeyboard = 0, quickMode = 0;
  int gameMouse = 0, scale = 0, quality = 0;
}

Future<_Host> _pump(
  WidgetTester tester, {
  Size size = const Size(360, 800),
  EdgeInsets padding = EdgeInsets.zero,
  List<DisplayFeature> features = const [],
  AndroidBubbleEdge edge = AndroidBubbleEdge.right,
  double along = .62,
  bool cluster = false,
  double? textScale,
  Rect? area,
  double? latencyMs = 12,
  bool mouseLock = true,
  // The old flat list: 배율 as a row of its own beside 화면.
  bool scaleRow = false,
  // The host opens its own 연결 panel from the 연결 품질 row.
  bool ownQuality = false,
}) async {
  final host = _Host();
  addTearDown(host.hold.dispose);
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(
      home: MediaQuery(
        data: MediaQueryData(
          size: size,
          padding: padding,
          viewPadding: padding,
          displayFeatures: features,
          textScaler: textScale == null
              ? MediaQueryData.fromView(tester.view).textScaler
              : TextScaler.linear(textScale),
        ),
        child: Scaffold(
          body: Stack(
            fit: StackFit.expand,
            children: [
              // The remote picture: its own pointer handling, like the
              // sessions' surface.
              Listener(
                key: const Key('remote'),
                behavior: HitTestBehavior.opaque,
                onPointerDown: (event) {
                  host.hold.down(event.pointer);
                  host.remoteTaps.add(event.position);
                  host.barKey.currentState?.fold();
                },
                onPointerUp: (event) => host.hold.up(event.pointer),
                onPointerCancel: (event) => host.hold.up(event.pointer),
                child: const ColoredBox(color: Color(0xFF2B4C6F)),
              ),
              Positioned.fill(
                key: const ValueKey('bar-host'),
                child: AndroidSessionBar(
                  key: host.barKey,
                  title: 'VM',
                  modeLabel: '스트리밍',
                  latencyMs: latencyMs,
                  pointerHeld: host.hold.held,
                  popups: host.popups,
                  bubbleArea: area ?? padding.deflateRect(Offset.zero & size),
                  quick: [
                    if (cluster) ...[
                      AndroidQuickButton(
                        key: const Key('quick-mode'),
                        icon: Icons.mouse_rounded,
                        label: '포인터',
                        tooltip: '입력 방식',
                        onPressed: () => host.quickMode++,
                      ),
                      AndroidQuickButton(
                        key: const Key('quick-keyboard'),
                        icon: Icons.keyboard_rounded,
                        tooltip: '키보드 열기',
                        onPressed: () => host.quickKeyboard++,
                      ),
                    ],
                  ],
                  bubbleEdge: edge,
                  bubbleAlong: along,
                  onBubbleMoved: (edge, along) => host.moves.add((edge, along)),
                  onInteract: () {},
                  onDisconnect: () => host.disconnects++,
                  qualityItems: () => const [
                    SessionMenuItem.info('해상도 · 원격 화면', '1920 × 1080'),
                  ],
                  onQuality: ownQuality ? () => host.quality++ : null,
                  actions: [
                    SessionToolbarMenu.command(
                      key: const Key('cmd-screen'),
                      label: '화면',
                      icon: Icons.fullscreen_rounded,
                      onInteract: () {},
                      onPressed: () => host.screen++,
                    ),
                    if (scaleRow)
                      SessionToolbarMenu.command(
                        key: const Key('cmd-scale'),
                        label: '배율',
                        icon: Icons.text_fields_rounded,
                        onInteract: () {},
                        onPressed: () => host.scale++,
                      ),
                    SessionToolbarMenu(
                      key: const Key('menu-keys'),
                      // A menu of rows (a page), under a shared name of the
                      // 설정 group (the real remote view gives one 설정 row
                      // instead, which opens the PC's 설정 panel).
                      label: '화질',
                      icon: Icons.keyboard_command_key_rounded,
                      onInteract: () {},
                      items: [
                        SessionMenuItem(
                          'Windows 키',
                          key: const Key('item-windows'),
                          onSelected: () => host.chosen.add('windows'),
                        ),
                        const SessionMenuItem(
                          '보낼 수 없는 키',
                          key: Key('item-off'),
                          disabledReason: '지금은 보낼 수 없어요',
                        ),
                      ],
                    ),
                    SessionToolbarMenu.command(
                      key: const Key('cmd-game-mouse'),
                      label: '마우스 잠금',
                      icon: Icons.sports_esports_rounded,
                      active: mouseLock,
                      onInteract: () {},
                      onPressed: () => host.gameMouse++,
                    ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    ),
  );
  await tester.pumpAndSettle();
  return host;
}

Finder get _opener => find.byKey(const Key('expand-session-toolbar'));
Finder get _menu => find.byKey(const Key('session-command-bar'));
Finder get _page => find.byKey(const Key('android-session-menu-card'));

Future<void> _open(WidgetTester tester) async {
  await tester.tap(_opener);
  await tester.pumpAndSettle();
}

/// The open menu's group captions, top to bottom.
List<String> _captions(WidgetTester tester) => [
  for (final caption in tester.widgetList<AndroidMenuCaption>(
    find.descendant(of: _menu, matching: find.byType(AndroidMenuCaption)),
  ))
    caption.label,
];

/// The session's amber and the dark ink on it (android_toolbar.dart).
const _amber = Color(0xFFF5A623), _amberInk = Color(0xFF241600);

/// The cluster's rail: its one capsule with a shadow.
BoxDecoration _rail(WidgetTester tester) => tester
    .widgetList<DecoratedBox>(
      find.descendant(
        of: find.byType(AndroidBubbleCluster),
        matching: find.byType(DecoratedBox),
      ),
    )
    .map((box) => box.decoration)
    .whereType<BoxDecoration>()
    .firstWhere((decoration) => decoration.boxShadow != null);

/// The tonal mark behind a cluster button's icon (transparent: none).
Color? _mark(WidgetTester tester, Key key) {
  final cell = tester.widget<AnimatedContainer>(
    find.descendant(
      of: find.byKey(key),
      matching: find.byType(AnimatedContainer),
    ),
  );
  return (cell.decoration! as BoxDecoration).color;
}

/// The ink of a cluster button's icon.
Color? _iconInk(WidgetTester tester, Key key) => tester
    .widget<Icon>(
      find.descendant(of: find.byKey(key), matching: find.byType(Icon)),
    )
    .color;

/// The open card (the menu or a page) lies wholly inside [area] less 8 dp,
/// on the far side of the cluster from [edge], without covering it.
void _expectBeside(
  WidgetTester tester,
  Finder card,
  Rect area,
  AndroidBubbleEdge edge,
) {
  final rect = tester.getRect(card);
  final cluster = tester.getRect(find.byKey(const Key('session-bubble')));
  final inner = area.deflate(8);
  expect(rect.left, greaterThanOrEqualTo(inner.left - .01), reason: '$edge');
  expect(rect.top, greaterThanOrEqualTo(inner.top - .01), reason: '$edge');
  expect(rect.right, lessThanOrEqualTo(inner.right + .01), reason: '$edge');
  expect(rect.bottom, lessThanOrEqualTo(inner.bottom + .01), reason: '$edge');
  expect(rect.overlaps(cluster), isFalse, reason: '$edge');
  _expectAway(rect, cluster, edge);
}

void _expectAway(Rect rect, Rect cluster, AndroidBubbleEdge edge) {
  switch (edge) {
    case AndroidBubbleEdge.right:
      expect(rect.right, lessThanOrEqualTo(cluster.left));
    case AndroidBubbleEdge.left:
      expect(rect.left, greaterThanOrEqualTo(cluster.right));
    case AndroidBubbleEdge.top:
      expect(rect.top, greaterThanOrEqualTo(cluster.bottom));
    case AndroidBubbleEdge.bottom:
      expect(rect.bottom, lessThanOrEqualTo(cluster.top));
  }
}

void main() {
  testWidgets('200% labelled rail survives every frame of a drag rotation', (
    tester,
  ) async {
    final host = await _pump(tester, cluster: true, textScale: 2);
    await tester.dragFrom(
      tester.getCenter(find.byKey(const Key('quick-mode'))),
      const Offset(-140, -400),
    );
    for (var frame = 0; frame < 15; frame++) {
      await tester.pump(const Duration(milliseconds: 16));
      expect(tester.takeException(), isNull, reason: 'rotation frame $frame');
    }
    await tester.pumpAndSettle();
    expect(host.moves.last.$1, AndroidBubbleEdge.top);
    final cluster = tester.getRect(find.byKey(const Key('session-bubble')));
    expect(cluster.width, closeTo(144, .01));
    expect(cluster.height, closeTo(48, .01));
    for (final (key, label) in [
      (const Key('quick-mode'), '포인터'),
      (const Key('quick-keyboard'), '키보드'),
    ]) {
      final target = find.byKey(key);
      // The label's own paragraph (the icon beside it is drawn as text too).
      final rich = find.descendant(
        of: find.descendant(of: target, matching: find.text(label)),
        matching: find.byType(RichText),
      );
      final paragraph = tester.renderObject<RenderParagraph>(rich);
      expect(paragraph.text.toPlainText(), label);
      // The 48 dp cells follow the text size up to 130 % (9 dp -> 11.7).
      expect(paragraph.textScaler.scale(9), closeTo(11.7, .01));
      expect(paragraph.didExceedMaxLines, isFalse);
      expect(target.hitTestable(), findsOneWidget);
      expect(tester.getSize(target), const Size(48, 48));
    }
    expect(host.quickMode, 0, reason: 'dragging does not activate a tool');
    expect(host.remoteTaps, isEmpty);
  });

  for (final edge in [AndroidBubbleEdge.right, AndroidBubbleEdge.bottom]) {
    // At 200 % the labels used to double, break between syllables ("포인 / 터",
    // "키보 / 드") and push their icons out (inspection, 2026-11-12). The
    // cells follow the text size up to 130 %: one line, with the icon.
    testWidgets('rail labels keep one line and their icon at 200% on $edge', (
      tester,
    ) async {
      final host = await _pump(
        tester,
        cluster: true,
        edge: edge,
        textScale: 2,
      );
      for (final (key, label) in [
        (const Key('quick-mode'), '포인터'),
        (const Key('quick-keyboard'), '키보드'),
      ]) {
        final target = find.byKey(key);
        // The label's own paragraph (the icon beside it is drawn as text too).
        final text = find.descendant(
          of: find.descendant(of: target, matching: find.text(label)),
          matching: find.byType(RichText),
        );
        final paragraph = tester.renderObject<RenderParagraph>(text);
        final rect = tester.getRect(text);
        final button = tester.getRect(target);
        expect(paragraph.text.toPlainText(), label);
        expect(paragraph.textScaler.scale(9), closeTo(11.7, .01));
        expect(paragraph.didExceedMaxLines, isFalse);
        expect(rect.left, greaterThanOrEqualTo(button.left));
        expect(rect.right, lessThanOrEqualTo(button.right));
        expect(rect.top, greaterThanOrEqualTo(button.top));
        expect(rect.bottom, lessThanOrEqualTo(button.bottom));
        final boxes = paragraph.getBoxesForSelection(
          TextSelection(baseOffset: 0, extentOffset: label.length),
        );
        expect(boxes.map((box) => box.top.round()).toSet(), hasLength(1));
        expect(
          find.descendant(of: target, matching: find.byType(Icon)),
          findsOneWidget,
          reason: '$label keeps its icon',
        );
        expect(target.hitTestable(), findsOneWidget);
      }
      await tester.tap(find.byKey(const Key('quick-keyboard')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('quick-mode')));
      await tester.pumpAndSettle();
      expect((host.quickKeyboard, host.quickMode), (1, 1));
      expect(host.remoteTaps, isEmpty);
      expect(tester.takeException(), isNull);
    });
  }

  testWidgets('folded: one 48 dp amber bubble on the right edge by default', (
    tester,
  ) async {
    await _pump(tester);
    expect(_menu, findsNothing);
    final bubble = tester.getRect(_opener);
    final placed = androidBubbleRect(
      const Rect.fromLTWH(0, 0, 360, 800),
      AndroidBubbleEdge.right,
      .62,
    );
    expect(bubble.width, closeTo(48, .01));
    expect(bubble.height, closeTo(48, .01));
    expect(bubble.right, closeTo(360 - 8, .01));
    expect(bubble.top, closeTo(placed.top, .01));
    expect(find.byKey(const Key('session-bubble-latency')), findsOneWidget);
  });

  testWidgets('upright, the cluster reads ⋯, 입력 방식, 키보드 from the top '
      '(keyboard at the bottom); each is one tap that never reaches the '
      'remote', (tester) async {
    final host = await _pump(tester, cluster: true);
    final cluster = tester.getRect(find.byKey(const Key('session-bubble')));
    expect(cluster.width, closeTo(48, .01));
    expect(cluster.height, closeTo(144, .01));
    expect(cluster.right, closeTo(360 - 8, .01));
    final tools = tester.getRect(_opener);
    final mode = tester.getRect(find.byKey(const Key('quick-mode')));
    final keyboard = tester.getRect(find.byKey(const Key('quick-keyboard')));
    for (final cell in [keyboard, mode, tools]) {
      expect(cell.width, greaterThanOrEqualTo(48));
      expect(cell.height, greaterThanOrEqualTo(47.9));
    }
    expect(tools.bottom, closeTo(mode.top, .01));
    expect(mode.bottom, closeTo(keyboard.top, .01));
    // The latency dot sits on ⋯.
    final dot = tester.getRect(find.byKey(const Key('session-bubble-latency')));
    expect(tools.inflate(4).contains(dot.center), isTrue);
    expect(find.text('포인터'), findsOneWidget);
    await tester.tap(find.byKey(const Key('quick-keyboard')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('quick-mode')));
    await tester.pumpAndSettle();
    expect((host.quickKeyboard, host.quickMode), (1, 1));
    expect(_menu, findsNothing, reason: 'they do not open the menu');
    expect(host.remoteTaps, isEmpty);
    await _open(tester);
    expect(_menu, findsOneWidget);
    expect(host.remoteTaps, isEmpty);
  });

  testWidgets('across (top edge) the cluster reads ⋯, 입력 방식, 키보드 from '
      'the left', (tester) async {
    await _pump(tester, cluster: true, edge: AndroidBubbleEdge.top, along: .5);
    final tools = tester.getRect(_opener);
    final mode = tester.getRect(find.byKey(const Key('quick-mode')));
    final keyboard = tester.getRect(find.byKey(const Key('quick-keyboard')));
    expect(tools.right, closeTo(mode.left, .01));
    expect(mode.right, closeTo(keyboard.left, .01));
  });

  testWidgets('⋯ opens the menu beside the cluster; the cluster and its '
      'one-tap buttons stay; ⋯ again closes it', (tester) async {
    final host = await _pump(tester, cluster: true);
    await _open(tester);
    _expectBeside(
      tester,
      _menu,
      const Rect.fromLTWH(0, 0, 360, 800),
      AndroidBubbleEdge.right,
    );
    // Level with ⋯: the menu's top at ⋯'s top (upper half) or its bottom
    // at ⋯'s bottom (lower half), unless that would leave the screen.
    expect(find.byKey(const Key('quick-keyboard')), findsOneWidget);
    await tester.tap(find.byKey(const Key('quick-keyboard')));
    await tester.pumpAndSettle();
    expect(host.quickKeyboard, 1);
    expect(_menu, findsOneWidget, reason: 'the menu stays');
    await _open(tester);
    expect(_menu, findsNothing);
    expect(host.remoteTaps, isEmpty);
  });

  for (final (size, label) in const [
    (Size(360, 800), 'portrait'),
    (Size(915, 412), 'landscape'),
    (Size(1280, 800), 'tablet'),
  ]) {
    for (final edge in AndroidBubbleEdge.values) {
      for (final along in const [0.0, .5, 1.0]) {
        // A phone on its side has height only beside a cluster on the top or
        // bottom edge: the menu stands beside it there, on the wider side.
        final sideways = label == 'landscape' && !androidBubbleUpright(edge);
        testWidgets('$label: from a cluster on the ${edge.name} edge at '
            '$along the menu grows ${sideways ? 'beside it' : 'away from the edge'}, '
            'fully on screen', (tester) async {
          await _pump(
            tester,
            size: size,
            cluster: true,
            edge: edge,
            along: along,
          );
          await _open(tester);
          if (sideways) {
            final rect = tester.getRect(_menu);
            final cluster = tester.getRect(
              find.byKey(const Key('session-bubble')),
            );
            expect((Offset.zero & size).contains(rect.topLeft), isTrue);
            expect(rect.bottom, lessThanOrEqualTo(size.height - 8 + .01));
            expect(
              rect.right <= cluster.left || rect.left >= cluster.right,
              isTrue,
            );
            // Whole: nothing to scroll for 연결 끊기.
            final scroll = tester.state<ScrollableState>(
              find.descendant(of: _menu, matching: find.byType(Scrollable)),
            );
            expect(scroll.position.maxScrollExtent, 0);
          } else {
            _expectBeside(tester, _menu, Offset.zero & size, edge);
          }
          for (final key in ['cmd-screen', 'session-disconnect']) {
            expect(tester.getRect(find.byKey(Key(key))).height, 48);
          }
          expect(tester.takeException(), isNull);
        });
      }
    }
  }

  testWidgets('the phone keyboard shrinks the area: the menu stays inside '
      'it', (tester) async {
    const typing = Rect.fromLTWH(0, 0, 360, 452);
    await _pump(tester, cluster: true, area: typing);
    await _open(tester);
    _expectBeside(tester, _menu, typing, AndroidBubbleEdge.right);
    expect(tester.takeException(), isNull);
  });

  testWidgets('landscape with the keyboard open: the cluster lies across '
      'and the menu still fits beside it', (tester) async {
    const typing = Rect.fromLTWH(0, 0, 800, 200);
    await _pump(
      tester,
      size: const Size(800, 360),
      cluster: true,
      area: typing,
    );
    await _open(tester);
    _expectBeside(tester, _menu, typing, AndroidBubbleEdge.right);
    expect(tester.takeException(), isNull);
  });

  testWidgets('the menu lists the entries with 48 dp rows under the PC\'s '
      'group captions, the 연결 품질 row on top and 연결 끊기 last', (tester) async {
    final host = await _pump(tester);
    await _open(tester);
    // Which desktop this is: the title row, without a latency chip.
    expect(
      tester.widget<Text>(find.byKey(const Key('session-menu-title'))).data,
      'VM · 스트리밍',
    );
    for (final label in ['연결 품질', '화질', '마우스 잠금', '연결 끊기']) {
      expect(
        find.descendant(of: _menu, matching: find.text(label)),
        findsOneWidget,
        reason: label,
      );
    }
    // 화면 is the only row of its group and is named like it: no caption
    // over it. 설정 names the group of 화질 and 마우스 잠금.
    expect(
      find.descendant(of: _menu, matching: find.text('화면')),
      findsOneWidget,
    );
    expect(
      find.descendant(of: _menu, matching: find.text('설정')),
      findsOneWidget,
    );
    expect(_captions(tester), ['설정']);
    for (final gone in ['화면·영상', '내 기기 공유', '입력·도움말']) {
      expect(find.text(gone), findsNothing, reason: gone);
    }
    final screen = tester.getRect(find.byKey(const Key('cmd-screen')));
    final keys = tester.getRect(find.byKey(const Key('menu-keys')));
    final mouse = tester.getRect(find.byKey(const Key('cmd-game-mouse')));
    final disconnect = tester.getRect(
      find.byKey(const Key('session-disconnect')),
    );
    final quality = tester.getRect(find.byKey(const Key('session-quality')));
    final title = tester.getRect(find.byKey(const Key('session-menu-title')));
    expect(title.bottom, lessThanOrEqualTo(quality.top));
    expect(quality.height, greaterThanOrEqualTo(48));
    // 화면 follows 연결 품질 at once: no caption between them.
    expect(screen.top, closeTo(quality.bottom, .01));
    expect(screen.top, lessThan(keys.top));
    // 화질 starts the 설정 group: a gap and the caption after 화면.
    expect(keys.top - screen.bottom, greaterThan(8));
    // 마우스 잠금 is in the same group: it follows at once.
    expect(mouse.top, closeTo(keys.bottom, .01));
    expect(mouse.height, greaterThanOrEqualTo(48));
    // 연결 끊기 stands alone after a hairline.
    expect(disconnect.top, greaterThan(mouse.bottom));
    // 마우스 잠금 is a switch that shows it is on, as in the Windows 설정
    // panel; it no longer wears the chosen row's amber check.
    final mouseRow = find.byKey(const Key('cmd-game-mouse'));
    expect(
      tester
          .widget<Switch>(
            find.descendant(
              of: mouseRow,
              matching: find.byKey(const Key('android-session-row-switch')),
            ),
          )
          .value,
      isTrue,
    );
    expect(
      find.descendant(of: mouseRow, matching: find.byIcon(Icons.check_rounded)),
      findsNothing,
    );
    await tester.tap(find.byKey(const Key('session-disconnect')));
    await tester.pumpAndSettle();
    expect(host.disconnects, 1);
    expect(_menu, findsNothing);
  });

  testWidgets('a command runs and closes the menu', (tester) async {
    final host = await _pump(tester);
    await _open(tester);
    await tester.tap(find.byKey(const Key('cmd-screen')));
    await tester.pumpAndSettle();
    expect(host.screen, 1);
    expect(_menu, findsNothing);
    expect(host.remoteTaps, isEmpty);
  });

  testWidgets('a menu opens its rows at the same spot; back shows the menu; '
      'a row runs and closes', (tester) async {
    final host = await _pump(tester);
    await _open(tester);
    final menu = tester.getRect(_menu);
    await tester.tap(find.byKey(const Key('menu-keys')));
    await tester.pumpAndSettle();
    expect(_page, findsOneWidget);
    expect(_menu, findsNothing, reason: 'the page replaces the menu');
    final page = tester.getRect(_page);
    expect(page.right, closeTo(menu.right, .01), reason: 'the same spot');
    expect(host.popups.isEmpty, isFalse, reason: 'tracked as an open popup');
    final row = tester.getRect(find.byKey(const Key('item-windows')));
    expect(row.height, greaterThanOrEqualTo(48));
    // A greyed row says why and does nothing.
    final off = tester.widget<AndroidBarMenuRow>(
      find.ancestor(
        of: find.byKey(const Key('item-off')),
        matching: find.byType(AndroidBarMenuRow),
      ),
    );
    expect(off.enabled, isFalse);
    await tester.tap(find.byKey(const Key('session-menu-back')));
    await tester.pumpAndSettle();
    expect(_menu, findsOneWidget);
    expect(host.popups.isEmpty, isTrue);
    await tester.tap(find.byKey(const Key('menu-keys')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('item-windows')));
    await tester.pumpAndSettle();
    expect(host.chosen, ['windows']);
    expect(_menu, findsNothing);
    expect(_page, findsNothing);
    expect(host.popups.isEmpty, isTrue);
  });

  testWidgets('Back goes one level up: a page to the menu, the menu closed', (
    tester,
  ) async {
    final host = await _pump(tester);
    final bar = host.barKey.currentState!;
    expect(bar.back(), isFalse, reason: 'nothing open');
    await _open(tester);
    await tester.tap(find.byKey(const Key('menu-keys')));
    await tester.pumpAndSettle();
    expect(bar.back(), isTrue);
    await tester.pumpAndSettle();
    expect(_menu, findsOneWidget);
    expect(bar.back(), isTrue);
    await tester.pumpAndSettle();
    expect(_menu, findsNothing);
    expect(bar.isOpen, isFalse);
  });

  testWidgets('C hamburger closes a page and reopens at the main menu', (
    tester,
  ) async {
    await _pump(tester);
    await _open(tester);
    await tester.tap(find.byKey(const Key('menu-keys')));
    await tester.pumpAndSettle();
    await _open(tester);
    expect(_page, findsNothing);
    expect(_menu, findsNothing);
    await _open(tester);
    expect(_menu, findsOneWidget);
    expect(_page, findsNothing);
    await tester.tap(find.byKey(const Key('menu-keys')));
    await tester.pumpAndSettle();
    await tester.sendKeyEvent(LogicalKeyboardKey.escape);
    await tester.pumpAndSettle();
    expect(_page, findsNothing);
    expect(_menu, findsNothing);
    expect(
      FocusManager.instance.primaryFocus?.debugLabel,
      'Android menu toggle',
    );
  });

  testWidgets('a flat list (화면, 배율, 화질, 마우스 잠금) keeps the '
      '화면 and 설정 captions with the gap between the groups', (tester) async {
    await _pump(tester, scaleRow: true);
    await _open(tester);
    expect(_captions(tester), ['화면', '설정']);
    // 화면 names its group and its row.
    expect(
      find.descendant(of: _menu, matching: find.text('화면')),
      findsNWidgets(2),
    );
    final quality = tester.getRect(find.byKey(const Key('session-quality')));
    final screen = tester.getRect(find.byKey(const Key('cmd-screen')));
    final scale = tester.getRect(find.byKey(const Key('cmd-scale')));
    final keys = tester.getRect(find.byKey(const Key('menu-keys')));
    // The 화면 caption stands between 연결 품질 and the 화면 row.
    expect(screen.top, greaterThan(quality.bottom));
    expect(scale.top, closeTo(screen.bottom, .01));
    expect(keys.top - scale.bottom, greaterThan(8));
  });

  testWidgets('the 연결 품질 row shows the measured latency with its word '
      'and, without a panel of the host\'s, opens its page in the card', (
    tester,
  ) async {
    final host = await _pump(tester);
    await _open(tester);
    final quality = find.byKey(const Key('session-quality'));
    final row = tester.widget<AndroidBarMenuRow>(
      find.ancestor(of: quality, matching: find.byType(AndroidBarMenuRow)),
    );
    expect(row.item.label, '연결 품질');
    expect(row.description, '12 ms · 좋음');
    expect(row.opens, isTrue);
    expect(
      find.descendant(of: quality, matching: find.text('12 ms · 좋음')),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: quality,
        matching: find.byIcon(Icons.chevron_right_rounded),
      ),
      findsOneWidget,
    );
    await tester.tap(quality);
    await tester.pumpAndSettle();
    expect(host.quality, 0);
    expect(_page, findsOneWidget);
    expect(_menu, findsNothing, reason: 'the page replaces the menu');
    expect(find.text('연결 품질'), findsOneWidget);
    expect(find.text('왕복 지연 · Host 응답'), findsOneWidget);
    expect(
      find.descendant(of: _page, matching: find.text('12 ms')),
      findsOneWidget,
    );
    expect(find.text('해상도 · 원격 화면'), findsOneWidget);
  });

  testWidgets('the 연결 품질 row calls the host\'s onQuality instead of '
      'opening the page in the card', (tester) async {
    final host = await _pump(tester, ownQuality: true);
    await _open(tester);
    await tester.tap(find.byKey(const Key('session-quality')));
    await tester.pumpAndSettle();
    expect(host.quality, 1);
    expect(_page, findsNothing);
    expect(find.text('왕복 지연 · Host 응답'), findsNothing);
    // The host shows its panel through AndroidSessionBar.panel; this one
    // gave none, so the menu is still there.
    expect(_menu, findsOneWidget);
  });

  for (final (ms, text) in const <(double?, String)>[
    (58.0, '58 ms · 보통'),
    (142.0, '142 ms · 느림'),
    (null, '연결됨'),
  ]) {
    testWidgets('the 연결 품질 row reads "$text" at $ms ms', (tester) async {
      await _pump(tester, latencyMs: ms);
      await _open(tester);
      expect(
        find.descendant(
          of: find.byKey(const Key('session-quality')),
          matching: find.text(text),
        ),
        findsOneWidget,
      );
    });
  }

  testWidgets('closing every popup takes an open page back to the menu', (
    tester,
  ) async {
    final host = await _pump(tester);
    await _open(tester);
    await tester.tap(find.byKey(const Key('menu-keys')));
    await tester.pumpAndSettle();
    host.popups.closeAll();
    await tester.pumpAndSettle();
    expect(_page, findsNothing);
    expect(_menu, findsOneWidget);
  });

  testWidgets('a touch outside the open menu closes it and never reaches '
      'the remote picture', (tester) async {
    final host = await _pump(tester);
    await _open(tester);
    await tester.tapAt(const Offset(20, 40));
    await tester.pumpAndSettle();
    expect(_menu, findsNothing);
    expect(host.remoteTaps, isEmpty);
    // Closed, touches reach the picture again.
    await tester.tapAt(const Offset(20, 40));
    await tester.pump();
    expect(host.remoteTaps, [const Offset(20, 40)]);
  });

  testWidgets('touches outside the folded bubble reach the remote picture; '
      'a tap on it opens the menu and never reaches the remote', (
    tester,
  ) async {
    final host = await _pump(tester);
    final bubble = tester.getRect(_opener);
    for (final at in [
      const Offset(10, 10),
      const Offset(180, 20),
      bubble.centerLeft - const Offset(4, 0), // just left of the bubble
      const Offset(340, 790),
    ]) {
      await tester.tapAt(at);
      await tester.pump();
    }
    expect(host.remoteTaps, hasLength(4));
    await tester.tapAt(bubble.center);
    await tester.pumpAndSettle();
    expect(host.remoteTaps, hasLength(4));
    expect(_menu, findsOneWidget);
  });

  testWidgets('while a finger that began on the picture is down, the menu '
      'takes no touches: one on it closes the menu and reaches nothing', (
    tester,
  ) async {
    final host = await _pump(tester);
    final gesture = await tester.startGesture(const Offset(100, 300));
    await tester.pump();
    expect(host.remoteTaps, hasLength(1));
    // A second finger on ⋯ opens the menu while the first is still down.
    await tester.tap(_opener);
    await tester.pumpAndSettle();
    expect(_menu, findsOneWidget);
    await tester.tap(find.byKey(const Key('cmd-screen')), warnIfMissed: false);
    await tester.pumpAndSettle();
    expect(host.screen, 0);
    expect(_menu, findsNothing);
    expect(host.remoteTaps, hasLength(1));
    await gesture.up();
    await tester.pumpAndSettle();
    await _open(tester);
    await tester.tap(find.byKey(const Key('cmd-screen')));
    await tester.pumpAndSettle();
    expect(host.screen, 1);
  });

  testWidgets('the cluster moves as one: dragged from any button it snaps to '
      'the top edge and lies across there', (tester) async {
    final host = await _pump(tester, cluster: true);
    await tester.dragFrom(
      tester.getCenter(find.byKey(const Key('quick-mode'))),
      const Offset(-140, -400),
    );
    await tester.pumpAndSettle();
    expect(host.moves.last.$1, AndroidBubbleEdge.top);
    final cluster = tester.getRect(find.byKey(const Key('session-bubble')));
    expect(cluster.width, closeTo(144, .01));
    expect(cluster.height, closeTo(48, .01));
    expect(cluster.top, closeTo(8, .01));
    expect(host.quickMode, 0, reason: 'a drag is not a tap');
    expect(host.remoteTaps, isEmpty);
  });

  testWidgets('a drag of the cluster closes an open menu first', (
    tester,
  ) async {
    final host = await _pump(tester, cluster: true);
    await _open(tester);
    await tester.dragFrom(
      tester.getCenter(find.byKey(const Key('quick-mode'))),
      const Offset(-140, -400),
    );
    await tester.pumpAndSettle();
    expect(_menu, findsNothing);
    expect(host.moves.last.$1, AndroidBubbleEdge.top);
  });

  testWidgets('a short area (landscape with the keyboard open) turns the '
      'upright cluster across at the same edge', (tester) async {
    await _pump(
      tester,
      size: const Size(800, 360),
      cluster: true,
      area: const Rect.fromLTWH(0, 0, 800, 52),
    );
    final cluster = tester.getRect(find.byKey(const Key('session-bubble')));
    expect(cluster.width, closeTo(144, .01));
    expect(cluster.height, closeTo(48, .01));
    expect(cluster.right, closeTo(800 - 8, .01));
    expect(cluster.top, greaterThanOrEqualTo(0));
    expect(cluster.bottom, lessThanOrEqualTo(52));
  });

  testWidgets('a remembered place is where the bubble starts', (tester) async {
    await _pump(tester, edge: AndroidBubbleEdge.left, along: .25);
    final bubble = tester.getRect(_opener);
    expect(bubble.left, 8);
    expect(bubble.top, closeTo(8 + .25 * (800 - 16 - 48), .01));
  });

  testWidgets('a drag moves the bubble; let go, it snaps to the nearest edge '
      'and reports where; nothing reaches the remote', (tester) async {
    final host = await _pump(tester);
    final from = tester.getCenter(_opener);
    final gesture = await tester.startGesture(from);
    await gesture.moveBy(const Offset(-40, 0));
    await gesture.moveTo(const Offset(60, 300));
    await tester.pump();
    // It follows the finger while held.
    expect(tester.getCenter(_opener).dx, closeTo(60, 1));
    await gesture.up();
    await tester.pumpAndSettle();
    expect(host.moves, hasLength(1));
    expect(host.moves.single.$1, AndroidBubbleEdge.left);
    expect(
      host.moves.single.$2,
      closeTo((300 - 24 - 8) / (800 - 16 - 48), .01),
    );
    final bubble = tester.getRect(_opener);
    expect(bubble.left, 8, reason: 'snapped onto the left edge');
    expect(bubble.center.dy, closeTo(300, 1));
    expect(host.remoteTaps, isEmpty);
    expect(_menu, findsNothing, reason: 'a drag does not open the menu');
    // Near the top it rests on the top edge.
    await tester.dragFrom(tester.getCenter(_opener), const Offset(140, -270));
    await tester.pumpAndSettle();
    expect(host.moves.last.$1, AndroidBubbleEdge.top);
    expect(tester.getRect(_opener).top, 8);
    expect(host.remoteTaps, isEmpty);
  });

  testWidgets('the bubble fades after 3 s alone, not while its menu is '
      'open, and comes back on a touch', (tester) async {
    final host = await _pump(tester);
    double opacity() => tester
        .widget<AnimatedOpacity>(find.byKey(const Key('session-bubble')))
        .opacity;
    expect(opacity(), 1);
    await tester.pump(const Duration(seconds: 2));
    expect(opacity(), 1);
    await tester.pump(const Duration(seconds: 2));
    await tester.pumpAndSettle();
    expect(opacity(), AndroidSessionBar.fadedOpacity);
    expect(host.barKey.currentState!.bubbleFaded, isTrue);
    // A faded bubble still takes the touch: a finger on it wakes it.
    final gesture = await tester.startGesture(tester.getCenter(_opener));
    await tester.pump();
    expect(opacity(), 1);
    await gesture.up();
    await tester.pumpAndSettle();
    expect(_menu, findsOneWidget, reason: 'the tap opened the menu');
    expect(host.remoteTaps, isEmpty);
    await tester.pump(const Duration(seconds: 5));
    await tester.pumpAndSettle();
    expect(opacity(), 1, reason: 'no fade while the menu is open');
    // Closed again it fades again when left alone.
    await tester.tap(find.byKey(const Key('session-toolbar-fold')));
    await tester.pumpAndSettle();
    expect(opacity(), 1);
    await tester.pump(const Duration(seconds: 4));
    await tester.pumpAndSettle();
    expect(opacity(), AndroidSessionBar.fadedOpacity);
  });

  testWidgets('a narrow phone (320 dp): too little room beside the cluster, '
      'the menu takes the larger room above or below it, whole and readable '
      'even with enlarged text', (tester) async {
    tester.platformDispatcher.textScaleFactorTestValue = 2;
    addTearDown(tester.platformDispatcher.clearAllTestValues);
    for (final along in const [.1, .9]) {
      await _pump(
        tester,
        size: const Size(320, 700),
        cluster: true,
        along: along,
      );
      await _open(tester);
      expect(tester.takeException(), isNull);
      final rect = tester.getRect(_menu);
      final cluster = tester.getRect(find.byKey(const Key('session-bubble')));
      expect(rect.overlaps(cluster), isFalse);
      expect(
        const Rect.fromLTRB(8, 8, 312, 692).contains(rect.topLeft),
        isTrue,
      );
      expect(rect.right, lessThanOrEqualTo(312.01));
      expect(rect.bottom, lessThanOrEqualTo(692.01));
      expect(rect.width, greaterThan(260), reason: 'wider than beside it');
      _expectAway(
        rect,
        cluster,
        along < .5 ? AndroidBubbleEdge.top : AndroidBubbleEdge.bottom,
      );
      expect(
        tester.getRect(find.byKey(const Key('cmd-screen'))).height,
        greaterThanOrEqualTo(48),
      );
    }
  });

  testWidgets('a folded hinge keeps the menu on the cluster\'s side', (
    tester,
  ) async {
    await _pump(
      tester,
      size: const Size(1000, 600),
      features: const [
        DisplayFeature(
          bounds: Rect.fromLTWH(480, 0, 40, 600),
          type: DisplayFeatureType.hinge,
          state: DisplayFeatureState.postureFlat,
        ),
      ],
    );
    await _open(tester);
    expect(tester.getRect(_menu).left, greaterThanOrEqualTo(520));
  });

  testWidgets('the cluster is one solid amber rail with dark ink on its '
      'icons and labels, upright and across', (tester) async {
    for (final edge in [AndroidBubbleEdge.right, AndroidBubbleEdge.top]) {
      await _pump(tester, cluster: true, edge: edge, along: .5);
      expect(_rail(tester).color, _amber, reason: '$edge');
      for (final (key, label) in [
        (const Key('expand-session-toolbar'), '메뉴'),
        (const Key('quick-mode'), '포인터'),
        (const Key('quick-keyboard'), '키보드'),
      ]) {
        expect(_iconInk(tester, key), _amberInk, reason: '$label icon · $edge');
        final text = tester.widget<Text>(
          find.descendant(of: find.byKey(key), matching: find.text(label)),
        );
        expect(text.style?.color, _amberInk, reason: '$label label · $edge');
        expect(
          _mark(tester, key),
          Colors.transparent,
          reason: '$label · $edge',
        );
      }
    }
  });

  testWidgets('only the tool that is on wears the darker mark: ⋯ while its '
      'menu is open', (tester) async {
    await _pump(tester, cluster: true);
    const tools = Key('expand-session-toolbar');
    expect(_mark(tester, tools), Colors.transparent);
    await _open(tester);
    expect(_mark(tester, tools), _amberInk.withValues(alpha: .16));
    for (final key in const [Key('quick-mode'), Key('quick-keyboard')]) {
      expect(_mark(tester, key), Colors.transparent, reason: '$key');
    }
    // The rail stays amber and the ink dark, whatever is on.
    expect(_rail(tester).color, _amber);
    expect(_iconInk(tester, tools), _amberInk);
    await _open(tester);
    expect(_menu, findsNothing);
    expect(_mark(tester, tools), Colors.transparent);
  });

  testWidgets('a one-tap tool that is on (the open keyboard) gets the mark '
      'and keeps the dark ink', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: Center(
            child: SizedBox(
              width: 48,
              height: 96,
              child: AndroidBubbleCluster(
                buttons: [
                  AndroidQuickButton(
                    key: const Key('quick-mode'),
                    icon: Icons.mouse_rounded,
                    label: '포인터',
                    tooltip: '입력 방식',
                    onPressed: () {},
                  ),
                  AndroidQuickButton(
                    key: const Key('quick-keyboard'),
                    icon: Icons.keyboard_hide_rounded,
                    tooltip: '키보드 닫기',
                    active: true,
                    onPressed: () {},
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(_rail(tester).color, _amber);
    expect(_mark(tester, const Key('quick-mode')), Colors.transparent);
    expect(
      _mark(tester, const Key('quick-keyboard')),
      _amberInk.withValues(alpha: .16),
    );
    for (final key in const [Key('quick-mode'), Key('quick-keyboard')]) {
      expect(_iconInk(tester, key), _amberInk, reason: '$key');
    }
    // Nothing measured: no dot.
    expect(find.byKey(const Key('session-bubble-latency')), findsNothing);
  });

  testWidgets('the latency dot takes the light chrome\'s colours (the darker '
      'ones, on amber) with an amber rim; none before a measurement', (
    tester,
  ) async {
    const light = SessionChrome.forDark(false);
    for (final (ms, colour) in [
      (12.0, light.good),
      (39.0, light.good),
      (40.0, light.fair),
      (79.0, light.fair),
      (80.0, light.poor),
      (240.0, light.poor),
    ]) {
      await _pump(tester, latencyMs: ms);
      final dot = tester.widget<Container>(
        find.byKey(const Key('session-bubble-latency')),
      );
      final decoration = dot.decoration! as BoxDecoration;
      expect(decoration.color, colour, reason: '$ms ms');
      expect(decoration.color, light.latencyColor(ms), reason: '$ms ms');
      expect(
        decoration.color,
        isNot(const SessionChrome.forDark(true).latencyColor(ms)),
        reason: '$ms ms: not the dark chrome\'s',
      );
      expect(decoration.shape, BoxShape.circle);
      expect(decoration.border, Border.all(color: _amber, width: 2));
    }
    await _pump(tester, latencyMs: null);
    expect(find.byKey(const Key('session-bubble-latency')), findsNothing);
    expect(_rail(tester).color, _amber);
  });

  group('the bubble\'s place', () {
    const phone = Rect.fromLTWH(0, 0, 360, 800);

    test('each edge, inset from the area and inside it', () {
      for (final edge in AndroidBubbleEdge.values) {
        for (final along in [0.0, .5, 1.0]) {
          final rect = androidBubbleRect(phone, edge, along);
          expect(phone.deflate(8).contains(rect.topLeft), isTrue);
          expect(phone.deflate(7.9).contains(rect.bottomRight), isTrue);
        }
      }
      expect(
        androidBubbleRect(phone, AndroidBubbleEdge.top, 0).topLeft,
        const Offset(8, 8),
      );
      expect(
        androidBubbleRect(phone, AndroidBubbleEdge.bottom, 1).bottomRight,
        const Offset(352, 792),
      );
    });

    test('the phone keyboard shrinks the area: the bubble stays inside at '
        'the same proportion', () {
      const typing = Rect.fromLTWH(0, 0, 360, 452);
      final rect = androidBubbleRect(typing, AndroidBubbleEdge.right, .62);
      expect(rect.bottom, lessThanOrEqualTo(452 - 8));
      expect(rect.top, closeTo(8 + .62 * (452 - 16 - 48), .01));
      // Only just room: the margin gives way, the bubble stays whole.
      const tight = Rect.fromLTWH(0, 0, 360, 52);
      final squeezed = androidBubbleRect(tight, AndroidBubbleEdge.right, .5);
      expect(squeezed.top, greaterThanOrEqualTo(0));
      expect(squeezed.bottom, lessThanOrEqualTo(52));
    });

    test('a cluster lies along its edge and keeps its proportion', () {
      for (final edge in AndroidBubbleEdge.values) {
        final layout = androidBubbleLayout(phone, edge, .5, length: 144);
        expect(layout.upright, androidBubbleUpright(edge));
        expect(phone.deflate(7.9).contains(layout.rect.topLeft), isTrue);
        expect(phone.deflate(7.9).contains(layout.rect.bottomRight), isTrue);
      }
      final (edge, along) = androidBubbleSnap(
        phone,
        const Offset(330, 300),
        length: 144,
      );
      expect(edge, AndroidBubbleEdge.right);
      final back = androidBubbleLayout(phone, edge, along, length: 144).rect;
      expect(back.center.dy, closeTo(300, .01));
    });

    test('a bubble let go snaps to the nearest edge, its spot kept', () {
      expect(
        androidBubbleSnap(phone, const Offset(30, 400)).$1,
        AndroidBubbleEdge.left,
      );
      expect(
        androidBubbleSnap(phone, const Offset(340, 100)).$1,
        AndroidBubbleEdge.right,
      );
      expect(
        androidBubbleSnap(phone, const Offset(180, 20)).$1,
        AndroidBubbleEdge.top,
      );
      expect(
        androidBubbleSnap(phone, const Offset(180, 785)).$1,
        AndroidBubbleEdge.bottom,
      );
      final (edge, along) = androidBubbleSnap(phone, const Offset(300, 120));
      expect(
        androidBubbleRect(phone, edge, along).center.dy,
        closeTo(120, .01),
      );
      // Landscape: the same rules on a wide, short area.
      const wide = Rect.fromLTWH(0, 0, 915, 412);
      expect(
        androidBubbleSnap(wide, const Offset(880, 200)).$1,
        AndroidBubbleEdge.right,
      );
      expect(
        androidBubbleSnap(wide, const Offset(450, 390)).$1,
        AndroidBubbleEdge.bottom,
      );
    });

    test('the menu starts level with ⋯ and grows towards the middle', () {
      const area = Rect.fromLTWH(0, 0, 360, 800);
      // ⋯ high on the right edge: the menu hangs down from its top.
      final high = AndroidMenuPlacement(
        area: area,
        cluster: const Rect.fromLTWH(304, 100, 48, 144),
        edge: AndroidBubbleEdge.right,
        width: 288,
      );
      expect(
        high.getPositionForChild(area.size, const Size(288, 400)),
        const Offset(304 - 8 - 288, 100),
      );
      // Low: it rises from ⋯'s bottom, and slides inside the area.
      final low = AndroidMenuPlacement(
        area: area,
        cluster: const Rect.fromLTWH(304, 600, 48, 144),
        edge: AndroidBubbleEdge.right,
        width: 288,
      );
      expect(
        low.getPositionForChild(area.size, const Size(288, 400)),
        const Offset(304 - 8 - 288, 648 - 400),
      );
      expect(
        low.getPositionForChild(area.size, const Size(288, 760)).dy,
        8,
        reason: 'taller than the room above ⋯: it slides down to fit',
      );
      // The top edge: below the cluster, from ⋯'s left.
      final top = AndroidMenuPlacement(
        area: area,
        cluster: const Rect.fromLTWH(30, 8, 144, 48),
        edge: AndroidBubbleEdge.top,
        width: 288,
      );
      expect(
        top.getPositionForChild(area.size, const Size(288, 300)),
        const Offset(30, 64),
      );
      expect(
        top.getConstraintsForChild(const BoxConstraints()).maxHeight,
        800 - 64 - 8,
      );
    });

    test('this phone remembers where the bubble rests', () async {
      TestWidgetsFlutterBinding.ensureInitialized();
      final store = MemoryStore()..install();
      addTearDown(
        () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(AndroidStore.channel, null),
      );
      expect(
        (await AndroidStore().sessionSettings()).toolbarEdge,
        AndroidBubbleEdge.right,
      );
      await AndroidStore().saveSessionSettings(
        const AndroidSessionSettings(
          toolbarEdge: AndroidBubbleEdge.top,
          toolbar: .25,
        ),
      );
      expect(store.prefs['session.toolbar.edge'], 'top');
      final read = await AndroidStore().sessionSettings();
      expect((read.toolbarEdge, read.toolbar), (AndroidBubbleEdge.top, .25));
    });
  });

  group('androidToolbarArea', () {
    test('sits inside the system bars', () {
      final area = androidToolbarArea(
        const Size(800, 360),
        const EdgeInsets.fromLTRB(48, 24, 48, 0),
        const [],
      );
      expect((area.left, area.right, area.top), (48, 752, 24));
    });

    test('a cutout is kept clear by the padding alone', () {
      final area = androidToolbarArea(
        const Size(360, 800),
        const EdgeInsets.only(top: 40),
        const [
          DisplayFeature(
            bounds: Rect.fromLTWH(150, 0, 60, 40),
            type: DisplayFeatureType.cutout,
            state: DisplayFeatureState.unknown,
          ),
        ],
      );
      expect((area.left, area.right), (0, 360));
    });

    test('a vertical hinge in the top band: the wider side is used', () {
      final area = androidToolbarArea(
        const Size(1000, 700),
        const EdgeInsets.only(top: 24),
        const [
          DisplayFeature(
            bounds: Rect.fromLTWH(480, 0, 40, 700),
            type: DisplayFeatureType.hinge,
            state: DisplayFeatureState.postureFlat,
          ),
        ],
      );
      // Both sides are 480 wide; the first one.
      expect((area.left, area.right), (0, 480));
      final right = androidToolbarArea(
        const Size(1000, 700),
        EdgeInsets.zero,
        const [
          DisplayFeature(
            bounds: Rect.fromLTWH(300, 0, 40, 700),
            type: DisplayFeatureType.fold,
            state: DisplayFeatureState.postureFlat,
          ),
        ],
      );
      expect((right.left, right.right), (340, 1000));
    });

    test('a horizontal hinge outside the band changes nothing', () {
      for (final bottom in [false, true]) {
        final area = androidToolbarArea(
          const Size(700, 1000),
          EdgeInsets.zero,
          const [
            DisplayFeature(
              bounds: Rect.fromLTWH(0, 480, 700, 40),
              type: DisplayFeatureType.hinge,
              state: DisplayFeatureState.postureHalfOpened,
            ),
          ],
          bottom: bottom,
        );
        expect((area.left, area.right), (0, 700));
      }
    });

    test('the bottom band sits above the system bar', () {
      final area = androidToolbarArea(
        const Size(360, 800),
        const EdgeInsets.only(bottom: 48),
        const [],
        bottom: true,
      );
      expect((area.top, area.bottom), (800 - 48 - 64, 800 - 48));
    });
  });
}

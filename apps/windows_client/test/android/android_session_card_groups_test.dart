// The Android session menu card's groups (regrouped 2026-11-10; the PC's bar
// as rows 2026-11-11): 연결 품질 first, a caption (화면 · 설정) only over a
// group that has rows of its own, 마우스 잠금 as a switch row, and the
// emergency console's own commands without a caption. The bar alone with
// fake commands; no session, device or host.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_toolbar.dart';
import 'package:multitap_client/ui/session_menu.dart';
import 'package:multitap_client/ui/session_tools.dart';

class _Host {
  final hold = AndroidPointerHold();

  /// The commands that ran, by id.
  final used = <String>[];
  int disconnects = 0;
}

Future<_Host> _pump(
  WidgetTester tester,
  List<SessionToolbarMenu> Function(_Host host) actions,
) async {
  const size = Size(360, 800);
  final host = _Host();
  addTearDown(host.hold.dispose);
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(
      home: Scaffold(
        body: AndroidSessionBar(
          title: 'VM',
          modeLabel: '스트리밍',
          latencyMs: 12,
          pointerHeld: host.hold.held,
          bubbleArea: Offset.zero & size,
          onInteract: () {},
          onDisconnect: () => host.disconnects++,
          actions: actions(host),
        ),
      ),
    ),
  );
  await tester.pumpAndSettle();
  return host;
}

/// The phone session's entries as AndroidRemoteView lists them now
/// (`_barActions`): 화면 and 설정, each handing over to the host
/// (`onPressedAt`) for its panel.
List<SessionToolbarMenu> _real(_Host host) => [
  for (final (id, tool, tooltip) in [
    ('screen', SessionTool.screen, '화면 · 보기, 배율, 해상도'),
    ('more', SessionTool.more, '설정 · 화질, 마우스 잠금, 입력 방식, 사용안내'),
  ])
    SessionToolbarMenu.command(
      key: Key('android-session-$id'),
      keepOpenInOverflow: true,
      label: tool.androidLabel,
      tooltip: tooltip,
      icon: tool.icon,
      onInteract: () {},
      disabledReason: '연결된 뒤 바꿀 수 있어요',
      onPressed: () => host.used.add(id),
      onPressedAt: (_) async {
        host.used.add(id);
        return false;
      },
    ),
];

/// The flat list a host may still give (AndroidRemoteView's until
/// 2026-11-11): 화면, 배율 (where the Host sets it), 화질, 마우스
/// 잠금 (where granted; [mouse]: one is connected, [locked]: it is on) and
/// 사용안내. The panel rows hand over to the host (`onPressedAt`).
List<SessionToolbarMenu> _phone(
  _Host host, {
  bool scale = true,
  bool mouseOffered = true,
  bool mouse = true,
  bool locked = false,
}) {
  SessionToolbarMenu panel(String id, SessionTool tool, String tooltip) =>
      SessionToolbarMenu.command(
        key: Key('android-session-$id'),
        keepOpenInOverflow: true,
        label: tool.androidLabel,
        tooltip: tooltip,
        icon: tool.icon,
        onInteract: () {},
        disabledReason: '연결된 뒤 바꿀 수 있어요',
        onPressed: () => host.used.add(id),
        onPressedAt: (_) async {
          host.used.add(id);
          return false;
        },
      );
  return [
    panel('screen', SessionTool.screen, '화면 · 해상도, 전체 보기, 화면 이동'),
    if (scale)
      panel('scale', SessionTool.scale, 'Windows 글자 크기 · 원격 해상도와 별개예요'),
    panel('settings', SessionTool.settings, '화질 · 프레임·전송률'),
    if (mouseOffered)
      SessionToolbarMenu.command(
        key: const Key('android-session-game-mouse'),
        label: SessionTool.gameMouse.label,
        tooltip: locked ? '마우스 잠금 끄기 · 뒤로 가기로 풀기' : '마우스 잠금 · 마우스를 데스크톱 안에 잠가요',
        icon: SessionTool.gameMouse.icon,
        active: locked,
        disabledReason: '마우스를 연결하면 쓸 수 있어요',
        onInteract: () {},
        onPressed: mouse ? () => host.used.add('game-mouse') : null,
      ),
    panel('help', SessionTool.guide, '터치 조작 안내'),
  ];
}

/// The emergency console's entries as AndroidConsoleView lists them
/// (`_actions`): 키보드 and 키 조합 are its own, outside the vocabulary.
List<SessionToolbarMenu> _console(_Host host) => [
  SessionToolbarMenu.command(
    label: '키보드',
    tooltip: '콘솔 키보드',
    key: const Key('console-keyboard-menu'),
    icon: Icons.keyboard_rounded,
    onInteract: () {},
    onPressed: () => host.used.add('keyboard'),
  ),
  SessionToolbarMenu(
    label: '화면',
    tooltip: '화면 맞춤 · 원본 크기',
    icon: Icons.fit_screen_rounded,
    onInteract: () {},
    items: [
      SessionMenuItem(
        '화면 맞춤',
        checked: true,
        onSelected: () => host.used.add('fit'),
      ),
      SessionMenuItem('원본 크기', onSelected: () => host.used.add('original')),
    ],
  ),
  SessionToolbarMenu(
    label: '키 조합',
    tooltip: '키 조합',
    icon: Icons.keyboard_command_key_rounded,
    onInteract: () {},
    items: [
      SessionMenuItem(
        'Ctrl+Alt+Del',
        key: const Key('console-secure-attention'),
        onSelected: () => host.used.add('secure'),
      ),
    ],
  ),
];

Finder get _opener => find.byKey(const Key('expand-session-toolbar'));
Finder get _menu => find.byKey(const Key('session-command-bar'));
Finder get _mouseRow => find.byKey(const Key('android-session-game-mouse'));

Future<void> _open(WidgetTester tester) async {
  await tester.tap(_opener);
  await tester.pumpAndSettle();
}

/// The open card's group captions, top to bottom.
List<String> _captions(WidgetTester tester) => [
  for (final caption in tester.widgetList<AndroidMenuCaption>(
    find.descendant(of: _menu, matching: find.byType(AndroidMenuCaption)),
  ))
    caption.label,
];

/// The open card's rows, top to bottom (연결 품질 and 연결 끊기 included).
List<String> _rows(WidgetTester tester) => [
  for (final row in tester.widgetList<AndroidBarMenuRow>(
    find.descendant(of: _menu, matching: find.byType(AndroidBarMenuRow)),
  ))
    row.item.label,
];

Finder _caption(String label) => find.descendant(
  of: _menu,
  matching: find.byWidgetPredicate(
    (widget) => widget is AndroidMenuCaption && widget.label == label,
  ),
);

Finder _row(String label) => find.descendant(
  of: _menu,
  matching: find.byWidgetPredicate(
    (widget) => widget is AndroidBarMenuRow && widget.item.label == label,
  ),
);

/// The 마우스 잠금 row's widget and its switch.
AndroidBarMenuRow _mouseWidget(WidgetTester tester) =>
    tester.widget<AndroidBarMenuRow>(_row('마우스 잠금'));

Finder get _mouseSwitch => find.descendant(
  of: _mouseRow,
  matching: find.byKey(const Key('android-session-row-switch')),
);

/// 마우스 잠금 never looks like a chosen row: no amber check and no tinted
/// background, whatever its switch says.
void _expectNotChosen(WidgetTester tester) {
  expect(
    find.descendant(of: _mouseRow, matching: find.byIcon(Icons.check_rounded)),
    findsNothing,
  );
  final background = tester.widget<Material>(
    find.descendant(of: _row('마우스 잠금'), matching: find.byType(Material)).first,
  );
  expect(background.color, Colors.transparent);
  final icon = tester.widget<Icon>(
    find.descendant(
      of: _mouseRow,
      matching: find.byIcon(SessionTool.gameMouse.icon),
    ),
  );
  expect(icon.color, isNot(SessionChrome.of(tester.element(_menu)).accent));
}

void main() {
  testWidgets('the real menu is the PC\'s bar as rows: 연결 품질, 화면, 설정 '
      'and 연결 끊기, without a caption over a single row', (tester) async {
    final host = await _pump(tester, _real);
    await _open(tester);
    expect(_rows(tester), [
      SessionTool.quality.label,
      SessionTool.screen.label,
      SessionTool.more.label,
      SessionTool.disconnect.label,
    ]);
    expect(_rows(tester), ['연결 품질', '화면', '설정', '연결 끊기']);
    // 화면 and 설정 are their groups' only rows and are named like them.
    expect(_captions(tester), isEmpty);
    expect(find.byType(AndroidMenuCaption), findsNothing);
    expect(find.text('화면'), findsOneWidget);
    expect(find.text('설정'), findsOneWidget);
    // The card names the desktop; the latency is the 연결 품질 row's.
    expect(
      tester.widget<Text>(find.byKey(const Key('session-menu-title'))).data,
      'VM · 스트리밍',
    );
    expect(
      tester.widget<AndroidBarMenuRow>(_row('연결 품질')).description,
      '12 ms · 좋음',
    );
    expect(
      tester.widget<AndroidBarMenuRow>(_row('화면')).description,
      '보기 · 배율 · 해상도',
    );
    expect(
      tester.widget<AndroidBarMenuRow>(_row('설정')).description,
      '화질 · 입력 · 사용안내',
    );
    final rects = [
      for (final label in ['연결 품질', '화면', '설정', '연결 끊기'])
        tester.getRect(_row(label)),
    ];
    final [quality, screen, more, disconnect] = rects;
    // The three follow each other at once: no caption and no gap between
    // them. 연결 끊기 stands alone after a hairline, in red.
    expect(screen.top, closeTo(quality.bottom, .01));
    expect(more.top, closeTo(screen.bottom, .01));
    expect(disconnect.top, greaterThan(more.bottom));
    expect(
      tester.widget<AndroidBarMenuRow>(_row('연결 끊기')).item.destructive,
      isTrue,
    );
    for (final rect in rects) {
      expect(rect.height, greaterThanOrEqualTo(48));
    }
    // Each of the three opens something: a chevron, never a switch.
    for (final key in [
      'session-quality',
      'android-session-screen',
      'android-session-more',
    ]) {
      expect(
        find.descendant(
          of: find.byKey(Key(key)),
          matching: find.byIcon(Icons.chevron_right_rounded),
        ),
        findsOneWidget,
        reason: key,
      );
    }
    expect(find.byKey(const Key('android-session-row-switch')), findsNothing);
    expect(
      find.descendant(
        of: find.byKey(const Key('android-session-screen')),
        matching: find.byIcon(SessionTool.screen.icon),
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: find.byKey(const Key('android-session-more')),
        matching: find.byIcon(SessionTool.more.icon),
      ),
      findsOneWidget,
    );
    // The former rows are inside the two panels now.
    for (final gone in ['scale', 'settings', 'game-mouse', 'help']) {
      expect(find.byKey(Key('android-session-$gone')), findsNothing);
    }
    // A row hands over to the host and leaves the card to it.
    await tester.tap(find.byKey(const Key('android-session-more')));
    await tester.pumpAndSettle();
    expect(host.used, ['more']);
    expect(_menu, findsOneWidget, reason: 'the host shows its panel there');
  });

  testWidgets('a flat list keeps the PC\'s groups: 화면 over 화면 and '
      '배율, 설정 over 화질, 마우스 잠금 and 사용안내, 연결 끊기 last', (tester) async {
    final host = await _pump(tester, _phone);
    await _open(tester);
    expect(_captions(tester), ['화면', '설정']);
    expect(_captions(tester), [SessionTool.caption(1), SessionTool.caption(3)]);
    expect(_rows(tester), [
      '연결 품질',
      '화면',
      '배율',
      '화질',
      '마우스 잠금',
      '사용안내',
      '연결 끊기',
    ]);
    // The former captions are gone; a phone shares nothing, so no 공유.
    for (final gone in ['화면·영상', '내 기기 공유', '입력·도움말', '공유', '스트리밍 설정']) {
      expect(find.text(gone), findsNothing, reason: gone);
    }
    // '설정' is the caption only: the row under it is 화질.
    expect(find.text('설정'), findsOneWidget);

    // Top to bottom: 연결 품질, then each caption over its rows.
    final order = [
      _row('연결 품질'),
      _caption('화면'),
      _row('화면'),
      _row('배율'),
      _caption('설정'),
      _row('화질'),
      _row('마우스 잠금'),
      _row('사용안내'),
      _row('연결 끊기'),
    ];
    for (final entry in order) {
      expect(entry, findsOneWidget);
    }
    final rects = [for (final entry in order) tester.getRect(entry)];
    for (var i = 1; i < rects.length; i++) {
      expect(
        rects[i].top,
        greaterThanOrEqualTo(rects[i - 1].bottom - .01),
        reason: 'entry $i is under entry ${i - 1}',
      );
    }
    final [
      quality,
      screenCaption,
      screen,
      scale,
      settingsCaption,
      settings,
      mouse,
      help,
      disconnect,
    ] = rects;
    // The first caption follows 연결 품질 without the gap between groups.
    expect(screenCaption.top, closeTo(quality.bottom, .01));
    // A caption sits right over its first row, and a group's rows follow
    // each other at once.
    expect(screen.top, closeTo(screenCaption.bottom, .01));
    expect(scale.top, closeTo(screen.bottom, .01));
    expect(settings.top, closeTo(settingsCaption.bottom, .01));
    expect(mouse.top, closeTo(settings.bottom, .01));
    expect(help.top, closeTo(mouse.bottom, .01));
    // Between the groups: the 8 dp gap, then the next caption.
    expect(settingsCaption.top - scale.bottom, closeTo(8, .01));
    // 연결 끊기 stands alone after a hairline, in red.
    expect(disconnect.top, greaterThan(help.bottom));
    expect(
      tester.widget<AndroidBarMenuRow>(_row('연결 끊기')).item.destructive,
      isTrue,
    );
    for (final rect in [
      quality,
      screen,
      scale,
      settings,
      mouse,
      help,
      disconnect,
    ]) {
      expect(rect.height, greaterThanOrEqualTo(48));
    }

    // Each row keeps its key and carries the shared name.
    for (final (id, label) in [
      ('screen', '화면'),
      ('scale', '배율'),
      ('settings', '화질'),
      ('game-mouse', '마우스 잠금'),
      ('help', '사용안내'),
    ]) {
      expect(
        find.descendant(
          of: find.byKey(Key('android-session-$id')),
          matching: find.text(label),
        ),
        findsOneWidget,
        reason: id,
      );
    }
    // 화질 (the former 설정) says what it changes.
    expect(
      find.descendant(
        of: find.byKey(const Key('android-session-settings')),
        matching: find.text('프레임·전송률'),
      ),
      findsOneWidget,
    );
    expect(find.byTooltip('화질 · 프레임·전송률'), findsOneWidget);
    // The rows that open a panel end in a chevron; 마우스 잠금 in a switch.
    for (final id in ['screen', 'scale', 'settings', 'help']) {
      final row = find.byKey(Key('android-session-$id'));
      expect(
        find.descendant(
          of: row,
          matching: find.byIcon(Icons.chevron_right_rounded),
        ),
        findsOneWidget,
        reason: id,
      );
      expect(
        find.descendant(of: row, matching: find.byType(Switch)),
        findsNothing,
        reason: id,
      );
    }
    expect(_mouseSwitch, findsOneWidget);
    expect(
      find.descendant(
        of: _mouseRow,
        matching: find.byIcon(Icons.chevron_right_rounded),
      ),
      findsNothing,
    );

    await tester.tap(find.byKey(const Key('session-disconnect')));
    await tester.pumpAndSettle();
    expect(host.disconnects, 1);
    expect(host.used, isEmpty);
    expect(_menu, findsNothing);
  });

  testWidgets('the rows follow the shared vocabulary, in whatever order the '
      'host lists them', (tester) async {
    await _pump(tester, (host) => _phone(host).reversed.toList());
    await _open(tester);
    expect(_rows(tester), [
      '연결 품질',
      '화면',
      '배율',
      '화질',
      '마우스 잠금',
      '사용안내',
      '연결 끊기',
    ]);
    expect(_captions(tester), ['화면', '설정']);
  });

  testWidgets('설정 listed before 화면 still comes after it, as on the PC\'s '
      'bar', (tester) async {
    await _pump(tester, (host) => _real(host).reversed.toList());
    await _open(tester);
    expect(_rows(tester), ['연결 품질', '화면', '설정', '연결 끊기']);
    expect(_captions(tester), isEmpty);
  });

  testWidgets('without 배율 and 마우스 잠금 (an older Host, no grant) 화면 '
      'is its group\'s only row and loses the caption; 설정 keeps its own', (
    tester,
  ) async {
    await _pump(
      tester,
      (host) => _phone(host, scale: false, mouseOffered: false),
    );
    await _open(tester);
    expect(_captions(tester), ['설정']);
    expect(_rows(tester), ['연결 품질', '화면', '화질', '사용안내', '연결 끊기']);
    expect(_mouseRow, findsNothing);
    expect(find.byKey(const Key('android-session-row-switch')), findsNothing);
    final quality = tester.getRect(_row('연결 품질'));
    final screen = tester.getRect(_row('화면'));
    final caption = tester.getRect(_caption('설정'));
    final settings = tester.getRect(_row('화질'));
    expect(screen.top, closeTo(quality.bottom, .01));
    expect(caption.top - screen.bottom, closeTo(8, .01));
    expect(settings.top, closeTo(caption.bottom, .01));
  });

  testWidgets('a panel row hands over to the host and leaves the card to '
      'it', (tester) async {
    final host = await _pump(tester, _phone);
    await _open(tester);
    await tester.tap(find.byKey(const Key('android-session-settings')));
    await tester.pumpAndSettle();
    expect(host.used, ['settings']);
    expect(_menu, findsOneWidget, reason: 'the host shows its panel there');
  });

  group('마우스 잠금 is a switch row', () {
    testWidgets('off: the switch is off and a tap anywhere on the row runs '
        'the command once and closes the card', (tester) async {
      final host = await _pump(tester, _phone);
      await _open(tester);
      expect(_mouseWidget(tester).toggle, isTrue);
      expect(_mouseWidget(tester).enabled, isTrue);
      expect(tester.widget<Switch>(_mouseSwitch).value, isFalse);
      expect(tester.widget<Switch>(_mouseSwitch).onChanged, isNotNull);
      _expectNotChosen(tester);
      expect(
        find.descendant(of: _mouseRow, matching: find.text('외부 마우스 사용')),
        findsOneWidget,
      );
      await tester.tap(_mouseRow);
      await tester.pumpAndSettle();
      expect(host.used, ['game-mouse']);
      expect(_menu, findsNothing);

      // The switch is part of the row's one target.
      await _open(tester);
      await tester.tapAt(tester.getCenter(_mouseSwitch));
      await tester.pumpAndSettle();
      expect(host.used, ['game-mouse', 'game-mouse']);
      expect(_menu, findsNothing);
    });

    testWidgets('on: the switch is on, without the chosen row\'s amber check '
        'or tint; a tap runs the command', (tester) async {
      final host = await _pump(tester, (host) => _phone(host, locked: true));
      await _open(tester);
      expect(_mouseWidget(tester).toggle, isTrue);
      expect(_mouseWidget(tester).item.checked, isTrue);
      expect(tester.widget<Switch>(_mouseSwitch).value, isTrue);
      _expectNotChosen(tester);
      expect(find.byTooltip('마우스 잠금 끄기 · 뒤로 가기로 풀기'), findsOneWidget);
      await tester.tap(_mouseRow);
      await tester.pumpAndSettle();
      expect(host.used, ['game-mouse']);
      expect(_menu, findsNothing);
    });

    testWidgets('without a mouse (no onPressed) it is greyed with its reason '
        'and a tap runs nothing', (tester) async {
      final host = await _pump(tester, (host) => _phone(host, mouse: false));
      await _open(tester);
      expect(_mouseWidget(tester).toggle, isTrue);
      expect(_mouseWidget(tester).enabled, isFalse);
      expect(tester.widget<Switch>(_mouseSwitch).value, isFalse);
      expect(tester.widget<Switch>(_mouseSwitch).onChanged, isNull);
      expect(tester.widget<InkWell>(_mouseRow).onTap, isNull);
      // The reason is under the name and in the row's tooltip.
      expect(
        find.descendant(
          of: _mouseRow,
          matching: find.text('마우스를 연결하면 쓸 수 있어요'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(of: _mouseRow, matching: find.text('외부 마우스 사용')),
        findsNothing,
      );
      expect(
        find.byTooltip('마우스 잠금 · 마우스를 데스크톱 안에 잠가요 · 마우스를 연결하면 쓸 수 있어요'),
        findsOneWidget,
      );
      await tester.tap(_mouseRow, warnIfMissed: false);
      await tester.pumpAndSettle();
      expect(host.used, isEmpty);
      expect(_menu, findsOneWidget, reason: 'nothing ran, the card stays');
    });
  });

  testWidgets('the console\'s card has no caption: 화면 is its group\'s '
      'only row, then 키보드 and 키 조합 of its own follow', (tester) async {
    final host = await _pump(tester, _console);
    await _open(tester);
    // 화면 is in the vocabulary; 키보드 and 키 조합 keep their own order
    // after it.
    expect(_rows(tester), ['연결 품질', '화면', '키보드', '키 조합', '연결 끊기']);
    expect(_captions(tester), isEmpty);
    expect(find.text('화면'), findsOneWidget);
    for (final gone in ['설정', '공유', '화면·영상', '내 기기 공유', '입력·도움말']) {
      expect(find.text(gone), findsNothing, reason: gone);
    }
    final quality = tester.getRect(_row('연결 품질'));
    final screen = tester.getRect(_row('화면'));
    final keyboard = tester.getRect(_row('키보드'));
    final keys = tester.getRect(_row('키 조합'));
    final disconnect = tester.getRect(_row('연결 끊기'));
    expect(screen.top, closeTo(quality.bottom, .01));
    // A gap comes only with a caption: the rows follow each other at once.
    expect(keyboard.top, closeTo(screen.bottom, .01));
    expect(keys.top, closeTo(keyboard.bottom, .01));
    expect(disconnect.top, greaterThan(keys.bottom));
    // None of them is a switch.
    expect(find.byKey(const Key('android-session-row-switch')), findsNothing);
    await tester.tap(find.byKey(const Key('console-keyboard-menu')));
    await tester.pumpAndSettle();
    expect(host.used, ['keyboard']);
    expect(_menu, findsNothing);
    // A menu of rows still opens its page.
    await _open(tester);
    await tester.tap(_row('키 조합'));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-session-menu-card')), findsOneWidget);
    await tester.tap(find.byKey(const Key('console-secure-attention')));
    await tester.pumpAndSettle();
    expect(host.used, ['keyboard', 'secure']);
  });
}

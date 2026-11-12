// The Android session menu as the PC's session bar turned on its side:
// the card's rows are 연결 품질, 화면, 설정 and
// 연결 끊기, and each opens the PC's own panel in the card's place: the 연결
// panel (SessionConnectionPanel), 화면 with 배율 inside, and 설정
// (SessionSettingsPanel) leading to 화질, 입력 방식 and 사용안내.
//
// The real AndroidRemoteView with the shared RemoteSession over a fake relay
// socket, the Android native media contract fake and a recorded pointer
// capture; the round trip, a resize error, the print channel and the
// clipboard channel are set on that session by the test. No phone, mouse,
// Host, VM or printer: nothing here says a device shows it this way. The TV
// (its own local panels) is not covered here.
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_toolbar.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/print_session.dart';
import 'package:multitap_client/data/relative_mouse.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_guide.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_connection_panel.dart';
import 'package:multitap_client/ui/session_menu.dart' show SessionChrome;
import 'package:multitap_client/ui/session_panels.dart';
import 'package:multitap_client/ui/session_stats_dialog.dart';
import 'package:multitap_client/ui/session_tools.dart';

import '../remote_session_test.dart' show SessionMedia, SessionSocket;
import 'android_session_contract_test.dart' show AndroidNativeFake;

const _sessionUrl = 'wss://fixture.invalid/api/v1/sessions/android-session';
const _pen = MethodChannel('multitap/pen-input');

/// MainActivity's pointer capture (AndroidGameMouse.kt), recorded, as in
/// android_game_mouse_test.dart.
class _Capture implements MousePresenceNative {
  final calls = <String>[];
  bool mouse = true;
  void Function(bool)? _onMouse;

  int get engages => calls.where((call) => call == 'engage').length;
  int get releases => calls.where((call) => call == 'release').length;

  @override
  Future<RelativeMousePath?> engage(
    Rect? picture, {
    bool pin = false,
    bool conceal = false,
  }) async {
    calls.add('engage');
    return RelativeMousePath.injected;
  }

  @override
  Future<void> release() async => calls.add('release');
  @override
  Future<void> armKey(bool armed) async => calls.add('key:$armed');
  @override
  void listen(void Function(String event) onEvent) {}
  @override
  void dispose() {}
  @override
  Future<bool> mousePresent() async => mouse;
  @override
  set onMouse(void Function(bool present)? callback) => _onMouse = callback;

  /// A mouse came, or the last one left.
  void plug(bool present) {
    mouse = present;
    _onMouse?.call(present);
  }
}

/// A clipboard channel as the guide reads it: what the pair allows now
/// ([allowed]), paired ([ready]) or ended ([ended]). Nothing is dialled or
/// sent.
class _Clipboard extends ClipboardSession {
  _Clipboard(this.allowed, {bool ready = true, this.ended = false})
    : super(
        url: Uri.parse('$_sessionUrl/clipboard'),
        token: 'not-persisted',
        sessionId: 'android-session',
      ) {
    this.ready = ready;
  }
  final ClipboardPolicy allowed;
  final bool ended;
  @override
  ClipboardPolicy get effectivePolicy => allowed;
  @override
  bool get canSend => allowed.toRemote;
  @override
  bool get canReceive => allowed.toLocal;
  @override
  bool get closed => ended;
  @override
  Future<void> close({String reason = 'clipboard_transport_closed'}) async {}
}

/// A print channel that was never dialled: only what the guide reads.
PrintSession _printer({bool queueReady = true}) => PrintSession(
  url: Uri.parse('$_sessionUrl/print'),
  token: 'not-persisted',
  sessionId: 'android-session',
)..queueReady = queueReady;

class _Harness {
  final capture = _Capture();
  final sockets = <SessionSocket>[];

  /// What each connection asked the broker for, and its Windows scale.
  final requested = <StreamOptions>[];
  final scales = <int?>[];

  /// What the view handed to `onOptions` (this device remembers it).
  final remembered = <StreamOptions>[];
  final saved = <AndroidSessionSettings>[];
  late RemoteSession session;
  SessionSocket get socket => sockets.last;

  /// Every message of [type] sent to the relay, in order.
  List<Map<String, dynamic>> sent(String type) => [
    for (final socket in sockets)
      for (final message in socket.sent)
        if (message['type'] == type) message,
  ];

  /// The session measured a round trip of [ms] (null: none yet).
  Future<void> latency(WidgetTester tester, double? ms) async {
    session.rttMs = ms;
    session.notifyListeners();
    await tester.pump();
  }
}

Uint8List _video(int pts, {bool key = false}) {
  final bytes = Uint8List(17);
  bytes[0] = 1;
  bytes[1] = key ? 1 : 0;
  for (var i = 0; i < 8; i++) {
    bytes[4 + i] = (pts >> (8 * i)) & 255;
  }
  bytes.setAll(12, [0, 0, 0, 1, 0x65]);
  return bytes;
}

/// A connected remote view on a phone held upright (412 x 915 dp).
/// [screen]: a Host that resizes the stream and sets Windows' scale (화면
/// then has 배율 and 해상도). [relative]: relative-mouse-v1 was granted, with
/// [mouse] connected or not. [printer]: print-v1 granted and the VM's printer
/// ready. [clipboard]: clipboard sharing granted with this policy, its
/// channel ready.
Future<_Harness> _open(
  WidgetTester tester, {
  StreamOptions options = const StreamOptions(),
  AndroidTouchMode touch = AndroidTouchMode.trackpad,
  bool screen = false,
  bool relative = false,
  bool mouse = true,
  bool printer = false,
  ClipboardPolicy? clipboard,
}) async {
  tester.view.physicalSize = const Size(412, 915);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  AndroidNativeFake().install();
  addTearDown(AndroidNativeFake.uninstall);
  final h = _Harness()..capture.mouse = mouse;
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: AndroidRemoteView(
        name: 'VM173',
        token: 'not-persisted',
        options: options,
        onOptions: h.remembered.add,
        settings: AndroidSessionSettings(helpSeen: true, touch: touch),
        onSettings: h.saved.add,
        displayScale: screen,
        decoderLimits: () async => null,
        density: () async => null,
        load: (options, _, scale) async {
          h.requested.add(options);
          h.scales.add(scale);
          return BrokerConnection(
            sessionId: 'android-session',
            webSocketUrl: Uri.parse(_sessionUrl),
            options: options,
            epochMedia: screen,
            viewportProvisioning: screen,
            displayScale: screen ? scale : null,
            cursor: relative,
            relativeMouse: relative,
            printWebSocketUrl: printer ? Uri.parse('$_sessionUrl/print') : null,
            clipboardWebSocketUrl: clipboard == null
                ? null
                : Uri.parse('$_sessionUrl/clipboard'),
            clipboardSync: clipboard != null,
            clipboardPolicy: clipboard ?? const ClipboardPolicy(),
          );
        },
        sessionFactory: (connection, media, statsSink) {
          final session = RemoteSession(
            connection: connection,
            token: 'not-persisted',
            media: media,
            statsSink: statsSink,
            dialer: (_, _) async {
              final socket = SessionSocket();
              h.sockets.add(socket);
              return socket;
            },
          );
          // The channels are in place before the first picture, so the
          // session dials none of its own.
          if (printer) session.printer = _printer();
          if (clipboard != null) session.clipboard = _Clipboard(clipboard);
          return h.session = session;
        },
        pictureBuilder: (_) => const ColoredBox(color: Color(0xFF2B4C6F)),
        relativeMouseNative: relative ? h.capture : null,
      ),
    ),
  );
  for (var i = 0; i < 5; i++) {
    await tester.pump(const Duration(milliseconds: 20));
  }
  // A resizable stream arrives at the size the view asked for (nothing to
  // resize); any other at a fixed 1280 x 720.
  final asked = h.requested.last;
  final width = screen ? asked.maxWidth : 1280;
  final height = screen ? asked.maxHeight : 720;
  h.socket.incoming.add(
    jsonEncode({
      'type': 'stream.info',
      'codec': 'h264',
      'width': width,
      'height': height,
      'sourceWidth': width,
      'sourceHeight': height,
    }),
  );
  await tester.pump(const Duration(milliseconds: 20));
  h.socket.incoming.add(_video(16667, key: true));
  for (var i = 0; i < 6; i++) {
    await tester.pump(const Duration(milliseconds: 40));
  }
  expect(find.byKey(const Key('android-remote-surface')), findsOneWidget);
  return h;
}

Future<void> _close(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  // RemoteSession closes its fake wires within a bounded wait.
  await tester.pump(const Duration(seconds: 3));
}

/// Fixed frames where a dialog or a held mouse may keep something moving.
Future<void> _frames(WidgetTester tester, [int count = 12]) async {
  for (var i = 0; i < count; i++) {
    await tester.pump(const Duration(milliseconds: 40));
  }
}

final _card = find.byKey(const Key('session-command-bar'));

/// ⋯ opens the menu card.
Future<void> _openMenu(WidgetTester tester) async {
  if (_card.evaluate().isNotEmpty) return;
  await tester.tap(find.byKey(const Key('expand-session-toolbar')));
  await tester.pumpAndSettle();
  expect(_card, findsOneWidget);
}

/// A row of the menu card ([row]) opens its [panel] in the card's place.
Future<void> _openRow(WidgetTester tester, String row, String panel) async {
  await _openMenu(tester);
  await tester.tap(find.byKey(Key(row)));
  await tester.pumpAndSettle();
  expect(find.byKey(Key(panel)), findsOneWidget);
  expect(_card, findsNothing);
}

Future<void> _openConnection(WidgetTester tester) =>
    _openRow(tester, 'session-quality', 'android-connection-panel');
Future<void> _openScreen(WidgetTester tester) =>
    _openRow(tester, 'android-session-screen', 'android-screen-panel');
Future<void> _openMore(WidgetTester tester) =>
    _openRow(tester, 'android-session-more', 'android-more-panel');

/// A row of a shared panel (the PC's SessionPanelRow), by its key. The row
/// widget is matched, as in relative_mouse_test.dart.
Finder _panelRow(String key) => find.byWidgetPredicate(
  (widget) => widget is SessionPanelRow && widget.key == Key(key),
);

/// Brings [target] into its card's view, then taps it.
Future<void> _tap(WidgetTester tester, Finder target) async {
  await tester.ensureVisible(target);
  await tester.pumpAndSettle();
  await tester.tap(target);
  await tester.pumpAndSettle();
}

/// The Android Back key.
Future<void> _back(WidgetTester tester) async {
  await tester.binding.handlePopRoute();
  await tester.pumpAndSettle();
}

/// The Semantics a switch row is described by (null: it is no switch).
Semantics? _switchSemantics(WidgetTester tester, String key) {
  for (final semantics in tester.widgetList<Semantics>(
    find.descendant(of: _panelRow(key), matching: find.byType(Semantics)),
  )) {
    if (semantics.properties.toggled != null) return semantics;
  }
  return null;
}

bool? _toggled(WidgetTester tester, String key) =>
    _switchSemantics(tester, key)?.properties.toggled;

/// The rows of the 설정 panel, top to bottom.
List<SessionPanelRow> _moreRows(WidgetTester tester) => tester
    .widgetList<SessionPanelRow>(
      find.descendant(
        of: find.byKey(const Key('session-settings-panel')),
        matching: find.byType(SessionPanelRow),
      ),
    )
    .toList();

double _top(WidgetTester tester, Finder finder) => tester.getTopLeft(finder).dy;

Finder _caption(String label) => find.byWidgetPredicate(
  (widget) => widget is AndroidMenuCaption && widget.label == label,
);

/// A session that was never started, as SessionGuideContent reads it.
RemoteSession _guideSession({
  bool printing = false,
  bool channel = true,
  bool sync = true,
  ClipboardPolicy policy = const ClipboardPolicy(),
}) => RemoteSession(
  connection: BrokerConnection(
    sessionId: 'android-session',
    webSocketUrl: Uri.parse(_sessionUrl),
    printWebSocketUrl: printing ? Uri.parse('$_sessionUrl/print') : null,
    clipboardWebSocketUrl: channel ? Uri.parse('$_sessionUrl/clipboard') : null,
    clipboardSync: sync,
    clipboardPolicy: policy,
  ),
  token: 'not-persisted',
  media: SessionMedia(),
  dialer: (_, _) async => SessionSocket(),
);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  // The shipped font, as in android_screen_panel_landscape_test.dart: the
  // test font draws every glyph a full em wide, which no device does.
  setUpAll(() async {
    final font = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      font.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await font.load();
    await (FontLoader(
      'MaterialIcons',
    )..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'))).load();
  });
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(_pen, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? 'started' : true,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(_pen, null);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });

  // ------------------------------------------------------------ menu card

  testWidgets('The menu card is the PC bar as rows: 연결 품질, 화면, 설정, then '
      '연결 끊기, without captions or the old flat rows', (tester) async {
    // 마우스 잠금 is offered here too: it is a row of 설정, not of the card.
    final h = await _open(tester, screen: true, relative: true);
    try {
      await _openMenu(tester);
      final title = find.byKey(const Key('session-menu-title'));
      expect(find.descendant(of: _card, matching: title), findsOneWidget);
      expect(tester.widget<Text>(title).data, 'VM173 · 스트리밍');

      final rows = tester
          .widgetList<AndroidBarMenuRow>(
            find.descendant(
              of: _card,
              matching: find.byType(AndroidBarMenuRow),
            ),
          )
          .toList();
      const keys = [
        'session-quality',
        'android-session-screen',
        'android-session-more',
        'session-disconnect',
      ];
      expect(rows.map((row) => row.item.key).toList(), [
        for (final key in keys) Key(key),
      ]);
      expect(rows.map((row) => row.item.label).toList(), [
        '연결 품질',
        '화면',
        '설정',
        '연결 끊기',
      ]);
      expect(rows.map((row) => row.item.label).toList(), [
        SessionTool.quality.label,
        SessionTool.screen.label,
        SessionTool.more.label,
        SessionTool.disconnect.label,
      ]);
      // Before a measurement 연결 품질 says only that it is connected.
      expect(rows.map((row) => row.description).toList(), [
        '연결됨',
        '보기 · 배율 · 해상도',
        '화질 · 입력 · 사용안내',
        null,
      ]);
      // The first three open a panel (a chevron); 연결 끊기 acts.
      expect(rows.map((row) => row.opens).toList(), [true, true, true, false]);
      expect(rows.where((row) => row.toggle), isEmpty);
      expect(rows.last.item.destructive, isTrue);

      // Top to bottom as listed, each a 48 dp target with its words.
      var above = _top(tester, title);
      for (final (index, key) in keys.indexed) {
        final row = find.byKey(Key(key));
        expect(row, findsOneWidget, reason: key);
        expect(
          tester.getSize(row).height,
          greaterThanOrEqualTo(48),
          reason: key,
        );
        expect(_top(tester, row), greaterThan(above), reason: key);
        above = _top(tester, row);
        expect(
          find.descendant(of: row, matching: find.text(rows[index].item.label)),
          findsOneWidget,
          reason: key,
        );
        final description = rows[index].description;
        if (description != null) {
          expect(
            find.descendant(of: row, matching: find.text(description)),
            findsOneWidget,
            reason: key,
          );
        }
      }

      // No group captions: each group is one row named like its caption.
      expect(
        find.descendant(of: _card, matching: find.byType(AndroidMenuCaption)),
        findsNothing,
      );
      // What the card listed flat before now lives in 화면 and 설정.
      for (final gone in const [
        'android-session-scale',
        'android-session-settings',
        'android-session-game-mouse',
        'android-session-help',
        'android-session-input',
        'android-session-row-switch',
      ]) {
        expect(find.byKey(Key(gone)), findsNothing, reason: gone);
      }
      // The cluster keeps its three buttons.
      for (final button in const [
        'expand-session-toolbar',
        'android-remote-touch-mode-button',
        'android-remote-keyboard-button',
      ]) {
        expect(find.byKey(Key(button)), findsOneWidget, reason: button);
      }
      expect(find.byKey(const Key('session-toolbar-fold')), findsOneWidget);
      expect(h.requested, hasLength(1));
      expect(tester.takeException(), isNull);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('연결 품질 says the round trip and its word on the card and in '
      'its panel: 12 좋음, 58 보통, 142 느림', (tester) async {
    final h = await _open(tester);
    try {
      const chrome = SessionChrome.forDark(true);
      for (final (ms, word) in const [
        (12.0, '좋음'),
        (58.0, '보통'),
        (142.0, '느림'),
      ]) {
        final number = ms.toStringAsFixed(0);
        await h.latency(tester, ms);
        await _openMenu(tester);
        final row = find.byKey(const Key('session-quality'));
        expect(
          find.descendant(of: row, matching: find.text('$number ms · $word')),
          findsOneWidget,
          reason: number,
        );
        expect(
          find.descendant(of: row, matching: find.text('연결됨')),
          findsNothing,
        );
        // Its icon takes the latency's colour.
        expect(
          tester
              .widget<Icon>(
                find.descendant(
                  of: row,
                  matching: find.byIcon(SessionTool.quality.icon),
                ),
              )
              .color,
          chrome.latencyColor(ms),
          reason: number,
        );

        await _openConnection(tester);
        final panel = find.byKey(const Key('android-connection-panel'));
        final latency = find.descendant(
          of: panel,
          matching: find.byKey(const Key('quality-latency')),
        );
        expect(latency, findsOneWidget);
        expect(
          tester.widget<Text>(latency).textSpan!.toPlainText(),
          '$number ms',
        );
        expect(
          find.descendant(
            of: find.byKey(const Key('quality-word')),
            matching: find.text(word),
          ),
          findsOneWidget,
          reason: number,
        );
        // Back to the card for the next band.
        await tester.tap(find.byKey(const Key('session-menu-back')));
        await tester.pumpAndSettle();
        expect(_card, findsOneWidget);
        expect(panel, findsNothing);
      }
      expect(tester.takeException(), isNull);
    } finally {
      await _close(tester);
    }
  });

  // -------------------------------------------------------- 연결 품질 panel

  testWidgets('연결 품질 opens the PC\'s 연결 panel in the card: the number, three '
      'tiles, three lines and 연결 통계 보기', (tester) async {
    final h = await _open(tester);
    try {
      await h.latency(tester, 12);
      await _openConnection(tester);
      final panel = find.byKey(const Key('android-connection-panel'));
      expect(tester.widget<AndroidToolbarPanel>(panel).title, '연결 품질');
      final shared = find.descendant(
        of: panel,
        matching: find.byType(SessionConnectionPanel),
      );
      expect(shared, findsOneWidget);
      // The card has the title: the shared panel brings no header of its own.
      expect(tester.widget<SessionConnectionPanel>(shared).embedded, isTrue);
      expect(
        find.descendant(of: panel, matching: find.byType(SessionPanelHeader)),
        findsNothing,
      );
      expect(
        find.descendant(
          of: panel,
          matching: find.byKey(const Key('session-connection-panel')),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(of: panel, matching: find.text('왕복 지연 · Host 응답')),
        findsOneWidget,
      );
      for (final (id, label) in const [
        ('fps', '화면'),
        ('rate', '수신'),
        ('resolution', '해상도'),
        ('path', '영상 경로'),
        ('encode', '인코딩 · Host 처리'),
        ('decode', '디코딩 · 이 기기 처리'),
      ]) {
        final value = find.descendant(
          of: panel,
          matching: find.byKey(Key('quality-value-$id')),
        );
        expect(value, findsOneWidget, reason: id);
        expect(
          find.descendant(of: value, matching: find.text(label)),
          findsOneWidget,
          reason: id,
        );
      }
      // The three tiles in one row, the three lines under them.
      final tiles = [
        for (final id in const ['fps', 'rate', 'resolution'])
          tester.getRect(find.byKey(Key('quality-value-$id'))),
      ];
      expect(tiles[1].top, closeTo(tiles[0].top, .01));
      expect(tiles[2].top, closeTo(tiles[0].top, .01));
      expect(tiles[0].right, lessThanOrEqualTo(tiles[1].left));
      expect(tiles[1].right, lessThanOrEqualTo(tiles[2].left));
      var above = tiles[0].bottom;
      for (final id in const ['path', 'encode', 'decode']) {
        final line = tester.getRect(find.byKey(Key('quality-value-$id')));
        expect(line.top, greaterThanOrEqualTo(above), reason: id);
        above = line.bottom;
      }
      // Nothing is said about a slow connection while it is not slow, and
      // 화면 크기 다시 적용 is not offered without a failed resize.
      expect(find.byKey(const Key('quality-advice')), findsNothing);
      expect(find.byKey(const Key('quality-saver')), findsNothing);
      expect(find.byKey(const Key('android-session-resize')), findsNothing);

      final stats = _panelRow('quality-stats');
      expect(stats, findsOneWidget);
      expect(tester.widget<SessionPanelRow>(stats).title, '연결 통계 보기');
      expect(tester.getSize(stats).height, greaterThanOrEqualTo(48));
      expect(tester.getRect(stats).top, greaterThanOrEqualTo(above));
      expect(find.byType(SessionStatsDialog), findsNothing);
      await _tap(tester, stats);
      expect(find.byType(SessionStatsDialog), findsOneWidget);
      expect(tester.takeException(), isNull);
    } finally {
      await _close(tester);
    }
  });

  for (final audio in [true, false]) {
    testWidgets('A round trip of 80 ms or more for 5 s brings the advice; '
        '절약으로 다시 연결 reconnects with 30 fps and a fixed 8 Mbps, sound '
        '${audio ? 'on' : 'off'} as it was', (tester) async {
      final options = StreamOptions(audio: audio);
      final h = await _open(tester, options: options);
      try {
        final advice = find.byKey(const Key('quality-advice'));
        final saver = find.byKey(const Key('quality-saver'));
        await h.latency(tester, 142);
        await _openConnection(tester);
        expect(
          find.descendant(
            of: find.byKey(const Key('quality-word')),
            matching: find.text('느림'),
          ),
          findsOneWidget,
        );
        expect(advice, findsNothing, reason: 'slow only just now');
        expect(saver, findsNothing);
        await tester.pump(const Duration(seconds: 2));
        expect(advice, findsNothing, reason: 'not yet 5 s');
        // It stays slow past SessionPoorLatency.hold.
        await tester.pump(SessionPoorLatency.hold);
        await tester.pump();
        expect(advice, findsOneWidget);
        expect(
          find.descendant(of: advice, matching: find.textContaining('연결이 느려요')),
          findsOneWidget,
        );
        expect(find.descendant(of: advice, matching: saver), findsOneWidget);
        expect(
          find.descendant(of: saver, matching: find.text('절약으로 다시 연결')),
          findsOneWidget,
        );
        expect(h.requested, hasLength(1));
        expect(h.remembered, isEmpty);

        await tester.ensureVisible(saver);
        await tester.pumpAndSettle();
        await tester.tap(saver);
        // Fixed frames: the new connection is being made.
        for (var i = 0; i < 10; i++) {
          await tester.pump(const Duration(milliseconds: 50));
        }
        // The view's onOptions hears 절약's frame rate and bitrate with
        // everything else as it was.
        expect(h.remembered, hasLength(1));
        final kept = h.remembered.single;
        expect(
          (kept.fps, kept.bitrate, kept.bitrateMode),
          (30, 8000000, StreamBitrateMode.fixed),
        );
        expect((kept.audio, kept.monitor), (audio, options.monitor));
        expect(
          (kept.maxWidth, kept.maxHeight),
          (options.maxWidth, options.maxHeight),
        );
        expect(StreamPreset.of(kept, null), StreamPreset.saver);
        // One new connection asks the broker for the same.
        expect(h.requested, hasLength(2), reason: 'one reconnect');
        final asked = h.requested.last;
        expect(
          (asked.fps, asked.bitrate, asked.automaticBitrate),
          (30, 8000000, false),
        );
        expect((asked.audio, asked.monitor), (audio, options.monitor));
        // The menu went with the old connection.
        expect(_card, findsNothing);
        expect(find.byKey(const Key('android-connection-panel')), findsNothing);
      } finally {
        await _close(tester);
      }
    });
  }

  testWidgets('Already in 절약: the advice shows without its button', (
    tester,
  ) async {
    final options = StreamPreset.saver.options;
    expect(StreamPreset.of(options, null), StreamPreset.saver);
    final h = await _open(tester, options: options);
    try {
      await h.latency(tester, 142);
      await _openConnection(tester);
      expect(find.byKey(const Key('quality-advice')), findsNothing);
      await tester.pump(SessionPoorLatency.hold);
      await tester.pump();
      final advice = find.byKey(const Key('quality-advice'));
      expect(advice, findsOneWidget);
      expect(
        find.descendant(of: advice, matching: find.textContaining('연결이 느려요')),
        findsOneWidget,
      );
      expect(find.byKey(const Key('quality-saver')), findsNothing);
      expect(find.text('절약으로 다시 연결'), findsNothing);
      expect(h.requested, hasLength(1));
      expect(h.remembered, isEmpty);
      expect(tester.takeException(), isNull);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('A round trip that gets better again within 5 s brings no '
      'advice', (tester) async {
    final h = await _open(tester);
    try {
      await h.latency(tester, 142);
      await _openConnection(tester);
      await tester.pump(const Duration(seconds: 2));
      await h.latency(tester, 12);
      await tester.pump(SessionPoorLatency.hold);
      await tester.pump();
      expect(find.byKey(const Key('quality-advice')), findsNothing);
      expect(find.byKey(const Key('quality-saver')), findsNothing);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('화면 크기 다시 적용 is a row of 연결 품질 only after a failed resize on '
      'a session that can resize', (tester) async {
    for (final resizable in [true, false]) {
      final h = await _open(tester, screen: resizable);
      try {
        expect(h.session.canResizeViewport, resizable);
        await _openConnection(tester);
        final resize = _panelRow('android-session-resize');
        expect(resize, findsNothing, reason: 'no failed resize');
        h.session.resizeError = RemoteSession.keptSizeMessage;
        h.session.notifyListeners();
        await tester.pump();
        expect(h.session.canResizeViewport, resizable);
        expect(
          resize,
          resizable ? findsOneWidget : findsNothing,
          reason: 'resizable: $resizable',
        );
        if (resizable) {
          expect(tester.widget<SessionPanelRow>(resize).title, '화면 크기 다시 적용');
          expect(tester.getSize(resize).height, greaterThanOrEqualTo(48));
          // Before 연결 통계 보기, in the same panel.
          expect(
            find.descendant(
              of: find.byKey(const Key('android-connection-panel')),
              matching: resize,
            ),
            findsOneWidget,
          );
          expect(
            _top(tester, resize),
            lessThan(_top(tester, _panelRow('quality-stats'))),
          );
        }
      } finally {
        await _close(tester);
      }
    }
  });

  // ---------------------------------------------------------------- 화면

  testWidgets('화면 holds, top to bottom: the observed state, the view rows, '
      '배율 with its five steps, then 해상도; there is no 배율 panel', (tester) async {
    final h = await _open(tester, screen: true);
    try {
      expect(h.session.displayScaleSupported, isTrue);
      expect(h.session.canResizeViewport, isTrue);
      await _openScreen(tester);
      final panel = find.byKey(const Key('android-screen-panel'));
      expect(tester.widget<AndroidToolbarPanel>(panel).title, '화면');
      Finder part(String key) =>
          find.descendant(of: panel, matching: find.byKey(Key(key)));
      const parts = [
        'android-screen-observed-state',
        'android-screen-fit',
        'android-session-move',
        'android-screen-scale',
        'android-screen-resolution',
      ];
      double? above;
      for (final key in parts) {
        expect(part(key), findsOneWidget, reason: key);
        final top = _top(tester, part(key));
        if (above != null) expect(top, greaterThan(above), reason: key);
        above = top;
      }
      final observed = tester
          .widget<Text>(part('android-screen-observed-state'))
          .data!;
      expect(observed, startsWith('원격 해상도 '));
      expect(observed, contains('\nWindows 배율 · '));

      // Exactly the five steps of the PC's 배율, no 자동 chip.
      final scale = tester.widget<AndroidPanelChoices>(
        part('android-screen-scale'),
      );
      expect(scale.label, 'Windows 배율 · 휴대폰');
      expect(scale.items.map((item) => item.key).toList(), [
        for (final percent in const [100, 125, 150, 175, 200])
          Key('android-screen-scale-$percent'),
      ]);
      expect(scale.items.map((item) => item.label).toList(), [
        '100',
        '125',
        '150',
        '175',
        '200',
      ]);
      expect(scale.items.where((item) => item.selected), hasLength(1));
      expect(find.byKey(const Key('android-screen-scale-0')), findsNothing);
      for (final percent in const [100, 125, 150, 175, 200]) {
        final step = find.byKey(Key('android-screen-scale-$percent'));
        await tester.ensureVisible(step);
        await tester.pumpAndSettle();
        expect(step.hitTestable(), findsOneWidget, reason: '$percent');
        expect(
          tester.getSize(step).height,
          greaterThanOrEqualTo(48),
          reason: '$percent',
        );
      }
      // 배율 has no panel or menu row of its own any more.
      expect(find.byKey(const Key('android-scale-panel')), findsNothing);
      expect(find.byKey(const Key('android-session-scale')), findsNothing);
      // 화면 방향 and 포인터 속도 stay out of the phone's 화면.
      expect(find.byKey(const Key('android-screen-orientation')), findsNothing);
      expect(find.byKey(const Key('android-speed-fast')), findsNothing);
      expect(tester.takeException(), isNull);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('화면 has no 배율 where the session cannot set Windows\' scale', (
    tester,
  ) async {
    final h = await _open(tester);
    try {
      expect(h.session.displayScaleSupported, isFalse);
      await _openScreen(tester);
      for (final key in const [
        'android-screen-observed-state',
        'android-screen-fit',
        'android-session-move',
      ]) {
        expect(find.byKey(Key(key)), findsOneWidget, reason: key);
      }
      expect(find.byKey(const Key('android-screen-scale')), findsNothing);
      expect(find.byKey(const Key('android-screen-scale-100')), findsNothing);
      // Nor 해상도 without a stream that can resize.
      expect(find.byKey(const Key('android-screen-resolution')), findsNothing);
      expect(find.byKey(const Key('android-scale-panel')), findsNothing);
      expect(tester.takeException(), isNull);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('A 배율 step in 화면 sets Windows\' scale live and is remembered, '
      'without a resize or a reconnect', (tester) async {
    final h = await _open(tester, screen: true);
    try {
      await _openScreen(tester);
      // A step other than the one in force, so it is a change.
      final percent = h.session.displayScaleWanted == 175 ? 150 : 175;
      final step = find.byKey(Key('android-screen-scale-$percent'));
      await tester.ensureVisible(step);
      await tester.pumpAndSettle();
      await tester.tap(step);
      await tester.pumpAndSettle();
      await tester.pump(const Duration(seconds: 1));
      expect(h.saved, isNotEmpty);
      expect(h.saved.last.screen(AndroidDeviceClass.phone).scale, percent);
      expect(
        h.sent('display.scale').map((message) => message['scale']).last,
        percent,
      );
      expect(h.sent('stream.resize'), isEmpty);
      expect(h.requested, hasLength(1), reason: 'no reconnect');
      expect(h.remembered, isEmpty);
    } finally {
      await _close(tester);
    }
  });

  // ---------------------------------------------------------------- 설정

  testWidgets('설정 opens the PC\'s 설정 panel in the card: 화질, 마우스 잠금 '
      'where offered, 입력 방식, 사용안내, and no pin row', (tester) async {
    for (final relative in [false, true]) {
      await _open(tester, relative: relative);
      try {
        await _openMore(tester);
        final panel = find.byKey(const Key('android-more-panel'));
        expect(tester.widget<AndroidToolbarPanel>(panel).title, '설정');
        final shared = find.descendant(
          of: panel,
          matching: find.byType(SessionSettingsPanel),
        );
        expect(shared, findsOneWidget);
        final settings = tester.widget<SessionSettingsPanel>(shared);
        expect(settings.embedded, isTrue);
        expect(settings.showPin, isFalse);
        expect(
          find.descendant(of: panel, matching: find.byType(SessionPanelHeader)),
          findsNothing,
        );
        expect(find.byKey(const Key('pin-session-toolbar')), findsNothing);
        expect(find.text('툴바 열어 두기'), findsNothing);

        final keys = [
          'android-session-settings',
          if (relative) 'android-session-game-mouse',
          'android-session-input',
          'android-session-help',
        ];
        final rows = _moreRows(tester);
        expect(rows.map((row) => row.key).toList(), [
          for (final key in keys) Key(key),
        ], reason: 'relative: $relative');
        expect(rows.map((row) => row.title).toList(), [
          '화질',
          if (relative) '마우스 잠금',
          '입력 방식',
          '사용안내',
        ]);
        expect(rows.map((row) => row.subtitle).toList(), [
          '작업 · 60 fps',
          if (relative) '마우스를 데스크톱 안에 잠가요',
          '포인터 · 속도 표준',
          '터치 조작 · 프린터 · 복사',
        ]);
        // 마우스 잠금 is the one switch; the others open something.
        expect(rows.map((row) => row.toggled).toList(), [
          null,
          if (relative) false,
          null,
          null,
        ]);
        expect(rows.map((row) => row.opens).toList(), [
          true,
          if (relative) false,
          true,
          true,
        ]);
        expect(rows.every((row) => row.onTap != null), isTrue);
        double? above;
        for (final key in keys) {
          final row = _panelRow(key);
          expect(row, findsOneWidget, reason: key);
          expect(
            tester.getSize(row).height,
            greaterThanOrEqualTo(48),
            reason: key,
          );
          final top = _top(tester, row);
          if (above != null) expect(top, greaterThan(above), reason: key);
          above = top;
        }
        expect(tester.takeException(), isNull);
      } finally {
        await _close(tester);
      }
    }
  });

  testWidgets('화질 in 설정 says the mode and frame rate in use; 입력 방식 '
      'says the current way', (tester) async {
    for (final (options, summary) in const [
      (StreamOptions(), '작업 · 60 fps'),
      (StreamOptions(fps: 30, bitrate: 8000000), '절약 · 30 fps'),
      (StreamOptions(fps: 30), '사용자 설정 · 30 fps'),
    ]) {
      await _open(tester, options: options);
      try {
        await _openMore(tester);
        final quality = tester.widget<SessionPanelRow>(
          _panelRow('android-session-settings'),
        );
        expect(quality.subtitle, summary);
        expect(quality.subtitle, contains('${options.fps} fps'));
      } finally {
        await _close(tester);
      }
    }
    for (final (touch, subtitle) in const [
      (AndroidTouchMode.trackpad, '포인터 · 속도 표준'),
      (AndroidTouchMode.direct, '터치'),
    ]) {
      await _open(tester, touch: touch);
      try {
        await _openMore(tester);
        expect(
          tester
              .widget<SessionPanelRow>(_panelRow('android-session-input'))
              .subtitle,
          subtitle,
          reason: touch.name,
        );
      } finally {
        await _close(tester);
      }
    }
  });

  testWidgets('마우스 잠금 in 설정 is a switch: off, a tap turns it on and closes '
      'the menu, and it reads on afterwards', (tester) async {
    final h = await _open(tester, relative: true);
    try {
      const key = 'android-session-game-mouse';
      Future<void> openMore() async {
        await tester.tap(find.byKey(const Key('expand-session-toolbar')));
        await _frames(tester);
        await tester.tap(find.byKey(const Key('android-session-more')));
        await _frames(tester);
        expect(find.byKey(const Key('android-more-panel')), findsOneWidget);
      }

      void closed() {
        expect(find.byKey(const Key('android-more-panel')), findsNothing);
        expect(_card, findsNothing);
      }

      await openMore();
      expect(_toggled(tester, key), isFalse);
      expect(_switchSemantics(tester, key)!.properties.enabled, isTrue);
      expect(
        find.descendant(
          of: _panelRow(key),
          matching: find.text('마우스를 데스크톱 안에 잠가요'),
        ),
        findsOneWidget,
      );
      expect(
        tester
            .widget<Switch>(
              find.descendant(
                of: _panelRow(key),
                matching: find.byType(Switch),
              ),
            )
            .value,
        isFalse,
      );

      await tester.tap(_panelRow(key));
      await _frames(tester);
      // The menu closes with the tap; the hand-over explains itself once.
      closed();
      expect(find.byKey(const Key('game-mouse-dialog')), findsOneWidget);
      await tester.tap(find.byKey(const Key('game-mouse-dialog-ok')));
      await _frames(tester);
      expect(h.capture.engages, 1);
      // Back takes the mouse back; the option stays on.
      await tester.binding.handlePopRoute();
      await _frames(tester);
      expect(h.capture.releases, 1);

      await openMore();
      expect(_toggled(tester, key), isTrue);
      expect(
        find.descendant(of: _panelRow(key), matching: find.text('뒤로 가기로 풀어요')),
        findsOneWidget,
      );
      expect(
        tester
            .widget<Switch>(
              find.descendant(
                of: _panelRow(key),
                matching: find.byType(Switch),
              ),
            )
            .value,
        isTrue,
      );
      // A tap turns it off again, without the dialog, and closes the menu.
      await tester.tap(_panelRow(key));
      await _frames(tester);
      closed();
      expect(find.byKey(const Key('game-mouse-dialog')), findsNothing);
      expect(h.capture.engages, 1);
      await openMore();
      expect(_toggled(tester, key), isFalse);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('마우스 잠금 without a mouse is greyed and says why; a mouse '
      'arriving makes it a working switch', (tester) async {
    final h = await _open(tester, relative: true, mouse: false);
    try {
      const key = 'android-session-game-mouse';
      await _openMore(tester);
      final row = _panelRow(key);
      expect(row, findsOneWidget);
      final greyed = tester.widget<SessionPanelRow>(row);
      expect(greyed.onTap, isNull);
      expect(greyed.disabledReason, '마우스를 연결하면 쓸 수 있어요');
      expect(greyed.subtitle, '마우스를 데스크톱 안에 잠가요');
      expect(_toggled(tester, key), isFalse);
      expect(_switchSemantics(tester, key)!.properties.enabled, isFalse);
      expect(find.byTooltip('마우스 잠금 · 마우스를 연결하면 쓸 수 있어요'), findsOneWidget);
      // A tap changes nothing: the panel stays, nothing is handed over.
      await tester.tap(row);
      await _frames(tester);
      expect(find.byKey(const Key('android-more-panel')), findsOneWidget);
      expect(find.byKey(const Key('game-mouse-dialog')), findsNothing);
      expect(_toggled(tester, key), isFalse);
      expect(h.capture.engages, 0);

      // A mouse arrives (USB, Bluetooth, DeX).
      h.capture.plug(true);
      await _frames(tester);
      final ready = tester.widget<SessionPanelRow>(row);
      expect(ready.onTap, isNotNull);
      expect(ready.disabledReason, isNull);
      expect(_switchSemantics(tester, key)!.properties.enabled, isTrue);
      expect(find.byTooltip('마우스 잠금 · 마우스를 연결하면 쓸 수 있어요'), findsNothing);
    } finally {
      await _close(tester);
    }
  });

  // ------------------------------------------------------------ navigation

  testWidgets('Back goes one level up: 화질 to 설정, 설정 to the menu card, '
      'then the menu closes', (tester) async {
    final h = await _open(tester);
    try {
      final more = find.byKey(const Key('android-more-panel'));
      final quality = find.byKey(const Key('android-settings-panel'));
      await _openMore(tester);
      await _tap(tester, _panelRow('android-session-settings'));
      expect(quality, findsOneWidget);
      expect(more, findsNothing);
      expect(
        tester.widget<AndroidStreamSettingsPanel>(quality).options.fps,
        60,
      );
      expect(
        find.descendant(of: quality, matching: find.text('화질')),
        findsOneWidget,
      );

      await _back(tester);
      expect(more, findsOneWidget);
      expect(quality, findsNothing);
      expect(_card, findsNothing);

      await _back(tester);
      expect(_card, findsOneWidget);
      expect(more, findsNothing);

      await _back(tester);
      expect(_card, findsNothing);
      expect(find.byKey(const Key('session-toolbar-pointer')), findsNothing);
      // Nothing was asked of the broker or remembered on the way.
      expect(h.requested, hasLength(1));
      expect(h.remembered, isEmpty);
      expect(find.text('연결을 끊을까요?'), findsNothing);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('The panel\'s own back arrow does the same, and 취소 in 화질 '
      'returns to 설정', (tester) async {
    final h = await _open(tester);
    try {
      final more = find.byKey(const Key('android-more-panel'));
      final quality = find.byKey(const Key('android-settings-panel'));
      final arrow = find.byKey(const Key('session-menu-back'));
      await _openMore(tester);
      await _tap(tester, _panelRow('android-session-settings'));
      expect(quality, findsOneWidget);
      await tester.tap(arrow);
      await tester.pumpAndSettle();
      expect(more, findsOneWidget);
      expect(quality, findsNothing);

      // 취소 after a choice: back in 설정, nothing applied.
      await _tap(tester, _panelRow('android-session-settings'));
      await _tap(tester, find.byKey(const Key('stream-preset-saver')));
      await _tap(tester, find.byKey(const Key('cancel-stream-settings')));
      expect(more, findsOneWidget);
      expect(quality, findsNothing);
      expect(
        tester
            .widget<SessionPanelRow>(_panelRow('android-session-settings'))
            .subtitle,
        '작업 · 60 fps',
      );
      expect(h.requested, hasLength(1));
      expect(h.remembered, isEmpty);

      await tester.tap(arrow);
      await tester.pumpAndSettle();
      expect(_card, findsOneWidget);
      expect(more, findsNothing);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('화면 and 연결 품질 go back to the menu card, by Back and by the '
      'arrow', (tester) async {
    await _open(tester);
    try {
      for (final (row, panel) in const [
        ('android-session-screen', 'android-screen-panel'),
        ('session-quality', 'android-connection-panel'),
        ('android-session-more', 'android-more-panel'),
      ]) {
        await _openRow(tester, row, panel);
        await _back(tester);
        expect(_card, findsOneWidget, reason: panel);
        expect(find.byKey(Key(panel)), findsNothing, reason: panel);

        await _openRow(tester, row, panel);
        await tester.tap(find.byKey(const Key('session-menu-back')));
        await tester.pumpAndSettle();
        expect(_card, findsOneWidget, reason: panel);
        expect(find.byKey(Key(panel)), findsNothing, reason: panel);
      }
    } finally {
      await _close(tester);
    }
  });

  testWidgets('입력 방식 from 설정 goes back to 설정 with the new way; from the '
      'long press on its cluster button it goes back to the menu card', (
    tester,
  ) async {
    final h = await _open(tester);
    try {
      final more = find.byKey(const Key('android-more-panel'));
      final input = find.byKey(const Key('android-input-panel'));
      final button = find.byKey(const Key('android-remote-touch-mode-button'));
      await _openMore(tester);
      await _tap(tester, _panelRow('android-session-input'));
      expect(input, findsOneWidget);
      expect(more, findsNothing);
      expect(tester.widget<AndroidToolbarPanel>(input).title, '입력 방식');
      // 포인터 has its 속도 here; 터치 has none.
      expect(find.byKey(const Key('android-speed-fast')), findsOneWidget);
      await _tap(tester, find.byKey(const Key('android-input-direct')));
      expect(input, findsOneWidget);
      expect(find.byKey(const Key('android-speed-fast')), findsNothing);
      expect(h.saved.last.touch, AndroidTouchMode.direct);

      await _back(tester);
      expect(more, findsOneWidget);
      expect(input, findsNothing);
      expect(
        tester
            .widget<SessionPanelRow>(_panelRow('android-session-input'))
            .subtitle,
        '터치',
      );

      // Into 입력 방식 once more, then ⋯ closes the whole menu from there.
      await _tap(tester, _panelRow('android-session-input'));
      expect(input, findsOneWidget);
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('session-toolbar-pointer')), findsNothing);
      expect(input, findsNothing);

      // The long press opens the same panel; Back leads to the menu card,
      // not to 설정 it was last opened from.
      await tester.longPress(button);
      await tester.pumpAndSettle();
      expect(input, findsOneWidget);
      await _back(tester);
      expect(_card, findsOneWidget);
      expect(input, findsNothing);
      expect(more, findsNothing);

      await _back(tester);
      expect(_card, findsNothing);
      // The same from a closed menu, by the panel's arrow.
      await tester.longPress(button);
      await tester.pumpAndSettle();
      expect(input, findsOneWidget);
      await tester.tap(find.byKey(const Key('session-menu-back')));
      await tester.pumpAndSettle();
      expect(_card, findsOneWidget);
      expect(input, findsNothing);
      await _back(tester);
      expect(_card, findsNothing);

      // 사용안내's 확인 closes the menu from a panel opened in 설정. What is
      // opened next does not go back to 설정: neither a row of the card ...
      Future<void> leaveByGuide() async {
        await _openMore(tester);
        await _tap(tester, _panelRow('android-session-help'));
        await _tap(tester, find.byKey(const Key('android-touch-help-close')));
        expect(_card, findsNothing);
        expect(find.byKey(const Key('android-touch-help')), findsNothing);
      }

      await leaveByGuide();
      await _openScreen(tester);
      await _back(tester);
      expect(_card, findsOneWidget);
      expect(more, findsNothing);
      await _back(tester);
      expect(_card, findsNothing);
      // ... nor the long press.
      await leaveByGuide();
      await tester.longPress(button);
      await tester.pumpAndSettle();
      expect(input, findsOneWidget);
      await _back(tester);
      expect(_card, findsOneWidget);
      expect(more, findsNothing);
      expect(input, findsNothing);
    } finally {
      await _close(tester);
    }
  });

  // -------------------------------------------------------------- 사용안내

  testWidgets('사용안내 from 설정 keeps the touch gestures and lists 프린터 and '
      '복사·붙여넣기 as usable when granted and ready; Back returns to 설정', (
    tester,
  ) async {
    final h = await _open(
      tester,
      printer: true,
      clipboard: const ClipboardPolicy(textToRemote: true, textToLocal: true),
    );
    try {
      await _openMore(tester);
      await _tap(tester, _panelRow('android-session-help'));
      final help = find.byKey(const Key('android-touch-help'));
      expect(help, findsOneWidget);
      expect(find.byKey(const Key('android-more-panel')), findsNothing);
      expect(tester.widget<AndroidToolbarPanel>(help).title, '사용안내');
      Finder said(String text) =>
          find.descendant(of: help, matching: find.text(text));

      // The gestures of the current 입력 방식 (포인터).
      expect(_caption('입력 방식 · 포인터'), findsOneWidget);
      for (final gesture in const [
        '한 손가락으로 밀기',
        '탭',
        '두 번 탭하고 두 번째를 누른 채 움직이기',
        '두 손가락으로 위아래',
        '길게 누르기 · 두 손가락 탭',
        '두 손가락 벌리기',
        '세 손가락 탭',
        '네 손가락 탭',
      ]) {
        expect(said(gesture), findsOneWidget, reason: gesture);
      }

      // Then what this connection can use, as the guide model says it.
      final tips = SessionGuideContent.forAndroid(h.session).tips;
      expect(tips.map((tip) => tip.feature).toList(), [
        SessionGuideFeature.printer,
        SessionGuideFeature.clipboard,
      ]);
      final shown = tester.widget<AndroidTouchHelp>(
        find.byType(AndroidTouchHelp),
      );
      expect(shown.mode, AndroidTouchMode.trackpad);
      expect(shown.tips.map((tip) => tip.feature).toList(), [
        SessionGuideFeature.printer,
        SessionGuideFeature.clipboard,
      ]);
      final caption = _caption('이 연결에서 쓸 수 있는 기능');
      expect(find.descendant(of: help, matching: caption), findsOneWidget);
      expect(_top(tester, caption), greaterThan(_top(tester, said('네 손가락 탭'))));
      var above = _top(tester, caption);
      for (final (tip, title) in [(tips[0], '프린터'), (tips[1], '복사·붙여넣기')]) {
        final block = find.descendant(
          of: help,
          matching: find.byKey(Key('android-guide-tip-${tip.feature.name}')),
        );
        expect(block, findsOneWidget, reason: title);
        expect(_top(tester, block), greaterThan(above), reason: title);
        above = _top(tester, block);
        expect(tip.title, title);
        expect(tip.availability, SessionGuideAvailability.ready, reason: title);
        Finder inBlock(String text) =>
            find.descendant(of: block, matching: find.text(text));
        expect(inBlock(title), findsOneWidget, reason: title);
        expect(inBlock('사용 가능'), findsOneWidget, reason: title);
        expect(inBlock(tip.message), findsOneWidget, reason: title);
        expect(tip.message, isNot(contains('관리자에게')), reason: title);
      }
      expect(said('이번 연결에서 사용 불가'), findsNothing);
      expect(said('연결 확인 중'), findsNothing);
      expect(said('관리자에게 요청하세요.'), findsNothing);
      // Android offers neither USB devices nor shared folders.
      for (final feature in const ['usb', 'folders']) {
        expect(
          find.byKey(Key('android-guide-tip-$feature')),
          findsNothing,
          reason: feature,
        );
      }
      expect(said('USB 장치'), findsNothing);
      expect(said('공유 폴더'), findsNothing);
      expect(tester.takeException(), isNull);

      await _back(tester);
      expect(find.byKey(const Key('android-more-panel')), findsOneWidget);
      expect(help, findsNothing);
      // 확인 closes the whole menu, as before.
      await _tap(tester, _panelRow('android-session-help'));
      await _tap(tester, find.byKey(const Key('android-touch-help-close')));
      expect(help, findsNothing);
      expect(_card, findsNothing);
      expect(find.byKey(const Key('session-toolbar-pointer')), findsNothing);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('사용안내 says 프린터 and 복사·붙여넣기 cannot be used in a connection '
      'that was granted neither, and shows 터치\'s gestures in 터치', (
    tester,
  ) async {
    final h = await _open(tester, touch: AndroidTouchMode.direct);
    try {
      await _openMore(tester);
      await _tap(tester, _panelRow('android-session-help'));
      final help = find.byKey(const Key('android-touch-help'));
      expect(help, findsOneWidget);
      Finder said(String text) =>
          find.descendant(of: help, matching: find.text(text));
      expect(_caption('입력 방식 · 터치'), findsOneWidget);
      expect(said('한 손가락으로 위아래'), findsOneWidget);
      expect(said('두 번 탭하고 누른 채 움직이기'), findsOneWidget);
      expect(said('한 손가락으로 밀기'), findsNothing);
      expect(said('네 손가락 탭'), findsOneWidget);

      final tips = SessionGuideContent.forAndroid(h.session).tips;
      expect(tips, hasLength(2));
      expect(_caption('이 연결에서 쓸 수 있는 기능'), findsOneWidget);
      for (final (tip, title) in [(tips[0], '프린터'), (tips[1], '복사·붙여넣기')]) {
        expect(
          tip.availability,
          SessionGuideAvailability.unavailable,
          reason: title,
        );
        final block = find.descendant(
          of: help,
          matching: find.byKey(Key('android-guide-tip-${tip.feature.name}')),
        );
        expect(block, findsOneWidget, reason: title);
        Finder inBlock(String text) =>
            find.descendant(of: block, matching: find.text(text));
        expect(inBlock(title), findsOneWidget, reason: title);
        expect(inBlock('이번 연결에서 사용 불가'), findsOneWidget, reason: title);
        expect(inBlock('관리자에게 요청하세요.'), findsOneWidget, reason: title);
        expect(inBlock('사용 가능'), findsNothing, reason: title);
      }
      expect(find.byKey(const Key('android-guide-tip-usb')), findsNothing);
      expect(find.byKey(const Key('android-guide-tip-folders')), findsNothing);
      expect(tester.takeException(), isNull);
    } finally {
      await _close(tester);
    }
  });

  // ------------------------------------------------ SessionGuideContent

  test('forAndroid lists only 프린터 and 복사·붙여넣기, each unavailable without '
      'its grant', () {
    final tips = SessionGuideContent.forAndroid(
      _guideSession(channel: false, sync: false),
    ).tips;
    expect(tips.map((tip) => tip.feature).toList(), [
      SessionGuideFeature.printer,
      SessionGuideFeature.clipboard,
    ]);
    expect(tips.map((tip) => tip.title).toList(), ['프린터', '복사·붙여넣기']);
    for (final tip in tips) {
      expect(tip.availability, SessionGuideAvailability.unavailable);
      expect(tip.message, '관리자에게 요청하세요.');
    }
    // Ready channels without the broker's grant change nothing.
    final stale = _guideSession(channel: false, sync: false)
      ..printer = _printer()
      ..clipboard = _Clipboard(const ClipboardPolicy.all());
    for (final tip in SessionGuideContent.forAndroid(stale).tips) {
      expect(tip.availability, SessionGuideAvailability.unavailable);
      expect(tip.message, '관리자에게 요청하세요.');
    }
  });

  test('forAndroid says which way text can be copied: both ways, to the '
      'desktop only, to the device only', () {
    for (final (policy, message) in const [
      (
        ClipboardPolicy(textToRemote: true, textToLocal: true),
        '복사한 글을 기기와 데스크톱 사이에서 붙여넣을 수 있어요.',
      ),
      (ClipboardPolicy(textToRemote: true), '기기에서 복사한 글을 데스크톱에 붙여넣을 수 있어요.'),
      (ClipboardPolicy(textToLocal: true), '데스크톱에서 복사한 글을 기기에 붙여넣을 수 있어요.'),
    ]) {
      // Before the channel is paired: what the broker granted.
      final waiting = SessionGuideContent.forAndroid(
        _guideSession(policy: policy),
      ).tips.last;
      expect(waiting.feature, SessionGuideFeature.clipboard);
      expect(waiting.availability, SessionGuideAvailability.preparing);
      expect(waiting.message, message);
      // Paired: what the pair allows now, even where the grant was wider.
      final paired = SessionGuideContent.forAndroid(
        _guideSession(policy: const ClipboardPolicy.all())
          ..clipboard = _Clipboard(policy),
      ).tips.last;
      expect(paired.availability, SessionGuideAvailability.ready);
      expect(paired.message, message);
      // A channel still pairing reads the grant.
      final pairing = SessionGuideContent.forAndroid(
        _guideSession(policy: policy)
          ..clipboard = _Clipboard(const ClipboardPolicy(), ready: false),
      ).tips.last;
      expect(pairing.availability, SessionGuideAvailability.preparing);
      expect(pairing.message, message);
    }
  });

  test('forAndroid: files alone, a missing sync grant or an ended channel '
      'leave 복사·붙여넣기 unavailable', () {
    SessionGuideTip clipboard(RemoteSession session) =>
        SessionGuideContent.forAndroid(session).tips.last;
    const text = ClipboardPolicy(textToRemote: true, textToLocal: true);
    for (final session in [
      // Android copies text only.
      _guideSession(
        policy: const ClipboardPolicy(filesToRemote: true, filesToLocal: true),
      ),
      _guideSession(policy: text, sync: false),
      _guideSession(policy: text, channel: false),
      _guideSession(policy: text)..clipboard = _Clipboard(text, ended: true),
    ]) {
      final tip = clipboard(session);
      expect(tip.availability, SessionGuideAvailability.unavailable);
      expect(tip.message, '관리자에게 요청하세요.');
    }
  });

  test('forAndroid: 프린터 is ready with the VM\'s printer, waits for it, and '
      'is unavailable after a failure', () {
    SessionGuideTip printer(RemoteSession session) =>
        SessionGuideContent.forAndroid(session).tips.first;
    final waiting = printer(_guideSession(printing: true));
    expect(waiting.availability, SessionGuideAvailability.preparing);
    expect(waiting.message, contains('Multitap 프린터'));
    final queued = printer(
      _guideSession(printing: true)..printer = _printer(queueReady: false),
    );
    expect(queued.availability, SessionGuideAvailability.preparing);
    final ready = printer(_guideSession(printing: true)..printer = _printer());
    expect(ready.availability, SessionGuideAvailability.ready);
    expect(ready.message, waiting.message);
    for (final failed in [
      _printer()..queueReason = 'print_queue_failed',
      _printer()..closed = true,
    ]) {
      final tip = printer(_guideSession(printing: true)..printer = failed);
      expect(tip.availability, SessionGuideAvailability.unavailable);
      expect(tip.message, '관리자에게 요청하세요.');
    }
  });

  // -------------------------------------------------- SessionPoorLatency

  group('SessionPoorLatency', () {
    const hold = SessionPoorLatency.hold;
    const almost = Duration(milliseconds: 4999);

    test('the hold is 5 s', () {
      expect(hold, const Duration(seconds: 5));
    });

    testWidgets('a spike shorter than 5 s never sets it', (tester) async {
      var changes = 0;
      final poor = SessionPoorLatency(() => changes++);
      addTearDown(poor.dispose);
      expect(poor.sustained, isFalse);
      poor.update(connected: true, ms: 142);
      await tester.pump(almost);
      expect(poor.sustained, isFalse);
      poor.update(connected: true, ms: 12);
      await tester.pump(hold);
      await tester.pump(hold);
      expect(poor.sustained, isFalse);
      expect(changes, 0);
    });

    testWidgets('5 s at 80 ms or more sets it and calls onChanged once', (
      tester,
    ) async {
      var changes = 0;
      final poor = SessionPoorLatency(() => changes++);
      addTearDown(poor.dispose);
      poor.update(connected: true, ms: SessionChrome.fairMs);
      // More slow samples meanwhile do not start the wait again.
      await tester.pump(const Duration(seconds: 3));
      poor.update(connected: true, ms: 142);
      await tester.pump(const Duration(milliseconds: 1999));
      expect(poor.sustained, isFalse);
      expect(changes, 0);
      await tester.pump(const Duration(milliseconds: 1));
      expect(poor.sustained, isTrue);
      expect(changes, 1);
      poor.update(connected: true, ms: 142);
      await tester.pump(hold);
      await tester.pump(hold);
      expect(poor.sustained, isTrue);
      expect(changes, 1);
    });

    testWidgets('just under 80 ms, no measurement and nonsense are not slow', (
      tester,
    ) async {
      var changes = 0;
      final poor = SessionPoorLatency(() => changes++);
      addTearDown(poor.dispose);
      for (final ms in <double?>[79.9, null, 0, -1, double.nan]) {
        poor.update(connected: true, ms: ms);
        await tester.pump(hold);
        expect(poor.sustained, isFalse, reason: '$ms');
      }
      expect(changes, 0);
    });

    testWidgets('a dip shorter than 5 s keeps it; 5 s better clears it', (
      tester,
    ) async {
      var changes = 0;
      final poor = SessionPoorLatency(() => changes++);
      addTearDown(poor.dispose);
      poor.update(connected: true, ms: 142);
      await tester.pump(hold);
      expect(poor.sustained, isTrue);
      expect(changes, 1);

      poor.update(connected: true, ms: 12);
      await tester.pump(almost);
      expect(poor.sustained, isTrue);
      poor.update(connected: true, ms: 142);
      await tester.pump(hold);
      expect(poor.sustained, isTrue, reason: 'one good sample changes nothing');
      expect(changes, 1);

      poor.update(connected: true, ms: 12);
      await tester.pump(almost);
      expect(poor.sustained, isTrue);
      await tester.pump(const Duration(milliseconds: 1));
      expect(poor.sustained, isFalse);
      expect(changes, 2);
    });

    testWidgets('connected: false clears it at once and drops a pending '
        'change', (tester) async {
      var changes = 0;
      final poor = SessionPoorLatency(() => changes++);
      addTearDown(poor.dispose);
      poor.update(connected: true, ms: 142);
      await tester.pump(hold);
      expect(poor.sustained, isTrue);
      expect(changes, 1);
      // The value is read in the same build: no callback for it.
      poor.update(connected: false, ms: 142);
      expect(poor.sustained, isFalse);
      expect(changes, 1);
      await tester.pump(hold);
      expect(poor.sustained, isFalse);
      expect(changes, 1);

      // Slow for a while, then the connection ends: nothing arrives later.
      poor.update(connected: true, ms: 142);
      await tester.pump(almost);
      poor.update(connected: false, ms: null);
      await tester.pump(hold);
      expect(poor.sustained, isFalse);
      expect(changes, 1);
      // A new connection starts its own 5 s.
      poor.update(connected: true, ms: 142);
      await tester.pump(almost);
      expect(poor.sustained, isFalse);
      await tester.pump(const Duration(milliseconds: 1));
      expect(poor.sustained, isTrue);
      expect(changes, 2);
    });

    testWidgets('dispose cancels a pending change', (tester) async {
      var changes = 0;
      final poor = SessionPoorLatency(() => changes++);
      poor.update(connected: true, ms: 142);
      await tester.pump(almost);
      poor.dispose();
      await tester.pump(hold);
      expect(poor.sustained, isFalse);
      expect(changes, 0);
    });
  });
}

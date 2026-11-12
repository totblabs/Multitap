// The session toolbar is one vocabulary on both platforms: the
// Windows bar with its three panels and the Android ⋯ menu show the same
// entries in the same order with the same names. Windows lays the groups
// along its bar (연결 | 화면 · 폴더·USB · 설정 | 창 버튼 | 연결 끊기) and
// keeps each group's entries in the panel under its button; Android lists
// the bar's commands down its menu as rows (연결 품질, 화면, 설정, 연결
// 끊기) and keeps the same entries in the same panels, opened in the menu's
// place. What only Windows has is listed
// below by name; Android has nothing of its own in the menu
// (no 클립보드 or 키 조합), only its cluster buttons
// beside ⋯ and 입력 방식 as a row of its 설정 panel. Both real views are
// rendered with every grant on; socket, media, device and native sides are
// fixtures (no VM, no device).
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_toolbar.dart'
    show AndroidBarMenuRow, AndroidMenuCaption, AndroidPanelChoices;
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_menu.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_menu.dart';
import 'package:multitap_client/ui/session_panels.dart' show SessionPanelRow;
import 'package:multitap_client/ui/session_tools.dart';

import 'android/android_session_contract_test.dart' show AndroidNativeFake;
import 'drive_session_test.dart' show FakeFiles;
import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;
import 'stream_display_entry_test.dart' show entryMachine;
import 'usb_redirection_test.dart' show FakeBackend;

/// The toolbar's entries in order, with their one name ([SessionTool]).
const sessionToolOrder = [
  ('quality', '연결 품질'),
  // 화면: the screen mode, with 배율 in the same panel on Windows.
  ('screen', '화면'),
  ('scale', '배율'),
  // 폴더·USB: the Windows bar's one button; a tab for each side.
  ('share', '폴더·USB'),
  ('drives', '공유 폴더'),
  ('usb', 'USB 장치'),
  // 설정: the Windows bar's button; 화질 is the former 설정.
  ('more', '설정'),
  ('settings', '화질'),
  // 2026-11-03: the former 게임 마우스, and 마우스 가두기 is gone.
  ('gameMouse', '마우스 잠금'),
  ('pin', '툴바 열어 두기'),
  ('guide', '사용안내'),
  ('minimize', '최소화'),
  ('restore', '창 모드로'),
  ('disconnect', '연결 끊기'),
];

/// Only Windows: the PC's own devices (폴더·USB with its 공유 폴더 and USB
/// 장치 tabs), which Android does not redirect; 툴바 열어 두기 (Android's
/// menu is a card, not a bar, and its three buttons are always there); and
/// the window's buttons in full screen (최소화, 창 모드로). 설정 is on both
/// since 2026-11-11: Android's menu has the same row with the same panel.
const windowsOnly = {'share', 'drives', 'usb', 'pin', 'minimize', 'restore'};

/// Only Android: nothing of the vocabulary (2026-11-03). Its extras are the
/// cluster's one-tap buttons and [androidPanelExtras] below, which are not
/// entries of the vocabulary.
const androidOnly = <String>{};

/// Android's own row inside its 설정 panel, outside the vocabulary: 입력
/// 방식 (how a finger works; the PC has a mouse), between the switches and
/// 사용안내.
const androidPanelExtras = [('android-session-input', '입력 방식')];

/// Android's one-tap buttons beside ⋯ (2026-11-02: always outside the
/// menu), in their cluster order under ⋯ (2026-11-03): not menu entries.
const androidCluster = [
  ('android-remote-touch-mode-button', '입력 방식'),
  ('android-remote-keyboard-button', '키보드'),
];

/// On the Windows bar itself, left to right (full screen, every grant on).
const _windowsBar = {
  'quality': 'session-quality',
  'screen': 'stream-menu-화면',
  'share': 'remote-share',
  'more': 'remote-settings',
  'minimize': 'session-minimize',
  'restore': 'session-restore',
  'disconnect': 'session-disconnect',
};

/// In the panel under a bar button: the group's own entries, in order.
const _windowsPanels = {
  'screen': ['scale'],
  'share': ['drives', 'usb'],
  'more': ['settings', 'gameMouse', 'pin', 'guide'],
};

const _windowsPanelKeys = {
  'scale': 'remote-display-scale',
  'drives': 'remote-drives',
  'usb': 'remote-usb',
  'settings': 'remote-stream-settings',
  'gameMouse': 'session-game-mouse',
  'pin': 'pin-session-toolbar',
  'guide': 'remote-session-guide',
};

/// On the Android menu card itself, top to bottom: the Windows bar's
/// commands as rows.
const _androidMenu = {
  'quality': 'session-quality',
  'screen': 'android-session-screen',
  'more': 'android-session-more',
  'disconnect': 'session-disconnect',
};

/// In the panel a menu row opens in the card's place: the group's own
/// entries, in order (the Windows panels' less what only Windows has).
const _androidPanels = {
  'screen': ['scale'],
  'more': ['settings', 'gameMouse', 'guide'],
};

/// The card each of those rows opens.
const _androidPanelCards = {
  'screen': 'android-screen-panel',
  'more': 'android-more-panel',
};

const _androidPanelKeys = {
  'scale': 'android-screen-scale',
  'settings': 'android-session-settings',
  'gameMouse': 'android-session-game-mouse',
  'guide': 'android-session-help',
};

String _label(String id) =>
    sessionToolOrder.firstWhere((entry) => entry.$1 == id).$2;

String _androidLabel(String id) => _label(id);

/// The entries shown, in reading order along [axis].
List<String> _shown(WidgetTester tester, Map<String, String> keys, Axis axis) {
  final found = [
    for (final MapEntry(key: id, value: key) in keys.entries)
      if (find.byKey(Key(key)).evaluate().isNotEmpty)
        (id, tester.getRect(find.byKey(Key(key)).first)),
  ];
  found.sort(
    (a, b) => axis == Axis.horizontal
        ? a.$2.left.compareTo(b.$2.left)
        : a.$2.top.compareTo(b.$2.top),
  );
  return [for (final (id, _) in found) id];
}

List<String> _expected(Set<String> without) => [
  for (final (id, _) in sessionToolOrder)
    if (!without.contains(id)) id,
];

/// No clipboard sharing yet on the PC's side.
class _WindowsMedia extends SessionMedia {
  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    // Not itself inside a remote desktop: 마우스 잠금 may hand over.
    'remoteSession': false,
  };
}

final _shareUrl = Uri.parse('wss://fixture.invalid/api/v1/sessions/parity');

/// Granted, ready shared folders with nothing shared (as
/// toolbar_device_state_test's): no file system, no socket.
class _Drive extends DriveSession {
  _Drive()
    : super(
        url: _shareUrl,
        token: 'fixture',
        sessionId: 'parity',
        files: FakeFiles(),
      );
  @override
  Future<void> start() async {
    ready = true;
    notifyListeners();
  }

  @override
  Future<void> close({String reason = 'drive_session_closed'}) async {
    closed = true;
    ready = false;
    notifyListeners();
  }
}

/// Granted, ready USB with no device (no capture backend, no socket).
class _Usb extends UsbSession {
  _Usb()
    : super(
        url: _shareUrl,
        token: 'fixture',
        sessionId: 'parity',
        backend: FakeBackend(),
      );
  bool ended = false;
  @override
  bool get ready => !ended;
  @override
  bool get closed => ended;
  @override
  Future<void> start() async => notifyListeners();
  @override
  Future<void> refresh() async {}
  @override
  Future<void> close({String reason = 'usb_session_closed'}) async {
    ended = true;
    notifyListeners();
  }
}

/// A popup route opened or closed: its short transition, without waiting
/// for anything else in the view to come to rest.
Future<void> _popup(WidgetTester tester) async {
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
}

Future<(RemoteSession, _WindowsMedia)> _windows(WidgetTester tester) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = const Size(1600, 900);
  addTearDown(tester.view.reset);
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  for (final name in [
    'multitap/window',
    'multitap/window/events',
    'multitap/relative-mouse',
  ]) {
    final channel = MethodChannel(name);
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => call.method == 'fullscreen' ? true : null,
    );
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
  }
  final socket = SessionSocket();
  final media = _WindowsMedia();
  final drive = _Drive(), usb = _Usb();
  final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: 'parity',
      webSocketUrl: Uri.parse('wss://fixture.invalid/api/v1/sessions/parity'),
      // Every grant on: shared folders and USB both usable, so 폴더·USB
      // opens with a tab for each (a side without its grant is covered by
      // session_toolbar_items_test and toolbar_device_state_test).
      driveWebSocketUrl: _shareUrl,
      usbRequested: true,
      usbWebSocketUrl: _shareUrl,
      cursor: true,
      relativeMouse: true,
    ),
    token: 'fixture-only-token',
    media: media,
    dialer: (_, _) async => socket,
    driveFactory: (_, _) => drive,
    usbFactory: (_, _) => usb,
  );
  final machine = entryMachine(1);
  var connected = false;
  await tester.pumpWidget(
    MaterialApp(
      navigatorObservers: [MenuDismissObserver()],
      home: RemoteView(
        machine: machine,
        prepareDisplaysOnEntry: true,
        windowActions: SessionWindowActions(closeWindow: () async {}),
        initialDisplayMode: SessionDisplayMode.fullscreen,
        load: () async => session,
        onReconnect: (_) async {
          if (connected) throw const BrokerException('network_error');
          connected = true;
          return session;
        },
        onConfigureDisplays: (count) async => machine,
      ),
    ),
  );
  await tester.pump();
  await tester.pump();
  socket.incoming.add(jsonEncode(info));
  await tester.pump();
  await tester.pump();
  socket.incoming.add(packet());
  await tester.pump();
  await tester.pump();
  expect(session.frameReady, isTrue, reason: session.error);
  // The media's first statistics (not in a remote desktop).
  await tester.pump(const Duration(seconds: 1));
  await tester.pump();
  if (find.byKey(const Key('session-command-bar')).evaluate().isEmpty) {
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pump();
  }
  return (session, media);
}

/// As session_toolbar_items_test closes its view.
Future<void> _closeWindows(
  WidgetTester tester,
  (RemoteSession, _WindowsMedia) view,
) async {
  await tester.pumpWidget(const SizedBox.shrink());
  unawaited(view.$1.close());
  await tester.pump(const Duration(seconds: 3));
  await tester.pump();
  unawaited(view.$2.events.close());
}

/// A granted, ready clipboard (nothing is sent).
class _Clipboard extends ClipboardSession {
  _Clipboard()
    : super(
        url: Uri.parse('wss://fixture.invalid/api/v1/sessions/parity/clip'),
        token: 'not-persisted',
        sessionId: 'parity',
      ) {
    ready = true;
  }
  @override
  bool get canSend => true;
  @override
  bool get canReceive => true;
  @override
  Future<void> close({String reason = 'clipboard_transport_closed'}) async {}
}

Uint8List _video(int pts) {
  final bytes = Uint8List(17);
  bytes[0] = 1;
  bytes[1] = 1;
  for (var i = 0; i < 8; i++) {
    bytes[4 + i] = (pts >> (8 * i)) & 255;
  }
  bytes.setAll(12, [0, 0, 0, 1, 0x65]);
  return bytes;
}

Future<void> _android(WidgetTester tester, Size size) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = size;
  addTearDown(tester.view.reset);
  AndroidNativeFake().install();
  addTearDown(AndroidNativeFake.uninstall);
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  for (final channel in const [
    MethodChannel('multitap/pen-input'),
    AndroidSessionKeepalive.channel,
  ]) {
    messenger.setMockMethodCallHandler(channel, (_) async => null);
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
  }
  addTearDown(
    () => messenger.setMockMethodCallHandler(AndroidStore.channel, null),
  );
  final sockets = <SessionSocket>[];
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: AndroidRemoteView(
        name: 'TESTER',
        token: 'not-persisted',
        options: const StreamOptions(),
        density: () async => null,
        displayScale: true,
        settings: const AndroidSessionSettings(helpSeen: true),
        load: (options, _, scale) async => BrokerConnection(
          sessionId: 'parity',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/api/v1/sessions/parity',
          ),
          options: options,
          epochMedia: true,
          viewportProvisioning: true,
          displayScale: scale,
          cursor: true,
          relativeMouse: true,
        ),
        sessionFactory: (connection, media, _) => RemoteSession(
          connection: connection,
          token: 'not-persisted',
          media: media,
          dialer: (_, _) async {
            final socket = SessionSocket();
            sockets.add(socket);
            return socket;
          },
        )..clipboard = _Clipboard(),
        pictureBuilder: (_) => const ColoredBox(color: Color(0xFF1B4F9C)),
      ),
    ),
  );
  for (var i = 0; i < 4; i++) {
    await tester.pump(const Duration(milliseconds: 20));
  }
  sockets.last.incoming.add(
    jsonEncode({
      'type': 'stream.info',
      'codec': 'h264',
      'width': size.width.toInt() ~/ 2 * 2,
      'height': size.height.toInt() ~/ 2 * 2,
    }),
  );
  await tester.pump(const Duration(milliseconds: 20));
  sockets.last.incoming.add(_video(16667));
  for (var i = 0; i < 6; i++) {
    await tester.pump(const Duration(milliseconds: 40));
  }
  await tester.tap(find.byKey(const Key('expand-session-toolbar')));
  await tester.pumpAndSettle();
  addTearDown(() async {
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump(const Duration(seconds: 1));
  });
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('Windows: the bar shows the groups, and each panel its '
      'entries, in the shared order and names', (tester) async {
    final view = await _windows(tester);
    const bar = Key('session-command-bar');
    // 마우스 가두기 is gone (2026-11-03): 마우스 잠금 is the only mouse
    // entry, and nothing is called 게임 마우스 any more.
    void noOldMouseNames(String where) {
      expect(find.byKey(const Key('session-pointer-confine')), findsNothing);
      for (final gone in const ['가두기', '게임 마우스']) {
        expect(
          find.byWidgetPredicate(
            (widget) =>
                widget is Tooltip && (widget.message ?? '').contains(gone),
          ),
          findsNothing,
          reason: '$gone · $where',
        );
        expect(
          find.textContaining(gone),
          findsNothing,
          reason: '$gone · $where',
        );
      }
    }

    // The bar: the quality chip, the three groups, the window's buttons
    // (full screen) and 연결 끊기, in the vocabulary's order.
    expect(_shown(tester, _windowsBar, Axis.horizontal), [
      for (final (id, _) in sessionToolOrder)
        if (_windowsBar.containsKey(id)) id,
    ]);
    // The group buttons carry the shared name; the icon buttons say it
    // first in their hover text.
    for (final id in _windowsPanels.keys) {
      final menu = tester.widget<SessionToolbarMenu>(
        find.byKey(Key(_windowsBar[id]!)).first,
      );
      expect(menu.label, _label(id), reason: id);
      expect(
        find.descendant(
          of: find.byKey(Key(_windowsBar[id]!)),
          matching: find.text(_label(id)),
        ),
        findsOneWidget,
        reason: id,
      );
    }
    for (final id in ['minimize', 'restore', 'disconnect']) {
      final key = find.byKey(Key(_windowsBar[id]!));
      final tooltips = [
        ...find.ancestor(of: key, matching: find.byType(Tooltip)).evaluate(),
        ...find.descendant(of: key, matching: find.byType(Tooltip)).evaluate(),
      ].map((element) => (element.widget as Tooltip).message);
      expect(tooltips, contains(startsWith(_label(id))), reason: id);
    }
    // Nothing of a group sits on the bar itself (2026-11-10), there is no
    // full-screen toggle, and no command carries a chevron.
    for (final MapEntry(key: id, value: key) in _windowsPanelKeys.entries) {
      expect(find.byKey(Key(key)), findsNothing, reason: id);
    }
    expect(find.byKey(const Key('session-fullscreen')), findsNothing);
    expect(
      find.descendant(
        of: find.byKey(bar),
        matching: find.byIcon(Icons.expand_more_rounded),
      ),
      findsNothing,
    );
    noOldMouseNames('bar');

    // Each group's panel: its entries in the vocabulary's order, across
    // for 폴더·USB's tabs and down for the others, each with its one name.
    for (final MapEntry(key: group, value: members) in _windowsPanels.entries) {
      await tester.tap(find.byKey(Key(_windowsBar[group]!)));
      await _popup(tester);
      expect(
        _shown(tester, {
          for (final id in members) id: _windowsPanelKeys[id]!,
        }, group == 'share' ? Axis.horizontal : Axis.vertical),
        [
          for (final (id, _) in sessionToolOrder)
            if (members.contains(id)) id,
        ],
        reason: group,
      );
      for (final id in members) {
        expect(
          find.descendant(
            of: find.byKey(Key(_windowsPanelKeys[id]!)),
            matching: find.text(_label(id)),
          ),
          findsOneWidget,
          reason: id,
        );
      }
      noOldMouseNames(group);
      // The bar stays open under its panel.
      expect(find.byKey(bar), findsOneWidget, reason: group);
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await _popup(tester);
      for (final id in members) {
        expect(
          find.byKey(Key(_windowsPanelKeys[id]!)),
          findsNothing,
          reason: id,
        );
      }
    }
    await _closeWindows(tester, view);
  });

  for (final (size, label) in const [
    (Size(412, 915), 'phone'),
    (Size(915, 412), 'phone on its side'),
    (Size(1280, 800), 'tablet'),
  ]) {
    testWidgets('Android ($label): the ⋯ menu lists the PC bar\'s commands '
        'as rows and each panel its entries, in the shared order and names; '
        '키보드 and 입력 방식 are the cluster\'s', (tester) async {
      await _android(tester, size);
      // The menu card: 연결 품질, 화면, 설정 and 연결 끊기, top to bottom,
      // in the vocabulary's order.
      final onMenu = [
        for (final (id, _) in sessionToolOrder)
          if (_androidMenu.containsKey(id)) id,
      ];
      expect(onMenu, ['quality', 'screen', 'more', 'disconnect']);
      expect(_shown(tester, _androidMenu, Axis.vertical), onMenu);
      final menu = find.byKey(const Key('session-command-bar'));
      // Nothing else in the menu (2026-11-03): every row is a shared entry
      // with its one name and icon, and the no-longer Android-only 클립보드
      // and 키 조합 are gone.
      final rows = tester
          .widgetList<AndroidBarMenuRow>(
            find.descendant(of: menu, matching: find.byType(AndroidBarMenuRow)),
          )
          .toList();
      expect(
        [for (final row in rows) row.item.label],
        [for (final id in onMenu) _androidLabel(id)],
      );
      for (final (index, id) in onMenu.indexed) {
        final tool = SessionTool.values.byName(id);
        final row = find.byKey(Key(_androidMenu[id]!));
        expect(rows[index].item.icon, tool.icon, reason: id);
        expect(
          find.descendant(of: row, matching: find.text(_androidLabel(id))),
          findsOneWidget,
          reason: id,
        );
        expect(
          find.descendant(of: row, matching: find.byIcon(tool.icon)),
          findsOneWidget,
          reason: id,
        );
      }
      // The Windows bar has no captions and neither has the card
      // (2026-11-11): 화면 and 설정 are rows, not captions over rows. No
      // 공유 group on a phone, and the former captions are gone.
      expect(
        find.descendant(of: menu, matching: find.byType(AndroidMenuCaption)),
        findsNothing,
      );
      for (final gone in const ['화면·영상', '내 기기 공유', '입력·도움말', '공유']) {
        expect(
          find.descendant(of: menu, matching: find.text(gone)),
          findsNothing,
          reason: gone,
        );
      }
      // Nothing of a group sits on the card itself, as nothing of a group
      // sits on the Windows bar: no switch, no row of a panel.
      expect(find.byKey(const Key('android-session-row-switch')), findsNothing);
      for (final key in [
        ..._androidPanelKeys.values,
        for (final (key, _) in androidPanelExtras) key,
        'android-session-scale',
        'android-session-clipboard',
        'android-session-shortcuts',
      ]) {
        expect(find.byKey(Key(key)), findsNothing, reason: key);
      }
      // Not duplicated in the menu: they are one tap beside it.
      for (final (key, name) in androidCluster) {
        expect(find.byKey(Key(key)), findsOneWidget, reason: name);
        expect(
          find.descendant(of: menu, matching: find.text(name)),
          findsNothing,
          reason: name,
        );
      }
      final ordered = [
        for (final (key, _) in androidCluster)
          tester.getRect(find.byKey(Key(key))),
      ];
      final tools = tester.getRect(
        find.byKey(const Key('expand-session-toolbar')),
      );
      // ⋯, then 입력 방식, then 키보드 (2026-11-03).
      expect(tools.top, lessThanOrEqualTo(ordered.first.top));
      expect(ordered.first.top, lessThan(ordered.last.top));

      // Each row's panel, in the card's place: its entries in the
      // vocabulary's order, each with its one name, as in the Windows
      // panel under the same button.
      Future<Finder> open(String group) async {
        await tester.tap(find.byKey(Key(_androidMenu[group]!)));
        // Bounded: the 화면 panel follows the live session and never settles.
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 400));
        final card = find.byKey(Key(_androidPanelCards[group]!));
        expect(card, findsOneWidget, reason: group);
        expect(menu, findsNothing, reason: 'in the menu\'s place · $group');
        // The panel carries its row's name.
        expect(
          find.descendant(of: card, matching: find.text(_androidLabel(group))),
          findsOneWidget,
          reason: group,
        );
        final members = _androidPanels[group]!;
        expect(
          _shown(tester, {
            for (final id in members) id: _androidPanelKeys[id]!,
          }, Axis.vertical),
          [
            for (final (id, _) in sessionToolOrder)
              if (members.contains(id)) id,
          ],
          reason: group,
        );
        for (final id in members) {
          expect(
            find.descendant(
              of: card,
              matching: find.byKey(Key(_androidPanelKeys[id]!)),
            ),
            findsOneWidget,
            reason: id,
          );
        }
        return card;
      }

      // Back shows the menu card again.
      Future<void> back(String group) async {
        await tester.tap(find.byKey(const Key('session-menu-back')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 400));
        expect(
          find.byKey(Key(_androidPanelCards[group]!)),
          findsNothing,
          reason: group,
        );
        expect(menu, findsOneWidget, reason: group);
      }

      // 화면: 배율 is inside it, Windows' five steps, after how the picture
      // sits here.
      await open('screen');
      final scale = find.byKey(Key(_androidPanelKeys['scale']!));
      final steps = tester.widget<AndroidPanelChoices>(scale);
      expect(steps.label, contains(_androidLabel('scale')));
      expect(
        [for (final item in steps.items) item.key],
        [
          for (final percent in const [100, 125, 150, 175, 200])
            Key('android-screen-scale-$percent'),
        ],
      );
      expect(
        [for (final item in steps.items) item.label],
        ['100', '125', '150', '175', '200'],
      );
      for (final item in steps.items) {
        expect(
          find.descendant(of: scale, matching: find.byKey(item.key)),
          findsOneWidget,
          reason: '${item.key}',
        );
      }
      expect(find.byKey(const Key('android-screen-scale-0')), findsNothing);
      expect(find.byKey(const Key('android-scale-panel')), findsNothing);
      expect(
        tester.getRect(scale).top,
        greaterThanOrEqualTo(
          tester.getRect(find.byKey(const Key('android-session-move'))).bottom -
              .01,
        ),
      );
      await back('screen');

      // 설정: 화질, 마우스 잠금 (a switch row, as in the Windows 설정
      // panel), Android's own 입력 방식, then 사용안내.
      final more = await open('more');
      expect(
        find.descendant(
          of: more,
          matching: find.byKey(const Key('session-settings-panel')),
        ),
        findsOneWidget,
      );
      expect(
        _shown(tester, {
          for (final id in _androidPanels['more']!) id: _androidPanelKeys[id]!,
          for (final (key, name) in androidPanelExtras) name: key,
        }, Axis.vertical),
        ['settings', 'gameMouse', '입력 방식', 'guide'],
      );
      for (final id in _androidPanels['more']!) {
        final tool = SessionTool.values.byName(id);
        final row = tester.widget<SessionPanelRow>(
          find.byKey(Key(_androidPanelKeys[id]!)),
        );
        expect(row.title, _androidLabel(id), reason: id);
        expect(row.icon, tool.icon, reason: id);
        expect(
          find.descendant(
            of: find.byKey(Key(_androidPanelKeys[id]!)),
            matching: find.text(_androidLabel(id)),
          ),
          findsOneWidget,
          reason: id,
        );
        // Only 마우스 잠금 turns something on and off; the others open.
        expect(row.toggled, id == 'gameMouse' ? isFalse : isNull, reason: id);
        expect(row.opens, id != 'gameMouse', reason: id);
      }
      expect(
        find.descendant(of: more, matching: find.byType(Switch)),
        findsOneWidget,
        reason: 'Only 마우스 잠금 turns something on and off.',
      );
      // 입력 방식 is Android's own: outside the vocabulary, with the name
      // of its cluster button.
      for (final (key, name) in androidPanelExtras) {
        expect(SessionTool.named(name), isNull, reason: name);
        final row = tester.widget<SessionPanelRow>(find.byKey(Key(key)));
        expect(row.title, name);
        expect(row.opens, isTrue);
        expect(androidCluster.map((button) => button.$2), contains(name));
      }
      // What only Windows has is not in the panel either.
      for (final id in _windowsPanels['more']!) {
        if (!windowsOnly.contains(id)) continue;
        expect(
          find.byKey(Key(_windowsPanelKeys[id]!)),
          findsNothing,
          reason: id,
        );
      }
      await back('more');
    });
  }

  test('both platforms list the same shared entries once in one order', () {
    final windows = _expected(androidOnly)
        .where((id) => !windowsOnly.contains(id))
        .toList();
    final android = _expected(windowsOnly)
        .where((id) => !androidOnly.contains(id))
        .toList();
    expect(windows, android);
    expect(windowsOnly.intersection(androidOnly), isEmpty);
    expect(
      sessionToolOrder.map((entry) => entry.$2).toSet(),
      hasLength(sessionToolOrder.length),
    );
    // The shared vocabulary is exactly this list: no Android-only entry.
    expect(androidOnly, isEmpty);
    expect([
      for (final tool in SessionTool.values) (tool.name, tool.label),
    ], sessionToolOrder);
    // Every entry has its one place on Windows, on the bar or in the panel
    // of its group, and the panels follow the vocabulary's groups.
    final ids = [for (final (id, _) in sessionToolOrder) id];
    expect({
      ..._windowsBar.keys,
      ..._windowsPanelKeys.keys,
    }, ids.toSet().difference(androidOnly));
    expect(
      _windowsBar.keys.toSet().intersection(_windowsPanelKeys.keys.toSet()),
      isEmpty,
    );
    expect(
      [for (final members in _windowsPanels.values) ...members],
      [
        for (final id in ids)
          if (_windowsPanelKeys.containsKey(id)) id,
      ],
    );
    for (final MapEntry(key: group, value: members) in _windowsPanels.entries) {
      final button = SessionTool.values.byName(group);
      expect(_windowsBar.containsKey(group), isTrue, reason: group);
      for (final id in members) {
        expect(
          SessionTool.values.byName(id).group,
          button.group,
          reason: '$id is in $group',
        );
      }
    }
    // What Android shows is the vocabulary less what only Windows has, each
    // entry once: on its menu card where Windows has it on its bar, or in
    // the panel where Windows has it in the panel under the same button.
    // What only Windows has is on its bar or in a panel.
    expect({
      ..._androidMenu.keys,
      ..._androidPanelKeys.keys,
    }, ids.toSet().difference(windowsOnly));
    expect(
      _androidMenu.keys.toSet().intersection(_androidPanelKeys.keys.toSet()),
      isEmpty,
    );
    expect(_windowsBar.keys.toSet().containsAll(_androidMenu.keys), isTrue);
    expect(_androidPanelCards.keys, _androidPanels.keys);
    for (final MapEntry(key: group, value: members) in _androidPanels.entries) {
      expect(_androidMenu.containsKey(group), isTrue, reason: group);
      expect(members, [
        for (final id in _windowsPanels[group]!)
          if (!windowsOnly.contains(id)) id,
      ], reason: group);
    }
    // 입력 방식 is Android's own row, outside the vocabulary.
    for (final (_, name) in androidPanelExtras) {
      expect(SessionTool.named(name), isNull, reason: name);
    }
    expect(
      {..._windowsBar.keys, ..._windowsPanelKeys.keys}.containsAll(windowsOnly),
      isTrue,
    );
  });
}

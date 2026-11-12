// Which session-bar commands the streaming view offers, and when
// (2026-10-26: show a command only while it applies), and the panels that
// drop down from the bar (2026-11-10, session bar v2: 화면 · 폴더·USB · 설정
// and the quality chip's 연결 panel; 화질, 바 열어 두기 and 사용안내
// are rows of 설정). Media, socket and the native window are fixtures; no
// real monitors or VM are involved.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_print.dart' show printCodeMessage;
import 'package:multitap_client/data/print_session.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_menu.dart';
import 'package:multitap_client/ui/drive_panel.dart' show driveMessage;
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_panels.dart';
import 'package:multitap_client/ui/session_stats_dialog.dart';
import 'package:multitap_client/ui/session_toolbar.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';

import 'clipboard_session_test.dart'
    show ClipboardSocket, MemoryClipboard, ready, sessionId;
import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;
import 'stream_display_entry_test.dart' show entryMachine;
import 'stream_display_mode_transition_test.dart' show layout;

const _bar = Key('session-command-bar');
const _panel = Key('stream-settings-panel');

/// The bar's 설정 button and its panel (2026-11-10).
const _settings = Key('remote-settings');
const _settingsPanel = Key('session-settings-panel');
const _overflow = Key('session-toolbar-overflow');

/// The tap target of a keyed row: a panel's row or a row of the narrow
/// bar's 메뉴. A SessionPanelRow hands its own key on to its InkWell, so in
/// a panel the key alone finds both widgets; this is the one InkWell.
Finder _row(String key) {
  final keyed = find.byKey(Key(key));
  final inside = find.descendant(of: keyed, matching: find.byType(InkWell));
  return inside.evaluate().isNotEmpty ? inside : keyed;
}

/// 설정 from the open bar: the panel under its button. In a bar too narrow
/// for its row the 메뉴 lists the same entries instead.
Future<void> _openSettings(WidgetTester tester) async {
  final narrow = find.byKey(_overflow).evaluate().isNotEmpty;
  await tester.tap(find.byKey(narrow ? _overflow : _settings));
  await tester.pumpAndSettle();
}

/// 화질 (key remote-stream-settings, the bar's former settings button):
/// a row of 설정 that opens the streaming choices in its place.
Future<void> _openQuality(WidgetTester tester) async {
  await _openSettings(tester);
  await tester.tap(_row('remote-stream-settings'));
  await tester.pumpAndSettle();
}

/// 바 열어 두기 (key pin-session-toolbar, the bar's former pin button): a
/// switch row of 설정 that applies in place, so Escape closes the panel.
Future<void> _pinBar(WidgetTester tester) async {
  await tester.tap(find.byKey(_settings));
  await tester.pumpAndSettle();
  await tester.tap(_row('pin-session-toolbar'));
  await tester.pump();
  await tester.sendKeyEvent(LogicalKeyboardKey.escape);
  await tester.pumpAndSettle();
  expect(find.byKey(_settingsPanel), findsNothing);
  expect(find.byKey(_bar), findsOneWidget);
  expect(_toolbar(tester).pinned, isTrue);
}

SessionToolbarState _toolbar(WidgetTester tester) =>
    tester.state<SessionToolbarState>(find.byType(SessionToolbar));

class _HeldCaptureMedia extends SessionMedia {
  Completer<bool>? release;
  @override
  Future<bool> capture(bool enabled) {
    captures.add(enabled);
    return !enabled && release != null
        ? release!.future
        : Future.value(enabled);
  }
}

class _View {
  _View({
    this.monitors = 1,
    this.fullscreen = false,
    this.width = 1440,
    this.driveUnavailable,
    this.printUnavailable,
    this.usbRequested = false,
    this.printRequested = false,
    SessionMedia? media,
  }) : media = media ?? SessionMedia();

  /// The connect asked for usb-v1 / print-v1 (the Host offers them) and the
  /// broker answered without their sockets: the assignment does not allow.
  final bool usbRequested, printRequested;

  /// The Host's reason it cannot map drives although they are allowed.
  final String? driveUnavailable;

  /// The Host's reason the VM cannot offer its PC printer.
  final String? printUnavailable;

  /// What the broker's inventory gives the view: the same machine every time
  /// (the entry flow reads it again), with or without the drive reason.
  DesktopMachine _machine(int count) {
    final base = entryMachine(count);
    if (driveUnavailable == null && printUnavailable == null) {
      return base;
    }
    return DesktopMachine(
      id: base.id,
      name: base.name,
      description: base.description,
      kind: base.kind,
      status: base.status,
      specification: base.specification,
      accent: base.accent,
      hostOnline: base.hostOnline,
      virtualMonitorControl: base.virtualMonitorControl,
      independentDisplayStreaming: base.independentDisplayStreaming,
      display: base.display,
      monitors: base.monitors,
      driveUnavailable: driveUnavailable,
      printUnavailable: printUnavailable,
    );
  }

  /// Local monitors Windows reports. The native side answers anything but
  /// two with an error, exactly as flutter_window.cpp does.
  final int monitors;
  final bool fullscreen;
  final double width;
  final socket = SessionSocket();
  final SessionMedia media;
  final calls = <String>[];
  final reconnects = <StreamOptions>[];
  var _connected = false;
  late final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: 'items-qa',
      webSocketUrl: Uri.parse('wss://fixture.invalid/api/v1/sessions/items-qa'),
      usbRequested: usbRequested,
      printRequested: printRequested,
    ),
    token: 'fixture-only-token',
    media: media,
    dialer: (_, _) async => socket,
  );

  Future<void> mount(
    WidgetTester tester, {
    bool sessionWindow = true,
    bool connect = true,
    double devicePixelRatio = 1,
  }) async {
    tester.view.devicePixelRatio = devicePixelRatio;
    tester.view.physicalSize = Size(
      width * devicePixelRatio,
      900 * devicePixelRatio,
    );
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    const window = MethodChannel('multitap/window'),
        events = MethodChannel('multitap/window/events');
    messenger.setMockMethodCallHandler(window, (call) async {
      calls.add(call.method);
      if (call.method == 'monitorLayout') {
        if (monitors != 2) {
          throw PlatformException(
            code: 'local_display_layout_unavailable',
            message: 'Exactly two local monitors are required.',
          );
        }
        return layout(2);
      }
      if (call.method == 'fullscreen') return fullscreen;
      return false;
    });
    messenger.setMockMethodCallHandler(events, (_) async => null);
    addTearDown(() {
      messenger.setMockMethodCallHandler(window, null);
      messenger.setMockMethodCallHandler(events, null);
    });
    await tester.pumpWidget(
      MaterialApp(
        navigatorObservers: [MenuDismissObserver()],
        home: RemoteView(
          machine: _machine(1),
          // Session windows derive the virtual-monitor count from the mode.
          prepareDisplaysOnEntry: sessionWindow,
          windowActions: sessionWindow
              ? SessionWindowActions(closeWindow: () async {})
              : null,
          initialDisplayMode: fullscreen
              ? SessionDisplayMode.fullscreen
              : SessionDisplayMode.windowed,
          load: () async => session,
          // The first connection, then recorded reconnects that fail.
          onReconnect: (options) async {
            if (!_connected) {
              _connected = true;
              return session;
            }
            reconnects.add(options);
            throw const BrokerException('network_error');
          },
          onConfigureDisplays: (count) async => _machine(count),
          onConnectTracks: (_, _) async =>
              throw const BrokerException('network_error'),
        ),
      ),
    );
    await tester.pump();
    await tester.pump();
    if (!connect) return;
    socket.incoming.add(jsonEncode(info));
    await tester.pump();
    await tester.pump();
    socket.incoming.add(packet());
    await tester.pump();
    await tester.pump();
    expect(session.frameReady, isTrue, reason: session.error);
  }

  Future<void> open(WidgetTester tester) async {
    if (find.byKey(_bar).evaluate().isEmpty) {
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await tester.pump();
    }
  }

  Future<void> close(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox.shrink());
    unawaited(session.close());
    await tester.pump(const Duration(seconds: 3));
    await tester.pump();
    unawaited(media.events.close());
  }
}

void main() {
  for (final dpr in [1.0, 2.0]) {
    testWidgets(
      'RemoteView first captured menu press opens before native release at DPR $dpr',
      (tester) async {
        final media = _HeldCaptureMedia();
        final view = _View(media: media);
        await view.mount(tester, devicePixelRatio: dpr);
        try {
          await view.open(tester);
          await _pinBar(tester);
          await tester.tapAt(
            const Offset(720, 450),
            kind: PointerDeviceKind.mouse,
          );
          await tester.pump(Duration.zero);
          expect(view.session.captured, isTrue);
          media.release = Completer<bool>();
          final before = view.socket.sent
              .where((event) => event['type'] == 'input')
              .length;
          // No hover/release/pump preparation: this first down is hit-tested
          // against the REAL bar that was built while capture was still true.
          await tester.tapAt(
            tester.getCenter(find.byKey(const Key('stream-menu-화면'))),
            kind: PointerDeviceKind.mouse,
          );
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 100));
          expect(view.session.captured, isFalse);
          expect(media.release!.isCompleted, isFalse);
          expect(
            find.byKey(const Key('stream-windowed')),
            findsOneWidget,
            reason: 'The first local menu press must not be absorbed by stale captured state.',
          );
          expect(find.byKey(const Key('stream-fullscreen')), findsOneWidget);
          // Ordinary session notifications must not invalidate an otherwise
          // unchanged SessionPopups generation while the native wait is held.
          view.session.notifyListeners();
          await tester.pump();
          expect(find.byKey(const Key('stream-windowed')), findsOneWidget);
          expect(
            view.socket.sent.where((event) => event['type'] == 'input').length,
            before,
            reason: 'A local toolbar press must not reach the remote desktop.',
          );
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.inactive,
          );
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 100));
          expect(find.byKey(const Key('stream-windowed')), findsNothing);
          expect(
            find.byKey(const Key('session-command-bar')),
            findsOneWidget,
            reason: 'A pinned bar remains, while its old popup retires on focus loss.',
          );
        } finally {
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.resumed,
          );
          final release = media.release;
          if (release != null && !release.isCompleted) release.complete(false);
          media.release = null;
          await tester.pump();
          await view.close(tester);
        }
      },
    );
  }

  testWidgets('RemoteView held remote press never becomes a toolbar command', (
    tester,
  ) async {
    final view = _View();
    await view.mount(tester);
    TestGesture? drag;
    try {
      await view.open(tester);
      await _pinBar(tester);
      await tester.tapAt(const Offset(720, 450), kind: PointerDeviceKind.mouse);
      await tester.pump(Duration.zero);
      expect(view.session.captured, isTrue);
      drag = await tester.createGesture(kind: PointerDeviceKind.mouse);
      await drag.down(const Offset(700, 430));
      await tester.pump();
      final target = tester.getCenter(find.byKey(const Key('stream-menu-화면')));
      await drag.moveTo(target);
      await tester.pump();
      expect(
        view.session.captured,
        isTrue,
        reason: 'The press that began in the picture still owns its drag.',
      );
      expect(find.byKey(const Key('stream-windowed')), findsNothing);
      await drag.up();
      await tester.pump();
      final remoteButtons = view.socket.sent
          .where(
            (event) => event['type'] == 'input' && event['event'] == 'button',
          )
          .toList();
      expect(
        remoteButtons.last['down'],
        isFalse,
        reason: 'Its real release must reach the remote, including over the toolbar.',
      );
      final inputs = view.socket.sent
          .where((event) => event['type'] == 'input')
          .length;
      await tester.tapAt(target, kind: PointerDeviceKind.mouse);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));
      expect(find.byKey(const Key('stream-windowed')), findsOneWidget);
      expect(
        view.socket.sent.where((event) => event['type'] == 'input').length,
        inputs,
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
    } finally {
      await drag?.removePointer();
      await view.close(tester);
    }
  });

  testWidgets(
    'One local monitor: a 화면 panel with window and full screen only; no fullscreen toggle, count, statistics or clipboard command',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        await view.open(tester);
        expect(find.byKey(const Key('session-fullscreen')), findsNothing);
        expect(find.byKey(const Key('stream-menu-정보')), findsNothing);
        expect(find.byKey(const Key('stream-menu-클립보드')), findsNothing);
        expect(find.byTooltip('화면 모드 · 창 모드'), findsOneWidget);
        // 배율 is part of the 화면 panel, not a bar button of its own.
        expect(find.byKey(const Key('remote-display-scale')), findsNothing);
        final button = tester.getRect(find.byKey(const Key('stream-menu-화면')));
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pumpAndSettle();
        const screen = Key('session-screen-panel');
        expect(find.byKey(screen), findsOneWidget);
        expect(find.byKey(const Key('stream-windowed')), findsOneWidget);
        expect(find.byKey(const Key('stream-fullscreen')), findsOneWidget);
        expect(find.byKey(const Key('stream-all-monitors')), findsNothing);
        expect(find.byKey(const Key('remote-virtual-displays')), findsNothing);
        expect(find.byKey(const Key('remote-display-scale')), findsOneWidget);
        // It hangs under its button, as wide as two modes need, and the bar
        // stays open above it.
        final panel = tester.getRect(find.byKey(const Key('anchored-panel')));
        expect(panel.top, greaterThanOrEqualTo(button.bottom));
        expect(panel.top - button.bottom, lessThan(24));
        expect(panel.width, SessionScreenPanel.widthFor(2));
        expect(find.byKey(_bar), findsOneWidget);
        // The window mode in use is the marked one: choosing it only closes
        // the panel, and the bar stays for another command.
        expect(
          find.descendant(
            of: find.byKey(screen),
            matching: find.byIcon(Icons.check_rounded),
          ),
          findsOneWidget,
        );
        expect(
          find.descendant(
            of: find.byKey(const Key('stream-windowed')),
            matching: find.byIcon(Icons.check_rounded),
          ),
          findsOneWidget,
        );
        await tester.tap(find.byKey(const Key('stream-windowed')));
        await tester.pumpAndSettle();
        expect(find.byKey(screen), findsNothing);
        expect(find.byKey(_bar), findsOneWidget);
        expect(find.byTooltip('화면 모드 · 창 모드'), findsOneWidget);
        // Escape closes it as well.
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pumpAndSettle();
        expect(find.byKey(screen), findsOneWidget);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(find.byKey(screen), findsNothing);
        expect(find.byKey(_bar), findsOneWidget);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'Two local monitors: the panel adds 모든 모니터 사용, still no count in a session window',
    (tester) async {
      final view = _View(monitors: 2);
      await view.mount(tester);
      try {
        await view.open(tester);
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pumpAndSettle();
        final lefts = <double>[];
        for (final key in [
          'stream-windowed',
          'stream-fullscreen',
          'stream-all-monitors',
        ]) {
          expect(find.byKey(Key(key)), findsOneWidget, reason: key);
          lefts.add(tester.getRect(find.byKey(Key(key))).left);
        }
        // Three tiles side by side, in a wider panel.
        expect(lefts[0], lessThan(lefts[1]));
        expect(lefts[1], lessThan(lefts[2]));
        expect(
          tester.getSize(find.byKey(const Key('anchored-panel'))).width,
          SessionScreenPanel.widthFor(3),
        );
        expect(view.calls.where((call) => call == 'monitorLayout'), isNotEmpty);
        expect(find.byKey(const Key('remote-virtual-displays')), findsNothing);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets('최소화 and 창 모드로 only in full screen (a window has its title '
      'bar)', (tester) async {
    for (final fullscreen in [false, true]) {
      final view = _View(fullscreen: fullscreen);
      await view.mount(tester);
      try {
        await view.open(tester);
        expect(
          find.byKey(const Key('session-minimize')),
          fullscreen ? findsOneWidget : findsNothing,
        );
        expect(
          find.byKey(const Key('session-restore')),
          fullscreen ? findsOneWidget : findsNothing,
        );
        expect(find.byKey(const Key('session-fullscreen')), findsNothing);
        // The 화면 button shows the current mode.
        expect(
          find.byTooltip(fullscreen ? '화면 모드 · 전체 화면' : '화면 모드 · 창 모드'),
          findsOneWidget,
        );
        // Left to right: the chip, the groups, the window's own buttons as
        // on a title bar (— then ❐), and 연결 끊기 last.
        final lefts = [
          for (final key in [
            'session-quality',
            'stream-menu-화면',
            'remote-settings',
            if (fullscreen) ...['session-minimize', 'session-restore'],
            'session-disconnect',
          ])
            tester.getRect(find.byKey(Key(key))).left,
        ];
        for (var n = 1; n < lefts.length; n++) {
          expect(lefts[n - 1], lessThan(lefts[n]), reason: '$fullscreen $n');
        }
        if (fullscreen) {
          expect(find.byTooltip('최소화'), findsOneWidget);
          expect(find.byTooltip('창 모드로'), findsOneWidget);
        }
        expect(find.byTooltip('연결 끊기'), findsOneWidget);
      } finally {
        await view.close(tester);
      }
    }
  });

  testWidgets(
    'Settings stay visible but say why they wait before the picture arrives',
    (tester) async {
      final view = _View();
      await view.mount(tester, connect: false);
      try {
        await view.open(tester);
        // 설정 itself opens; its rows that need a picture are greyed with
        // the reason. (The waiting view animates: no settling here.)
        expect(find.byTooltip('설정 · 화질, 사용안내 등'), findsOneWidget);
        await tester.tap(find.byKey(_settings));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        expect(find.byKey(_settingsPanel), findsOneWidget);
        expect(find.byTooltip('화질 · 연결된 뒤 바꿀 수 있어요'), findsOneWidget);
        expect(
          tester.widget<InkWell>(_row('remote-stream-settings')).onTap,
          isNull,
        );
        expect(find.byTooltip('사용안내 · 연결된 뒤 볼 수 있어요'), findsOneWidget);
        expect(
          tester.widget<InkWell>(_row('remote-session-guide')).onTap,
          isNull,
        );
        // 바 열어 두기 needs no picture.
        expect(
          tester.widget<InkWell>(_row('pin-session-toolbar')).onTap,
          isNotNull,
        );
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    '설정 holds 화질, 바 열어 두기 and 사용안내; the switch applies in place and the panel stays',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        await view.open(tester);
        expect(find.byTooltip('설정 · 화질, 사용안내 등'), findsOneWidget);
        // None of them is a button of the bar any more.
        for (final key in [
          'remote-stream-settings',
          'pin-session-toolbar',
          'remote-session-guide',
          'session-game-mouse',
        ]) {
          expect(find.byKey(Key(key)), findsNothing, reason: key);
        }
        await _openSettings(tester);
        expect(find.byKey(_settingsPanel), findsOneWidget);
        expect(
          tester.getSize(find.byKey(const Key('anchored-panel'))).width,
          SessionSettingsPanel.width,
        );
        // Top to bottom, in the vocabulary's order.
        var last = double.negativeInfinity;
        for (final key in [
          'remote-stream-settings',
          'pin-session-toolbar',
          'remote-session-guide',
        ]) {
          expect(_row(key), findsOneWidget, reason: key);
          final top = tester.getRect(_row(key)).top;
          expect(top, greaterThan(last), reason: key);
          last = top;
        }
        // This connection was not granted relative-mouse-v1: no 마우스 잠금.
        expect(find.byKey(const Key('session-game-mouse')), findsNothing);
        // 화질 says what the connection uses now.
        expect(
          find.descendant(
            of: find.byKey(const Key('remote-stream-settings')),
            matching: find.textContaining('60 fps'),
          ),
          findsOneWidget,
        );
        expect(_toolbar(tester).pinned, isFalse);
        await tester.tap(_row('pin-session-toolbar'));
        await tester.pump();
        expect(_toolbar(tester).pinned, isTrue);
        expect(
          find.byKey(_settingsPanel),
          findsOneWidget,
          reason: 'The switch applies in place.',
        );
        await tester.tap(_row('pin-session-toolbar'));
        await tester.pump();
        expect(_toolbar(tester).pinned, isFalse);
        expect(find.byKey(_settingsPanel), findsOneWidget);
        await tester.tap(_row('pin-session-toolbar'));
        await tester.pump();
        expect(_toolbar(tester).pinned, isTrue);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(find.byKey(_settingsPanel), findsNothing);
        // Kept open: a click back into the picture no longer folds it.
        await tester.tapAt(
          const Offset(1200, 700),
          kind: PointerDeviceKind.mouse,
        );
        await tester.pump();
        expect(find.byKey(_bar), findsOneWidget);
        // Opened again, the switch shows what the bar has.
        await tester.tap(find.byKey(_settings));
        await tester.pumpAndSettle();
        expect(
          tester
              .widget<Switch>(
                find.descendant(
                  of: find.byKey(const Key('pin-session-toolbar')),
                  matching: find.byType(Switch),
                ),
              )
              .value,
          isTrue,
        );
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'Settings drop down under their button, keep the bar open and apply only a change',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        await view.open(tester);
        final button = tester.getRect(find.byKey(_settings));
        await tester.tap(find.byKey(_settings));
        await tester.pumpAndSettle();
        // 설정 hangs under its button; the bar stays open, its button lit.
        expect(find.byKey(_settingsPanel), findsOneWidget);
        var panel = tester.getRect(find.byKey(const Key('anchored-panel')));
        expect(panel.top, greaterThanOrEqualTo(button.bottom));
        expect(panel.top - button.bottom, lessThan(24));
        expect((panel.center.dx - button.center.dx).abs(), lessThan(1));
        expect(panel.width, SessionSettingsPanel.width);
        expect(find.byKey(_bar), findsOneWidget);
        // 화질 opens the streaming choices in its place, under the same
        // button.
        await tester.tap(_row('remote-stream-settings'));
        await tester.pumpAndSettle();
        expect(find.byKey(_settingsPanel), findsNothing);
        expect(find.byKey(_panel), findsOneWidget);
        expect(find.byType(StreamSettingsDialog), findsNothing);
        expect(
          find.descendant(of: find.byKey(_panel), matching: find.text('화질')),
          findsOneWidget,
        );
        panel = tester.getRect(find.byKey(const Key('anchored-panel')));
        expect(panel.top, greaterThanOrEqualTo(button.bottom));
        expect(panel.top - button.bottom, lessThan(24));
        expect((panel.center.dx - button.center.dx).abs(), lessThan(1));
        expect(panel.width, streamSettingsPanelWidth);
        // The bar stays open under its panel, its button lit.
        expect(find.byKey(_bar), findsOneWidget);
        // Sound is always on (2026-11-11): no 소리 switch at any level; the
        // detail controls remain reachable through an explicit disclosure.
        expect(find.byKey(const Key('stream-current-summary')), findsOneWidget);
        expect(find.byKey(const Key('stream-audio')), findsNothing);
        expect(find.byKey(const Key('stream-audio-label')), findsNothing);
        expect(find.byKey(const Key('stream-fps-label')), findsNothing);
        await tester.tap(find.byKey(const Key('stream-advanced')));
        await tester.pumpAndSettle();
        for (final key in [
          'stream-fps-label',
          'stream-bitrate-label',
          'stream-hardware-label',
          'stream-dynamic-resolution',
        ]) {
          expect(find.byKey(Key(key)), findsOneWidget, reason: key);
        }
        expect(find.byKey(const Key('stream-audio')), findsNothing);
        expect(find.textContaining('가상 화면'), findsNothing);
        // The VM's sound device was not reported missing: no long hint.
        expect(find.byKey(const Key('stream-audio-hint')), findsNothing);
        FilledButton apply() => tester.widget<FilledButton>(
          find.byKey(const Key('apply-stream-settings')),
        );
        expect(apply().onPressed, isNull, reason: 'Nothing changed yet.');
        await tester.tap(find.text('30 fps'));
        await tester.pump();
        expect(apply().onPressed, isNotNull);
        await tester.tap(find.byKey(const Key('apply-stream-settings')));
        await tester.pumpAndSettle();
        expect(find.byKey(_panel), findsNothing);
        expect(find.byKey(_settingsPanel), findsNothing);
        expect(view.reconnects.single.fps, 30);
        // Applied: the bar folds again.
        expect(find.byKey(_bar), findsNothing);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    '화질 has a back button that returns to 설정 without applying anything',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        await view.open(tester);
        await _openQuality(tester);
        expect(find.byKey(_panel), findsOneWidget);
        expect(find.byTooltip('설정으로 돌아가기'), findsOneWidget);
        // A change that is not applied.
        await tester.tap(find.byKey(const Key('stream-preset-saver')));
        await tester.pump();
        await tester.tap(find.byKey(const Key('stream-settings-back')));
        await tester.pumpAndSettle();
        expect(find.byKey(_panel), findsNothing);
        expect(find.byKey(_settingsPanel), findsOneWidget);
        expect(find.byKey(_bar), findsOneWidget);
        expect(view.reconnects, isEmpty);
        // And into it again from there, as before.
        await tester.tap(_row('remote-stream-settings'));
        await tester.pumpAndSettle();
        expect(find.byKey(_panel), findsOneWidget);
        expect(find.byKey(_settingsPanel), findsNothing);
        // 취소 closes the streaming choices without going back.
        await tester.tap(find.byKey(const Key('cancel-stream-settings')));
        await tester.pumpAndSettle();
        expect(find.byKey(_panel), findsNothing);
        expect(find.byKey(_settingsPanel), findsNothing);
        expect(find.byKey(_bar), findsOneWidget);
        expect(view.reconnects, isEmpty);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'Quality modes on top of the panel: the session reads 작업; 절약 reconnects at 30 fps and 8 Mbps',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        await view.open(tester);
        await _openQuality(tester);
        final row = tester.getRect(find.byKey(const Key('stream-preset')));
        // Above 고급 설정 and what it holds.
        expect(
          row.top,
          lessThan(tester.getRect(find.byKey(const Key('stream-advanced'))).top),
        );
        await tester.tap(find.byKey(const Key('stream-advanced')));
        await tester.pumpAndSettle();
        expect(
          row.top,
          lessThan(
            tester.getRect(find.byKey(const Key('stream-fps-label'))).top,
          ),
        );
        // The connection's own request (a released Host's fixed 20 Mbps).
        expect(
          tester
              .widget<Text>(find.byKey(const Key('stream-preset-label')))
              .textSpan!
              .toPlainText(),
          '화질 모드  작업',
        );
        FilledButton apply() => tester.widget<FilledButton>(
          find.byKey(const Key('apply-stream-settings')),
        );
        expect(apply().onPressed, isNull);
        await tester.tap(find.byKey(const Key('stream-preset-saver')));
        await tester.pump();
        expect(apply().onPressed, isNotNull);
        await tester.tap(find.byKey(const Key('apply-stream-settings')));
        await tester.pumpAndSettle();
        final sent = view.reconnects.single;
        expect(sent.fps, 30);
        expect(sent.bitrate, 8000000);
        expect(sent.automaticBitrate, isFalse);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'No sound device: 화질 says one plain line, only when the Host reported none, and offers no action',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        view.session.remoteAudioMissing = true;
        await view.open(tester);
        await _openQuality(tester);
        final hint = find.byKey(const Key('stream-audio-hint'));
        expect(hint, findsOneWidget);
        expect(
          tester.widget<Text>(hint).data,
          '이 데스크톱에는 소리 장치가 없어요. 관리자가 Proxmox에서 오디오 장치를 추가해야 해요.',
        );
        expect(tester.widget<Text>(hint).data, audioDeviceMissingText);
        // Under the current choices, above 고급 설정.
        expect(
          tester.getRect(hint).top,
          greaterThan(
            tester.getRect(find.byKey(const Key('stream-current-summary'))).top,
          ),
        );
        expect(
          tester.getRect(hint).bottom,
          lessThanOrEqualTo(
            tester.getRect(find.byKey(const Key('stream-advanced'))).top,
          ),
        );
        // Multitap adds no sound device (2026-11-10): nothing to press.
        expect(find.byKey(const Key('stream-add-sound-card')), findsNothing);
        expect(find.textContaining('사운드카드'), findsNothing);
        expect(
          find.descendant(
            of: find.byKey(_panel),
            matching: find.textContaining('추가…'),
          ),
          findsNothing,
        );
        // Sound cannot be turned off (2026-11-11): the line stays.
        expect(find.byKey(const Key('stream-audio')), findsNothing);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(view.reconnects, isEmpty);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'No sound device: the notice over the picture is the same plain line, with nothing to press',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        view.session.remoteAudioMissing = true;
        view.session.notifyListeners();
        await tester.pump();
        await tester.pump();
        final toast = find.byKey(const Key('pointer-confinement-toast'));
        expect(toast, findsOneWidget);
        expect(
          find.descendant(
            of: toast,
            matching: find.text(audioDeviceMissingText),
          ),
          findsOneWidget,
        );
        expect(find.byKey(const Key('session-toast-action')), findsNothing);
        expect(
          find.descendant(of: toast, matching: find.byType(TextButton)),
          findsNothing,
        );
        expect(find.textContaining('사운드카드'), findsNothing);
        // Said once, and it goes away by itself.
        await tester.pump(const Duration(seconds: 11));
        expect(toast, findsNothing);
        view.session.notifyListeners();
        await tester.pump();
        await tester.pump();
        expect(toast, findsNothing);
        expect(tester.takeException(), isNull);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'The panel closes on Escape, an outside click and a dialog opening over it',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        Future<void> openPanel() async {
          await view.open(tester);
          await _openQuality(tester);
          expect(find.byKey(_panel), findsOneWidget);
        }

        await openPanel();
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(find.byKey(_panel), findsNothing);
        // Closed, not gone back: 설정 does not come up again.
        expect(find.byKey(_settingsPanel), findsNothing);
        // Cancelled, the bar is still open for another command.
        expect(find.byKey(_bar), findsOneWidget);
        await openPanel();
        await tester.tapAt(const Offset(1300, 820));
        await tester.pumpAndSettle();
        expect(find.byKey(_panel), findsNothing);
        expect(find.byKey(_settingsPanel), findsNothing);
        expect(view.reconnects, isEmpty);
        await openPanel();
        unawaited(
          showDialog<void>(
            context: tester.element(find.byType(RemoteView)),
            builder: (_) => const AlertDialog(content: Text('다른 창')),
          ),
        );
        await tester.pumpAndSettle();
        expect(find.byKey(_panel), findsNothing);
        expect(find.text('다른 창'), findsOneWidget);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'The 설정 panel itself closes on Escape, an outside click and a dialog opening over it',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        Future<void> openPanel() async {
          await view.open(tester);
          await _openSettings(tester);
          expect(find.byKey(_settingsPanel), findsOneWidget);
        }

        await openPanel();
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(find.byKey(_settingsPanel), findsNothing);
        expect(find.byKey(_bar), findsOneWidget);
        await openPanel();
        await tester.tapAt(const Offset(1300, 820));
        await tester.pumpAndSettle();
        expect(find.byKey(_settingsPanel), findsNothing);
        await openPanel();
        unawaited(
          showDialog<void>(
            context: tester.element(find.byType(RemoteView)),
            builder: (_) => const AlertDialog(content: Text('다른 창')),
          ),
        );
        await tester.pumpAndSettle();
        expect(find.byKey(_settingsPanel), findsNothing);
        expect(find.text('다른 창'), findsOneWidget);
        expect(view.reconnects, isEmpty);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'With a panel open, one click on the picture only closes it; one click on another bar button opens its panel',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        await view.open(tester);
        // The quality chip's 연결 panel.
        await tester.tap(find.byKey(const Key('session-quality')));
        await tester.pumpAndSettle();
        expect(
          find.byKey(const Key('session-connection-panel')),
          findsOneWidget,
        );
        expect(find.text('왕복 지연 · Host 응답'), findsOneWidget);
        view.socket.sent.clear();
        await tester.tapAt(const Offset(1200, 700));
        await tester.pumpAndSettle();
        expect(find.text('왕복 지연 · Host 응답'), findsNothing);
        // That click closed the panel and reached nothing else.
        expect(
          view.socket.sent.where(
            (event) => event['type'] == 'input' && event['event'] == 'button',
          ),
          isEmpty,
        );
        expect(find.byKey(_panel), findsNothing);
        expect(find.byKey(_settingsPanel), findsNothing);
        await view.open(tester);
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('stream-windowed')), findsOneWidget);
        // From 화면 straight to 설정.
        await tester.tap(find.byKey(_settings), warnIfMissed: false);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('stream-windowed')), findsNothing);
        expect(find.byKey(_settingsPanel), findsOneWidget);
        // From 설정 straight back to 화면.
        await tester.tap(
          find.byKey(const Key('stream-menu-화면')),
          warnIfMissed: false,
        );
        await tester.pumpAndSettle();
        expect(find.byKey(_settingsPanel), findsNothing);
        expect(find.byKey(const Key('stream-windowed')), findsOneWidget);
        // From 화면 to the chip, and from the chip to 설정.
        await tester.tap(
          find.byKey(const Key('session-quality')),
          warnIfMissed: false,
        );
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('stream-windowed')), findsNothing);
        expect(
          find.byKey(const Key('session-connection-panel')),
          findsOneWidget,
        );
        await tester.tap(find.byKey(_settings), warnIfMissed: false);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('session-connection-panel')), findsNothing);
        expect(find.byKey(_settingsPanel), findsOneWidget);
        // 화질 opened from 설정 is still the 설정 button's panel: one
        // click on 화면 leaves it.
        await tester.tap(_row('remote-stream-settings'));
        await tester.pumpAndSettle();
        expect(find.byKey(_panel), findsOneWidget);
        await tester.tap(
          find.byKey(const Key('stream-menu-화면')),
          warnIfMissed: false,
        );
        await tester.pumpAndSettle();
        expect(find.byKey(_panel), findsNothing);
        expect(find.byKey(_settingsPanel), findsNothing);
        expect(find.byKey(const Key('stream-windowed')), findsOneWidget);
        expect(view.reconnects, isEmpty);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(find.byKey(_bar), findsOneWidget);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'A window too narrow for the panel opens the settings as a dialog',
    (tester) async {
      final view = _View(width: 360);
      await view.mount(tester);
      try {
        await view.open(tester);
        // From the 설정 panel, or from the 메뉴 where the row does not fit.
        await _openQuality(tester);
        expect(find.byKey(_panel), findsNothing);
        expect(find.byType(StreamSettingsDialog), findsOneWidget);
        // In the session the dialog is 화질 as well.
        expect(
          find.descendant(
            of: find.byType(StreamSettingsDialog),
            matching: find.text('화질'),
          ),
          findsWidgets,
        );
        expect(find.text('스트리밍 설정'), findsNothing);
        expect(tester.takeException(), isNull);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'A bar too narrow for its row: one 메뉴 lists every group\'s entries under its caption, with 바 열어 두기',
    (tester) async {
      // Shared folders and USB offered but not usable here: the group is
      // listed with both entries greyed.
      final view = _View(
        width: 360,
        driveUnavailable: 'drive_webclient_unavailable',
        usbRequested: true,
      );
      await view.mount(tester);
      try {
        await view.open(tester);
        expect(find.byKey(_overflow), findsOneWidget);
        expect(find.byTooltip('메뉴 · 화면, 설정 등'), findsOneWidget);
        expect(
          find.descendant(of: find.byKey(_overflow), matching: find.text('메뉴')),
          findsOneWidget,
        );
        for (final key in [
          'stream-menu-화면',
          'remote-share',
          'remote-settings',
        ]) {
          expect(find.byKey(Key(key)), findsNothing, reason: key);
        }
        await tester.tap(find.byKey(_overflow));
        await tester.pumpAndSettle();
        // 화면, then 폴더·USB's two entries, then 설정's, then the bar's own.
        var last = double.negativeInfinity;
        for (final key in [
          'stream-menu-화면',
          'remote-drives',
          'remote-usb',
          'remote-stream-settings',
          'remote-session-guide',
          'pin-session-toolbar',
        ]) {
          expect(find.byKey(Key(key)), findsOneWidget, reason: key);
          final top = tester.getRect(find.byKey(Key(key))).top;
          expect(top, greaterThan(last), reason: key);
          last = top;
        }
        for (final (key, label) in const [
          ('stream-menu-화면', '화면'),
          ('remote-drives', '공유 폴더'),
          ('remote-usb', 'USB 장치'),
          ('remote-stream-settings', '화질'),
          ('remote-session-guide', '사용안내'),
          ('pin-session-toolbar', '툴바 열어 두기'),
        ]) {
          expect(
            find.descendant(
              of: find.byKey(Key(key)),
              matching: find.text(label),
            ),
            findsOneWidget,
            reason: key,
          );
        }
        // The groups' captions; 바 열어 두기 joins 설정, so no caption of
        // its own.
        expect(find.text('폴더·USB'), findsOneWidget);
        expect(find.text('설정'), findsOneWidget);
        expect(find.text('툴바'), findsNothing);
        final caption = tester.getRect(find.text('설정')).top;
        expect(
          tester.getRect(find.byKey(const Key('remote-usb'))).top,
          lessThan(caption),
        );
        expect(
          caption,
          lessThan(
            tester.getRect(find.byKey(const Key('remote-stream-settings'))).top,
          ),
        );
        // Neither side can be used here: both greyed, each with its reason.
        for (final key in ['remote-drives', 'remote-usb']) {
          expect(
            tester.widget<PopupMenuItem<int>>(find.byKey(Key(key))).enabled,
            isFalse,
            reason: key,
          );
        }
        expect(
          find.byTooltip(
            '공유 폴더\n${driveMessage('drive_webclient_unavailable')}',
          ),
          findsOneWidget,
        );
        expect(
          find.byTooltip('USB 장치 · $adminPermissionNeeded'),
          findsOneWidget,
        );
        // 바 열어 두기 switches in the 메뉴 and keeps the bar.
        expect(_toolbar(tester).pinned, isFalse);
        await tester.tap(find.byKey(const Key('pin-session-toolbar')));
        await tester.pumpAndSettle();
        expect(_toolbar(tester).pinned, isTrue);
        expect(find.byKey(_bar), findsOneWidget);
        await tester.tap(find.byKey(_overflow));
        await tester.pumpAndSettle();
        expect(
          find.descendant(
            of: find.byKey(const Key('pin-session-toolbar')),
            matching: find.byIcon(Icons.check_rounded),
          ),
          findsOneWidget,
        );
        // 화면 from the 메뉴 opens the same panel, under the whole bar.
        final bar = tester.getRect(find.byKey(_bar));
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('session-screen-panel')), findsOneWidget);
        expect(find.byKey(const Key('stream-windowed')), findsOneWidget);
        final panel = tester.getRect(find.byKey(const Key('anchored-panel')));
        expect(panel.top, greaterThanOrEqualTo(bar.bottom));
        expect(find.byKey(_bar), findsOneWidget);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(tester.takeException(), isNull);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'Clipboard has no bar command; a long transfer offers 취소 and stopped sharing says so once',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      final socket = ClipboardSocket();
      final bridge = MemoryClipboard()..captureGate = Completer<void>();
      final clipboard = ClipboardSession(
        url: Uri.parse(
          'wss://fixture.invalid/api/v1/sessions/$sessionId/clipboard',
        ),
        token: 'fixture-only',
        sessionId: sessionId,
        policy: const ClipboardPolicy(textToRemote: true),
        bridge: bridge,
        dialer: (_, _) async => socket,
      );
      try {
        view.session.clipboard = clipboard;
        clipboard.addListener(view.session.notifyListeners);
        unawaited(clipboard.start().catchError((Object _) {}));
        await tester.pump();
        socket.emit(ready);
        for (var i = 0; i < 6 && !clipboard.ready; i++) {
          await tester.pump();
        }
        expect(clipboard.ready, isTrue);
        await view.open(tester);
        expect(find.byKey(const Key('stream-menu-클립보드')), findsNothing);
        // A transfer held at the local clipboard read: after a moment it
        // shows, with a cancel action.
        unawaited(clipboard.sendLocal().catchError((Object _) {}));
        await tester.pump();
        await tester.pump();
        expect(clipboard.busy, isTrue);
        expect(find.byKey(const Key('clipboard-transfer')), findsNothing);
        await tester.pump(const Duration(milliseconds: 1300));
        await tester.pump(const Duration(milliseconds: 300));
        expect(find.byKey(const Key('clipboard-transfer')), findsOneWidget);
        expect(find.text('취소'), findsOneWidget);
        // The channel drops: the transfer ends and one notice says why.
        await socket.incoming.close();
        bridge.captureGate!.complete();
        for (var i = 0; i < 8; i++) {
          await tester.pump(const Duration(milliseconds: 50));
        }
        expect(clipboard.closed, isTrue);
        expect(find.textContaining('클립보드 공유가 멈췄어요'), findsOneWidget);
        await tester.pump(const Duration(seconds: 7));
        await tester.pumpAndSettle();
        expect(find.textContaining('클립보드 공유가 멈췄어요'), findsNothing);
      } finally {
        clipboard.removeListener(view.session.notifyListeners);
        unawaited(clipboard.close());
        await tester.pump(const Duration(seconds: 3));
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'Without a drive reason there is no 폴더·USB button (not granted, not allowed)',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        await view.open(tester);
        expect(find.byKey(_settings), findsOneWidget);
        expect(find.byKey(const Key('remote-share')), findsNothing);
        expect(find.byKey(const Key('remote-drives')), findsNothing);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'Drives allowed but the VM cannot map them: a disabled 공유 폴더 button says why',
    (tester) async {
      final view = _View(driveUnavailable: 'drive_webclient_unavailable');
      await view.mount(tester);
      try {
        await view.open(tester);
        // Only this side is offered: the button is named for it.
        const share = Key('remote-share');
        expect(find.byKey(share), findsOneWidget);
        final command = tester.widget<SessionToolbarMenu>(find.byKey(share));
        expect(command.label, '공유 폴더');
        expect(command.enabled, isFalse);
        expect(command.active, isFalse);
        expect(
          tester
              .widget<TextButton>(
                find.descendant(
                  of: find.byKey(share),
                  matching: find.byType(TextButton),
                ),
              )
              .onPressed,
          isNull,
        );
        final reason = driveMessage('drive_webclient_unavailable');
        expect(find.byTooltip('공유 폴더\n$reason'), findsOneWidget);
        // Nothing to open: no tabs anywhere.
        expect(find.byKey(const Key('remote-drives')), findsNothing);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'USB the Host offers but the administrator did not allow: shown disabled with the reason, on hover and on a tap; printing has no toolbar item',
    (tester) async {
      final view = _View(usbRequested: true, printRequested: true);
      await view.mount(tester);
      try {
        await view.open(tester);
        const share = Key('remote-share');
        expect(
          tester.widget<SessionToolbarMenu>(find.byKey(share)).label,
          'USB 장치',
        );
        final button = find.descendant(
          of: find.byKey(share),
          matching: find.byType(TextButton),
        );
        expect(tester.widget<TextButton>(button).onPressed, isNull);
        expect(find.byTooltip('USB 장치 · 관리자 허용이 필요해요'), findsOneWidget);
        // There is no printer button: the VM prints to 「Multitap 프린터 (PC)」
        // by itself.
        expect(find.byKey(const Key('remote-printer')), findsNothing);
        // By finger there is no hover: a tap says it.
        await tester.tap(find.byKey(share));
        await tester.pump(const Duration(milliseconds: 100));
        expect(find.text('USB 장치 · 관리자 허용이 필요해요'), findsOneWidget);
        expect(find.byKey(const Key('session-share-panel')), findsNothing);
        await tester.pump(const Duration(seconds: 3));
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'Shared folders and USB both offered and neither usable: one disabled 폴더·USB button',
    (tester) async {
      final view = _View(
        driveUnavailable: 'drive_webclient_unavailable',
        usbRequested: true,
      );
      await view.mount(tester);
      try {
        await view.open(tester);
        const share = Key('remote-share');
        final command = tester.widget<SessionToolbarMenu>(find.byKey(share));
        expect(command.label, '폴더·USB');
        expect(command.enabled, isFalse);
        expect(command.badge, isNull);
        // Greyed, it says a side's name and why.
        final reason = driveMessage('drive_webclient_unavailable');
        expect(command.hint, '공유 폴더\n$reason');
        expect(find.byTooltip('공유 폴더\n$reason'), findsOneWidget);
        // Between 화면 and 설정.
        final left = tester.getRect(find.byKey(share)).left;
        expect(
          tester.getRect(find.byKey(const Key('stream-menu-화면'))).left,
          lessThan(left),
        );
        expect(left, lessThan(tester.getRect(find.byKey(_settings)).left));
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'Not requested (a Host without USB or print) shows neither control',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      try {
        await view.open(tester);
        expect(find.byKey(const Key('remote-share')), findsNothing);
        expect(find.byKey(const Key('remote-usb')), findsNothing);
        expect(find.byKey(const Key('remote-printer')), findsNothing);
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets(
    'A document from the VM printer flashes the taskbar, names the documents waiting behind it, and survives the channel ending under the open dialog',
    (tester) async {
      final view = _View();
      await view.mount(tester);
      final folder = Directory.systemTemp.createTempSync('multitap-offer-');
      final printer = PrintSession(
        url: Uri.parse('wss://fixture.invalid/api/v1/sessions/items-qa/print'),
        token: 'fixture-only',
        sessionId: 'items-qa',
        storage: folder,
        dialer: (_, _) async => throw const SocketException('offline'),
      );
      PrintDocument document(String id) {
        final file = File('${folder.path}${Platform.pathSeparator}$id.pdf')
          ..writeAsBytesSync([0x25, 0x50, 0x44, 0x46, 0x2d]);
        return PrintDocument(jobId: id, title: '문서 $id', size: 5, file: file);
      }

      try {
        final first = document('a'), second = document('b');
        view.session.printer = printer;
        printer.addListener(view.session.notifyListeners);
        printer.pending.addAll([first, second]);
        view.calls.clear();
        view.session.notifyListeners();
        await tester.pump();
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 200));
        expect(view.calls, contains('flash'));
        expect(find.byKey(const Key('print-dialog')), findsOneWidget);
        expect(find.textContaining('대기 1개 더'), findsOneWidget);
        // The channel ends while the dialog is open: the shown document is
        // still on disk, the one behind it is lost and counted.
        await tester.runAsync(printer.close);
        await tester.pump();
        expect(printer.lostDocuments, 1);
        expect(first.file.existsSync(), isTrue);
        expect(second.file.existsSync(), isFalse);
        // Cancelling finishes the held document (deleted) and no second
        // dialog opens: nothing else is waiting.
        await tester.tap(find.byKey(const Key('print-cancel')));
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: 300)),
        );
        // (the dialog's printer list never loads under fake time, so its
        // progress bar animates: no pumpAndSettle)
        await tester.pump(const Duration(milliseconds: 400));
        await tester.pump(const Duration(milliseconds: 400));
        expect(first.file.existsSync(), isFalse);
        expect(find.byKey(const Key('print-dialog')), findsNothing);
      } finally {
        printer.removeListener(view.session.notifyListeners);
        await view.close(tester);
        if (folder.existsSync()) folder.deleteSync(recursive: true);
      }
    },
  );

  testWidgets(
    'Printers allowed but the VM cannot offer one: said once when connected, with the reason; no notice otherwise',
    (tester) async {
      var view = _View();
      await view.mount(tester);
      try {
        await tester.pump(const Duration(milliseconds: 300));
        expect(find.textContaining('PC로 인쇄할 수 없어요'), findsNothing);
      } finally {
        await view.close(tester);
      }
      view = _View(printUnavailable: 'print_spooler_unavailable');
      await view.mount(tester);
      try {
        await tester.pump(const Duration(milliseconds: 300));
        expect(
          find.textContaining(
            '이 데스크톱에서는 PC로 인쇄할 수 없어요. ${printCodeMessage('print_spooler_unavailable')}',
          ),
          findsOneWidget,
        );
        expect(printCodeMessage('print_spooler_unavailable'), contains('스풀러'));
      } finally {
        await view.close(tester);
      }
    },
  );

  testWidgets('The quality chip opens the 연결 panel under itself, with the '
      'actual remote resolution', (tester) async {
    final view = _View();
    await view.mount(tester);
    try {
      await view.open(tester);
      final chip = tester.getRect(find.byKey(const Key('session-quality')));
      await tester.tap(find.byKey(const Key('session-quality')));
      await tester.pumpAndSettle();
      const connection = Key('session-connection-panel');
      expect(find.byKey(connection), findsOneWidget);
      final panel = tester.getRect(find.byKey(const Key('anchored-panel')));
      expect(panel.top, greaterThanOrEqualTo(chip.bottom));
      expect(panel.top - chip.bottom, lessThan(24));
      // Which desktop and what kind of connection.
      for (final text in ['TESTER', '스트리밍', '왕복 지연 · Host 응답']) {
        expect(
          find.descendant(
            of: find.byKey(connection),
            matching: find.text(text),
          ),
          findsOneWidget,
          reason: text,
        );
      }
      // The remote Windows size the picture really has, as one of three
      // tiles (no space inside the tile's number).
      final tile = find.byKey(const Key('quality-value-resolution'));
      expect(
        find.descendant(of: tile, matching: find.text('해상도')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: tile, matching: find.text('1280×720')),
        findsOneWidget,
      );
      for (final id in ['fps', 'rate', 'path', 'encode', 'decode']) {
        expect(
          find.byKey(Key('quality-value-$id')),
          findsOneWidget,
          reason: id,
        );
      }
      // The latency is not poor: no advice.
      expect(find.byKey(const Key('quality-advice')), findsNothing);
      expect(find.byKey(const Key('quality-saver')), findsNothing);
      expect(_row('quality-stats'), findsOneWidget);
      // The bar stays open under it.
      expect(find.byKey(_bar), findsOneWidget);
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(find.byKey(connection), findsNothing);
      expect(find.byKey(_bar), findsOneWidget);
    } finally {
      await view.close(tester);
    }
  });

  testWidgets('연결 통계 보기 in the 연결 panel opens the statistics and folds '
      'the bar', (tester) async {
    final view = _View();
    await view.mount(tester);
    try {
      await view.open(tester);
      await tester.tap(find.byKey(const Key('session-quality')));
      await tester.pumpAndSettle();
      await tester.tap(_row('quality-stats'));
      // (the statistics refresh every second: no settling)
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      expect(find.byKey(const Key('session-connection-panel')), findsNothing);
      expect(find.byType(SessionStatsDialog), findsOneWidget);
      expect(find.byKey(_bar), findsNothing, reason: 'Used, it folds.');
      await tester.tap(find.byKey(const Key('close-session-stats')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      expect(find.byType(SessionStatsDialog), findsNothing);
    } finally {
      await view.close(tester);
    }
  });
}

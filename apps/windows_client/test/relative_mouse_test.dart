// relative-mouse-v1: the state machine with an injected clock and a recorded
// runner, the exact connect gating, what a session sends, and the picture's
// pointer handling. Injected HTTP, socket, media and runner: no network, VM,
// real cursor or raw input, and nothing here says a game works.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/input_latency.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/relative_mouse.dart';
import 'package:multitap_client/data/remote_cursor.dart'
    show cursorEchoAdvertised, cursorComposeAdvertised, cursorMediaKind;
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/usb_game_mouse.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_panels.dart';

import 'display_remote_test.dart' show machine;
import 'power_api_test.dart' show ApiClient;
import 'remote_cursor_test.dart'
    show CursorMedia, cursorPacket, shapeRecord, stateRecord;
import 'remote_session_test.dart' show SessionSocket, flush, packet;
import 'usb_redirection_test.dart' show FakeBackend, FakeSocket;
import 'viewport_session_test.dart' show startInfo;

const _origin = 'https://fixture.invalid';
const _notice = '마우스를 데스크톱 안에 잠갔어요 · Ctrl+Shift+Alt+Z로 켜기·끄기';
const _injectedNotice = '롤 같은 일부 게임에서는 마우스가 움직이지 않을 수 있어요';
const _picture = Rect.fromLTWH(10, 20, 640, 360);

/// A row of the bar's 설정 panel (마우스 잠금, 바 열어 두기, 화질). The
/// row widget is matched, not its key alone: the row hands its key to its
/// InkWell too, so the key by itself finds two widgets.
Finder _panelRow(String key) => find.byWidgetPredicate(
  (widget) => widget is SessionPanelRow && widget.key == Key(key),
);

/// The Semantics `toggled` of a switch row of the 설정 panel (null: no switch).
bool? _rowToggled(WidgetTester tester, String key) => tester
    .widgetList<Semantics>(
      find.descendant(of: _panelRow(key), matching: find.byType(Semantics)),
    )
    .map((semantics) => semantics.properties.toggled)
    .firstWhere((toggled) => toggled != null, orElse: () => null);

List<int> _pts(int pts) => [for (var i = 0; i < 8; i++) (pts >> (8 * i)) & 255];

/// A cursor record of the Host's capture tick [pts].
Uint8List _cursorAt(Uint8List record, int pts) =>
    Uint8List.fromList([cursorMediaKind, 0, 0, 0, ..._pts(pts), ...record]);

/// A video frame (not a key frame) of the Host's capture tick [pts].
Uint8List _videoAt(int pts) =>
    Uint8List.fromList([1, 0, 0, 0, ..._pts(pts), 0, 0, 0, 1, 0x41]);

class _Native implements RelativeMouseNative {
  final calls = <String>[];
  final pictures = <Rect?>[];
  // cursor-echo-v1: whether each engagement asked to be pinned.
  final pins = <bool>[];
  // cursor-compose-v1: whether each engagement asked to show no cursor.
  final conceals = <bool>[];
  bool accept = true;
  // The runner's virtual USB mouse takes this engagement.
  bool usb = false;
  Object? failure;
  Completer<bool>? gate;
  void Function(String)? _listener;
  bool disposed = false;

  int get engages => calls.where((call) => call == 'engage').length;
  int get releases => calls.where((call) => call == 'release').length;

  @override
  Future<RelativeMousePath?> engage(
    Rect? picture, {
    bool pin = false,
    bool conceal = false,
  }) async {
    calls.add('engage');
    pictures.add(picture);
    pins.add(pin);
    conceals.add(conceal);
    if (failure != null) throw failure!;
    final accepted = await (gate?.future ?? Future.value(accept));
    return !accepted
        ? null
        : usb
        ? RelativeMousePath.usb
        : RelativeMousePath.injected;
  }

  @override
  Future<void> release() async => calls.add('release');
  @override
  Future<void> armKey(bool armed) async => calls.add('key:$armed');
  @override
  void listen(void Function(String event) onEvent) => _listener = onEvent;
  @override
  void dispose() => disposed = true;
  void emit(String event) => _listener?.call(event);
}

class _Rig {
  _Rig() {
    control = RelativeMouseControl(
      native: native,
      picture: () => _picture,
      onChanged: () => changes++,
      onEngaged: () => engagedNotices++,
    );
  }
  final native = _Native();
  late final RelativeMouseControl control;
  int changes = 0, engagedNotices = 0;
  // The remote cursor is visible: the ordinary desktop.
  bool available = true, hidden = false, owned = true, over = true;

  void update() => control.update(
    available: available,
    hidden: hidden,
    owned: owned,
    over: over,
  );

  /// A click into the picture.
  Future<void> click() async {
    update();
    control.pressed();
    await pumpEventQueue();
  }

  /// Handed over the ordinary way: a click into the picture.
  Future<void> engaged() async {
    await click();
    expect(control.engaged, isTrue);
  }
}

// A session whose native answers report whether this PC is an RDP session.
// [losslessTiles]: its engine composites tiles-v1 (off unless a test asks).
class _Media extends CursorMedia implements LosslessTileMedia {
  bool? remoteSession = false;
  @override
  bool losslessTiles = false;
  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    'remoteSession': ?remoteSession,
  };
}

BrokerConnection _connection({
  bool cursor = true,
  bool relative = true,
  bool epochMedia = false,
  bool usb = false,
  bool echo = false,
  bool compose = false,
}) => BrokerConnection(
  sessionId: 'relative-session',
  webSocketUrl: Uri.parse(
    'wss://fixture.invalid/api/v1/sessions/relative-session',
  ),
  cursor: cursor,
  relativeMouse: relative,
  cursorEcho: echo,
  cursorCompose: compose,
  epochMedia: epochMedia,
  viewportProvisioning: epochMedia,
  usbWebSocketUrl: usb
      ? Uri.parse('wss://fixture.invalid/api/v1/sessions/relative-session/usb')
      : null,
);

// The runner's side of the 마우스 잠금 virtual USB mouse (usb_game_mouse.dart).
class _UsbNative implements UsbGameMouseNative {
  void Function()? confirmed;
  @override
  Future<void> attached(int device) async {}
  @override
  Future<void> detached() async {}
  @override
  Future<String> frame(Uint8List bytes) async => 'ok';
  @override
  void listen({
    required void Function(Uint8List frame) send,
    required void Function() confirmed,
  }) => this.confirmed = confirmed;
  @override
  void dispose() {}
}

Map<String, dynamic> _desktop(Map<String, Object> capabilities) => {
  'id': 'pve-149',
  'name': 'TESTER',
  'kind': 'vm',
  'description': '',
  'os': 'Windows',
  'status': 'ready',
  'canConnect': true,
  'host': {'connected': true, 'capabilities': capabilities},
};
Map<String, dynamic> _connected({bool granted = false}) => {
  'sessionId': 'relative-session',
  'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/relative-session',
  'mode': 'stream',
  'options': const StreamOptions().toJson(),
  if (granted) 'relativeMouseProtocol': 'relative-mouse-v1',
};

Future<(List<Map>, BrokerConnection)> _connect(
  Map<String, Object> capabilities,
  List<(int, Map<String, dynamic>)> replies, {
  bool request = true,
}) async {
  final client = ApiClient()
    ..responses.add((
      200,
      {
        'desktops': [_desktop(capabilities)],
      },
    ))
    ..responses.addAll(replies);
  final api = HttpBrokerApi(
    origin: Uri.parse(_origin),
    client: client,
    requestRelativeMouse: request,
  );
  await api.desktops('fixture');
  final connection = await api.connect('fixture', 'pve-149');
  return (
    [
      for (final item in client.requests.skip(1))
        jsonDecode(item.body.toString()) as Map,
    ],
    connection,
  );
}

class _ViewRig {
  _ViewRig({
    this.relative = true,
    this.windowActions = true,
    this.gameMouse = true,
    SessionLauncherMemory? memory,
    bool? remoteSession = false,
    this.usb = false,
    this.echo = false,
    this.compose = false,
    bool tiles = false,
    this.settings = false,
  }) : memory = memory ?? SessionLauncherMemory() {
    media.remoteSession = remoteSession;
    media.losslessTiles = tiles;
  }
  final bool relative, windowActions, gameMouse, usb, echo, compose;

  /// The 설정 panel offers 화질 (a reconnect path); a reconnect fails.
  final bool settings;
  final SessionLauncherMemory memory;
  final native = _Native();
  final usbNative = _UsbNative();
  final socket = SessionSocket();
  final usbSocket = FakeSocket();
  final media = _Media();
  final actions = SessionWindowActions(closeWindow: () async {});
  Duration now = Duration.zero;
  late final session = RemoteSession(
    connection: _connection(
      relative: relative,
      usb: usb,
      echo: echo,
      compose: compose,
    ),
    token: 'fixture-only-token',
    media: media,
    dialer: (_, _) async => socket,
    usbFactory: (connection, token) => UsbSession(
      url: connection.usbWebSocketUrl!,
      token: token,
      sessionId: 'relative-session',
      backend: FakeBackend()..devices = [],
      dialer: (_, _) async => usbSocket,
    ),
  );

  /// The VM took the virtual USB mouse through a Host that reads ahead.
  Future<void> usbMouse(WidgetTester tester) async {
    usbSocket.emit({
      'type': 'usb.ready',
      'sessionId': 'relative-session',
      'protocol': 'usb-v1',
      'limits': {
        'maxUrbBytes': 262144,
        'windowBytes': 1048576,
        'maxInFlight': 32,
        'maxDevices': 4,
        'maxAttaches': 64,
        'descriptorBytes': 4096,
      },
    });
    usbSocket.emit({'type': 'usb.status', 'state': 'ready'});
    await settle(tester);
    final attach = usbSocket.texts.singleWhere(
      (text) => text['type'] == 'usb.attach',
    );
    usbSocket.emit({'type': 'usb.attached', 'device': attach['device']});
    await settle(tester);
    usbNative.confirmed!();
    await settle(tester);
  }

  late TestGesture mouse;

  List<Map<String, dynamic>> get inputs =>
      socket.sent.where((event) => event['type'] == 'input').toList();

  Future<void> mount(WidgetTester tester) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1280, 800);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
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
    await tester.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: machine(1),
          load: () async => session,
          onReconnect: settings
              ? (_) async => throw const BrokerException('network_error')
              : null,
          windowActions: windowActions ? actions : null,
          pointerElapsed: () => now,
          pointerMemory: memory,
          sessionGuideEnabled: false,
          relativeMouseNative: native,
          usbGameMouseNative: usbNative,
        ),
      ),
    );
    await tester.pump();
    await tester.pump();
    socket.incoming.add(jsonEncode(startInfo));
    await tester.pump();
    await tester.pump();
    socket.incoming.add(packet());
    await tester.pump();
    await tester.pump();
    expect(session.frameReady, isTrue);
    socket.incoming.add(cursorPacket(shapeRecord(1)));
    socket.incoming.add(cursorPacket(stateRecord(true, 1)));
    await settle(tester);
    // The runner's answer about RDP arrives with the first statistics.
    await tester.pump(const Duration(seconds: 1));
    await settle(tester);
    mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
    await mouse.addPointer(location: const Offset(640, 400));
    addTearDown(removeMouse);
    await tester.pump();
    // Every connection starts with 마우스 잠금 off: on is the 설정 panel's
    // switch, and its dialog closed with Esc leaves the mouse here until a
    // click (확인 hands it over at once: its own tests).
    if (gameMouse &&
        relative &&
        windowActions &&
        media.remoteSession == false) {
      await enableGameMouse(tester);
    }
  }

  bool _mouseRemoved = false;
  Future<void> removeMouse() async {
    if (_mouseRemoved) return;
    _mouseRemoved = true;
    await mouse.removePointer();
  }

  Future<void> showBar(WidgetTester tester) async {
    if (find.byKey(const Key('session-command-bar')).evaluate().isEmpty) {
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await settle(tester);
    }
  }

  /// The bar's 설정 panel, where 마우스 잠금 and 바 열어 두기 are (2026-11-10).
  /// No time passes here, so the panel is up at the start of its entrance
  /// and a choice or a pop removes it at once.
  Future<void> openSettings(WidgetTester tester) async {
    await showBar(tester);
    await tester.tap(find.byKey(const Key('remote-settings')));
    await settle(tester);
    expect(find.byKey(const Key('session-settings-panel')), findsOneWidget);
  }

  /// Closes the open 설정 panel without a choice.
  Future<void> closeSettings(WidgetTester tester) async {
    Navigator.of(
      tester.element(find.byKey(const Key('session-settings-panel'))),
    ).pop();
    await settle(tester);
    expect(find.byKey(const Key('session-settings-panel')), findsNothing);
  }

  /// The 설정 panel's 마우스 잠금 switch row: the panel closes and the option
  /// changes (turning it on opens its dialog).
  Future<void> toggleGameMouse(WidgetTester tester) async {
    await openSettings(tester);
    await tester.tap(_panelRow('session-game-mouse'));
    await settle(tester);
    expect(find.byKey(const Key('session-settings-panel')), findsNothing);
  }

  /// 바 열어 두기 from the 설정 panel (the switch applies in place and the
  /// panel stays, so it is closed here).
  Future<void> pinBar(WidgetTester tester) async {
    await openSettings(tester);
    expect(_rowToggled(tester, 'pin-session-toolbar'), isFalse);
    await tester.tap(_panelRow('pin-session-toolbar'));
    await settle(tester);
    expect(_rowToggled(tester, 'pin-session-toolbar'), isTrue);
    await closeSettings(tester);
  }

  /// The 설정 panel's 마우스 잠금 row, then the dialog it opens closed with
  /// Esc: on, and nothing handed over yet.
  Future<void> enableGameMouse(WidgetTester tester) async {
    await toggleGameMouse(tester);
    expect(find.byKey(const Key('game-mouse-dialog')), findsOneWidget);
    await tester.sendKeyEvent(LogicalKeyboardKey.escape);
    await settle(tester);
    expect(find.byKey(const Key('game-mouse-dialog')), findsNothing);
    expect(native.engages, 0, reason: 'Esc hands nothing over');
  }

  Future<void> settle(WidgetTester tester) async {
    for (var i = 0; i < 6; i++) {
      // Native release replies are followed by an EventChannel event-loop
      // turn before the next claim. Null-duration pump flushes only frames
      // and microtasks; explicitly elapse zero to drain that queued timer.
      await tester.pump(Duration.zero);
    }
  }

  Future<void> click(WidgetTester tester) async {
    await mouse.down(const Offset(640, 400));
    await settle(tester);
    await mouse.up();
    await settle(tester);
  }

  Future<void> cursorState(WidgetTester tester, bool visible) async {
    socket.incoming.add(cursorPacket(stateRecord(visible, visible ? 1 : 0)));
    await settle(tester);
  }

  Future<void> wait(WidgetTester tester, Duration by) async {
    now += by;
    await tester.pump(by);
    await settle(tester);
  }

  /// Clicked into the picture: the mouse is handed over (the remote cursor
  /// stays visible).
  Future<void> engage(WidgetTester tester) async {
    await click(tester);
    expect(session.captured, isTrue);
    expect(native.engages, greaterThan(0));
  }

  /// A click that stays absolute: the runner refuses this once.
  Future<void> plainClick(WidgetTester tester) async {
    native.accept = false;
    await click(tester);
    native.accept = true;
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
  group('State machine', () {
    test(
      'A click hands the mouse over with the remote cursor visible',
      () async {
        final rig = _Rig();
        rig.update();
        await pumpEventQueue();
        expect(rig.native.engages, 0, reason: 'nothing before a click');
        rig.control.pressed();
        await pumpEventQueue();
        expect(rig.native.engages, 1);
        expect(rig.control.engaged, isTrue);
        expect(rig.engagedNotices, 1);
        expect(rig.native.calls, ['key:true', 'engage']);
        // The runner holds the pointer in the picture.
        expect(rig.native.pictures, [_picture]);
      },
    );

    // The lock needs no off and on after a resize.
    test('A resize lets go for a moment and the lock comes back by itself', () async {
      final rig = _Rig();
      await rig.click();
      expect(rig.native.engages, 1);
      rig.owned = false;
      rig.control.update(available: true, hidden: false, owned: false, over: true, paused: true);
      await pumpEventQueue();
      expect(rig.control.engaged, isFalse, reason: 'let go while resizing');
      rig.owned = true;
      rig.update();
      await pumpEventQueue();
      expect(rig.native.engages, 2, reason: 'taken again after the resize, no click');
      expect(rig.control.engaged, isTrue);
    });

    test('Focus lost after a resize does not take the mouse back by itself', () async {
      final rig = _Rig();
      await rig.click();
      rig.control.update(available: true, hidden: false, owned: false, over: true, paused: true);
      await pumpEventQueue();
      rig.control.update(available: true, hidden: false, owned: false, over: true);
      await pumpEventQueue();
      rig.update();
      await pumpEventQueue();
      expect(rig.native.engages, 1, reason: 'only a new click hands it over');
    });

    test('The picture moving takes the lock again on the new picture', () async {
      var picture = _picture;
      final native = _Native();
      final control = RelativeMouseControl(native: native, picture: () => picture);
      void update() => control.update(available: true, hidden: false, owned: true, over: true);
      update();
      control.pressed();
      await pumpEventQueue();
      expect(native.pictures, [_picture]);
      control.pictureMoved();
      await pumpEventQueue();
      expect(native.engages, 1, reason: 'unchanged picture: nothing');
      picture = const Rect.fromLTWH(0, 30, 800, 450);
      control.pictureMoved();
      await pumpEventQueue();
      expect(native.engages, 2);
      expect(native.pictures.last, picture);
      expect(control.engaged, isTrue);
    });

    test('A hidden remote cursor alone hands nothing over', () async {
      final rig = _Rig()..hidden = true;
      rig.update();
      await pumpEventQueue();
      expect(rig.native.engages, 0);
      expect(rig.control.localPointerNeeded, isTrue, reason: 'a pointer shows');
      await rig.engaged();
      expect(rig.control.localPointerNeeded, isFalse);
    });

    test(
      'A click before focus or the pointer on the picture waits for them',
      () async {
        final rig = _Rig()
          ..owned = false
          ..over = false;
        await rig.click();
        expect(rig.native.engages, 0);
        rig.owned = true;
        rig.update();
        await pumpEventQueue();
        expect(rig.native.engages, 0, reason: 'the pointer is not on it');
        rig.over = true;
        rig.update();
        await pumpEventQueue();
        expect(rig.native.engages, 1);
      },
    );

    test('확인 hands the mouse over wherever the pointer stands, once the window owns it', () async {
      final rig = _Rig()
        ..owned = false
        ..over = false;
      rig.update();
      rig.control.handOver();
      await pumpEventQueue();
      expect(
        rig.native.engages,
        0,
        reason: 'the window does not own input yet',
      );
      rig.owned = true;
      rig.update();
      await pumpEventQueue();
      expect(
        rig.native.engages,
        1,
        reason: 'no click and no pointer on the picture needed',
      );
      expect(rig.control.engaged, isTrue);
      expect(rig.engagedNotices, 1);
      expect(rig.native.pictures, [_picture], reason: 'held in the picture');
      // The release key lets go; only a click into the picture (pointer on
      // it) or the key hands it over again: the confirmation was used up.
      rig.native.emit('toggle');
      rig.update();
      await pumpEventQueue();
      expect(rig.native.engages, 1);
      await rig.click();
      expect(rig.native.engages, 1, reason: 'a click away from the picture');
      rig.over = true;
      await rig.click();
      expect(rig.native.engages, 2);
    });

    test('A refused or released 확인 hand-over is not retried', () async {
      final rig = _Rig()..over = false;
      rig.native.accept = false;
      rig.update();
      rig.control.handOver();
      await pumpEventQueue();
      expect(rig.native.engages, 1);
      expect(rig.control.engaged, isFalse);
      rig.native.accept = true;
      rig.update();
      await pumpEventQueue();
      expect(rig.native.engages, 1, reason: 'refused: absolute until a click');
      // Asked while the window does not own input, then focus is lost.
      final waiting = _Rig()
        ..owned = false
        ..over = false;
      waiting.update();
      waiting.control.handOver();
      waiting.available = false;
      waiting.update();
      waiting.available = waiting.owned = true;
      waiting.update();
      await pumpEventQueue();
      expect(waiting.native.engages, 0, reason: 'turned off before it began');
      // Off: nothing at all.
      final off = _Rig()..available = false;
      off.update();
      off.control.handOver();
      await pumpEventQueue();
      expect(off.native.calls, isEmpty);
    });

    test('Off: a click hands nothing over and the key stays remote', () async {
      final rig = _Rig()..available = false;
      await rig.click();
      rig.native.emit('toggle');
      await pumpEventQueue();
      expect(rig.native.calls, isEmpty);
      expect(rig.control.localPointerNeeded, isFalse);
    });

    test('The cursor showing or hiding never lets go', () async {
      final rig = _Rig();
      await rig.engaged();
      for (final hidden in [true, false, true, false]) {
        rig.hidden = hidden;
        rig.update();
      }
      expect(rig.control.engaged, isTrue);
      expect(rig.native.releases, 0);
      // The pointer leaving the picture while held does not let go either.
      rig.over = false;
      rig.update();
      expect(rig.control.engaged, isTrue);
    });

    test(
      'The release key lets go; the next click or the key hands it over again',
      () async {
        final rig = _Rig();
        await rig.engaged();
        rig.native.emit('toggle');
        expect(rig.control.engaged, isFalse);
        expect(rig.native.releases, 1);
        rig.update();
        await pumpEventQueue();
        expect(rig.native.engages, 1, reason: 'not without a click');
        await rig.click();
        expect(rig.control.engaged, isTrue);
        expect(rig.native.engages, 2);
        rig.native.emit('toggle');
        rig.native.emit('toggle');
        await pumpEventQueue();
        expect(rig.control.engaged, isTrue);
        expect(rig.native.engages, 3);
      },
    );

    for (final (event, what) in [
      ('focus', 'Focus loss (Alt+Tab, Win) or minimize'),
      ('lost', 'A clip Windows or another program dropped'),
      ('device', 'A pen or touch'),
    ]) {
      test('$what lets go until the next click', () async {
        final rig = _Rig();
        await rig.engaged();
        rig.native.emit(event);
        expect(rig.control.engaged, isFalse);
        expect(rig.native.releases, 0, reason: 'the runner already released');
        rig.owned = false;
        rig.update();
        rig.owned = true;
        rig.update();
        await pumpEventQueue();
        expect(rig.native.engages, 1, reason: 'focus coming back is no click');
        await rig.click();
        expect(rig.native.engages, 2);
        expect(rig.control.engaged, isTrue);
      });
    }

    test('Dart losing focus first releases too, without the runner', () async {
      final rig = _Rig();
      await rig.engaged();
      rig.owned = false;
      rig.update();
      expect(rig.control.engaged, isFalse);
      expect(rig.native.releases, 1);
      rig.owned = true;
      rig.update();
      await pumpEventQueue();
      expect(rig.native.engages, 1);
    });

    test(
      'A refused or failed engage stays absolute until the next click',
      () async {
        final rig = _Rig()..hidden = true;
        rig.native.accept = false;
        await rig.click();
        expect(rig.native.engages, 1);
        expect(rig.control.capturing, isFalse);
        expect(rig.engagedNotices, 0);
        expect(rig.control.localPointerNeeded, isTrue);
        rig.update();
        await pumpEventQueue();
        expect(rig.native.engages, 1, reason: 'no retry loop');
        rig.native.accept = true;
        await rig.click();
        expect(rig.control.engaged, isTrue);
        final broken = _Rig();
        broken.native.failure = StateError('native');
        await broken.click();
        expect(broken.control.capturing, isFalse);
      },
    );

    test('A release while the runner is answering wins', () async {
      final rig = _Rig();
      rig.native.gate = Completer<bool>();
      await rig.click();
      expect(rig.control.capturing, isTrue);
      expect(rig.control.engaged, isFalse);
      rig.native.emit('toggle');
      rig.native.gate!.complete(true);
      await pumpEventQueue();
      expect(rig.control.capturing, isFalse);
      expect(rig.native.releases, 1);
      expect(rig.engagedNotices, 0);
    });

    test('Turning it off or the session ending lets go and disarms', () async {
      final rig = _Rig();
      await rig.engaged();
      rig.available = false;
      rig.update();
      expect(rig.control.engaged, isFalse);
      expect(rig.native.calls, containsAll(['release', 'key:false']));
      rig.control.pressed();
      await pumpEventQueue();
      expect(rig.native.engages, 1);
      final waiting = _Rig();
      waiting.update();
      waiting.control.dispose();
      waiting.control.pressed();
      await pumpEventQueue();
      expect(waiting.native.engages, 0);
      expect(waiting.native.disposed, isTrue);
      expect(waiting.native.calls, ['key:true', 'key:false']);
    });

    test('Closing the window while captured releases, once', () async {
      final rig = _Rig();
      await rig.engaged();
      rig.control.dispose();
      rig.control.dispose();
      expect(rig.native.releases, 1);
      expect(rig.native.disposed, isTrue);
      rig.native.emit('toggle');
      rig.update();
      await pumpEventQueue();
      expect(rig.native.engages, 1);
    });

    test('The path the runner chose holds for that engagement only', () async {
      final rig = _Rig()..native.usb = true;
      await rig.engaged();
      expect(rig.control.usbPath, isTrue);
      rig.native.emit('toggle');
      expect(rig.control.usbPath, isFalse, reason: 'released');
      rig.native.usb = false;
      await rig.engaged();
      expect(rig.control.usbPath, isFalse, reason: 'injected this time');
      rig.native.emit('lost');
      expect(rig.control.usbPath, isFalse);
    });

    test('Each hand-over reports itself', () async {
      final rig = _Rig();
      await rig.engaged();
      rig.native.emit('toggle');
      await rig.engaged();
      expect(rig.engagedNotices, 2);
    });
  });

  group('Capability gate', () {
    const released = <String, Object>{'service': true, 'streaming': true};
    const capable = <String, Object>{
      ...released,
      'relativeMouseProtocol': 'relative-mouse-v1',
    };

    test('Advertised only as the exact marker', () {
      expect(relativeMouseAdvertised(capable), isTrue);
      for (final value in <Object?>[
        released,
        {'relativeMouseProtocol': 'relative-mouse-v2'},
        {'relativeMouseProtocol': true},
        null,
        'relative-mouse-v1',
      ]) {
        expect(relativeMouseAdvertised(value), isFalse);
      }
    });

    test('Old Host or Broker: the field is never sent', () async {
      final legacy = await _connect(released, [(200, _connected())]);
      expect(legacy.$1.single.containsKey('relativeMouseProtocol'), isFalse);
      expect(legacy.$2.relativeMouse, isFalse);
      final other = await _connect(
        {...released, 'relativeMouseProtocol': 'relative-mouse-v2'},
        [(200, _connected())],
      );
      expect(other.$1.single.containsKey('relativeMouseProtocol'), isFalse);
    });

    test(
      'Android and every client that does not ask: the field is never sent',
      () async {
        final quiet = await _connect(capable, [
          (200, _connected()),
        ], request: false);
        expect(quiet.$1.single.containsKey('relativeMouseProtocol'), isFalse);
        expect(quiet.$2.relativeMouse, isFalse);
      },
    );

    test('A capable desktop asks and the grant is read', () async {
      final granted = await _connect(capable, [
        (200, _connected(granted: true)),
      ]);
      expect(granted.$1.single['relativeMouseProtocol'], 'relative-mouse-v1');
      expect(granted.$2.relativeMouse, isTrue);
      // Requested but not granted (two monitors, a replaced Host).
      final ungranted = await _connect(capable, [(200, _connected())]);
      expect(ungranted.$2.relativeMouse, isFalse);
    });

    test('A Broker rejecting the field gets one retry without it', () async {
      final fallback = await _connect(capable, [
        (
          400,
          {
            'error': {'code': 'unexpected_field'},
          },
        ),
        (200, _connected()),
      ]);
      expect(fallback.$1, hasLength(2));
      expect(fallback.$1.first['relativeMouseProtocol'], 'relative-mouse-v1');
      expect(fallback.$1.last.containsKey('relativeMouseProtocol'), isFalse);
      expect(fallback.$2.relativeMouse, isFalse);
    });

    test('A grant is accepted only when requested, exact and for a stream', () {
      final origin = Uri.parse(_origin);
      expect(
        () => BrokerConnection.fromJson(_connected(granted: true), origin),
        throwsA(isA<BrokerException>()),
        reason: 'never requested',
      );
      expect(
        () => BrokerConnection.fromJson(
          {..._connected(), 'relativeMouseProtocol': 'relative-mouse-v2'},
          origin,
          requestedRelativeMouse: true,
        ),
        throwsA(isA<BrokerException>()),
      );
      expect(
        BrokerConnection.fromJson(
          _connected(granted: true),
          origin,
          requestedRelativeMouse: true,
        ).relativeMouse,
        isTrue,
      );
    });
  });

  group('Session', () {
    Future<(RemoteSession, SessionSocket, _Media)> started({
      bool cursor = true,
      bool relative = true,
    }) async {
      final socket = SessionSocket(), media = _Media();
      final session = RemoteSession(
        connection: _connection(cursor: cursor, relative: relative),
        token: 'fixture',
        media: media,
        dialer: (_, _) async => socket,
      );
      unawaited(session.start());
      await flush();
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.info',
          'codec': 'h264',
          'width': 1280,
          'height': 720,
        }),
      );
      await flush();
      socket.incoming.add(packet());
      await flush();
      await session.capture(true);
      await flush();
      addTearDown(() async {
        await session.close();
        await media.events.close();
      });
      return (session, socket, media);
    }

    List<Map<String, dynamic>> moves(SessionSocket socket) =>
        socket.sent.where((event) => event['event'] == 'moveRelative').toList();
    const move = {'type': 'input', 'event': 'moveRelative', 'dx': 12, 'dy': -3};

    test('Relative only with the grant, cursor-v1 and no tracks', () async {
      expect((await started()).$1.relativeMouse, isTrue);
      expect((await started(relative: false)).$1.relativeMouse, isFalse);
      expect((await started(cursor: false)).$1.relativeMouse, isFalse);
    });

    test('The runner\'s movement is sent exactly while captured', () async {
      final (session, socket, media) = await started();
      media.events.add(move);
      await flush();
      expect(moves(socket), [move]);
      await session.capture(false);
      await flush();
      media.events.add(move);
      await flush();
      expect(moves(socket), hasLength(1));
    });

    test('Nothing new is sent in a session without the grant', () async {
      final (_, socket, media) = await started(relative: false);
      media.events.add(move);
      await flush();
      expect(moves(socket), isEmpty);
    });

    test(
      'Movement stays dropped while the output changes and after close',
      () async {
        final socket = SessionSocket(), media = _Media();
        final session = RemoteSession(
          connection: _connection(epochMedia: true),
          token: 'fixture',
          media: media,
          dialer: (_, _) async => socket,
        );
        unawaited(session.start());
        socket.incoming.add(jsonEncode(startInfo));
        await flush();
        socket.incoming.add(packet());
        await flush();
        await session.capture(true);
        media.events.add(move);
        await flush();
        expect(moves(socket), hasLength(1));
        unawaited(session.resizeViewport(1346, 812));
        await flush();
        expect(session.resizing, isTrue);
        media.events.add(move);
        await flush();
        expect(moves(socket), hasLength(1));
        await session.close();
        media.events.add(move);
        await flush();
        expect(moves(socket), hasLength(1));
        await media.events.close();
      },
    );

    test('Hidden needs an accepted explicit state', () async {
      final (session, socket, media) = await started();
      expect(session.remoteCursorHidden, isFalse, reason: 'no state yet');
      socket.incoming.add(cursorPacket(shapeRecord(1)));
      await flush();
      expect(session.remoteCursorHidden, isFalse, reason: 'a shape alone');
      socket.incoming.add(cursorPacket(stateRecord(false, 0)));
      await flush();
      expect(session.remoteCursorHidden, isTrue);
      socket.incoming.add(cursorPacket(stateRecord(true, 1)));
      await flush();
      expect(session.remoteCursorHidden, isFalse);
      socket.incoming.add(cursorPacket(stateRecord(false, 0)));
      await flush();
      expect(session.remoteCursorHidden, isTrue);
      // The presenter failing is uncertainty: not hidden.
      media.cursorFailure = PlatformException(code: 'invalid_cursor');
      socket.incoming.add(cursorPacket(stateRecord(false, 0)));
      await flush();
      expect(session.remoteCursorHidden, isFalse);
    });

    test('Movement is sampled for input latency like a move', () {
      final latency = InputLatencyMeter();
      expect(latency.sample(move, 1000000), isNotNull);
      expect(
        latency.sample({...move, 'dx': 1}, 1001000),
        isNull,
        reason: 'the same spacing as a move',
      );
    });
  });

  group('Picture', () {
    testWidgets('Absolute mode is unchanged when the feature does not engage', (
      tester,
    ) async {
      final rig = _ViewRig(relative: false);
      await rig.mount(tester);
      await rig.click(tester);
      await rig.cursorState(tester, false);
      await rig.wait(tester, const Duration(seconds: 1));
      await rig.mouse.moveTo(const Offset(320, 200));
      await rig.settle(tester);
      await rig.mouse.down(const Offset(320, 200));
      await rig.settle(tester);
      await rig.mouse.up();
      await rig.settle(tester);
      await tester.sendEventToBinding(
        const PointerScrollEvent(
          position: Offset(320, 200),
          scrollDelta: Offset(0, 40),
          kind: PointerDeviceKind.mouse,
        ),
      );
      await rig.settle(tester);
      expect(rig.native.calls, isEmpty);
      final kinds = rig.inputs.map((event) => event['event']).toSet();
      expect(kinds, containsAll(['move', 'button', 'wheel']));
      expect(kinds, isNot(contains('moveRelative')));
      for (final event in rig.inputs) {
        expect(event.containsKey('x'), isTrue, reason: '$event');
        expect(event.containsKey('y'), isTrue, reason: '$event');
      }
      await rig.close(tester);
    });

    testWidgets('A click hands the mouse over with the remote cursor visible', (
      tester,
    ) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      expect(rig.session.remoteCursorHidden, isFalse);
      expect(rig.native.engages, 0, reason: 'nothing before a click');
      await rig.click(tester);
      expect(rig.native.engages, 1);
      expect(find.text(_notice), findsOneWidget);
      // The click lands where it was made; the runner holds the pointer in
      // the picture it was told.
      final buttons = rig.inputs.where((e) => e['event'] == 'button').toList();
      expect(buttons.first['down'], isTrue);
      expect(buttons.first.containsKey('x'), isTrue);
      final picture = rig.native.pictures.single!;
      expect(picture.width, greaterThan(100));
      expect(picture.contains(const Offset(640, 400)), isTrue);
      rig.socket.sent.clear();
      await rig.mouse.moveTo(const Offset(320, 200));
      await rig.settle(tester);
      await rig.wait(tester, const Duration(milliseconds: 10));
      expect(rig.inputs.where((event) => event['event'] == 'move'), isEmpty);
      // The notice goes after a few seconds.
      await rig.wait(tester, const Duration(seconds: 3));
      expect(find.text(_notice), findsNothing);
      await rig.close(tester);
    });

    testWidgets(
      'With the virtual USB mouse nothing is injected: not the click that hands it over, nor anything after',
      (tester) async {
        final rig = _ViewRig(usb: true);
        rig.native.usb = true;
        await rig.mount(tester);
        await rig.usbMouse(tester);
        rig.socket.sent.clear();
        await rig.click(tester);
        expect(rig.native.engages, 1);
        expect(rig.native.calls.last, 'engage');
        await rig.mouse.moveTo(const Offset(300, 300));
        await rig.settle(tester);
        await rig.mouse.down(const Offset(300, 300));
        await rig.settle(tester);
        await rig.mouse.up();
        await rig.settle(tester);
        await tester.sendEventToBinding(
          const PointerScrollEvent(
            position: Offset(300, 300),
            scrollDelta: Offset(0, 40),
            kind: PointerDeviceKind.mouse,
          ),
        );
        await rig.settle(tester);
        await rig.wait(tester, const Duration(milliseconds: 10));
        expect(rig.inputs, isEmpty);
        expect(find.text(_notice), findsOneWidget);
        // Nothing to warn about on the USB path.
        await rig.wait(tester, const Duration(seconds: 3));
        expect(find.text(_injectedNotice), findsNothing);
        await rig.close(tester);
      },
    );

    testWidgets(
      'Without the USB mouse a hand-over warns once per session that some games may ignore it',
      (tester) async {
        final rig = _ViewRig();
        await rig.mount(tester);
        await rig.engage(tester);
        expect(find.text(_notice), findsOneWidget);
        expect(find.text(_injectedNotice), findsNothing);
        await rig.wait(tester, const Duration(milliseconds: 2600));
        expect(find.text(_injectedNotice), findsOneWidget);
        await rig.wait(tester, const Duration(seconds: 5));
        expect(find.text(_injectedNotice), findsNothing);
        rig.native.emit('toggle');
        await rig.settle(tester);
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(rig.native.engages, 2);
        await rig.wait(tester, const Duration(seconds: 3));
        expect(find.text(_injectedNotice), findsNothing, reason: 'once');
        await rig.close(tester);
      },
    );

    testWidgets('A pen or a finger on the picture hands nothing over', (
      tester,
    ) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      for (final kind in [PointerDeviceKind.touch, PointerDeviceKind.stylus]) {
        final finger = await tester.startGesture(
          const Offset(600, 380),
          kind: kind,
        );
        await rig.settle(tester);
        await finger.up();
        await rig.settle(tester);
      }
      expect(rig.native.engages, 0);
      await rig.close(tester);
    });

    testWidgets('While held, the bar takes no pointer; after the key it does', (
      tester,
    ) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      await rig.engage(tester);
      await tester.tap(
        find.byKey(const Key('expand-session-toolbar')),
        warnIfMissed: false,
      );
      await rig.settle(tester);
      expect(find.byKey(const Key('session-command-bar')), findsNothing);
      expect(rig.native.calls.last, isNot('release'));
      rig.native.emit('toggle');
      await rig.settle(tester);
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await rig.settle(tester);
      expect(find.byKey(const Key('session-command-bar')), findsOneWidget);
      expect(
        rig.native.engages,
        1,
        reason: 'a click on the bar is not one into the picture',
      );
      await rig.close(tester);
    });

    testWidgets('No session window, or RDP around this client: never engages', (
      tester,
    ) async {
      final plain = _ViewRig(windowActions: false);
      await plain.mount(tester);
      await plain.click(tester);
      await plain.cursorState(tester, false);
      await plain.wait(tester, const Duration(seconds: 1));
      expect(plain.native.calls, isEmpty);
      await plain.close(tester);
    });

    testWidgets('RDP around this client: never engages', (tester) async {
      final rdp = _ViewRig(remoteSession: true);
      await rdp.mount(tester);
      await rdp.click(tester);
      await rdp.cursorState(tester, false);
      await rdp.wait(tester, const Duration(seconds: 1));
      expect(rdp.native.calls, isEmpty);
      await rdp.close(tester);
    });

    testWidgets('An unknown answer about RDP is not a yes', (tester) async {
      final rig = _ViewRig(remoteSession: null);
      await rig.mount(tester);
      await rig.click(tester);
      await rig.cursorState(tester, false);
      await rig.wait(tester, const Duration(seconds: 1));
      expect(rig.native.calls, isEmpty);
      await rig.close(tester);
    });

    testWidgets(
      'Captured: buttons and wheel carry no position, no moves go out',
      (tester) async {
        final rig = _ViewRig();
        await rig.mount(tester);
        await rig.engage(tester);
        expect(rig.native.engages, 1);
        expect(find.text(_notice), findsOneWidget);
        rig.socket.sent.clear();
        await rig.mouse.moveTo(const Offset(100, 100));
        await rig.settle(tester);
        await rig.mouse.down(const Offset(100, 100));
        await rig.settle(tester);
        await rig.mouse.up();
        await rig.settle(tester);
        await tester.sendEventToBinding(
          const PointerScrollEvent(
            position: Offset(100, 100),
            scrollDelta: Offset(0, 40),
            kind: PointerDeviceKind.mouse,
          ),
        );
        await rig.settle(tester);
        expect(rig.inputs, [
          {'type': 'input', 'event': 'button', 'button': 'left', 'down': true},
          {'type': 'input', 'event': 'button', 'button': 'left', 'down': false},
          {'type': 'input', 'event': 'wheel', 'delta': -120},
        ]);
        // The runner's movement reaches the socket in order with them.
        rig.media.events.add({
          'type': 'input',
          'event': 'moveRelative',
          'dx': 4,
          'dy': 5,
        });
        await rig.settle(tester);
        expect(rig.inputs.last, {
          'type': 'input',
          'event': 'moveRelative',
          'dx': 4,
          'dy': 5,
        });
        await rig.close(tester);
      },
    );

    testWidgets(
      'Side buttons carry a position in absolute mode, none when captured',
      (tester) async {
        final rig = _ViewRig();
        await rig.mount(tester);
        await rig.plainClick(tester);
        Future<List<Map<String, dynamic>>> press(int buttons) async {
          rig.socket.sent.clear();
          final side = await tester.startGesture(
            const Offset(640, 400),
            kind: PointerDeviceKind.mouse,
            buttons: buttons,
          );
          // Removed at the end: one mouse leaving reads as the picture left.
          addTearDown(side.removePointer);
          await rig.settle(tester);
          expect(rig.session.remoteInputHeld, isTrue);
          await side.up();
          await rig.settle(tester);
          expect(rig.session.remoteInputHeld, isFalse);
          return rig.inputs.where((e) => e['event'] == 'button').toList();
        }

        rig.native.accept = false;
        for (final (buttons, name) in [
          (kBackMouseButton, 'x1'),
          (kForwardMouseButton, 'x2'),
        ]) {
          final absolute = await press(buttons);
          expect(absolute.map((e) => (e['button'], e['down'])), [
            (name, true),
            (name, false),
          ]);
          for (final event in absolute) {
            expect(event['x'], isA<double>());
            expect(event['y'], isA<double>());
          }
        }
        rig.native.accept = true;
        await rig.engage(tester);
        expect(await press(kBackMouseButton), [
          {'type': 'input', 'event': 'button', 'button': 'x1', 'down': true},
          {'type': 'input', 'event': 'button', 'button': 'x1', 'down': false},
        ]);
        expect(await press(kForwardMouseButton), [
          {'type': 'input', 'event': 'button', 'button': 'x2', 'down': true},
          {'type': 'input', 'event': 'button', 'button': 'x2', 'down': false},
        ]);
        await rig.close(tester);
      },
    );

    testWidgets('The click that hands the mouse over stays one press', (
      tester,
    ) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      await rig.plainClick(tester);
      // This press hands the mouse over while it is still held.
      await rig.mouse.down(const Offset(640, 400));
      await rig.settle(tester);
      expect(rig.native.engages, 2);
      expect(rig.native.calls.last, 'engage');
      final presses = rig.inputs
          .where((event) => event['event'] == 'button')
          .length;
      await rig.mouse.up();
      await rig.settle(tester);
      expect(
        rig.inputs.where((event) => event['event'] == 'button'),
        hasLength(presses + 1),
      );
      expect(rig.inputs.last['down'], isFalse);
      await rig.close(tester);
    });

    testWidgets(
      'On the virtual USB mouse, buttons and wheel are its own: nothing is sent',
      (tester) async {
        final rig = _ViewRig();
        rig.native.usb = true;
        await rig.mount(tester);
        await rig.engage(tester);
        rig.socket.sent.clear();
        await rig.mouse.moveTo(const Offset(100, 100));
        await rig.settle(tester);
        await rig.mouse.down(const Offset(100, 100));
        await rig.settle(tester);
        await rig.mouse.up();
        await rig.settle(tester);
        await tester.sendEventToBinding(
          const PointerScrollEvent(
            position: Offset(100, 100),
            scrollDelta: Offset(0, 40),
            kind: PointerDeviceKind.mouse,
          ),
        );
        await rig.settle(tester);
        expect(rig.inputs, isEmpty);
        // Released: buttons carry positions again.
        rig.native.emit('toggle');
        await rig.settle(tester);
        rig.socket.sent.clear();
        await rig.plainClick(tester);
        final buttons = rig.inputs.where((e) => e['event'] == 'button');
        expect(buttons.map((e) => e['down']), [true, false]);
        expect(buttons.every((e) => e.containsKey('x')), isTrue);
        await rig.close(tester);
      },
    );

    testWidgets(
      'The press that hands over the virtual USB mouse is still let go here',
      (tester) async {
        final rig = _ViewRig();
        rig.native.usb = true;
        await rig.mount(tester);
        await rig.plainClick(tester);
        await rig.mouse.down(const Offset(640, 400));
        await rig.settle(tester);
        expect(rig.native.engages, 2);
        final presses = rig.inputs.where((e) => e['event'] == 'button').length;
        await rig.mouse.up();
        await rig.settle(tester);
        expect(
          rig.inputs.where((e) => e['event'] == 'button'),
          hasLength(presses + 1),
        );
        expect(rig.inputs.last, {
          'type': 'input',
          'event': 'button',
          'button': 'left',
          'down': false,
        });
        // A new press is the virtual mouse's.
        await rig.mouse.down(const Offset(640, 400));
        await rig.settle(tester);
        await rig.mouse.up();
        await rig.settle(tester);
        expect(
          rig.inputs.where((e) => e['event'] == 'button'),
          hasLength(presses + 1),
        );
        await rig.close(tester);
      },
    );

    testWidgets('The release key returns to absolute moves', (tester) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      await rig.engage(tester);
      // The remote cursor showing or hiding never lets go.
      await rig.cursorState(tester, false);
      await rig.cursorState(tester, true);
      expect(rig.native.releases, 0);
      rig.native.emit('toggle');
      await rig.settle(tester);
      expect(rig.native.releases, 1);
      rig.socket.sent.clear();
      await rig.mouse.moveTo(const Offset(320, 200));
      await rig.settle(tester);
      await rig.wait(tester, const Duration(milliseconds: 10));
      expect(rig.inputs.single['event'], 'move');
      await rig.close(tester);
    });

    testWidgets('Every hand-over says so, briefly', (tester) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      await rig.engage(tester);
      expect(find.text(_notice), findsOneWidget);
      await rig.wait(tester, const Duration(seconds: 3));
      expect(find.text(_notice), findsNothing);
      rig.native.emit('toggle');
      await rig.settle(tester);
      rig.native.emit('toggle');
      await rig.settle(tester);
      expect(rig.native.engages, 2);
      expect(find.text(_notice), findsOneWidget);
      await rig.close(tester);
    });

    testWidgets(
      'Toggling the option off preserves the remote hidden cursor state',
      (tester) async {
        final rig = _ViewRig();
        await rig.mount(tester);
        await rig.engage(tester);
        await rig.cursorState(tester, false);
        MouseCursor shown() => tester
            .widgetList<MouseRegion>(find.byType(MouseRegion))
            .firstWhere((region) => region.child is Texture)
            .cursor;
        expect(shown(), SystemMouseCursors.none);
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(shown(), SystemMouseCursors.none);
        expect(rig.session.captured, isTrue, reason: 'the keyboard stays');
        await rig.close(tester);
      },
    );

    testWidgets('The release key frees the toolbar, which works as before', (
      tester,
    ) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      await rig.engage(tester);
      expect(find.byKey(const Key('session-command-bar')), findsNothing);
      rig.native.emit('toggle');
      await rig.settle(tester);
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await rig.settle(tester);
      expect(find.byKey(const Key('session-command-bar')), findsOneWidget);
      await rig.close(tester);
    });

    testWidgets('A pinned toolbar stays visible and capture still works', (
      tester,
    ) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      // mount's 마우스 잠금 folded the bar again; 바 열어 두기 is in 설정.
      await rig.pinBar(tester);
      expect(find.byKey(const Key('session-command-bar')), findsOneWidget);
      await rig.engage(tester);
      expect(rig.native.engages, 1);
      expect(find.byKey(const Key('session-command-bar')), findsOneWidget);
      await rig.close(tester);
    });

    testWidgets('Losing input ownership releases', (tester) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      await rig.engage(tester);
      rig.media.events.add({'type': 'capture.released'});
      await rig.settle(tester);
      expect(rig.native.releases, 1);
      await rig.close(tester);
    });

    testWidgets('Closing the view releases', (tester) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      await rig.engage(tester);
      await rig.close(tester);
      expect(rig.native.releases, 1);
      expect(rig.native.disposed, isTrue);
    });
  });

  group('마우스 잠금 option', () {
    // 마우스 잠금 is a switch row of the bar's 설정 panel (2026-11-10).
    const button = 'session-game-mouse';
    Future<void> toggle(WidgetTester tester, _ViewRig rig) =>
        rig.toggleGameMouse(tester);

    /// The 설정 panel offers 마우스 잠금 and shows it off, with its keys.
    Future<void> expectOfferedOff(WidgetTester tester, _ViewRig rig) async {
      await rig.openSettings(tester);
      expect(_panelRow(button), findsOneWidget);
      expect(_rowToggled(tester, button), isFalse);
      expect(
        find.descendant(of: _panelRow(button), matching: find.text('로 켜고 꺼요')),
        findsOneWidget,
      );
      for (final key in SessionKeycaps.mouseLock) {
        expect(
          find.descendant(of: _panelRow(button), matching: find.text(key)),
          findsOneWidget,
          reason: key,
        );
      }
      await rig.closeSettings(tester);
    }

    testWidgets(
      'The shortcut toggles the option on and off, including from off',
      (tester) async {
        final rig = _ViewRig(gameMouse: false);
        await rig.mount(tester);
        await rig.click(tester);
        expect(rig.session.captured, isTrue);
        expect(rig.native.calls, contains('key:true'));
        expect(rig.native.engages, 0);

        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(find.byKey(const Key('game-mouse-dialog')), findsNothing);
        expect(rig.native.engages, 1);
        expect(find.text(_notice), findsOneWidget);

        rig.native.emit('toggle');
        expect(
          rig.native.releases,
          1,
          reason: 'off releases before another frame',
        );
        await rig.settle(tester);
        expect(find.text('마우스 잠금 꺼짐'), findsOneWidget);
        await rig.click(tester);
        expect(
          rig.native.engages,
          1,
          reason: 'off stays off after picture clicks',
        );

        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(find.byKey(const Key('game-mouse-dialog')), findsNothing);
        expect(
          rig.native.engages,
          2,
          reason: 'the next press turns it back on',
        );
        await rig.close(tester);
      },
    );

    testWidgets('The shortcut cannot enable an ungranted session', (
      tester,
    ) async {
      final rig = _ViewRig(gameMouse: false, relative: false);
      await rig.mount(tester);
      await rig.click(tester);
      rig.native.emit('toggle');
      await rig.settle(tester);
      expect(rig.native.calls, isEmpty);
      expect(find.byKey(const Key('game-mouse-dialog')), findsNothing);
      await rig.close(tester);
    });

    testWidgets('A new shortcut ON gets its own USB hand-over wait after OFF', (
      tester,
    ) async {
      final rig = _ViewRig(gameMouse: false, usb: true);
      await rig.mount(tester);
      await rig.click(tester);
      rig.native.emit('toggle');
      await rig.settle(tester);
      for (var i = 0; i < 35; i++) {
        await rig.wait(tester, const Duration(milliseconds: 100));
      }
      expect(rig.native.engages, 0);
      rig.native.emit('toggle');
      await rig.settle(tester);
      rig.native.emit('toggle');
      await rig.settle(tester);
      for (var i = 0; i < 6; i++) {
        await rig.wait(tester, const Duration(milliseconds: 100));
      }
      expect(
        rig.native.engages,
        0,
        reason: 'the cancelled ON cannot spend the new ON wait',
      );
      rig.native.usb = true;
      await rig.usbMouse(tester);
      await rig.wait(tester, const Duration(milliseconds: 100));
      expect(rig.native.engages, 1);
      expect(find.byKey(const Key('game-mouse-dialog')), findsNothing);
      await rig.close(tester);
    });

    testWidgets(
      'Losing focus cancels a shortcut USB hand-over before it can recapture',
      (tester) async {
        final rig = _ViewRig(gameMouse: false, usb: true);
        rig.native.usb = true;
        await rig.mount(tester);
        await rig.click(tester);
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(rig.native.engages, 0);
        tester.binding.handleAppLifecycleStateChanged(
          AppLifecycleState.inactive,
        );
        await rig.settle(tester);
        await rig.usbMouse(tester);
        await rig.wait(tester, const Duration(milliseconds: 100));
        expect(
          rig.session.captured,
          isFalse,
          reason: 'late USB completion cannot request focus',
        );
        tester.binding.handleAppLifecycleStateChanged(
          AppLifecycleState.resumed,
        );
        await rig.settle(tester);
        expect(
          rig.native.engages,
          0,
          reason: 'resuming is not a new hand-over',
        );
        await rig.close(tester);
      },
    );

    testWidgets(
      'Off by default: clicks and a hidden cursor stay absolute, without capture',
      (tester) async {
        final rig = _ViewRig(gameMouse: false);
        await rig.mount(tester);
        await rig.click(tester);
        await rig.click(tester);
        await rig.cursorState(tester, false);
        await rig.wait(tester, const Duration(seconds: 1));
        await rig.mouse.moveTo(const Offset(320, 200));
        await rig.settle(tester);
        // No clip or raw input. The local shortcut can still turn it on.
        expect(rig.native.calls, ['key:true']);
        final kinds = rig.inputs.map((event) => event['event']).toSet();
        expect(kinds, contains('move'));
        expect(kinds, isNot(contains('moveRelative')));
        await rig.close(tester);
      },
    );

    testWidgets(
      'A quality mode never turns it on: clicks stay absolute, without capture',
      (tester) async {
        final rig = _ViewRig(gameMouse: false, settings: true);
        await rig.mount(tester);
        expect(rig.session.captured, isTrue);
        expect(rig.native.calls, ['key:true']);
        // 설정, then its 화질 row: the settings open in its place.
        await rig.openSettings(tester);
        await tester.tap(_panelRow('remote-stream-settings'));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 500));
        expect(find.byKey(const Key('stream-settings-panel')), findsOneWidget);
        expect(rig.session.captured, isFalse);
        expect(rig.native.calls, ['key:true', 'key:false']);
        expect(rig.native.engages, 0);
        for (final mode in ['saver', 'work']) {
          await tester.tap(find.byKey(Key('stream-preset-$mode')));
          await rig.settle(tester);
        }
        expect(find.byKey(const Key('game-mouse-dialog')), findsNothing);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 500));
        expect(find.byKey(const Key('stream-settings-panel')), findsNothing);
        await rig.click(tester);
        await rig.cursorState(tester, false);
        await rig.wait(tester, const Duration(seconds: 1));
        await rig.mouse.moveTo(const Offset(320, 200));
        await rig.settle(tester);
        expect(rig.native.calls, [
          'key:true',
          'key:false',
          'key:true',
        ], reason: 'initial keyboard, local UI release, then picture claim');
        expect(
          rig.native.engages,
          0,
          reason: 'shortcut only, no mouse capture',
        );
        expect(
          rig.inputs.map((event) => event['event']),
          isNot(contains('moveRelative')),
        );
        // 설정 still offers turning it on.
        await expectOfferedOff(tester, rig);
        await rig.close(tester);
      },
    );

    testWidgets(
      'Every connection starts off: ON in one connection is not carried to the next',
      (tester) async {
        final first = _ViewRig();
        await first.mount(tester);
        await first.engage(tester);
        expect(first.native.engages, 1);
        await first.close(tester);
        await first.removeMouse();
        // The next connection on this PC, with the same local memory.
        final rig = _ViewRig(gameMouse: false, memory: first.memory);
        await rig.mount(tester);
        await rig.click(tester);
        await rig.cursorState(tester, false);
        await rig.wait(tester, const Duration(seconds: 1));
        expect(rig.native.calls, [
          'key:true',
        ], reason: 'shortcut only, no mouse capture');
        // 설정 offers turning it on.
        await expectOfferedOff(tester, rig);
        await rig.close(tester);
      },
    );

    testWidgets(
      'Turning it on opens a dialog that says how to release; its 확인 hands the mouse over at once',
      (tester) async {
        final rig = _ViewRig(gameMouse: false);
        await rig.mount(tester);
        await toggle(tester, rig);
        final dialog = find.byKey(const Key('game-mouse-dialog'));
        expect(dialog, findsOneWidget);
        expect(
          find.descendant(of: dialog, matching: find.text('마우스 잠금')),
          findsOneWidget,
        );
        expect(
          find.descendant(
            of: dialog,
            matching: find.text(
              '확인을 누르면 마우스를 데스크톱 안에 잠가요.\nCtrl+Shift+Alt+Z로 다시 끌 수 있어요.',
            ),
          ),
          findsOneWidget,
        );
        expect(
          find.descendant(
            of: dialog,
            matching: find.byWidgetPredicate((w) => w is ButtonStyleButton),
          ),
          findsOneWidget,
          reason: 'one button',
        );
        expect(
          find.descendant(of: dialog, matching: find.text('확인')),
          findsOneWidget,
        );
        // The old one-line notice is gone; the dialog says it.
        expect(find.text('게임 마우스 켜짐 · 화면을 클릭하면 마우스를 VM에 넘겨요'), findsNothing);
        // A mouse click on 확인, right over the picture.
        await rig.mouse.moveTo(
          tester.getCenter(find.byKey(const Key('game-mouse-dialog-ok'))),
        );
        await rig.settle(tester);
        await rig.mouse.down(
          tester.getCenter(find.byKey(const Key('game-mouse-dialog-ok'))),
        );
        await rig.settle(tester);
        await rig.mouse.up();
        await rig.settle(tester);
        expect(dialog, findsNothing);
        // No click into the picture: 확인 handed the mouse over by itself.
        expect(rig.native.engages, 1, reason: '확인 hands the mouse over');
        expect(
          rig.session.captured,
          isTrue,
          reason: 'the picture takes input again',
        );
        expect(
          rig.native.pictures.last,
          isNotNull,
          reason: 'held in the picture',
        );
        expect(find.text(_notice), findsOneWidget);
        expect(
          rig.inputs.where((e) => e['event'] == 'button'),
          isEmpty,
          reason: 'the click on 확인 never reached the remote',
        );
        // The shortcut turns the option off; only another toggle turns it on.
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(rig.native.releases, 1);
        await rig.mouse.moveTo(const Offset(640, 400));
        await rig.settle(tester);
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(rig.native.engages, 2);
        await rig.close(tester);
      },
    );

    testWidgets(
      '확인 waits for the virtual USB mouse so the hand-over uses it, and goes on without it after 4 s',
      (tester) async {
        final rig = _ViewRig(gameMouse: false, usb: true);
        rig.native.usb = true;
        await rig.mount(tester);
        await toggle(tester, rig);
        await tester.tap(find.byKey(const Key('game-mouse-dialog-ok')));
        await rig.settle(tester);
        await rig.wait(tester, const Duration(milliseconds: 500));
        expect(
          rig.native.engages,
          0,
          reason: 'the VM has not confirmed the USB mouse yet',
        );
        await rig.usbMouse(tester);
        await rig.wait(tester, const Duration(milliseconds: 300));
        expect(
          rig.native.engages,
          1,
          reason: 'handed over once the USB mouse is there',
        );
        expect(find.text(_notice), findsOneWidget);
        expect(
          find.text(_injectedNotice),
          findsNothing,
          reason: 'the USB path, not injected movement',
        );
        await rig.close(tester);
        await rig.removeMouse();
        // No USB answer at all: injected movement after the bounded wait.
        final slow = _ViewRig(gameMouse: false, usb: true);
        await slow.mount(tester);
        await toggle(tester, slow);
        await tester.tap(find.byKey(const Key('game-mouse-dialog-ok')));
        await slow.settle(tester);
        await slow.wait(tester, const Duration(seconds: 3));
        expect(slow.native.engages, 0);
        await slow.wait(tester, const Duration(seconds: 2));
        expect(
          slow.native.engages,
          1,
          reason: 'never left without the hand-over',
        );
        await slow.close(tester);
      },
    );

    testWidgets('Its dialog closed with Esc hands nothing over until a click', (
      tester,
    ) async {
      final rig = _ViewRig(gameMouse: false);
      await rig.mount(tester);
      await rig.enableGameMouse(tester);
      await rig.wait(tester, const Duration(seconds: 1));
      expect(rig.native.engages, 0);
      expect(find.text(_notice), findsNothing);
      await rig.mouse.moveTo(const Offset(640, 400));
      await rig.settle(tester);
      await rig.engage(tester);
      expect(rig.native.engages, 1);
      expect(find.text(_notice), findsOneWidget);
      await rig.close(tester);
    });

    testWidgets('Turning it off while captured lets go and stays off', (
      tester,
    ) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      await rig.engage(tester);
      rig.native.emit('focus');
      await rig.settle(tester);
      await toggle(tester, rig);
      expect(find.byKey(const Key('game-mouse-dialog')), findsNothing);
      final engages = rig.native.engages;
      await rig.click(tester);
      await rig.cursorState(tester, false);
      await rig.wait(tester, const Duration(seconds: 1));
      expect(rig.native.engages, engages);
      await rig.close(tester);
    });

    testWidgets('A change on a pinned bar applies without another event', (
      tester,
    ) async {
      final rig = _ViewRig(gameMouse: false);
      await rig.mount(tester);
      expect(rig.session.captured, isTrue);
      expect(rig.native.calls, ['key:true']);
      await rig.pinBar(tester);
      await rig.cursorState(tester, false);
      expect(rig.session.captured, isFalse);
      expect(rig.native.calls, ['key:true', 'key:false']);
      expect(rig.native.engages, 0);
      await toggle(tester, rig);
      expect(find.byKey(const Key('game-mouse-dialog')), findsOneWidget);
      // Esc: armed without a hand-over (확인 would hand the mouse over too).
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await rig.settle(tester);
      expect(find.byKey(const Key('game-mouse-dialog')), findsNothing);
      expect(
        find.byKey(const Key('session-command-bar')),
        findsOneWidget,
        reason: 'the pinned bar stays open after the switch',
      );
      expect(rig.native.calls, [
        'key:true',
        'key:false',
      ], reason: 'local toolbar released the initial keyboard claim');
      expect(rig.session.captured, isFalse);
      expect(rig.native.engages, 0);
      await toggle(tester, rig);
      expect(find.text('마우스 잠금 꺼짐'), findsOneWidget);
      expect(rig.native.calls, [
        'key:true',
        'key:false',
      ], reason: 'no mouse capture and no keyboard ownership');
      expect(rig.session.captured, isFalse);
      expect(rig.native.engages, 0);
      await rig.close(tester);
    });

    testWidgets('설정 offers it on a capable session', (tester) async {
      final rig = _ViewRig(gameMouse: false);
      await rig.mount(tester);
      await expectOfferedOff(tester, rig);
      await rig.close(tester);
    });

    testWidgets('On, its 설정 row says which path it uses', (tester) async {
      final rig = _ViewRig();
      await rig.mount(tester);
      await rig.openSettings(tester);
      // No USB in this session: injected movement. The release key is named.
      expect(_rowToggled(tester, button), isTrue);
      expect(
        find.descendant(
          of: _panelRow(button),
          matching: find.text('로 꺼요 · 일반 입력'),
        ),
        findsOneWidget,
      );
      for (final key in SessionKeycaps.mouseLock) {
        expect(
          find.descendant(of: _panelRow(button), matching: find.text(key)),
          findsOneWidget,
          reason: key,
        );
      }
      await rig.closeSettings(tester);
      await rig.close(tester);
      // The same row once the VM took the virtual USB mouse.
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionSettingsPanel(
              mouseLock: true,
              mouseLockUsb: true,
              onSelected: (_) {},
            ),
          ),
        ),
      );
      await tester.pump();
      expect(_rowToggled(tester, button), isTrue);
      expect(find.text('로 꺼요 · USB 마우스'), findsOneWidget);
      expect(find.text('로 꺼요 · 일반 입력'), findsNothing);
    });

    for (final (name, rig) in [
      ('without the grant', () => _ViewRig(gameMouse: false, relative: false)),
      ('inside RDP', () => _ViewRig(gameMouse: false, remoteSession: true)),
      (
        'with no answer about RDP',
        () => _ViewRig(gameMouse: false, remoteSession: null),
      ),
    ]) {
      testWidgets('설정 leaves it out $name', (tester) async {
        final other = rig();
        await other.mount(tester);
        await other.openSettings(tester);
        expect(find.byKey(const Key('session-command-bar')), findsOneWidget);
        expect(_panelRow(button), findsNothing);
        expect(find.byKey(const Key(button)), findsNothing);
        // The panel's other switch is there: the row is left out, not hidden.
        expect(_panelRow('pin-session-toolbar'), findsOneWidget);
        await other.closeSettings(tester);
        await other.close(tester);
      });
    }
  });

  group('Cursor echo (cursor-echo-v1)', () {
    const released = <String, Object>{'service': true, 'streaming': true};
    const relativeOnly = <String, Object>{
      ...released,
      'cursorProtocol': 'cursor-v1',
      'relativeMouseProtocol': 'relative-mouse-v1',
    };
    const echoing = <String, Object>{
      ...relativeOnly,
      'cursorEchoProtocol': 'cursor-echo-v1',
    };
    Map<String, dynamic> grant({bool echo = true}) => {
      ..._connected(granted: true),
      'cursorProtocol': 'cursor-v1',
      if (echo) 'cursorEchoProtocol': 'cursor-echo-v1',
    };
    List<bool> echoes(SessionSocket socket) => [
      for (final event in socket.sent)
        if (event['type'] == 'cursor.echo') event['on'] as bool,
    ];

    test('Advertised only with cursor-v1 and the exact marker', () {
      expect(cursorEchoAdvertised(echoing), isTrue);
      for (final value in <Object?>[
        relativeOnly,
        {...released, 'cursorEchoProtocol': 'cursor-echo-v1'},
        {...relativeOnly, 'cursorEchoProtocol': 'cursor-echo-v2'},
        {...relativeOnly, 'cursorEchoProtocol': true},
        null,
      ]) {
        expect(cursorEchoAdvertised(value), isFalse, reason: '$value');
      }
    });

    test('Asked with cursor-v1 and relative-mouse-v1 where advertised; older peers never see it', () async {
      final granted = await _connect(echoing, [(200, grant())]);
      expect(granted.$1.single['cursorEchoProtocol'], 'cursor-echo-v1');
      expect(granted.$1.single['cursorProtocol'], 'cursor-v1');
      expect(granted.$2.cursorEcho, isTrue);
      final older = await _connect(relativeOnly, [(200, grant(echo: false))]);
      expect(older.$1.single.containsKey('cursorEchoProtocol'), isFalse);
      expect(older.$2.cursorEcho, isFalse);
      final quiet = await _connect(echoing, [
        (200, {..._connected(), 'cursorProtocol': 'cursor-v1'}),
      ], request: false);
      expect(
        quiet.$1.single.containsKey('cursorEchoProtocol'),
        isFalse,
        reason: 'not without relative-mouse-v1',
      );
      final ungranted = await _connect(echoing, [(200, grant(echo: false))]);
      expect(ungranted.$2.cursorEcho, isFalse);
    });

    test('A grant is accepted only when requested, with cursor-v1, exact', () {
      final origin = Uri.parse(_origin);
      expect(
        () => BrokerConnection.fromJson(
          grant(),
          origin,
          requestedCursor: true,
          requestedRelativeMouse: true,
        ),
        throwsA(isA<BrokerException>()),
        reason: 'never requested',
      );
      expect(
        () => BrokerConnection.fromJson(
          {..._connected(), 'cursorEchoProtocol': 'cursor-echo-v1'},
          origin,
          requestedCursorEcho: true,
        ),
        throwsA(isA<BrokerException>()),
        reason: 'without the cursor-v1 grant',
      );
      expect(
        () => BrokerConnection.fromJson(
          {...grant(), 'cursorEchoProtocol': 'cursor-echo-v2'},
          origin,
          requestedCursor: true,
          requestedRelativeMouse: true,
          requestedCursorEcho: true,
        ),
        throwsA(isA<BrokerException>()),
      );
      expect(
        BrokerConnection.fromJson(
          grant(),
          origin,
          requestedCursor: true,
          requestedRelativeMouse: true,
          requestedCursorEcho: true,
        ).cursorEcho,
        isTrue,
      );
    });

    Future<(RemoteSession, SessionSocket, _Media)> started(bool echo) async {
      final socket = SessionSocket(), media = _Media();
      final session = RemoteSession(
        connection: _connection(echo: echo),
        token: 'fixture',
        media: media,
        dialer: (_, _) async => socket,
      );
      unawaited(session.start());
      await flush();
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.info',
          'codec': 'h264',
          'width': 1280,
          'height': 720,
        }),
      );
      await flush();
      socket.incoming.add(packet());
      await flush();
      addTearDown(() async {
        await session.close();
        await media.events.close();
      });
      return (session, socket, media);
    }

    test('The session asks and stops once each, only with the grant', () async {
      for (final granted in [true, false]) {
        final (session, socket, _) = await started(granted);
        expect(session.cursorEcho, granted);
        expect(session.echoCursor(true), granted);
        expect(session.echoCursor(true), granted);
        expect(session.cursorEchoing, granted);
        expect(session.echoCursor(false), granted);
        await flush();
        expect(echoes(socket), granted ? [true, false] : isEmpty);
      }
    });

    test('Echoed positions reach the presenter in order; a visibility change still waits', () async {
      final (session, socket, media) = await started(true);
      socket.incoming.add(cursorPacket(shapeRecord(1)));
      socket.incoming.add(cursorPacket(stateRecord(true, 1)));
      await flush();
      session.echoCursor(true);
      for (var n = 0; n < 40; n++) {
        socket.incoming.add(
          cursorPacket(stateRecord(true, 1, position: true, x: n, y: 7)),
        );
      }
      socket.incoming.add(packet());
      socket.incoming.add(cursorPacket(stateRecord(false, 0)));
      await flush();
      final states = [
        for (final (_, record) in media.records)
          if (record[0] == 2) record,
      ];
      expect(states, hasLength(42));
      expect(
        [for (final r in states.skip(1).take(40)) r[8]],
        [for (var n = 0; n < 40; n++) n],
      );
      expect(states.last, stateRecord(false, 0));
      expect(session.remoteCursorHidden, isTrue);
    });

    testWidgets(
      'On an echoing Host a hand-over is pinned and asks for the echo; every release stops it',
      (tester) async {
        final rig = _ViewRig(echo: true);
        await rig.mount(tester);
        expect(echoes(rig.socket), isEmpty, reason: 'nothing before a click');
        await rig.engage(tester);
        expect(rig.native.pins.last, isTrue);
        expect(echoes(rig.socket), [true]);
        // The release key.
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(echoes(rig.socket), [true, false]);
        // Another shortcut turns it on, then the runner lets go (focus loss).
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(rig.native.pins.last, isTrue);
        rig.native.emit('focus');
        await rig.settle(tester);
        expect(echoes(rig.socket), [true, false, true, false]);
        // A pen or touch device ended it.
        await rig.engage(tester);
        rig.native.emit('device');
        await rig.settle(tester);
        expect(echoes(rig.socket), [true, false, true, false, true, false]);
        await rig.close(tester);
      },
    );

    testWidgets(
      'An older Host: never pinned and nothing asked',
      (tester) async {
        final rig = _ViewRig();
        await rig.mount(tester);
        await rig.engage(tester);
        expect(rig.native.pins, everyElement(isFalse));
        expect(
          rig.native.pictures.last,
          isNotNull,
          reason: 'held in the picture',
        );
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(echoes(rig.socket), isEmpty);
        await rig.close(tester);
      },
    );
  });

  group('Cursor in the video (cursor-compose-v1)', () {
    const released = <String, Object>{'service': true, 'streaming': true};
    const relativeOnly = <String, Object>{
      ...released,
      'cursorProtocol': 'cursor-v1',
      'relativeMouseProtocol': 'relative-mouse-v1',
    };
    const echoing = <String, Object>{
      ...relativeOnly,
      'cursorEchoProtocol': 'cursor-echo-v1',
    };
    const composing = <String, Object>{
      ...echoing,
      'cursorComposeProtocol': 'cursor-compose-v1',
    };
    Map<String, dynamic> grant({bool compose = true, bool echo = true}) => {
      ..._connected(granted: true),
      'cursorProtocol': 'cursor-v1',
      if (echo) 'cursorEchoProtocol': 'cursor-echo-v1',
      if (compose) 'cursorComposeProtocol': 'cursor-compose-v1',
    };
    List<bool> sent(SessionSocket socket, String type) => [
      for (final event in socket.sent)
        if (event['type'] == type) event['on'] as bool,
    ];

    test('Advertised only with cursor-v1 and the exact marker', () {
      expect(cursorComposeAdvertised(composing), isTrue);
      expect(
        cursorComposeAdvertised({
          ...relativeOnly,
          'cursorComposeProtocol': 'cursor-compose-v1',
        }),
        isTrue,
      );
      for (final value in <Object?>[
        echoing,
        {...released, 'cursorComposeProtocol': 'cursor-compose-v1'},
        {...echoing, 'cursorComposeProtocol': 'cursor-compose-v2'},
        {...echoing, 'cursorComposeProtocol': true},
        null,
      ]) {
        expect(cursorComposeAdvertised(value), isFalse, reason: '$value');
      }
    });

    test('Asked beside the echo where advertised; older peers never see it', () async {
      final granted = await _connect(composing, [(200, grant())]);
      expect(granted.$1.single['cursorComposeProtocol'], 'cursor-compose-v1');
      expect(granted.$1.single['cursorEchoProtocol'], 'cursor-echo-v1');
      expect(granted.$2.cursorCompose, isTrue);
      final older = await _connect(echoing, [(200, grant(compose: false))]);
      expect(older.$1.single.containsKey('cursorComposeProtocol'), isFalse);
      expect(older.$2.cursorCompose, isFalse);
      expect(
        older.$2.cursorEcho,
        isTrue,
        reason: 'the echo stays the fallback',
      );
      final quiet = await _connect(composing, [
        (200, {..._connected(), 'cursorProtocol': 'cursor-v1'}),
      ], request: false);
      expect(
        quiet.$1.single.containsKey('cursorComposeProtocol'),
        isFalse,
        reason: 'not without relative-mouse-v1',
      );
      final ungranted = await _connect(composing, [
        (200, grant(compose: false)),
      ]);
      expect(ungranted.$2.cursorCompose, isFalse);
      // A released broker that rejects the field: the retry carries none of it.
      final retried = await _connect(composing, [
        (
          400,
          {
            'error': {'code': 'unexpected_field'},
          },
        ),
        (200, _connected()),
      ]);
      expect(retried.$1.first['cursorComposeProtocol'], 'cursor-compose-v1');
      expect(retried.$1.last.containsKey('cursorComposeProtocol'), isFalse);
      expect(retried.$2.cursorCompose, isFalse);
    });

    test('A grant is accepted only when requested, with cursor-v1, exact', () {
      final origin = Uri.parse(_origin);
      expect(
        () => BrokerConnection.fromJson(
          grant(echo: false),
          origin,
          requestedCursor: true,
          requestedRelativeMouse: true,
        ),
        throwsA(isA<BrokerException>()),
        reason: 'never requested',
      );
      expect(
        () => BrokerConnection.fromJson(
          {..._connected(), 'cursorComposeProtocol': 'cursor-compose-v1'},
          origin,
          requestedCursorCompose: true,
        ),
        throwsA(isA<BrokerException>()),
        reason: 'without the cursor-v1 grant',
      );
      expect(
        () => BrokerConnection.fromJson(
          {...grant(echo: false), 'cursorComposeProtocol': 'cursor-compose-v2'},
          origin,
          requestedCursor: true,
          requestedRelativeMouse: true,
          requestedCursorCompose: true,
        ),
        throwsA(isA<BrokerException>()),
      );
      expect(
        BrokerConnection.fromJson(
          grant(echo: false),
          origin,
          requestedCursor: true,
          requestedRelativeMouse: true,
          requestedCursorCompose: true,
        ).cursorCompose,
        isTrue,
      );
    });

    test('The session asks and stops once each, only with the grant', () async {
      for (final granted in [true, false]) {
        final socket = SessionSocket(), media = _Media();
        final session = RemoteSession(
          connection: _connection(compose: granted),
          token: 'fixture',
          media: media,
          dialer: (_, _) async => socket,
        );
        unawaited(session.start());
        await flush();
        expect(session.cursorCompose, granted);
        expect(session.composeCursor(true), granted);
        expect(session.composeCursor(true), granted);
        expect(session.cursorComposing, granted);
        expect(session.composeCursor(false), granted);
        await flush();
        expect(
          sent(socket, 'cursor.compose'),
          granted ? [true, false] : isEmpty,
        );
        await session.close();
        await media.events.close();
      }
    });

    testWidgets(
      'On a composing Host a hand-over shows no cursor and asks for compose (not the echo); every release stops it',
      (tester) async {
        final rig = _ViewRig(echo: true, compose: true);
        await rig.mount(tester);
        expect(sent(rig.socket, 'cursor.compose'), isEmpty);
        await rig.engage(tester);
        expect(rig.native.conceals.last, isTrue, reason: 'no local cursor');
        expect(rig.native.pins.last, isFalse, reason: 'no pinning');
        expect(rig.native.pictures.last, isNull, reason: 'parked');
        expect(sent(rig.socket, 'cursor.compose'), [true]);
        expect(sent(rig.socket, 'cursor.echo'), isEmpty, reason: 'no echo');
        // The release key.
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(sent(rig.socket, 'cursor.compose'), [true, false]);
        // Another shortcut turns it on, then the runner lets go (focus loss).
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(rig.native.conceals.last, isTrue);
        rig.native.emit('focus');
        await rig.settle(tester);
        expect(sent(rig.socket, 'cursor.compose'), [true, false, true, false]);
        // A pen or touch device ended it.
        await rig.engage(tester);
        rig.native.emit('device');
        await rig.settle(tester);
        expect(sent(rig.socket, 'cursor.compose'), [
          true,
          false,
          true,
          false,
          true,
          false,
        ]);
        // Movement still travels as before while concealed.
        await rig.engage(tester);
        expect(rig.session.captured, isTrue);
        expect(sent(rig.socket, 'cursor.echo'), isEmpty);
        rig.native.emit('toggle');
        await rig.settle(tester);
        await rig.close(tester);
      },
    );

    testWidgets('An echo-only Host: pinned as before, compose never asked', (
      tester,
    ) async {
      final rig = _ViewRig(echo: true);
      await rig.mount(tester);
      await rig.engage(tester);
      expect(rig.native.pins.last, isTrue);
      expect(rig.native.conceals, everyElement(isFalse));
      expect(sent(rig.socket, 'cursor.echo'), [true]);
      rig.native.emit('toggle');
      await rig.settle(tester);
      expect(sent(rig.socket, 'cursor.compose'), isEmpty);
      await rig.close(tester);
    });

    testWidgets(
      'An older Host: held in the picture, nothing concealed or asked',
      (tester) async {
        final rig = _ViewRig();
        await rig.mount(tester);
        await rig.engage(tester);
        expect(rig.native.conceals, everyElement(isFalse));
        expect(rig.native.pictures.last, isNotNull);
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(sent(rig.socket, 'cursor.compose'), isEmpty);
        await rig.close(tester);
      },
    );

    // Backlog: after a release the video still carries the composed cursor
    // for about one round trip; a local cursor meanwhile makes two.
    MouseCursor shown(WidgetTester tester) => tester
        .widgetList<MouseRegion>(find.byType(MouseRegion))
        .firstWhere((region) => region.child is Texture)
        .cursor;

    Future<_ViewRig> letGo(WidgetTester tester) async {
      final rig = _ViewRig(echo: true, compose: true);
      await rig.mount(tester);
      await rig.engage(tester);
      // While composing the Host reports the cursor hidden.
      await rig.cursorState(tester, false);
      rig.native.emit('toggle');
      await rig.settle(tester);
      expect(sent(rig.socket, 'cursor.compose'), [true, false]);
      return rig;
    }

    testWidgets(
      'After a release no cursor shows here until the first frame after the Host stopped composing',
      (tester) async {
        final rig = await letGo(tester);
        final presented = rig.media.records.length;
        expect(shown(tester), SystemMouseCursors.none, reason: 'no fallback');
        // The Host's tick 100 stopped composing: its records follow that
        // tick's frame, the last one with the cursor drawn in.
        rig.socket.incoming.add(_cursorAt(shapeRecord(2), 100));
        rig.socket.incoming.add(_cursorAt(stateRecord(true, 2), 100));
        rig.socket.incoming.add(_videoAt(100));
        await rig.settle(tester);
        expect(rig.media.records, hasLength(presented), reason: 'held');
        expect(shown(tester), SystemMouseCursors.none);
        // The first frame captured without it.
        rig.socket.incoming.add(_videoAt(116));
        await rig.settle(tester);
        expect(
          [for (final (_, record) in rig.media.records.skip(presented)) record],
          [shapeRecord(2), stateRecord(true, 2)],
          reason: 'presented in order',
        );
        expect(rig.session.remoteCursorHidden, isFalse);
        // Later records go straight through again.
        rig.socket.incoming.add(_cursorAt(stateRecord(false, 0), 132));
        await rig.settle(tester);
        expect(rig.media.records, hasLength(presented + 3));
        await rig.close(tester);
      },
    );

    testWidgets(
      'A cursor that stays hidden sends nothing after the whole option is off',
      (tester) async {
        final rig = await letGo(tester);
        await rig.wait(tester, const Duration(milliseconds: 200));
        expect(shown(tester), SystemMouseCursors.none);
        await rig.wait(tester, const Duration(milliseconds: 60));
        expect(
          shown(tester),
          SystemMouseCursors.none,
          reason: 'ordinary off mode preserves the remote hidden state',
        );
        await rig.close(tester);
      },
    );

    testWidgets(
      'A frame after the tick that came first (UDP) ends the wait at the record',
      (tester) async {
        final rig = await letGo(tester);
        final presented = rig.media.records.length;
        rig.socket.incoming.add(_videoAt(116));
        await rig.settle(tester);
        expect(rig.media.records, hasLength(presented));
        expect(shown(tester), SystemMouseCursors.none, reason: 'not yet known');
        rig.socket.incoming.add(_cursorAt(stateRecord(true, 1), 100));
        await rig.settle(tester);
        expect(rig.media.records, hasLength(presented + 1));
        expect(rig.media.records.last.$2, stateRecord(true, 1));
        await rig.close(tester);
      },
    );

    testWidgets(
      'A hidden position from before the stop does not end the wait; a new hand-over does',
      (tester) async {
        final rig = await letGo(tester);
        final presented = rig.media.records.length;
        // An application warp the Host sent while it still composed.
        rig.socket.incoming.add(
          _cursorAt(stateRecord(false, 0, position: true, x: 5, y: 6), 90),
        );
        rig.socket.incoming.add(_videoAt(116));
        await rig.settle(tester);
        expect(rig.media.records, hasLength(presented));
        // Turned on again: compose is asked anew, the held warp applies.
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(sent(rig.socket, 'cursor.compose'), [true, false, true]);
        expect(rig.media.records, hasLength(presented + 1));
        rig.native.emit('toggle');
        await rig.settle(tester);
        await rig.close(tester);
      },
    );
  });

  group('Lossless tiles while the mouse is the VM\'s', () {
    List<String> prefixes(SessionSocket socket) => [
      for (final event in socket.sent)
        if (event['type'] == 'ping') (event['id'] as String).substring(0, 4),
    ];

    testWidgets(
      'Every hand-over pauses the tiles at once and every release opts in again',
      (tester) async {
        final rig = _ViewRig(tiles: true);
        await rig.mount(tester);
        // The opt-in went with the first media; 마우스 잠금 on alone pauses nothing.
        expect(prefixes(rig.socket), [losslessTilesPingPrefix]);
        await rig.engage(tester);
        expect(rig.session.losslessTilesPaused, isTrue);
        expect(prefixes(rig.socket).last, losslessTilesPausePrefix);
        // The release key.
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(rig.session.losslessTilesPaused, isFalse);
        expect(prefixes(rig.socket).last, losslessTilesPingPrefix);
        // Another shortcut turns it on, then the runner lets go (focus loss).
        rig.native.emit('toggle');
        await rig.settle(tester);
        expect(prefixes(rig.socket).last, losslessTilesPausePrefix);
        rig.native.emit('focus');
        await rig.settle(tester);
        expect(prefixes(rig.socket).last, losslessTilesPingPrefix);
        expect(prefixes(rig.socket), [
          losslessTilesPingPrefix,
          losslessTilesPausePrefix,
          losslessTilesPingPrefix,
          losslessTilesPausePrefix,
          losslessTilesPingPrefix,
        ]);
        await rig.close(tester);
      },
    );

    testWidgets('A refused hand-over pauses nothing', (tester) async {
      final rig = _ViewRig(tiles: true);
      await rig.mount(tester);
      await rig.plainClick(tester);
      expect(rig.session.losslessTilesPaused, isFalse);
      expect(prefixes(rig.socket), [losslessTilesPingPrefix]);
      await rig.close(tester);
    });
  });

  group('Local memory', () {
    test(
      'An earlier client\'s 마우스 가두기 and game choices are dropped, never read; '
      'anything else in the file stays',
      () async {
        final directory = await Directory.systemTemp.createTemp(
          'multitap-game',
        );
        addTearDown(() => directory.delete(recursive: true));
        final launcher = File('${directory.path}/launcher.json');
        await launcher.writeAsString('{"https://a.example":{"view":"cards"}}');
        final session = File('${directory.path}/session.json');
        final memory = FileLauncherMemory(launcher);
        // Nothing saved: nothing is created.
        await memory.forgetPointerConfinement();
        expect(session.existsSync(), isFalse);
        // Only retired keys: the file goes with them.
        await session.writeAsString(
          jsonEncode({'confinePointer': true, 'gameMouse': true}),
        );
        await memory.forgetPointerConfinement();
        expect(session.existsSync(), isFalse);
        // A key this client does not know is kept as it was.
        await session.writeAsString(
          jsonEncode({'confinePointer': true, 'other': 1}),
        );
        await memory.forgetPointerConfinement();
        expect(jsonDecode(await session.readAsString()), {'other': 1});
        // An unreadable file holds no choice; it is left alone.
        await session.writeAsString('{not json');
        await memory.forgetPointerConfinement();
        expect(await session.readAsString(), '{not json');
        expect(
          await launcher.readAsString(),
          '{"https://a.example":{"view":"cards"}}',
        );
        // The process-only memory has nothing to drop.
        await SessionLauncherMemory().forgetPointerConfinement();
      },
    );
  });
}

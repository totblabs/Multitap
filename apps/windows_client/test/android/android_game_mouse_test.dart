// 마우스 잠금 (formerly 게임 마우스) on Android: what the app asks the broker, the dock's toggle (off
// at every connection, disabled without a mouse), its dialog, the hand-over by
// a mouse click (never a finger), Back, capture loss and the release key, the
// cursor in the video, the virtual USB mouse over usb-v1 and the native
// moveRelative path. The shared RemoteSession with a fake relay socket, the
// Android media fake and a recorded pointer capture: no phone, mouse, VM or
// game, and nothing here says a game works.
import 'dart:async';
import 'dart:convert';
import 'dart:io' show HttpClientRequest, HttpHeaders;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart'
    show androidBrokerApi;
import 'package:multitap_client/android/android_game_mouse.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/relative_mouse.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/usb_game_mouse.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';

import '../power_api_test.dart'
    show ApiClient, ApiHeaders, ApiRequest, ApiResponse;
import '../remote_session_test.dart' show SessionSocket;
import '../usb_redirection_test.dart' show FakeBackend, FakeSocket;
import 'android_session_contract_test.dart' show AndroidNativeFake;

const _sessionUrl = 'wss://fixture.invalid/api/v1/sessions/android-session';

// The app's broker client reads each answer's request ID (diagnostics on).
class _Headers extends ApiHeaders {
  @override
  String? value(String name) => null;
}

class _Response extends ApiResponse {
  _Response(super.statusCode, super.value);
  @override
  final HttpHeaders headers = _Headers();
}

class _Client extends ApiClient {
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    final (status, value) = responses.removeAt(0);
    final request = ApiRequest(method, uri, _Response(status, value));
    requests.add(request);
    return request;
  }
}

/// MainActivity's pointer capture (AndroidGameMouse.kt), recorded.
class _Capture implements MousePresenceNative {
  final calls = <String>[];
  final pins = <bool>[], conceals = <bool>[];
  bool accept = true, usb = false, mouse = true;
  void Function(String)? _listener;
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
    pins.add(pin);
    conceals.add(conceal);
    return !accept
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
  void dispose() {}
  @override
  Future<bool> mousePresent() async => mouse;
  @override
  set onMouse(void Function(bool present)? callback) => _onMouse = callback;

  /// Android ended capture itself, or the release key.
  void emit(String event) => _listener?.call(event);

  /// A mouse came, or the last one left.
  void plug(bool present) {
    mouse = present;
    _onMouse?.call(present);
  }
}

/// AndroidGameMouse.kt's side of the virtual USB mouse.
class _UsbNative implements UsbGameMouseNative {
  void Function()? confirmed;
  final devices = <int>[];
  @override
  Future<void> attached(int device) async => devices.add(device);
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

class _Harness {
  final capture = _Capture();
  final usbNative = _UsbNative();
  final sockets = <SessionSocket>[];
  final usbSockets = <FakeSocket>[];
  late RemoteSession session;
  late AndroidNativeFake native;
  int connects = 0;
  SessionSocket get socket => sockets.last;
  FakeSocket get usbSocket => usbSockets.last;

  List<Map<String, dynamic>> get inputs => [
    for (final socket in sockets)
      for (final m in socket.sent)
        if (m['type'] == 'input') m,
  ];

  /// The `on` of every cursor.compose (or cursor.echo) sent, in order.
  List<bool> sent(String type) => [
    for (final socket in sockets)
      for (final m in socket.sent)
        if (m['type'] == type) m['on'] as bool,
  ];
}

Future<_Harness> _open(
  WidgetTester tester, {
  bool relative = true,
  bool compose = false,
  bool echo = false,
  bool usb = false,
  bool mouse = true,
}) async {
  tester.view.physicalSize = const Size(1080, 2400);
  tester.view.devicePixelRatio = 3;
  addTearDown(tester.view.reset);
  final h = _Harness()..capture.mouse = mouse;
  h.native = AndroidNativeFake()..install();
  addTearDown(AndroidNativeFake.uninstall);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: AndroidRemoteView(
        name: 'TESTER',
        token: 'not-persisted',
        options: const StreamOptions(),
        density: () async => null,
        settings: const AndroidSessionSettings(
          helpSeen: true,
          screens: {
            AndroidDeviceClass.phone: AndroidScreenChoice(
              resolution: AndroidResolution.device,
            ),
          },
        ),
        load: (options, authorized, scale) async {
          h.connects++;
          return BrokerConnection(
            reservation: BrokerSessionReservation(() async {}),
            sessionId: 'android-session',
            webSocketUrl: Uri.parse(_sessionUrl),
            cursor: true,
            relativeMouse: relative,
            cursorEcho: echo,
            cursorCompose: compose,
            usbWebSocketUrl: usb ? Uri.parse('$_sessionUrl/usb') : null,
          );
        },
        sessionFactory: (connection, media, statsSink) {
          return h.session = RemoteSession(
            connection: connection,
            token: 'not-persisted',
            media: media,
            statsSink: statsSink,
            dialer: (_, _) async {
              final socket = SessionSocket();
              h.sockets.add(socket);
              return socket;
            },
            usbFactory: (connection, token) => UsbSession(
              url: connection.usbWebSocketUrl!,
              token: token,
              sessionId: connection.sessionId,
              backend: FakeBackend()..devices = [],
              dialer: (_, _) async {
                final socket = FakeSocket();
                h.usbSockets.add(socket);
                return socket;
              },
            ),
          );
        },
        pictureBuilder: (_) => const ColoredBox(color: Color(0xFF2B4C6F)),
        relativeMouseNative: h.capture,
        usbGameMouseNative: h.usbNative,
      ),
    ),
  );
  await _frame(tester, h);
  return h;
}

/// The relay announces the picture and one key frame arrives.
Future<void> _frame(WidgetTester tester, _Harness h) async {
  for (var i = 0; i < 4; i++) {
    await tester.pump(const Duration(milliseconds: 20));
  }
  h.socket.incoming.add(
    jsonEncode({
      'type': 'stream.info',
      'codec': 'h264',
      'width': 1280,
      'height': 720,
      'sourceWidth': 1280,
      'sourceHeight': 720,
    }),
  );
  await tester.pump(const Duration(milliseconds: 20));
  final key = Uint8List(17)
    ..[0] = 1
    ..[1] = 1
    ..setAll(12, [0, 0, 0, 1, 0x65]);
  h.socket.incoming.add(key);
  for (var i = 0; i < 6; i++) {
    await tester.pump(const Duration(milliseconds: 40));
  }
  expect(find.byKey(const Key('android-remote-surface')), findsOneWidget);
}

Future<void> _settle(WidgetTester tester) async {
  for (var i = 0; i < 8; i++) {
    await tester.pump(const Duration(milliseconds: 20));
  }
}

/// The session menu's 설정 panel (the amber bubble opens the menu, its 설정
/// row the panel): where 마우스 잠금 is, as on the PC. Stays where it is when
/// the panel is already open.
Future<void> _openMore(WidgetTester tester) async {
  if (find.byKey(const Key('android-more-panel')).evaluate().isNotEmpty) {
    return;
  }
  if (find.byKey(const Key('session-command-bar')).evaluate().isEmpty) {
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
  }
  await tester.tap(find.byKey(const Key('android-session-more')));
  await _settle(tester);
  expect(find.byKey(const Key('android-more-panel')), findsOneWidget);
}

/// The 설정 panel, then its 마우스 잠금 row.
Future<void> _tapTile(WidgetTester tester) async {
  await _openMore(tester);
  await tester.tap(find.byKey(const Key('android-session-game-mouse')));
  await _settle(tester);
}

/// 마우스 잠금 on, and 확인 in its dialog: the mouse goes to the VM.
Future<void> _turnOn(WidgetTester tester, _Harness h) async {
  await _tapTile(tester);
  expect(find.byKey(const Key('game-mouse-dialog')), findsOneWidget);
  await tester.tap(find.byKey(const Key('game-mouse-dialog-ok')));
  await _settle(tester);
}

Future<void> _back(WidgetTester tester) async {
  await tester.binding.handlePopRoute();
  await _settle(tester);
}

Offset _picture(WidgetTester tester) =>
    tester.getCenter(find.byKey(const Key('android-remote-surface')));

Future<void> _mouseClick(WidgetTester tester) async {
  final mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
  await mouse.addPointer(location: _picture(tester));
  await mouse.down(_picture(tester));
  await _settle(tester);
  await mouse.up();
  await _settle(tester);
  await mouse.removePointer();
}

/// The 설정 panel's 마우스 잠금 row is on: its switch is on and the line under
/// its name says how to turn it off. Opens the menu and the panel, which
/// takes the mouse back like any menu use.
Future<bool> _tileOn(WidgetTester tester) async {
  await _openMore(tester);
  final tile = find.byKey(const Key('android-session-game-mouse'));
  expect(tile, findsOneWidget);
  final on = find
      .descendant(of: tile, matching: find.text('뒤로 가기로 풀어요'))
      .evaluate()
      .isNotEmpty;
  expect(
    find.descendant(of: tile, matching: find.text('마우스를 데스크톱 안에 잠가요')),
    on ? findsNothing : findsOneWidget,
  );
  // The row is a switch row, as in the Windows 설정 panel: the switch says
  // the same.
  expect(
    tester
        .widget<Switch>(
          find.descendant(of: tile, matching: find.byType(Switch)),
        )
        .value,
    on,
  );
  return on;
}

Future<void> _close(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pump(const Duration(seconds: 1));
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const pen = MethodChannel('multitap/pen-input');
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(pen, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? 'started' : true,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(pen, null);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });

  test('Android asks for relative-mouse-v1, the cursor in the video and '
      'usb-v1 where the Host shows them', () async {
    final client = _Client()
      ..responses.addAll([
        (
          200,
          {
            'desktops': [
              {
                'id': 'pve-149',
                'name': 'TESTER',
                'kind': 'vm',
                'description': '',
                'os': 'Windows',
                'status': 'ready',
                'canConnect': true,
                'host': {
                  'connected': true,
                  'capabilities': {
                    'service': true,
                    'streaming': true,
                    'cursorProtocol': 'cursor-v1',
                    'relativeMouseProtocol': 'relative-mouse-v1',
                    'cursorEchoProtocol': 'cursor-echo-v1',
                    'cursorComposeProtocol': 'cursor-compose-v1',
                    'usbProtocol': 'usb-v1',
                  },
                },
              },
            ],
          },
        ),
        (
          200,
          {
            'sessionId': 'android-session',
            'webSocketUrl': _sessionUrl,
            'mode': 'stream',
            'options': const StreamOptions().toJson(),
            'cursorProtocol': 'cursor-v1',
            'relativeMouseProtocol': 'relative-mouse-v1',
            'cursorComposeProtocol': 'cursor-compose-v1',
            'usbProtocol': 'usb-v1',
            'usbWebSocketUrl': '$_sessionUrl/usb',
          },
        ),
      ]);
    final api = androidBrokerApi(
      Uri.parse('https://fixture.invalid'),
      diagnosticsVersion: () => null,
      client: client,
    );
    await api.desktops('fixture');
    final connection = await api.connect('fixture', 'pve-149');
    final body =
        jsonDecode(
              client.requests
                  .singleWhere((r) => r.uri.path.endsWith('/connect'))
                  .body
                  .toString(),
            )
            as Map;
    expect(body['relativeMouseProtocol'], 'relative-mouse-v1');
    expect(body['cursorComposeProtocol'], 'cursor-compose-v1');
    expect(body['cursorEchoProtocol'], 'cursor-echo-v1');
    expect(body['usbProtocol'], 'usb-v1');
    expect(connection.relativeMouse, isTrue);
    expect(connection.cursorCompose, isTrue);
    expect(connection.usbWebSocketUrl, Uri.parse('$_sessionUrl/usb'));
    api.close();
  });

  test('usb-v1 on Android offers none of this device\'s USB devices', () async {
    final usb = androidUsbSession(
      BrokerConnection(
        sessionId: 'android-session',
        webSocketUrl: Uri.parse(_sessionUrl),
        usbWebSocketUrl: Uri.parse('$_sessionUrl/usb'),
      ),
      'not-persisted',
    );
    expect(usb.url, Uri.parse('$_sessionUrl/usb'));
    expect(
      (await usb.backend.environment()).driver,
      UsbDriverState.unsupported,
    );
    expect(await usb.backend.list(), isEmpty);
    await usb.close();
  });

  testWidgets('Without a mouse the tile says why; with one it turns on with '
      'its dialog; not offered without the grant', (tester) async {
    final h = await _open(tester, mouse: false);
    await _tapTile(tester);
    expect(find.byKey(const Key('game-mouse-dialog')), findsNothing);
    expect(find.byTooltip('마우스 잠금 · 마우스를 연결하면 쓸 수 있어요'), findsOneWidget);
    // Nothing ran: the 설정 panel is still there, its switch off.
    expect(find.byKey(const Key('android-more-panel')), findsOneWidget);
    expect(await _tileOn(tester), isFalse);
    expect(h.capture.engages, 0);
    // A mouse arrives (USB, Bluetooth, DeX): the tile works.
    h.capture.plug(true);
    await _settle(tester);
    await _tapTile(tester);
    expect(find.byKey(const Key('game-mouse-dialog')), findsOneWidget);
    expect(find.text('마우스 잠금'), findsWidgets);
    expect(
      find.text('확인을 누르면 마우스를 데스크톱 안에 잠가요.\n뒤로 가기를 누르면 풀려요.'),
      findsOneWidget,
    );
    await tester.tap(find.byKey(const Key('game-mouse-dialog-ok')));
    await _settle(tester);
    expect(h.capture.engages, 1);
    await _close(tester);

    // An older Host or broker: no relative-mouse-v1, no entry.
    await _open(tester, relative: false);
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-session-screen')), findsOneWidget);
    expect(find.byKey(const Key('android-session-game-mouse')), findsNothing);
    // Nor in the 설정 panel, beside 화질.
    await _openMore(tester);
    expect(find.byKey(const Key('android-session-settings')), findsOneWidget);
    expect(find.byKey(const Key('android-session-game-mouse')), findsNothing);
    await _close(tester);
  });

  testWidgets('확인 hands the mouse over with the cursor in the video; Back '
      'takes it back and nothing else', (tester) async {
    final h = await _open(tester, compose: true, echo: true);
    expect(find.byKey(const Key('android-remote-cursor')), findsOneWidget);
    await _turnOn(tester, h);
    expect(h.capture.engages, 1);
    expect(h.capture.conceals, [true], reason: 'compose wins over the echo');
    expect(h.sent('cursor.compose'), [true]);
    expect(h.sent('cursor.echo'), isEmpty);
    expect(h.capture.calls, contains('key:true'));
    expect(
      find.textContaining('마우스를 데스크톱 안에 잠갔어요 · 뒤로 가기로 풀기'),
      findsOneWidget,
    );
    // The Host draws the VM's cursor into the video: none here.
    expect(find.byKey(const Key('android-remote-cursor')), findsNothing);

    await _back(tester);
    expect(h.capture.releases, 1);
    expect(h.sent('cursor.compose'), [true, false]);
    expect(find.text('연결을 끊을까요?'), findsNothing);
    expect(find.byKey(const Key('android-remote-cursor')), findsOneWidget);
    // The option stays on: the next mouse click hands it over again.
    await _mouseClick(tester);
    expect(h.capture.engages, 2);
    // Back with nothing held is Back again.
    await _back(tester);
    await _back(tester);
    expect(find.text('연결을 끊을까요?'), findsOneWidget);
    await tester.tap(find.text('취소'));
    await _settle(tester);
    await _close(tester);
  });

  testWidgets('A mouse click hands it over and is used up; a finger never '
      'does, and takes the mouse back', (tester) async {
    final h = await _open(tester);
    await _turnOn(tester, h);
    await _back(tester);
    expect(h.capture.engages, 1);
    final before = h.inputs.where((e) => e['event'] == 'button').length;
    await _mouseClick(tester);
    expect(h.capture.engages, 2);
    expect(h.capture.pins.last, isFalse);
    expect(h.capture.conceals.last, isFalse);
    expect(
      h.inputs.where((e) => e['event'] == 'button').length,
      before,
      reason: 'the click that hands the mouse over never reaches the VM',
    );
    // A finger on the picture takes it back and is ordinary touch input.
    await tester.tapAt(_picture(tester));
    await _settle(tester);
    expect(h.capture.releases, 2);
    await tester.tapAt(_picture(tester));
    await tester.pump(const Duration(milliseconds: 500));
    await _settle(tester);
    expect(h.capture.engages, 2, reason: 'a finger never hands it over');
    await _close(tester);
  });

  testWidgets('Android taking capture back, the release key and the last '
      'mouse leaving', (tester) async {
    final h = await _open(tester, compose: true);
    await _turnOn(tester, h);
    expect(h.sent('cursor.compose'), [true]);
    // The app left the front or another window took focus.
    h.capture.emit('focus');
    await _settle(tester);
    expect(h.sent('cursor.compose'), [true, false]);
    expect(h.capture.releases, 0, reason: 'native already let go');
    // Ctrl+Alt+Shift+Z hands it over again, and back.
    h.capture.emit('toggle');
    await _settle(tester);
    expect(h.capture.engages, 2);
    h.capture.emit('toggle');
    await _settle(tester);
    expect(h.capture.releases, 1);
    h.capture.emit('toggle');
    await _settle(tester);
    expect(h.capture.engages, 3);
    // The last mouse left: released, and the option goes off.
    h.capture.plug(false);
    await _settle(tester);
    expect(h.capture.releases, 2);
    expect(h.sent('cursor.compose').last, isFalse);
    expect(h.capture.calls.skip(h.capture.calls.length - 2), [
      'key:false',
      'release',
    ]);
    expect(await _tileOn(tester), isFalse);
    expect(find.byTooltip('마우스 잠금 · 마우스를 연결하면 쓸 수 있어요'), findsOneWidget);
    await _close(tester);
  });

  testWidgets('Every connection starts with it off, a reconnect included', (
    tester,
  ) async {
    final h = await _open(tester);
    await _turnOn(tester, h);
    expect(h.capture.engages, 1);
    // The relay socket ends without a reason: a quiet reconnect.
    unawaited(h.socket.incoming.close());
    await tester.pump(const Duration(milliseconds: 50));
    expect(h.capture.releases, 1);
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.connects, 2);
    await _frame(tester, h);
    expect(await _tileOn(tester), isFalse);
    await _mouseClick(tester);
    expect(h.capture.engages, 1, reason: 'off: a click is a click');
    await _close(tester);
  });

  testWidgets('The virtual USB mouse is attached while it is on and the '
      'hand-over waits for it', (tester) async {
    final h = await _open(tester, usb: true);
    h.capture.usb = true;
    h.usbSocket.emit({
      'type': 'usb.ready',
      'sessionId': 'android-session',
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
    h.usbSocket.emit({'type': 'usb.status', 'state': 'ready'});
    await _settle(tester);
    expect(h.usbSocket.texts.where((t) => t['type'] == 'usb.attach'), isEmpty);
    await _tapTile(tester);
    final attach = h.usbSocket.texts.singleWhere(
      (t) => t['type'] == 'usb.attach',
    );
    expect(base64Decode(attach['descriptors'] as String), gameMouseDescriptors);
    await tester.tap(find.byKey(const Key('game-mouse-dialog-ok')));
    await _settle(tester);
    expect(h.capture.engages, 0, reason: 'waits while the VM takes the mouse');
    // The wait is said on the picture; nothing moved there before.
    const waiting = Key('android-game-mouse-waiting');
    expect(find.byKey(waiting), findsOneWidget);
    expect(
      find.descendant(
        of: find.byKey(waiting),
        matching: find.text('마우스를 데스크톱에 연결하는 중이에요'),
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: find.byKey(waiting),
        matching: find.byType(CircularProgressIndicator),
      ),
      findsOneWidget,
    );
    h.usbSocket.emit({'type': 'usb.attached', 'device': attach['device']});
    await _settle(tester);
    expect(h.usbNative.devices, [attach['device']]);
    h.usbNative.confirmed!();
    await tester.pump(const Duration(milliseconds: 200));
    await _settle(tester);
    expect(h.capture.engages, 1);
    expect(find.byKey(waiting), findsNothing);
    expect(find.textContaining('롤 같은 일부 게임에서는'), findsNothing);
    // Off: the VM's mouse goes away again.
    await _back(tester);
    await _tapTile(tester);
    expect(
      h.usbSocket.texts.where((t) => t['type'] == 'usb.detach'),
      hasLength(1),
    );
    await _close(tester);
  });

  testWidgets('Without the virtual USB mouse: moveRelative from the native '
      'side reaches the VM, and the hand-over says some games may refuse it '
      'once', (tester) async {
    final h = await _open(tester);
    await _turnOn(tester, h);
    expect(find.textContaining('롤 같은 일부 게임에서는 마우스가 움직이지 않을 수 있어요'),
        findsOneWidget);
    // AndroidGameMouse.kt sends through the media's input channel.
    for (final event in [
      {'type': 'input', 'event': 'moveRelative', 'dx': 5, 'dy': -3},
      {'type': 'input', 'event': 'button', 'button': 'x1', 'down': true},
      {'type': 'input', 'event': 'wheel', 'delta': -120},
    ]) {
      tester.binding.channelBuffers.push(
        'multitap/remote/input',
        const StandardMethodCodec().encodeSuccessEnvelope({
          ...event,
          'lease': h.native.lease,
        }),
        (_) {},
      );
    }
    await _settle(tester);
    expect(h.inputs.skip(h.inputs.length - 3), [
      {'type': 'input', 'event': 'moveRelative', 'dx': 5, 'dy': -3},
      {'type': 'input', 'event': 'button', 'button': 'x1', 'down': true},
      {'type': 'input', 'event': 'wheel', 'delta': -120},
    ]);
    await _back(tester);
    await _mouseClick(tester);
    expect(h.capture.engages, 2);
    expect(find.textContaining('롤 같은 일부 게임에서는'), findsNothing);
    expect(find.textContaining('마우스를 데스크톱 안에 잠갔어요'), findsOneWidget);
    await _close(tester);
  });
}

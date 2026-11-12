// The Android remote view with the shared RemoteSession, a fake relay
// socket and the Android native media contract fake. Touches, IME edits and
// lifecycle changes are simulated; no device, MediaCodec, IME or host.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/gestures.dart';
import 'package:multitap_client/ui/session_stats_dialog.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/connection_wait_status.dart';
import 'package:multitap_client/android/android_application.dart';
import 'package:multitap_client/android/android_keyboard.dart';
import 'package:multitap_client/android/android_network.dart';
import 'package:multitap_client/android/android_reconnect.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_toolbar.dart'
    show AndroidBarMenuRow, AndroidMenuCaption;
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/initial_viewport.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';

import '../remote_session_test.dart' show SessionSocket;
import 'android_session_contract_test.dart' show AndroidNativeFake;
import 'android_session_keepalive_test.dart' show deliverNotificationStop;

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

double _noJitter() => 0;

class _Harness {
  late RemoteSession session;
  final sockets = <SessionSocket>[];
  final requested = <StreamOptions>[];
  final requestedScales = <int?>[];
  final released = <String>[];

  /// Broker answers for the next connects (null = connected).
  final loadFailures = <Object?>[];

  /// The answer of a connect at this moment (null = connected), after
  /// [loadFailures].
  Object? Function()? loadAnswer;
  Completer<BrokerConnection>? loadGate;
  final saved = <AndroidSessionSettings>[];
  final stats = <Map<String, Object>>[];
  late final AndroidNativeFake native;
  SessionSocket get socket => sockets.last;

  List<String> get texts => [
    for (final m in inputs)
      if (m['event'] == 'text') m['text'] as String,
  ];

  /// Buttons, keys and text in order: L↓ L↑ R↓ … a2↓ … «text».
  List<String> get sequence => [
    for (final m in inputs)
      switch (m['event']) {
        'button' =>
          '${m['button'] == 'left' ? 'L' : 'R'}${m['down'] == true ? '↓' : '↑'}',
        'key' =>
          '${(m['vk'] as int).toRadixString(16)}${m['down'] == true ? '↓' : '↑'}',
        'text' => '«${m['text']}»',
        _ => '',
      },
  ].where((s) => s.isNotEmpty).toList();

  List<Map<String, dynamic>> get inputs => [
    for (final socket in sockets)
      for (final m in socket.sent)
        if (m['type'] == 'input') m,
  ];

  List<String> get keys => [
    for (final m in inputs)
      if (m['event'] == 'key')
        '${(m['vk'] as int).toRadixString(16)}${m['down'] == true ? '↓' : '↑'}',
  ];
}

class _NoticeClipboard extends ClipboardSession {
  _NoticeClipboard()
    : super(
        url: Uri.parse(
          'wss://fixture.invalid/api/v1/sessions/android-session/clipboard',
        ),
        token: 'not-persisted',
        sessionId: 'android-session',
      ) {
    ready = true;
  }
  int sends = 0;
  @override
  bool get canSend => true;
  @override
  Future<void> sendLocal({bool automatic = false}) async {
    sends++;
  }

  @override
  Future<void> close({String reason = 'clipboard_transport_closed'}) async {}
}

/// A granted clipboard whose sharing the relay can end ([stopped]).
class _StoppingClipboard extends _NoticeClipboard {
  bool stopped = false;
  @override
  bool get closed => stopped;
}

class _UncertainShortcutSession extends RemoteSession {
  _UncertainShortcutSession({
    required super.connection,
    required super.media,
    super.statsSink,
    super.dialer,
  }) : super(token: 'not-persisted');
  @override
  Future<bool> shortcut(String command) async {
    await super.shortcut(command); // The fake wire may already contain keys.
    return false;
  }
}

// Hold the view's capture await after the fake native acceptance. This tests
// queue ownership, not an Android IME or a delayed native platform callback.
class _DeferredCaptureSession extends RemoteSession {
  _DeferredCaptureSession({
    required super.connection,
    required super.media,
    super.statsSink,
    super.dialer,
  }) : super(token: 'not-persisted');
  Completer<void>? gate;
  int held = 0;
  @override
  Future<void> capture(bool enabled) async {
    await super.capture(enabled);
    final pending = gate;
    if (enabled && pending != null) {
      held++;
      await pending.future;
    }
  }
}

Future<_Harness> _open(
  WidgetTester tester, {
  // 해상도 기기 화소: the device's own pixels (as the former 작게 did).
  AndroidSessionSettings settings = const AndroidSessionSettings(
    helpSeen: true,
    screens: {
      AndroidDeviceClass.phone: AndroidScreenChoice(
        resolution: AndroidResolution.device,
      ),
    },
  ),
  StreamOptions options = const StreamOptions(),
  bool resizable = false,
  bool textInput = false,
  bool inputLatency = false,
  bool routed = false,
  Size picture = const Size(1280, 720),
  Size? source,
  int? grantedScale,
  Size physical = const Size(1080, 2400),
  double devicePixelRatio = 3,
  AndroidScreenDensity? density,
  AndroidNetworkEvents? network,
  TextScaler? textScaler,
  bool? boldText,
  bool uncertainShortcut = false,
  bool deferredCapture = false,
  bool firstFrame = true,
  List<Object?> loadFailures = const [],
  bool initialViewport = false,
  ClipboardSession? clipboard,
  bool television = false,
  bool tvNavigation = false,
  // Reconnect waits at their full step unless a test asks for jitter.
  double Function() reconnectJitter = _noJitter,
  Future<void> Function(bool Function() current, Duration limit)?
  awaitBrokerRestart,
  Future<void> Function(bool Function() current, Duration limit)?
  awaitHostUpdate,
  Future<void> Function(
    bool Function() current,
    Duration limit,
    void Function() onHostAway,
  )?
  awaitHostReturn,
}) async {
  tester.view.physicalSize = physical;
  tester.view.devicePixelRatio = devicePixelRatio;
  addTearDown(tester.view.reset);
  final native = AndroidNativeFake()..install();
  addTearDown(AndroidNativeFake.uninstall);
  final h = _Harness()..native = native;
  h.loadFailures.addAll(loadFailures);
  final navigator = GlobalKey<NavigatorState>();
  Widget view() => AndroidRemoteView(
    name: 'TESTER',
    token: 'not-persisted',
    options: options,
    settings: settings,
    onSettings: h.saved.add,
    onStats: (values, _) => h.stats.add(values),
    displayScale: grantedScale != null,
    network: network,
    initialDensity: density,
    density: () async => density,
    load: (options, authorized, scale) async {
      h.requested.add(options);
      h.requestedScales.add(scale);
      if (h.loadFailures.isNotEmpty) {
        final failure = h.loadFailures.removeAt(0);
        if (failure != null) throw failure;
      }
      final answer = h.loadAnswer?.call();
      if (answer != null) throw answer;
      if (h.loadGate != null) return h.loadGate!.future;
      final attempt = h.requested.length;
      final sessionId = initialViewport
          ? '00000000-0000-0000-0000-${attempt.toString().padLeft(12, '0')}'
          : 'android-session';
      return BrokerConnection(
        reservation: BrokerSessionReservation(
          () async => h.released.add('android-session-$attempt'),
        ),
        sessionId: sessionId,
        webSocketUrl: Uri.parse(
          'wss://fixture.invalid/api/v1/sessions/android-session',
        ),
        epochMedia: resizable,
        initialViewport: initialViewport
            ? InitialViewportPreparation.fromJson(
                {
                  'protocol': initialViewportProtocol,
                  'sessionId': sessionId,
                  'requestId': '10000000-0000-0000-0000-000000000001',
                  'resizeId': '20000000-0000-0000-0000-000000000001',
                  'monitor': '0',
                  'width': 1280,
                  'height': 720,
                  'fps': 60,
                  'epoch': 1,
                },
                sessionId: sessionId,
                requested: InitialViewport(width: 1280, height: 720, fps: 60),
              )
            : null,
        viewportProvisioning: resizable,
        textInput: textInput,
        inputLatency: inputLatency,
        displayScale: grantedScale == null ? null : scale,
      );
    },
    sessionFactory: (connection, media, statsSink) {
      Future<SessionSocket> dial(Uri _, String token) async {
        final socket = SessionSocket();
        h.sockets.add(socket);
        return socket;
      }

      h.session = deferredCapture
          ? _DeferredCaptureSession(
              connection: connection,
              media: media,
              statsSink: statsSink,
              dialer: dial,
            )
          : uncertainShortcut
          ? _UncertainShortcutSession(
              connection: connection,
              media: media,
              statsSink: statsSink,
              dialer: dial,
            )
          : RemoteSession(
              connection: connection,
              token: 'not-persisted',
              media: media,
              statsSink: statsSink,
              dialer: dial,
            );
      h.session.clipboard = clipboard;
      return h.session;
    },
    pictureBuilder: (_) => const ColoredBox(color: Color(0xFF2B4C6F)),
    reconnectJitter: reconnectJitter,
    awaitBrokerRestart: awaitBrokerRestart,
    awaitHostUpdate: awaitHostUpdate,
    awaitHostReturn: awaitHostReturn,
  );
  await tester.pumpWidget(
    MaterialApp(
      navigatorKey: navigator,
      theme: buildAppTheme(brightness: Brightness.dark),
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(context).copyWith(
          textScaler: textScaler ?? MediaQuery.textScalerOf(context),
          boldText: boldText ?? MediaQuery.boldTextOf(context),
        ),
        child: tvNavigation
            ? AndroidTvNavigation(television: television, child: child!)
            : AndroidTvScope(television: television, child: child!),
      ),
      home: routed ? const Scaffold(body: Text('list')) : view(),
    ),
  );
  if (routed) {
    unawaited(
      navigator.currentState!.push(
        MaterialPageRoute<void>(builder: (_) => view()),
      ),
    );
    await tester.pump();
  }
  if (firstFrame) {
    await _frame(tester, h, picture, source);
  } else {
    for (var i = 0; i < 4; i++) {
      await tester.pump(const Duration(milliseconds: 20));
    }
  }
  return h;
}

/// The relay announces the picture size and one key frame arrives.
Future<void> _frame(
  WidgetTester tester,
  _Harness h, [
  Size picture = const Size(1280, 720),
  Size? source,
]) async {
  for (var i = 0; i < 4; i++) {
    await tester.pump(const Duration(milliseconds: 20));
  }
  h.socket.incoming.add(
    jsonEncode({
      'type': 'stream.info',
      'codec': 'h264',
      'width': picture.width.toInt(),
      'height': picture.height.toInt(),
      'sourceWidth': (source ?? picture).width.toInt(),
      'sourceHeight': (source ?? picture).height.toInt(),
    }),
  );
  await tester.pump(const Duration(milliseconds: 20));
  h.socket.incoming.add(_video(16667, key: true));
  for (var i = 0; i < 6; i++) {
    await tester.pump(const Duration(milliseconds: 40));
  }
  expect(find.byKey(const Key('android-remote-surface')), findsOneWidget);
}

Future<void> _openScreenPanel(WidgetTester tester) =>
    _openPanel(tester, 'screen', 'android-screen-panel');

/// A panel of the session menu ([id]; [key] is the panel's own), from ⋯, as
/// the PC's bar opens it: 연결 품질 ('connection'), 화면 ('screen') and 설정
/// ('more') are rows of the menu card; 화질 ('settings'), 입력 방식
/// ('input') and 사용안내 ('help') are rows of the 설정 panel.
Future<void> _openPanel(WidgetTester tester, String id, String key) async {
  final panel = Key(key);
  if (find.byKey(panel).evaluate().isNotEmpty) return;
  final inMore = const {'settings', 'input', 'help'}.contains(id);
  final command = switch (id) {
    'connection' => const Key('session-quality'),
    _ when inMore => const Key('android-session-more'),
    _ => Key('android-session-$id'),
  };
  // The 설정 panel is already there (Back or 취소 from one of its panels).
  final moreOpen =
      inMore &&
      find.byKey(const Key('android-more-panel')).evaluate().isNotEmpty;
  if (!moreOpen) {
    if (find.byKey(command).evaluate().isEmpty) {
      // C hamburger opens a closed menu, or closes another page first.
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await tester.pumpAndSettle();
      if (find.byKey(command).evaluate().isEmpty) {
        await tester.tap(find.byKey(const Key('expand-session-toolbar')));
        await tester.pumpAndSettle();
      }
    }
    await tester.tap(find.byKey(command));
    await tester.pumpAndSettle();
  }
  if (inMore) {
    expect(find.byKey(const Key('android-more-panel')), findsOneWidget);
    final row = find.byKey(Key('android-session-$id'));
    await tester.ensureVisible(row);
    await tester.pumpAndSettle();
    await tester.tap(row);
    await tester.pumpAndSettle();
  }
  expect(find.byKey(panel), findsOneWidget);
}

/// 배율: Windows' scale steps inside the 화면 panel (it has no row or panel
/// of its own), scrolled into view.
Future<void> _openScale(WidgetTester tester) async {
  await _openScreenPanel(tester);
  final scale = find.byKey(const Key('android-screen-scale'));
  expect(scale, findsOneWidget);
  await tester.ensureVisible(scale);
  await tester.pumpAndSettle();
}

/// C hamburger closes the complete menu from any page or panel.
Future<void> _foldTools(WidgetTester tester) async {
  if (find.byKey(const Key('session-toolbar-pointer')).evaluate().isEmpty) return;
  await tester.tap(find.byKey(const Key('expand-session-toolbar')));
  await tester.pumpAndSettle();
  expect(find.byKey(const Key('session-toolbar-pointer')), findsNothing);
}

/// Exercise the production two-finger gesture after closing local tools.
/// Removing phone zoom buttons must not remove the actual zoom/pan path.
Future<void> _pinchPicture(WidgetTester tester) async {
  await _foldTools(tester);
  final size = tester.view.physicalSize / tester.view.devicePixelRatio;
  final picture = tester.getRect(_penPicture).intersect(Offset.zero & size);
  final centre = picture.center;
  final initial = size.width * .08;
  final target = size.width * .2;
  final first = await tester.startGesture(
    centre - Offset(initial, 0),
    pointer: 971,
  );
  final second = await tester.startGesture(
    centre + Offset(initial, 0),
    pointer: 972,
  );
  await first.moveTo(centre - Offset(target, 0));
  await second.moveTo(centre + Offset(target, 0));
  await first.up();
  await second.up();
  await tester.pumpAndSettle();
}

/// The phone keyboard opens and closes with one tap on the floating
/// cluster's 키보드 button.
Future<void> _toggleKeyboard(WidgetTester tester) async {
  await tester.tap(find.byKey(const Key('android-remote-keyboard-button')));
  await tester.pumpAndSettle();
}

/// Scrolls the open key row until its [id] key is on screen (it builds
/// lazily); [back] scrolls towards its start.
Future<Finder> _keyRowShow(
  WidgetTester tester,
  String id, {
  bool back = false,
}) async {
  final key = find.byKey(Key('android-key-$id'));
  await tester.dragUntilVisible(
    key,
    find.byKey(const Key('android-key-row-scroll')),
    Offset(back ? 120 : -120, 0),
  );
  await tester.pumpAndSettle();
  return key;
}

/// The floating cluster (키보드, 입력 방식, 도구).
Finder get _cluster => find.byKey(const Key('session-bubble'));

/// The phone keyboard is open: its key row shows.
bool _keyboardOpen(WidgetTester tester) =>
    find.byKey(const Key('android-key-row')).evaluate().isNotEmpty;

Future<void> _close(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pump(const Duration(seconds: 1));
}

const _penChannel = MethodChannel('multitap/pen-input');
Future<void> _penSignal(WidgetTester tester, Map<String, Object> value) async {
  tester.binding.channelBuffers.push(
    _penChannel.name,
    const StandardMethodCodec().encodeSuccessEnvelope(value),
    (_) {},
  );
  await tester.pump();
}

Future<void> _penDrain(WidgetTester tester) async {
  await tester.pump(const Duration(milliseconds: 10));
  for (var n = 0; n < 6; n++) {
    await tester.pump();
  }
}

Finder get _penPicture => find.byWidgetPredicate(
  (widget) => widget is ColoredBox && widget.color == const Color(0xFF2B4C6F),
);

/// Drive the real AndroidRemoteView listener and record its authenticated
/// SessionSocket output. Stop before lifting, so fling is not a second input
/// source and the same event partition is compared at both encoder sizes.
Future<List<Map<String, dynamic>>> _workspaceWheelTrace(
  WidgetTester tester,
  _Harness h,
) async {
  final before = h.inputs.length;
  const left = Offset(240, 220), right = Offset(360, 220);
  final first = await tester.createGesture(pointer: 801);
  final second = await tester.createGesture(pointer: 802);
  var firstDown = false, secondDown = false;
  try {
    await first.down(left, timeStamp: const Duration(milliseconds: 10));
    firstDown = true;
    await second.down(right, timeStamp: const Duration(milliseconds: 11));
    secondDown = true;
    for (var step = 1; step <= 10; step++) {
      final offset = Offset(0, -8.0 * step);
      await first.moveTo(
        left + offset,
        timeStamp: Duration(milliseconds: 20 + step * 16),
      );
      await second.moveTo(
        right + offset,
        timeStamp: Duration(milliseconds: 21 + step * 16),
      );
      await tester.pump();
    }
    // Drain only the issued capture/input queue, without advancing animation
    // or fling timers or waiting for the stream's periodic background work.
    for (var turn = 0; turn < 6; turn++) {
      await tester.pump();
    }
    return [
      for (final event in h.inputs.skip(before))
        if (event['event'] == 'wheel') Map<String, dynamic>.of(event),
    ];
  } finally {
    if (firstDown) await first.cancel();
    if (secondDown) await second.cancel();
    await tester.pump();
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  pixelScaleObservationRegressions();
  final keepaliveLeases = <String>[];
  setUp(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_penChannel, (_) async => null);
    keepaliveLeases.clear();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidSessionKeepalive.channel, (
          call,
        ) async {
          if (call.method == 'start') {
            keepaliveLeases.add((call.arguments as Map)['lease'] as String);
            return 'started';
          }
          return true;
        });
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_penChannel, null);
  });

  testWidgets(
    'No sound device: one plain line, long enough to read, no action; 화질 says the same',
    (tester) async {
      final h = await _open(tester);
      try {
        h.session.remoteAudioMissing = true;
        h.session.notifyListeners();
        await tester.pump();
        await tester.pump();
        // Longer than an ordinary 2 s notice.
        await tester.pump(const Duration(seconds: 8));
        final message = tester.widget<Text>(
          find.descendant(
            of: find.byKey(const Key('android-remote-notice-message')),
            matching: find.byWidgetPredicate((widget) => widget is Text),
          ),
        );
        expect(
          message.data,
          '이 데스크톱에는 소리 장치가 없어요. 관리자가 Proxmox에서 오디오 장치를 추가해야 해요.',
        );
        expect(message.data, audioDeviceMissingText);
        // Multitap adds no sound device (2026-11-10): nothing to press.
        expect(
          find.byKey(const Key('android-remote-notice-action')),
          findsNothing,
        );
        expect(find.textContaining('사운드카드'), findsNothing);
        await tester.pump(const Duration(seconds: 10));
        expect(find.byKey(const Key('android-remote-notice')), findsNothing);
        // 화질 is a row of the 설정 panel: the same line under the
        // choices, and no sound switch or sound-card action.
        await _openPanel(tester, 'settings', 'android-settings-panel');
        final hint = find.descendant(
          of: find.byKey(const Key('android-settings-panel')),
          matching: find.byKey(const Key('stream-audio-hint')),
        );
        expect(hint, findsOneWidget);
        expect(tester.widget<Text>(hint).data, audioDeviceMissingText);
        expect(find.textContaining('사운드카드'), findsNothing);
        expect(
          find.descendant(
            of: find.byKey(const Key('android-settings-panel')),
            matching: find.textContaining('추가…'),
          ),
          findsNothing,
        );
        expect(find.byKey(const Key('android-session-sound-card')), findsNothing);
        expect(find.byKey(const Key('stream-add-sound-card')), findsNothing);
        expect(find.byKey(const Key('stream-audio-on')), findsNothing);
        expect(find.byKey(const Key('stream-audio-off')), findsNothing);
      } finally {
        await _close(tester);
      }
    },
  );

  testWidgets('With a sound device 화질 has no sound-device line', (
    tester,
  ) async {
    final h = await _open(tester);
    try {
      expect(h.session.remoteAudioMissing, isFalse);
      await _openPanel(tester, 'settings', 'android-settings-panel');
      expect(find.byKey(const Key('stream-audio-hint')), findsNothing);
      expect(find.text(audioDeviceMissingText), findsNothing);
    } finally {
      await _close(tester);
    }
  });

  // The wait for a desktop restart is no longer asked from this view (the
  // sound-card flow that did was removed 2026-11-10): the Host going away
  // with the desktop running starts it. Its limit and its cancel:
  testWidgets(
    'the wait for a Host away with the VM running says so past its limit, and 기다리기 취소 stops at once',
    (tester) async {
      for (final cancel in [false, true]) {
        final broker = _HostReadyBroker()
          ..list = () => _restartDesktop(host: false);
        final h = await _open(tester, awaitHostReturn: _returnWait(broker));
        h.socket.incoming.add(
          jsonEncode({
            'type': 'session.ended',
            'reason': 'host_disconnected',
            'detail': 'host_service_stopping',
          }),
        );
        await tester.pump(const Duration(milliseconds: 50));
        for (
          var i = 0;
          i < 120 &&
              find.byKey(const Key('android-planned-restart')).evaluate().isEmpty;
          i++
        ) {
          await tester.pump(const Duration(seconds: 1));
        }
        expect(
          find.byKey(const Key('android-planned-restart')),
          findsOneWidget,
        );
        if (cancel) {
          await tester.tap(find.byKey(const Key('android-planned-restart')));
          await tester.pump();
          expect(find.textContaining('기다리기를 취소했어요'), findsOneWidget);
        } else {
          for (var i = 0; i < 181; i++) {
            await tester.pump(const Duration(seconds: 1));
          }
          expect(find.textContaining('데스크톱이 3분 안에 준비되지 않았어요'), findsOneWidget);
        }
        expect(find.byKey(const Key('android-retry')), findsOneWidget);
        final tries = h.requested.length;
        await tester.pump(const Duration(minutes: 2));
        expect(h.requested, hasLength(tries), reason: 'no more tries');
        await _close(tester);
      }
    },
  );

  testWidgets(
    'PEN absolute hover and contact use the shown fit while barrel transitions release only pen buttons',
    (tester) async {
      final h = await _open(tester);
      try {
        await _penSignal(tester, const {
          'type': 'ready',
          'protocol': 'pen-signals-v1',
        });
        final rect = tester.getRect(_penPicture);
        final point =
            rect.topLeft + Offset(rect.width * .25, rect.height * .75);
        final before = h.inputs.length;
        await tester.sendEventToBinding(
          PointerHoverEvent(
            kind: PointerDeviceKind.stylus,
            device: 71,
            position: point,
            timeStamp: const Duration(milliseconds: 1),
          ),
        );
        await _penDrain(tester);
        final hover = h.inputs
            .skip(before)
            .where((e) => e['event'] == 'move')
            .last;
        expect(hover['x'], closeTo(.25, .00001));
        expect(hover['y'], closeTo(.75, .00001));
        expect(
          h.inputs.skip(before).where((e) => e['event'] == 'button'),
          isEmpty,
        );
        await tester.sendEventToBinding(
          PointerDownEvent(
            kind: PointerDeviceKind.stylus,
            device: 71,
            pointer: 901,
            position: point,
            buttons: kPrimaryButton,
            timeStamp: const Duration(milliseconds: 2),
          ),
        );
        await tester.sendEventToBinding(
          PointerMoveEvent(
            kind: PointerDeviceKind.stylus,
            device: 71,
            pointer: 901,
            position: rect.bottomRight + const Offset(100, 100),
            buttons: kPrimaryButton | kPrimaryStylusButton,
            timeStamp: const Duration(milliseconds: 3),
          ),
        );
        await tester.sendEventToBinding(
          PointerUpEvent(
            kind: PointerDeviceKind.stylus,
            device: 71,
            pointer: 901,
            position: rect.bottomRight + const Offset(100, 100),
            timeStamp: const Duration(milliseconds: 4),
          ),
        );
        await _penDrain(tester);
        expect(
          h.inputs
              .skip(before)
              .where((e) => e['event'] == 'button')
              .map((e) => (e['button'], e['down']))
              .toList(),
          [('left', true), ('left', false), ('right', true), ('right', false)],
        );
        final barrel = h.inputs
            .skip(before)
            .lastWhere((e) => e['event'] == 'button');
        expect((barrel['x'], barrel['y']), (1.0, 1.0));
        await _penSignal(tester, const {
          'type': 'exit',
          'device': 71,
          'kind': 'stylus',
          'timeMicros': 4500,
        });
        final oldButtons = h.inputs.where((e) => e['event'] == 'button').length;
        await tester.sendEventToBinding(
          PointerDownEvent(
            kind: PointerDeviceKind.stylus,
            device: 72,
            pointer: 902,
            position: Offset(rect.center.dx, rect.top - 10),
            buttons: kPrimaryButton,
            timeStamp: const Duration(milliseconds: 5),
          ),
        );
        await tester.sendEventToBinding(
          PointerUpEvent(
            kind: PointerDeviceKind.stylus,
            device: 72,
            pointer: 902,
            position: Offset(rect.center.dx, rect.top - 10),
            timeStamp: const Duration(milliseconds: 6),
          ),
        );
        await _penDrain(tester);
        expect(
          h.inputs.where((e) => e['event'] == 'button').length,
          oldButtons,
          reason: 'letterbox pen input never becomes a relative finger click',
        );
      } finally {
        await _close(tester);
      }
    },
  );

  testWidgets(
    'PEN absolute coordinates follow the current zoomed picture without applying DPR twice',
    (tester) async {
      final h = await _open(tester, picture: const Size(540, 1200));
      try {
        final a = await tester.startGesture(
          const Offset(150, 400),
          pointer: 911,
        );
        final b = await tester.startGesture(
          const Offset(210, 400),
          pointer: 912,
        );
        await a.moveTo(const Offset(100, 400));
        await b.moveTo(const Offset(260, 400));
        await a.up();
        await b.up();
        await tester.pump();
        final rect = tester.getRect(_penPicture);
        expect(rect.width, greaterThan(360));
        await _penSignal(tester, const {
          'type': 'ready',
          'protocol': 'pen-signals-v1',
        });
        const point = Offset(180, 450);
        final before = h.inputs.length;
        await tester.sendEventToBinding(
          const PointerHoverEvent(
            kind: PointerDeviceKind.stylus,
            device: 76,
            position: point,
            timeStamp: Duration(milliseconds: 1),
          ),
        );
        await _penDrain(tester);
        final move = h.inputs
            .skip(before)
            .lastWhere((e) => e['event'] == 'move');
        expect(move['x'], closeTo((point.dx - rect.left) / rect.width, .00001));
        expect(move['y'], closeTo((point.dy - rect.top) / rect.height, .00001));
        expect(
          h.inputs.skip(before).where((e) => e['event'] == 'button'),
          isEmpty,
        );
      } finally {
        await _close(tester);
      }
    },
  );

  testWidgets(
    'PEN native exit retires hover and held palms cannot become a new two-finger gesture',
    (tester) async {
      final h = await _open(tester);
      try {
        await _penSignal(tester, const {
          'type': 'ready',
          'protocol': 'pen-signals-v1',
        });
        final rect = tester.getRect(_penPicture);
        await tester.sendEventToBinding(
          PointerHoverEvent(
            kind: PointerDeviceKind.stylus,
            device: 73,
            position: rect.center,
            timeStamp: const Duration(milliseconds: 10),
          ),
        );
        await _penDrain(tester);
        final before = h.inputs.length;
        final a = await tester.startGesture(
          rect.center - const Offset(30, 0),
          pointer: 903,
        );
        final b = await tester.startGesture(
          rect.center + const Offset(30, 0),
          pointer: 904,
        );
        await _penSignal(tester, const {
          'type': 'exit',
          'device': 73,
          'kind': 'stylus',
          'timeMicros': 20000,
        });
        await a.moveBy(const Offset(0, -40));
        await b.moveBy(const Offset(0, -40));
        await a.up();
        await b.up();
        await _penDrain(tester);
        expect(
          h.inputs
              .skip(before)
              .where((e) => e['event'] == 'wheel' || e['event'] == 'button'),
          isEmpty,
        );
        await tester.tapAt(rect.center);
        await tester.pump(const Duration(milliseconds: 350));
        expect(
          h.inputs
              .skip(before)
              .where((e) => e['event'] == 'button')
              .map((e) => e['down'])
              .toList(),
          [true, false],
        );
      } finally {
        await _close(tester);
      }
    },
  );

  testWidgets(
    'PEN takeover retires queued pointer work but preserves independent queued keyboard text',
    (tester) async {
      final h = await _open(tester, textInput: true, deferredCapture: true);
      final session = h.session as _DeferredCaptureSession;
      final gate = Completer<void>();
      try {
        await _toggleKeyboard(tester);
        session.gate = gate;
        await session.capture(false);
        final rect = tester.getRect(_penPicture),
            old = rect.topLeft + Offset(rect.width * .2, rect.height * .2);
        final current =
            rect.topLeft + Offset(rect.width * .7, rect.height * .6);
        final before = h.inputs.length;
        await tester.sendEventToBinding(
          PointerDownEvent(
            kind: PointerDeviceKind.mouse,
            device: 2,
            pointer: 905,
            position: old,
            buttons: kPrimaryButton,
            timeStamp: const Duration(milliseconds: 1),
          ),
        );
        await tester.pump();
        expect(session.held, 1);
        tester.testTextInput.updateEditingValue(
          const TextEditingValue(
            text: '${AndroidTypingBridge.sentinel}x',
            selection: TextSelection.collapsed(offset: 3),
          ),
        );
        await tester.pump();
        await tester.sendEventToBinding(
          PointerDownEvent(
            kind: PointerDeviceKind.stylus,
            device: 74,
            pointer: 906,
            position: current,
            buttons: kPrimaryButton,
            timeStamp: const Duration(milliseconds: 2),
          ),
        );
        session.gate = null;
        gate.complete();
        await _penDrain(tester);
        final sent = h.inputs.skip(before).toList();
        expect(h.texts, ['x']);
        expect(
          sent.where(
            (e) =>
                e['event'] == 'button' &&
                e['down'] == true &&
                (e['x'] as num) < .3,
          ),
          isEmpty,
        );
        expect(
          sent.where(
            (e) =>
                e['event'] == 'button' &&
                e['down'] == true &&
                (e['x'] as num) > .6,
          ),
          hasLength(1),
        );
        expect(_keyboardOpen(tester), isTrue);
        await tester.sendEventToBinding(
          PointerUpEvent(
            kind: PointerDeviceKind.stylus,
            device: 74,
            pointer: 906,
            position: current,
            timeStamp: const Duration(milliseconds: 3),
          ),
        );
        await tester.sendEventToBinding(
          PointerUpEvent(
            kind: PointerDeviceKind.mouse,
            device: 2,
            pointer: 905,
            position: old,
            timeStamp: const Duration(milliseconds: 4),
          ),
        );
        await _penDrain(tester);
      } finally {
        if (!gate.isCompleted) gate.complete();
        session.gate = null;
        await _close(tester);
      }
    },
  );

  testWidgets(
    'PEN missing native hover signals falls back to contact and focus loss blocks late pen work',
    (tester) async {
      final h = await _open(tester);
      try {
        final point = tester.getRect(_penPicture).center;
        await tester.sendEventToBinding(
          PointerDownEvent(
            kind: PointerDeviceKind.stylus,
            device: 75,
            pointer: 907,
            position: point,
            buttons: kPrimaryButton,
            timeStamp: const Duration(milliseconds: 1),
          ),
        );
        await tester.sendEventToBinding(
          PointerUpEvent(
            kind: PointerDeviceKind.stylus,
            device: 75,
            pointer: 907,
            position: point,
            timeStamp: const Duration(milliseconds: 2),
          ),
        );
        await _penDrain(tester);
        final before = h.inputs.length;
        await tester.tapAt(point);
        await tester.pump(const Duration(milliseconds: 350));
        expect(
          h.inputs.skip(before).where((e) => e['event'] == 'button'),
          hasLength(2),
        );
        tester.binding.handleAppLifecycleStateChanged(
          AppLifecycleState.inactive,
        );
        await tester.pump();
        final stopped = h.inputs.length;
        await tester.sendEventToBinding(
          PointerHoverEvent(
            kind: PointerDeviceKind.stylus,
            device: 75,
            position: point,
            timeStamp: const Duration(milliseconds: 3),
          ),
        );
        await _penDrain(tester);
        expect(h.inputs.length, stopped);
        tester.binding.handleAppLifecycleStateChanged(
          AppLifecycleState.resumed,
        );
        await tester.pump();
      } finally {
        if (tester.binding.lifecycleState == AppLifecycleState.inactive) {
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.resumed,
          );
        }
        await _close(tester);
      }
    },
  );

  testWidgets(
    'PEN metadata alone and an in-flight resize never grant pen input or replay old contact',
    (tester) async {
      final h = await _open(tester, resizable: true, firstFrame: false);
      Future<bool>? resize;
      try {
        await _penSignal(tester, const {
          'type': 'ready',
          'protocol': 'pen-signals-v1',
        });
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
        for (var n = 0; n < 4; n++) {
          await tester.pump(const Duration(milliseconds: 20));
        }
        expect(h.session.textureId, isNotNull);
        expect(h.session.frameReady, isFalse);
        await tester.sendEventToBinding(
          const PointerDownEvent(
            kind: PointerDeviceKind.stylus,
            device: 77,
            pointer: 913,
            position: Offset(180, 400),
            buttons: kPrimaryButton,
            timeStamp: Duration(milliseconds: 1),
          ),
        );
        await tester.sendEventToBinding(
          const PointerUpEvent(
            kind: PointerDeviceKind.stylus,
            device: 77,
            pointer: 913,
            position: Offset(180, 400),
            timeStamp: Duration(milliseconds: 2),
          ),
        );
        await _penDrain(tester);
        expect(h.inputs, isEmpty);
        h.socket.incoming.add(_video(16667, key: true));
        for (var n = 0; n < 6; n++) {
          await tester.pump(const Duration(milliseconds: 40));
        }
        final point = tester.getRect(_penPicture).center;
        await tester.sendEventToBinding(
          PointerDownEvent(
            kind: PointerDeviceKind.stylus,
            device: 77,
            pointer: 914,
            position: point,
            buttons: kPrimaryButton,
            timeStamp: const Duration(milliseconds: 3),
          ),
        );
        await _penDrain(tester);
        expect(
          h.inputs.where((e) => e['event'] == 'button' && e['down'] == true),
          hasLength(1),
        );
        resize = h.session.resizeViewport(1920, 1080);
        for (var n = 0; n < 4; n++) {
          await tester.pump();
        }
        expect(h.session.resizing, isTrue);
        final before = h.inputs.length;
        await tester.sendEventToBinding(
          PointerMoveEvent(
            kind: PointerDeviceKind.stylus,
            device: 77,
            pointer: 914,
            position: point + const Offset(10, 10),
            buttons: kPrimaryButton,
            timeStamp: const Duration(milliseconds: 4),
          ),
        );
        await tester.sendEventToBinding(
          PointerUpEvent(
            kind: PointerDeviceKind.stylus,
            device: 77,
            pointer: 914,
            position: point,
            timeStamp: const Duration(milliseconds: 5),
          ),
        );
        await _penSignal(tester, const {
          'type': 'exit',
          'device': 77,
          'kind': 'stylus',
          'timeMicros': 6000,
        });
        await _penDrain(tester);
        expect(h.inputs.length, before);
        expect(h.socket.sent.any((e) => e['type'] == 'input.reset'), isTrue);
      } finally {
        await _close(tester);
      }
      expect(await resize, isFalse);
    },
  );

  testWidgets(
    'PEN native capture release blocks hover reacquisition but an explicit contact may capture again',
    (tester) async {
      final h = await _open(tester);
      try {
        await _penSignal(tester, const {
          'type': 'ready',
          'protocol': 'pen-signals-v1',
        });
        await h.session.capture(true);
        expect(h.session.captured, isTrue);
        tester.binding.channelBuffers.push(
          'multitap/remote/input',
          const StandardMethodCodec().encodeSuccessEnvelope({
            'lease': h.native.lease,
            'type': 'capture.released',
          }),
          (_) {},
        );
        await tester.pump();
        await tester.pump();
        expect(h.session.captured, isFalse);
        final captures = h.native.methods
            .where((method) => method == 'capture')
            .length;
        final before = h.inputs.length,
            point = tester.getRect(_penPicture).center;
        await tester.sendEventToBinding(
          PointerHoverEvent(
            kind: PointerDeviceKind.stylus,
            device: 78,
            position: point,
            timeStamp: const Duration(milliseconds: 1),
          ),
        );
        await _penDrain(tester);
        expect(h.session.captured, isFalse);
        expect(
          h.native.methods.where((method) => method == 'capture').length,
          captures,
        );
        expect(h.inputs.length, before);
        await tester.sendEventToBinding(
          PointerDownEvent(
            kind: PointerDeviceKind.stylus,
            device: 78,
            pointer: 915,
            position: point,
            buttons: kPrimaryButton,
            timeStamp: const Duration(milliseconds: 2),
          ),
        );
        await _penDrain(tester);
        expect(h.session.captured, isTrue);
        expect(
          h.native.methods.where((method) => method == 'capture').length,
          captures + 1,
        );
        expect(
          h.inputs
              .skip(before)
              .where((e) => e['event'] == 'button' && e['down'] == true),
          hasLength(1),
        );
        await tester.sendEventToBinding(
          PointerUpEvent(
            kind: PointerDeviceKind.stylus,
            device: 78,
            pointer: 915,
            position: point,
            timeStamp: const Duration(milliseconds: 3),
          ),
        );
        await _penDrain(tester);
      } finally {
        await _close(tester);
      }
    },
  );

  testWidgets(
    'B04 actual workspace fixes wheel distance when only encoded picture halves',
    (tester) async {
      const workspace = Size(2160, 1080);
      final traces = <List<Map<String, dynamic>>>[];
      for (final spec in <(Size, Size?)>[
        (workspace, workspace),
        (const Size(1080, 540), workspace),
        // Legacy/default fixture: source is the encoded picture itself.
        (const Size(1080, 540), null),
      ]) {
        final h = await _open(
          tester,
          physical: const Size(1800, 1200),
          devicePixelRatio: 3,
          picture: spec.$1,
          source: spec.$2,
        );
        try {
          expect(h.session.frameReady, isTrue);
          expect(h.session.resizing, isFalse);
          expect(h.session.closed, isFalse);
          expect(
            (h.session.width, h.session.height),
            (spec.$1.width.toInt(), spec.$1.height.toInt()),
          );
          final source = spec.$2 ?? spec.$1;
          expect(
            (h.session.sourceWidth, h.session.sourceHeight),
            (source.width.toInt(), source.height.toInt()),
          );
          expect(
            find.byKey(const Key('android-remote-surface')),
            findsOneWidget,
          );
          final trace = await _workspaceWheelTrace(tester, h);
          expect(trace, isNotEmpty);
          expect(trace.every((event) => (event['delta'] as int) < 0), isTrue);
          expect(
            h.inputs.where((event) => event['event'] == 'button'),
            isEmpty,
            reason: 'the two-finger scroll never turns into a remote drag',
          );
          expect(
            h.socket.sent.where((event) => event['type'] == 'stream.resize'),
            isEmpty,
          );
          traces.add(trace);
        } finally {
          await _close(tester);
        }
      }
      int total(List<Map<String, dynamic>> trace) =>
          trace.fold(0, (sum, event) => sum + (event['delta'] as int));
      expect(
        total(traces[1]),
        total(traces[0]),
        reason: 'same actual 2160x1080 workspace and finger path must not depend on 2160x1080 versus 1080x540 encoded pixels',
      );
      expect(
        traces[1].map((event) => (event['x'], event['y'])).toList(),
        traces[0].map((event) => (event['x'], event['y'])).toList(),
        reason: 'the workspace coordinate anchor stays the same',
      );
      expect(
        total(traces[2]).abs(),
        lessThan(total(traces[0]).abs()),
        reason: 'legacy source=encoded retains its smaller-workspace behavior',
      );
    },
  );

  testWidgets(
    'B04 workspace scrolling stays gated before first frame and throughout resize',
    (tester) async {
      final h = await _open(
        tester,
        physical: const Size(1800, 1200),
        devicePixelRatio: 3,
        resizable: true,
        firstFrame: false,
      );
      Future<bool>? resize;
      try {
        h.socket.incoming.add(
          jsonEncode({
            'type': 'stream.info',
            'codec': 'h264',
            'width': 1080,
            'height': 540,
            'sourceWidth': 2160,
            'sourceHeight': 1080,
          }),
        );
        for (var turn = 0; turn < 4; turn++) {
          await tester.pump(const Duration(milliseconds: 20));
        }
        expect(h.session.textureId, isNotNull);
        expect((h.session.sourceWidth, h.session.sourceHeight), (2160, 1080));
        expect(
          h.session.frameReady,
          isFalse,
          reason: 'metadata and a native texture are not a first decoded frame',
        );
        expect(await _workspaceWheelTrace(tester, h), isEmpty);
        h.socket.incoming.add(_video(16667, key: true));
        for (var turn = 0; turn < 6; turn++) {
          await tester.pump(const Duration(milliseconds: 40));
        }
        expect(h.session.frameReady, isTrue);
        expect(
          await _workspaceWheelTrace(tester, h),
          isNotEmpty,
          reason:
              'positive control uses the same real view after its first frame',
        );
        resize = h.session.resizeViewport(2560, 1440);
        for (var turn = 0; turn < 4; turn++) {
          await tester.pump();
        }
        expect(h.session.resizing, isTrue);
        expect(
          h.socket.sent.where((event) => event['type'] == 'stream.resize'),
          hasLength(1),
        );
        expect(
          await _workspaceWheelTrace(tester, h),
          isEmpty,
          reason: 'a retained old picture does not grant resize-time input',
        );
      } finally {
        await _close(tester);
      }
      expect(
        await resize,
        isFalse,
        reason: 'closing retires the one pending resize',
      );
    },
  );

  testWidgets(
    'notification disconnect closes its streaming route while paused',
    (tester) async {
      final h = await _open(tester, routed: true);
      final binding = tester.binding;
      addTearDown(() {
        if (binding.lifecycleState == AppLifecycleState.paused) {
          binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
        }
        if (binding.lifecycleState == AppLifecycleState.hidden) {
          binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
        }
        if (binding.lifecycleState == AppLifecycleState.inactive) {
          binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
        }
      });
      binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
      await tester.pump();
      final result = deliverNotificationStop(keepaliveLeases.last);
      await tester.pump();
      await tester.pump(const Duration(seconds: 6));
      expect(h.session.closed, isTrue);
      expect(h.released, hasLength(1));
      expect(await result, isTrue);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pumpAndSettle();
      expect(find.text('list'), findsOneWidget);
      expect(
        h.sockets,
        hasLength(1),
        reason: 'an explicit stop never reconnects',
      );
      await _close(tester);
    },
  );
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });

  testWidgets(
    'TV33 streaming overlay IME keeps D-pad ownership with zero insets',
    (tester) async {
      final h = await _open(
        tester,
        television: true,
        tvNavigation: true,
        textInput: true,
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.contextMenu);
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.enter);
      await tester.pumpAndSettle();
      final field = tester.widget<TextField>(
        find.byKey(const Key('android-remote-keyboard-field')),
      );
      expect(tester.view.viewInsets.bottom, 0);
      expect(tester.testTextInput.isVisible, isTrue);
      expect(field.focusNode!.hasFocus, isTrue);
      final count = h.inputs.length;
      for (final key in [
        LogicalKeyboardKey.arrowUp,
        LogicalKeyboardKey.arrowDown,
        LogicalKeyboardKey.arrowLeft,
        LogicalKeyboardKey.arrowRight,
        LogicalKeyboardKey.select,
        LogicalKeyboardKey.enter,
      ]) {
        expect(
          await tester.sendKeyEvent(key),
          isFalse,
          reason:
              'IME navigation must remain unhandled by Flutter, not swallowed',
        );
        await tester.pump();
        expect(
          field.focusNode!.hasFocus,
          isTrue,
          reason: 'a TV overlay keyboard can own input without resizing the Flutter view',
        );
      }
      expect(h.inputs.length, count);
      // Simulate an InputConnection commit, not a real Android IME.
      tester.testTextInput.enterText('${AndroidTypingBridge.sentinel}가');
      await tester.pumpAndSettle();
      expect(h.texts, ['가']);
      await tester.binding.handlePopRoute();
      await tester.pumpAndSettle();
      expect(field.focusNode!.hasFocus, isFalse);
      final afterTyping = h.inputs.length;
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowRight);
      await tester.pump(const Duration(milliseconds: 50));
      expect(
        h.inputs.skip(afterTyping).any((e) => e['event'] == 'move'),
        isTrue,
        reason: 'Back closes the local editor and restores remote control',
      );
      await _close(tester);
    },
  );

  testWidgets(
    'TV33 streaming native overlay dismissal closes the observed editor after its final commit',
    (tester) async {
      AndroidApplication.imeVisible.value = false;
      addTearDown(() => AndroidApplication.imeVisible.value = false);
      final h = await _open(
        tester,
        television: true,
        tvNavigation: true,
        textInput: true,
      );
      await _toggleKeyboard(tester);
      final field = tester.widget<TextField>(
        find.byKey(const Key('android-remote-keyboard-field')),
      );
      expect(
        field.focusNode!.hasFocus,
        isTrue,
        reason: 'initial native false does not cancel an opening request',
      );
      AndroidApplication.imeVisible.value = true;
      await tester.pump();
      expect(tester.view.viewInsets.bottom, 0);
      AndroidApplication.imeVisible.value = false;
      tester.testTextInput.enterText('${AndroidTypingBridge.sentinel}가');
      await tester.pumpAndSettle();
      expect(h.texts, ['가']);
      expect(
        field.focusNode!.hasFocus,
        isFalse,
        reason: 'native Back can close an overlay IME without changing insets or popping the app',
      );
      expect(find.byType(AndroidKeyRow), findsNothing);
      await _close(tester);
    },
  );

  testWidgets(
    'TV streaming D-pad clicks and local menu leave IME and disconnect confirmation in control',
    (tester) async {
      final h = await _open(tester, television: true);
      await tester.pump();
      await tester.sendKeyDownEvent(LogicalKeyboardKey.arrowRight);
      await tester.pump(const Duration(milliseconds: 70));
      await tester.sendKeyUpEvent(LogicalKeyboardKey.arrowRight);
      await tester.sendKeyEvent(LogicalKeyboardKey.select);
      await tester.pump(const Duration(milliseconds: 40));
      expect(
        h.inputs.any((e) => e['event'] == 'move' && (e['x'] as num) > .5),
        isTrue,
      );
      expect(h.sequence, ['L↓', 'L↑']);
      await tester.sendKeyDownEvent(LogicalKeyboardKey.select);
      await tester.pump(const Duration(milliseconds: 600));
      await tester.sendKeyUpEvent(LogicalKeyboardKey.select);
      await tester.pump(const Duration(milliseconds: 40));
      expect(h.sequence, ['L↓', 'L↑', 'R↓', 'R↑']);
      await tester.sendKeyEvent(LogicalKeyboardKey.contextMenu);
      await tester.pumpAndSettle();
      expect(find.text('원격 화면 메뉴'), findsOneWidget);
      final count = h.inputs.length;
      await tester.sendKeyEvent(LogicalKeyboardKey.enter);
      await tester.pumpAndSettle();
      final field = tester.widget<TextField>(
        find.byKey(const Key('android-remote-keyboard-field')),
      );
      expect(field.focusNode!.hasFocus, isTrue);
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowRight);
      await tester.pump(const Duration(milliseconds: 100));
      expect(
        h.inputs.length,
        count,
        reason: 'TV navigation belongs to the local IME',
      );
      await tester.binding.handlePopRoute();
      await tester.pumpAndSettle();
      await tester.binding.handlePopRoute();
      await tester.pumpAndSettle();
      expect(find.text('원격 화면 메뉴'), findsOneWidget);
      await tester.tap(find.text('연결 끊기'));
      await tester.pumpAndSettle();
      expect(find.text('연결을 끊을까요?'), findsOneWidget);
      expect(h.session.closed, isFalse);
      await tester.tap(find.text('취소'));
      await tester.pumpAndSettle();
      await _close(tester);
    },
  );

  testWidgets(
    'TV32 streaming tools reuse settings and shortcuts, drag resets on Menu and scroll reaches the wire',
    (tester) async {
      final h = await _open(
        tester,
        television: true,
        settings: const AndroidSessionSettings(
          helpSeen: true,
          touch: AndroidTouchMode.direct,
        ),
      );
      Future<void> choose(String key) async {
        await tester.sendKeyEvent(LogicalKeyboardKey.contextMenu);
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(Key(key)));
        await tester.pumpAndSettle();
      }

      await choose('android-tv-menu-screen');
      expect(find.byKey(const Key('android-screen-panel')), findsOneWidget);
      await tester.ensureVisible(
        find.byKey(const Key('android-speed-precise')),
      );
      await tester.tap(find.byKey(const Key('android-speed-precise')));
      await tester.pump();
      expect(h.saved.last.speed, AndroidPointerSpeed.precise);
      await tester.binding.handlePopRoute();
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('android-screen-panel')), findsNothing);
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowRight);
      await tester.pump(const Duration(milliseconds: 50));
      final first = h.inputs.lastWhere((e) => e['event'] == 'move');
      expect((first['x'] as num).toDouble() - .5, closeTo(1 / 1280, .00001));
      await choose('android-tv-menu-shortcuts');
      await tester.tap(find.text('전체 선택  Ctrl+A'));
      await tester.pumpAndSettle();
      expect(h.keys, ['a2↓', '41↓', '41↑', 'a2↑']);
      await choose('android-tv-mode-drag');
      await tester.sendKeyEvent(LogicalKeyboardKey.select);
      await tester.sendKeyDownEvent(LogicalKeyboardKey.arrowRight);
      await tester.pump(const Duration(milliseconds: 120));
      await tester.sendKeyUpEvent(LogicalKeyboardKey.arrowRight);
      final events = h.inputs,
          down = events.indexWhere(
            (e) => e['event'] == 'button' && e['down'] == true,
          );
      expect(events.skip(down + 1).any((e) => e['event'] == 'move'), isTrue);
      final before = h.socket.sent.length;
      await tester.sendKeyEvent(LogicalKeyboardKey.contextMenu);
      await tester.pumpAndSettle();
      expect(
        h.socket.sent.skip(before).any((e) => e['type'] == 'input.reset'),
        isTrue,
      );
      await tester.tap(find.byKey(const Key('android-tv-mode-scroll')));
      await tester.pumpAndSettle();
      final count = h.inputs.length;
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowLeft);
      await tester.pump(const Duration(milliseconds: 50));
      expect(h.inputs.length, count);
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await tester.pump(const Duration(milliseconds: 50));
      expect(h.inputs.last['event'], 'wheel');
      expect(h.inputs.last['delta'], -120);
      await _close(tester);
    },
  );

  testWidgets(
    'TV32 cancelling a drag while capture awaits cannot send its late mouse down',
    (tester) async {
      final h = await _open(tester, television: true, deferredCapture: true);
      final session = h.session as _DeferredCaptureSession;
      session.gate = Completer<void>();
      await tester.sendKeyEvent(LogicalKeyboardKey.contextMenu);
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('android-tv-mode-drag')));
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.select);
      await tester.pump();
      expect(session.held, greaterThan(0));
      expect(
        h.inputs.where((e) => e['event'] == 'button' && e['down'] == true),
        isEmpty,
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.contextMenu);
      await tester.pumpAndSettle();
      expect(session.captured, isFalse);
      session.gate!.complete();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));
      expect(
        h.inputs.where((e) => e['event'] == 'button' && e['down'] == true),
        isEmpty,
      );
      expect(h.socket.sent.any((e) => e['type'] == 'input.reset'), isTrue);
      await _close(tester);
    },
  );

  testWidgets('a tap clicks there; ⋯ opens the menu, a touch outside closes '
      'it without reaching the remote', (tester) async {
    final h = await _open(tester);
    expect(find.byKey(const Key('expand-session-toolbar')), findsOneWidget);
    expect(find.byKey(const Key('session-command-bar')), findsNothing);
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    final menu = tester.getRect(find.byKey(const Key('session-command-bar')));
    // Touching the picture beside the menu closes it and reaches nothing.
    final outside = Offset(
      menu.center.dx,
      menu.bottom + 40 < 800 ? menu.bottom + 30 : menu.top - 30,
    );
    expect(menu.contains(outside), isFalse);
    await tester.tapAt(outside);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('session-command-bar')), findsNothing);
    expect(find.text('연결을 끊을까요?'), findsNothing);
    expect(h.inputs.where((m) => m['event'] == 'button'), isEmpty);
    // Closed, a tap clicks (at the pointer, after the double-tap wait).
    await tester.tapAt(const Offset(180, 400));
    await tester.pump(const Duration(milliseconds: 500));
    await tester.pumpAndSettle();
    final buttons = [
      for (final m in h.inputs)
        if (m['event'] == 'button') m,
    ];
    expect(buttons.map((m) => '${m['button']}:${m['down']}'), [
      'left:true',
      'left:false',
    ]);
    expect(buttons.first['x'], closeTo(.5, .01));
    expect(buttons.first['y'], closeTo(.5, .01));
    await _close(tester);
  });

  testWidgets('one amber cluster on the right edge holds 도구, 입력 방식 and '
      '키보드 (at the bottom); the keyboard opens and closes in one tap '
      'without remote touches', (tester) async {
    final h = await _open(tester, picture: const Size(540, 1200));
    final keyboard = find.byKey(const Key('android-remote-keyboard-button'));
    final mode = find.byKey(const Key('android-remote-touch-mode-button'));
    final tools = find.byKey(const Key('expand-session-toolbar'));
    for (final button in [keyboard, mode, tools]) {
      expect(button, findsOneWidget);
      expect(tester.getSize(button).width, greaterThanOrEqualTo(48));
      expect(tester.getSize(button).height, greaterThanOrEqualTo(47.9));
    }
    final cluster = tester.getRect(_cluster);
    expect(cluster.right, closeTo(360 - 8, .01));
    expect(cluster.height, closeTo(144, .01));
    // Top to bottom: the menu, 입력 방식, then 키보드 at the end.
    expect(tester.getRect(tools).top, closeTo(cluster.top, .01));
    expect(
      tester.getRect(mode).top,
      closeTo(tester.getRect(tools).bottom, .01),
    );
    expect(tester.getRect(keyboard).bottom, closeTo(cluster.bottom, .01));
    final sent = h.inputs.length;
    await _toggleKeyboard(tester);
    expect(_keyboardOpen(tester), isTrue);
    expect(find.byKey(const Key('session-command-bar')), findsNothing);
    expect(h.inputs.length, sent);
    tester.view.viewInsets = const FakeViewPadding(bottom: 900);
    await tester.pumpAndSettle();
    // Above the phone keyboard and its key row, at the same proportion,
    // with 화면 이동 beside it towards the picture.
    final rect = tester.getRect(_cluster);
    expect(rect.bottom, lessThanOrEqualTo(800 - 300 - 48));
    final handle = find.byKey(const Key('android-session-move-handle'));
    expect(handle, findsOneWidget);
    expect(tester.getRect(handle).right, closeTo(rect.left - 8, .01));
    expect(tester.getRect(handle).center.dy, closeTo(rect.center.dy, .01));
    await _toggleKeyboard(tester);
    tester.view.viewInsets = FakeViewPadding.zero;
    await tester.pumpAndSettle();
    expect(_keyboardOpen(tester), isFalse);
    expect(h.inputs.length, sent);
    await _close(tester);
  });

  testWidgets('the cluster keeps to the safe area and above the keyboard; '
      'too little room keeps the key-row fallback', (tester) async {
    await _open(
      tester,
      physical: const Size(2400, 1080),
      picture: const Size(2400, 1080),
    );
    final bubble = _cluster;
    tester.view.viewPadding = const FakeViewPadding(bottom: 90, right: 24);
    await tester.pumpAndSettle();
    // 800 × 360 dp, 30 dp at the bottom and 8 at the right kept clear.
    expect(tester.getRect(bubble).right, closeTo(800 - 8 - 8, .01));
    expect(tester.getRect(bubble).bottom, lessThanOrEqualTo(360 - 30 - 8));
    expect(tester.getRect(bubble).height, closeTo(144, .01));
    await _toggleKeyboard(tester);
    tester.view.viewInsets = const FakeViewPadding(bottom: 780);
    await tester.pumpAndSettle();
    // 52 dp above the key row: the cluster lies across the strip at the
    // right, 화면 이동 beside it.
    final rect = tester.getRect(bubble);
    expect(rect.width, closeTo(144, .01));
    expect(rect.right, closeTo(800 - 8 - 8, .01));
    expect(rect.top, greaterThanOrEqualTo(0));
    expect(rect.bottom, lessThanOrEqualTo(52));
    final handle = find.byKey(const Key('android-session-move-handle'));
    expect(handle, findsOneWidget);
    expect(tester.getRect(handle).right, closeTo(rect.left - 8, .01));
    tester.view.viewInsets = const FakeViewPadding(bottom: 930);
    await tester.pumpAndSettle();
    expect(bubble, findsNothing);
    expect(handle, findsNothing);
    // No room for the bubble: the key row's hide key (or Back) brings it back.
    expect(find.byKey(const Key('android-key-hide')), findsOneWidget);
    await tester.tap(find.byKey(const Key('android-key-hide')));
    tester.view.viewInsets = FakeViewPadding.zero;
    await tester.pumpAndSettle();
    expect(bubble, findsOneWidget);
    expect(_keyboardOpen(tester), isFalse);
    await _close(tester);
  });

  testWidgets('the 입력 방식 button switches 터치 and 포인터 in one tap, shows '
      'which, and saves it without clicking the remote', (tester) async {
    final h = await _open(tester);
    final mode = find.byKey(const Key('android-remote-touch-mode-button'));
    final sent = h.inputs.length;
    expect(
      find.descendant(of: mode, matching: find.text('포인터')),
      findsOneWidget,
    );
    await tester.tap(mode);
    await tester.pumpAndSettle();
    expect(h.saved.last.touch, AndroidTouchMode.direct);
    expect(
      find.descendant(of: mode, matching: find.text('터치')),
      findsOneWidget,
    );
    await tester.tap(mode);
    await tester.pumpAndSettle();
    expect(h.saved.last.touch, AndroidTouchMode.trackpad);
    expect(h.inputs.length, sent);
    expect(find.byKey(const Key('session-command-bar')), findsNothing);
    await _close(tester);
  });

  for (final (television, physical, dpr) in const [
    (false, Size(1080, 2400), 3.0),
    (false, Size(2560, 1600), 2.0),
    (true, Size(1920, 1080), 2.0),
  ]) {
    testWidgets(
      'zoom buttons are TV-only; phone/tablet gestures and fit stay available at $physical TV=$television',
      (tester) async {
        final h = await _open(
          tester,
          television: television,
          physical: physical,
          devicePixelRatio: dpr,
        );
        try {
          final requests = h.requested.length;
          final source = (h.session.sourceWidth, h.session.sourceHeight);
          if (television) {
            await tester.sendKeyEvent(LogicalKeyboardKey.contextMenu);
            await tester.pumpAndSettle();
            await tester.tap(find.byKey(const Key('android-tv-menu-screen')));
            await tester.pumpAndSettle();
          } else {
            await _openScreenPanel(tester);
          }
          for (final key in [
            'android-screen-zoom',
            'android-screen-zoom-in',
            'android-screen-zoom-out',
          ]) {
            expect(
              find.byKey(Key(key)),
              television ? findsOneWidget : findsNothing,
              reason: key,
            );
          }
          final fitted = tester.getSize(_penPicture);
          if (television) {
            await tester.ensureVisible(find.byKey(const Key('android-screen-zoom-in')));
            await tester.tap(find.byKey(const Key('android-screen-zoom-in')));
            await tester.pumpAndSettle();
          } else {
            await _pinchPicture(tester);
          }
          expect(tester.getSize(_penPicture).width, greaterThan(fitted.width));
          if (!television) await _openScreenPanel(tester);
          final fit = find.byKey(const Key('android-screen-fit'));
          await tester.ensureVisible(fit);
          await tester.tap(fit);
          await tester.pumpAndSettle();
          expect(tester.getSize(_penPicture).width, closeTo(fitted.width, .01));
          expect((h.session.sourceWidth, h.session.sourceHeight), source);
          expect(h.requested, hasLength(requests), reason: 'local zoom does not reconnect or resize the Host');
          expect(tester.takeException(), isNull);
        } finally {
          await _close(tester);
        }
      },
    );
  }

  testWidgets('on a narrow view the zoom chip stays off the dock and the '
      'bubble; the screen menu still fits the picture', (tester) async {
    await _open(tester);
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('android-session-screen')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-screen-zoom-in')), findsNothing);
    expect(find.byKey(const Key('android-screen-zoom-out')), findsNothing);
    expect(find.byKey(const Key('android-screen-zoom')), findsNothing);
    await _pinchPicture(tester);
    await _openScreenPanel(tester);
    // Under the open dock the chip is not shown.
    expect(find.byKey(const Key('android-zoom-chip')), findsNothing);
    tester.view.physicalSize = const Size(540, 2400);
    await tester.pumpAndSettle();
    // The viewport change fits a new local view. Enlarge that new view
    // before asserting the chip, so a zoom reset cannot pass this.
    await _openScreenPanel(tester);
    await tester.tap(find.byKey(const Key('android-screen-fit')));
    await tester.pumpAndSettle();
    final picture = find.byWidgetPredicate(
      (w) => w is ColoredBox && w.color == const Color(0xFF2B4C6F),
    );
    final fitted = tester.getSize(picture);
    await _pinchPicture(tester);
    expect(tester.getSize(picture).width, greaterThan(fitted.width));
    await _openScreenPanel(tester);
    expect(find.byKey(const Key('android-zoom-chip')), findsNothing);
    await _foldTools(tester);
    // Folded, the chip is back where it does not touch the bubble.
    final chip = find.byKey(const Key('android-zoom-chip'));
    final bubble = _cluster;
    expect(bubble, findsOneWidget);
    if (chip.evaluate().isNotEmpty) {
      expect(tester.getRect(chip).overlaps(tester.getRect(bubble)), isFalse);
    }
    // The existing screen panel still owns the same fit action.
    await _openScreenPanel(tester);
    await tester.tap(find.byKey(const Key('android-screen-fit')));
    await tester.pumpAndSettle();
    expect(tester.getSize(picture).width, closeTo(fitted.width, .01));
    expect(find.byKey(const Key('android-zoom-chip')), findsNothing);
    await _close(tester);
  });

  testWidgets('large bold zoom labels never overlap the bubble or the move '
      'handle across width boundaries', (tester) async {
    await _open(
      tester,
      physical: const Size(1800, 2400),
      textScaler: TextScaler.linear(1.5),
      boldText: true,
    );
    var shown = false;
    for (final width in [600.0, 480.0, 360.0, 320.0, 280.0, 240.0]) {
      if (_keyboardOpen(tester)) await _toggleKeyboard(tester);
      tester.view.viewInsets = FakeViewPadding.zero;
      tester.view.physicalSize = Size(width * 3, 2400);
      await tester.pumpAndSettle();
      await _openScreenPanel(tester);
      // A real viewport change resets local zoom. Fit then zoom explicitly
      // for every width, rather than mistaking that reset for chip hiding.
      await tester.tap(find.byKey(const Key('android-screen-fit')));
      await tester.pumpAndSettle();
      final picture = find.byWidgetPredicate(
        (w) => w is ColoredBox && w.color == const Color(0xFF2B4C6F),
      );
      final fitted = tester.getSize(picture);
      await _pinchPicture(tester);
      expect(tester.getSize(picture).width, greaterThan(fitted.width));
      await _foldTools(tester);
      await _toggleKeyboard(tester);
      tester.view.viewInsets = const FakeViewPadding(bottom: 900);
      await tester.pumpAndSettle();
      expect(_keyboardOpen(tester), isTrue);
      final move = find.byKey(const Key('android-session-move-handle'));
      expect(move, findsOneWidget);
      final bubble = _cluster;
      expect(bubble, findsOneWidget);
      expect(tester.getSize(picture).width, greaterThan(fitted.width));
      final chip = find.byKey(const Key('android-zoom-chip'));
      if (chip.evaluate().isEmpty) continue;
      shown = true;
      final bounds = tester.getRect(chip);
      expect(
        bounds.overlaps(tester.getRect(bubble)),
        isFalse,
        reason: 'zoom chip versus the bubble at $width dp',
      );
      expect(
        bounds.overlaps(tester.getRect(move)),
        isFalse,
        reason: 'zoom chip versus move handle at $width dp',
      );
    }
    expect(shown, isTrue);
    await _close(tester);
  });

  testWidgets('a bubble resting on the bottom edge hides the zoom chip it '
      'would cover', (tester) async {
    await _open(
      tester,
      settings: const AndroidSessionSettings(
        helpSeen: true,
        toolbarEdge: AndroidBubbleEdge.bottom,
        toolbar: .5,
        screens: {
          AndroidDeviceClass.phone: AndroidScreenChoice(
            resolution: AndroidResolution.device,
          ),
        },
      ),
    );
    await _openScreenPanel(tester);
    await _pinchPicture(tester);
    await _foldTools(tester);
    expect(find.byKey(const Key('expand-session-toolbar')), findsOneWidget);
    expect(find.byKey(const Key('android-zoom-chip')), findsNothing);
    await _close(tester);
  });

  testWidgets('the menu leaves 키보드 and 입력 방식 to the cluster beside it, '
      'still one tap while the menu is open', (tester) async {
    final h = await _open(tester);
    final sent = h.inputs.length;
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    final menu = find.byKey(const Key('session-command-bar'));
    for (final label in ['키보드', '입력 방식']) {
      expect(
        find.descendant(of: menu, matching: find.text(label)),
        findsNothing,
      );
    }
    await tester.tap(find.byKey(const Key('android-remote-touch-mode-button')));
    await tester.pumpAndSettle();
    expect(h.saved.last.touch, AndroidTouchMode.direct);
    expect(menu, findsOneWidget, reason: 'the menu stays open');
    expect(h.inputs.length, sent);
    await _close(tester);
  });

  testWidgets('the menu is the PC\'s bar as rows, without captions: 연결 품질, '
      '화면, 설정 and 연결 끊기 last; 설정 holds 화질, 입력 방식 and '
      '사용안내', (tester) async {
    await _open(tester);
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    final menu = find.byKey(const Key('session-command-bar'));
    expect(
      [
        for (final row in tester.widgetList<AndroidBarMenuRow>(
          find.descendant(of: menu, matching: find.byType(AndroidBarMenuRow)),
        ))
          row.item.label,
      ],
      ['연결 품질', '화면', '설정', '연결 끊기'],
    );
    // 화면 and 설정 are rows that are their groups: no caption over them.
    expect(
      find.descendant(of: menu, matching: find.byType(AndroidMenuCaption)),
      findsNothing,
    );
    expect(
      find.descendant(of: menu, matching: find.text('화면')),
      findsOneWidget,
    );
    expect(
      find.descendant(of: menu, matching: find.text('설정')),
      findsOneWidget,
    );
    // The card names the desktop; the latency chip left the title row.
    expect(
      tester.widget<Text>(find.byKey(const Key('session-menu-title'))).data,
      'TESTER · 스트리밍',
    );
    for (final gone in ['화면·영상', '내 기기 공유', '입력·도움말', '스트리밍 설정']) {
      expect(find.text(gone), findsNothing, reason: gone);
    }
    // Each row says what its panel holds.
    for (final (key, texts) in [
      ('session-quality', ['연결 품질']),
      ('android-session-screen', ['화면', '보기 · 배율 · 해상도']),
      ('android-session-more', ['설정', '화질 · 입력 · 사용안내']),
      ('session-disconnect', ['연결 끊기']),
    ]) {
      for (final text in texts) {
        expect(
          find.descendant(of: find.byKey(Key(key)), matching: find.text(text)),
          findsOneWidget,
          reason: '$key · $text',
        );
      }
    }
    expect(find.byTooltip('화면 · 보기, 배율, 해상도'), findsOneWidget);
    expect(
      find.byTooltip('설정 · 화질, 마우스 잠금, 입력 방식, 사용안내'),
      findsOneWidget,
    );
    // What was a row of the card is inside 화면 or 설정 now.
    for (final id in ['scale', 'settings', 'game-mouse', 'input', 'help']) {
      expect(find.byKey(Key('android-session-$id')), findsNothing, reason: id);
    }
    Rect rect(String key) => tester.getRect(find.byKey(Key(key)));
    // The three follow each other at once; 연결 끊기 after a hairline.
    expect(
      rect('android-session-screen').top,
      closeTo(rect('session-quality').bottom, .01),
    );
    expect(
      rect('android-session-more').top,
      closeTo(rect('android-session-screen').bottom, .01),
    );
    expect(
      rect('session-disconnect').top,
      greaterThan(rect('android-session-more').bottom),
    );

    // 설정: the PC's 설정 panel in the card's place. This connection has no
    // 마우스 잠금 grant: 화질, 입력 방식, 사용안내.
    await tester.tap(find.byKey(const Key('android-session-more')));
    await tester.pumpAndSettle();
    expect(menu, findsNothing);
    final more = find.byKey(const Key('android-more-panel'));
    expect(more, findsOneWidget);
    expect(
      find.descendant(
        of: more,
        matching: find.byKey(const Key('session-settings-panel')),
      ),
      findsOneWidget,
    );
    expect(find.byKey(const Key('android-session-game-mouse')), findsNothing);
    expect(find.byKey(const Key('pin-session-toolbar')), findsNothing);
    for (final (id, title) in [
      ('settings', '화질'),
      ('input', '입력 방식'),
      ('help', '사용안내'),
    ]) {
      expect(
        find.descendant(
          of: find.byKey(Key('android-session-$id')),
          matching: find.text(title),
        ),
        findsOneWidget,
        reason: id,
      );
      expect(
        rect('android-session-$id').height,
        greaterThanOrEqualTo(48),
        reason: id,
      );
    }
    expect(
      find.descendant(
        of: find.byKey(const Key('android-session-settings')),
        matching: find.textContaining(' fps'),
      ),
      findsOneWidget,
    );
    expect(
      find.descendant(
        of: find.byKey(const Key('android-session-help')),
        matching: find.text('터치 조작 · 프린터 · 복사'),
      ),
      findsOneWidget,
    );
    expect(
      rect('android-session-input').top,
      greaterThanOrEqualTo(rect('android-session-settings').bottom - .01),
    );
    expect(
      rect('android-session-help').top,
      greaterThan(rect('android-session-input').bottom),
    );
    await _close(tester);
  });

  testWidgets('a dragged bubble is remembered for the next session', (
    tester,
  ) async {
    final h = await _open(tester);
    final bubble = find.byKey(const Key('expand-session-toolbar'));
    final sent = h.inputs.length;
    await tester.dragFrom(tester.getCenter(bubble), const Offset(-300, -200));
    await tester.pumpAndSettle();
    expect(h.saved.last.toolbarEdge, AndroidBubbleEdge.left);
    expect(tester.getRect(bubble).left, closeTo(8, .01));
    expect(h.inputs.length, sent, reason: 'the drag never reached the remote');
    expect(find.byKey(const Key('session-command-bar')), findsNothing);
    await _close(tester);
  });
  testWidgets('resize-time IME edits reset locally without a later deletion '
      'backspacing remote text; normal composition still works', (
    tester,
  ) async {
    final h = await _open(
      tester,
      textInput: true,
      resizable: true,
      picture: const Size(540, 1200),
    );
    await _toggleKeyboard(tester);
    const s = AndroidTypingBridge.sentinel;
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '${s}z',
        selection: TextSelection.collapsed(offset: 3),
      ),
    );
    await tester.pumpAndSettle();
    expect(h.texts, ['z']);
    final change = h.session.resizeViewport(1346, 812);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 20));
    expect(h.session.resizing, isTrue);
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '${s}za',
        selection: TextSelection.collapsed(offset: 4),
        composing: TextRange(start: 3, end: 4),
      ),
    );
    await tester.pump();
    final field = tester.widget<TextField>(
      find.byKey(const Key('android-remote-keyboard-field')),
    );
    expect(field.controller!.value, AndroidTypingBridge.initial);
    expect(h.texts, ['z']);
    expect(find.textContaining('화면 크기를 맞추는 중이라 입력을 보내지 못했어요'), findsOneWidget);
    final id = h.socket.sent.lastWhere(
      (m) => m['type'] == 'stream.resize',
    )['requestId'];
    h.socket.incoming.add(
      jsonEncode({
        'type': 'stream.resize.failed',
        'requestId': id,
        'unchanged': true,
        'reason': 'unsupported_display_mode',
      }),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 20));
    expect(await change, isFalse);
    // An IME deletion returning to the reset field must not delete remote z.
    tester.testTextInput.updateEditingValue(AndroidTypingBridge.initial);
    await tester.pumpAndSettle();
    expect(h.keys.where((k) => k.startsWith('8')), isEmpty);
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '${s}b',
        selection: TextSelection.collapsed(offset: 3),
        composing: TextRange(start: 2, end: 3),
      ),
    );
    await tester.pumpAndSettle();
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '${s}bc',
        selection: TextSelection.collapsed(offset: 4),
        composing: TextRange(start: 3, end: 4),
      ),
    );
    await tester.pumpAndSettle();
    expect(h.texts, ['z', 'b', 'c']);
    expect(h.keys.where((k) => k.startsWith('8')), isEmpty);
    await _close(tester);
  });

  testWidgets('phone typing, a sticky Ctrl and the key row send keys', (
    tester,
  ) async {
    final h = await _open(tester);
    await _toggleKeyboard(tester);
    expect(find.byKey(const Key('android-key-row')), findsOneWidget);
    const s = AndroidTypingBridge.sentinel;
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '${s}a',
        selection: TextSelection.collapsed(offset: 3),
      ),
    );
    await tester.pumpAndSettle();
    expect(h.keys, ['41↓', '41↑']);
    // Ctrl held for the next key only.
    await tester.tap(find.byKey(const Key('android-key-ctrl')));
    await tester.pump();
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '${s}ac',
        selection: TextSelection.collapsed(offset: 4),
      ),
    );
    await tester.pumpAndSettle();
    expect(h.keys.skip(2), ['a2↓', '43↓', '43↑', 'a2↑']);
    await tester.tap(find.byKey(const Key('android-key-escape')));
    await tester.pumpAndSettle();
    expect(h.keys.skip(6), ['1b↓', '1b↑']);
    // Nothing is left pressed: every down has its up.
    final held = <String>{};
    for (final k in h.keys) {
      final code = k.substring(0, k.length - 1);
      k.endsWith('↓') ? held.add(code) : held.remove(code);
    }
    expect(held, isEmpty);
    await tester.tap(find.byKey(const Key('android-key-hide')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-key-row')), findsNothing);
    await _close(tester);
  });

  testWidgets('the phone keyboard never changes the remote resolution', (
    tester,
  ) async {
    // 2400 rows exceed a stream's 2160: the device's size is the exact
    // half, 540×1200 (drawn at 2 device pixels per remote pixel).
    final h = await _open(
      tester,
      resizable: true,
      picture: const Size(540, 1200),
    );
    await tester.pump(const Duration(seconds: 1));
    final before = h.socket.sent.where((m) => m['type'] == 'stream.resize');
    expect(before, isEmpty, reason: 'already the device size');
    tester.view.viewInsets = const FakeViewPadding(bottom: 900);
    await tester.pump(const Duration(seconds: 1));
    expect(h.socket.sent.where((m) => m['type'] == 'stream.resize'), isEmpty);
    tester.view.viewInsets = FakeViewPadding.zero;
    // Rotation is a real change: the remote follows the new shape.
    tester.view.physicalSize = const Size(2400, 1080);
    await tester.pump();
    await tester.pump(const Duration(seconds: 1));
    final resize = h.socket.sent.where((m) => m['type'] == 'stream.resize');
    expect(resize, hasLength(1));
    expect((resize.single['width'], resize.single['height']), (2400, 1080));
    await _close(tester);
  });

  testWidgets('background and transient return geometry never resize the host; '
      'a stable rotation still does', (tester) async {
    final h = await _open(
      tester,
      resizable: true,
      picture: const Size(540, 1200),
    );
    await tester.pump(const Duration(seconds: 1));
    final binding = tester.binding;
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    tester.view.physicalSize = const Size(1080, 2100);
    await tester.pump(const Duration(seconds: 2));
    expect(h.socket.sent.where((m) => m['type'] == 'stream.resize'), isEmpty);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    tester.view.physicalSize = const Size(1080, 2400);
    await tester.pump();
    await tester.pump(const Duration(seconds: 2));
    expect(h.socket.sent.where((m) => m['type'] == 'stream.resize'), isEmpty);
    expect(h.requested, hasLength(1));
    tester.view.physicalSize = const Size(2400, 1080);
    await tester.pump();
    await tester.pump(const Duration(seconds: 1));
    final resize = h.socket.sent.where((m) => m['type'] == 'stream.resize');
    expect(resize, hasLength(1));
    expect((resize.single['width'], resize.single['height']), (2400, 1080));
    await _close(tester);
  });

  // The Windows 모든 모니터 minimize bug (2026-11-02: a minimized window's
  // 0x0 size read as a monitor change and refreshed the session), on the
  // phone: Android may report an empty surface while the app is away.
  testWidgets('an empty surface while away is no change: picture and input '
      'come back without a reconnect or resize', (tester) async {
    final h = await _open(
      tester,
      resizable: true,
      picture: const Size(540, 1200),
    );
    await tester.pump(const Duration(seconds: 1));
    final binding = tester.binding;
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    tester.view.physicalSize = Size.zero;
    await tester.pump(const Duration(seconds: 2));
    tester.view.physicalSize = const Size(1080, 2400);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump();
    await tester.pump(const Duration(seconds: 2));
    expect(tester.takeException(), isNull);
    expect(h.requested, hasLength(1), reason: 'no reconnect');
    expect(h.socket.sent.where((m) => m['type'] == 'stream.resize'), isEmpty);
    expect(find.byKey(const Key('android-remote-surface')), findsOneWidget);
    expect(find.byKey(const Key('android-session-panel')), findsNothing);
    final before = h.inputs.length;
    await tester.tap(find.byKey(const Key('android-remote-surface')));
    await tester.pump(const Duration(milliseconds: 400));
    expect(
      h.inputs.skip(before).where((event) => event['event'] == 'button'),
      isNotEmpty,
      reason: 'a tap reaches the remote again',
    );
    await _close(tester);
  });

  testWidgets('disconnect confirmation suspends a pending resize and cancel '
      'reschedules it', (tester) async {
    final h = await _open(
      tester,
      resizable: true,
      picture: const Size(540, 1200),
    );
    await tester.pump(const Duration(seconds: 1));
    tester.view.physicalSize = const Size(2400, 1080);
    await tester.pump();
    await tester.binding.handlePopRoute();
    await tester.pumpAndSettle();
    expect(find.text('연결을 끊을까요?'), findsOneWidget);
    await tester.pump(const Duration(seconds: 1));
    expect(h.socket.sent.where((m) => m['type'] == 'stream.resize'), isEmpty);
    await tester.tap(find.widgetWithText(TextButton, '취소'));
    await tester.pumpAndSettle();
    await tester.pump(const Duration(seconds: 1));
    expect(
      h.socket.sent.where((m) => m['type'] == 'stream.resize'),
      hasLength(1),
    );
    expect(h.requested, hasLength(1));
    await _close(tester);
  });

  testWidgets('cancelling disconnect after resume settling expired behind '
      'the dialog restarts settling without reconnecting', (tester) async {
    final h = await _open(
      tester,
      resizable: true,
      picture: const Size(540, 1200),
    );
    await tester.pump(const Duration(seconds: 1));
    final binding = tester.binding;
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    tester.view.physicalSize = const Size(2400, 1080);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump();
    await binding.handlePopRoute();
    await tester.pumpAndSettle();
    await tester.pump(const Duration(seconds: 1));
    expect(find.text('연결을 끊을까요?'), findsOneWidget);
    expect(h.socket.sent.where((m) => m['type'] == 'stream.resize'), isEmpty);
    await tester.tap(find.widgetWithText(TextButton, '취소'));
    await tester.pumpAndSettle();
    // Settle and resize each have their own 650 ms debounce. Separate pumps
    // let the second timer start at the first timer's actual completion.
    await tester.pump(const Duration(milliseconds: 700));
    await tester.pump(const Duration(milliseconds: 700));
    final requests = h.socket.sent.where((m) => m['type'] == 'stream.resize');
    expect(requests, hasLength(1));
    expect((requests.single['width'], requests.single['height']), (2400, 1080));
    expect(h.requested, hasLength(1));
    expect(h.sockets, hasLength(1));
    await _close(tester);
  });

  testWidgets('without display-scale-v1 a readable scale is an exact pixel '
      'fraction, drawn sharp', (tester) async {
    final h = await _open(
      tester,
      settings: const AndroidSessionSettings(helpSeen: true),
      picture: const Size(540, 1200),
    );
    // 360×800 dp at 3x, 보통 (about 1.6 remote pixels per dp): exactly half
    // of 1080×2400, and no Windows scale is asked for.
    expect(
      (h.requested.single.maxWidth, h.requested.single.maxHeight),
      (540, 1200),
    );
    expect(h.requestedScales.single, isNull);
    final picture = find.byWidgetPredicate(
      (w) => w is ColoredBox && w.color == const Color(0xFF2B4C6F),
    );
    // Exactly 2 device pixels per remote pixel: 540 px × 2 / 3 = 360 dp.
    expect(tester.getRect(picture), const Rect.fromLTWH(0, 0, 360, 800));
    await tester.pump(const Duration(seconds: 1));
    expect(h.socket.sent.where((m) => m['type'] == 'stream.resize'), isEmpty);
    await _close(tester);
  });

  testWidgets('with display-scale-v1 bounded resolution and Windows scale '
      'are independent and observed clamp stays honest', (tester) async {
    final h = await _open(
      tester,
      settings: const AndroidSessionSettings(helpSeen: true),
      resizable: true,
      grantedScale: 200,
      physical: const Size(2400, 1080),
      picture: const Size(1280, 576),
    );
    // Adaptive readability requests1280×576 for this device at3x DPR.
    expect(
      (h.requested.first.maxWidth, h.requested.first.maxHeight),
      (1280, 576),
    );
    expect(h.requestedScales.first, 100);
    final picture = find.byWidgetPredicate(
      (w) => w is ColoredBox && w.color == const Color(0xFF2B4C6F),
    );
    expect(tester.getRect(picture), const Rect.fromLTWH(0, 0, 800, 360));
    // The Host's result is shown in the statistics; 크게 asks for another
    // scale without any resize.
    h.socket.incoming.add(
      jsonEncode({
        'type': 'display.scale.applied',
        'sessionId': 'android-session',
        'protocol': 'display-scale-v1',
        'ok': true,
        'scale': 175,
        'maximum': 175,
        'code': 0,
      }),
    );
    await tester.pump(const Duration(milliseconds: 50));
    // 배율 distinguishes the requested initial100 scale from the actual
    // Host-observed value, including when Windows clamps the request.
    expect(h.session.displayScaleWanted, 100);
    expect(h.session.displayScaleApplied?.scale, 175);
    await _openScale(tester);
    expect(find.byKey(const Key('android-scale-panel')), findsNothing);
    // The panel's first lines say it once; the 배율 caption does not repeat
    // them (inspection, 2026-11-12).
    expect(
      find.textContaining('현재 175% · 수동 100% 요청', findRichText: true),
      findsNothing,
    );
    expect(
      find.textContaining(
        '해상도와 Windows 배율은 따로 적용돼요.',
        findRichText: true,
      ),
      findsOneWidget,
    );
    expect(
      tester
          .widget<Text>(find.byKey(const Key('android-screen-observed-state')))
          .data,
      contains('Windows 배율 · 현재 175% · 수동 100% 요청'),
    );
    final large = find.byKey(const Key('android-screen-scale-175'));
    await tester.ensureVisible(large);
    await tester.pumpAndSettle();
    await tester.tap(large);
    await tester.pumpAndSettle();
    final scales = h.socket.sent.where((m) => m['type'] == 'display.scale');
    // An explicit175% is sent without a resize or reconnect.
    expect(scales.map((m) => m['scale']), [175]);
    expect(h.saved.last.screen(AndroidDeviceClass.phone).scale, 175);
    expect(h.socket.sent.where((m) => m['type'] == 'stream.resize'), isEmpty);
    expect(h.requested, hasLength(1));
    // Windows allowed150%: said once, rather than claiming175% applied.
    h.socket.incoming.add(
      jsonEncode({
        'type': 'display.scale.applied',
        'sessionId': 'android-session',
        'protocol': 'display-scale-v1',
        'ok': true,
        'scale': 150,
        'maximum': 150,
        'code': 0,
      }),
    );
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.textContaining('최대 배율 150%'), findsOneWidget);
    await _close(tester);
  });

  testWidgets('the notification shade and even a long visit to another app '
      'keep a live stream without reconnecting on return', (tester) async {
    final h = await _open(tester);
    final binding = tester.binding;
    void leave() {
      binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    }

    void back() {
      binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    }

    // The remote keeps sending frames meanwhile (a live stream).
    var pts = 100000;
    Future<void> wait(Duration duration) async {
      for (var left = duration; left > Duration.zero;) {
        final step = left < const Duration(seconds: 2)
            ? left
            : const Duration(seconds: 2);
        if (!h.socket.wasClosed) h.socket.incoming.add(_video(pts += 33333));
        await tester.pump(step);
        left -= step;
      }
    }

    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    await tester.pump();
    expect(find.byKey(const Key('android-remote-surface')), findsOneWidget);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump();
    // Copying text in another app for half a minute: same connection.
    leave();
    await wait(const Duration(seconds: 30));
    expect(find.byKey(const Key('android-remote-surface')), findsOneWidget);
    back();
    await wait(const Duration(seconds: 40));
    expect(find.byKey(const Key('android-remote-surface')), findsOneWidget);
    expect(h.requested, hasLength(1));
    expect(h.sockets, hasLength(1));
    // Elapsed background time alone must not end a live connection.
    leave();
    await wait(const Duration(seconds: 90));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.sockets.single.wasClosed, isFalse);
    expect(h.requested, hasLength(1));
    // Return keeps the same session after geometry has settled.
    back();
    await tester.pump();
    await tester.pump(const Duration(seconds: 2));
    expect(h.requested, hasLength(1));
    expect(h.sockets, hasLength(1));
    await _close(tester);
  });

  testWidgets('initial Host maintenance waits and releases only its old '
      'reservation before connecting again', (tester) async {
    final h = await _open(tester, firstFrame: false);
    expect(h.session.hasPresentedFrame, isFalse);
    h.socket.incoming.add(
      jsonEncode({
        'type': 'stream.unavailable',
        'sessionId': 'android-session',
        'reason': 'host_maintenance',
      }),
    );
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.session.canRetryInitialPreparation, isTrue);
    expect(find.textContaining('잠시 기다려 주세요'), findsOneWidget);
    expect(find.byKey(const Key('android-retry')), findsNothing);
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(2));
    expect(h.released, ['android-session-1']);
    await _frame(tester, h);
    expect(h.session.hasPresentedFrame, isTrue);
    expect(find.byKey(const Key('android-session-panel')), findsNothing);
    await _close(tester);
  });

  testWidgets('initial HTTP maintenance is retried without a manual button', (
    tester,
  ) async {
    final h = await _open(
      tester,
      firstFrame: false,
      loadFailures: [const BrokerException('host_maintenance')],
    );
    expect(h.sockets, isEmpty);
    expect(find.textContaining('잠시 기다려 주세요'), findsOneWidget);
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(2));
    await _frame(tester, h);
    expect(find.byKey(const Key('android-retry')), findsNothing);
    await _close(tester);
  });

  testWidgets('preparation retries stop on another error or authorization '
      'rejection and never take over', (tester) async {
    for (final failure in [
      const BrokerException('virtual_display_required'),
      const BrokerException('desktop_busy', 409, null, true),
      const BrokerException('host_maintenance', 403),
      const BrokerException('session_expired', 401),
    ]) {
      final h = await _open(
        tester,
        firstFrame: false,
        loadFailures: [const BrokerException('host_maintenance'), failure],
      );
      await tester.pump(const Duration(seconds: 1));
      await tester.pump(const Duration(milliseconds: 100));
      await tester.pump(const Duration(seconds: 40));
      expect(h.requested, hasLength(2), reason: failure.code);
      expect(h.sockets, isEmpty);
      expect(find.textContaining('잠시 기다려 주세요'), findsNothing);
      await _close(tester);
    }
  });

  testWidgets('maintenance after a presented frame requires explicit action', (
    tester,
  ) async {
    final h = await _open(tester);
    h.socket.incoming.add(
      jsonEncode({
        'type': 'stream.unavailable',
        'sessionId': 'android-session',
        'reason': 'host_maintenance',
      }),
    );
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.session.hasPresentedFrame, isTrue);
    expect(h.session.canRetryInitialPreparation, isFalse);
    expect(find.byKey(const Key('android-retry')), findsOneWidget);
    await tester.pump(const Duration(seconds: 40));
    expect(h.requested, hasLength(1));
    await _close(tester);
  });

  testWidgets('initial WS failures other than maintenance are not retried', (
    tester,
  ) async {
    for (final reason in [
      'virtual_display_required',
      'taken_over',
      'session_expired',
    ]) {
      final h = await _open(tester, firstFrame: false);
      h.socket.incoming.add(
        jsonEncode({
          'type': 'stream.unavailable',
          'sessionId': 'android-session',
          'reason': reason,
        }),
      );
      await tester.pump(const Duration(milliseconds: 100));
      expect(h.session.canRetryInitialPreparation, isFalse);
      await tester.pump(const Duration(seconds: 40));
      expect(h.requested, hasLength(1), reason: reason);
      await _close(tester);
    }
  });

  testWidgets('a negotiated display gap continues only an active preparation '
      'cycle without resetting its delays', (tester) async {
    final h = await _open(tester, firstFrame: false, initialViewport: true);
    void unavailable(String reason) => h.socket.incoming.add(
      jsonEncode({
        'type': 'stream.unavailable',
        'sessionId': h.session.connection.sessionId,
        'reason': reason,
      }),
    );
    unavailable('host_maintenance');
    await tester.pump(const Duration(milliseconds: 100));
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 100));
    unavailable('virtual_display_required');
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.session.canRetryInitialDisplayPreparation, isTrue);
    expect(find.byKey(const Key('android-retry')), findsNothing);
    await tester.pump(const Duration(seconds: 1));
    expect(h.requested, hasLength(2), reason: 'the delay is now two seconds');
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(3));
    expect(h.released, ['android-session-1', 'android-session-2']);
    await _close(tester);
  });

  testWidgets('an isolated negotiated display failure still requires action', (
    tester,
  ) async {
    final h = await _open(tester, firstFrame: false, initialViewport: true);
    h.socket.incoming.add(
      jsonEncode({
        'type': 'stream.unavailable',
        'sessionId': h.session.connection.sessionId,
        'reason': 'virtual_display_required',
      }),
    );
    await tester.pump(const Duration(milliseconds: 100));
    await tester.pump(const Duration(seconds: 40));
    expect(h.requested, hasLength(1));
    expect(find.byKey(const Key('android-retry')), findsOneWidget);
    await _close(tester);
  });

  testWidgets(
    'an unnegotiated display failure ends an active preparation cycle',
    (tester) async {
      final h = await _open(
        tester,
        firstFrame: false,
        loadFailures: [const BrokerException('host_maintenance')],
      );
      await tester.pump(const Duration(seconds: 1));
      await tester.pump(const Duration(milliseconds: 100));
      h.socket.incoming.add(
        jsonEncode({
          'type': 'stream.unavailable',
          'sessionId': 'android-session',
          'reason': 'virtual_display_required',
        }),
      );
      await tester.pump(const Duration(milliseconds: 100));
      expect(h.session.canRetryInitialDisplayPreparation, isFalse);
      await tester.pump(const Duration(seconds: 40));
      expect(h.requested, hasLength(2));
      expect(find.byKey(const Key('android-retry')), findsOneWidget);
      await _close(tester);
    },
  );

  testWidgets('persistent preparation stops after five retries with the '
      'preparation reason, not a network error', (tester) async {
    final h = await _open(
      tester,
      firstFrame: false,
      loadFailures: List.filled(20, const BrokerException('host_maintenance')),
    );
    for (var i = 0; i < 40; i++) {
      await tester.pump(const Duration(seconds: 1));
    }
    expect(h.requested, hasLength(6));
    expect(find.textContaining('준비가 아직 끝나지 않았어요'), findsOneWidget);
    expect(find.byKey(const Key('android-retry')), findsOneWidget);
    await tester.pump(const Duration(minutes: 2));
    expect(h.requested, hasLength(6));
    await _close(tester);
  });

  testWidgets('preparation deadline abandons a pending reservation and '
      'releases a late result without dialing it', (tester) async {
    final h = await _open(
      tester,
      firstFrame: false,
      loadFailures: [const BrokerException('host_maintenance')],
    );
    final gate = h.loadGate = Completer<BrokerConnection>();
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(2));
    await tester.pump(const Duration(seconds: 30));
    expect(find.textContaining('준비가 아직 끝나지 않았어요'), findsOneWidget);
    gate.complete(
      BrokerConnection(
        sessionId: 'late',
        webSocketUrl: Uri.parse('wss://fixture.invalid/late'),
        reservation: BrokerSessionReservation(
          () async => h.released.add('late'),
        ),
      ),
    );
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.sockets, isEmpty);
    expect(h.released, ['late']);
    await _close(tester);
  });

  testWidgets('while the Host sets the virtual display the card is at 가상 화면 '
      '준비, below a checked 서버 연결 (2026-11-11: it looked skipped)', (
    tester,
  ) async {
    await _open(tester, firstFrame: false, initialViewport: true);
    final now = find.byKey(const Key('connection-wait-step'));
    expect(tester.widget<Text>(now).data, '가상 화면 준비');
    // Android's order: 연결 요청, 서버 연결, 가상 화면 준비, 첫 화면 수신 대기.
    double top(String label) => tester.getTopLeft(find.text(label)).dy;
    expect(top('서버 연결'), lessThan(top('가상 화면 준비')));
    expect(top('가상 화면 준비'), lessThan(top('첫 화면 수신 대기')));
    // The time is the wait's own, kept by the view through every retry: the
    // list's clock restarts with each attempt (it fell back to 00:00).
    expect(
      tester
          .widget<ConnectionWaitStatus>(find.byType(ConnectionWaitStatus))
          .elapsed,
      isNotNull,
    );
    // Both rows above it are checked, also when 서버 연결 was never drawn.
    expect(
      find.byKey(const ValueKey('connection-wait-done-request')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey('connection-wait-done-relay')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey('connection-wait-done-firstFrame')),
      findsNothing,
    );
    await _close(tester);
  });

  testWidgets('without a display to set the card waits at 서버 연결', (
    tester,
  ) async {
    await _open(tester, firstFrame: false);
    expect(
      tester.widget<Text>(find.byKey(const Key('connection-wait-step'))).data,
      '서버 연결',
    );
    await _close(tester);
  });
  testWidgets('leaving during preparation cancels its scheduled retry', (
    tester,
  ) async {
    final h = await _open(
      tester,
      firstFrame: false,
      loadFailures: [const BrokerException('host_maintenance')],
    );
    await _close(tester);
    await tester.pump(const Duration(seconds: 40));
    expect(h.requested, hasLength(1));
    expect(h.sockets, isEmpty);
  });

  testWidgets('a dropped connection reconnects quietly, keeping the last '
      'picture, after releasing its own old session', (tester) async {
    final h = await _open(tester);
    // The relay socket ends without a reason (a Wi-Fi/mobile handover).
    unawaited(h.socket.incoming.close());
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.byKey(const Key('android-reconnecting')), findsOneWidget);
    expect(
      find.byKey(const Key('android-reconnecting-picture')),
      findsOneWidget,
    );
    expect(find.byKey(const Key('android-session-panel')), findsNothing);
    expect(find.byKey(const Key('connection-wait-step')), findsOneWidget);
    expect(find.byKey(const Key('connection-wait-elapsed')), findsOneWidget);
    expect(h.requested, hasLength(1));
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(2));
    expect(h.released, ['android-session-1']);
    // The kept picture stays until the new connection shows a frame.
    expect(find.byKey(const Key('android-reconnecting')), findsOneWidget);
    await _frame(tester, h);
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    expect(find.byKey(const Key('android-session-panel')), findsNothing);
    await _close(tester);
  });

  testWidgets('a real disconnect retires keyboard composition and modifiers '
      'without flushing them into the replacement session', (tester) async {
    final h = await _open(tester, textInput: true);
    await _toggleKeyboard(tester);
    const s = AndroidTypingBridge.sentinel;
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '$s하',
        selection: TextSelection.collapsed(offset: 3),
        composing: TextRange(start: 2, end: 3),
      ),
    );
    await tester.pumpAndSettle();
    expect(h.texts, ['하']);
    await tester.tap(find.byKey(const Key('android-key-ctrl')));
    await tester.pump();
    unawaited(h.socket.incoming.close());
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.byKey(const Key('android-key-row')), findsNothing);
    final field = tester.widget<TextField>(
      find.byKey(const Key('android-remote-keyboard-field')),
    );
    expect(field.controller!.value, AndroidTypingBridge.initial);
    expect(field.focusNode!.hasFocus, isFalse);
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(2));
    expect(h.texts, ['하'], reason: 'no composition replay during reconnection');
    await _frame(tester, h);
    expect(_keyboardOpen(tester), isFalse);
    await _toggleKeyboard(tester);
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '$s한',
        selection: TextSelection.collapsed(offset: 3),
        composing: TextRange(start: 2, end: 3),
      ),
    );
    await tester.pumpAndSettle();
    expect(h.texts, ['하', '한']);
    expect(h.keys.where((key) => key.startsWith('8')), isEmpty);
    expect(
      h.keys.where((key) => key.startsWith('a2')),
      isEmpty,
      reason: 'old one-shot Ctrl cannot turn new text into a shortcut',
    );
    await _close(tester);
  });

  testWidgets('capture-await home cancellation retires unsent IME state '
      'without backspacing the remote document', (tester) async {
    final h = await _open(tester, textInput: true, deferredCapture: true);
    await _toggleKeyboard(tester);
    final session = h.session as _DeferredCaptureSession;
    final gate = Completer<void>();
    session.gate = gate;
    await session.capture(false);
    const s = AndroidTypingBridge.sentinel;
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '${s}x',
        selection: TextSelection.collapsed(offset: 3),
      ),
    );
    await tester.pump();
    expect(session.held, 1);
    expect(h.texts, isEmpty);
    final binding = tester.binding;
    addTearDown(() {
      if (binding.lifecycleState == AppLifecycleState.paused) {
        binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      }
      if (binding.lifecycleState == AppLifecycleState.hidden) {
        binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      }
      if (binding.lifecycleState == AppLifecycleState.inactive) {
        binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      }
    });
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    gate.complete();
    session.gate = null;
    await tester.pump();
    await tester.pump();
    final field = tester.widget<TextField>(
      find.byKey(const Key('android-remote-keyboard-field')),
    );
    expect(field.controller!.value, AndroidTypingBridge.initial);
    expect(field.focusNode!.hasFocus, isFalse);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 700));
    expect(_keyboardOpen(tester), isFalse);
    await _toggleKeyboard(tester);
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '${s}y',
        selection: TextSelection.collapsed(offset: 3),
      ),
    );
    await tester.pumpAndSettle();
    expect(h.texts, ['y'], reason: 'the cancelled x is never replayed');
    expect(
      h.keys.where((key) => key == '8↓'),
      isEmpty,
      reason: 'the new field cannot delete an x that was never sent',
    );
    await _close(tester);
  });

  testWidgets('ordinary capture delay preserves rapid Hangul composition', (
    tester,
  ) async {
    final h = await _open(tester, textInput: true, deferredCapture: true);
    await _toggleKeyboard(tester);
    final session = h.session as _DeferredCaptureSession;
    final gate = Completer<void>();
    session.gate = gate;
    await session.capture(false);
    const s = AndroidTypingBridge.sentinel;
    for (final text in ['ㄱ', '가']) {
      tester.testTextInput.updateEditingValue(
        TextEditingValue(
          text: '$s$text',
          selection: const TextSelection.collapsed(offset: 3),
          composing: const TextRange(start: 2, end: 3),
        ),
      );
    }
    await tester.pump();
    expect(session.held, 1);
    expect(h.texts, isEmpty);
    gate.complete();
    session.gate = null;
    await tester.pumpAndSettle();
    expect(h.texts, ['ㄱ', '가']);
    expect(_keyboardOpen(tester), isTrue);
    expect(find.textContaining('입력 전달을 확인하지 못해'), findsNothing);
    await _close(tester);
  });

  testWidgets('a late background IME update cannot advance remote typing', (
    tester,
  ) async {
    final h = await _open(tester, textInput: true);
    await _toggleKeyboard(tester);
    final binding = tester.binding;
    addTearDown(() {
      if (binding.lifecycleState == AppLifecycleState.paused) {
        binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      }
      if (binding.lifecycleState == AppLifecycleState.hidden) {
        binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      }
      if (binding.lifecycleState == AppLifecycleState.inactive) {
        binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      }
    });
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    await tester.pump();
    final field = tester.widget<TextField>(
      find.byKey(const Key('android-remote-keyboard-field')),
    );
    expect(
      field.focusNode!.hasFocus,
      isTrue,
      reason: 'healthy background alone preserves the existing keyboard',
    );
    const s = AndroidTypingBridge.sentinel;
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '${s}x',
        selection: TextSelection.collapsed(offset: 3),
      ),
    );
    await tester.pump();
    expect(field.controller!.value, AndroidTypingBridge.initial);
    expect(field.focusNode!.hasFocus, isFalse);
    expect(h.texts, isEmpty);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 700));
    await _toggleKeyboard(tester);
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '${s}y',
        selection: TextSelection.collapsed(offset: 3),
      ),
    );
    await tester.pumpAndSettle();
    expect(h.texts, ['y']);
    expect(h.keys.where((key) => key == '8↓'), isEmpty);
    await _close(tester);
  });

  testWidgets('a healthy home visit preserves the open keyboard without '
      'replaying its composition', (tester) async {
    final h = await _open(tester, textInput: true);
    await _toggleKeyboard(tester);
    tester.view.viewInsets = const FakeViewPadding(bottom: 900);
    await tester.pumpAndSettle();
    const s = AndroidTypingBridge.sentinel;
    tester.testTextInput.updateEditingValue(
      const TextEditingValue(
        text: '$s하',
        selection: TextSelection.collapsed(offset: 3),
        composing: TextRange(start: 2, end: 3),
      ),
    );
    await tester.pumpAndSettle();
    final binding = tester.binding;
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    await tester.pump(const Duration(seconds: 1));
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 700));
    expect(_keyboardOpen(tester), isTrue);
    expect(find.byKey(const Key('android-key-row')), findsOneWidget);
    expect(h.requested, hasLength(1));
    expect(h.texts, ['하']);
    await _close(tester);
  });

  testWidgets('a lost network ends the connection at once and a new one '
      'reconnects without waiting', (tester) async {
    final network = _FakeNetwork();
    final h = await _open(tester, network: network);
    await tester.pump(const Duration(milliseconds: 100));
    expect(network.marks, 1, reason: 'the connection was made on this network');
    h.loadFailures.add(const BrokerException('network_error'));
    network.events.add('lost');
    await tester.pump(const Duration(milliseconds: 50));
    expect(h.sockets.single.wasClosed, isTrue);
    expect(find.byKey(const Key('android-reconnecting')), findsOneWidget);
    // The first try fails (still offline) ...
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(2));
    // ... and mobile data arriving retries at once, not after 2 s.
    network.events.add('available');
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(3));
    await _frame(tester, h);
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    await tester.pump(const Duration(milliseconds: 100));
    expect(network.marks, 2);
    await network.events.close();
    await _close(tester);
  });

  testWidgets('reconnecting gives up after its tries with a clear action and '
      'never loops', (tester) async {
    final h = await _open(tester);
    h.loadFailures.addAll(
      List.filled(20, const BrokerException('network_error')),
    );
    unawaited(h.socket.incoming.close());
    for (var i = 0; i < 90; i++) {
      await tester.pump(const Duration(seconds: 1));
    }
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    expect(find.textContaining('다시 연결하지 못했어요'), findsOneWidget);
    expect(find.byKey(const Key('android-retry')), findsOneWidget);
    final tries = h.requested.length;
    expect(tries, 11, reason: 'the first connection and ten tries');
    await tester.pump(const Duration(minutes: 3));
    expect(h.requested, hasLength(tries));
    // The action connects again.
    h.loadFailures.clear();
    await tester.tap(find.byKey(const Key('android-retry')));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(tries + 1));
    await _frame(tester, h);
    await _close(tester);
  });

  testWidgets('an end decided elsewhere is shown, not retried', (tester) async {
    final h = await _open(tester);
    h.socket.incoming.add(
      jsonEncode({'type': 'session.ended', 'reason': 'taken_over'}),
    );
    await tester.pump(const Duration(milliseconds: 100));
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    await tester.pump(const Duration(seconds: 10));
    expect(h.requested, hasLength(1));
    expect(find.byKey(const Key('android-retry')), findsOneWidget);
    await _close(tester);
  });

  // 2026-11-09 edge-case audit: the end says who took the VM.
  testWidgets('a takeover by another account names it', (tester) async {
    final h = await _open(tester);
    h.socket.incoming.add(
      jsonEncode({
        'type': 'session.ended',
        'reason': 'taken_over',
        'detail': 'other_account',
        'by': 'bob',
      }),
    );
    await tester.pump(const Duration(milliseconds: 100));
    expect(find.textContaining('bob 계정이 다른 창이나 기기에서'), findsOneWidget);
    expect(find.byKey(const Key('android-retry')), findsOneWidget);
    await _close(tester);
  });

  testWidgets('a restarting Host is retried at 1, 2, 4 s; network events do '
      'not hurry a connect the Host refused', (tester) async {
    final network = _FakeNetwork();
    final h = await _open(tester, network: network);
    await tester.pump(const Duration(milliseconds: 100));
    h.loadFailures.addAll(const [
      BrokerException('host_not_registered', 409),
      BrokerException('stream_unavailable', 409),
    ]);
    h.socket.incoming.add(
      jsonEncode({'type': 'session.ended', 'reason': 'host_disconnected'}),
    );
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.byKey(const Key('android-reconnecting')), findsOneWidget);
    // Another network than the marked one stays the default and reports each
    // change of its capabilities (field 2026-11-02: 3 connects within 2 s).
    final counts = <int>[];
    for (var tenth = 1; tenth <= 75; tenth++) {
      network.events.add('available');
      await tester.pump(const Duration(milliseconds: 100));
      counts.add(h.requested.length);
    }
    int at(double seconds) => counts[(seconds * 10).round() - 1];
    expect(at(0.9), 1, reason: 'nothing before the first second');
    expect(at(1.2), 2);
    expect(at(2.9), 2, reason: 'refused: the next try waits 2 s');
    expect(at(3.3), 3);
    expect(at(6.9), 3, reason: 'then 4 s');
    expect(at(7.5), 4, reason: 'the Host is back');
    await _frame(tester, h);
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    await network.events.close();
    await _close(tester);
  });

  testWidgets('a refusal that cannot change ends the reconnect at once', (
    tester,
  ) async {
    final h = await _open(tester);
    h.loadFailures.add(const BrokerException('invalid_initial_viewport', 400));
    unawaited(h.socket.incoming.close());
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(2));
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    expect(find.byKey(const Key('android-retry')), findsOneWidget);
    await tester.pump(const Duration(seconds: 30));
    expect(h.requested, hasLength(2), reason: 'never tried again by itself');
    await _close(tester);
  });

  testWidgets('a new network cuts a wait for the network short only once', (
    tester,
  ) async {
    final network = _FakeNetwork();
    final h = await _open(tester, network: network);
    await tester.pump(const Duration(milliseconds: 100));
    h.loadFailures.addAll(
      List.filled(3, const BrokerException('network_error')),
    );
    network.events.add('lost');
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump(const Duration(seconds: 1));
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(2), reason: 'the first try, still offline');
    network.events.add('available');
    await tester.pump(const Duration(milliseconds: 100));
    expect(h.requested, hasLength(3), reason: 'a new network: at once');
    // The same network repeating itself does not shorten the 4 s wait.
    for (var i = 0; i < 38; i++) {
      network.events.add('available');
      await tester.pump(const Duration(milliseconds: 100));
    }
    expect(h.requested, hasLength(3));
    await tester.pump(const Duration(milliseconds: 300));
    expect(h.requested, hasLength(4));
    await network.events.close();
    await _close(tester);
  });

  testWidgets('reconnect waits are spread by jitter, never below 80 %', (
    tester,
  ) async {
    final h = await _open(tester, reconnectJitter: () => 1);
    h.loadFailures.add(const BrokerException('network_error'));
    unawaited(h.socket.incoming.close());
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump(const Duration(milliseconds: 700));
    expect(h.requested, hasLength(1));
    await tester.pump(const Duration(milliseconds: 100));
    await tester.pump(const Duration(milliseconds: 20));
    expect(h.requested, hasLength(2), reason: '0.8 s instead of 1 s');
    await tester.pump(const Duration(milliseconds: 1500));
    expect(h.requested, hasLength(2));
    await tester.pump(const Duration(milliseconds: 150));
    await tester.pump(const Duration(milliseconds: 20));
    expect(h.requested, hasLength(3), reason: '1.6 s instead of 2 s');
    await _frame(tester, h);
    await _close(tester);
  });

  // Restart recovery (host-ready-wait-v1, 2026-11-03): the Windows wait.
  testWidgets('a broker restart waits on the broker every 250 ms, then on '
      'host-ready, and reconnects within one answer', (tester) async {
    final broker = _HostReadyBroker()..list = () => throw _refused;
    final h = await _open(tester, awaitBrokerRestart: _restartWait(broker));
    _serverStopping(h);
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.byKey(const Key('android-reconnecting')), findsOneWidget);
    expect(find.text(serverRestartingMessage), findsOneWidget);
    // Refused: asked again every 250 ms (before: one connect after 1 s).
    await tester.pump(const Duration(seconds: 1));
    expect(broker.reads, inInclusiveRange(3, 4));
    expect(h.requested, hasLength(1), reason: 'no connect to a broker away');
    // The new broker answers; its Host has not reported yet.
    broker.list = () => _restartDesktop(host: false);
    await tester.pump(const Duration(milliseconds: 300));
    expect(broker.asks, 1);
    final reads = broker.reads;
    await tester.pump(const Duration(seconds: 3));
    expect(broker.reads, reads, reason: 'no list polling while it waits');
    expect(h.requested, hasLength(1));
    expect(find.text(serverRestartingMessage), findsOneWidget);
    // The Host reports: one list read, then the connect at once.
    broker.list = () => _restartDesktop(host: true);
    broker.report();
    for (var i = 0; i < 5; i++) {
      await tester.pump(const Duration(milliseconds: 1));
    }
    expect(broker.reads, reads + 1);
    expect(h.requested, hasLength(2));
    await _frame(tester, h);
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    expect(find.text(serverRestartingMessage), findsNothing);
    await _close(tester);
  });

  testWidgets('a relay drop whose reconnect meets a refused broker turns to '
      'the restart wait', (tester) async {
    final broker = _HostReadyBroker()..list = () => throw _refused;
    final h = await _open(tester, awaitBrokerRestart: _restartWait(broker));
    h.loadFailures.add(_refused);
    unawaited(h.socket.incoming.close());
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.text('다시 연결 중'), findsOneWidget);
    await tester.pump(const Duration(seconds: 1));
    expect(h.requested, hasLength(2), reason: 'the back-off try, refused');
    expect(broker.reads, 0);
    await tester.pump(const Duration(milliseconds: 600));
    expect(find.text(serverRestartingMessage), findsOneWidget);
    expect(broker.reads, inInclusiveRange(2, 3), reason: '250 ms, not 2 s');
    broker.list = () => _restartDesktop(host: true);
    await tester.pump(const Duration(milliseconds: 300));
    expect(h.requested, hasLength(3));
    await _frame(tester, h);
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    await _close(tester);
  });

  testWidgets('a broker without host-ready is read every 500 ms while its '
      'Host is away, and connects within one read', (tester) async {
    final broker = _RestartBroker()..list = () => _restartDesktop(host: false);
    final h = await _open(tester, awaitBrokerRestart: _restartWait(broker));
    _serverStopping(h);
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump(const Duration(milliseconds: 250));
    expect(broker.reads, 1);
    await tester.pump(const Duration(seconds: 5));
    expect(broker.reads, 11, reason: 'one read per 500 ms');
    expect(h.requested, hasLength(1));
    broker.list = () => _restartDesktop(host: true);
    await tester.pump(const Duration(milliseconds: 500));
    await tester.pump(const Duration(milliseconds: 5));
    expect(h.requested, hasLength(2));
    await _frame(tester, h);
    await _close(tester);
  });

  testWidgets('the restart wait is bounded: 30 s of polling, then the '
      '1-2-4-8 s back-off without polling', (tester) async {
    final broker = _RestartBroker()..list = () => _restartDesktop(host: false);
    final h = await _open(tester, awaitBrokerRestart: _restartWait(broker));
    h.loadAnswer = () => const BrokerException('host_offline', 409);
    _serverStopping(h);
    await tester.pump(const Duration(milliseconds: 50));
    for (var i = 0; i < 30; i++) {
      await tester.pump(const Duration(seconds: 1));
    }
    expect(h.requested, hasLength(1));
    final reads = broker.reads;
    expect(reads, inInclusiveRange(59, 61), reason: 'one read per 500 ms');
    // t = 30.25 s: the wait ends; the next tries follow the back-off.
    final counts = <int>[];
    for (var tenth = 1; tenth <= 160; tenth++) {
      await tester.pump(const Duration(milliseconds: 100));
      counts.add(h.requested.length);
    }
    int at(double seconds) => counts[((seconds - 30.05) * 10).round() - 1];
    expect(at(32.1), 1);
    expect(at(32.4), 2, reason: '2 s after the wait');
    expect(at(36.1), 2);
    expect(at(36.4), 3, reason: 'then 4 s');
    expect(at(44.1), 3);
    expect(at(44.4), 4, reason: 'then 8 s');
    expect(broker.reads, lessThanOrEqualTo(reads + 1), reason: 'no polling');
    await _close(tester);
  });

  testWidgets('with restart recovery wired, other failures keep the '
      '1-2-4-8 s back-off', (tester) async {
    var waits = 0;
    final h = await _open(
      tester,
      awaitBrokerRestart: (current, limit) async => waits++,
    );
    h.loadFailures.addAll(const [
      BrokerException('host_not_registered', 409),
      BrokerException('stream_unavailable', 409),
      BrokerException('network_error'),
    ]);
    h.socket.incoming.add(
      jsonEncode({'type': 'session.ended', 'reason': 'host_disconnected'}),
    );
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.text('다시 연결 중'), findsOneWidget);
    final counts = <int>[];
    for (var tenth = 1; tenth <= 160; tenth++) {
      await tester.pump(const Duration(milliseconds: 100));
      counts.add(h.requested.length);
    }
    int at(double seconds) => counts[(seconds * 10).round() - 1];
    expect(at(0.9), 1);
    expect(at(1.2), 2);
    expect(at(2.9), 2);
    expect(at(3.2), 3);
    expect(at(6.9), 3);
    expect(at(7.2), 4);
    expect(at(14.9), 4);
    expect(at(15.2), 5);
    expect(waits, 0, reason: 'no broker restart was met');
    expect(find.text(serverRestartingMessage), findsNothing);
    await _frame(tester, h);
    await _close(tester);
  });

  testWidgets('a final refusal during the restart wait still stops', (
    tester,
  ) async {
    final broker = _HostReadyBroker()..list = () => throw _refused;
    final h = await _open(tester, awaitBrokerRestart: _restartWait(broker));
    _serverStopping(h);
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump(const Duration(milliseconds: 600));
    // The restarted broker no longer assigns this VM to the account.
    broker.list = () => throw const BrokerException('desktop_not_found', 404);
    await tester.pump(const Duration(milliseconds: 300));
    final reads = broker.reads;
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    expect(find.byKey(const Key('android-retry')), findsNothing);
    expect(find.byKey(const Key('android-leave')), findsOneWidget);
    await tester.pump(const Duration(seconds: 30));
    expect(broker.reads, reads, reason: 'never asked again by itself');
    expect(h.requested, hasLength(1));
    await _close(tester);
  });

  testWidgets('a final refusal of the connect after the restart still stops', (
    tester,
  ) async {
    final broker = _HostReadyBroker();
    final h = await _open(tester, awaitBrokerRestart: _restartWait(broker));
    h.loadFailures.add(const BrokerException('invalid_initial_viewport', 400));
    _serverStopping(h);
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump(const Duration(milliseconds: 300));
    await tester.pump();
    expect(h.requested, hasLength(2));
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    expect(find.byKey(const Key('android-retry')), findsOneWidget);
    final reads = broker.reads;
    await tester.pump(const Duration(seconds: 30));
    expect(broker.reads, reads);
    expect(h.requested, hasLength(2), reason: 'never tried again by itself');
    await _close(tester);
  });

  // A Host update ended the stream (host_update, 2026-11-03, as on Windows).
  testWidgets('a Host update end waits on the update (list every 2 s) and on '
      'its Host, then reconnects by itself', (tester) async {
    final broker = _HostReadyBroker()
      ..list = () => _updateDesktop('downloading');
    final h = await _open(tester, awaitHostUpdate: _updateWait(broker));
    _hostUpdateEnd(h);
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.byKey(const Key('android-host-update')), findsOneWidget);
    expect(find.text('Host 업데이트 대기'), findsOneWidget);
    for (var i = 0; i < 11; i++) {
      await tester.pump(const Duration(seconds: 1));
    }
    expect(h.requested, hasLength(1), reason: 'no connect the broker refuses');
    expect(broker.reads, inInclusiveRange(5, 6), reason: 'every 2 s');
    // The installer restarts the Host: the broker answers when it reports.
    broker.list = () => _updateDesktop('installing', host: false);
    await tester.pump(const Duration(seconds: 3));
    expect(broker.asks, 1);
    expect(find.byKey(const Key('android-host-update')), findsOneWidget);
    broker.list = () => _restartDesktop(host: true);
    broker.report();
    for (var i = 0; i < 5; i++) {
      await tester.pump(const Duration(milliseconds: 1));
    }
    expect(h.requested, hasLength(2));
    await _frame(tester, h);
    expect(find.byKey(const Key('android-host-update')), findsNothing);
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    expect(find.byKey(const Key('android-retry')), findsNothing);
    await _close(tester);
  });

  testWidgets('without the update wait, refused tries go on past the '
      'ordinary 90 s until the update is over', (tester) async {
    final h = await _open(tester);
    h.loadFailures.addAll(
      List.filled(40, const BrokerException('host_update_in_progress', 409)),
    );
    _hostUpdateEnd(h);
    await tester.pump(const Duration(milliseconds: 50));
    for (var i = 0; i < 120; i++) {
      await tester.pump(const Duration(seconds: 1));
    }
    expect(find.byKey(const Key('android-host-update')), findsOneWidget);
    expect(find.byKey(const Key('android-retry')), findsNothing);
    expect(h.requested.length, greaterThan(11));
    h.loadFailures.clear();
    await tester.pump(const Duration(seconds: 9));
    await _frame(tester, h);
    expect(find.byKey(const Key('android-host-update')), findsNothing);
    await _close(tester);
  });

  // 2026-11-09 edge-case audit: Windows restarted inside the VM keeps its
  // Host away for longer than the 90 s back-off; the view waits like its own
  // VM restart (3 minutes) and reconnects when the Host is back.
  testWidgets('a Host away with the VM running is waited for past 90 s and '
      'reconnected', (tester) async {
    final broker = _HostReadyBroker()
      ..list = () => _restartDesktop(host: false);
    final h = await _open(tester, awaitHostReturn: _returnWait(broker));
    h.socket.incoming.add(
      jsonEncode({
        'type': 'session.ended',
        'reason': 'host_disconnected',
        'detail': 'host_service_stopping',
      }),
    );
    await tester.pump(const Duration(milliseconds: 50));
    for (var i = 0; i < 120; i++) {
      await tester.pump(const Duration(seconds: 1));
    }
    expect(find.byKey(const Key('android-planned-restart')), findsOneWidget);
    expect(find.textContaining('데스크톱 다시 시작 대기'), findsOneWidget);
    expect(find.byKey(const Key('android-retry')), findsNothing);
    expect(h.requested, hasLength(1), reason: 'no connect while it is away');
    broker.list = () => _restartDesktop(host: true);
    broker.report();
    for (var i = 0; i < 10; i++) {
      await tester.pump(const Duration(milliseconds: 100));
    }
    expect(h.requested, hasLength(2));
    await _frame(tester, h);
    expect(find.byKey(const Key('android-planned-restart')), findsNothing);
    expect(find.byKey(const Key('android-retry')), findsNothing);
    await _close(tester);
  });

  testWidgets('기다리기 취소 stops a Host update wait at once', (tester) async {
    final broker = _HostReadyBroker()
      ..list = () => _updateDesktop('downloading');
    final h = await _open(tester, awaitHostUpdate: _updateWait(broker));
    _hostUpdateEnd(h);
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump(const Duration(seconds: 2));
    await tester.tap(find.byKey(const Key('android-host-update')));
    await tester.pump();
    expect(find.textContaining('Host 업데이트 기다리기를 취소했어요'), findsOneWidget);
    expect(find.byKey(const Key('android-retry')), findsOneWidget);
    final reads = broker.reads;
    broker.list = () => _restartDesktop(host: true);
    await tester.pump(const Duration(minutes: 2));
    expect(h.requested, hasLength(1), reason: 'no more tries');
    expect(broker.reads, reads, reason: 'the wait stopped too');
    await _close(tester);
  });

  testWidgets('a Host update past 10 minutes leaves the manual message and '
      'stops', (tester) async {
    final broker = _HostReadyBroker()
      ..list = () => _updateDesktop('installing');
    final h = await _open(tester, awaitHostUpdate: _updateWait(broker));
    _hostUpdateEnd(h);
    await tester.pump(const Duration(milliseconds: 50));
    for (var i = 0; i < 59; i++) {
      await tester.pump(const Duration(seconds: 10));
    }
    expect(find.byKey(const Key('android-host-update')), findsOneWidget);
    for (var i = 0; i < 3; i++) {
      await tester.pump(const Duration(seconds: 10));
    }
    expect(find.byKey(const Key('android-host-update')), findsNothing);
    expect(find.textContaining('업데이트가 끝나면 다시 연결하세요'), findsOneWidget);
    expect(find.byKey(const Key('android-retry')), findsOneWidget);
    expect(h.requested, hasLength(1));
    final reads = broker.reads;
    await tester.pump(const Duration(minutes: 2));
    expect(h.requested, hasLength(1));
    expect(broker.reads, reads);
    await _close(tester);
  });

  testWidgets('a visit to another app during a Host update wait: the wait '
      'goes on on return', (tester) async {
    final broker = _HostReadyBroker()
      ..list = () => _updateDesktop('downloading');
    final h = await _open(tester, awaitHostUpdate: _updateWait(broker));
    final binding = tester.binding;
    addTearDown(() {
      for (final state in const [
        AppLifecycleState.hidden,
        AppLifecycleState.inactive,
        AppLifecycleState.resumed,
      ]) {
        if (binding.lifecycleState != AppLifecycleState.resumed) {
          binding.handleAppLifecycleStateChanged(state);
        }
      }
    });
    _hostUpdateEnd(h);
    await tester.pump(const Duration(milliseconds: 50));
    await tester.pump(const Duration(seconds: 3));
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    await tester.pump(const Duration(seconds: 30));
    binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pump();
    await tester.pump(const Duration(seconds: 2));
    // The last picture with its chip, as before the visit.
    expect(find.byKey(const Key('android-host-update')), findsOneWidget);
    expect(find.byKey(const Key('android-retry')), findsNothing);
    expect(h.requested, hasLength(1), reason: 'still no refused connect');
    broker.list = () => _restartDesktop(host: true);
    await tester.pump(const Duration(seconds: 3));
    expect(h.requested, hasLength(2));
    await _frame(tester, h);
    expect(find.byKey(const Key('android-session-panel')), findsNothing);
    await _close(tester);
  });

  testWidgets('with the update wait wired, another end is still shown, not '
      'waited for', (tester) async {
    var waits = 0;
    final h = await _open(
      tester,
      awaitHostUpdate: (current, limit) async => waits++,
    );
    h.socket.incoming.add(
      jsonEncode({
        'type': 'session.ended',
        'reason': 'clipboard_policy_changed',
      }),
    );
    await tester.pump(const Duration(milliseconds: 100));
    expect(find.byKey(const Key('android-host-update')), findsNothing);
    expect(find.byKey(const Key('android-reconnecting')), findsNothing);
    await tester.pump(const Duration(seconds: 30));
    expect(h.requested, hasLength(1));
    expect(waits, 0);
    expect(find.byKey(const Key('android-retry')), findsOneWidget);
    await _close(tester);
  });

  final restartRows = <Map<String, int>>[];
  for (final (brokerUpMs, hostMs) in const [
    (400, 770),
    (900, 1790),
    (1300, 1790),
    (5000, 6000),
  ]) {
    testWidgets('simulated: broker up at $brokerUpMs ms, Host at $hostMs ms, '
        'back-off vs restart recovery', (tester) async {
      final before = await _restartRecoveryMs(
        tester,
        brokerUpMs: brokerUpMs,
        hostMs: hostMs,
        recovery: false,
      );
      final after = await _restartRecoveryMs(
        tester,
        brokerUpMs: brokerUpMs,
        hostMs: hostMs,
        recovery: true,
      );
      expect(after, lessThanOrEqualTo(before));
      expect(after - hostMs, lessThanOrEqualTo(30), reason: 'one list read');
      restartRows.add({
        'brokerUpMs': brokerUpMs,
        'hostMs': hostMs,
        'backoffConnectMs': before,
        'restartRecoveryConnectMs': after,
      });
    });
  }
  test('simulated restart recovery times', () {
    // ignore: avoid_print
    print(jsonEncode({'simulated': true, 'endToConnect': restartRows}));
  });

  testWidgets('사용안내 opens the gestures in the menu\'s place, as Windows\' '
      '사용안내 opens beside its bar; touch mode is kept', (tester) async {
    final h = await _open(tester, settings: const AndroidSessionSettings());
    final help = find.byKey(const Key('android-touch-help'));
    final menu = find.byKey(const Key('session-command-bar'));
    expect(help, findsNothing);
    expect(find.byKey(const Key('expand-session-toolbar')), findsOneWidget);
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    final at = tester.getRect(menu);
    // At Windows' 사용안내 place, with its name: a row of the 설정 panel,
    // not of the menu card.
    expect(find.byKey(const Key('android-session-help')), findsNothing);
    await tester.tap(find.byKey(const Key('android-session-more')));
    await tester.pumpAndSettle();
    final more = find.byKey(const Key('android-more-panel'));
    expect(more, findsOneWidget);
    expect(
      find.descendant(
        of: find.byKey(const Key('android-session-help')),
        matching: find.text('사용안내'),
      ),
      findsOneWidget,
    );
    await tester.tap(find.byKey(const Key('android-session-help')));
    await tester.pumpAndSettle();
    expect(help, findsOneWidget);
    expect(menu, findsNothing, reason: 'in the menu\'s place');
    expect(more, findsNothing, reason: 'in the 설정 panel\'s place');
    expect(tester.getRect(help).right, closeTo(at.right, 1));
    // No sheet over the picture: Back shows 설정 again (where it was opened
    // from), and Back there the menu.
    await tester.tap(find.byKey(const Key('session-menu-back')));
    await tester.pumpAndSettle();
    expect(help, findsNothing);
    expect(more, findsOneWidget);
    expect(menu, findsNothing);
    await tester.tap(find.byKey(const Key('session-menu-back')));
    await tester.pumpAndSettle();
    expect(more, findsNothing);
    expect(menu, findsOneWidget);
    await _openPanel(tester, 'help', 'android-touch-help');
    final close = find.byKey(const Key('android-touch-help-close'));
    await tester.ensureVisible(close);
    await tester.pumpAndSettle();
    await tester.tap(close);
    await tester.pumpAndSettle();
    expect(help, findsNothing);
    expect(more, findsNothing);
    expect(menu, findsNothing);
    expect(find.byKey(const Key('expand-session-toolbar')), findsOneWidget);
    // 포인터 is the default; the cluster's 입력 방식 switches to 터치.
    await tester.tap(find.byKey(const Key('android-remote-touch-mode-button')));
    await tester.pumpAndSettle();
    expect(h.saved.last.touch, AndroidTouchMode.direct);
    await _close(tester);
  });

  testWidgets('TV 단축키: a false shortcut result is uncertain even when keys '
      'were sent', (tester) async {
    final h = await _open(tester, television: true, uncertainShortcut: true);
    // The TV keeps its 단축키 (its remote has no key row).
    await tester.sendKeyEvent(LogicalKeyboardKey.contextMenu);
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('android-tv-menu-shortcuts')));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Windows 키  Win'));
    await tester.pumpAndSettle();
    expect(h.keys, ['5b↓', '5b↑']);
    expect(find.textContaining('단축키 전송 결과를 확인하지 못했어요'), findsOneWidget);
    await tester.pump(const Duration(seconds: 1));
    expect(h.keys, ['5b↓', '5b↑'], reason: 'no automatic resend');
    await _close(tester);
  });

  testWidgets(
    'a compact notice passes actual remote taps and preserves another Snackbar',
    (tester) async {
      final h = await _open(
        tester,
        physical: const Size(320, 780),
        devicePixelRatio: 1,
        picture: const Size(320, 780),
      );
      final messenger = ScaffoldMessenger.of(
        tester.element(find.byType(AndroidRemoteView)),
      );
      // A notice of two sentences: the key row's Ctrl+Alt+Del says why it
      // is not sent.
      await tester.tap(find.byKey(const Key('android-remote-keyboard-button')));
      await tester.pumpAndSettle();
      final secure = await _keyRowShow(tester, 'secure');
      // Another notice at the bottom edge (over the key row: its keys are
      // pressed through their own callbacks below).
      messenger.showSnackBar(
        const SnackBar(
          duration: Duration(seconds: 20),
          content: Text('Keep another notice'),
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      tester.widget<InkWell>(secure).onTap!();
      await tester.pumpAndSettle();
      tester
          .widget<InkWell>(find.byKey(const Key('android-key-hide')))
          .onTap!();
      await tester.pumpAndSettle();
      final notice = find.byKey(const Key('android-remote-notice-message'));
      expect(notice, findsOneWidget);
      expect(find.text('Keep another notice'), findsOneWidget);
      h.socket.sent.clear();
      await tester.tapAt(tester.getCenter(notice));
      await tester.pump(const Duration(milliseconds: 350));
      final buttons = h.inputs.where((event) => event['event'] == 'button');
      expect(buttons.map((event) => event['down']), [true, false]);
      // Two sentences stay long enough to read slowly (they went after 2 s).
      await tester.pump(const Duration(seconds: 3));
      expect(notice, findsOneWidget);
      await tester.pump(const Duration(seconds: 10));
      expect(notice, findsNothing);
      expect(find.text('Keep another notice'), findsOneWidget);
      await _close(tester);
    },
  );

  testWidgets('Android latency statistics name the decoder endpoint without '
      'claiming frame presentation', (tester) async {
    await _open(tester, inputLatency: true);
    // 연결 품질 opens the PC's 연결 panel in the card's place; its 연결 통계
    // 보기 opens the statistics.
    await _openPanel(tester, 'connection', 'android-connection-panel');
    await tester.ensureVisible(find.byKey(const Key('quality-stats')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('quality-stats')));
    await tester.pumpAndSettle();
    final dialog = tester.widget<SessionStatsDialog>(
      find.byType(SessionStatsDialog),
    );
    final sections = dialog.sections();
    final labels = sections
        .expand((section) => section.metrics)
        .map((metric) => metric.label);
    expect(labels, contains('입력→새 프레임 디코딩 (p50/p95)'));
    expect(labels, isNot(contains('입력 → 새 화면 전달 (p50/p95)')));
    final note = sections
        .singleWhere((section) => section.title == '프레임과 네트워크')
        .note!;
    expect(note, contains('MediaCodec 출력 콜백'));
    expect(note, contains('텍스처 전달·폐기'));
    expect(note, contains('실제 화면 표시는 포함하지 않아요'));
    await _close(tester);
  });

  testWidgets('clipboard sharing is silent; as on Windows, a paste that could '
      'not be applied and sharing that stopped are said once', (tester) async {
    final clipboard = _StoppingClipboard();
    final h = await _open(tester, clipboard: clipboard);
    h.session.notifyListeners();
    await tester.pump();
    await tester.pump();
    expect(find.textContaining('클립보드'), findsNothing);
    clipboard.pasteNotice = '폴더는 아직 옮길 수 없어요.';
    h.session.notifyListeners();
    await tester.pump();
    await tester.pump();
    expect(find.text('폴더는 아직 옮길 수 없어요.'), findsOneWidget);
    await tester.pump(const Duration(seconds: 16));
    clipboard
      ..stopped = true
      ..error = 'clipboard_policy_changed'
      ..status = '관리자가 클립보드 권한을 바꿨어요.';
    h.session.notifyListeners();
    await tester.pump();
    await tester.pump();
    expect(find.text('클립보드 공유가 멈췄어요. 관리자가 클립보드 권한을 바꿨어요.'), findsOneWidget);
    await tester.pump(const Duration(seconds: 16));
    h.session.notifyListeners();
    await tester.pump();
    await tester.pump();
    expect(find.textContaining('클립보드 공유가 멈췄어요'), findsNothing);
    await _close(tester);
  });

  testWidgets('no 클립보드 or 키 조합 entry, even with a clipboard grant, as '
      'on Windows: Win and Ctrl+Alt+Del are on the key row; the quality '
      'chip holds the statistics', (tester) async {
    final h = await _open(tester, clipboard: _NoticeClipboard());
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    for (final gone in const [
      'android-session-clipboard',
      'android-session-shortcuts',
    ]) {
      expect(find.byKey(Key(gone)), findsNothing, reason: gone);
    }
    for (final label in const ['클립보드', '키 조합']) {
      expect(find.text(label), findsNothing, reason: label);
    }
    await tester.tap(find.byKey(const Key('session-toolbar-fold')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('android-remote-keyboard-button')));
    await tester.pumpAndSettle();
    await _keyRowShow(tester, 'secure');
    final win = await _keyRowShow(tester, 'win', back: true);
    await tester.tap(win);
    await tester.pumpAndSettle();
    expect(h.keys, ['5b↓', '5b↑']);
    await tester.tap(find.byKey(const Key('android-key-hide')));
    await tester.pumpAndSettle();
    // The 연결 품질 row opens the PC's 연결 panel in the card's place (not
    // the former list of values in the card); it holds the statistics.
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    expect(
      find.descendant(
        of: find.byKey(const Key('session-quality')),
        matching: find.text('연결 품질'),
      ),
      findsOneWidget,
    );
    await tester.tap(find.byKey(const Key('session-quality')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('session-command-bar')), findsNothing);
    expect(find.byKey(const Key('android-session-menu-card')), findsNothing);
    final connection = find.byKey(const Key('android-connection-panel'));
    expect(connection, findsOneWidget);
    for (final key in const [
      'session-connection-panel',
      'quality-latency',
      'quality-value-fps',
      'quality-value-rate',
      'quality-value-resolution',
      'quality-value-path',
      'quality-stats',
    ]) {
      expect(
        find.descendant(of: connection, matching: find.byKey(Key(key))),
        findsOneWidget,
        reason: key,
      );
    }
    expect(find.text('왕복 지연 · Host 응답'), findsOneWidget);
    // Nothing went wrong with a resize and the latency is not poor: no
    // 화면 크기 다시 적용, no advice.
    expect(find.byKey(const Key('android-session-resize')), findsNothing);
    expect(find.byKey(const Key('quality-advice')), findsNothing);
    // Back shows the menu card again.
    await tester.tap(find.byKey(const Key('session-menu-back')));
    await tester.pumpAndSettle();
    expect(connection, findsNothing);
    expect(find.byKey(const Key('session-command-bar')), findsOneWidget);
    await _openPanel(tester, 'connection', 'android-connection-panel');
    await tester.ensureVisible(find.byKey(const Key('quality-stats')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('quality-stats')));
    await tester.pumpAndSettle();
    expect(find.byType(SessionStatsDialog), findsOneWidget);
    await _close(tester);
  });

  testWidgets('터치 mode: double tap and hold-move drags from the second '
      'press, released at the lift; the first click is never sent', (
    tester,
  ) async {
    final h = await _open(
      tester,
      settings: const AndroidSessionSettings(
        touch: AndroidTouchMode.direct,
        helpSeen: true,
      ),
    );
    // 1280×720 fitted into 360×800: rows 298.75..501.25.
    await tester.tapAt(const Offset(100, 400));
    await tester.pump(const Duration(milliseconds: 100));
    final gesture = await tester.startGesture(const Offset(102, 400));
    await tester.pump(const Duration(milliseconds: 16));
    for (final step in const [Offset(4, 0), Offset(40, 0), Offset(60, 30)]) {
      await gesture.moveBy(step);
      await tester.pump(const Duration(milliseconds: 16));
    }
    await gesture.up();
    await tester.pumpAndSettle();
    expect(h.sequence, ['L↓', 'L↑']);
    final buttons = [
      for (final m in h.inputs)
        if (m['event'] == 'button') m,
    ];
    expect(buttons.first['x'], closeTo(102 / 360, .002));
    expect(buttons.first['y'], closeTo(.5, .002));
    expect(buttons.last['x'], closeTo(206 / 360, .002));
    final down = h.inputs.indexOf(buttons.first);
    final up = h.inputs.indexOf(buttons.last);
    expect(
      h.inputs.sublist(down, up).where((m) => m['event'] == 'move'),
      isNotEmpty,
      reason: 'the pointer moves while the button is held',
    );
    await _close(tester);
  });

  testWidgets('a second finger on 입력 방식 switches mode during a remote drag, '
      'resets input and discards the old gesture without another click', (
    tester,
  ) async {
    final h = await _open(
      tester,
      settings: const AndroidSessionSettings(
        touch: AndroidTouchMode.direct,
        helpSeen: true,
      ),
    );
    await tester.tapAt(const Offset(100, 400));
    await tester.pump(const Duration(milliseconds: 100));
    final drag = await tester.startGesture(const Offset(102, 400), pointer: 31);
    await tester.pump(const Duration(milliseconds: 16));
    for (final delta in const [Offset(4, 0), Offset(40, 0)]) {
      await drag.moveBy(delta);
      await tester.pump(const Duration(milliseconds: 16));
    }
    expect(h.sequence, [
      'L↓',
    ], reason: 'the remote button is down before switching');
    final beforeSwitch = h.socket.sent.length;
    final mode = find.byKey(const Key('android-remote-touch-mode-button'));
    final toggle = await tester.startGesture(
      tester.getCenter(mode),
      pointer: 32,
    );
    await toggle.up();
    await tester.pumpAndSettle();
    expect(h.saved.last.touch, AndroidTouchMode.trackpad);
    final afterSwitch = h.socket.sent.skip(beforeSwitch);
    expect(afterSwitch.where((m) => m['type'] == 'input.reset'), isNotEmpty);
    final buttonsAfterReset = h.inputs
        .where((m) => m['event'] == 'button')
        .length;
    await drag.moveBy(const Offset(30, 15));
    await drag.up();
    await tester.pumpAndSettle();
    await tester.pump(const Duration(seconds: 1));
    expect(
      h.inputs.where((m) => m['event'] == 'button'),
      hasLength(buttonsAfterReset),
      reason: 'the discarded first gesture must not click in the new mode',
    );
    expect(h.sequence, ['L↓'], reason: 'input.reset releases the held button');
    await _close(tester);
  });

  testWidgets('opening the keyboard with another finger resets a remote drag '
      'without a late click and still accepts typing', (tester) async {
    final h = await _open(
      tester,
      textInput: true,
      settings: const AndroidSessionSettings(
        touch: AndroidTouchMode.direct,
        helpSeen: true,
      ),
    );
    await tester.tapAt(const Offset(100, 400));
    await tester.pump(const Duration(milliseconds: 100));
    final drag = await tester.startGesture(const Offset(102, 400), pointer: 41);
    await tester.pump(const Duration(milliseconds: 16));
    for (final delta in const [Offset(4, 0), Offset(40, 0)]) {
      await drag.moveBy(delta);
      await tester.pump(const Duration(milliseconds: 16));
    }
    expect(h.sequence, ['L↓']);
    final beforeOpen = h.socket.sent.length;
    final keyboard = find.byKey(const Key('android-remote-keyboard-button'));
    final opener = await tester.startGesture(
      tester.getCenter(keyboard),
      pointer: 42,
    );
    await opener.up();
    await tester.pumpAndSettle();
    expect(_keyboardOpen(tester), isTrue);
    expect(
      h.socket.sent.skip(beforeOpen).where((m) => m['type'] == 'input.reset'),
      isNotEmpty,
    );
    await drag.moveBy(const Offset(30, 15));
    await drag.up();
    await tester.pumpAndSettle();
    await tester.pump(const Duration(seconds: 1));
    expect(h.sequence, ['L↓'], reason: 'input.reset ends the discarded drag');
    const sentinel = AndroidTypingBridge.sentinel;
    tester.testTextInput.updateEditingValue(
      TextEditingValue(
        text: '${sentinel}a',
        selection: TextSelection.collapsed(offset: sentinel.length + 1),
      ),
    );
    await tester.pumpAndSettle();
    expect(h.texts, ['a']);
    await _close(tester);
  });

  testWidgets('포인터 mode (default): the toolbar folds out move and screen; '
      'the move handle pans the zoomed picture without input', (tester) async {
    final h = await _open(tester);
    // Folded: only the cluster.
    expect(find.byKey(const Key('android-session-screen')), findsNothing);
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-session-screen')), findsOneWidget);
    await tester.tap(find.byKey(const Key('android-session-screen')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-screen-panel')), findsOneWidget);
    final fitted = tester.getSize(_penPicture);
    await _pinchPicture(tester);
    expect(tester.getSize(_penPicture).width, greaterThan(fitted.width));
    await _openScreenPanel(tester);
    expect(find.byKey(const Key('android-screen-zoom-in')), findsNothing);
    await tester.tap(find.byKey(const Key('android-session-move')));
    await tester.pumpAndSettle();
    // The menu makes way for the move handle beside the cluster.
    expect(find.byKey(const Key('session-command-bar')), findsNothing);
    expect(find.byKey(const Key('android-screen-panel')), findsNothing);
    final picture = find.byWidgetPredicate(
      (w) => w is ColoredBox && w.color == const Color(0xFF2B4C6F),
    );
    final before = tester.getRect(picture);
    final sent = h.inputs.length;
    await tester.drag(
      find.byKey(const Key('android-session-move-handle')),
      const Offset(30, 0),
    );
    await tester.pumpAndSettle();
    expect(tester.getRect(picture).left, greaterThan(before.left));
    expect(h.inputs.length, sent, reason: 'moving the picture sends nothing');
    // Back ends 이동 before asking to leave.
    await tester.binding.handlePopRoute();
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-session-move-handle')), findsNothing);
    expect(find.byKey(const Key('android-session-screen')), findsNothing);
    expect(find.text('연결을 끊을까요?'), findsNothing);
    await _close(tester);
  });

  // 화면 이동 stayed on after 전체 보기 took the picture back to the whole
  // view: its handle was gone, yet the next Back only switched it off and
  // nothing visible happened (inspection, 2026-11-12).
  testWidgets('전체 보기 ends 화면 이동: the next Back asks to leave', (
    tester,
  ) async {
    await _open(tester);
    await _pinchPicture(tester);
    await _openScreenPanel(tester);
    await tester.tap(find.byKey(const Key('android-session-move')));
    await tester.pumpAndSettle();
    expect(
      find.byKey(const Key('android-session-move-handle')),
      findsOneWidget,
    );
    await _openScreenPanel(tester);
    await tester.tap(find.byKey(const Key('android-screen-fit')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-session-move-handle')), findsNothing);
    await _foldTools(tester);
    await tester.binding.handlePopRoute();
    await tester.pumpAndSettle();
    expect(find.text('연결을 끊을까요?'), findsOneWidget);
    await _close(tester);
  });
  Future<void> openKeyboard(WidgetTester tester) => _toggleKeyboard(tester);

  const s = AndroidTypingBridge.sentinel;
  TextEditingValue typed(String text, {int? composeFrom}) {
    final value = '$s$text';
    return TextEditingValue(
      text: value,
      selection: TextSelection.collapsed(offset: value.length),
      composing: composeFrom == null
          ? TextRange.empty
          : TextRange(start: s.length + composeFrom, end: value.length),
    );
  }

  testWidgets('text-input-v1: Korean appears remotely while composing, emoji '
      'as text, Enter as a key', (tester) async {
    final h = await _open(tester, textInput: true);
    await openKeyboard(tester);
    for (final step in [
      typed('ㅎ', composeFrom: 0),
      typed('하', composeFrom: 0),
      typed('한', composeFrom: 0),
      typed('한ㄱ', composeFrom: 1),
      typed('한그', composeFrom: 1),
      typed('한글', composeFrom: 1),
    ]) {
      tester.testTextInput.updateEditingValue(step);
      await tester.pump();
    }
    // Each composing step replaces the syllable on the remote, like typing
    // on the PC; no local "입력 중" preview.
    expect(h.texts, ['ㅎ', '하', '한', 'ㄱ', '그', '글']);
    expect(find.textContaining('입력 중'), findsNothing);
    tester.testTextInput.updateEditingValue(typed('한글\n'));
    await tester.pumpAndSettle();
    expect(h.sequence, [
      '«ㅎ»', '8↓', '8↑', '«하»', '8↓', '8↑', '«한»', //
      '«ㄱ»', '8↓', '8↑', '«그»', '8↓', '8↑', '«글»', 'd↓', 'd↑',
    ]);
    tester.testTextInput.updateEditingValue(typed('😀 ok'));
    await tester.pumpAndSettle();
    expect(h.texts.last, '😀 ok');
    // No 2-set key emulation in a text session.
    expect(h.keys.where((k) => k.startsWith('47')), isEmpty);
    await _close(tester);
  });

  testWidgets('text being composed is already typed when a click moves the '
      'caret', (tester) async {
    final h = await _open(tester, textInput: true);
    await openKeyboard(tester);
    tester.testTextInput.updateEditingValue(typed('하', composeFrom: 0));
    await tester.pump();
    expect(h.texts, ['하']);
    await tester.tapAt(const Offset(180, 400));
    await tester.pumpAndSettle();
    expect(h.sequence, ['«하»', 'L↓', 'L↑']);
    await _close(tester);
  });

  testWidgets('Ctrl from the key row makes typed letters a shortcut, not '
      'text', (tester) async {
    final h = await _open(tester, textInput: true);
    await openKeyboard(tester);
    await tester.tap(find.byKey(const Key('android-key-ctrl')));
    await tester.pump();
    tester.testTextInput.updateEditingValue(typed('c'));
    await tester.pumpAndSettle();
    expect(h.sequence, ['a2↓', '43↓', '43↑', 'a2↑']);
    expect(h.texts, isEmpty);
    await _close(tester);
  });

  testWidgets('an older Host without text input still gets 2-set keys', (
    tester,
  ) async {
    final h = await _open(tester);
    await openKeyboard(tester);
    tester.testTextInput.updateEditingValue(typed('한'));
    await tester.pumpAndSettle();
    expect(h.texts, isEmpty);
    expect(h.keys, ['47↓', '47↑', '4b↓', '4b↑', '53↓', '53↑']);
    await _close(tester);
  });

  testWidgets('input counts (never content) ride along with the statistics '
      'window', (tester) async {
    final h = await _open(tester, textInput: true, routed: true);
    // A click (after the double-tap window), then a double-tap drag.
    await tester.tapAt(const Offset(180, 400));
    await tester.pump(const Duration(milliseconds: 400));
    await tester.tapAt(const Offset(100, 400));
    await tester.pump(const Duration(milliseconds: 60));
    final drag = await tester.startGesture(const Offset(100, 400));
    await drag.moveBy(const Offset(50, 0));
    await tester.pump(const Duration(milliseconds: 16));
    await drag.up();
    await tester.pumpAndSettle();
    await openKeyboard(tester);
    tester.testTextInput.updateEditingValue(typed('hi'));
    await tester.pumpAndSettle();
    // A few seconds of steady streaming (one statistics sample per second).
    for (var n = 2; n <= 5; n++) {
      h.socket.incoming.add(_video(16667 * n));
      await tester.pump(const Duration(seconds: 1));
    }
    expect(h.stats, isEmpty, reason: 'the window is still open');
    // Disconnecting sends the open window with its input counts.
    await tester.tap(find.byKey(const Key('android-key-hide')));
    await tester.pumpAndSettle();
    // Disconnecting is Back with one confirmation.
    await tester.binding.handlePopRoute();
    await tester.pumpAndSettle();
    await tester.tap(find.widgetWithText(FilledButton, '연결 끊기'));
    await tester.pumpAndSettle();
    expect(find.text('list'), findsOneWidget);
    expect(h.stats, isNotEmpty);
    final row = h.stats.first;
    expect(row['taps'], 1);
    expect(row['dragStarts'], 1);
    expect(row['keyboardOpens'], 1);
    expect(row['textCommits'], 1);
    expect(row['textUnits'], 2);
    expect(row['textInput'], true);
    expect(row['touchMode'], 1, reason: '포인터 is the default');
    expect(row.values.every((v) => v is int || v is bool), true);
    await _close(tester);
  });

  // --- ToTBDesk parity while typing, and the 화면 options ------------------

  final picture = find.byWidgetPredicate(
    (w) => w is ColoredBox && w.color == const Color(0xFF2B4C6F),
  );

  testWidgets('the phone keyboard lifts the picture by itself and its key '
      'row, shows the move handle, and the picture returns on close', (
    tester,
  ) async {
    // 540×1200 at 2 device pixels each fills the 360×800 dp screen.
    final h = await _open(tester, picture: const Size(540, 1200));
    expect(tester.getRect(picture), const Rect.fromLTWH(0, 0, 360, 800));
    expect(find.byKey(const Key('android-session-move-handle')), findsNothing);
    await openKeyboard(tester);
    tester.view.viewInsets = const FakeViewPadding(bottom: 900); // 300 dp
    await tester.pumpAndSettle();
    // Room above the keys: 800 - 300 - 48 = 452 dp. The picture moved up by
    // (almost) everything covered, keeping its zoom, the cursor in view.
    final lifted = tester.getRect(picture);
    expect(lifted.size, const Size(360, 800));
    expect(lifted.top, lessThanOrEqualTo(-340));
    expect(lifted.bottom, greaterThanOrEqualTo(452));
    // 이동 shows by itself while part of the picture is hidden; dragging it
    // moves the picture and sends nothing.
    final handle = find.byKey(const Key('android-session-move-handle'));
    expect(handle, findsOneWidget);
    expect(tester.getRect(handle).bottom, lessThanOrEqualTo(452));
    final sent = h.inputs.length;
    await tester.drag(handle, const Offset(0, 120));
    await tester.pumpAndSettle();
    expect(tester.getRect(picture).top, greaterThan(lifted.top + 60));
    expect(h.inputs.length, sent, reason: 'moving the picture sends nothing');
    // Closing the keyboard puts it back exactly.
    await tester.tap(find.byKey(const Key('android-key-hide')));
    tester.view.viewInsets = FakeViewPadding.zero;
    await tester.pumpAndSettle();
    expect(tester.getRect(picture), const Rect.fromLTWH(0, 0, 360, 800));
    expect(find.byKey(const Key('android-session-move-handle')), findsNothing);
    await _close(tester);
  });

  testWidgets('이동 waits until something is hidden; 화면 closes the keyboard '
      'first; the bar starts folded above it', (tester) async {
    await _open(tester, picture: const Size(540, 1200));
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('android-session-screen')));
    await tester.pumpAndSettle();
    final move = tester.widget<InkWell>(
      find.byKey(const Key('android-session-move')),
    );
    expect(move.onTap, isNull, reason: 'the whole picture is in view');
    // The cluster's 키보드 beside the open panel.
    await tester.tap(find.byKey(const Key('android-remote-keyboard-button')));
    tester.view.viewInsets = const FakeViewPadding(bottom: 900);
    await tester.pumpAndSettle();
    expect(_keyboardOpen(tester), isTrue);
    await _openScreenPanel(tester);
    tester.view.viewInsets = FakeViewPadding.zero;
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-key-row')), findsNothing);
    expect(find.byKey(const Key('android-screen-panel')), findsOneWidget);
    await _close(tester);
  });

  testWidgets('an external keyboard works before any touch (input is taken '
      'when the picture is up)', (tester) async {
    final h = await _open(tester);
    expect(h.native.methods, contains('capture'));
    await _close(tester);
  });

  testWidgets('배율 has five live choices and 화면 exposes bounded source sizes '
      'while rotation follows the device', (tester) async {
    final orientations = <Object?>[];
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      SystemChannels.platform,
      (call) async {
        if (call.method == 'SystemChrome.setPreferredOrientations') {
          orientations.add(call.arguments);
        }
        return null;
      },
    );
    addTearDown(
      () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        SystemChannels.platform,
        null,
      ),
    );
    final h = await _open(
      tester,
      settings: const AndroidSessionSettings(helpSeen: true),
      resizable: true,
      grantedScale: 200,
      physical: const Size(2400, 1080),
      picture: const Size(1280, 576),
    );
    // Exactly the five requested manual choices, without an extra auto chip,
    // inside 화면 as on the PC (no 배율 row or panel of its own).
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-session-scale')), findsNothing);
    await _openScale(tester);
    expect(find.byKey(const Key('android-scale-panel')), findsNothing);
    expect(find.byKey(const Key('android-screen-scale-0')), findsNothing);
    for (final percent in [100, 125, 150, 175, 200]) {
      expect(find.byKey(Key('android-screen-scale-$percent')), findsOneWidget);
    }
    // A fixed 배율 applies live and is remembered for this kind of screen:
    // no resize, no reconnect.
    final fixed = find.byKey(const Key('android-screen-scale-175'));
    await tester.ensureVisible(fixed);
    await tester.pumpAndSettle();
    await tester.tap(fixed);
    await tester.pump(const Duration(seconds: 1));
    expect(h.saved.last.screen(AndroidDeviceClass.phone).scale, 175);
    expect(
      h.socket.sent
          .where((m) => m['type'] == 'display.scale')
          .map((m) => m['scale'])
          .last,
      175,
    );
    expect(h.socket.sent.where((m) => m['type'] == 'stream.resize'), isEmpty);
    expect(h.requested, hasLength(1), reason: 'no reconnect');
    // Resolution is separate; old oversized/half and orientation controls
    // remain absent on handheld.
    await _openScreenPanel(tester);
    for (final gone in const [
      'android-screen-resolution-half',
      'android-screen-resolution-w200',
      'android-screen-orientation-landscape',
    ]) {
      expect(find.byKey(Key(gone)), findsNothing, reason: gone);
    }
    expect(find.byKey(const Key('android-screen-resolution')), findsOneWidget);
    await _close(tester);
    expect(orientations, isEmpty);
  });

  testWidgets('a tablet recommendation includes density and bounded source; '
      'manual scale remains per-class', (tester) async {
    final h = await _open(
      tester,
      settings: const AndroidSessionSettings(helpSeen: true),
      resizable: true,
      grantedScale: 175,
      physical: const Size(2560, 1600),
      devicePixelRatio: 2,
      density: const AndroidScreenDensity(
        xdpi: 274.3,
        ydpi: 274.3,
        width: 2560,
        height: 1600,
      ),
      picture: const Size(1462, 914),
    );
    expect(h.requestedScales.first, 100);
    expect(
      (h.requested.first.maxWidth, h.requested.first.maxHeight),
      (1462, 914),
    );
    // A choice made here belongs to tablets only.
    await _openScale(tester);
    expect(find.textContaining('태블릿'), findsOneWidget);
    final scale = find.byKey(const Key('android-screen-scale-200'));
    await tester.ensureVisible(scale);
    await tester.pumpAndSettle();
    await tester.tap(scale);
    await tester.pumpAndSettle();
    expect(h.saved.last.screen(AndroidDeviceClass.tablet).scale, 200);
    expect(h.saved.last.screen(AndroidDeviceClass.phone).scale, 0);
    await _close(tester);
  });

  for (final audio in [true, false]) {
    testWidgets('화질 (the former 설정): 재연결 waits for a change; '
      '절약 reconnects once with 30 fps, fixed 8 Mbps and sound (connection audio=$audio)', (tester) async {
    final h = await _open(tester, options: StreamOptions(audio: audio));
    expect(h.requested, hasLength(1));
    await _openPanel(tester, 'settings', 'android-settings-panel');
    // The panel carries its row's name.
    expect(
      find.descendant(
        of: find.byKey(const Key('android-settings-panel')),
        matching: find.text('화질'),
      ),
      findsOneWidget,
    );
    expect(find.text('스트리밍 설정'), findsNothing);
    // 취소 goes back to the 설정 panel it was opened from, not to the menu,
    // and reconnects nothing.
    final cancel = find.byKey(const Key('cancel-stream-settings'));
    await tester.ensureVisible(cancel);
    await tester.pumpAndSettle();
    await tester.tap(cancel);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-settings-panel')), findsNothing);
    expect(find.byKey(const Key('android-more-panel')), findsOneWidget);
    expect(find.byKey(const Key('session-command-bar')), findsNothing);
    expect(h.requested, hasLength(1));
    await _openPanel(tester, 'settings', 'android-settings-panel');
    final apply = find.byKey(const Key('apply-stream-settings'));
    expect(tester.widget<InkWell>(apply).onTap, isNull, reason: 'no change');
    await tester.tap(find.byKey(const Key('stream-preset-saver')));
    await tester.pumpAndSettle();
    expect(find.textContaining('절약'), findsWidgets);
    await tester.tap(apply);
    for (var i = 0; i < 10; i++) {
      await tester.pump(const Duration(milliseconds: 50));
    }
    expect(h.requested, hasLength(2), reason: 'one reconnect');
    final asked = h.requested.last;
    expect(
      (asked.fps, asked.bitrate, asked.automaticBitrate),
      (30, 8000000, false),
    );
    // Sound is always on (2026-11-11): the reconnect asks for it.
    expect(asked.audio, isTrue);
    expect(find.byKey(const Key('session-command-bar')), findsNothing);
    expect(find.byKey(const Key('android-settings-panel')), findsNothing);
    await _close(tester);
  });
  }
}

class _FakeNetwork implements AndroidNetworkEvents {
  final events = StreamController<String>.broadcast();
  int marks = 0;
  @override
  Stream<String> get changes => events.stream;
  @override
  Future<void> mark() async => marks++;
}

// --- restart recovery (host-ready-wait-v1) ---------------------------------

const _refused = BrokerException('network_error', 0, 'connection_refused');

/// The VM runs; [host]: its Host has reported to the (new) broker.
BrokerDesktop _restartDesktop({required bool host}) => BrokerDesktop(
  id: 'pve-149',
  name: 'TESTER',
  kind: 'vm',
  description: '',
  os: 'Windows',
  status: host ? 'ready' : 'host_offline',
  canConnect: host,
  hostConnected: host,
  power: BrokerPowerSnapshot(
    state: 'running',
    stale: false,
    checkedAt: DateTime.utc(2026, 11, 3),
  ),
);

/// A broker whose list answers [list] at the moment of each read (a desktop,
/// or an error to throw). No host-ready: a released broker.
class _RestartBroker implements BrokerApi {
  Object Function() list = () => _restartDesktop(host: true);
  int reads = 0;

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    reads++;
    final answer = list();
    if (answer is BrokerDesktop) return [answer];
    throw answer;
  }

  @override
  dynamic noSuchMethod(Invocation invocation) =>
      throw UnimplementedError(invocation.memberName.toString());
}

/// host-ready-wait-v1 as the broker answers it: true the moment the Host
/// reported ([report]), false after a 5 s hold.
class _HostReadyBroker extends _RestartBroker implements HostReadyBrokerApi {
  int asks = 0;
  final _reported = Completer<void>();
  void report() {
    if (!_reported.isCompleted) _reported.complete();
  }

  @override
  Future<bool?> waitHostReady(String token, String desktopId) {
    asks++;
    final answer = Completer<bool?>();
    final hold = Timer(const Duration(seconds: 5), () {
      if (!answer.isCompleted) answer.complete(false);
    });
    unawaited(
      _reported.future.then((_) {
        hold.cancel();
        if (!answer.isCompleted) answer.complete(true);
      }),
    );
    return answer.future;
  }
}

/// The view's wait, wired as android_app wires it, on [broker].
Future<void> Function(bool Function(), Duration) _restartWait(
  BrokerApi broker,
) =>
    (current, limit) => androidAwaitBrokerRestart(
      broker,
      'not-persisted',
      machineFromBroker(_restartDesktop(host: true)),
      current: current,
      limit: limit,
    );

void _serverStopping(_Harness h) => h.socket.incoming.add(
  jsonEncode({'type': 'session.ended', 'reason': 'server_stopping'}),
);

/// The VM runs; its Host is under [status] of an update ([host]: connected).
BrokerDesktop _updateDesktop(String status, {bool host = true}) =>
    BrokerDesktop(
      id: 'pve-149',
      name: 'TESTER',
      kind: 'vm',
      description: '',
      os: 'Windows',
      status: host ? 'ready' : 'host_offline',
      canConnect: host,
      hostConnected: host,
      power: BrokerPowerSnapshot(
        state: 'running',
        stale: false,
        checkedAt: DateTime.utc(2026, 11, 3),
      ),
      hostUpdate: HostUpdateState(
        present: true,
        connected: host,
        supported: true,
        version: host ? '0.2.130' : null,
        latestVersion: '0.2.131',
        operation: HostUpdateOperation(
          status: status,
          targetVersion: '0.2.131',
          checkedAt: DateTime.now().toUtc(),
        ),
      ),
    );

/// The view's Host update wait, wired as android_app wires it, on [broker].
Future<void> Function(bool Function(), Duration) _updateWait(
  BrokerApi broker,
) =>
    (current, limit) => androidAwaitBrokerRestart(
      broker,
      'not-persisted',
      machineFromBroker(_restartDesktop(host: true)),
      current: current,
      limit: limit,
      hostUpdating: true,
    );

void _hostUpdateEnd(_Harness h) => h.socket.incoming.add(
  jsonEncode({'type': 'session.ended', 'reason': 'host_update'}),
);

Future<void> Function(bool Function(), Duration, void Function()) _returnWait(
  BrokerApi broker,
) =>
    (current, limit, away) => androidAwaitBrokerRestart(
      broker,
      'not-persisted',
      machineFromBroker(_restartDesktop(host: true)),
      current: current,
      limit: limit,
      hostAwayWait: AndroidRemoteView.hostAwayWait,
      onHostAway: away,
    );

/// Simulated (fake time, not a field result): from the server_stopping end to
/// the successful connect request. The broker is down until [brokerUpMs], its
/// Host reports at [hostMs]; without [recovery] the view has no restart wait
/// (the released back-off: a connect at 1, 3, 7, 15 s).
Future<int> _restartRecoveryMs(
  WidgetTester tester, {
  required int brokerUpMs,
  required int hostMs,
  required bool recovery,
}) async {
  var ms = 0;
  final broker = _HostReadyBroker()
    ..list = () =>
        ms < brokerUpMs ? throw _refused : _restartDesktop(host: ms >= hostMs);
  final h = await _open(
    tester,
    awaitBrokerRestart: recovery ? _restartWait(broker) : null,
  );
  int? connectedAt;
  // What AndroidStreamPreparation.connect meets at this moment.
  h.loadAnswer = () {
    if (ms < brokerUpMs) return _refused;
    if (ms < hostMs) return const BrokerException('display_not_supported');
    connectedAt ??= ms;
    return null;
  };
  final tick = Timer.periodic(const Duration(milliseconds: 1), (_) => ms++);
  final report = Timer(Duration(milliseconds: hostMs), broker.report);
  _serverStopping(h);
  for (var n = 0; n < 2000 && connectedAt == null; n++) {
    await tester.pump(const Duration(milliseconds: 10));
  }
  tick.cancel();
  report.cancel();
  broker.report();
  expect(connectedAt, isNotNull);
  await _frame(tester, h);
  await _close(tester);
  return connectedAt!;
}

void pixelScaleObservationRegressions({bool privateHarness = false}) {
  if (privateHarness) {
    TestWidgetsFlutterBinding.ensureInitialized();
    setUp(() {
      final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(_penChannel, (_) async => null);
      messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel,
        (call) async => call.method == 'start' ? 'started' : true);
    });
    tearDown(() {
      final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(_penChannel, null);
      messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
    });
  }
  for (final scenario in ['unknown', 'fractional', 'integer', 'zoom']) {
    testWidgets('actual Android rendered pixelScale observation: $scenario', (tester) async {
      final exact = scenario == 'integer' || scenario == 'zoom';
      final h = await _open(
        tester,
        physical: exact ? const Size(1280, 720) : const Size(1080, 2400),
        devicePixelRatio: exact ? 2 : 3,
        picture: const Size(1280, 720),
        firstFrame: scenario != 'unknown',
      );
      try {
        if (scenario == 'zoom') {
          await _pinchPicture(tester);
        }
        // This is the actual sink wrapper created by AndroidRemoteView.
        // It adds production _statsInput to the same session's scalar stats.
        h.session.statsSink!.call(const {'width': 1280, 'height': 720});
        expect(h.stats, isNotEmpty);
        final values = h.stats.last;
        if (scenario == 'unknown') {
          expect(values.containsKey('pixelScale'), false,
            reason: 'A plan without an actual shown view cannot prove raster scale.');
        } else {
          expect(values['pixelScale'], scenario == 'integer' ? 1 : 0,
            reason: 'Fractional fitting and zoom cannot inherit an integer plan value.');
        }
      } finally {
        await _close(tester);
      }
    });
  }
}

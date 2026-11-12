// This device's touch feedback in the real Android remote view: the haptic
// asked of the platform and the marks drawn over the picture, next to what
// the fake relay received. A fake socket and the native media fake; no
// device, vibrator, MediaCodec or host. How the haptics feel is not tested.
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_touch.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';

import '../remote_session_test.dart' show SessionSocket;
import 'android_session_contract_test.dart' show AndroidNativeFake;

const _feedback = Key('android-touch-feedback');
const _penChannel = MethodChannel('multitap/pen-input');

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

class _Rig {
  final sockets = <SessionSocket>[];

  /// The haptics asked of the platform, in order: the HapticFeedbackType
  /// name, or 'vibrate' (Android's LONG_PRESS).
  final haptics = <String>[];

  List<Map<String, dynamic>> get buttons => [
    for (final socket in sockets)
      for (final m in socket.sent)
        if (m['type'] == 'input' && m['event'] == 'button') m,
  ];

  List<String> get clicks => [
    for (final m in buttons)
      '${m['button'] == 'left' ? 'L' : 'R'}${m['down'] == true ? '↓' : '↑'}',
  ];
}

/// Real Windows touch granted (touch-input-v1): records the contact phases.
class _Contacts implements AndroidTouchContacts {
  final phases = <String>[];
  @override
  bool get available => true;
  @override
  void contact(int id, Offset point, AndroidContactPhase phase) =>
      phases.add(phase.name);
}

/// The remote view at 360×800 dp showing a 1280×720 picture (rows
/// 298.75..501.25), connected and drawing.
Future<_Rig> _open(
  WidgetTester tester,
  AndroidTouchMode mode, {
  AndroidTouchContacts? contacts,
}) async {
  tester.view.physicalSize = const Size(1080, 2400);
  tester.view.devicePixelRatio = 3;
  addTearDown(tester.view.reset);
  AndroidNativeFake().install();
  addTearDown(AndroidNativeFake.uninstall);
  final rig = _Rig();
  final messenger = tester.binding.defaultBinaryMessenger;
  messenger.setMockMethodCallHandler(SystemChannels.platform, (call) async {
    if (call.method == 'HapticFeedback.vibrate') {
      rig.haptics.add(
        (call.arguments as String?)?.split('.').last ?? 'vibrate',
      );
    }
    return null;
  });
  addTearDown(
    () => messenger.setMockMethodCallHandler(SystemChannels.platform, null),
  );
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: AndroidRemoteView(
        name: 'TESTER',
        token: 'not-persisted',
        options: const StreamOptions(),
        settings: AndroidSessionSettings(
          touch: mode,
          helpSeen: true,
          screens: const {
            AndroidDeviceClass.phone: AndroidScreenChoice(
              resolution: AndroidResolution.device,
            ),
          },
        ),
        density: () async => null,
        touchContacts: contacts == null ? null : (_) => contacts,
        load: (options, authorized, scale) async => BrokerConnection(
          reservation: BrokerSessionReservation(() async {}),
          sessionId: 'android-session',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/api/v1/sessions/android-session',
          ),
        ),
        sessionFactory: (connection, media, statsSink) => RemoteSession(
          connection: connection,
          token: 'not-persisted',
          media: media,
          statsSink: statsSink,
          dialer: (_, _) async {
            final socket = SessionSocket();
            rig.sockets.add(socket);
            return socket;
          },
        ),
        pictureBuilder: (_) => const ColoredBox(color: Color(0xFF2B4C6F)),
      ),
    ),
  );
  for (var i = 0; i < 4; i++) {
    await tester.pump(const Duration(milliseconds: 20));
  }
  rig.sockets.last.incoming.add(
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
  rig.sockets.last.incoming.add(_video(16667, key: true));
  for (var i = 0; i < 6; i++) {
    await tester.pump(const Duration(milliseconds: 40));
  }
  expect(find.byKey(const Key('android-remote-surface')), findsOneWidget);
  return rig;
}

Future<void> _close(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pump(const Duration(seconds: 1));
}

/// What the feedback overlay draws now (screen coordinates): circle centres
/// and arc sweeps.
({List<Offset> circles, List<double> arcs}) _drawn(WidgetTester tester) {
  final canvas = TestRecordingCanvas();
  final origin = tester.getTopLeft(find.byKey(_feedback));
  tester
      .renderObject(find.byKey(_feedback))
      .paint(TestRecordingPaintingContext(canvas), origin);
  final circles = <Offset>{}, arcs = <double>[];
  for (final call in canvas.invocations) {
    final args = call.invocation.positionalArguments;
    if (call.invocation.memberName == #drawCircle) {
      circles.add(args[0] as Offset);
    } else if (call.invocation.memberName == #drawArc) {
      arcs.add(args[2] as double);
    }
  }
  return (circles: circles.toList(), arcs: arcs);
}

/// Some drawn centre lies on [point] (picture points mapped back to the
/// screen round off).
Matcher _near(Offset point) => contains(
  predicate<Offset>((c) => (c - point).distance < .5, 'near $point'),
);

/// Spreads two fingers around the middle: the picture zooms to 3x on this
/// device only (the remote is not touched).
Future<void> _zoom(WidgetTester tester) async {
  final a = await tester.startGesture(const Offset(150, 400), pointer: 11);
  final b = await tester.startGesture(const Offset(210, 400), pointer: 12);
  for (var step = 1; step <= 6; step++) {
    await a.moveTo(Offset(150 - 10.0 * step, 400));
    await b.moveTo(Offset(210 + 10.0 * step, 400));
    await tester.pump(const Duration(milliseconds: 16));
  }
  await a.up();
  await b.up();
  await tester.pump(const Duration(milliseconds: 400));
}

/// The picture point (0..1) of a button message.
Offset _at(Map<String, dynamic> m) =>
    Offset((m['x'] as num).toDouble(), (m['y'] as num).toDouble());

void main() {
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(_penChannel, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? 'started' : true,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(_penChannel, null);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });

  testWidgets('터치: a tap is felt at once and marked where it clicks, also '
      'on a zoomed picture; the mark takes no touch', (tester) async {
    final rig = await _open(tester, AndroidTouchMode.direct);
    await _zoom(tester);
    rig.haptics.clear();
    expect(rig.buttons, isEmpty);

    const finger = Offset(120, 420);
    final tap = await tester.startGesture(finger, pointer: 13);
    await tester.pump(const Duration(milliseconds: 40));
    await tap.up();
    await tester.pump();
    expect(rig.haptics, ['lightImpact'], reason: 'felt at the lift');
    expect(rig.clicks, isEmpty, reason: 'the click waits for a double tap');
    expect(_drawn(tester).circles, _near(finger));
    // The mark is drawn on top but never takes the touch.
    final hits = tester.hitTestOnBinding(finger).path.map((e) => e.target);
    final marks = find.descendant(
      of: find.byKey(_feedback),
      matching: find.byType(CustomPaint),
    );
    expect(hits, isNot(contains(tester.renderObject(marks))));
    expect(
      hits,
      contains(
        tester.renderObject(find.byKey(const Key('android-remote-surface'))),
      ),
    );

    await tester.pump(const Duration(milliseconds: 320));
    expect(rig.clicks, ['L↓', 'L↑']);
    // Zoomed: the click is not where the unzoomed picture had this point.
    final landed = _at(rig.buttons.first);
    expect(landed.dx, isNot(closeTo(120 / 360, .02)));
    expect(_drawn(tester).circles, isEmpty, reason: 'gone within 300 ms');
    await _close(tester);
  });

  testWidgets('터치: the marks stay on the clicked picture point while a '
      'zoomed picture moves under a drag at the edge', (tester) async {
    final rig = await _open(tester, AndroidTouchMode.direct);
    await _zoom(tester);
    await tester.tapAt(const Offset(60, 420));
    await tester.pump(const Duration(milliseconds: 100));
    final drag = await tester.startGesture(const Offset(62, 420));
    await tester.pump(const Duration(milliseconds: 16));
    // Each move near the left edge brings 10 dp more of the picture in.
    for (final x in const [30.0, 25.0, 20.0]) {
      await drag.moveTo(Offset(x, 420));
      await tester.pump(const Duration(milliseconds: 16));
    }
    expect(rig.clicks, ['L↓']);
    final drawn = _drawn(tester).circles;
    expect(drawn, _near(const Offset(92, 420)), reason: 'the drag start');
    expect(drawn, isNot(_near(const Offset(62, 420))));
    await drag.up();
    await tester.pump(const Duration(seconds: 1));
    await _close(tester);
  });

  testWidgets('터치 with real Windows touch: each contact is marked where it '
      'lands; Windows interprets it, so no mouse and no haptic', (
    tester,
  ) async {
    final contacts = _Contacts();
    final rig = await _open(
      tester,
      AndroidTouchMode.direct,
      contacts: contacts,
    );
    final a = await tester.startGesture(const Offset(100, 400), pointer: 41);
    final b = await tester.startGesture(const Offset(250, 450), pointer: 42);
    await tester.pump();
    final drawn = _drawn(tester).circles;
    expect(drawn, _near(const Offset(100, 400)));
    expect(drawn, _near(const Offset(250, 450)));
    await tester.pump(const Duration(milliseconds: 700));
    expect(_drawn(tester).arcs, isEmpty, reason: 'no long-press ring');
    await a.up();
    await b.up();
    await tester.pump(const Duration(seconds: 1));
    expect(contacts.phases, ['down', 'down', 'up', 'up']);
    expect(rig.clicks, isEmpty);
    expect(rig.haptics, isEmpty);
    await _close(tester);
  });

  testWidgets('터치: holding fills a ring, then right-clicks with the '
      'long-press haptic and a flash there', (tester) async {
    final rig = await _open(tester, AndroidTouchMode.direct);
    const finger = Offset(100, 400);
    final hold = await tester.startGesture(finger, pointer: 21);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(_drawn(tester).arcs, isEmpty, reason: 'a quick tap shows no ring');
    await tester.pump(const Duration(milliseconds: 150));
    final ring = _drawn(tester);
    expect(ring.arcs, hasLength(1));
    expect(ring.arcs.single, closeTo(3.1416, .2), reason: 'half way');
    expect(ring.circles, _near(finger));
    expect(rig.clicks, isEmpty);
    expect(rig.haptics, isEmpty);

    await tester.pump(const Duration(milliseconds: 230));
    expect(rig.clicks, isEmpty);
    await tester.pump(const Duration(milliseconds: 30));
    expect(rig.clicks, ['R↓', 'R↑']);
    expect(rig.haptics, ['vibrate']);
    final flash = _drawn(tester);
    expect(flash.arcs, isEmpty, reason: 'the ring became the flash');
    expect(flash.circles, _near(finger));
    expect(
      _at(rig.buttons.first),
      within(distance: .002, from: Offset(100 / 360, (400 - 298.75) / 202.5)),
    );
    await hold.up();
    await tester.pump(const Duration(seconds: 1));
    expect(rig.clicks, ['R↓', 'R↑'], reason: 'no click at the lift');
    expect(rig.haptics, ['vibrate']);
    await _close(tester);
  });

  testWidgets('터치: a double-tap drag ticks when it starts and when it is '
      'released', (tester) async {
    final rig = await _open(tester, AndroidTouchMode.direct);
    await tester.tapAt(const Offset(100, 400));
    await tester.pump(const Duration(milliseconds: 100));
    final drag = await tester.startGesture(const Offset(102, 400));
    await tester.pump(const Duration(milliseconds: 16));
    await drag.moveBy(const Offset(40, 0));
    await tester.pump(const Duration(milliseconds: 16));
    expect(rig.clicks, ['L↓']);
    expect(rig.haptics, ['lightImpact', 'lightImpact']);
    expect(_drawn(tester).circles, _near(const Offset(102, 400)));
    await drag.moveBy(const Offset(40, 0));
    await drag.up();
    await tester.pump();
    expect(rig.clicks, ['L↓', 'L↑']);
    expect(rig.haptics, ['lightImpact', 'lightImpact', 'selectionClick']);
    await tester.pump(const Duration(seconds: 1));
    await _close(tester);
  });

  testWidgets('포인터: a tap ticks lightly and pulses at the cursor; a '
      'two-finger tap right-clicks there with the long-press haptic', (
    tester,
  ) async {
    final rig = await _open(tester, AndroidTouchMode.trackpad);
    // The cursor rests in the middle of the picture; the finger is far off.
    const cursor = Offset(180, 400);
    await tester.tapAt(const Offset(60, 700));
    await tester.pump();
    expect(rig.haptics, ['lightImpact']);
    expect(_drawn(tester).circles, _near(cursor));
    await tester.pump(const Duration(milliseconds: 320));
    expect(rig.clicks, ['L↓', 'L↑']);
    expect(
      _at(rig.buttons.first),
      within(distance: .002, from: const Offset(.5, .5)),
    );

    await tester.pump(const Duration(milliseconds: 400));
    final a = await tester.startGesture(const Offset(100, 650), pointer: 31);
    final b = await tester.startGesture(const Offset(160, 650), pointer: 32);
    await tester.pump(const Duration(milliseconds: 60));
    await b.up();
    await a.up();
    await tester.pump();
    expect(rig.clicks, ['L↓', 'L↑', 'R↓', 'R↑']);
    expect(rig.haptics, ['lightImpact', 'vibrate']);
    expect(_drawn(tester).circles, _near(cursor));
    await tester.pump(const Duration(seconds: 1));
    await _close(tester);
  });
}

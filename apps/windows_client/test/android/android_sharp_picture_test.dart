// 화면 › 해상도 › 선명하게 on Android: the choice, how the remote output and
// Windows' scale are planned from the engine's answer, and the fallbacks to
// the automatic size (the engine's, a refused resize, a picture the desktop
// encodes smaller). The engine's rule itself is SharpPictureTest.kt.
//
// The real AndroidRemoteView with the shared RemoteSession over a fake relay
// socket and a recorded engine. No phone, decoder, Host or VM: nothing here
// says a panel shows sharper text.
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_engine_stats.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_sharp_picture.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_size.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';

import '../remote_session_test.dart' show SessionSocket;
import 'android_session_contract_test.dart' show AndroidNativeFake;

const _sessionUrl = 'wss://fixture.invalid/api/v1/sessions/android-session';
const _pen = MethodChannel('multitap/pen-input');

/// The engine's side, recorded. [answer] builds the plan (null: cannot say).
class _Engine implements AndroidSharpPicture {
  final asked = <({Size panel, Size base, int fps, bool portrait})>[];
  final fallbacks = <String>[];
  int offs = 0;
  AndroidSharpPlan? Function(Size panel, Size base) answer = (panel, base) =>
      AndroidSharpPlan(
        width: panel.width.floor() ~/ 2 * 2,
        height: panel.height.floor() ~/ 2 * 2,
        baseWidth: base.width.round(),
        baseHeight: base.height.round(),
        scale: 175,
      );

  @override
  Future<AndroidSharpPlan?> plan({
    required Size panel,
    required Size base,
    required int fps,
    required bool portrait,
  }) async {
    asked.add((panel: panel, base: base, fps: fps, portrait: portrait));
    return answer(panel, base);
  }

  @override
  void hostFallback(String reason) => fallbacks.add(reason);
  @override
  void off() => offs++;
}

class _Harness {
  final engine = _Engine();
  final sockets = <SessionSocket>[];
  final requested = <StreamOptions>[];
  final scales = <int?>[];
  final saved = <AndroidSessionSettings>[];
  late RemoteSession session;
  SessionSocket get socket => sockets.last;
  List<Map<String, dynamic>> sent(String type) => [
    for (final socket in sockets)
      for (final message in socket.sent)
        if (message['type'] == type) message,
  ];
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

const _phone = AndroidDeviceClass.phone;

/// A 1080x2160 phone at 2.625 pixels per dp (its automatic size is about
/// 658x1316), on a Host that resizes the stream and sets Windows' scale
/// ([hostScale]).
Future<_Harness> _open(
  WidgetTester tester, {
  AndroidResolution resolution = AndroidResolution.auto,
  bool hostScale = true,
  void Function(_Engine engine)? prepare,
}) async {
  tester.view.physicalSize = const Size(1080, 2160);
  tester.view.devicePixelRatio = 2.625;
  addTearDown(tester.view.reset);
  AndroidNativeFake().install();
  addTearDown(AndroidNativeFake.uninstall);
  final h = _Harness();
  prepare?.call(h.engine);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: AndroidRemoteView(
        name: 'VM173',
        token: 'not-persisted',
        options: const StreamOptions(),
        settings: AndroidSessionSettings(
          helpSeen: true,
          screens: {_phone: AndroidScreenChoice(resolution: resolution)},
        ),
        onSettings: h.saved.add,
        displayScale: hostScale,
        decoderLimits: () async => null,
        density: () async => null,
        sharpPicture: h.engine,
        load: (options, _, scale) async {
          h.requested.add(options);
          h.scales.add(scale);
          return BrokerConnection(
            sessionId: 'android-session',
            webSocketUrl: Uri.parse(_sessionUrl),
            options: options,
            epochMedia: hostScale,
            viewportProvisioning: hostScale,
            displayScale: hostScale ? scale : null,
          );
        },
        sessionFactory: (connection, media, statsSink) =>
            h.session = RemoteSession(
              connection: connection,
              token: 'not-persisted',
              media: media,
              statsSink: statsSink,
              dialer: (_, _) async {
                final socket = SessionSocket();
                h.sockets.add(socket);
                return socket;
              },
            ),
        pictureBuilder: (_) => const ColoredBox(color: Color(0xFF2B4C6F)),
      ),
    ),
  );
  for (var i = 0; i < 6; i++) {
    await tester.pump(const Duration(milliseconds: 20));
  }
  final asked = h.requested.last;
  await _picture(tester, h, asked.maxWidth, asked.maxHeight);
  expect(find.byKey(const Key('android-remote-surface')), findsOneWidget);
  return h;
}

/// The Host's picture at [width]x[height] (its source the same, or [source]).
Future<void> _picture(
  WidgetTester tester,
  _Harness h,
  int width,
  int height, {
  Size? source,
  String? limit,
}) async {
  h.socket.incoming.add(
    jsonEncode({
      'type': 'stream.info',
      'codec': 'h264',
      'width': width,
      'height': height,
      'sourceWidth': source?.width.toInt() ?? width,
      'sourceHeight': source?.height.toInt() ?? height,
      'encodeLimit': ?limit,
    }),
  );
  await tester.pump(const Duration(milliseconds: 20));
  h.socket.incoming.add(_video(16667, key: true));
  for (var i = 0; i < 6; i++) {
    await tester.pump(const Duration(milliseconds: 40));
  }
}

Future<void> _close(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pump(const Duration(seconds: 3));
}

Future<void> _openScreen(WidgetTester tester) async {
  await tester.tap(find.byKey(const Key('expand-session-toolbar')));
  await tester.pumpAndSettle();
  await tester.tap(find.byKey(const Key('android-session-screen')));
  await tester.pumpAndSettle();
  expect(find.byKey(const Key('android-screen-panel')), findsOneWidget);
}

/// Fixed frames: a resize in flight keeps its progress moving, so nothing
/// here waits for the screen to settle. Long enough for the resize's delay.
Future<void> _frames(WidgetTester tester, [int count = 24]) async {
  for (var i = 0; i < count; i++) {
    await tester.pump(const Duration(milliseconds: 50));
  }
}

/// A 해상도 choice of the open 화면 panel.
Future<void> _choose(WidgetTester tester, Finder choice) async {
  await tester.ensureVisible(choice);
  await tester.pumpAndSettle();
  await tester.tap(choice);
  await _frames(tester);
}

final _sharp = find.byKey(const Key('android-screen-resolution-sharp'));

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
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

  // ---------------------------------------------------------------- choice

  test('선명하게 is a 해상도 choice of its own, remembered per kind of screen', () {
    expect(AndroidResolution.sharp.encode(), 'sharp');
    expect(AndroidResolution.parse('sharp'), AndroidResolution.sharp);
    const choice = AndroidScreenChoice(resolution: AndroidResolution.sharp);
    expect(AndroidScreenChoice.parse(choice.encode()), choice);
    // Kept for a phone, a foldable's inner screen and a tablet.
    for (final kind in const [
      AndroidDeviceClass.phone,
      AndroidDeviceClass.fold,
      AndroidDeviceClass.tablet,
    ]) {
      expect(choice.forDevice(kind).resolution, AndroidResolution.sharp);
    }
    // The cover screen may have it while the inner screen stays automatic.
    final settings = const AndroidSessionSettings(helpSeen: true)
        .withScreen(AndroidDeviceClass.phone, choice);
    expect(
      settings.screen(AndroidDeviceClass.phone).resolution,
      AndroidResolution.sharp,
    );
    expect(
      settings.screen(AndroidDeviceClass.fold).resolution,
      AndroidResolution.auto,
    );
    // 자동 stays the default.
    expect(const AndroidScreenChoice().resolution, AndroidResolution.auto);
    expect(AndroidResolution.parse('nonsense'), AndroidResolution.auto);
  });

  test('It is offered after 자동 only where Windows\' scale can follow and the '
      'panel has more pixels than the automatic size', () {
    const panel = Size(1080, 2400);
    List<String> labels({bool sharp = true, double dpr = 2.625, Size? size}) =>
        androidResolutionOptions(
          size ?? panel,
          policy: AndroidScreenPolicy(window: (size ?? panel) / dpr),
          devicePixelRatio: dpr,
          sharp: sharp,
        ).map((option) => option.label).toList();
    expect(labels().take(2), ['자동 (권장)', '선명하게']);
    expect(labels(sharp: false).contains('선명하게'), isFalse);
    // 1.6 pixels per dp or less: the automatic size already is the panel's.
    expect(labels(dpr: 1.5).contains('선명하게'), isFalse);
    // A TV keeps its own list.
    expect(
      androidResolutionOptions(
        const Size(1920, 1080),
        deviceClass: AndroidDeviceClass.television,
        sharp: true,
      ).map((option) => option.label).contains('선명하게'),
      isFalse,
    );
    final option = androidResolutionOptions(
      panel,
      policy: AndroidScreenPolicy(window: panel / 2.625),
      devicePixelRatio: 2.625,
      sharp: true,
    )[1];
    expect(option.resolution, AndroidResolution.sharp);
    expect(option.size, const Size(1080, 2400));
  });

  test('The output is the planned size with the chosen Windows scale, within '
      'the stream\'s limits; without a plan it is the automatic size', () {
    const panel = Size(1080, 2400);
    AndroidScreenPolicy policy(Size? sharp, {int scale = 175}) =>
        AndroidScreenPolicy(
          window: panel / 2.625,
          choice: AndroidScreenChoice(
            scale: scale,
            resolution: AndroidResolution.sharp,
          ),
          sharp: sharp,
        );
    final automatic = AndroidViewportPlan.of(
      panel,
      2.625,
      AndroidScreenPolicy(
        window: panel / 2.625,
        choice: const AndroidScreenChoice(scale: 100),
      ),
      hostScale: true,
    );
    expect(automatic.width, lessThan(1080));
    expect(automatic.displayScale, 100);

    final sharp = AndroidViewportPlan.of(
      panel,
      2.625,
      policy(const Size(1080, 2400)),
      hostScale: true,
      portraitLimits: const DecoderLimits(
        maxWidth: 4096,
        maxHeight: 4096,
        maxPixels: 4096 * 4096,
        maxPixelRate: 0,
      ),
    );
    expect((sharp.width, sharp.height), (1080, 2400));
    expect(sharp.displayScale, 175);
    // One device pixel per remote pixel: drawn without smoothing.
    expect(sharp.pixelScale, 1);

    // Not planned (or fallen back): exactly today's output.
    final none = AndroidViewportPlan.of(
      panel,
      2.625,
      policy(null, scale: 100),
      hostScale: true,
    );
    expect((none.width, none.height), (automatic.width, automatic.height));

    // A released stream is at most 2160 rows: the planned size is fitted in
    // proportion, never cut.
    final bounded = AndroidViewportPlan.of(
      panel,
      2.625,
      policy(const Size(1080, 2400)),
      hostScale: true,
    );
    expect(bounded.height, lessThanOrEqualTo(2160));
    expect(bounded.width / bounded.height, closeTo(1080 / 2400, .01));
    expect(bounded.width, greaterThan(automatic.width));
  });

  // ---------------------------------------------------------------- engine

  test('The engine is asked with the panel, the automatic size, the frame '
      'rate and the connection\'s ceiling; anything else is no plan', () async {
    const channel = MethodChannel('multitap/remote-sharp-test');
    final calls = <MethodCall>[];
    Object? reply = {
      'width': 2176,
      'height': 1642,
      'scale': 200,
      'reason': null,
      'fallback': false,
    };
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          calls.add(call);
          if (reply is Exception) throw reply;
          return call.method == 'sharpPicture' ? reply : null;
        });
    addTearDown(
      () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null),
    );
    const engine = NativeAndroidSharpPicture(channel);
    Future<AndroidSharpPlan?> ask({bool portrait = true}) => engine.plan(
      panel: const Size(2176.4, 1642.9),
      base: const Size(1142, 862),
      fps: 60,
      portrait: portrait,
    );
    final plan = await ask();
    expect(calls.single.method, 'sharpPicture');
    expect(calls.single.arguments, {
      'panelWidth': 2176,
      'panelHeight': 1642,
      'baseWidth': 1142,
      'baseHeight': 862,
      'fps': 60,
      'hardware': true,
      'limitWidth': 3840,
      'limitHeight': 3840,
      'limitPixels': 8294400,
    });
    expect((plan!.width, plan.height, plan.scale), (2176, 1642, 200));
    expect(plan.reason, isNull);
    expect(plan.gains, isTrue);
    expect(plan.factor, closeTo(1.905, .001));
    // Without stream-size the released 3840x2160.
    await ask(portrait: false);
    expect((calls.last.arguments as Map)['limitHeight'], 2160);

    reply = {
      'width': 1142,
      'height': 862,
      'scale': 100,
      'reason': 'decoder_size',
      'fallback': true,
    };
    final fallen = await ask();
    expect(fallen!.fallback, isTrue);
    expect(fallen.gains, isFalse);
    expect(fallen.reason, 'decoder_size');

    for (final bad in <Object?>[
      null,
      'x',
      {'width': 2177, 'height': 1642, 'scale': 200},
      {'width': 1000, 'height': 1642, 'scale': 200},
      {'width': 2176, 'height': 1642, 'scale': 190},
      {'width': 2176, 'height': 1642, 'scale': 200, 'reason': 'Has Spaces'},
      {'width': 40000, 'height': 1642, 'scale': 200},
      PlatformException(code: 'engine_closed'),
    ]) {
      reply = bad;
      expect(await ask(), isNull, reason: '$bad');
    }

    engine.hostFallback('host_refused');
    engine.off();
    await Future<void>.delayed(Duration.zero);
    expect(calls[calls.length - 2].method, 'sharpPictureFallback');
    expect(calls[calls.length - 2].arguments, {'reason': 'host_refused'});
    expect(calls.last.method, 'sharpPictureOff');
  });

  test('The statistics say what 선명하게 got and how often it fell back', () {
    String row(Map<String, dynamic>? stats) =>
        androidEngineMetrics(stats).firstWhere((row) => row.$1 == '선명하게').$2;
    expect(row({}), '미측정');
    expect(row({'sharpRequested': false, 'sharpFallbacks': 0}), '꺼짐');
    expect(
      row({
        'sharpRequested': true,
        'sharpWidth': 2176,
        'sharpHeight': 1642,
        'sharpScale': 200,
        'sharpReason': null,
        'sharpFallbacks': 0,
      }),
      '켜짐 · 기기 화소 그대로 2176×1642',
    );
    expect(
      row({
        'sharpRequested': true,
        'sharpWidth': 1400,
        'sharpHeight': 1056,
        'sharpReason': 'decoder_size',
        'sharpFallbacks': 0,
      }),
      '켜짐 · 1400×1056 (이 기기 디코더 크기 한도)',
    );
    expect(
      row({
        'sharpRequested': true,
        'sharpWidth': 1316,
        'sharpHeight': 2924,
        'sharpReason': 'scale_limit',
        'sharpFallbacks': 0,
      }),
      '켜짐 · 1316×2924 (Windows 배율 200% 한도)',
    );
    expect(
      row({
        'sharpRequested': true,
        'sharpWidth': 1142,
        'sharpHeight': 862,
        'sharpReason': 'decoder_rate',
        'sharpFallbacks': 2,
        'sharpFallbackReason': 'host_refused',
      }),
      '켜짐 · 1142×862 (이 기기 디코더 속도 한도) · 자동 크기로 돌아감 2회 (데스크톱 한도)',
    );
    expect(
      row({
        'sharpRequested': false,
        'sharpFallbacks': 1,
        'sharpFallbackReason': 'memory_budget',
      }),
      '꺼짐 · 자동 크기로 돌아감 1회 (이 기기 메모리)',
    );
    for (final text in [
      androidSharpLabel,
      androidSharpDetail,
      androidSharpHint,
    ]) {
      expect(text.contains('PC'), isFalse);
    }
    expect(androidSharpHint, contains('데이터와 배터리'));
  });

  // ------------------------------------------------------------------ view

  testWidgets('자동 stays the default: nothing is asked of the engine and the '
      'output is the automatic size at 100 %', (tester) async {
    final h = await _open(tester);
    try {
      expect(h.engine.asked, isEmpty);
      expect(h.requested.single.maxWidth, lessThan(1080));
      expect(h.scales.single, 100);
      await _openScreen(tester);
      await tester.ensureVisible(_sharp);
      await tester.pumpAndSettle();
      // Named plainly, with what it costs, before it is chosen.
      expect(
        find.descendant(of: _sharp, matching: find.text('선명하게')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: _sharp, matching: find.text('데이터·배터리 더 씀')),
        findsOneWidget,
      );
      expect(tester.getSize(_sharp).height, greaterThanOrEqualTo(48));
      expect(h.engine.asked, isEmpty);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('Choosing 선명하게 asks the engine once, resizes to the planned '
      'size, raises Windows\' scale and is remembered', (tester) async {
    final h = await _open(tester);
    try {
      final automatic = h.requested.single;
      await _openScreen(tester);
      await _choose(tester, _sharp);
      expect(h.engine.asked, hasLength(1));
      final asked = h.engine.asked.single;
      expect(asked.panel, const Size(1080, 2160));
      expect(
        (asked.base.width, asked.base.height),
        (automatic.maxWidth.toDouble(), automatic.maxHeight.toDouble()),
      );
      expect(asked.fps, 60);
      expect(
        h.saved.last.screen(_phone).resolution,
        AndroidResolution.sharp,
        reason: 'remembered for this kind of screen',
      );
      final resize = h.sent('stream.resize').last;
      expect((resize['width'], resize['height']), (1080, 2160));
      expect(h.sent('display.scale').last['scale'], 175);
      expect(h.requested, hasLength(1), reason: 'no reconnect');
      expect(h.engine.fallbacks, isEmpty);
      // The same area again asks nothing more.
      await _frames(tester);
      expect(h.engine.asked, hasLength(1));
      expect(h.sent('stream.resize'), hasLength(1));
    } finally {
      await _close(tester);
    }
  });

  testWidgets('Remembered: the first request of a connection already asks for '
      'the planned size and scale; 자동 returns both and tells the engine', (
    tester,
  ) async {
    final h = await _open(tester, resolution: AndroidResolution.sharp);
    try {
      expect(h.engine.asked, hasLength(1));
      expect(
        (h.requested.single.maxWidth, h.requested.single.maxHeight),
        (1080, 2160),
      );
      expect(h.scales.single, 175);
      expect(h.sent('stream.resize'), isEmpty);
      expect(h.engine.fallbacks, isEmpty);
      final automatic = h.engine.asked.single.base;

      await _openScreen(tester);
      await _choose(
        tester,
        find.byKey(const Key('android-screen-resolution-auto')),
      );
      final back = h.sent('stream.resize').last;
      expect(
        (back['width'], back['height']),
        (automatic.width.round(), automatic.height.round()),
      );
      expect(h.sent('display.scale').last['scale'], 100);
      expect(h.engine.offs, greaterThanOrEqualTo(1));
      expect(h.saved.last.screen(_phone).resolution, AndroidResolution.auto);
      expect(h.requested, hasLength(1), reason: 'no reconnect');
    } finally {
      await _close(tester);
    }
  });

  testWidgets('A manual Windows scale carries over: 125 % of the automatic '
      'size stays 125 % of it when 선명하게 goes on', (tester) async {
    final h = await _open(
      tester,
      prepare: (engine) => engine.answer = (panel, base) => AndroidSharpPlan(
        width: base.width.round() * 8 ~/ 5 ~/ 2 * 2,
        height: base.height.round() * 8 ~/ 5 ~/ 2 * 2,
        baseWidth: base.width.round(),
        baseHeight: base.height.round(),
        scale: 150,
        reason: 'decoder_size', // 1.6 to one: 150 % by itself
      ),
    );
    try {
      await _openScreen(tester);
      final step = find.byKey(const Key('android-screen-scale-125'));
      await tester.ensureVisible(step);
      await tester.pumpAndSettle();
      await tester.tap(step);
      await tester.pumpAndSettle();
      expect(h.sent('display.scale').last['scale'], 125);
      await _choose(tester, _sharp);
      // 1.25 x 1.6 = 200 %.
      expect(h.sent('display.scale').last['scale'], 200);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('What the device cannot take is the automatic size, said once; '
      'an engine that cannot say is the automatic size too', (tester) async {
    for (final silent in [false, true]) {
      final h = await _open(
        tester,
        resolution: AndroidResolution.sharp,
        prepare: (engine) => engine.answer = (panel, base) => silent
            ? null
            : AndroidSharpPlan(
                width: base.width.round(),
                height: base.height.round(),
                baseWidth: base.width.round(),
                baseHeight: base.height.round(),
                scale: 100,
                reason: 'decoder_size',
                fallback: true,
              ),
      );
      try {
        expect(h.engine.asked, hasLength(1));
        expect(h.requested.single.maxWidth, lessThan(1080));
        expect(h.scales.single, 100);
        expect(
          find.text('이 기기에서는 더 선명하게 받을 수 없어 자동 크기로 보여요.'),
          silent ? findsNothing : findsOneWidget,
        );
        // The choice stays the person's; its note says why it is not in use.
        await _openScreen(tester);
        expect(find.textContaining('더 선명하게 받을 수 없어 자동 크기로 보여요'), findsWidgets);
      } finally {
        await _close(tester);
      }
    }
  });

  testWidgets('A desktop that refuses the size gets today\'s size back, is '
      'counted, and is not asked again in this view', (tester) async {
    final h = await _open(tester);
    try {
      final automatic = h.requested.single;
      await _openScreen(tester);
      await _choose(tester, _sharp);
      final resize = h.sent('stream.resize').last;
      expect((resize['width'], resize['height']), (1080, 2160));
      h.socket.incoming.add(
        jsonEncode({
          'type': 'stream.resize.failed',
          'requestId': resize['requestId'],
          'unchanged': true,
          'reason': 'unsupported_display_mode',
        }),
      );
      await _frames(tester, 8);
      expect(h.engine.fallbacks, ['host_refused']);
      expect(find.text('선명하게를 적용하지 못해 자동 크기로 보여요.'), findsOneWidget);
      expect(h.sent('display.scale').last['scale'], 100);
      // The stream still has the automatic size: nothing more is resized.
      await _frames(tester, 40);
      expect(h.sent('stream.resize'), hasLength(1));
      expect(h.session.sourceWidth, automatic.maxWidth);
      expect(h.engine.fallbacks, hasLength(1));
      // The choice is kept; its note says what happened.
      expect(h.saved.last.screen(_phone).resolution, AndroidResolution.sharp);
      expect(
        find.textContaining('선명하게를 적용하지 못해 자동 크기로 보여요 (데스크톱 한도)'),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('A picture the desktop encodes smaller than the planned size is '
      'not sharper: the automatic size, counted', (tester) async {
    final h = await _open(tester, resolution: AndroidResolution.sharp);
    try {
      expect(h.engine.fallbacks, isEmpty);
      final automatic = h.engine.asked.single.base;
      // The desktop has 1080x2160 but sends 810x1620 (its own load).
      h.session
        ..width = 810
        ..height = 1620
        ..encodeLimit = EncodeLimit.hostCpu
        ..notifyListeners();
      await _frames(tester);
      expect(h.engine.fallbacks, ['host_reduced']);
      expect(find.text('선명하게를 적용하지 못해 자동 크기로 보여요.'), findsOneWidget);
      final resize = h.sent('stream.resize').last;
      expect(
        (resize['width'], resize['height']),
        (automatic.width.round(), automatic.height.round()),
      );
      expect(h.sent('display.scale').last['scale'], 100);
    } finally {
      await _close(tester);
    }
  });

  testWidgets('Without a Windows scale to raise (an older Host) the choice is '
      'not offered and a remembered one is the automatic size', (tester) async {
    final h = await _open(
      tester,
      resolution: AndroidResolution.sharp,
      hostScale: false,
    );
    try {
      expect(h.engine.asked, isEmpty);
      expect(h.requested.single.maxWidth, lessThanOrEqualTo(1080));
      expect(h.scales.single, isNull);
    } finally {
      await _close(tester);
    }
  });
}

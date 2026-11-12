// A fake relay/media engine exercises real view requests.
// No device decoder, Host, VM or actual remote display is operated here.
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_size.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';

import '../remote_session_test.dart' show SessionSocket;
import 'android_session_contract_test.dart' show AndroidNativeFake;

const _optionalPenChannel = MethodChannel('multitap/pen-input');

const _full = DecoderLimits(
  maxWidth: 3840,
  maxHeight: 3840,
  maxPixels: 8294400,
  maxPixelRate: 0,
);
const _limited = DecoderLimits(
  maxWidth: 1920,
  maxHeight: 1080,
  maxPixels: 2097152,
  maxPixelRate: 2097152 * 60,
);

AndroidSessionSettings _settings(AndroidResolution resolution) =>
    AndroidSessionSettings(
      helpSeen: true,
      screens: {
        for (final kind in AndroidDeviceClass.values)
          kind: AndroidScreenChoice(resolution: resolution),
      },
    );

void _viewport(WidgetTester t, Size physical, double dpr) {
  t.view.physicalSize = physical;
  t.view.devicePixelRatio = dpr;
  addTearDown(t.view.reset);
}

Uint8List _video([int epoch = 0]) => Uint8List.fromList([
  1,
  1,
  epoch & 255,
  epoch >> 8,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  0x65,
]);

class LargeDesktopStream {
  late RemoteSession session;
  late SessionSocket socket;
  final starts = <StreamOptions>[],
      scales = <int?>[],
      saved = <AndroidSessionSettings>[];
}

Future<LargeDesktopStream> openLargeDesktopStream(
  WidgetTester t, {
  AndroidResolution resolution = AndroidResolution.device,
  Size physical = const Size(1280, 720),
  double dpr = 1,
  bool television = false,
  DecoderLimits? limits,
  AndroidScreenDensity? density,
  Size? source,
  GlobalKey? boundary,
  Brightness brightness = Brightness.dark,
  double textScale = 1,
  AndroidSessionSettings? settings,
  StreamOptions quality = const StreamOptions(),
  Object? firstFailure,
  bool firstFrame = true,
  bool takeover = false,
  bool streamSizeV2 = false,
}) async {
  _viewport(t, physical, dpr);
  AndroidNativeFake().install();
  addTearDown(AndroidNativeFake.uninstall);
  final h = LargeDesktopStream();
  final view = MaterialApp(
    debugShowCheckedModeBanner: false,
    theme: buildAppTheme(brightness: brightness),
    builder: (context, child) => MediaQuery(
      data: MediaQuery.of(context)
          .copyWith(textScaler: TextScaler.linear(textScale)),
      child: AndroidTvNavigation(television: television, child: child!),
    ),
    home: AndroidRemoteView(
      name: 'Local fixture',
      token: 'fixture-only',
      options: quality,
      settings: settings ?? _settings(resolution),
      takeover: takeover ? (offer, current) async {} : null,
      onSettings: h.saved.add,
      displayScale: true,
      streamSize: limits != null,
      streamSizeV2: streamSizeV2,
      decoderLimits: () async => limits,
      initialDensity: density,
      density: () async => density,
      load: (options, _, scale) async {
        h.starts.add(options);
        h.scales.add(scale);
        if (h.starts.length == 1 && firstFailure != null) throw firstFailure;
        return BrokerConnection(
          sessionId: 'large-desktop',
          webSocketUrl: Uri.parse('wss://fixture.invalid/large-desktop'),
          options: options,
          epochMedia: true,
          viewportProvisioning: true,
          displayScale: scale,
          streamSize: limits,
          streamSizeV2: streamSizeV2,
          viewportEncoderLimits: limits != null,
        );
      },
      sessionFactory: (connection, media, stats) => h.session = RemoteSession(
        connection: connection,
        media: media,
        token: 'fixture-only',
        statsSink: stats,
        dialer: (_, _) async => h.socket = SessionSocket(),
      ),
      pictureBuilder: (_) => const ColoredBox(
        key: Key('large-desktop-picture'),
        color: Color(0xFF243343),
      ),
    ),
  );
  await t.pumpWidget(
    boundary == null ? view : RepaintBoundary(key: boundary, child: view),
  );
  for (var i = 0; i < 5; i++) {
    await t.pump(const Duration(milliseconds: 20));
  }
  if (!firstFrame) return h;
  final size =
      source ??
      Size(
        h.starts.last.maxWidth.toDouble(),
        h.starts.last.maxHeight.toDouble(),
      );
  h.socket.incoming.add(
    jsonEncode({
      'type': 'stream.info',
      'codec': 'h264',
      'width': size.width.toInt(),
      'height': size.height.toInt(),
      'sourceWidth': size.width.toInt(),
      'sourceHeight': size.height.toInt(),
    }),
  );
  await t.pump(const Duration(milliseconds: 20));
  h.socket.incoming.add(_video());
  for (var i = 0; i < 6; i++) {
    await t.pump(const Duration(milliseconds: 40));
  }
  expect(h.session.frameReady, isTrue);
  return h;
}

Future<void> openLargeDesktopScreenPanel(
  WidgetTester t, {
  bool television = false,
}) async {
  if (television) {
    await t.sendKeyEvent(LogicalKeyboardKey.contextMenu);
    await t.pumpAndSettle();
    await t.tap(find.byKey(const Key('android-tv-menu-screen')));
    await t.pumpAndSettle();
  } else {
    await t.tap(find.byKey(const Key('expand-session-toolbar')));
    await t.pumpAndSettle();
    final screen = find.byKey(const Key('android-session-screen'));
    if (screen.evaluate().isEmpty) {
      await t.tap(find.byKey(const Key('session-toolbar-overflow')));
      await t.pumpAndSettle();
    }
    await t.tap(screen);
    await t.pumpAndSettle();
  }
  // The TV keeps all parts in one group of choices. A handheld panel has
  // 배율 (where the Host sets it) and then 해상도, each a group of its own
  // carrying the same choice: `find.byType(AndroidScreenOptions).last` is
  // the 해상도 one.
  expect(
    find.byType(AndroidScreenOptions),
    television ? findsOneWidget : findsWidgets,
  );
  expect(find.byKey(const Key('android-screen-panel')), findsOneWidget);
}

Future<void> _choose(WidgetTester t, int shortSide) async {
  final item = find.byKey(Key('android-screen-resolution-w$shortSide'));
  await t.ensureVisible(item);
  await t.pumpAndSettle();
  await t.tap(item);
  await t.pump();
}

Future<void> closeLargeDesktopStreamView(WidgetTester t) async {
  await t.pumpWidget(const SizedBox.shrink());
  await t.pump(const Duration(seconds: 1));
}

AndroidSessionSettings _recoverySettings() =>
    _settings(const AndroidResolution.workspace(200))
        .copyWith(
          touch: AndroidTouchMode.direct,
          speed: AndroidPointerSpeed.precise,
          toolbar: .3,
        )
        .withScreen(
          AndroidDeviceClass.television,
          const AndroidScreenChoice(
            resolution: AndroidResolution.workspace(200),
            scale: 175,
            orientation: AndroidOrientation.landscape,
          ),
        );

void _preservedSettings(
  AndroidSessionSettings saved,
  AndroidSessionSettings initial,
) {
  expect(
    saved.screen(AndroidDeviceClass.television),
    initial
        .screen(AndroidDeviceClass.television)
        .copyWith(resolution: AndroidResolution.auto),
  );
  expect(
    saved.screen(AndroidDeviceClass.phone),
    initial.screen(AndroidDeviceClass.phone),
  );
  expect(
    (saved.touch, saved.speed, saved.toolbar),
    (initial.touch, initial.speed, initial.toolbar),
  );
}

Future<void> _capture(WidgetTester t, GlobalKey boundary, String name) async {
  final directory = Platform.environment['LARGE_DESKTOP_CAPTURE_DIR'];
  if (directory == null) return;
  await t.runAsync(() async {
    final image =
        await (boundary.currentContext!.findRenderObject()
                as RenderRepaintBoundary)
            .toImage(pixelRatio: 1);
    try {
      final png = await image.toByteData(format: ui.ImageByteFormat.png);
      await Directory(directory).create(recursive: true);
      await File('$directory/$name.png')
          .writeAsBytes(png!.buffer.asUint8List());
    } finally {
      image.dispose();
    }
  });
}

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
    // ACK only: this fixture does not advertise native pen readiness.
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_optionalPenChannel, (_) async => null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          AndroidSessionKeepalive.channel,
          (_) async => null,
        );
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_optionalPenChannel, null);
    for (final channel in [
      AndroidSessionKeepalive.channel,
      AndroidStore.channel,
    ]) {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null);
    }
  });

  for (final spec in [
    (1080, const Size(1920, 1080)),
    (1440, const Size(2560, 1440)),
    (2160, const Size(3840, 2160)),
  ]) {
    test(
      'large desktop ${spec.$1} is fixed 16:9 and preserves native automatic DPI',
      () {
        final resolution = AndroidResolution.desktop(spec.$1);
        expect(resolution.encode(), 'd${spec.$1}');
        expect(
          AndroidResolution.parse(resolution.encode()),
          AndroidResolution.workspace(
            const {1080: 100, 1440: 150, 2160: 200}[spec.$1]!,
          ),
        );
        for (final television in [false, true]) {
          const physical = Size(1280, 720);
          AndroidScreenPolicy policy(AndroidResolution r) =>
              AndroidScreenPolicy.of(
                physical / 2,
                2,
                null,
                (_) => AndroidScreenChoice(resolution: r),
                television: television,
              );
          final base = policy(AndroidResolution.device).windowsScale(2, 1);
          final selected = policy(resolution);
          expect(selected.base(physical), spec.$2);
          expect(
            selected.base(const Size(720, 1280)),
            Size(spec.$2.height, spec.$2.width),
          );
          final plan = AndroidViewportPlan.of(
            physical,
            2,
            selected,
            hostScale: true,
          );
          expect(plan.size, spec.$2);
          expect(plan.displayScale, base);
          expect(
            AndroidViewportPlan.of(
              physical,
              2,
              selected,
              hostScale: false,
            ).size,
            spec.$2,
          );
          final fixed = AndroidScreenPolicy(
            choice: AndroidScreenChoice(resolution: resolution, scale: 175),
          );
          expect(
            AndroidViewportPlan.of(
              physical,
              2,
              fixed,
              hostScale: true,
            ).displayScale,
            175,
          );
        }
      },
    );
    testWidgets(
      'large desktop ${spec.$1} reaches actual stream start options above device pixels',
      (t) async {
        final h = await openLargeDesktopStream(
          t,
          resolution: AndroidResolution.desktop(spec.$1),
          television: true,
        );
        expect(
          (h.starts.single.maxWidth, h.starts.single.maxHeight),
          (spec.$2.width.toInt(), spec.$2.height.toInt()),
        );
        expect(h.session.sourceWidth, spec.$2.width.toInt());
        await closeLargeDesktopStreamView(t);
      },
    );
  }

  test('large desktop decoder ceiling reduces the planned stream without deleting the saved selection', () {
    final policy = AndroidScreenPolicy(
      choice: const AndroidScreenChoice(
        resolution: AndroidResolution.desktop(2160),
      ),
    );
    final plan = AndroidViewportPlan.of(
      const Size(1280, 720),
      1,
      policy,
      hostScale: true,
      portraitLimits: _limited,
    );
    expect(plan.size, const Size(1920, 1080));
    expect(policy.choice.resolution.encode(), 'd2160');
    final portrait = AndroidViewportPlan.of(
      const Size(720, 1280),
      1,
      policy,
      hostScale: true,
      portraitLimits: _full,
    );
    expect(portrait.size, const Size(2160, 3840));
    for (final value in ['auto', 'device', 'half', 'p720', 'p1440']) {
      expect(
        AndroidResolution.parse(value).encode(),
        value == 'device' ? 'w100' : value,
      );
    }
    for (final value in ['d0', 'd720', 'd4320', 'dNaN']) {
      expect(AndroidResolution.parse(value), AndroidResolution.auto);
    }
  });

  testWidgets(
    'selecting 2x workspace sends a real resize and reports the current source until its matching epoch arrives',
    (t) async {
      final h = await openLargeDesktopStream(t, television: true);
      await openLargeDesktopScreenPanel(t, television: true);
      await _choose(t, 200);
      var options = t.widget<AndroidScreenOptions>(
        find.byType(AndroidScreenOptions),
      );
      expect(options.resolutionNote, contains('1280×720'));
      expect(options.resolutionNote, contains('선택 2560×1440'));
      expect(options.resolutionNote, isNot(contains('현재 2560×1440')));
      expect(
        options.resolutions
            .singleWhere(
              (o) => o.resolution == const AndroidResolution.workspace(200),
            )
            .size,
        const Size(2560, 1440),
      );
      await t.tap(find.byKey(const Key('android-tv-tools-close')));
      await t.pumpAndSettle();
      await t.pump(const Duration(milliseconds: 700));
      await t.pump(const Duration(milliseconds: 700));
      final request = h.socket.sent.singleWhere(
        (m) => m['type'] == 'stream.resize',
      );
      expect((request['width'], request['height']), (2560, 1440));
      expect(
        h.starts,
        hasLength(1),
        reason: 'a capable stream resizes without reconnecting',
      );
      expect(
        h.saved.last.screen(AndroidDeviceClass.television).resolution,
        const AndroidResolution.workspace(200),
      );
      final owner = {
        'sessionId': 'large-desktop',
        'requestId': request['requestId'],
        'resizeId': 'large-desktop-resize',
        'epoch': 1,
      };
      h.socket.incoming.add(
        jsonEncode({
          'type': 'stream.resizing',
          ...owner,
          'width': 2560,
          'height': 1440,
          'fps': 60,
          'monitor': '0',
        }),
      );
      await t.pump(const Duration(milliseconds: 20));
      h.socket.incoming.add(
        jsonEncode({
          'type': 'stream.info',
          ...owner,
          'codec': 'h264',
          'width': 2560,
          'height': 1440,
          'sourceWidth': 2560,
          'sourceHeight': 1440,
          'fpsTarget': 60,
        }),
      );
      await t.pump(const Duration(milliseconds: 20));
      h.socket.incoming.add(_video(1));
      for (var i = 0; i < 6; i++) {
        await t.pump(const Duration(milliseconds: 60));
      }
      expect(h.session.closed, isFalse);
      expect(h.session.resizing, isFalse);
      await openLargeDesktopScreenPanel(t, television: true);
      options = t.widget<AndroidScreenOptions>(
        find.byType(AndroidScreenOptions),
      );
      expect(options.resolutionNote, contains('2560×1440'));
      expect(options.resolutionNote, contains('현재 2560×1440'));
      await closeLargeDesktopStreamView(t);
    },
  );

  testWidgets(
    'decoder-limited 2x workspace selection keeps the actual FHD source distinct in the screen menu',
    (t) async {
      final h = await openLargeDesktopStream(
        t,
        resolution: const AndroidResolution.workspace(200),
        television: true,
        limits: _limited,
      );
      expect(
        (h.starts.single.maxWidth, h.starts.single.maxHeight),
        (1920, 1080),
      );
      await openLargeDesktopScreenPanel(t, television: true);
      final options = t.widget<AndroidScreenOptions>(
        find.byType(AndroidScreenOptions),
      );
      expect(options.choice.resolution, const AndroidResolution.workspace(200));
      expect(options.resolutionNote, contains('1920×1080'));
      expect(options.resolutionNote, contains('선택 2560×1440'));
      expect(options.resolutionNote, contains('지원 범위 1920×1080'));
      expect(options.resolutionNote, isNot(contains('현재 2560×1440')));
      expect(
        options.resolutions
            .singleWhere((o) => o.resolution == options.choice.resolution)
            .size,
        const Size(2560, 1440),
      );
      await closeLargeDesktopStreamView(t);
    },
  );

  testWidgets(
    'failed enlarged workspace stream can explicitly recover to auto while preserving scale and quality',
    (t) async {
      final initial = _recoverySettings();
      const quality = StreamOptions(
        fps: 30,
        bitrate: 12000000,
        audio: false,
        bitrateMode: StreamBitrateMode.auto,
      );
      final h = await openLargeDesktopStream(
        t,
        settings: initial,
        quality: quality,
        television: true,
        firstFailure: const BrokerException('frame_layout_unsupported'),
        firstFrame: false,
      );
      expect(
        (h.starts.single.maxWidth, h.starts.single.maxHeight),
        (2560, 1440),
      );
      expect(h.scales.single, 100);
      final recover = find.byKey(const Key('android-resolution-auto-retry'));
      expect(recover, findsOneWidget);
      await t.tap(recover);
      for (var i = 0; i < 6; i++) {
        await t.pump(const Duration(milliseconds: 20));
      }
      expect(h.starts, hasLength(2));
      expect((h.starts.last.maxWidth, h.starts.last.maxHeight), (1280, 720));
      expect(h.scales.last, 100);
      expect(
        (
          h.starts.last.fps,
          h.starts.last.bitrate,
          h.starts.last.audio,
          h.starts.last.bitrateMode,
        ),
        (quality.fps, quality.bitrate, quality.audio, quality.bitrateMode),
      );
      _preservedSettings(h.saved.last, initial);
      await closeLargeDesktopStreamView(t);
    },
  );

  testWidgets(
    'auto resolution recovery is absent for auth nonretry and takeover failures',
    (t) async {
      for (final error in [
        const BrokerException('session_expired', 401),
        const BrokerException('desktop_not_found', 404),
        const BrokerException('desktop_busy', 409, null, true),
      ]) {
        final initial = _recoverySettings();
        final h = await openLargeDesktopStream(
          t,
          settings: initial,
          television: true,
          firstFailure: error,
          firstFrame: false,
          takeover: true,
        );
        expect(
          find.byKey(const Key('android-resolution-auto-retry')),
          findsNothing,
          reason: error.code,
        );
        expect(h.starts, hasLength(1));
        expect(h.saved, isEmpty);
        await closeLargeDesktopStreamView(t);
      }
    },
  );

  // Only the TV chooses a 해상도 (a phone follows its screen, 2026-11-03).
  for (final spec in [(const Size(960, 540), true, Brightness.dark)]) {
    testWidgets('workspace screen menu fits ${spec.$1} TV=${spec.$2} text150', (
      t,
    ) async {
      final shadows = debugDisableShadows;
      debugDisableShadows = false;
      addTearDown(() => debugDisableShadows = shadows);
      try {
        final boundary = GlobalKey();
        await openLargeDesktopStream(
          t,
          resolution: const AndroidResolution.workspace(200),
          physical: spec.$1,
          television: spec.$2,
          boundary: boundary,
          brightness: spec.$3,
          textScale: 1.5,
        );
        await openLargeDesktopScreenPanel(t, television: spec.$2);
        await t.ensureVisible(
          find.byKey(const Key('android-screen-resolution')),
        );
        await t.pumpAndSettle();
        expect(t.takeException(), isNull);
        final rect = t.getRect(
          find.byKey(const Key('android-screen-resolution-w200')),
        );
        expect(rect.left, greaterThanOrEqualTo(0));
        expect(rect.right, lessThanOrEqualTo(spec.$1.width));
        expect(rect.top, greaterThanOrEqualTo(0));
        expect(rect.bottom, lessThanOrEqualTo(spec.$1.height));
        await _capture(
          t,
          boundary,
          'large-desktop-${spec.$2 ? 'tv' : 'phone'}-${spec.$1.width.toInt()}-${spec.$3.name}-150',
        );
        await closeLargeDesktopStreamView(t);
      } finally {
        debugDisableShadows = shadows;
      }
    });
  }
}

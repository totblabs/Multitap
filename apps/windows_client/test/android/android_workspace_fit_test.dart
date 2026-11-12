// Actual Flutter view/start requests against fake media channels.
// These fixtures cannot establish physical-device decoder or Host acceptance.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/android/android_touch.dart'
    show AndroidPictureView;
import 'package:multitap_client/data/stream_size.dart';

import 'android_large_desktop_test.dart'
    show openLargeDesktopStream, closeLargeDesktopStreamView;

const _optionalPenChannel = MethodChannel('multitap/pen-input');

const _full = DecoderLimits(
  maxWidth: 3840,
  maxHeight: 3840,
  maxPixels: 8294400,
  maxPixelRate: 0,
);

AndroidScreenPolicy _policy(
  Size physical,
  double dpr,
  AndroidResolution resolution, {
  bool television = false,
  int scale = 0,
}) => AndroidScreenPolicy.of(
  physical / dpr,
  dpr,
  null,
  (_) => AndroidScreenChoice(resolution: resolution, scale: scale),
  television: television,
);

void _sameAspect(Size source, Size physical) {
  // Even-side alignment can remove less than two pixels on either axis.
  final widthError = (source.width - source.height * physical.aspectRatio)
      .abs();
  expect(widthError, lessThanOrEqualTo(2 * physical.aspectRatio + 2));
}

void _fills(Rect picture, Size viewport) {
  expect(picture.left.abs(), lessThanOrEqualTo(1));
  expect(picture.top.abs(), lessThanOrEqualTo(1));
  expect((picture.right - viewport.width).abs(), lessThanOrEqualTo(1));
  expect((picture.bottom - viewport.height).abs(), lessThanOrEqualTo(1));
}

Future<void> _capture(WidgetTester t, GlobalKey key, String name) async {
  final path = Platform.environment['WORKSPACE_CAPTURE_DIR'];
  if (path == null) return;
  await t.runAsync(() async {
    final image =
        await (key.currentContext!.findRenderObject() as RenderRepaintBoundary)
            .toImage(pixelRatio: 1);
    try {
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      await Directory(path).create(recursive: true);
      await File('$path/$name.png').writeAsBytes(bytes!.buffer.asUint8List());
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

  test('workspace persistence migrates released fixed desktops and device while preserving lower presets', () {
    const migrations = {
      'device': 100,
      'd1080': 100,
      'd1440': 150,
      'd2160': 200,
    };
    for (final item in migrations.entries) {
      expect(
        AndroidResolution.parse(item.key),
        AndroidResolution.workspace(item.value),
      );
    }
    for (final percent in [100, 125, 150, 200]) {
      final selected = AndroidResolution.workspace(percent);
      expect(selected.encode(), 'w$percent');
      expect(AndroidResolution.parse(selected.encode()), selected);
      expect(selected.workspacePercent, percent);
    }
    for (final value in ['w0', 'w50', 'w175', 'wNaN', 'w2000', 'd4320']) {
      expect(AndroidResolution.parse(value), AndroidResolution.auto);
    }
    for (final value in ['auto', 'half', 'p720', 'p1080', 'p1440']) {
      expect(AndroidResolution.parse(value).encode(), value);
    }
    final options = androidResolutionOptions(const Size(2400, 1080));
    expect(options.map((o) => o.resolution.encode()), [
      'auto',
      'l1280',
      'l1600',
      'l1920',
      'l2560',
    ]);
    expect(
      options.any(
        (o) =>
            o.resolution.kind == AndroidResolutionKind.desktop ||
            o.resolution.kind == AndroidResolutionKind.device ||
            o.resolution.kind == AndroidResolutionKind.workspace ||
            o.resolution.kind == AndroidResolutionKind.half,
      ),
      isFalse,
    );
    final legacy = _policy(const Size(2400, 1080), 3, AndroidResolution.device);
    expect(
      AndroidViewportPlan.of(
        const Size(2400, 1080),
        3,
        legacy,
        hostScale: true,
      ).size,
      const Size(2400, 1080),
    );
    final half = _policy(const Size(2400, 1080), 3, AndroidResolution.half);
    expect(
      AndroidViewportPlan.of(
        const Size(2400, 1080),
        3,
        half,
        hostScale: true,
      ).size,
      const Size(1200, 540),
    );
    expect(
      _policy(
        const Size(2400, 1080),
        3,
        const AndroidResolution.preset(720),
      ).base(const Size(2400, 1080)),
      const Size(1600, 720),
    );
  });

  test(
    'legacy streaming fits remaining codec headroom of the requested workspace',
    () {
      const physical = Size(2560, 1600);
      final policy = _policy(
        physical,
        2,
        const AndroidResolution.workspace(200),
      );
      final plan = AndroidViewportPlan.of(physical, 2, policy, hostScale: true);
      expect(policy.base(physical), const Size(5120, 3200));
      // The largest valid even goal can use another two pixels after rounding.
      expect(plan.width, closeTo(3456, 2));
      expect(plan.height, 2160);
      expect(plan.width, greaterThan(physical.width));
      expect(plan.height, greaterThan(physical.height));
      _sameAspect(plan.size, physical);
      final nativeDpi = _policy(
        physical,
        2,
        AndroidResolution.device,
      ).windowsScale(2, 1);
      expect(plan.displayScale, nativeDpi);
      final manual = _policy(
        physical,
        2,
        const AndroidResolution.workspace(200),
        scale: 175,
      );
      expect(
        AndroidViewportPlan.of(
          physical,
          2,
          manual,
          hostScale: true,
        ).displayScale,
        175,
      );
      _fills(
        AndroidPictureView.fit(
          const Rect.fromLTWH(0, 0, 1280, 800),
          plan.size,
        ).rect,
        physical / 2,
      );
    },
  );

  test('workspace decoder pixel-rate ceiling fits continuously at the requested FPS', () {
    const physical = Size(1280, 720);
    const limits = DecoderLimits(
      maxWidth: 3840,
      maxHeight: 2160,
      maxPixels: 8294400,
      maxPixelRate: 2097152 * 30,
    );
    final policy = _policy(physical, 1, const AndroidResolution.workspace(200));
    final slow = AndroidViewportPlan.of(
      physical,
      1,
      policy,
      hostScale: true,
      portraitLimits: limits,
      fps: 30,
    );
    final fast = AndroidViewportPlan.of(
      physical,
      1,
      policy,
      hostScale: true,
      portraitLimits: limits,
      fps: 60,
    );
    expect(limits.fits(slow.width, slow.height, 30), isTrue);
    expect(limits.fits(fast.width, fast.height, 60), isTrue);
    expect(slow.width, greaterThan(fast.width));
    expect(slow.height, greaterThan(fast.height));
    expect(
      slow.width,
      greaterThan(1280),
      reason: '30 fps has usable headroom above the original device',
    );
    _sameAspect(slow.size, physical);
    _sameAspect(fast.size, physical);
    expect(
      (
        slow.width.isEven,
        slow.height.isEven,
        fast.width.isEven,
        fast.height.isEven,
      ),
      (true, true, true, true),
    );
  });

  testWidgets(
    'released fixed 16:9 source reproduces 20:9 side bars while device workspace removes them',
    (t) async {
      final shadows = debugDisableShadows;
      debugDisableShadows = false;
      addTearDown(() => debugDisableShadows = shadows);
      try {
        final before = GlobalKey();
        final legacy = await openLargeDesktopStream(
          t,
          resolution: const AndroidResolution.desktop(1080),
          physical: const Size(2400, 1080),
          dpr: 3,
          boundary: before,
        );
        expect(
          (legacy.starts.single.maxWidth, legacy.starts.single.maxHeight),
          (1920, 1080),
        );
        expect(
          t.getRect(find.byKey(const Key('large-desktop-picture'))),
          const Rect.fromLTWH(80, 0, 640, 360),
        );
        await _capture(t, before, 'workspace-before-20x9-800');
        await closeLargeDesktopStreamView(t);
        final after = GlobalKey();
        final current = await openLargeDesktopStream(
          t,
          resolution: const AndroidResolution.workspace(100),
          physical: const Size(2400, 1080),
          dpr: 3,
          boundary: after,
        );
        expect(
          (current.starts.single.maxWidth, current.starts.single.maxHeight),
          (2400, 1080),
        );
        _fills(
          t.getRect(find.byKey(const Key('large-desktop-picture'))),
          const Size(800, 360),
        );
        await _capture(t, after, 'workspace-after-20x9-800');
        await closeLargeDesktopStreamView(t);
      } finally {
        debugDisableShadows = shadows;
      }
    },
  );

  for (final spec in [
    ('20x9', const Size(2400, 1080), 3.0, false),
    ('16x10', const Size(2560, 1600), 2.0, false),
    ('4x3', const Size(2048, 1536), 2.0, false),
    ('TV16x9', const Size(1920, 1080), 2.0, true),
    ('portrait', const Size(1080, 2400), 3.0, false),
  ]) {
    testWidgets(
      'workspace ${spec.$1} requests its actual aspect and fits the stream view',
      (t) async {
        final h = await openLargeDesktopStream(
          t,
          resolution: const AndroidResolution.workspace(100),
          physical: spec.$2,
          dpr: spec.$3,
          television: spec.$4,
          limits: _full,
        );
        expect(
          (h.starts.single.maxWidth, h.starts.single.maxHeight),
          (spec.$2.width.toInt(), spec.$2.height.toInt()),
        );
        expect(
          (h.session.sourceWidth, h.session.sourceHeight),
          (spec.$2.width.toInt(), spec.$2.height.toInt()),
        );
        _fills(
          t.getRect(find.byKey(const Key('large-desktop-picture'))),
          spec.$2 / spec.$3,
        );
        await closeLargeDesktopStreamView(t);
      },
    );
    test(
      'workspace ${spec.$1} keeps its ratio through enlargement and negotiated pixel caps',
      () {
        final nativeDpi = _policy(
          spec.$2,
          spec.$3,
          AndroidResolution.device,
          television: spec.$4,
        ).windowsScale(spec.$3, 1);
        for (final percent in [100, 125, 150, 200]) {
          final policy = _policy(
            spec.$2,
            spec.$3,
            AndroidResolution.workspace(percent),
            television: spec.$4,
          );
          final plan = AndroidViewportPlan.of(
            spec.$2,
            spec.$3,
            policy,
            hostScale: true,
            portraitLimits: _full,
          );
          expect(_full.fits(plan.width, plan.height, 60), isTrue);
          expect(plan.width, lessThanOrEqualTo(3840));
          expect(plan.height, lessThanOrEqualTo(3840));
          expect(plan.width * plan.height, lessThanOrEqualTo(8294400));
          _sameAspect(plan.size, spec.$2);
          expect(plan.displayScale, nativeDpi);
          _fills(
            AndroidPictureView.fit(
              Offset.zero & (spec.$2 / spec.$3),
              plan.size,
            ).rect,
            spec.$2 / spec.$3,
          );
        }
      },
    );
  }
}

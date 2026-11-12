// Bounded handheld source/independent DPI policy and actual option callbacks.
// Mocked preferences/relay/media only, never an Android or Windows operation.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/android/android_toolbar.dart';
import 'package:multitap_client/data/display_scale.dart';
import 'package:multitap_client/data/stream_size.dart';

import 'android_large_desktop_test.dart'
    show
        openLargeDesktopStream,
        openLargeDesktopScreenPanel,
        closeLargeDesktopStreamView;

const _limits = DecoderLimits(
  maxWidth: 3840,
  maxHeight: 3840,
  maxPixels: 8294400,
  maxPixelRate: 0,
);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? 'started' : true,
    );
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      (_) async => null,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      null,
    );
  });
  for (final kind in [
    AndroidDeviceClass.phone,
    AndroidDeviceClass.fold,
    AndroidDeviceClass.tablet,
  ]) {
    for (final physical in [
      const Size(1080, 2400),
      const Size(2400, 1080),
      const Size(2448, 1848),
      const Size(1280, 720),
    ]) {
      test(
        '$kind $physical bounded source fits orientation and never upsamples',
        () {
          final choices = androidResolutionOptions(physical, deviceClass: kind);
          final explicit = choices
              .where(
                (option) =>
                    option.resolution.kind == AndroidResolutionKind.longEdge,
              )
              .toList();
          expect(explicit.length, inInclusiveRange(1, 4));
          expect(
            explicit.map((option) => option.size).toSet(),
            hasLength(explicit.length),
          );
          for (final option in choices) {
            final size = option.size!;
            expect(size.longestSide, lessThanOrEqualTo(2560));
            expect(size.width, lessThanOrEqualTo(physical.width));
            expect(size.height, lessThanOrEqualTo(physical.height));
            expect(
              size.width.toInt().isEven && size.height.toInt().isEven,
              true,
            );
            expect(
              (size.width * physical.height - size.height * physical.width)
                  .abs(),
              lessThanOrEqualTo(2 * (physical.width + physical.height)),
            );
          }
          final plan = AndroidViewportPlan.of(
            physical,
            2.625,
            AndroidScreenPolicy(deviceClass: kind),
            hostScale: true,
            fullWorkspace: true,
          );
          expect(plan.size.longestSide, lessThanOrEqualTo(2560));
          expect(plan.displayScale, inInclusiveRange(100, 200));
        },
      );
    }
  }

  test('four Fold choices retain the native source and lower aspect-preserving sizes', () {
    final options = androidResolutionOptions(const Size(2448, 1848));
    expect(options.map((option) => option.resolution.encode()), [
      'auto',
      'l1280',
      'l1600',
      'l1920',
      'l2560',
    ]);
    expect(options.skip(1).map((option) => option.size), [
      const Size(1280, 966),
      const Size(1600, 1206),
      const Size(1920, 1448),
      const Size(2448, 1848),
    ]);
    expect(options.skip(1).map((option) => option.label), [
      '1280×966',
      '1600×1206',
      '1920×1448',
      '기기에 맞춤',
    ]);
  });

  test(
    'manual scale wins; recommendation bounded; old Host keeps exact fraction',
    () {
      for (final scale in androidScreenScales) {
        final plan = AndroidViewportPlan.of(
          const Size(2400, 1080),
          3,
          AndroidScreenPolicy(
            choice: AndroidScreenChoice(
              scale: scale,
              resolution: const AndroidResolution.longEdge(1600),
            ),
          ),
          hostScale: true,
          fullWorkspace: true,
        );
        expect(plan.size, const Size(1600, 720));
        expect(plan.displayScale, scale);
      }
      for (final dpr in [1.0, 2.625, 3.0, 5.0]) {
        expect(
          const AndroidScreenPolicy().windowsScale(dpr, 1),
          inInclusiveRange(100, 200),
        );
      }
      final legacy = AndroidViewportPlan.of(
        const Size(2400, 1080),
        3,
        const AndroidScreenPolicy(),
        hostScale: false,
        fullWorkspace: true,
      );
      expect(legacy.size, const Size(1200, 540));
      expect(legacy.pixelScale, 2);
      expect(legacy.displayScale, isNull);
    },
  );

  test('decoder bound fits proportionally and v2 source remains separate', () {
    const limited = DecoderLimits(
      maxWidth: 1280,
      maxHeight: 720,
      maxPixels: 921600,
      maxPixelRate: 0,
    );
    const mode = AndroidScreenPolicy(
      choice: AndroidScreenChoice(resolution: AndroidResolution.longEdge(1920)),
    );
    final bounded = AndroidViewportPlan.of(
      const Size(2448, 1848),
      2.625,
      mode,
      hostScale: true,
      portraitLimits: limited,
    );
    expect(limited.fits(bounded.width, bounded.height, 60), true);
    expect(bounded.width, greaterThan(900));
    expect(bounded.height, 720);
    final separate = AndroidViewportPlan.of(
      const Size(2448, 1848),
      2.625,
      mode,
      hostScale: true,
      portraitLimits: limited,
      fullWorkspace: true,
    );
    expect(separate.size, const Size(1920, 1448));
    expect(
      separate.pixelScale,
      0,
      reason:
          'fractional fitting is not advertised as native1to1 or a clarity fix',
    );
  });

  test('saved new choices are per-class; old oversized values normalize only in memory', () async {
    final raw = <String, String>{
      'session.screen.phone': '{"s":300,"r":"w200","o":"portrait"}',
      'session.screen.fold': '{"s":150,"r":"l1600","o":"auto"}',
      'session.screen.tablet': '{"s":200,"r":"l1280","o":"auto"}',
      'session.screen.television': '{"s":300,"r":"w200","o":"landscape"}',
    };
    final before = Map<String, String>.of(raw), calls = <MethodCall>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, (call) async {
          calls.add(call);
          return call.method == 'preferences'
              ? Map<String, String>.of(raw)
              : null;
        });
    addTearDown(
      () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(AndroidStore.channel, null),
    );
    final settings = await AndroidStore().sessionSettings();
    expect(
      settings.screen(AndroidDeviceClass.phone),
      const AndroidScreenChoice(scale: 200),
    );
    expect(
      settings.screen(AndroidDeviceClass.fold),
      const AndroidScreenChoice(
        scale: 150,
        resolution: AndroidResolution.longEdge(1600),
      ),
    );
    expect(
      settings.screen(AndroidDeviceClass.tablet).resolution,
      const AndroidResolution.longEdge(1280),
    );
    expect(settings.screen(AndroidDeviceClass.television).scale, 300);
    expect(
      settings.screen(AndroidDeviceClass.television).resolution,
      const AndroidResolution.workspace(200),
    );
    expect(raw, before);
    expect(calls.map((call) => call.method), ['preferences']);
    for (final scale in [225, 250, 300]) {
      expect(
        AndroidScreenChoice(scale: scale)
            .forDevice(AndroidDeviceClass.phone)
            .scale,
        200,
      );
    }
    for (final edge in AndroidResolution.longEdges) {
      final choice = AndroidScreenChoice(
        scale: 125,
        resolution: AndroidResolution.longEdge(edge),
      );
      expect(
        AndroidScreenChoice.parse(choice.encode())
            .forDevice(AndroidDeviceClass.fold),
        choice,
      );
    }
    expect(AndroidResolution.parse('l999'), AndroidResolution.auto);
    expect(
      AndroidResolution.parse('l999999999999999999999'),
      AndroidResolution.auto,
    );
  });

  test('saved admission resets old orientation but active per-session choices retain it', () {
    const active = AndroidScreenChoice(
      scale: 150,
      resolution: AndroidResolution.longEdge(1600),
      orientation: AndroidOrientation.portrait,
    );
    final policy = AndroidScreenPolicy.of(
      const Size(932, 704),
      2.625,
      null,
      (_) => active,
    );
    expect(policy.choice.orientation, AndroidOrientation.portrait);
    expect(
      policy.choice
          .copyWith(
            scale: 200,
            resolution: const AndroidResolution.longEdge(1280),
          )
          .orientation,
      AndroidOrientation.portrait,
    );
    expect(
      active.forDevice(AndroidDeviceClass.fold).orientation,
      AndroidOrientation.auto,
    );
    expect(
      active.forDevice(AndroidDeviceClass.television).orientation,
      AndroidOrientation.portrait,
    );
  });

  testWidgets(
    'saved2560 aliases the effective smaller size across window rotation without rewriting it',
    (tester) async {
      const choice = AndroidScreenChoice(
        scale: 175,
        resolution: AndroidResolution.longEdge(2560),
      );
      final encoded = choice.encode(), changed = <AndroidScreenChoice>[];
      for (final physical in [
        const Size(1920, 1080),
        const Size(1280, 720),
        const Size(720, 1280),
        const Size(2448, 1848),
      ]) {
        final options = androidResolutionOptions(physical);
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: SingleChildScrollView(
                child: AndroidScreenOptions(
                  deviceClass: AndroidDeviceClass.fold,
                  choice: choice,
                  autoScale: 150,
                  resolutions: options,
                  parts: const {AndroidScreenPart.resolution},
                  onChoice: changed.add,
                ),
              ),
            ),
          ),
        );
        final row = tester.widget<AndroidPanelChoices>(
          find.byKey(const Key('android-screen-resolution')),
        );
        final selected = row.items.where((item) => item.selected).toList();
        expect(selected, hasLength(1));
        final effective = options.singleWhere(
          (option) =>
              Key('android-screen-resolution-${option.resolution.encode()}') ==
              selected.single.key,
        );
        expect(effective.size, physical);
        expect(effective.deviceSize, true);
        expect(changed, isEmpty);
        expect(choice.encode(), encoded);
        expect(
          AndroidScreenChoice.parse(encoded).forDevice(AndroidDeviceClass.fold),
          choice,
        );
      }
    },
  );

  for (final television in [false, true]) {
    testWidgets(
      'options keep separate sections and ${television ? 'legacyTV' : 'five handheld'} scale choices',
      (tester) async {
        final choices = <AndroidScreenChoice>[];
        final kind = television
            ? AndroidDeviceClass.television
            : AndroidDeviceClass.fold;
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: SingleChildScrollView(
                child: AndroidScreenOptions(
                  deviceClass: kind,
                  choice: const AndroidScreenChoice(
                    orientation: AndroidOrientation.portrait,
                  ),
                  autoScale: 150,
                  resolutions: androidResolutionOptions(
                    const Size(2448, 1848),
                    deviceClass: kind,
                  ),
                  scaleNote: '현재125% ·150% 요청',
                  resolutionNote: '현재1280×966',
                  parts: const {
                    AndroidScreenPart.scale,
                    AndroidScreenPart.resolution,
                  },
                  onChoice: choices.add,
                ),
              ),
            ),
          ),
        );
        expect(
          choices,
          isEmpty,
          reason: 'opening options performs no apply/save',
        );
        final scales = tester.widget<AndroidPanelChoices>(
          find.byKey(const Key('android-screen-scale')),
        );
        expect(scales.items, hasLength(television ? 9 : 5));
        expect(
          find.byKey(const Key('android-screen-scale-0')),
          television ? findsOneWidget : findsNothing,
        );
        expect(
          scales.items.where((item) => item.selected).single.key,
          Key(
            television ? 'android-screen-scale-0' : 'android-screen-scale-150',
          ),
        );
        expect(
          tester
              .getTopLeft(find.byKey(const Key('android-screen-resolution')))
              .dy,
          lessThan(
            tester.getTopLeft(find.byKey(const Key('android-screen-scale'))).dy,
          ),
        );
        expect(scales.note, '현재125% ·150% 요청');
        scales.items
            .firstWhere(
              (item) => item.key == const Key('android-screen-scale-100'),
            )
            .onPressed!();
        expect(choices.single.orientation, AndroidOrientation.portrait);
        if (!television) {
          expect(
            find.byKey(const Key('android-screen-scale-225')),
            findsNothing,
          );
          expect(
            find.byKey(const Key('android-screen-resolution-w200')),
            findsNothing,
          );
        }
      },
    );
  }

  testWidgets(
    'actual remote starts at chosen source and only explicit selection resizes',
    (tester) async {
      final h = await openLargeDesktopStream(
        tester,
        resolution: const AndroidResolution.longEdge(1600),
        physical: const Size(2448, 1848),
        dpr: 2.625,
        limits: _limits,
        streamSizeV2: false,
      );
      expect(
        (h.starts.single.maxWidth, h.starts.single.maxHeight),
        (1600, 1206),
      );
      final before = h.socket.sent
          .where(
            (event) =>
                event['type'] == 'display.scale' ||
                event['type'] == 'stream.resize',
          )
          .length;
      await openLargeDesktopScreenPanel(tester);
      expect(
        h.socket.sent.where(
          (event) =>
              event['type'] == 'display.scale' ||
              event['type'] == 'stream.resize',
        ),
        hasLength(before),
      );
      final choice = find.byKey(const Key('android-screen-resolution-l1280'));
      await tester.ensureVisible(choice);
      await tester.pumpAndSettle();
      await tester.tap(choice);
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('session-toolbar-pointer')), findsNothing);
      await tester.pump(const Duration(milliseconds: 700));
      await tester.pump(const Duration(milliseconds: 700));
      expect(
        h.saved.last.screen(AndroidDeviceClass.fold).resolution,
        const AndroidResolution.longEdge(1280),
      );
      final resize = h.socket.sent.lastWhere(
        (event) => event['type'] == 'stream.resize',
      );
      expect((resize['width'], resize['height']), (1280, 966));
      expect(
        androidWindowsTextScaleStatus(
          automatic: false,
          supported: true,
          wanted: 200,
          applied: const DisplayScaleApplied(
            ok: true,
            scale: 150,
            maximum: 150,
            code: 0,
          ),
        ),
        contains('현재 150%'),
      );
      await closeLargeDesktopStreamView(tester);
    },
  );
}

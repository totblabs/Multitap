import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/ui/app_theme.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    final text = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      text.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await text.load();
    await (FontLoader(
      'MaterialIcons',
    )..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'))).load();
  });
  tearDown(
    () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null),
  );

  AndroidScreenPolicy television(
    Size pixels,
    double dpr, [
    AndroidScreenChoice choice = const AndroidScreenChoice(),
  ]) => AndroidScreenPolicy.of(
    pixels / dpr,
    dpr,
    null,
    (_) => choice,
    television: true,
  );

  test(
    'TV capability selects a separate class at every logical screen size',
    () {
      for (final size in [const Size(960, 540), const Size(1920, 1080)]) {
        final seen = <AndroidDeviceClass>[];
        final policy = AndroidScreenPolicy.of(size, 2, null, (kind) {
          seen.add(kind);
          return const AndroidScreenChoice(scale: 175);
        }, television: true);
        expect(policy.deviceClass, AndroidDeviceClass.television);
        expect(seen, [AndroidDeviceClass.television]);
        expect(policy.choice.scale, 175);
        expect(policy.deviceClass.label, 'TV');
      }
      expect(
        AndroidDeviceClass.parse('television'),
        AndroidDeviceClass.television,
      );
      // The original four-argument API and mobile classification stay intact.
      expect(
        AndroidScreenPolicy.of(
          const Size(960, 540),
          2,
          null,
          (_) => const AndroidScreenChoice(),
        ).deviceClass,
        AndroidDeviceClass.phone,
      );
      expect(
        AndroidScreenPolicy.of(
          const Size(933, 704),
          2,
          null,
          (_) => const AndroidScreenChoice(),
        ).deviceClass,
        AndroidDeviceClass.fold,
      );
      expect(
        AndroidScreenPolicy.of(
          const Size(1920, 1080),
          1,
          null,
          (_) => const AndroidScreenChoice(),
        ).deviceClass,
        AndroidDeviceClass.tablet,
      );
    },
  );

  test(
    'TV auto scale follows the remote viewport, independent of density bucket',
    () {
      for (final dpr in [1.0, 1.5, 2.0, 3.0]) {
        expect(
          television(const Size(1920, 1080), dpr).windowsScale(dpr, 1),
          150,
        );
        expect(
          television(const Size(3840, 2160), dpr).windowsScale(dpr, 1),
          225,
        );
      }
      expect(
        television(const Size(1920, 1040), 2).windowsScale(2, 1),
        150,
        reason: 'small system insets must not drop the 1080p default',
      );
      expect(television(const Size(1280, 720), 1).windowsScale(1, 1), 125);
      expect(
        television(const Size(3840, 2160), 2).windowsScale(2, 2),
        150,
        reason: 'half of 4K is a 1080p remote output',
      );
      const lowPpi = AndroidScreenDensity(
        xdpi: 40,
        ydpi: 40,
        width: 1920,
        height: 1080,
      );
      final tv = AndroidScreenPolicy.of(
        const Size(960, 540),
        2,
        lowPpi,
        (_) => const AndroidScreenChoice(),
        television: true,
      );
      expect(
        tv.windowsScale(2, 1),
        150,
        reason:
            'TV panel PPI does not use a handheld viewing-distance estimate',
      );
    },
  );

  test(
    'TV viewport negotiation preserves full, half, legacy and manual choices',
    () {
      final full = television(const Size(3840, 2160), 2);
      expect(
        AndroidViewportPlan.of(
          const Size(3840, 2160),
          2,
          full,
          hostScale: true,
        ),
        const AndroidViewportPlan(
          width: 3840,
          height: 2160,
          pixelScale: 1,
          displayScale: 225,
        ),
      );
      final half = television(
        const Size(3840, 2160),
        2,
        const AndroidScreenChoice(resolution: AndroidResolution.half),
      );
      expect(
        AndroidViewportPlan.of(
          const Size(3840, 2160),
          2,
          half,
          hostScale: true,
        ),
        const AndroidViewportPlan(
          width: 1920,
          height: 1080,
          pixelScale: 2,
          displayScale: 150,
        ),
      );
      expect(
        AndroidViewportPlan.of(
          const Size(3840, 2160),
          2,
          full,
          hostScale: false,
        ),
        const AndroidViewportPlan(width: 1920, height: 1080, pixelScale: 2),
      );
      for (final scale in [125, 150, 175, 200, 225, 250, 300]) {
        final choice = AndroidScreenChoice(
          scale: scale,
          orientation: AndroidOrientation.landscape,
        );
        expect(AndroidScreenChoice.parse(choice.encode()), choice);
        expect(
          television(const Size(1920, 1080), 2, choice).windowsScale(2, 1),
          scale,
        );
      }
    },
  );

  test(
    'TV preferences round-trip separately and do not inherit tablet values',
    () async {
      final prefs = <String, String>{
        'session.screen.tablet': const AndroidScreenChoice(
          scale: 300,
          orientation: AndroidOrientation.portrait,
        ).encode(),
        'session.screen.phone': const AndroidScreenChoice(scale: 125).encode(),
      };
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(AndroidStore.channel, (call) async {
            if (call.method == 'preferences') return {...prefs};
            if (call.method == 'savePreference') {
              final args = call.arguments as Map;
              prefs[args['key'] as String] = args['value'] as String;
            }
            return null;
          });
      final store = AndroidStore(),
          initial = await AndroidStore().sessionSettings();
      expect(
        initial.screen(AndroidDeviceClass.television),
        const AndroidScreenChoice(orientation: AndroidOrientation.landscape),
      );
      expect(
        initial.screen(AndroidDeviceClass.fold),
        const AndroidScreenChoice(),
      );
      final saved = initial.withScreen(
        AndroidDeviceClass.television,
        const AndroidScreenChoice(
          scale: 225,
          resolution: AndroidResolution.half,
          orientation: AndroidOrientation.auto,
        ),
      );
      await store.saveSessionSettings(saved);
      final restored = await store.sessionSettings();
      expect(
        restored.screen(AndroidDeviceClass.television),
        saved.screen(AndroidDeviceClass.television),
      );
      expect(
        restored.screen(AndroidDeviceClass.television).orientation,
        AndroidOrientation.auto,
        reason: 'explicit saved orientation is preserved',
      );
      expect(
        restored.screen(AndroidDeviceClass.tablet),
        initial.screen(AndroidDeviceClass.tablet),
      );
      expect(
        restored.screen(AndroidDeviceClass.phone),
        initial.screen(AndroidDeviceClass.phone),
      );
      expect(prefs.keys, contains('session.screen.television'));
    },
  );

  for (final brightness in Brightness.values) {
    testWidgets(
      'TV screen panel ${brightness.name}: large text and manual scale',
      (tester) async {
        tester.view.physicalSize = const Size(960, 540);
        tester.view.devicePixelRatio = 1;
        tester.platformDispatcher.textScaleFactorTestValue = 2;
        addTearDown(tester.view.reset);
        addTearDown(tester.platformDispatcher.clearAllTestValues);
        final previousShadows = debugDisableShadows;
        debugDisableShadows = false;
        final boundary = GlobalKey();
        AndroidScreenChoice? chosen;
        try {
          await tester.pumpWidget(
            MaterialApp(
              theme: buildAppTheme(brightness: brightness),
              home: Scaffold(
                body: Center(
                  child: SizedBox(
                    width: 560,
                    child: RepaintBoundary(
                      key: boundary,
                      child: ColoredBox(
                        color: const Color(0xFF1A2025),
                        child: SingleChildScrollView(
                          padding: const EdgeInsets.all(16),
                          child: AndroidScreenOptions(
                            deviceClass: AndroidDeviceClass.television,
                            choice: const AndroidScreenChoice(),
                            autoScale: 225,
                            resolutions: const [],
                            resolutionAvailable: false,
                            onChoice: (value) => chosen = value,
                          ),
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          expect(find.text('Windows 배율 · TV'), findsOneWidget);
          expect(find.text('태블릿'), findsNothing);
          for (final label in [
            '자동',
            '100',
            '125',
            '150',
            '175',
            '200',
            '225',
            '250',
            '300',
            '225%',
          ]) {
            expect(
              tester
                  .renderObject<RenderParagraph>(
                    // The label's paragraph (a Korean label keeps its words
                    // whole through KeepAllText, which wraps it).
                    find.descendant(
                      of: find.text(label),
                      matching: find.byType(RichText),
                    ),
                  )
                  .didExceedMaxLines,
              isFalse,
              reason: '$label must remain readable at 200%',
            );
          }
          final option = find.byKey(const Key('android-screen-scale-225'));
          await tester.ensureVisible(option);
          await tester.tap(option);
          expect(chosen?.scale, 225);
          expect(tester.takeException(), isNull);
        } finally {
          debugDisableShadows = previousShadows;
        }
      },
    );
  }
}

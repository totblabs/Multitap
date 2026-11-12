import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/android/android_tv_remote.dart';
import 'package:multitap_client/ui/app_theme.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    final loader = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      loader.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await loader.load();
  });
  testWidgets(
    'TV32 locked-drag status fits a narrow viewport with large text',
    (t) async {
      await t.binding.setSurfaceSize(const Size(320, 480));
      addTearDown(() => t.binding.setSurfaceSize(null));
      await t.pumpWidget(
        MaterialApp(
          debugShowCheckedModeBanner: false,
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context)
                .copyWith(textScaler: TextScaler.linear(2)),
            child: child!,
          ),
          home: Scaffold(
            body: AndroidTvRemote(
              enabled: true,
              session: 'local-fixture',
              mode: AndroidTvPointerMode.drag,
              position: () => const Offset(.5, .5),
              source: () => const Size(1920, 1080),
              canControl: () => true,
              onMove: (_) {},
              onClick: (_, _) {},
              onMenu: () {},
              onDragStart: (_) => (_, {required cancelled}) {},
              child: const SizedBox.expand(),
            ),
          ),
        ),
      );
      await t.pump();
      await t.sendKeyEvent(LogicalKeyboardKey.select);
      await t.pump();
      await t.pump();
      final marker = find.byKey(const Key('android-tv-pointer-mode'));
      expect(find.text('끄는 중 · OK로 놓기'), findsOneWidget);
      final rect = t.getRect(marker);
      expect(rect.left, greaterThanOrEqualTo(12));
      expect(rect.right, lessThanOrEqualTo(308));
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );
  for (final spec in [
    (const Size(960, 540), 1.5, Brightness.dark),
    (const Size(960, 540), 1.5, Brightness.light),
    (const Size(390, 700), 1.5, Brightness.dark),
    (const Size(320, 480), 2.0, Brightness.light),
  ]) {
    testWidgets(
      'TV32 menu remains local, scrollable and closable at ${spec.$1} text${spec.$2} ${spec.$3.name}',
      (t) async {
        await t.binding.setSurfaceSize(spec.$1);
        final shadows = debugDisableShadows;
        debugDisableShadows = false;
        addTearDown(() {
          debugDisableShadows = shadows;
          t.binding.setSurfaceSize(null);
        });
        try {
          final boundary = GlobalKey(), navigator = GlobalKey<NavigatorState>();
          await t.pumpWidget(
            RepaintBoundary(
              key: boundary,
              child: MaterialApp(
                debugShowCheckedModeBanner: false,
                navigatorKey: navigator,
                theme: buildAppTheme(brightness: spec.$3),
                builder: (context, child) => MediaQuery(
                  data: MediaQuery.of(context)
                      .copyWith(textScaler: TextScaler.linear(spec.$2)),
                  child: AndroidTvNavigation(television: true, child: child!),
                ),
                home: const Scaffold(body: SizedBox.expand()),
              ),
            ),
          );
          final future = showAndroidTvRemoteMenu(
            navigator.currentContext!,
            keyboardOpen: false,
            mode: AndroidTvPointerMode.drag,
          );
          await t.pumpAndSettle();
          expect(t.takeException(), isNull);
          expect(
            find.byKey(const Key('android-tv-menu-screen')),
            findsOneWidget,
          );
          expect(
            find.byKey(const Key('android-tv-menu-shortcuts')),
            findsOneWidget,
          );
          expect(find.byKey(const Key('android-tv-mode-drag')), findsOneWidget);
          final close = find.widgetWithText(TextButton, '원격 화면으로');
          final rect = t.getRect(close);
          expect(rect.left, greaterThanOrEqualTo(0));
          expect(rect.right, lessThanOrEqualTo(spec.$1.width));
          expect(rect.bottom, lessThanOrEqualTo(spec.$1.height));
          if (spec.$1.width == 320) {
            final scrolls = t.stateList<ScrollableState>(
              find.byType(Scrollable),
            );
            expect(scrolls.any((s) => s.position.maxScrollExtent > 0), true);
          }
          final directory = Platform.environment['TV_UI_CAPTURE_DIR'];
          if (directory != null) {
            await t.runAsync(() async {
              final image =
                  await (boundary.currentContext!.findRenderObject()
                          as RenderRepaintBoundary)
                      .toImage(pixelRatio: 1);
              final bytes = await image.toByteData(
                format: ui.ImageByteFormat.png,
              );
              await Directory(directory).create(recursive: true);
              await File(
                '$directory/tv32-menu-${spec.$1.width.toInt()}-text${spec.$2}-${spec.$3.name}.png',
              ).writeAsBytes(bytes!.buffer.asUint8List());
              image.dispose();
            });
          }
          if (spec.$1.width == 320) {
            Key? focused() => FocusManager.instance.primaryFocus?.context
                ?.findAncestorWidgetOfExactType<TextButton>()
                ?.key;
            for (
              var i = 0;
              i < 10 && focused() != const Key('android-tv-mode-scroll');
              i++
            ) {
              await t.sendKeyEvent(LogicalKeyboardKey.arrowDown);
              await t.pump();
              await t.pump(const Duration(milliseconds: 180));
            }
            expect(focused(), const Key('android-tv-mode-scroll'));
            final mode = t.getRect(
              find.byKey(const Key('android-tv-mode-scroll')),
            );
            expect(mode.top, greaterThan(0));
            expect(mode.bottom, lessThan(spec.$1.height));
          }
          await t.tap(close);
          await t.pumpAndSettle();
          expect(await future, isNull);
        } finally {
          debugDisableShadows = shadows;
        }
      },
    );
  }
}

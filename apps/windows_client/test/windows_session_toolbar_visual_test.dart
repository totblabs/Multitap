// Actual Windows toolbar widgets over a synthetic desktop, never a live VM.
// CAPTURE_UI requires an explicit, fresh CAPTURE_DIR. Fonts are the shipped
// Pretendard and Flutter Material Icons; pointer/keyboard events stay in tests.
//
// Session bar v2 (2026-11-10): the bar is the chip, 화면, 폴더·USB, 설정, the
// window's buttons in full screen and disconnect; everything else is in the
// panel its button opens (or, in a narrow bar, in the 메뉴). The fixture is
// the real bar with the real panels (ToolbarV2Bar).
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/l10n/l10n.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_toolbar.dart';

import 'helpers/session_bar_fixture.dart' show ToolbarV2Bar, ToolbarV2Share;

const _bar = Key('session-command-bar');
const _grip = Key('expand-session-toolbar');
const _capture = bool.fromEnvironment('CAPTURE_UI');
const _captureDir = String.fromEnvironment('CAPTURE_DIR');

/// The bar's three commands, as RemoteView keys them.
const _commands = ['stream-menu-화면', 'remote-share', 'remote-settings'];

Future<void> _fonts(WidgetTester tester) async {
  await tester.runAsync(() async {
    final pretendard = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      pretendard.addFont(
        File('assets/fonts/Pretendard-$weight.otf')
            .readAsBytes()
            .then(ByteData.sublistView),
      );
    }
    await pretendard.load();
    final artifacts = File(Platform.resolvedExecutable).parent.parent.parent;
    await (FontLoader('MaterialIcons')..addFont(
          File('${artifacts.path}/material_fonts/MaterialIcons-Regular.otf')
              .readAsBytes()
              .then(ByteData.sublistView),
        ))
        .load();
  });
}

Rect _visibleRect(Element element) {
  final box = element.findRenderObject()! as RenderBox;
  return Rect.fromPoints(
    box.localToGlobal(Offset.zero),
    box.localToGlobal(box.size.bottomRight(Offset.zero)),
  );
}

class _Fixture extends StatelessWidget {
  const _Fixture({
    super.key,
    this.bright = false,
    this.connected = true,
    this.active = false,
    this.fullscreen = false,
    this.touch = false,
    this.held = false,
    this.share,
    this.shareReason,
    this.onAction,
  });

  final bool bright, connected, active, fullscreen, touch, held;

  /// Live fakes for the 폴더·USB panel; without them the button only shows
  /// what is connected ([active]).
  final ToolbarV2Share? share;
  final String? shareReason;
  final ValueChanged<String>? onAction;

  @override
  Widget build(BuildContext context) => Stack(
    fit: StackFit.expand,
    children: [
      ColoredBox(
        color: bright ? const Color(0xFFF6F7F8) : const Color(0xFF111820),
        child: Column(
          children: [
            Container(
              height: 34,
              color: bright ? const Color(0xFFE7E9EB) : const Color(0xFF1C252D),
              padding: const EdgeInsets.symmetric(horizontal: 16),
              alignment: Alignment.centerLeft,
              child: Text(
                'WorkPC · 문서',
                style: TextStyle(
                  fontSize: 12,
                  color: bright
                      ? const Color(0xFF47515B)
                      : const Color(0xFFAFBAC3),
                ),
              ),
            ),
            Expanded(
              child: Align(
                alignment: const Alignment(-.8, -.5),
                child: Text(
                  '작업 공간',
                  style: TextStyle(
                    fontSize: 22,
                    color: bright
                        ? const Color(0xFF535D67)
                        : const Color(0xFF738391),
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
      ToolbarV2Bar(
        title: 'WorkPC',
        connected: connected,
        latencyMs: 18,
        shown: fullscreen
            ? SessionDisplayMode.fullscreen
            : SessionDisplayMode.windowed,
        modes: const [
          SessionDisplayMode.windowed,
          SessionDisplayMode.fullscreen,
          SessionDisplayMode.allMonitors,
        ],
        // Full screen has no title bar: the bar carries 최소화 and 창 모드로.
        windowButtons: fullscreen,
        // 마우스 잠금 is on and something of each kind is connected.
        gameMouse: active,
        folders: active ? 1 : 0,
        devices: active ? 1 : 0,
        share: share,
        shareReason: shareReason,
        pointerHeld: held,
        touch: touch,
        onAction: onAction,
      ),
    ],
  );
}

Future<void> _mount(
  WidgetTester tester, {
  double width = 1440,
  double textScale = 1,
  bool bright = false,
  bool connected = true,
  bool active = false,
  bool fullscreen = false,
  bool touch = false,
  bool held = false,
  ToolbarV2Share? share,
  String? shareReason,
  ValueChanged<String>? onAction,
}) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = Size(width, 720);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(),
      debugShowCheckedModeBanner: false,
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(context)
            .copyWith(textScaler: TextScaler.linear(textScale)),
        child: child!,
      ),
      home: Scaffold(
        body: _Fixture(
          key: UniqueKey(),
          bright: bright,
          connected: connected,
          active: active,
          fullscreen: fullscreen,
          touch: touch,
          held: held,
          share: share,
          shareReason: shareReason,
          onAction: onAction,
        ),
      ),
    ),
  );
  await tester.pump();
}

Future<void> _open(WidgetTester tester) async {
  await tester.tap(find.byKey(_grip));
  await tester.pump();
  expect(find.byKey(_bar), findsOneWidget);
}

/// The buttons on the open bar: the chip, the commands (or the 메뉴), the
/// window's buttons and disconnect.
Finder _barButtons() => find.descendant(
  of: find.byKey(_bar),
  matching: find.byWidgetPredicate(
    (widget) => widget is TextButton || widget is IconButton,
  ),
);

void main() {
  // Also in English (docs/i18n.md): every surface without Korean but the
  // fixture's own desktop text, and nothing overflows.
  for (final english in [false, true]) {
    testWidgets(
      'Windows toolbar real-widget capture matrix${english ? ' in English' : ''}',
      (tester) async {
        AppLanguageController.debugSystemLanguage = english ? 'en' : 'ko';
        await _fonts(tester);
        final previousShadows = debugDisableShadows;
        debugDisableShadows = false;
        addTearDown(tester.view.reset);
        try {
          final audit = <Map<String, Object?>>[];
          // Every surface of the bar: the handle, the open bar, each panel
          // (연결, 화면, 폴더·USB on both tabs, 설정, 설정 › 화질) and the
          // narrow bar's 메뉴.
          final cases = [
            (name: 'dark-grip', width: 1440.0, scale: 1.0, view: 'grip'),
            (name: 'dark-hover', width: 1440.0, scale: 1.0, view: 'hover'),
            (name: 'dark-open', width: 1440.0, scale: 1.0, view: 'open'),
            (name: 'bright-open', width: 1440.0, scale: 1.0, view: 'open'),
            (name: 'dark-active', width: 1440.0, scale: 1.0, view: 'open'),
            (name: 'dark-waiting', width: 1440.0, scale: 1.0, view: 'open'),
            (name: 'dark-fullscreen', width: 1440.0, scale: 1.0, view: 'open'),
            (
              name: 'dark-connection',
              width: 1440.0,
              scale: 1.0,
              view: 'connection',
            ),
            (name: 'dark-screen', width: 1440.0, scale: 1.0, view: 'screen'),
            (
              name: 'dark-share-folders',
              width: 1440.0,
              scale: 1.0,
              view: 'share-folders',
            ),
            (
              name: 'dark-share-usb',
              width: 1440.0,
              scale: 1.0,
              view: 'share-usb',
            ),
            (
              name: 'dark-settings',
              width: 1440.0,
              scale: 1.0,
              view: 'settings',
            ),
            (
              name: 'dark-settings-quality',
              width: 1440.0,
              scale: 1.0,
              view: 'settings-quality',
            ),
            (name: 'dark-narrow-open', width: 640.0, scale: 1.0, view: 'open'),
            (
              name: 'dark-narrow-overflow',
              width: 360.0,
              scale: 1.0,
              view: 'overflow',
            ),
            (name: 'dark-text200', width: 1280.0, scale: 2.0, view: 'open'),
            // 480 px: at 640 the three commands still fit at 200% text.
            (
              name: 'bright-text200',
              width: 480.0,
              scale: 2.0,
              view: 'overflow',
            ),
          ];
          for (final sample in cases) {
            final boundary = GlobalKey();
            final share = sample.view.startsWith('share')
                ? await ToolbarV2Share.start(tester)
                : null;
            await _mount(
              tester,
              width: sample.width,
              textScale: sample.scale,
              bright: sample.name.startsWith('bright'),
              connected: sample.name != 'dark-waiting',
              active: sample.name == 'dark-active',
              fullscreen: sample.name == 'dark-fullscreen',
              share: share,
            );
            // Keep the Navigator overlay inside the image, including real popups.
            final app = tester.widget<MaterialApp>(find.byType(MaterialApp));
            await tester.pumpWidget(RepaintBoundary(key: boundary, child: app));
            await tester.pump();
            TestGesture? mouse;
            if (sample.view == 'hover') {
              mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
              await mouse.addPointer(location: const Offset(700, 300));
              await mouse.moveTo(tester.getCenter(find.byKey(_grip)));
              await tester.pump();
            } else if (sample.view != 'grip') {
              await _open(tester);
            }
            switch (sample.view) {
              case 'connection':
                await tester.tap(find.byKey(const Key('session-quality')));
                break;
              case 'screen':
                await tester.tap(find.byKey(const Key('stream-menu-화면')));
                break;
              case 'share-folders':
              case 'share-usb':
                await tester.tap(find.byKey(const Key('remote-share')));
                break;
              case 'settings':
              case 'settings-quality':
                await tester.tap(find.byKey(const Key('remote-settings')));
                break;
              case 'overflow':
                await tester.tap(
                  find.byKey(const Key('session-toolbar-overflow')),
                );
                break;
            }
            if (share != null) {
              // The panel follows live fakes: fixed frames, not pumpAndSettle.
              for (var i = 0; i < 6; i++) {
                await tester.pump(const Duration(milliseconds: 80));
              }
              if (sample.view == 'share-usb') {
                await tester.tap(find.byKey(const Key('remote-usb')));
                for (var i = 0; i < 6; i++) {
                  await tester.pump(const Duration(milliseconds: 80));
                }
              }
            } else {
              await tester.pumpAndSettle();
            }
            if (sample.view == 'settings-quality') {
              // The row and its InkWell share the key.
              await tester.tap(
                find.byKey(const Key('remote-stream-settings')).last,
              );
              await tester.pumpAndSettle();
            }
            if (sample.view == 'connection') {
              expect(
                find.byKey(const Key('session-connection-panel')),
                findsOneWidget,
              );
              expect(
                find.byKey(const Key('quality-value-rate')),
                findsOneWidget,
              );
            } else if (sample.view == 'screen') {
              expect(
                find.byKey(const Key('session-screen-panel')),
                findsOneWidget,
              );
              expect(
                find.byKey(const Key('stream-all-monitors')),
                findsOneWidget,
              );
              expect(
                find.byKey(const Key('display-scale-steps')),
                findsOneWidget,
              );
            } else if (sample.view == 'share-folders') {
              expect(
                find.byKey(const Key('session-share-panel')),
                findsOneWidget,
              );
              expect(find.byKey(const Key('drive-panel')), findsOneWidget);
            } else if (sample.view == 'share-usb') {
              expect(
                find.byKey(const Key('session-share-panel')),
                findsOneWidget,
              );
              expect(find.byKey(const Key('usb-panel')), findsOneWidget);
            } else if (sample.view == 'settings') {
              expect(
                find.byKey(const Key('session-settings-panel')),
                findsOneWidget,
              );
              expect(
                find.byKey(const Key('pin-session-toolbar')),
                findsWidgets,
              );
            } else if (sample.view == 'settings-quality') {
              expect(
                find.byKey(const Key('stream-settings-panel')),
                findsOneWidget,
              );
              expect(
                find.byKey(const Key('stream-settings-back')),
                findsOneWidget,
              );
            } else if (sample.view == 'overflow') {
              expect(find.byType(PopupMenuItem<int>), findsWidgets);
              expect(
                find.byKey(const Key('pin-session-toolbar')),
                findsOneWidget,
              );
            }
            final buttons = <Map<String, Object?>>[];
            for (final element in _barButtons().evaluate()) {
              final rect = _visibleRect(element);
              buttons.add({
                'key': element.widget.key?.toString(),
                'type': element.widget.runtimeType.toString(),
                'x': rect.left,
                'y': rect.top,
                'width': rect.width,
                'height': rect.height,
              });
              expect(rect.left, greaterThanOrEqualTo(-.5));
              expect(rect.right, lessThanOrEqualTo(sample.width + .5));
            }
            expect(tester.takeException(), isNull, reason: sample.name);
            // In English the same surfaces must not overflow either. The bar
            // here is the test's own copy with Korean fixture words
            // (ToolbarV2Bar), so the no-Korean check of the real session
            // panels is in test/android/android_ui_layout_test.dart.
            audit.add({
              'name': sample.name,
              'width': sample.width,
              'textScale': sample.scale,
              'buttons': buttons,
              // A bar button is SessionChrome.button (34 px) tall.
              'under34pxMouseTargets': buttons
                  .where(
                    (button) =>
                        (button['height']! as double) <
                        SessionChrome.button - .5,
                  )
                  .length,
            });
            if (_capture) {
              if (_captureDir.isEmpty) {
                throw StateError(
                  'CAPTURE_DIR must name a fresh explicit directory',
                );
              }
              await tester.runAsync(() async {
                final file = File('$_captureDir/${sample.name}.png');
                if (await file.exists()) {
                  throw StateError('Capture exists: ${file.path}');
                }
                await file.parent.create(recursive: true);
                final render =
                    boundary.currentContext!.findRenderObject()!
                        as RenderRepaintBoundary;
                final image = await render.toImage(pixelRatio: 1);
                try {
                  final bytes = (await image.toByteData(
                    format: ui.ImageByteFormat.png,
                  ))!;
                  await file.writeAsBytes(
                    bytes.buffer.asUint8List(),
                    flush: true,
                  );
                } finally {
                  image.dispose();
                }
              });
            }
            await mouse?.removePointer();
            await tester.pumpWidget(const SizedBox.shrink());
            if (share != null) {
              await share.close(tester);
              await tester.pump(const Duration(seconds: 1));
            }
            await tester.pumpAndSettle();
          }
          if (_capture) {
            await tester.runAsync(() async {
              final file = File('$_captureDir/geometry-audit.json');
              if (await file.exists()) {
                throw StateError('Audit exists: ${file.path}');
              }
              await file.writeAsString(
                const JsonEncoder.withIndent('  ').convert(audit),
                flush: true,
              );
            });
          }
        } finally {
          debugDisableShadows = previousShadows;
        }
      },
    );
  }

  testWidgets(
    'Windows toolbar commands share visible 34 px targets at 100/200%',
    (tester) async {
      await _fonts(tester);
      addTearDown(tester.view.reset);
      for (final scale in [1.0, 2.0]) {
        for (final fullscreen in [false, true]) {
          await _mount(tester, textScale: scale, fullscreen: fullscreen);
          await _open(tester);
          final buttons = _barButtons();
          // The chip, 화면, 폴더·USB, 설정 and disconnect; full screen adds
          // 최소화 and 창 모드로. Nothing else is on the bar.
          expect(
            buttons,
            findsNWidgets(fullscreen ? 7 : 5),
            reason: 'text $scale, full screen $fullscreen',
          );
          for (final element in buttons.evaluate()) {
            final rect = _visibleRect(element);
            final chip = element.widget.key == const Key('session-quality');
            if (chip && scale > 1) {
              // The chip alone has a minimum height, not a fixed one: its
              // text line may outgrow 34 px at 200%, never the bar.
              expect(
                rect.height,
                inInclusiveRange(
                  SessionChrome.button - .1,
                  SessionChrome.barHeight,
                ),
                reason: '${element.widget.key}',
              );
            } else {
              expect(
                rect.height,
                closeTo(SessionChrome.button, .1),
                reason: '${element.widget.key}',
              );
            }
            expect(rect.width, greaterThanOrEqualTo(34));
          }
          for (final key in _commands) {
            expect(find.byKey(Key(key)).hitTestable(), findsOneWidget);
          }
          expect(
            find.byKey(const Key('session-minimize')),
            fullscreen ? findsOneWidget : findsNothing,
          );
          expect(
            find.byKey(const Key('session-restore')),
            fullscreen ? findsOneWidget : findsNothing,
          );
          // The former bar buttons are in the panels now.
          for (final key in [
            'session-fullscreen',
            'session-game-mouse',
            'pin-session-toolbar',
            'remote-display-scale',
            'remote-stream-settings',
            'remote-drives',
            'remote-usb',
            'remote-session-guide',
          ]) {
            expect(find.byKey(Key(key)), findsNothing, reason: key);
          }
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox.shrink());
        }
      }
    },
  );

  testWidgets(
    'toolbar command fires once and disabled connection stays quiet',
    (tester) async {
      await _fonts(tester);
      addTearDown(tester.view.reset);
      final actions = <String>[];
      await _mount(tester, onAction: actions.add);
      await _open(tester);
      // 마우스 잠금 is a row of the 설정 panel: one press there is one
      // command, and the bar folds after it.
      await tester.tap(find.byKey(const Key('remote-settings')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('session-settings-panel')), findsOneWidget);
      expect(actions.where((action) => action == 'mouse'), isEmpty);
      await tester.tap(find.byKey(const Key('session-game-mouse')).last);
      await tester.pumpAndSettle();
      expect(actions.where((action) => action == 'mouse'), hasLength(1));
      expect(find.byKey(const Key('session-settings-panel')), findsNothing);
      expect(find.byKey(_bar), findsNothing);
      // Nothing to share with yet: 폴더·USB is greyed and opens nothing.
      await _mount(
        tester,
        connected: false,
        shareReason: '관리자 허용이 필요해요',
        onAction: actions.add,
      );
      await _open(tester);
      expect(find.byTooltip('폴더·USB · 관리자 허용이 필요해요'), findsOneWidget);
      actions.clear();
      await tester.tap(find.byKey(const Key('remote-share')));
      await tester.pumpAndSettle();
      // The press reaches the bar (input returns to the PC), no command.
      expect(actions.where((action) => action != 'interact'), isEmpty);
      expect(find.byKey(const Key('session-share-panel')), findsNothing);
      expect(find.byKey(_bar), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );
}

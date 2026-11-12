// Actual widget rendering of the streaming session bar (v2, 2026-11-10) over
// a dark photo and a bright desktop, at 100% and 150%: the resting handle,
// its hover tab, the open bar on one and two monitors, the 화면 panel, the
// 설정 panel and 설정 › 화질. Fixture status only, not a remote desktop.
import 'package:flutter/gestures.dart' show PointerDeviceKind;
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/app_menu.dart';
import 'package:multitap_client/ui/app_theme.dart';

import 'helpers/session_bar_fixture.dart' show ToolbarV2Bar;

/// A bright remote desktop: a maximized app with a white title bar.
class _BrightDesktop extends StatelessWidget {
  const _BrightDesktop();
  @override
  Widget build(BuildContext context) => const Column(
    children: [
      ColoredBox(
        color: Color(0xFFF3F3F3),
        child: SizedBox(
          height: 32,
          child: Row(
            children: [
              SizedBox(width: 12),
              Icon(
                Icons.description_outlined,
                size: 15,
                color: Color(0xFF3A6EA5),
              ),
              SizedBox(width: 8),
              Text(
                '제목 없음 - 메모장',
                style: TextStyle(fontSize: 12, color: Color(0xFF1B1B1B)),
              ),
              Spacer(),
              Icon(Icons.minimize, size: 14, color: Color(0xFF1B1B1B)),
              SizedBox(width: 22),
              Icon(Icons.crop_square, size: 13, color: Color(0xFF1B1B1B)),
              SizedBox(width: 22),
              Icon(Icons.close, size: 14, color: Color(0xFF1B1B1B)),
              SizedBox(width: 14),
            ],
          ),
        ),
      ),
      Divider(height: 1, color: Color(0xFFE0E0E0)),
      Expanded(child: ColoredBox(color: Colors.white)),
    ],
  );
}

void main() {
  testWidgets(
    'Session bar preview: handle, hover tab, open bar on one and two monitors, 화면 panel, 설정 panel and 화질',
    (tester) async {
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      for (final scale in [1.0, 1.5]) {
        for (final bright in [false, true]) {
          for (final view in [
            'grip',
            'tab',
            'one-monitor-window',
            'one-monitor-fullscreen',
            'two-monitors',
            'screen',
            'settings',
            'settings-quality',
          ]) {
            final two = view == 'two-monitors' || view == 'screen';
            final fullscreen = view != 'one-monitor-window';
            final mode = two
                ? SessionDisplayMode.allMonitors
                : fullscreen
                ? SessionDisplayMode.fullscreen
                : SessionDisplayMode.windowed;
            const width = 1000.0, height = 520.0;
            tester.view.devicePixelRatio = scale;
            tester.view.physicalSize = const Size(width, height) * scale;
            await tester.pumpWidget(
              MaterialApp(
                debugShowCheckedModeBanner: false,
                theme: buildAppTheme(),
                navigatorObservers: [MenuDismissObserver()],
                home: Scaffold(
                  body: Stack(
                    fit: StackFit.expand,
                    children: [
                      if (bright)
                        const _BrightDesktop()
                      else
                        const ColoredBox(color: Color(0xFF26384B)),
                      // The real bar with RemoteView's commands and the
                      // real panels; a connection without 폴더·USB.
                      ToolbarV2Bar(
                        title: 'TESTER',
                        latencyMs: 7,
                        shown: mode,
                        modes: [
                          SessionDisplayMode.windowed,
                          SessionDisplayMode.fullscreen,
                          if (two) SessionDisplayMode.allMonitors,
                        ],
                        // No title bar in full screen: 최소화, 창 모드로.
                        windowButtons: fullscreen,
                        // 마우스 잠금 is not offered on two monitors.
                        mouseLockOffered: !two,
                        shareOffered: false,
                      ),
                    ],
                  ),
                ),
              ),
            );
            await tester.pump();
            TestGesture? mouse;
            if (view == 'tab') {
              mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
              await mouse.addPointer(location: const Offset(500, 300));
              await mouse.moveTo(
                tester.getCenter(find.byKey(const Key('session-tab'))),
              );
              await tester.pump();
              // Hover shows the latency; it never opens the bar.
              expect(
                find.byKey(const Key('session-handle-grip')),
                findsNothing,
              );
              expect(
                find.byKey(const Key('session-command-bar')),
                findsNothing,
              );
            }
            if (view == 'grip') {
              expect(find.byKey(const Key('session-handle-grip')), findsOne);
            }
            if (view != 'grip' && view != 'tab') {
              await tester.tap(find.byKey(const Key('expand-session-toolbar')));
              await tester.pump();
              expect(find.byKey(const Key('session-command-bar')), findsOne);
              // The bar: the chip, 화면, 설정, the window's buttons in full
              // screen, disconnect, and the handle under it that folds it.
              for (final key in [
                'session-quality',
                'stream-menu-화면',
                'remote-settings',
                'session-disconnect',
                'session-toolbar-fold',
              ]) {
                expect(find.byKey(Key(key)), findsOne, reason: key);
              }
              expect(find.byKey(const Key('remote-share')), findsNothing);
              expect(find.byKey(const Key('session-fullscreen')), findsNothing);
              expect(
                find.byKey(const Key('session-minimize')),
                fullscreen ? findsOne : findsNothing,
              );
              expect(
                find.byKey(const Key('session-restore')),
                fullscreen ? findsOne : findsNothing,
              );
              // 마우스 잠금, 바 열어 두기 and 화질 are rows of the 설정
              // panel now, not bar buttons.
              for (final key in [
                'session-game-mouse',
                'pin-session-toolbar',
                'remote-stream-settings',
                'session-pointer-confine',
              ]) {
                expect(find.byKey(Key(key)), findsNothing, reason: key);
              }
            }
            if (view == 'screen') {
              await tester.tap(find.byKey(const Key('stream-menu-화면')));
              await tester.pumpAndSettle();
              expect(find.byKey(const Key('session-screen-panel')), findsOne);
              expect(find.byKey(const Key('stream-all-monitors')), findsOne);
              expect(find.text('모든 모니터'), findsWidgets);
              // 배율 is in the same panel, five steps across.
              expect(find.byKey(const Key('display-scale-steps')), findsOne);
            }
            if (view == 'settings' || view == 'settings-quality') {
              await tester.tap(find.byKey(const Key('remote-settings')));
              await tester.pumpAndSettle();
              expect(find.byKey(const Key('session-settings-panel')), findsOne);
              // A row and its InkWell share the key.
              expect(
                find.byKey(const Key('session-game-mouse')),
                two ? findsNothing : findsWidgets,
              );
              expect(
                find.byKey(const Key('pin-session-toolbar')),
                findsWidgets,
              );
              expect(
                find.byKey(const Key('remote-stream-settings')),
                findsWidgets,
              );
            }
            if (view == 'settings-quality') {
              await tester.tap(
                find.byKey(const Key('remote-stream-settings')).last,
              );
              await tester.pumpAndSettle();
              expect(
                find.byKey(const Key('session-settings-panel')),
                findsNothing,
              );
              expect(find.byKey(const Key('stream-settings-panel')), findsOne);
              expect(find.byKey(const Key('stream-settings-back')), findsOne);
            }
            expect(tester.takeException(), isNull);
            await mouse?.removePointer();
            await tester.pumpWidget(const SizedBox.shrink());
            await tester.pump(const Duration(milliseconds: 300));
          }
        }
      }
    },
  );
}

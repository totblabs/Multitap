import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/ui/app_palette.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';
import 'package:multitap_client/ui/themed_app.dart';

import 'auth_flow_test.dart' show TestLoginMemory;

void main() {
  testWidgets(
    'Both login themes share geometry, keep scenery exposed and adapt to short windows',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetPhysicalSize);
      for (final size in [
        const Size(1440, 900),
        const Size(1100, 700),
        const Size(640, 600),
        const Size(480, 480),
        const Size(320, 360),
      ]) {
        for (final scale in [1.0, 1.75, 2.0]) {
          Rect? darkCard;
          for (final mode in [ThemeMode.dark, ThemeMode.light]) {
            tester.view.physicalSize = size;
            final appearance = AppAppearance(mode: mode);
            await tester.pumpWidget(
              ThemedApp(
                appearance: appearance,
                home: MediaQuery(
                  data: MediaQueryData(
                    size: size,
                    textScaler: TextScaler.linear(scale),
                  ),
                  child: SignInScreen(
                    onLogin: (_, _) async => SignInResult.rejected,
                    serverOrigin: Uri.parse('https://cloud.example.com'),
                    onServerChanged: (_) async {},
                    loginMemory: TestLoginMemory(),
                  ),
                ),
              ),
            );
            await tester.pumpAndSettle();
            expect(
              tester.takeException(),
              isNull,
              reason: '$mode $size $scale',
            );
            expect(find.byType(Scrollbar), findsNothing);
            final rect = tester.getRect(find.byKey(const Key('login-card')));
            if (mode == ThemeMode.dark) {
              darkCard = rect;
            } else {
              expect(rect, darkCard, reason: 'Theme only changes appearance');
            }
            expect(rect.left, greaterThanOrEqualTo(0));
            expect(rect.right, lessThanOrEqualTo(size.width));
            final scroll = tester.state<ScrollableState>(
              find
                  .descendant(
                    of: find.byKey(const Key('login-scroll')),
                    matching: find.byType(Scrollable),
                  )
                  .first,
            );
            if (scale == 1 && size.height >= 480) {
              expect(
                scroll.position.maxScrollExtent,
                0,
                reason: '$size standard login fits',
              );
            }
            await tester.ensureVisible(find.byKey(const Key('login-submit')));
            await tester.pumpAndSettle();
            expect(
              tester.getRect(find.byKey(const Key('login-submit'))).bottom,
              lessThanOrEqualTo(size.height),
            );
            expect(tester.takeException(), isNull);
            await tester.pumpWidget(const SizedBox.shrink());
            appearance.dispose();
          }
        }
      }
    },
  );

  // Review 2026-11-12 (signin-light-1280): under the card the scene is dark
  // in both appearances, and the light theme drew its dark ink on it.
  testWidgets(
    'The line under the card and the version mark use the ink that reads on the scene in both themes',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1280, 800);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetPhysicalSize);
      const onScene = AppPalette(true);
      for (final mode in [ThemeMode.light, ThemeMode.dark]) {
        final appearance = AppAppearance(mode: mode);
        await tester.pumpWidget(
          ThemedApp(
            appearance: appearance,
            home: SignInScreen(
              onLogin: (_, _) async => SignInResult.rejected,
              serverOrigin: Uri.parse('https://cloud.example.com'),
              onServerChanged: (_) async {},
              loginMemory: TestLoginMemory(),
            ),
          ),
        );
        await tester.pumpAndSettle();
        final card = tester.element(find.byKey(const Key('login-card')));
        expect(
          Theme.of(card).brightness,
          mode == ThemeMode.light ? Brightness.light : Brightness.dark,
        );
        final footer = tester.widget<Text>(
          find.byKey(const Key('login-footer')),
        );
        expect(footer.style!.color, onScene.ink, reason: '$mode');
        expect(footer.style!.shadows, onScene.sceneHalo, reason: '$mode');
        final version = tester.widget<Text>(
          find.byKey(const Key('login-version')),
        );
        expect(version.style!.color, onScene.ink, reason: '$mode');
        expect(version.style!.shadows, onScene.sceneHalo, reason: '$mode');
        await tester.pumpWidget(const SizedBox.shrink());
        appearance.dispose();
      }
    },
  );

  // Review 2026-11-12: the password was emptied on any failure (a network
  // error, a cancelled two-step verification), and the server dot stayed
  // green right after the server could not be reached.
  testWidgets(
    'Only a wrong ID or password empties the password field; an unreachable server turns the dot',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1280, 800);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetPhysicalSize);
      var result = SignInResult.unreachable;
      final appearance = AppAppearance(mode: ThemeMode.dark);
      addTearDown(appearance.dispose);
      await tester.pumpWidget(
        ThemedApp(
          appearance: appearance,
          home: SignInScreen(
            onLogin: (_, _) async => result,
            serverOrigin: Uri.parse('https://cloud.example.com'),
            onServerChanged: (_) async {},
            loginMemory: TestLoginMemory(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      const p = AppPalette(true);
      Color dot() =>
          (tester
                      .widget<Container>(
                        find.byKey(const Key('login-server-dot')),
                      )
                      .decoration!
                  as BoxDecoration)
              .color!;
      String password() => tester
          .widget<TextField>(find.byKey(const Key('sign-in-password')))
          .controller!
          .text;
      Future<void> submit() async {
        await tester.tap(find.byKey(const Key('login-submit')));
        await tester.pumpAndSettle();
      }

      expect(dot(), p.green);
      await tester.enterText(find.byKey(const Key('sign-in-id')), 'alice');
      await tester.enterText(
        find.byKey(const Key('sign-in-password')),
        'typed-password',
      );
      await submit();
      expect(password(), 'typed-password', reason: 'the server did not answer');
      expect(dot(), p.warn);

      // A cancelled two-step verification, a refusal for another reason.
      result = SignInResult.failed;
      await submit();
      expect(password(), 'typed-password');
      expect(dot(), p.green, reason: 'the server answered');

      result = SignInResult.rejected;
      await submit();
      expect(password(), isEmpty, reason: 'the server said it is wrong');
      expect(dot(), p.green);
    },
  );
}

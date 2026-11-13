import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/ui/app_menu.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/usb_panel.dart';

import 'usb_redirection_test.dart' show FakeBackend;
import 'helpers/pump_work.dart';

class _SetupBackend extends FakeBackend {
  int installCalls = 0;
  @override
  Future<void> installDriver() {
    installCalls++;
    return super.installDriver();
  }
}

// Actual product panel/route/observer; only local driver and restart callbacks
// are synthetic. No native driver, Windows restart, socket or user data.
void main() {
  for (final action in ['install', 'restart']) {
    for (final boundary in [
      'confirm',
      'cancel',
      'escape',
      'owner',
      'unrelated',
    ]) {
      testWidgets('anchored USB setup action=$action boundary=$boundary', (
        tester,
      ) async {
        tester.view.physicalSize = const Size(1280, 900);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.reset);
        final backend = _SetupBackend()..driver = UsbDriverState.missing;
        final usb = UsbSession(
          url: Uri.parse('wss://synthetic.example/usb'),
          token: 'synthetic-token',
          sessionId: 'synthetic-session',
          backend: backend,
          dialer: (_, _) async =>
              throw StateError('No network in setup fixture'),
        );
        await tester.runAsync(() async {
          await usb.refresh();
          if (action == 'restart') {
            backend.installFailure = const UsbBackendException(
              'usbdk_restart_required',
            );
            expect(await usb.installDriver(), 'usbdk_restart_required');
          }
        });
        final beforeInstall = backend.installCalls;
        var restarts = 0;
        final navigator = GlobalKey<NavigatorState>();
        late AnchoredPanelRoute<void> panel;
        await tester.pumpWidget(
          MaterialApp(
            navigatorKey: navigator,
            theme: buildAppTheme(),
            navigatorObservers: [MenuDismissObserver()],
            home: Builder(
              builder: (context) => Scaffold(
                body: TextButton(
                  key: const Key('open-setup-panel'),
                  onPressed: () {
                    panel = AnchoredPanelRoute<void>(
                      anchor: const Rect.fromLTWH(600, 15, 32, 32),
                      width: 400,
                      label: 'USB 장치',
                      keepOpenForDialog: true,
                      builder: (_) => UsbPanel(
                        usb: usb,
                        restartPc: () async {
                          restarts++;
                          return true;
                        },
                      ),
                    );
                    unawaited(navigator.currentState!.push(panel));
                  },
                  child: const Text('open'),
                ),
              ),
            ),
          ),
        );
        await tester.tap(find.byKey(const Key('open-setup-panel')));
        await pumpWork(tester);
        final launch = Key(
          action == 'install' ? 'usb-install' : 'usb-restart-now',
        );
        await tester.ensureVisible(find.byKey(launch));
        await tester.tap(find.byKey(launch));
        await pumpWork(tester);
        final yes = Key(
          action == 'install'
              ? 'usb-install-confirm'
              : 'usb-restart-confirm-yes',
        );
        final no = Key(
          action == 'install' ? 'usb-install-cancel' : 'usb-restart-confirm-no',
        );
        expect(find.byKey(yes), findsOneWidget);
        expect(backend.installCalls, beforeInstall);
        expect(restarts, 0);
        if (boundary == 'confirm') {
          // Press the real confirmation before checking the side effect, so
          // RED proves the reported silent no-op rather than only lost layout.
          await tester.tap(find.byKey(yes));
        } else if (boundary == 'cancel') {
          await tester.tap(find.byKey(no));
        } else if (boundary == 'escape') {
          await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        } else if (boundary == 'owner') {
          if (panel.isActive) navigator.currentState!.removeRoute(panel);
        } else {
          unawaited(
            navigator.currentState!.push(
              MaterialPageRoute<void>(
                builder: (_) => const Scaffold(body: Text('unrelated page')),
              ),
            ),
          );
        }
        await pumpWork(tester);
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: 25)),
        );
        await pumpWork(tester);
        expect(
          backend.installCalls,
          beforeInstall +
              (boundary == 'confirm' && action == 'install' ? 1 : 0),
        );
        expect(restarts, boundary == 'confirm' && action == 'restart' ? 1 : 0);
        if (boundary == 'confirm' ||
            boundary == 'cancel' ||
            boundary == 'escape') {
          expect(find.byType(UsbPanel), findsOneWidget);
          expect(find.byKey(yes), findsNothing);
          if (boundary == 'confirm' && action == 'install') {
            expect(backend.driver, UsbDriverState.ready);
            expect(find.byKey(const Key('usb-install')), findsNothing);
          }
        } else {
          expect(find.byType(UsbPanel), findsNothing);
          expect(find.byKey(yes), findsNothing);
        }
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.runAsync(usb.close);
      });
    }
  }
}

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/app_menu.dart';
import 'package:multitap_client/ui/session_toolbar.dart';
import 'package:multitap_client/ui/usb_panel.dart';

import 'usb_redirection_test.dart'
    show started, shared, stick, FakeBackend, FakeSocket, FakeCapture;

// Actual product anchored toolbar route and USB panel; relay and capture are
// synthetic. No production socket, device, native method, driver or user data.
void main() {
  for (final boundary in ['normal', 'channel', 'owner']) {
    final closeDuringConfirm = boundary != 'normal';
    testWidgets('anchored toolbar USB detach confirm boundary=$boundary', (
      tester,
    ) async {
      tester.view.physicalSize = const Size(1280, 900);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      late UsbSession usb;
      late FakeSocket socket;
      late FakeBackend backend;
      late FakeCapture capture;
      await tester.runAsync(() async {
        final f = await started(devices: [stick()]);
        usb = f.$1;
        socket = f.$2;
        backend = f.$3;
        final current = await shared(usb, socket, backend);
        capture = current.$2;
      });
      final popups = SessionPopups();
      var usbOpen = false;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          navigatorObservers: [
            MenuDismissObserver(),
          ], // ThemedApp's actual observer.
          home: StatefulBuilder(
            builder: (context, setState) {
              Future<bool> openPanel(Rect anchor) async {
                setState(() => usbOpen = true);
                try {
                  await showUsbPanel(
                    context,
                    usb: usb,
                    anchor: anchor,
                    popups: popups,
                  );
                } finally {
                  setState(() => usbOpen = false);
                }
                return false; // RemoteView._usbDevices does not fold the toolbar.
              }

              return Scaffold(
                body: SessionToolbar(
                  title: 'SYNTHETIC VM',
                  connected: true,
                  onInteract: () {},
                  popups: popups,
                  contentKey: 'controlled-stream',
                  layoutKey: 'controlled-layout',
                  actions: [
                    SessionToolbarMenu.command(
                      key: const Key('synthetic-toolbar-usb'),
                      label: 'USB 장치',
                      icon: Icons.usb_rounded,
                      onInteract: () {},
                      onPressed: usbOpen
                          ? null
                          : () => unawaited(
                              openPanel(const Rect.fromLTWH(600, 15, 32, 32)),
                            ),
                      onPressedAt: usbOpen ? null : openPanel,
                    ),
                  ],
                ),
              );
            },
          ),
        ),
      );
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('synthetic-toolbar-usb')));
      await tester.pumpAndSettle();
      final stop = find.byKey(const ValueKey('usb-stop-stick'));
      expect(tester.widget<OutlinedButton>(stop).onPressed, isNotNull);
      await tester.ensureVisible(stop);
      await tester.tap(stop);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('usb-stop-confirm')), findsOneWidget);
      expect(
        find.byType(UsbPanel),
        findsOneWidget,
        reason: 'A confirmation must not dispose its USB panel owner.',
      );
      expect(socket.texts.where((m) => m['type'] == 'usb.detach'), isEmpty);
      if (closeDuringConfirm) {
        if (boundary == 'channel') {
          await tester.runAsync(() async {
            socket.emit({'type': 'usb.error', 'reason': 'usb_session_closed'});
            await Future<void>.delayed(const Duration(milliseconds: 20));
          });
        } else {
          popups.closeAll();
        }
        await tester.pump();
      } else {
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('usb-stop-confirm')), findsNothing);
        expect(find.byType(UsbPanel), findsOneWidget);
        expect(usb.entries.single.state, UsbEntryState.shared);
        await tester.tap(stop);
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('usb-stop-confirm-no')));
        await tester.pumpAndSettle();
        expect(usb.entries.single.state, UsbEntryState.shared);
        await tester.tap(stop);
        await tester.pumpAndSettle();
        capture.releaseGate = Completer<void>();
      }
      if (boundary != 'owner') {
        await tester.tap(find.byKey(const Key('usb-stop-confirm-yes')));
      } else {
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('usb-stop-confirm')), findsNothing);
      }
      await tester.pump();
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 25)),
      );
      await tester.pump();
      if (closeDuringConfirm) {
        expect(
          socket.texts.where(
            (m) =>
                m['type'] == 'usb.detach' && m['reason'] == 'usb_user_stopped',
          ),
          isEmpty,
        );
      } else {
        expect(
          socket.texts.where(
            (m) =>
                m['type'] == 'usb.detach' && m['reason'] == 'usb_user_stopped',
          ),
          hasLength(1),
        );
        expect(find.text('되돌리는 중'), findsOneWidget);
        expect(tester.widget<OutlinedButton>(stop).onPressed, isNull);
        expect(capture.releaseCalls, 1);
        await tester.runAsync(() async {
          capture.releaseGate!.complete();
          await Future<void>.delayed(const Duration(milliseconds: 25));
        });
        await tester.pump();
        expect(usb.entries.single.state, UsbEntryState.available);
      }
      expect(tester.takeException(), isNull);
      popups.closeAll();
      await tester.pumpAndSettle();
      await tester.runAsync(usb.close);
      await tester.pumpWidget(const SizedBox());
    });
  }

  for (final boundary in [
    'unmarked',
    'wrong-owner',
    'reuse',
    'page',
    'dialog',
    'replacement',
  ]) {
    testWidgets('owned confirmation retires safely for $boundary', (
      tester,
    ) async {
      final navigator = GlobalKey<NavigatorState>();
      RouteSettings? marker;
      await tester.pumpWidget(
        MaterialApp(
          navigatorKey: navigator,
          navigatorObservers: [MenuDismissObserver()],
          home: const Scaffold(body: Text('root')),
        ),
      );
      unawaited(
        navigator.currentState!.push(
          AnchoredPanelRoute<void>(
            anchor: const Rect.fromLTWH(200, 10, 30, 30),
            width: 400,
            label: 'owned panel',
            keepOpenForDialog: true,
            builder: (context) => TextButton(
              key: const Key('owned-confirm-launch'),
              onPressed: () {
                marker ??= AnchoredPanelRoute.ownedDialogSettings(context);
                unawaited(
                  showDialog<void>(
                    context: context,
                    routeSettings: boundary == 'unmarked'
                        ? null
                        : boundary == 'wrong-owner'
                        ? AnchoredPanelRoute.ownedDialogSettings(
                            navigator.currentState!.overlay!.context,
                          )
                        : marker,
                    builder: (_) => const AlertDialog(
                      key: Key('owned-confirm-dialog'),
                      content: Text('confirm'),
                    ),
                  ),
                );
              },
              child: const Text('confirm'),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('owned-confirm-launch')));
      await tester.pumpAndSettle();
      if (boundary == 'unmarked' || boundary == 'wrong-owner') {
        expect(find.byKey(const Key('owned-confirm-launch')), findsNothing);
      } else {
        expect(find.byKey(const Key('owned-confirm-launch')), findsOneWidget);
        if (boundary == 'reuse') {
          navigator.currentState!.pop();
          await tester.pumpAndSettle();
          await tester.tap(find.byKey(const Key('owned-confirm-launch')));
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('owned-confirm-launch')), findsNothing);
        } else {
          final route = MaterialPageRoute<void>(
            builder: (_) => const Scaffold(body: Text('unrelated page')),
          );
          if (boundary == 'dialog') {
            unawaited(
              showDialog<void>(
                context: navigator.currentState!.overlay!.context,
                builder: (_) =>
                    const AlertDialog(content: Text('unrelated dialog')),
              ),
            );
          } else if (boundary == 'page') {
            unawaited(navigator.currentState!.push(route));
          } else {
            unawaited(navigator.currentState!.pushReplacement(route));
          }
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('owned-confirm-launch')), findsNothing);
          expect(find.byKey(const Key('owned-confirm-dialog')), findsNothing);
          expect(
            find.text(
              boundary == 'dialog' ? 'unrelated dialog' : 'unrelated page',
            ),
            findsOneWidget,
          );
        }
      }
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
    });
  }
}

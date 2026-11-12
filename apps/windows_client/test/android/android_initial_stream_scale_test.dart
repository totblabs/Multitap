import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_session_ui.dart';

import 'android_large_desktop_test.dart'
    show
        openLargeDesktopStream,
        closeLargeDesktopStreamView,
        openLargeDesktopScreenPanel;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const pen = MethodChannel('multitap/pen-input');
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(pen, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? 'started' : true,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(pen, null);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });
  for (final device in [
    (kind: AndroidDeviceClass.phone, physical: const Size(1280, 720), dpr: 2.0),
    (kind: AndroidDeviceClass.fold, physical: const Size(1400, 1200), dpr: 2.0),
    (
      kind: AndroidDeviceClass.tablet,
      physical: const Size(1600, 1200),
      dpr: 1.0,
    ),
    (
      kind: AndroidDeviceClass.television,
      physical: const Size(1920, 1080),
      dpr: 1.0,
    ),
  ]) {
    for (final savedScale in [0, 150, 200]) {
      testWidgets(
        'fresh Android ${device.kind.name} ignores saved$savedScale before picture',
        (t) async {
          final settings = AndroidSessionSettings(
            helpSeen: true,
            screens: {
              for (final kind in AndroidDeviceClass.values)
                kind: AndroidScreenChoice(
                  scale: savedScale,
                  resolution: const AndroidResolution.workspace(150),
                  orientation: AndroidOrientation.landscape,
                ),
            },
          );
          try {
            final stream = await openLargeDesktopStream(
              t,
              television: device.kind == AndroidDeviceClass.television,
              physical: device.physical,
              dpr: device.dpr,
              settings: settings,
              firstFrame: false,
            );
            expect(stream.scales, [100]);
            expect(stream.session.frameReady, isFalse);
            expect(
              (stream.starts.single.maxWidth, stream.starts.single.maxHeight),
              (
                (device.physical.width * 1.5).round(),
                (device.physical.height * 1.5).round(),
              ),
            );
            final remembered = settings.screen(device.kind);
            expect(remembered.scale, savedScale);
            expect(
              remembered.resolution,
              const AndroidResolution.workspace(150),
            );
            expect(remembered.orientation, AndroidOrientation.landscape);
            expect(
              stream.saved,
              isEmpty,
              reason:
                  'initial scale is session-local, no eager preference rewrite',
            );
          } finally {
            await closeLargeDesktopStreamView(t);
          }
        },
      );
    }
  }

  testWidgets(
    'manual Android scale survives resize, background/resume and reconnect',
    (t) async {
      try {
        final stream = await openLargeDesktopStream(t, television: true);
        expect(stream.scales, [100]);
        await openLargeDesktopScreenPanel(t, television: true);
        var screen = t.widget<AndroidScreenOptions>(
          find.byType(AndroidScreenOptions),
        );
        expect(screen.choice.scale, 100);
        await t.tap(find.byKey(const Key('android-screen-scale-150')));
        await t.pump();
        screen = t.widget<AndroidScreenOptions>(
          find.byType(AndroidScreenOptions),
        );
        expect(screen.choice.scale, 150);
        expect(stream.session.displayScaleWanted, 150);
        expect(
          stream.saved.last.screen(AndroidDeviceClass.television).scale,
          150,
        );
        await t.tap(find.byKey(const Key('android-tv-tools-close')));
        await t.pumpAndSettle();
        t.view.physicalSize = const Size(1440, 900);
        await t.pump(const Duration(milliseconds: 800));
        await t.pump(const Duration(milliseconds: 800));
        expect(stream.scales, [100], reason: 'resize keeps the active stream');
        expect(stream.session.displayScaleWanted, 150);
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
        await t.pump();
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
        await t.pump(const Duration(milliseconds: 400));
        expect(stream.scales, [100]);
        expect(stream.session.displayScaleWanted, 150);
        // Explicitly retry an ended stream; no native display or live VM is used.
        stream.socket.incoming.add(
          jsonEncode({'type': 'session.ended', 'reason': 'host_disconnected'}),
        );
        await t.pump();
        await t.pump(const Duration(milliseconds: 400));
        final retry = find.byKey(const Key('android-retry'));
        if (retry.evaluate().isNotEmpty) {
          await t.tap(retry);
        }
        for (var i = 0; i < 8; i++) {
          await t.pump(const Duration(seconds: 1));
        }
        expect(stream.scales, [100, 150]);
      } finally {
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
        await closeLargeDesktopStreamView(t);
      }
    },
  );
}

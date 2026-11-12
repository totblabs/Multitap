import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_stream_preparation.dart';
import 'package:multitap_client/android/android_toolbar.dart'
    show AndroidPanelChoices;
import 'package:multitap_client/data/display_scale.dart';

import 'android_large_desktop_test.dart' show openLargeDesktopStream;

void main() {
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

  test('Fold manual DPI changes no native source pixels when Host scaling is supported', () {
    const density = AndroidScreenDensity(
      xdpi: 420,
      ydpi: 420,
      width: 2160,
      height: 2160,
    );
    final policy = AndroidScreenPolicy.of(
      const Size(720, 720),
      3,
      density,
      (_) => const AndroidScreenChoice(),
    );
    expect(policy.deviceClass, AndroidDeviceClass.fold);
    final automatic = AndroidViewportPlan.of(
      const Size(2160, 2160),
      3,
      policy,
      hostScale: true,
    );
    final manual = AndroidViewportPlan.of(
      const Size(2160, 2160),
      3,
      AndroidScreenPolicy(
        deviceClass: policy.deviceClass,
        density: density,
        window: policy.window,
        choice: const AndroidScreenChoice(scale: 100),
      ),
      hostScale: true,
    );
    expect(automatic.displayScale, 100);
    expect(manual.displayScale, 100);
    expect((automatic.width, automatic.height), (990, 990));
    expect((manual.width, manual.height), (automatic.width, automatic.height));
    expect(
      androidWindowsTextScaleStatus(
        automatic: true,
        supported: true,
        wanted: 200,
      ),
      '자동 200% 요청 · 실제 값 미확인',
    );
    expect(
      androidWindowsTextScaleStatus(
        automatic: true,
        supported: true,
        wanted: 200,
        applied: const DisplayScaleApplied(
          ok: true,
          scale: 225,
          maximum: 300,
          code: 0,
        ),
      ),
      '현재 225% · 자동 200% 요청',
    );
    expect(
      androidWindowsTextScaleStatus(
        automatic: false,
        supported: true,
        wanted: 100,
        applied: const DisplayScaleApplied(
          ok: true,
          scale: 225,
          maximum: 300,
          code: 0,
        ),
      ),
      '현재 225% · 수동 100% 요청',
    );
  });

  testWidgets(
    'Windows text scale and real remote resolution stay visibly distinct through a request',
    (t) async {
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      const pen = MethodChannel('multitap/pen-input');
      for (final channel in [
        pen,
        AndroidSessionKeepalive.channel,
        AndroidStore.channel,
      ]) {
        messenger.setMockMethodCallHandler(channel, (_) async => null);
      }
      addTearDown(() {
        for (final channel in [
          pen,
          AndroidSessionKeepalive.channel,
          AndroidStore.channel,
        ]) {
          messenger.setMockMethodCallHandler(channel, null);
        }
      });
      final fixture = await openLargeDesktopStream(
        t,
        physical: const Size(2160, 2160),
        dpr: 3,
        resolution: AndroidResolution.auto,
        density: const AndroidScreenDensity(
          xdpi: 420,
          ydpi: 420,
          width: 2160,
          height: 2160,
        ),
      );
      try {
        expect(fixture.scales.first, 100);
        await t.tap(find.byKey(const Key('expand-session-toolbar')));
        await t.pumpAndSettle();
        // 배율 is inside the 화면 panel, as on the PC: no row or panel of its
        // own.
        expect(find.byKey(const Key('android-session-scale')), findsNothing);
        await t.tap(find.byKey(const Key('android-session-screen')));
        await t.pumpAndSettle();
        expect(find.byKey(const Key('android-screen-panel')), findsOneWidget);
        expect(find.byKey(const Key('android-scale-panel')), findsNothing);
        expect(find.byKey(const Key('android-screen-scale')), findsOneWidget);
        // The observed state heads the panel in plain words too.
        expect(
          t
              .widget<Text>(
                find.byKey(const Key('android-screen-observed-state')),
              )
              .data,
          allOf(contains('원격 해상도 990 × 990'), contains('실제 값 미확인')),
        );
        // The 배율 caption does not say the two lines a second time
        // (inspection, 2026-11-12): only that the two apply separately.
        expect(
          t
              .widget<AndroidPanelChoices>(
                find.byKey(const Key('android-screen-scale')),
              )
              .note,
          '해상도와 Windows 배율은 따로 적용돼요.',
        );
        String observed() => t
            .widget<Text>(
              find.byKey(const Key('android-screen-observed-state')),
            )
            .data!;
        void applied(int scale) => fixture.socket.incoming.add(
          jsonEncode({
            'type': 'display.scale.applied',
            'protocol': displayScaleProtocol,
            'sessionId': 'large-desktop',
            'ok': true,
            'scale': scale,
            'maximum': 300,
            'code': 0,
          }),
        );
        applied(225);
        await t.pump(const Duration(milliseconds: 30));
        expect(observed(), contains('현재 225% · 수동 100% 요청'));
        // 배율 comes after the view rows: bring its step into view.
        await t.ensureVisible(
          find.byKey(const Key('android-screen-scale-100')),
        );
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('android-screen-scale-100')));
        await t.pumpAndSettle();
        expect(fixture.session.displayScaleWanted, 100);
        expect(
          (fixture.session.sourceWidth, fixture.session.sourceHeight),
          (990, 990),
        );
        expect(
          fixture.socket.sent.where((e) => e['type'] == 'stream.resize'),
          isEmpty,
        );
        expect(observed(), contains('현재 225% · 수동 100% 요청'));
        applied(100);
        await t.pump(const Duration(milliseconds: 30));
        expect(observed(), contains('현재 100% · 수동'));
        expect(
          find.textContaining('해상도와 Windows 배율은 따로 적용돼요.', findRichText: true),
          findsOneWidget,
        );
      } finally {
        await t.pumpWidget(const SizedBox.shrink());
        await t.pump(const Duration(seconds: 3));
        await fixture.session.close();
      }
    },
  );
}

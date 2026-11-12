import 'dart:io';
import 'dart:convert';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/models/desktop_use.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/data/appearance_settings.dart';

import 'android_launcher_test.dart'
    show MemoryStore, FakeBroker, vm, signedIn, mainButton;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    final fonts = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      fonts.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await fonts.load();
    final icons = FontLoader('MaterialIcons')
      ..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'));
    await icons.load();
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });
  for (final use in [DesktopUse.mine, DesktopUse.other, DesktopUse.someone]) {
    testWidgets(
      'shows the active session ${use.name} on the status card, as on Windows',
      (tester) async {
        await signedIn(tester, [vm(use: use)]);
        expect(find.text(use.label!), findsOneWidget);
        expect(find.byKey(const Key('desktop-in-use-pve-149')), findsOneWidget);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pumpAndSettle();
      },
    );
  }
  testWidgets('preparation failure has priority over active-session hint', (
    tester,
  ) async {
    await signedIn(tester, [
      vm(
        use: DesktopUse.other,
        stream: false,
        virtualDisplayReady: false,
        preparation: StreamPreparation.login,
      ),
    ]);
    expect(
      // Signed out: the Proxmox console, not an in-stream sign-in (2026-11-09).
      find.text('Console에서 로그인하세요. 다시 시작하면 자동으로 로그인돼요.'),
      findsOneWidget,
    );
    expect(find.text(DesktopUse.other.label!), findsNothing);
    expect(find.byKey(const Key('stream-state-pve-149')), findsOneWidget);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pumpAndSettle();
  });
  for (final size in [
    const Size(300, 720),
    const Size(320, 720),
    const Size(390, 844),
    const Size(800, 390),
    const Size(1280, 800),
    const Size(960, 540),
  ]) {
    for (final scale in [1.0, 1.5, if (size.width <= 320) 2.0]) {
      for (final dark in [false, true]) {
        final name =
            '${size.width.toInt()}x${size.height.toInt()}-${dark ? 'dark' : 'light'}-$scale';
        testWidgets(name, (tester) async {
          final previousError = FlutterError.onError;
          FlutterError.onError = (details) {
            debugPrintSynchronously(details.toString());
            previousError?.call(details);
          };
          addTearDown(() => FlutterError.onError = previousError);
          tester.view.physicalSize = size;
          tester.view.devicePixelRatio = 1;
          tester.platformDispatcher.textScaleFactorTestValue = scale;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
          MemoryStore(
            origin: 'https://vdi.example',
            username: 'tester',
            prefs: {'appearance': dark ? 'dark' : 'light'},
          ).install();
          final broker = FakeBroker([
            vm(
              id: 'pve-101',
              name: '긴 이름의 작업용 데스크톱 · 설계 및 문서 검토',
              resources: VmResources(
                cpuUsagePercent: 23.4,
                vcpus: 4,
                memoryUsedBytes: 3 * 1024 * 1024 * 1024,
                memoryTotalBytes: 8 * 1024 * 1024 * 1024,
                receivedAt: DateTime.now(),
                stale: false,
              ),
            ),
            vm(id: 'pve-102', power: 'stopped', name: '두 번째 데스크톱'),
            vm(id: 'pve-103', stream: false, name: '연결 준비가 필요한 데스크톱'),
          ]);
          await tester.pumpWidget(
            RepaintBoundary(
              key: const Key('capture'),
              child: AndroidVdiApp(
                apiFactory: (_) => broker,
                television: size.width == 960,
                hardwareKeyboard: true,
              ),
            ),
          );
          await tester.pumpAndSettle();
          await tester.enterText(
            find.byKey(const Key('sign-in-password')),
            'fixture-password',
          );
          final login = find.byKey(const Key('login-submit'));
          await tester.ensureVisible(login);
          await tester.pumpAndSettle();
          await tester.tap(login);
          await tester.pumpAndSettle();
          await tester.runAsync(
            () => precacheImage(
              AssetImage(BackgroundScene.peaks.asset(dark: dark)),
              tester.element(find.byKey(const Key('cards-view'))),
            ),
          );
          await tester.pumpAndSettle();
          expect(tester.takeException(), isNull);
          final cards = [
            for (final id in [101, 102, 103])
              tester.getSize(find.byKey(Key('card-pve-$id'))),
          ];
          // The Windows grid's tile width (android_card_parity_test), the
          // same for every state.
          expect(cards[0].width, closeTo(cards[1].width, .1));
          expect(cards[0].height, closeTo(cards[1].height, .1));
          expect(cards[0].height, closeTo(cards[2].height, .1));
          expect(find.textContaining('카드 크기'), findsNothing);
          final primary = mainButton('pve-101');
          expect(tester.getSize(primary).height, greaterThanOrEqualTo(47.99));
          // A TV poster must leave the VM name in the first viewport.
          if (size.width == 960) {
            expect(
              tester.getTopLeft(find.text('긴 이름의 작업용 데스크톱 · 설계 및 문서 검토')).dy,
              lessThan(size.height),
            );
          }
          final out = Platform.environment['ANDROID_CARD_CAPTURE_DIR'];
          if (out != null) {
            await tester.runAsync(() async {
              final boundary = tester.renderObject<RenderRepaintBoundary>(
                find.byKey(const Key('capture')),
              );
              final image = await boundary.toImage(pixelRatio: 1.5);
              final bytes = await image.toByteData(
                format: ui.ImageByteFormat.png,
              );
              final file = File('$out/auto-$name.png');
              await file.parent.create(recursive: true);
              await file.writeAsBytes(bytes!.buffer.asUint8List());
              await File('$out/auto-$name.json').writeAsString(
                jsonEncode({
                  'viewport': [size.width, size.height],
                  'textScale': scale,
                  'dark': dark,
                  'cardWidth': cards[0].width,
                  'cardHeight': cards[0].height,
                  'syntheticResources': true,
                  'physicalDeviceVerified': false,
                }),
              );
              image.dispose();
            });
          }
          expect(
            broker.calls.where((c) => c == 'connect' || c.startsWith('power:')),
            isEmpty,
          );
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pumpAndSettle();
        });
      }
    }
  }
}

// Dialog typography/layout only; no broker, native window or actual VM.
import '../helpers/stream_connect_consent.dart' show acceptStreamConnect;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/app_theme.dart';

import 'android_launcher_test.dart'
    show FakeBroker, MemoryStore, vm, mainButton;

const _vmName = '회계팀 공용 데스크톱 · 긴 VM 이름도 그대로 표시';

Widget _dialog(String kind) => switch (kind) {
  'server' => AndroidServerDialog(
    initial: null,
    save: (_) async {},
    probe: (_) async =>
        const BrokerProbeResult(version: 'fixture', elapsedMs: 5),
  ),
  'password' => AndroidPasswordDialog(
    api: FakeBroker([]),
    token: 'fixture-only',
  ),
  _ => AndroidStreamDialog(desktop: vm(name: _vmName)),
};

class _Broker extends FakeBroker {
  _Broker(super.inventory);
  bool unavailable = false;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    if (unavailable) throw const BrokerException('network_error');
    return super.desktops(token);
  }
}

void main() {
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      (_) async => null,
    );
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? true : null,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      null,
    );
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });
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

  for (final kind in ['empty', 'error', 'connect', 'exit']) {
    for (final brightness in Brightness.values) {
      testWidgets('home $kind 320 ${brightness.name} text 2.0', (tester) async {
        final originalShadows = debugDisableShadows;
        debugDisableShadows = false;
        try {
          tester.view.physicalSize = const Size(320, 780);
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.reset);
          tester.platformDispatcher.textScaleFactorTestValue = 2;
          addTearDown(tester.platformDispatcher.clearAllTestValues);
          MemoryStore(
            origin: 'https://vdi.example',
            username: 'tester',
          ).install();
          addTearDown(
            () => tester.binding.defaultBinaryMessenger
                .setMockMethodCallHandler(AndroidStore.channel, null),
          );
          final boundary = GlobalKey();
          final broker = _Broker(kind == 'connect' ? [vm(name: _vmName)] : []);
          await tester.pumpWidget(
            RepaintBoundary(
              key: boundary,
              child: MaterialApp(
                debugShowCheckedModeBanner: false,
                theme: buildAppTheme(brightness: brightness),
                home: AndroidHome(
                  store: AndroidStore(),
                  apiFactory: (_) => broker,
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          await tester.enterText(
            find.byKey(const Key('sign-in-password')),
            'fixture-only',
          );
          await tester.ensureVisible(find.byKey(const Key('login-submit')));
          await tester.tap(find.byKey(const Key('login-submit')));
          await tester.pumpAndSettle();
          if (kind == 'error') {
            broker.unavailable = true;
            await tester.tap(find.byKey(const Key('refresh-desktops')));
            await tester.pumpAndSettle();
            expect(find.byKey(const Key('retry-desktops')), findsOneWidget);
          } else if (kind == 'connect') {
            final primary = mainButton('pve-149');
            await tester.ensureVisible(primary);
            await tester.pumpAndSettle();
            // The full-height card puts its action below the initial viewport
            // at 200% text. Exercise actual scroll access before tapping.
            expect(primary.hitTestable(), findsOneWidget);
            final bounds = tester.getRect(primary);
            expect(bounds.top, greaterThanOrEqualTo(0));
            expect(bounds.bottom, lessThanOrEqualTo(780));
            await tester.tap(primary);
            await acceptStreamConnect(tester, 'pve-149');
            await tester.pumpAndSettle();
            expect(
              find.byKey(const Key('connect-confirm-dialog')),
              findsNothing,
            );
            expect(
              find.byKey(const Key('android-session-panel')),
              findsOneWidget,
            );
            expect(find.text(_vmName), findsOneWidget);
            expect(broker.calls.where((c) => c.startsWith('power:')), isEmpty);
          } else if (kind == 'exit') {
            await tester.tap(find.byKey(const Key('account-menu')));
            await tester.pumpAndSettle();
            await tester.tap(find.text('완전 종료'));
            await tester.pumpAndSettle();
            expect(
              find.byKey(const Key('confirm-android-exit')),
              findsOneWidget,
            );
          }
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox());
        } finally {
          debugDisableShadows = originalShadows;
        }
      });
    }
  }

  for (final size in [
    const Size(320, 780),
    const Size(360, 800),
    const Size(412, 915),
    const Size(840, 1120),
  ]) {
    for (final brightness in Brightness.values) {
      for (final scale in [1.0, 1.5, 2.0]) {
        for (final kind in ['server', 'password', 'quality']) {
          testWidgets(
            '$kind ${size.width.toInt()} ${brightness.name} text $scale',
            (tester) async {
              final originalShadows = debugDisableShadows;
              debugDisableShadows = false;
              try {
                tester.view.physicalSize = size;
                tester.view.devicePixelRatio = 1;
                addTearDown(tester.view.reset);
                final boundary = GlobalKey();
                await tester.pumpWidget(
                  RepaintBoundary(
                    key: boundary,
                    child: MaterialApp(
                      debugShowCheckedModeBanner: false,
                      theme: buildAppTheme(brightness: brightness),
                      builder: (context, child) => MediaQuery(
                        data: MediaQuery.of(context)
                            .copyWith(textScaler: TextScaler.linear(scale)),
                        child: child!,
                      ),
                      home: Scaffold(body: _dialog(kind)),
                    ),
                  ),
                );
                await tester.pump();
                final fault = tester.takeException();
                expect(fault, isNull);
                final bounds = tester.getRect(find.byType(AppDialog));
                expect(bounds.width, lessThanOrEqualTo(size.width));
                if (kind == 'quality') {
                  expect(
                    tester.widget<AppDialog>(find.byType(AppDialog)).subtitle,
                    _vmName,
                  );
                }
                await tester.pumpWidget(const SizedBox());
              } finally {
                debugDisableShadows = originalShadows;
              }
            },
          );
        }
      }
    }
  }
}

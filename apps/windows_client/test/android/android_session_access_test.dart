import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/product_updates.dart' show UpdateResponse;

import '../auth_flow_test.dart' show fixtureOrigin;
import '../pc_login_replacement_lifetime_test.dart' show AuthWatch;
import '../session_access_lifetime_test.dart' show ConfirmBroker, IdleBroker;
import '../session_renewal_test.dart' show VirtualClock;
import '../product_updates_test.dart' show Transport;
import 'android_updates_test.dart' show FakePlatform;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final calls = <String>[];
  String? savedPassword;
  Future<void> open(
    WidgetTester t,
    BrokerApi broker,
    VirtualClock clock, {
    AuthWatch? watch,
  }) async {
    calls.clear();
    savedPassword = 'secret-pass';
    t.view.physicalSize = const Size(1080, 2400);
    t.view.devicePixelRatio = 3;
    addTearDown(t.view.reset);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, (call) async {
          calls.add(call.method);
          switch (call.method) {
            case 'origin':
              return fixtureOrigin.toString();
            case 'login':
              return {'username': 'alice', 'password': savedPassword};
            case 'preferences':
              return {'launcher.view': 'cards'};
            case 'appVersion':
              return '1.0.6';
            case 'forgetPassword':
              savedPassword = null;
              return null;
            case 'saveLogin':
              final args = call.arguments as Map;
              savedPassword = args['remember'] == true
                  ? args['password'] as String?
                  : null;
              return null;
          }
          return null;
        });
    await t.pumpWidget(
      AndroidVdiApp(
        clock: clock.call,
        apiFactory: (_) => broker,
        desktopWatchFactory: watch == null ? null : (_, _) => watch,
        updatesFactory: (origin) => AndroidUpdates(
          origin,
          transport: Transport((_) {
            final bytes = utf8.encode('{"releases":[]}');
            return UpdateResponse(200, bytes.length, Stream.value(bytes));
          }),
        ),
        updatePlatform: FakePlatform(),
      ),
    );
    await t.pumpAndSettle();
  }

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  testWidgets(
    'Android manual conflict waits for confirmation and cancel keeps existing device',
    (t) async {
      final clock = VirtualClock(), broker = ConfirmBroker();
      await open(t, broker, clock);
      await t.ensureVisible(find.byKey(const Key('login-submit')));
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 350));
      expect(find.text('다른 곳에서 로그아웃하고 여기서 로그인할까요?'), findsOneWidget);
      expect(broker.confirmations, 0);
      expect(broker.liveOwner, 'other-device');
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(broker.liveOwner, 'other-device');
      expect(savedPassword, 'secret-pass');
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 350));
      await t.tap(find.text('로그아웃하고 로그인'));
      await t.pumpAndSettle();
      expect(broker.confirmations, 1);
      expect(broker.liveOwner, 'this-device');
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  for (final reason in ['pc_login_replaced', 'session_idle_timeout', 'session_reauthentication_required']) {
    testWidgets(
      'Android $reason retires foreground remote route and never recovers saved password',
      (t) async {
        final clock = VirtualClock(),
            broker = IdleBroker(clock),
            watch = AuthWatch();
        await open(t, broker, clock, watch: watch);
        await t.ensureVisible(find.byKey(const Key('login-submit')));
        await t.tap(find.byKey(const Key('login-submit')));
        await t.pumpAndSettle();
        final navigator = Navigator.of(t.element(find.byType(AndroidHome)));
        unawaited(
          navigator.push(
            MaterialPageRoute<void>(
              builder: (_) =>
                  const Scaffold(body: Text('android-active-video')),
            ),
          ),
        );
        await t.pumpAndSettle();
        watch.terminal.add(reason);
        await t.pumpAndSettle();
        expect(find.text('android-active-video'), findsNothing);
        expect(find.byKey(const Key('login-submit')), findsNothing);
        expect(find.byKey(const ValueKey('session-end-ack')), findsOneWidget);
        expect(broker.recoveries, 0);
        expect(savedPassword, 'secret-pass');
        expect(calls.where((value) => value == 'forgetPassword'), isEmpty);
        await t.tap(find.byKey(const ValueKey('session-end-ack')));
        await t.pumpAndSettle();
        expect(
          t
              .widget<TextField>(find.byKey(const Key('sign-in-password')))
              .controller!
              .text,
          'secret-pass',
          reason: '비밀번호 기억 is on: the saved password is filled, not sent',
        );
        expect(broker.manual, 1);
        await t.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  // No frame is drawn while the app is hidden, so closing the screens cannot
  // be confirmed within its 3 s: back in the app, 확인 on the sign-out
  // screen must not stay disabled for good ("연결이 모두 끊겼는지 확인하지
  // 못했어요. Multitap을 종료한 뒤 다시 여세요.").
  testWidgets(
    'Android automatic sign-out while the app is hidden: back in front the '
    'check is made again and 확인 works',
    (t) async {
      final clock = VirtualClock(), broker = IdleBroker(clock);
      await open(t, broker, clock);
      await t.ensureVisible(find.byKey(const Key('login-submit')));
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      final navigator = Navigator.of(t.element(find.byType(AndroidHome)));
      unawaited(
        navigator.push(
          MaterialPageRoute<void>(
            builder: (_) => const Scaffold(body: Text('android-active-video')),
          ),
        ),
      );
      await t.pumpAndSettle();
      expect(find.text('android-active-video'), findsOneWidget);
      // Home: the stream goes on behind, and nothing is drawn.
      for (final state in const [
        AppLifecycleState.inactive,
        AppLifecycleState.hidden,
        AppLifecycleState.paused,
      ]) {
        t.binding.handleAppLifecycleStateChanged(state);
      }
      addTearDown(
        () => t.binding.handleAppLifecycleStateChanged(
          AppLifecycleState.resumed,
        ),
      );
      // The 30 s without input pass, then the 3 s the screens have to close.
      await clock.advance(t, const Duration(seconds: 31));
      await t.pump(const Duration(seconds: 4));
      // Back in the app.
      for (final state in const [
        AppLifecycleState.hidden,
        AppLifecycleState.inactive,
        AppLifecycleState.resumed,
      ]) {
        t.binding.handleAppLifecycleStateChanged(state);
      }
      await t.pumpAndSettle();
      expect(find.text('android-active-video'), findsNothing);
      final ok = find.byKey(const ValueKey('session-end-ack'));
      expect(ok, findsOneWidget);
      expect(
        t.widget<FilledButton>(ok).onPressed,
        isNotNull,
        reason: 'the sign-out screen can be left',
      );
      expect(find.text('연결이 모두 끊겼는지 확인하지 못했어요. Multitap을 종료한 뒤 다시 여세요.'), findsNothing);
      await t.tap(ok);
      await t.pumpAndSettle();
      expect(find.byKey(const Key('login-submit')), findsOneWidget);
      expect(broker.recoveries, 0);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );
}

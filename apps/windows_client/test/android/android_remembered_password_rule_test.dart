import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/data/product_updates.dart' show UpdateResponse;

import '../auth_flow_test.dart' show fixtureOrigin;
import '../pc_login_replacement_lifetime_test.dart' show AuthWatch;
import '../product_updates_test.dart' show Transport;
import '../session_access_lifetime_test.dart' show IdleBroker;
import '../session_renewal_test.dart' show VirtualClock;
import 'android_updates_test.dart' show FakePlatform;

// The one rule of the Android sign-in screen (2026-11-11): with "비밀번호 기억"
// on, the saved password is filled in every time the screen appears; with it
// off, nothing is saved and nothing is filled. There is no second option.

String _field(WidgetTester t, String key) =>
    t.widget<TextField>(find.byKey(Key(key))).controller!.text;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  late List<String> calls;
  late List<AuthWatch> watches;
  String? savedPassword;
  setUp(() {
    calls = [];
    watches = [];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, (call) async {
          calls.add(call.method);
          switch (call.method) {
            case 'origin':
              return fixtureOrigin.toString();
            case 'login':
              return {'username': 'Alice', 'password': savedPassword};
            case 'preferences':
              return <String, String>{};
            case 'appVersion':
              return '1.0.7';
            case 'saveLogin':
              final args = call.arguments as Map;
              savedPassword = args['remember'] == true
                  ? args['password'] as String?
                  : null;
              return null;
            case 'forgetPassword':
              savedPassword = null;
              return null;
          }
          return null;
        });
  });
  tearDown(
    () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null),
  );

  Future<void> open(WidgetTester t, IdleBroker broker, VirtualClock clock) async {
    t.view.physicalSize = const Size(1080, 2400);
    t.view.devicePixelRatio = 3;
    addTearDown(t.view.reset);
    await t.pumpWidget(
      AndroidVdiApp(
        clock: clock.call,
        apiFactory: (_) => broker,
        desktopWatchFactory: (_, _) {
          final watch = AuthWatch();
          watches.add(watch);
          return watch;
        },
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

  Future<void> signIn(WidgetTester t, {String? typed}) async {
    if (typed != null) {
      await t.enterText(find.byKey(const Key('sign-in-password')), typed);
    }
    await t.ensureVisible(find.byKey(const Key('login-submit')));
    await t.tap(find.byKey(const Key('login-submit')));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('account-menu')), findsOneWidget);
  }

  for (final remember in [true, false]) {
    testWidgets(
      'Android 비밀번호 기억 ${remember ? 'on fills the saved password' : 'off leaves the password empty'} '
      'after sign-out, automatic sign-out and restart',
      (t) async {
        savedPassword = remember ? 'secret-pass' : null;
        final expected = remember ? 'secret-pass' : '';
        final typed = remember ? null : 'typed-pass';
        final clock = VirtualClock(), broker = IdleBroker(clock);
        await open(t, broker, clock);
        expect(
          find.byKey(const Key('retype-password-after-logout')),
          findsNothing,
        );
        expect(find.text('로그아웃 후 비밀번호 다시 입력'), findsNothing);
        expect(
          t.widget<Switch>(find.byKey(const Key('remember-password'))).value,
          remember,
        );
        expect(_field(t, 'sign-in-password'), expected);

        await signIn(t, typed: typed);
        await t.tap(find.byKey(const Key('account-menu')));
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('sign-out')));
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('confirm-sign-out')));
        await t.pumpAndSettle();
        expect(_field(t, 'sign-in-id'), 'Alice');
        expect(_field(t, 'sign-in-password'), expected, reason: 'sign-out');

        await signIn(t, typed: typed);
        watches.last.terminal.add('session_idle_timeout');
        await t.pumpAndSettle();
        await t.tap(find.byKey(const ValueKey('session-end-ack')));
        await t.pumpAndSettle();
        expect(_field(t, 'sign-in-id'), 'Alice');
        expect(_field(t, 'sign-in-password'), expected, reason: 'idle');

        expect(broker.manual, 2, reason: 'a filled password never signs in');
        expect(broker.recoveries, 0);
        expect(savedPassword, remember ? 'secret-pass' : isNull);
        expect(calls, isNot(contains('forgetPassword')));

        await t.pumpWidget(const SizedBox.shrink());
        await t.pumpAndSettle();
        final restarted = IdleBroker(clock);
        await open(t, restarted, clock);
        expect(_field(t, 'sign-in-id'), 'Alice');
        expect(_field(t, 'sign-in-password'), expected, reason: 'restart');
        expect(restarted.manual, 0);
        expect(restarted.recoveries, 0);
        await t.pumpWidget(const SizedBox.shrink());
      },
    );
  }
}

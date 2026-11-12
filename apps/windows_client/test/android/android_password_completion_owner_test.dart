import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';

class _Clock {
  DateTime now = DateTime.utc(2026, 1, 1);
  DateTime call() => now;
  Future<void> advance(WidgetTester tester, Duration total) async {
    for (var left = total; left > Duration.zero;) {
      final step = left < const Duration(seconds: 15)
          ? left
          : const Duration(seconds: 15);
      now = now.add(step);
      await tester.pump(step);
      left -= step;
    }
  }
}

class _Broker implements BrokerApi, SessionRenewalBrokerApi {
  _Broker(this.clock);
  final _Clock clock;
  bool renewSupported = true, rejectLogin = false;
  final logins = <String>[], records = <String>[], tokens = <String>[];
  final expired = <String>{};
  int renewals = 0;
  Completer<void>? loginWait;

  @override
  Future<BrokerSession> login(String username, String password) async {
    logins.add('$username:$password');
    if (loginWait != null) await loginWait!.future;
    if (rejectLogin) throw const BrokerException('invalid_credentials', 401);
    return BrokerSession(
      token: 'token-${logins.length}',
      expiresAt: clock.now.add(const Duration(minutes: 10)),
      username: username,
    );
  }

  @override
  Future<DateTime> renewSession(String token) async {
    if (!renewSupported) throw const BrokerException('not_found', 404);
    renewals++;
    return clock.now.add(const Duration(minutes: 10));
  }

  @override
  void recordSessionRenewal(
    String token, {
    required int stage,
    required bool ok,
    String? code,
  }) => records.add('$stage:${ok ? 'ok' : code}');

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    tokens.add(token);
    if (expired.remove(token)) {
      throw const BrokerException('session_expired', 401);
    }
    return const [];
  }

  @override
  Future<void> logout(String token) async {}
  @override
  void close() {}
  @override
  dynamic noSuchMethod(Invocation invocation) =>
      throw UnimplementedError(invocation.memberName.toString());
}

class _PasswordBroker extends _Broker {
  _PasswordBroker(super.clock);
  Completer<void>? passwordWait;
  int passwordChanges = 0;
  @override
  Future<void> changePassword(
    String token,
    String current,
    String password,
    String confirmation,
  ) async {
    passwordChanges++;
    await passwordWait?.future;
  }
}

Future<void> _tick(WidgetTester t) async {
  for (var i = 0; i < 20; i++) {
    await t.pump(const Duration(milliseconds: 20));
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final mode in [
    'current',
    'disposed',
    'successor-login',
    'same-token-renewal',
    'cancel',
    'store-failure',
  ]) {
    testWidgets('ST20 actual Android password completion keeps owner: $mode', (
      t,
    ) async {
      final gate = Completer<void>(), entered = Completer<void>();
      var forgets = 0;
      var savedUsername = 'tester';
      String? savedPassword = 'secret-pass';
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(AndroidStore.channel, (call) async {
        switch (call.method) {
          case 'origin':
            return 'https://vdi.example';
          case 'login':
            return {'username': savedUsername, 'password': savedPassword};
          case 'appVersion':
            return '1.0.5';
          case 'preferences':
            return <String, String>{};
          case 'saveLogin':
            final args = call.arguments as Map;
            savedUsername = args['username'] as String;
            savedPassword = args['remember'] == true
                ? args['password'] as String?
                : null;
            return null;
          case 'forgetPassword':
            // Production native handler commits deletion synchronously, then
            // acknowledges. Hold only that acknowledgement, never the delete.
            savedPassword = null;
            forgets++;
            if (forgets == 1) {
              entered.complete();
              if (mode == 'store-failure') {
                throw PlatformException(code: 'storage_unavailable');
              }
              await gate.future;
            }
            return null;
        }
        return null;
      });
      addTearDown(
        () => messenger.setMockMethodCallHandler(AndroidStore.channel, null),
      );
      t.view.physicalSize = const Size(1080, 2400);
      t.view.devicePixelRatio = 3;
      addTearDown(t.view.reset);
      final clock = _Clock();
      final api = _PasswordBroker(clock);
      if (mode == 'same-token-renewal') api.passwordWait = Completer<void>();
      await t.pumpWidget(
        AndroidVdiApp(
          apiFactory: (_) => api,
          clock: clock.call,
          sessionRetryDelay: const Duration(seconds: 1),
        ),
      );
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('open-app-settings')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('account-change-password')));
      await t.pumpAndSettle();
      await t.enterText(
        find
            .descendant(
              of: find.byType(AndroidPasswordDialog),
              matching: find.byType(TextField),
            )
            .at(0),
        'secret-pass',
      );
      await t.enterText(
        find
            .descendant(
              of: find.byType(AndroidPasswordDialog),
              matching: find.byType(TextField),
            )
            .at(1),
        'new-fixture-password',
      );
      await t.enterText(
        find
            .descendant(
              of: find.byType(AndroidPasswordDialog),
              matching: find.byType(TextField),
            )
            .at(2),
        'new-fixture-password',
      );
      if (mode == 'cancel') {
        await t.tap(find.widgetWithText(TextButton, '취소'));
        await _tick(t);
        expect(api.passwordChanges, 0);
        expect(forgets, 0);
        expect(savedPassword, 'secret-pass');
        expect(find.byKey(const Key('account-menu')), findsOneWidget);
        await t.pumpWidget(const SizedBox.shrink());
        await _tick(t);
        return;
      }
      await t.tap(find.widgetWithText(FilledButton, '변경'));
      await _tick(t);
      if (mode == 'same-token-renewal') {
        // Renewal while the actual password POST is still pending keeps owner.
        await clock.advance(t, const Duration(minutes: 5, seconds: 1));
        expect(api.renewals, greaterThanOrEqualTo(1));
        expect(api.logins, hasLength(1));
        api.passwordWait!.complete();
        await _tick(t);
      }
      expect(entered.isCompleted, true);
      if (mode == 'disposed') {
        await t.pumpWidget(const SizedBox.shrink());
      }
      if (mode == 'successor-login') {
        api.renewSupported = false;
        api.expired.add('token-1');
        await t.tap(find.byKey(const Key('refresh-desktops')));
        await _tick(t);
          await t.tap(find.byKey(const ValueKey('session-end-ack')));
          await _tick(t);
        expect(find.byKey(const Key('login-submit')), findsOneWidget);
        await t.enterText(find.byKey(const Key('sign-in-id')), 'bobby');
        await t.enterText(
          find.byKey(const Key('sign-in-password')),
          'successor-fixture-password',
        );
        if (!t.widget<Switch>(find.byKey(const Key('remember-password'))).value) {
          await t.tap(find.byKey(const Key('remember-password')));
        }
        await t.tap(find.byKey(const Key('login-submit')));
        await _tick(t);
        expect(find.byKey(const Key('account-menu')), findsOneWidget);
        expect(savedPassword, 'successor-fixture-password');
      }
      gate.complete();
      await _tick(t);
      expect(t.takeException(), isNull);
      if (['current', 'same-token-renewal', 'store-failure'].contains(mode)) {
        expect(find.byKey(const Key('login-submit')), findsOneWidget);
      }
      if (mode == 'successor-login') {
        expect(
          find.byKey(const Key('account-menu')),
          findsOneWidget,
          reason: 'A successful successor login must survive predecessor password cleanup',
        );
      }
      if (mode == 'successor-login') {
        expect(
          savedPassword,
          'successor-fixture-password',
          reason: 'Native deletion happened before the successor save; late ACK must not erase its record',
        );
      }
      await t.pumpWidget(const SizedBox.shrink());
      await _tick(t);
    });
  }
}

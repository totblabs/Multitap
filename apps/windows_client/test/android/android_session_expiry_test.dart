import 'dart:async';

import 'package:flutter/material.dart';
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

/// A store that remembers the password the user opted into.
void _installStore({bool remember = true}) {
  TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
      .setMockMethodCallHandler(AndroidStore.channel, (call) async {
        switch (call.method) {
          case 'origin':
            return 'https://vdi.example';
          case 'login':
            return {
              'username': 'tester',
              'password': remember ? 'secret-pass' : null,
            };
          case 'appVersion':
            return '0.1.0-preview.4';
          case 'preferences':
            return <String, String>{};
        }
        return null;
      });
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(
    () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null),
  );

  Future<void> start(WidgetTester tester, _Broker broker, _Clock clock) async {
    tester.view.physicalSize = const Size(1080, 2400);
    tester.view.devicePixelRatio = 3;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      AndroidVdiApp(
        apiFactory: (_) => broker,
        sessionRetryDelay: const Duration(seconds: 1),
        clock: clock.call,
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
  }

  bool onSignIn(WidgetTester tester) =>
      find.byKey(const Key('login-submit')).evaluate().isNotEmpty;
  String text(WidgetTester tester, String key) =>
      tester.widget<TextField>(find.byKey(Key(key))).controller!.text;

  testWidgets('the app start fills ID and remembered password', (tester) async {
    _installStore();
    final clock = _Clock();
    await start(tester, _Broker(clock), clock);
    expect(onSignIn(tester), isFalse);
  });

  for (final replaceAccount in [false, true]) {
    testWidgets(
      'a delayed automatic-login rejection ${replaceAccount ? 'preserves a replacement account secret' : 'forgets its unchanged old secret'}',
      (tester) async {
        _installStore();
        final clock = _Clock(), broker = _DeferredRejection(clock);
        await start(tester, broker, clock);
        var savedId = 'tester', savedPassword = 'secret-pass', forgets = 0;
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(AndroidStore.channel, (call) async {
              final args = (call.arguments as Map?) ?? const {};
              if (call.method == 'login') {
                return {
                  'username': savedId,
                  'password': savedPassword.isEmpty ? null : savedPassword,
                };
              }
              if (call.method == 'preferences') return <String, String>{};
              if (call.method == 'forgetPassword') {
                forgets++;
                savedPassword = '';
              }
              if (call.method == 'saveLogin') {
                savedId = args['username'] as String;
                savedPassword = args['password'] as String? ?? '';
              }
              return null;
            });
        broker.waitForOld = true;
        // Unsupported renewal, while the old lifetime is still valid, retains
        // the legacy recovery seam. Definitive 401 no longer starts recovery.
        await clock.advance(tester, const Duration(minutes: 5, seconds: 1));
        expect(
          broker.logins,
          hasLength(2),
          reason: 'old automatic login is awaiting rejection',
        );
        if (replaceAccount) {
          await tester.tap(find.byKey(const Key('account-menu')));
          await tester.pumpAndSettle();
          await tester.tap(find.byKey(const Key('sign-out')));
          await tester.pumpAndSettle();
          await tester.tap(find.byKey(const Key('confirm-sign-out')));
          await tester.pumpAndSettle();
          await tester.enterText(
            find.byKey(const Key('sign-in-id')),
            'other-user',
          );
          await tester.enterText(
            find.byKey(const Key('sign-in-password')),
            'other-secret',
          );
          await tester.tap(find.byKey(const Key('login-submit')));
          await tester.pumpAndSettle();
          expect(savedId, 'other-user');
          expect(savedPassword, 'other-secret');
        }
        final beforeRejection = forgets;
        broker.rejection.complete();
        await tester.pumpAndSettle();
        if (replaceAccount) {
          expect(forgets, beforeRejection);
          expect(savedPassword, 'other-secret');
          expect(onSignIn(tester), isFalse);
        } else {
          expect(forgets, beforeRejection + 1);
          expect(savedPassword, isEmpty);
          await tester.tap(find.byKey(const ValueKey('session-end-ack')));
          await tester.pumpAndSettle();
          expect(onSignIn(tester), isTrue);
        }
      },
    );
  }

  testWidgets('a late remembered-login read cannot replace new edits or '
      'the credentials of an in-flight login', (tester) async {
    _installStore();
    final clock = _Clock(), broker = _Broker(clock);
    await start(tester, broker, clock);
    final restored = Completer<Map<String, String>>();
    final writes = <Map<Object?, Object?>>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, (call) async {
          if (call.method == 'login') return restored.future;
          if (call.method == 'preferences') return <String, String>{};
          if (call.method == 'saveLogin') {
            writes.add(Map.of(call.arguments as Map));
          }
          return null;
        });
    await tester.tap(find.byKey(const Key('account-menu')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('sign-out')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('confirm-sign-out')));
    await tester.pumpAndSettle();
    expect(onSignIn(tester), isTrue);
    await tester.enterText(find.byKey(const Key('sign-in-id')), 'new-user');
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'new-secret',
    );
    broker.loginWait = Completer<void>();
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pump();
    restored.complete({'username': 'old-user', 'password': 'old-secret'});
    // Login is intentionally still pending, so its progress indicator cannot
    // settle. Process the late platform reply without waiting for completion.
    await tester.pump(const Duration(milliseconds: 50));
    expect(text(tester, 'sign-in-id'), 'new-user');
    expect(text(tester, 'sign-in-password'), 'new-secret');
    expect(broker.logins.last, 'new-user:new-secret');
    broker.loginWait!.complete();
    await tester.pumpAndSettle();
    expect(onSignIn(tester), isFalse);
    expect(writes, hasLength(1));
    expect(writes.single['origin'], 'https://vdi.example');
    expect(writes.single['username'], 'new-user');
    expect(writes.single['password'], 'new-secret');
    expect(writes.single['remember'], isTrue);
    expect(writes.single.keys, isNot(contains('token')));
  });

  testWidgets('renews the same token before expiry, no new sign-in', (
    tester,
  ) async {
    _installStore();
    final clock = _Clock(), broker = _Broker(clock);
    await start(tester, broker, clock);
    await clock.advance(tester, const Duration(minutes: 5, seconds: 1));
    expect(broker.renewals, 1);
    await clock.advance(tester, const Duration(minutes: 8));
    expect(onSignIn(tester), isFalse);
    expect(broker.logins, hasLength(1));
    expect(broker.records, contains('1:ok'));
  });

  testWidgets('a definitive 401 retires without saved-password recovery', (tester) async {
    _installStore();
    final clock = _Clock(), broker = _Broker(clock);
    await start(tester, broker, clock);
    broker.expired.add('token-1');
    await clock.advance(tester, const Duration(seconds: 20));
    await tester.pump(const Duration(seconds: 16));
    expect(onSignIn(tester), isFalse);
    expect(broker.logins, hasLength(1));
    expect(find.byKey(const ValueKey('session-end-ack')), findsOneWidget);
    expect(broker.renewals, 0);
  });

  testWidgets('without renewal the remembered password signs in again', (
    tester,
  ) async {
    _installStore();
    final clock = _Clock(), broker = _Broker(clock)..renewSupported = false;
    await start(tester, broker, clock);
    await clock.advance(tester, const Duration(minutes: 5, seconds: 1));
    await tester.pumpAndSettle();
    expect(onSignIn(tester), isFalse);
    expect(broker.logins, ['tester:secret-pass', 'tester:secret-pass']);
    expect(broker.records, containsAll(['1:request_failed', '2:ok']));
  });

  testWidgets(
    'no password to sign in with: the sign-in screen appears with the ID',
    (tester) async {
      _installStore();
      final clock = _Clock(), broker = _Broker(clock)..renewSupported = false;
      await start(tester, broker, clock);
      // The password was cleared from the field after sign-in; the broker is
      // down, so neither renewal nor silent sign-in works.
      broker.rejectLogin = false;
      broker.expired.add('token-1');
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(AndroidStore.channel, (call) async {
            if (call.method == 'login') {
              // Silent sign-in cannot read it now (e.g. store busy)...
              return {'username': 'tester', 'password': null};
            }
            return null;
          });
      await clock.advance(tester, const Duration(seconds: 20));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('session-end-ack')));
      await tester.pumpAndSettle();
      expect(onSignIn(tester), isTrue);
      expect(text(tester, 'sign-in-id'), 'tester');
    },
  );

  testWidgets(
    'sign-in after definitive expiry is manual, with the remembered password filled in',
    (tester) async {
      _installStore();
      final clock = _Clock(), broker = _FailingLogin(clock);
      await start(tester, broker, clock);
      expect(onSignIn(tester), isFalse);
      // The 401 arrives and the broker cannot be reached for longer than the
      // silent sign-in keeps trying.
      broker.down = true;
      broker.expired.add('token-1');
      await clock.advance(tester, const Duration(seconds: 15));
      for (var i = 0; i < 12; i++) {
        await clock.advance(tester, const Duration(seconds: 1));
      }
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('session-end-ack')));
      await tester.pumpAndSettle();
      expect(onSignIn(tester), isTrue);
      expect(text(tester, 'sign-in-id'), 'tester');
      expect(text(tester, 'sign-in-password'), 'secret-pass');
    },
  );
}

/// Every sign-in fails as unreachable once [down].
class _FailingLogin extends _Broker {
  _FailingLogin(super.clock) {
    renewSupported = false;
  }
  bool down = false;
  @override
  Future<BrokerSession> login(String username, String password) async {
    if (down) throw const BrokerException('network_error');
    return super.login(username, password);
  }
}

class _DeferredRejection extends _Broker {
  _DeferredRejection(super.clock) {
    renewSupported = false;
  }
  bool waitForOld = false;
  final rejection = Completer<void>();
  @override
  Future<BrokerSession> login(String username, String password) async {
    if (waitForOld && username == 'tester') {
      logins.add('$username:$password');
      await rejection.future;
      throw const BrokerException('invalid_credentials', 401);
    }
    return super.login(username, password);
  }
}

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/login_memory.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/mfa_login_dialog.dart';

import 'auth_flow_test.dart' show TestBroker, TestLoginMemory;

final origin = Uri.parse('https://mfa.fixture.invalid');
MfaChallenge challenge() => MfaChallenge.fromJson({
  'protocol': 'mfa-v1',
  'challengeToken': 'b' * 43,
  'expiresAt': DateTime.now()
      .toUtc()
      .add(const Duration(seconds: 120))
      .toIso8601String(),
  'methods': ['totp', 'recovery'],
}, origin: origin);
BrokerSession finalSession() => BrokerSession(
  token: 'verified-only',
  expiresAt: DateTime.now().add(const Duration(hours: 1)),
  username: 'alice',
);

class MfaFixtureBroker extends TestBroker implements MfaBrokerApi {
  bool off = false;
  BrokerException? proofError;
  Completer<BrokerSession>? held;
  MfaChallenge? pending;
  final proofs = <(String?, String?)>[];
  final inventoryTokens = <String>[];
  @override
  Future<BrokerSession> login(String username, String password) async {
    logins++;
    if (off) return finalSession();
    pending = challenge();
    throw MfaRequiredException(pending!);
  }

  @override
  Future<BrokerSession> verifyMfa(
    MfaChallenge challenge, {
    String? code,
    String? recoveryCode,
  }) async {
    proofs.add((code, recoveryCode));
    if (proofError != null) throw proofError!;
    return held == null ? finalSession() : held!.future;
  }

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    inventoryTokens.add(token);
    return [];
  }
}

class SilentMfaBroker extends MfaFixtureBroker
    implements SessionRenewalBrokerApi {
  @override
  Future<BrokerSession> login(String username, String password) async {
    if (logins == 0) {
      logins++;
      return finalSession();
    }
    return super.login(username, password);
  }

  @override
  Future<DateTime> renewSession(String token) async =>
      throw const BrokerException('not_found', 404);
  @override
  void recordSessionRenewal(
    String token, {
    required int stage,
    required bool ok,
    String? code,
  }) {}
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  Future<void> waitFor(WidgetTester tester, Finder finder) async {
    for (var i = 0; i < 100; i++) {
      await tester.pump(const Duration(milliseconds: 20));
      if (finder.evaluate().isNotEmpty) {
        final route = ModalRoute.of(finder.evaluate().first);
        if (route?.animation?.status != AnimationStatus.forward) return;
      }
    }
    expect(finder, findsOneWidget);
  }

  Future<void> windows(
    WidgetTester tester,
    MfaFixtureBroker broker,
    TestLoginMemory memory,
  ) async {
    tester.view.physicalSize = const Size(1366, 768);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MultitapApp(api: broker, serverOrigin: origin, loginMemory: memory),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('login-submit')));
    if (broker.off) {
      await tester.pumpAndSettle();
    } else {
      await waitFor(tester, find.byKey(const Key('mfa-title')));
    }
  }

  TestLoginMemory remembered() => TestLoginMemory()
    ..entries[origin.origin] = const RememberedLogin(
      username: 'alice',
      password: 'fixture-password',
    );

  testWidgets(
    'Windows202 never lists desktops or saves password; finalOTP200 alone signs in',
    (tester) async {
      final broker = MfaFixtureBroker(), memory = remembered();
      await windows(tester, broker, memory);
      expect(find.byKey(const Key('mfa-title')), findsOneWidget);
      expect(broker.inventoryTokens, isEmpty);
      expect(memory.passwordSaves, 0);
      await tester.enterText(find.byKey(const Key('mfa-code')), '123456');
      await tester.tap(find.byKey(const Key('mfa-submit')));
      await tester.pumpAndSettle();
      expect(broker.proofs, [('123456', null)]);
      expect(broker.inventoryTokens, ['verified-only']);
      expect(memory.passwordSaves, 1);
      expect(broker.pending!.token, isEmpty);
      expect(find.byKey(const Key('mfa-title')), findsNothing);
    },
  );
  testWidgets(
    'cancel clears pending challenge and preserves opted-in password; late successfulOTP is logged out',
    (tester) async {
      final broker = MfaFixtureBroker()..held = Completer<BrokerSession>(),
          memory = remembered();
      await windows(tester, broker, memory);
      await tester.enterText(find.byKey(const Key('mfa-code')), '123456');
      await tester.tap(find.byKey(const Key('mfa-submit')));
      await tester.pump();
      await tester.tap(find.byKey(const Key('mfa-cancel')));
      await tester.pumpAndSettle();
      expect(broker.pending!.token, isEmpty);
      expect(memory.entries[origin.origin]!.password, 'fixture-password');
      expect(memory.passwordSaves, 0);
      broker.held!.complete(finalSession());
      await tester.pumpAndSettle();
      expect(broker.logouts, 1);
      expect(broker.inventoryTokens, isEmpty);
      expect(find.byKey(const Key('sign-in-id')), findsOneWidget);
    },
  );
  testWidgets(
    'invalidOTP clears code, recovery sends exactly one proof and can finish',
    (tester) async {
      final broker = MfaFixtureBroker()
        ..proofError = const BrokerException('mfa_invalid_code', 400);
      await windows(tester, broker, remembered());
      await tester.enterText(find.byKey(const Key('mfa-code')), '123456');
      await tester.tap(find.byKey(const Key('mfa-submit')));
      await waitFor(tester, find.byKey(const Key('mfa-error')));
      expect(find.text('인증 코드를 확인하고 다시 입력하세요.'), findsOneWidget);
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('mfa-code')))
            .controller!
            .text,
        isEmpty,
      );
      // Review 2026-11-12: the field lost the keyboard focus while the code
      // was checked and did not get it back.
      await tester.pump();
      await tester.pump();
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('mfa-code')))
            .focusNode!
            .hasFocus,
        isTrue,
      );
      expect(broker.inventoryTokens, isEmpty);
      broker.proofError = null;
      await tester.tap(find.byKey(const Key('mfa-recovery-mode')));
      await tester.pump();
      await tester.enterText(find.byKey(const Key('mfa-code')), 'A' * 32);
      await tester.tap(find.byKey(const Key('mfa-submit')));
      await tester.pumpAndSettle();
      expect(broker.proofs.last, (null, 'a' * 32));
      expect(broker.inventoryTokens, ['verified-only']);
    },
  );
  testWidgets(
    'local120s deadline clears challenge and code without manufacturing a login',
    (tester) async {
      final broker = MfaFixtureBroker();
      await windows(tester, broker, remembered());
      await tester.enterText(find.byKey(const Key('mfa-code')), '123456');
      await tester.pump(const Duration(seconds: 121));
      await tester.pump();
      expect(find.text('인증 시간이 끝났어요.\n로그인부터 다시 시작하세요.'), findsOneWidget);
      expect(broker.pending!.token, isEmpty);
      expect(find.byKey(const Key('mfa-code')), findsNothing);
      expect(broker.proofs, isEmpty);
      await tester.tap(find.byKey(const Key('mfa-cancel')));
      await tester.pumpAndSettle();
      expect(broker.inventoryTokens, isEmpty);
    },
  );
  testWidgets('MFA-OFF remains the existing Windows sign-in flow', (
    tester,
  ) async {
    final broker = MfaFixtureBroker()..off = true, memory = remembered();
    await windows(tester, broker, memory);
    expect(find.byKey(const Key('mfa-title')), findsNothing);
    expect(broker.inventoryTokens, ['verified-only']);
    expect(memory.passwordSaves, 1);
  });
  testWidgets(
    'Windows silent MFA request stops without prompting or deleting valid remembered password',
    (tester) async {
      final broker = SilentMfaBroker(), memory = remembered();
      await tester.pumpWidget(
        MultitapApp(
          api: broker,
          serverOrigin: origin,
          loginMemory: memory,
          sessionRenewalLead: const Duration(hours: 2),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('login-submit')));
      await tester.pumpAndSettle();
      expect(broker.logins, 2);
      expect(broker.pending!.token, isEmpty);
      expect(broker.proofs, isEmpty);
      expect(find.byKey(const Key('mfa-title')), findsNothing);
      expect(memory.entries[origin.origin]!.password, 'fixture-password');
      await tester.pumpWidget(const SizedBox());
      await tester.pumpAndSettle();
    },
  );
  testWidgets(
    'system back cancels pending MFA and preserves remembered password',
    (tester) async {
      final broker = MfaFixtureBroker(), memory = remembered();
      await windows(tester, broker, memory);
      await tester.enterText(find.byKey(const Key('mfa-code')), '123456');
      await tester.binding.handlePopRoute();
      await tester.pumpAndSettle();
      expect(broker.pending!.token, isEmpty);
      expect(broker.proofs, isEmpty);
      expect(memory.entries[origin.origin]!.password, 'fixture-password');
    },
  );
  for (final (status, code) in [
    (410, 'mfa_challenge_expired'),
    (429, 'too_many_attempts'),
  ]) {
    testWidgets('MFA $status is shown honestly without authenticating', (
      tester,
    ) async {
      final broker = MfaFixtureBroker()
        ..proofError = BrokerException(code, status);
      await windows(tester, broker, remembered());
      await tester.enterText(find.byKey(const Key('mfa-code')), '123456');
      await tester.tap(find.byKey(const Key('mfa-submit')));
      await waitFor(tester, find.byKey(const Key('mfa-error')));
      expect(broker.inventoryTokens, isEmpty);
      expect(find.text(BrokerException(code, status).message), findsOneWidget);
      if (status == 410) expect(broker.pending!.token, isEmpty);
      await tester.tap(find.byKey(const Key('mfa-cancel')));
      await tester.pumpAndSettle();
    });
  }
  for (final recovery in [false, true]) {
    testWidgets(
      'Android202 waits for ${recovery ? 'recovery' : 'OTP'} before inventory and credential storage',
      (tester) async {
        final methods = <String>[];
        final messenger =
            TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
        messenger.setMockMethodCallHandler(AndroidStore.channel, (call) async {
          methods.add(call.method);
          return switch (call.method) {
            'origin' => origin.toString(),
            'login' => {'username': 'alice', 'password': 'fixture-password'},
            'preferences' => <String, String>{},
            'appVersion' => '0.1.0-preview.45',
            _ => null,
          };
        });
        addTearDown(
          () => messenger.setMockMethodCallHandler(AndroidStore.channel, null),
        );
        tester.view.physicalSize = const Size(1080, 2400);
        tester.view.devicePixelRatio = 3;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final broker = MfaFixtureBroker();
        await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('login-submit')));
        await waitFor(tester, find.byKey(const Key('mfa-title')));
        expect(find.byKey(const Key('mfa-title')), findsOneWidget);
        expect(broker.inventoryTokens, isEmpty);
        expect(methods.where((m) => m == 'saveLogin'), isEmpty);
        if (recovery) {
          await tester.tap(find.byKey(const Key('mfa-recovery-mode')));
          await tester.pump();
        }
        await tester.enterText(
          find.byKey(const Key('mfa-code')),
          recovery ? 'A' * 32 : '123456',
        );
        await tester.tap(find.byKey(const Key('mfa-submit')));
        await tester.pumpAndSettle();
        expect(broker.inventoryTokens, ['verified-only']);
        expect(methods.where((m) => m == 'saveLogin'), hasLength(1));
        expect(
          broker.proofs.single,
          recovery ? (null, 'a' * 32) : ('123456', null),
        );
        await tester.pumpWidget(const SizedBox());
        await tester.pumpAndSettle();
      },
    );
  }
  testWidgets(
    'Android silent OTP requirement keeps remembered password and waits for interactive sign-in',
    (tester) async {
      final methods = <String>[];
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(AndroidStore.channel, (call) async {
        methods.add(call.method);
        return switch (call.method) {
          'origin' => origin.toString(),
          'login' => {'username': 'alice', 'password': 'fixture-password'},
          'preferences' => <String, String>{},
          'appVersion' => 'fixture',
          _ => null,
        };
      });
      addTearDown(
        () => messenger.setMockMethodCallHandler(AndroidStore.channel, null),
      );
      final broker = SilentMfaBroker();
      await tester.pumpWidget(
        AndroidVdiApp(
          apiFactory: (_) => broker,
          sessionRenewalLead: const Duration(hours: 2),
        ),
      );
      await tester.pumpAndSettle();
      await tester.ensureVisible(find.byKey(const Key('login-submit')));
      await tester.tap(find.byKey(const Key('login-submit')));
      await tester.pumpAndSettle();
      expect(broker.logins, 2);
      expect(broker.pending!.token, isEmpty);
      expect(methods.contains('forgetPassword'), isFalse);
      expect(find.byKey(const Key('mfa-title')), findsNothing);
      expect(broker.proofs, isEmpty);
      await tester.pumpWidget(const SizedBox());
      await tester.pumpAndSettle();
    },
  );
  testWidgets(
    'pending authentication is retired when its account/server owner changes',
    (tester) async {
      var current = true;
      final pending = challenge(), broker = MfaFixtureBroker();
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: Builder(
            builder: (context) => Scaffold(
              body: TextButton(
                onPressed: () => unawaited(
                  showMfaLogin(
                    context,
                    api: broker,
                    challenge: pending,
                    current: () => current,
                  ),
                ),
                child: const Text('open'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('open'));
      await tester.pumpAndSettle();
      current = false;
      await tester.pump(const Duration(seconds: 1));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('mfa-title')), findsNothing);
      expect(pending.token, isEmpty);
      expect(broker.proofs, isEmpty);
    },
  );
  for (final brightness in [Brightness.dark, Brightness.light]) {
    testWidgets(
      'MFA dialog stays usable360x640 200percent ${brightness.name}',
      (tester) async {
        tester.view.physicalSize = const Size(360, 640);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final broker = MfaFixtureBroker(), pending = challenge();
        await tester.pumpWidget(
          MaterialApp(
            theme: buildAppTheme(brightness: brightness),
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(context)
                  .copyWith(textScaler: TextScaler.linear(2)),
              child: child!,
            ),
            home: Builder(
              builder: (context) => Scaffold(
                body: TextButton(
                  onPressed: () => unawaited(
                    showMfaLogin(
                      context,
                      api: broker,
                      challenge: pending,
                      current: () => true,
                    ),
                  ),
                  child: const Text('open'),
                ),
              ),
            ),
          ),
        );
        await tester.tap(find.text('open'));
        await tester.pumpAndSettle();
        expect(tester.takeException(), isNull);
        expect(
          tester.getRect(find.byKey(const Key('mfa-cancel'))).bottom,
          lessThanOrEqualTo(640),
        );
        expect(
          tester.getRect(find.byKey(const Key('mfa-submit'))).bottom,
          lessThanOrEqualTo(640),
        );
        await tester.tap(find.byKey(const Key('mfa-cancel')));
        await tester.pumpAndSettle();
        expect(pending.token, isEmpty);
      },
    );
  }
}

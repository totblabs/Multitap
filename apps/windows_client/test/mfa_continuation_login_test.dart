// Private real App/Android/shared-dialog fixture. Broker/native persistence
// substitutes are synthetic; this does not qualify a real server or device.
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

import 'account_login_context_parse_test.dart' show loginChallengeFixture;
import 'auth_flow_test.dart' show TestLoginMemory;
import 'mfa_login_test.dart' show MfaFixtureBroker, finalSession, origin;

MfaChallenge _mfa() => MfaChallenge.fromJson({
  'protocol': 'mfa-v1', 'challengeToken': 'b' * 43,
  'expiresAt': DateTime.now().add(const Duration(seconds: 120)).toUtc().toIso8601String(),
  'methods': ['totp', 'recovery'],
}, origin: origin, pcLoginIntent: PcLoginIntent.confirmable);

class _Broker extends MfaFixtureBroker implements LoginConfirmationBrokerApi {
  int confirmations = 0;
  bool oldLoginActive = true;
  LoginConfirmation? offered;
  Completer<BrokerSession>? heldConfirmation, heldProof;
  LoginConfirmationRequired continuation() {
    offered = LoginConfirmation.parse(loginChallengeFixture(), origin, DateTime.now());
    return LoginConfirmationRequired(offered!);
  }
  @override
  Future<BrokerSession> login(String username, String password) async {
    logins++; pending = _mfa(); throw MfaRequiredException(pending!);
  }
  @override
  Future<BrokerSession> verifyMfa(MfaChallenge challenge, {String? code, String? recoveryCode}) async {
    proofs.add((code, recoveryCode));
    if (heldProof != null) return heldProof!.future;
    throw continuation();
  }
  @override
  Future<BrokerSession> confirmLogin(LoginConfirmation challenge) async {
    challenge.consume(origin, DateTime.now());
    confirmations++;
    final session = heldConfirmation == null ? finalSession() : await heldConfirmation!.future;
    oldLoginActive = false;
    return session;
  }
}
Future<void> _wait(WidgetTester tester, Finder finder) async {
  for (var i = 0; i < 100; i++) {
    await tester.pump(const Duration(milliseconds: 20));
    if (finder.evaluate().isNotEmpty && ModalRoute.of(finder.evaluate().first)?.animation?.status != AnimationStatus.forward) return;
  }
  expect(finder, findsOneWidget);
}
class _Fixture {
  final broker = _Broker(), memory = TestLoginMemory();
  final methods = <String>[];
  Future<void> mount(WidgetTester tester, bool android) async {
    tester.view.physicalSize = android ? const Size(1080, 2400) : const Size(1366, 768);
    tester.view.devicePixelRatio = android ? 3 : 1;
    addTearDown(tester.view.reset);
    if (android) {
      final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(AndroidStore.channel, (call) async {
        methods.add(call.method);
        return switch (call.method) {
          'origin' => origin.toString(),
          'login' => {'username': 'alice', 'password': 'synthetic-password'},
          'preferences' => <String, String>{},
          'appVersion' => 'synthetic',
          _ => null,
        };
      });
      addTearDown(() => messenger.setMockMethodCallHandler(AndroidStore.channel, null));
      await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
    } else {
      memory.entries[origin.origin] = const RememberedLogin(username: 'alice', password: 'synthetic-password');
      await tester.pumpWidget(MultitapApp(api: broker, serverOrigin: origin, loginMemory: memory));
    }
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('login-submit')));
    await _wait(tester, find.byKey(const Key('mfa-title')));
  }
  Future<void> verify(WidgetTester tester) async {
    await tester.enterText(find.byKey(const Key('mfa-code')), '123456');
    await tester.tap(find.byKey(const Key('mfa-submit')));
    await _wait(tester, find.byKey(const Key('login-confirmation')));
  }
  Future<void> remove(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox.shrink()); await tester.pumpAndSettle();
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final android in [false, true]) {
    for (final cancel in [true, false]) {
      testWidgets('${android ? 'Android' : 'Windows'} MFA success still requires separate explicit replacement cancel=$cancel', (tester) async {
        final fixture = _Fixture();
        fixture.broker.heldConfirmation = Completer<BrokerSession>();
        try {
          await fixture.mount(tester, android);
          await fixture.verify(tester);
          expect(find.byKey(const Key('mfa-title')), findsNothing);
          expect(fixture.broker.pending!.token, isEmpty);
          expect(fixture.broker.confirmations, 0);
          expect(fixture.broker.oldLoginActive, isTrue);
          expect(fixture.broker.inventoryTokens, isEmpty);
          expect(fixture.memory.passwordSaves, 0);
          expect(fixture.methods.where((method) => method == 'saveLogin'), isEmpty);
          await tester.tap(find.byKey(Key(cancel ? 'cancel-account-login' : 'confirm-account-login')));
          // A held confirmation intentionally keeps the actual sign-in busy.
          // Advance the route animation without waiting for that spinner.
          await tester.pump(); await tester.pump(const Duration(milliseconds: 350));
          if (cancel) {
            expect(fixture.broker.confirmations, 0);
            expect(fixture.broker.oldLoginActive, isTrue);
            expect(fixture.broker.offered!.forgotten, isTrue);
            expect(fixture.broker.inventoryTokens, isEmpty);
            if (!android) {
              expect(fixture.memory.entries[origin.origin]!.password, 'synthetic-password');
              expect(fixture.memory.passwordSaves, 0);
            }
            expect(fixture.methods.where((method) => method == 'forgetPassword' || method == 'saveLogin'), isEmpty);
          } else {
            expect(fixture.broker.confirmations, 1);
            expect(fixture.broker.oldLoginActive, isTrue);
            expect(fixture.broker.inventoryTokens, isEmpty);
            fixture.broker.heldConfirmation!.complete(finalSession());
            await tester.pumpAndSettle();
            expect(fixture.broker.oldLoginActive, isFalse);
            expect(fixture.broker.inventoryTokens, ['verified-only']);
            expect(android ? fixture.methods.where((method) => method == 'saveLogin').length : fixture.memory.passwordSaves, 1);
          }
          expect(fixture.broker.proofs, [('123456', null)]);
          expect(fixture.broker.logins, 1, reason: 'the second question never replays password login');
        } finally { await fixture.remove(tester); }
      });
    }
  }
  testWidgets('late typed MFA continuation after Cancel is discarded without a second prompt', (tester) async {
    final fixture = _Fixture(); fixture.broker.heldProof = Completer<BrokerSession>();
    try {
      await fixture.mount(tester, false);
      await tester.enterText(find.byKey(const Key('mfa-code')), '123456');
      await tester.tap(find.byKey(const Key('mfa-submit'))); await tester.pump();
      await tester.tap(find.byKey(const Key('mfa-cancel'))); await tester.pumpAndSettle();
      final late = fixture.broker.continuation(); fixture.broker.heldProof!.completeError(late);
      await tester.pumpAndSettle();
      expect(fixture.broker.offered!.forgotten, isTrue);
      expect(find.byKey(const Key('login-confirmation')), findsNothing);
      expect(fixture.broker.confirmations, 0); expect(fixture.broker.inventoryTokens, isEmpty);
    } finally { await fixture.remove(tester); }
  });
  testWidgets('current-owner retirement while replacement is displayed cannot spend its proof', (tester) async {
    final broker = _Broker(); var current = true; BrokerSession? result;
    try {
      await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: Builder(builder: (context) => TextButton(
        key: const Key('begin-mfa'),
        onPressed: () async { result = await showMfaLogin(context, api: broker, challenge: _mfa(), current: () => current); },
        child: const Text('MFA'),
      ))));
      await tester.tap(find.byKey(const Key('begin-mfa'))); await _wait(tester, find.byKey(const Key('mfa-title')));
      await tester.enterText(find.byKey(const Key('mfa-code')), '123456'); await tester.tap(find.byKey(const Key('mfa-submit')));
      await _wait(tester, find.byKey(const Key('login-confirmation')));
      current = false;
      await tester.tap(find.byKey(const Key('confirm-account-login'))); await tester.pumpAndSettle();
      expect(result, isNull); expect(broker.confirmations, 0); expect(broker.offered!.forgotten, isTrue);
    } finally { await tester.pumpWidget(const SizedBox.shrink()); await tester.pumpAndSettle(); }
  });
  testWidgets('late confirmed session after owner retirement is logged out exactly once', (tester) async {
    final broker = _Broker()..heldConfirmation = Completer<BrokerSession>();
    var current = true; BrokerSession? result;
    try {
      await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: Builder(builder: (context) => TextButton(
        key: const Key('begin-mfa'),
        onPressed: () async { result = await showMfaLogin(context, api: broker, challenge: _mfa(), current: () => current); },
        child: const Text('MFA'),
      ))));
      await tester.tap(find.byKey(const Key('begin-mfa'))); await _wait(tester, find.byKey(const Key('mfa-title')));
      await tester.enterText(find.byKey(const Key('mfa-code')), '123456'); await tester.tap(find.byKey(const Key('mfa-submit')));
      await _wait(tester, find.byKey(const Key('login-confirmation')));
      await tester.tap(find.byKey(const Key('confirm-account-login'))); await tester.pump();
      expect(broker.confirmations, 1); expect(result, isNull); expect(broker.logouts, 0);
      current = false; broker.heldConfirmation!.complete(finalSession());
      await tester.pumpAndSettle();
      expect(result, isNull); expect(broker.logouts, 1); expect(broker.inventoryTokens, isEmpty);
    } finally { await tester.pumpWidget(const SizedBox.shrink()); await tester.pumpAndSettle(); }
  });
}

// Public App/Android + actual HttpBrokerApi proof transport fixture. No provider
// authority is implemented here. Current final-session/consent fakes match the
// already qualified original11 pattern, with proof HTTP replies injected.
import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/login_memory.dart';
import '../account_login_context_parse_test.dart' show loginChallengeFixture;
import '../auth_flow_test.dart' show TestLoginMemory;
import '../mfa_login_test.dart' show MfaFixtureBroker, finalSession, origin;
import '../pc_login_policy_api_test.dart' show AckClient;

const recoveryRaw = '0123456789abcdef0123456789abcdef';
const recoveryGrouped = '0123-4567-89AB-CDEF-0123-4567-89AB-CDEF';
const consumedNotice = '복구 코드는 사용됐어요. 취소해도 다시 쓸 수 없어요.';
MfaChallenge followupChallenge() => MfaChallenge.fromJson({
  'protocol': 'mfa-v1', 'challengeToken': 'b' * 43,
  'expiresAt': DateTime.now().add(const Duration(seconds: 120)).toUtc().toIso8601String(),
  'methods': ['totp', 'recovery'],
}, origin: origin, pcLoginIntent: PcLoginIntent.confirmable);

class FollowupBroker extends MfaFixtureBroker implements LoginConfirmationBrokerApi {
  FollowupBroker({bool malformed = false}) {
    client.queue.add((409, {
      'error': {'code': 'login_confirmation_required', 'details': {
        ...loginChallengeFixture(), if (malformed) 'unexpected': true,
      }},
    }, null));
    proofApi = HttpBrokerApi(origin: origin, client: client, requestPcLoginReplacement: true);
  }
  final client = AckClient();
  late final HttpBrokerApi proofApi;
  int confirmations = 0;
  bool oldLoginActive = true;
  LoginConfirmation? offered;
  Completer<BrokerSession>? heldConfirmation;
  Completer<void>? holdProofDelivery;
  final logoutTokens = <String>[];

  @override
  Future<BrokerSession> login(String username, String password) async {
    logins++; pending = followupChallenge(); throw MfaRequiredException(pending!);
  }
  @override
  Future<BrokerSession> verifyMfa(MfaChallenge challenge, {String? code, String? recoveryCode}) async {
    proofs.add((code, recoveryCode));
    try {
      final value = await proofApi.verifyMfa(challenge, code: code, recoveryCode: recoveryCode);
      if (holdProofDelivery != null) await holdProofDelivery!.future;
      return value;
    } catch (error, stack) {
      if (error is LoginConfirmationRequired) offered = error.challenge;
      if (holdProofDelivery != null) await holdProofDelivery!.future;
      Error.throwWithStackTrace(error, stack);
    }
  }
  @override
  Future<BrokerSession> confirmLogin(LoginConfirmation challenge) async {
    challenge.consume(origin, DateTime.now());
    confirmations++;
    final session = heldConfirmation == null ? finalSession() : await heldConfirmation!.future;
    oldLoginActive = false;
    return session;
  }
  @override
  Future<void> logout(String token) async { logoutTokens.add(token); await super.logout(token); }
  @override
  void close() { proofApi.close(); super.close(); }
}

Future<void> followupWait(WidgetTester tester, Finder finder) async {
  for (var i = 0; i < 100; i++) {
    // Allow the injected HTTP response Future its real event turn.
    await tester.runAsync(() => Future<void>.delayed(Duration.zero));
    await tester.pump(const Duration(milliseconds: 20));
    if (finder.evaluate().isNotEmpty &&
        ModalRoute.of(finder.evaluate().first)?.animation?.status != AnimationStatus.forward) { return; }
  }
  expect(finder, findsOneWidget);
}
Future<void> submitFollowup(WidgetTester tester, {bool recovery = true}) async {
  if (recovery) {
    await tester.tap(find.byKey(const Key('mfa-recovery-mode')));
    await tester.pump();
  }
  await tester.enterText(find.byKey(const Key('mfa-code')), recovery ? recoveryGrouped : '123456');
  await tester.tap(find.byKey(const Key('mfa-submit')));
}
class FollowupApp {
  FollowupApp(this.broker);
  final FollowupBroker broker;
  final memory = TestLoginMemory();
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
          'appVersion' => 'synthetic', _ => null,
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
    await followupWait(tester, find.byKey(const Key('mfa-title')));
  }
  Future<void> remove(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox.shrink());
    if (broker.holdProofDelivery != null && !broker.holdProofDelivery!.isCompleted) broker.holdProofDelivery!.complete();
    if (broker.heldConfirmation != null && !broker.heldConfirmation!.isCompleted) broker.heldConfirmation!.complete(finalSession());
    await tester.pumpAndSettle();
    broker.close();
  }
}

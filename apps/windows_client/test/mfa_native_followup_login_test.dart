// PRIVATE UNCOMPILED source. Actual shared dialog + Windows/Android app roots.
import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/mfa_login_dialog.dart';
import 'helpers/mfa_native_followup_fixture.dart';
import 'mfa_login_test.dart' show finalSession;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final android in [false, true]) {
    for (final cancel in [true, false]) {
      testWidgets('${android ? 'Android' : 'Windows'} recovery proof explains consumed code before consent cancel=$cancel', (tester) async {
        final broker = FollowupBroker()..heldConfirmation = Completer<BrokerSession>();
        final app = FollowupApp(broker);
        try {
          await app.mount(tester, android);
          await submitFollowup(tester);
          await followupWait(tester, find.byKey(const Key('login-confirmation')));
          expect(find.byKey(const Key('mfa-title')), findsNothing);
          expect(find.textContaining(consumedNotice), findsOneWidget);
          expect(find.textContaining('데스크톱과 실행 중인 작업은 그대로 유지돼요.'), findsOneWidget);
          expect(find.textContaining(recoveryRaw), findsNothing);
          expect(find.textContaining(recoveryGrouped), findsNothing);
          expect(broker.pending!.token, isEmpty);
          expect(broker.proofs, [(null, recoveryRaw)]);
          expect(broker.client.requests, hasLength(1));
          expect(broker.confirmations, 0);
          expect(broker.oldLoginActive, isTrue);
          expect(broker.inventoryTokens, isEmpty);
          expect(app.memory.passwordSaves, 0);
          expect(app.methods.where((method) => method == 'saveLogin'), isEmpty);
          await tester.tap(find.byKey(Key(cancel ? 'cancel-account-login' : 'confirm-account-login')));
          await tester.pump(); await tester.pump(const Duration(milliseconds: 350));
          if (cancel) {
            expect(broker.confirmations, 0);
            expect(broker.oldLoginActive, isTrue);
            expect(broker.offered!.forgotten, isTrue);
            expect(broker.inventoryTokens, isEmpty);
            expect(app.memory.passwordSaves, 0);
          } else {
            expect(broker.confirmations, 1);
            expect(broker.oldLoginActive, isTrue);
            expect(broker.inventoryTokens, isEmpty);
            broker.heldConfirmation!.complete(finalSession());
            await tester.pumpAndSettle();
            expect(broker.oldLoginActive, isFalse);
            expect(broker.inventoryTokens, ['verified-only']);
            expect(android ? app.methods.where((method) => method == 'saveLogin').length : app.memory.passwordSaves, 1);
          }
          expect(broker.logins, 1);
          expect(broker.proofs, hasLength(1), reason: 'Cancel does not refund or replay factor proof.');
          expect(broker.client.requests, hasLength(1));
          expect(tester.takeException(), isNull);
        } finally { await app.remove(tester); }
      });
    }
    testWidgets('${android ? 'Android' : 'Windows'} TOTP consent has no recovery-consumption notice', (tester) async {
      final broker = FollowupBroker();
      final currentApp = FollowupApp(broker);
      try {
        await currentApp.mount(tester, android);
        await submitFollowup(tester, recovery: false);
        await followupWait(tester, find.byKey(const Key('login-confirmation')));
        expect(find.textContaining(consumedNotice), findsNothing);
        expect(find.textContaining('데스크톱과 실행 중인 작업은 그대로 유지돼요.'), findsOneWidget);
        expect(broker.proofs, [('123456', null)]);
        await tester.tap(find.byKey(const Key('cancel-account-login')));
        await tester.pumpAndSettle();
        expect(broker.confirmations, 0);
        expect(broker.oldLoginActive, isTrue);
      } finally { await currentApp.remove(tester); }
    });
    testWidgets('${android ? 'Android' : 'Windows'} malformed definite MFA consent requires fresh login and sends no retry', (tester) async {
      final broker = FollowupBroker(malformed: true);
      final currentApp = FollowupApp(broker);
      try {
        await currentApp.mount(tester, android);
        await submitFollowup(tester, recovery: false);
        await followupWait(tester, find.byKey(const Key('mfa-fresh-login-required')));
        expect(broker.pending!.token, isEmpty);
        expect(broker.pending!.remaining, Duration.zero);
        expect(find.byKey(const Key('mfa-submit')), findsNothing);
        expect(find.byKey(const Key('mfa-code')), findsNothing);
        expect(find.byKey(const Key('login-confirmation')), findsNothing);
        expect(find.textContaining('로그인부터 다시 시작하세요.'), findsOneWidget);
        expect(broker.client.requests, hasLength(1));
        expect(broker.confirmations, 0);
        expect(broker.inventoryTokens, isEmpty);
        await tester.tap(find.byKey(const Key('mfa-cancel')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('login-submit')), findsOneWidget);
        expect(broker.logins, 1);
        expect(broker.proofs, hasLength(1));
        expect(broker.client.requests, hasLength(1));
        expect(currentApp.memory.passwordSaves, 0);
        expect(currentApp.methods.where((method) => method == 'saveLogin'), isEmpty);
      } finally { await currentApp.remove(tester); }
    });
  }
  for (final lateSession in [false, true]) {
    testWidgets('recovery consent cannot adopt a retired owner lateSession=$lateSession', (tester) async {
      final broker = FollowupBroker(); var current = true; BrokerSession? result;
      if (lateSession) broker.heldConfirmation = Completer<BrokerSession>();
      try {
        await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: Builder(builder: (context) => TextButton(
          key: const Key('begin-followup'),
          onPressed: () async { result = await showMfaLogin(context, api: broker, challenge: followupChallenge(), current: () => current); },
          child: const Text('MFA'),
        ))));
        await tester.tap(find.byKey(const Key('begin-followup')));
        await followupWait(tester, find.byKey(const Key('mfa-title')));
        await submitFollowup(tester);
        await followupWait(tester, find.byKey(const Key('login-confirmation')));
        expect(find.textContaining(consumedNotice), findsOneWidget);
        if (!lateSession) current = false;
        await tester.tap(find.byKey(const Key('confirm-account-login')));
        await tester.pump(); await tester.pump(const Duration(milliseconds: 350));
        if (lateSession) {
          expect(broker.confirmations, 1);
          current = false;
          broker.heldConfirmation!.complete(finalSession());
          await tester.pumpAndSettle();
          expect(broker.logoutTokens, ['verified-only']);
        } else {
          expect(broker.confirmations, 0);
          expect(broker.offered!.forgotten, isTrue);
          expect(broker.logoutTokens, isEmpty);
        }
        expect(result, isNull);
        expect(broker.inventoryTokens, isEmpty);
        expect(broker.proofs, [(null, recoveryRaw)]);
      } finally { await tester.pumpWidget(const SizedBox.shrink()); await tester.pumpAndSettle(); broker.close(); }
    });
  }
  testWidgets('late malformed MFA response cannot paint fresh-login state on retired owner', (tester) async {
    final broker = FollowupBroker(malformed: true)..holdProofDelivery = Completer<void>();
    var current = true; BrokerSession? result;
    try {
      await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: Builder(builder: (context) => TextButton(
        key: const Key('begin-followup'),
        onPressed: () async { result = await showMfaLogin(context, api: broker, challenge: followupChallenge(), current: () => current); },
        child: const Text('MFA'),
      ))));
      await tester.tap(find.byKey(const Key('begin-followup')));
      await followupWait(tester, find.byKey(const Key('mfa-title')));
      await submitFollowup(tester, recovery: false);
      await tester.pump();
      expect(broker.client.requests, hasLength(1));
      current = false; broker.holdProofDelivery!.complete();
      await tester.pump();
      expect(find.byKey(const Key('mfa-fresh-login-required')), findsNothing);
      await tester.pump(const Duration(seconds: 1)); await tester.pumpAndSettle();
      expect(find.byKey(const Key('mfa-title')), findsNothing);
      expect(find.byKey(const Key('login-confirmation')), findsNothing);
      expect(result, isNull);
      expect(broker.client.requests, hasLength(1));
      expect(broker.confirmations, 0);
      expect(broker.inventoryTokens, isEmpty);
    } finally {
      await tester.pumpWidget(const SizedBox.shrink());
      if (!broker.holdProofDelivery!.isCompleted) broker.holdProofDelivery!.complete();
      await tester.pumpAndSettle(); broker.close();
    }
  });
}

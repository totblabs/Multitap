import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/mfa_account_controller.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/mfa_account_dialog.dart';

import 'helpers/mfa_account_fake.dart';

MfaEnrollment _shortEnrollment() {
  const secret = 'JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP';
  return MfaEnrollment.parse({
    'protocol': 'mfa-v1', 'setupToken': 'a' * 43,
    'expiresAt': DateTime.now().add(const Duration(seconds: 2)).toUtc().toIso8601String(),
    'secret': secret,
    'uri': 'otpauth://totp/Multitap:synthetic?secret=$secret&issuer=Multitap&algorithm=SHA1&digits=6&period=30',
  });
}

Future<void> _settle(WidgetTester tester, Future<void> original) async {
  var done = false;
  Object? failure;
  StackTrace? stack;
  original.then((_) { done = true; }, onError: (Object error, StackTrace trace) {
    failure = error; stack = trace; done = true;
  });
  for (var turn = 0; turn < 50 && !done; turn++) {
    await tester.runAsync(() => Future<void>.delayed(Duration.zero));
    await tester.pump(const Duration(milliseconds: 1));
  }
  expect(done, isTrue, reason: 'The original operation must actually finish.');
  if (failure != null) { Error.throwWithStackTrace(failure!, stack!); }
}

Future<void> _draft(MfaTestOwner f) async {
  f.api.setup = () async => _shortEnrollment();
  await f.enrollment();
}

Future<void> _cleanup(WidgetTester tester, MfaTestOwner f,
    Completer<MfaAccountChange> reply, Future<void> work) async {
  if (!reply.isCompleted) { reply.complete(mfaChange()); }
  await _settle(tester, work);
  f.dispose();
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pump();
}

void main() {
  testWidgets('revealed key expires during the same held enable without closing or replay', (tester) async {
    final f = MfaTestOwner();
    f.api.setup = () async => _shortEnrollment();
    final reply = Completer<MfaAccountChange>();
    f.api.change = (_) => reply.future;
    await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: Scaffold(
      body: MfaAccountDialog(controller: f.controller, onClose: () {}))));
    await tester.pump();
    f.controller.choose(MfaAccountAction.enable);
    await f.controller.submitProof('synthetic-password');
    await tester.pump();
    final candidate = f.controller.enrollment!;
    await tester.ensureVisible(find.byKey(const Key('mfa-account-reveal')));
    await tester.tap(find.byKey(const Key('mfa-account-reveal')));
    await tester.pump();
    expect(find.byKey(const Key('mfa-account-secret')), findsOneWidget);
    final work = f.controller.enable('123456');
    try {
      expect(f.inbox.pendingFor(f.owner), isTrue);
      await tester.pump(const Duration(seconds: 3));
      expect(candidate.secret, isEmpty);
      expect(candidate.token, isEmpty);
      expect(candidate.uri, isNull);
      expect(f.controller.enrollment, isNull);
      expect(find.byKey(const Key('mfa-account-secret')), findsNothing);
      expect(find.byKey(const Key('mfa-account-pending-result')), findsOneWidget);
      expect(find.byKey(const Key('mfa-account-password')), findsNothing);
      expect(find.byKey(const Key('mfa-account-submit')), findsNothing);
      expect(tester.widget<TextButton>(find.byKey(const Key('mfa-account-close'))).onPressed, isNull);
      expect(f.controller.closeByUser(), isFalse);
      expect(f.controller.mutating, isTrue);
      expect(f.inbox.pendingFor(f.owner), isTrue);
      expect(f.api.calls, ['status', 'setup', 'enable']);
      expect(tester.takeException(), isNull);
    } finally { await _cleanup(tester, f, reply, work); }
  });

  testWidgets('definite rejection after expiry forgets the candidate and needs explicit new proof', (tester) async {
    final f = MfaTestOwner(); await _draft(f);
    final candidate = f.controller.enrollment!;
    final reply = Completer<MfaAccountChange>(); f.api.change = (_) => reply.future;
    final work = f.controller.enable('123456');
    try {
      await tester.pump(const Duration(seconds: 3));
      reply.completeError(const BrokerException('synthetic expired enrollment', 400));
      await _settle(tester, work);
      expect(f.controller.phase, MfaAccountPhase.proof);
      expect(f.controller.enrollment, isNull);
      expect(candidate.secret, isEmpty); expect(candidate.token, isEmpty); expect(candidate.uri, isNull);
      expect(f.inbox.pendingFor(f.owner), isFalse); expect(f.inbox.completed, isNull);
      await f.controller.enable('654321');
      expect(f.api.calls, ['status', 'setup', 'enable']);
      await f.controller.submitProof('synthetic-password');
      expect(f.api.calls, ['status', 'setup', 'enable', 'setup']);
      expect(f.controller.enrollment, isNot(same(candidate)));
    } finally { await _cleanup(tester, f, reply, work); }
  });

  testWidgets('same pending ticket accepts a real success after local candidate expiry', (tester) async {
    final f = MfaTestOwner(); await _draft(f);
    final candidate = f.controller.enrollment!;
    final reply = Completer<MfaAccountChange>(); f.api.change = (_) => reply.future;
    final work = f.controller.enable('123456');
    try {
      await tester.pump(const Duration(seconds: 3));
      expect(f.inbox.pendingFor(f.owner), isTrue);
      expect(f.controller.enrollment, isNull); expect(candidate.secret, isEmpty);
      f.authenticated = false; f.controller.authenticationRetired();
      reply.complete(mfaChange()); await _settle(tester, work);
      expect(f.inbox.completed!.owner, same(f.owner));
      expect(f.inbox.completed!.codes, mfaCodes);
      expect(f.controller.phase, MfaAccountPhase.completed);
      expect(f.api.calls, ['status', 'setup', 'enable']);
    } finally { await _cleanup(tester, f, reply, work); }
  });

  testWidgets('actual presentation retirement still discards a late success after expiry', (tester) async {
    final f = MfaTestOwner(); await _draft(f);
    final candidate = f.controller.enrollment!;
    final reply = Completer<MfaAccountChange>(); f.api.change = (_) => reply.future;
    final work = f.controller.enable('123456');
    try {
      await tester.pump(const Duration(seconds: 3));
      expect(candidate.secret, isEmpty);
      f.presentation = false; f.inbox.retire(f.owner);
      f.authenticated = false; f.controller.authenticationRetired();
      final result = mfaChange(); reply.complete(result); await _settle(tester, work);
      expect(f.inbox.completed, isNull); expect(result.takeCodes(), isEmpty);
      expect(f.controller.phase, MfaAccountPhase.closed);
      expect(f.api.calls, ['status', 'setup', 'enable']);
    } finally { await _cleanup(tester, f, reply, work); }
  });

  testWidgets('pre-expiry definite invalid code retains the live candidate for another number', (tester) async {
    final f = MfaTestOwner(); await _draft(f);
    final candidate = f.controller.enrollment!;
    final reply = Completer<MfaAccountChange>(); f.api.change = (_) => reply.future;
    final work = f.controller.enable('123456');
    try {
      reply.completeError(const BrokerException('synthetic invalid number', 400));
      await _settle(tester, work);
      expect(f.controller.enrollment, same(candidate));
      expect(candidate.secret, isNotEmpty); expect(candidate.token, isNotEmpty);
      expect(f.controller.phase, MfaAccountPhase.enrollment);
      expect(f.inbox.pendingFor(f.owner), isFalse); expect(f.inbox.completed, isNull);
      expect(f.api.calls, ['status', 'setup', 'enable']);
    } finally { await _cleanup(tester, f, reply, work); }
  });

  testWidgets('expiry without a mutation forgets the draft without sending any request', (tester) async {
    final f = MfaTestOwner();
    try {
      await _draft(f); final candidate = f.controller.enrollment!;
      await tester.pump(const Duration(seconds: 3));
      expect(f.controller.phase, MfaAccountPhase.proof);
      expect(f.controller.enrollment, isNull);
      expect(candidate.secret, isEmpty); expect(candidate.token, isEmpty); expect(candidate.uri, isNull);
      expect(f.controller.mutating, isFalse); expect(f.inbox.pendingFor(f.owner), isFalse);
      expect(f.api.calls, ['status', 'setup']);
    } finally { f.dispose(); await tester.pumpWidget(const SizedBox.shrink()); await tester.pump(); }
  });
}

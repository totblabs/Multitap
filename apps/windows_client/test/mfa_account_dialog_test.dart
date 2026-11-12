import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/mfa_account.dart';
import 'package:multitap_client/data/mfa_account_controller.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/mfa_account_dialog.dart';

import 'helpers/mfa_account_fake.dart';

Future<void> _mount(WidgetTester tester, Widget child, {double scale = 1}) => tester.pumpWidget(MaterialApp(
  theme: buildAppTheme(), home: MediaQuery(data: MediaQueryData(size: const Size(800, 600), textScaler: TextScaler.linear(scale)),
    child: Scaffold(body: child))));
Future<void> _remove(WidgetTester tester) async { await tester.pumpWidget(const SizedBox.shrink()); await tester.pump(); }

void main() {
  testWidgets('B setup hides local key until explicit reveal and clears submitted password', (tester) async {
    final f = MfaTestOwner(); addTearDown(f.dispose);
    await _mount(tester, MfaAccountDialog(controller: f.controller, onClose: () {})); await tester.pumpAndSettle();
    expect(find.byKey(const Key('mfa-account-enable')), findsOneWidget);
    await tester.tap(find.byKey(const Key('mfa-account-enable'))); await tester.pump();
    await tester.enterText(find.byKey(const Key('mfa-account-password')), 'synthetic-password');
    await tester.tap(find.byKey(const Key('mfa-account-submit'))); await tester.pumpAndSettle();
    expect(f.controller.phase, MfaAccountPhase.enrollment);
    expect(find.text('2/3 · 인증 앱 연결'), findsOneWidget);
    expect(find.byKey(const Key('mfa-account-secret')), findsNothing);
    await tester.ensureVisible(find.byKey(const Key('mfa-account-reveal')));
    await tester.tap(find.byKey(const Key('mfa-account-reveal'))); await tester.pump();
    expect(find.byKey(const Key('mfa-account-secret')), findsOneWidget);
    expect(f.api.calls, ['status', 'setup']);
    expect(tester.takeException(), isNull);
    await tester.tap(find.byKey(const Key('mfa-account-close'))); await tester.pump();
    expect(f.controller.phase, MfaAccountPhase.closed);
    await _remove(tester);
  });
  testWidgets('exact in-flight mutation disables close, then completed root view survives auth retirement', (tester) async {
    final f = MfaTestOwner(); addTearDown(f.dispose); var closes = 0;
    await _mount(tester, MfaAccountDialog(controller: f.controller, onClose: () => closes++)); await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('mfa-account-enable'))); await tester.pump();
    await tester.enterText(find.byKey(const Key('mfa-account-password')), 'synthetic-password');
    await tester.tap(find.byKey(const Key('mfa-account-submit'))); await tester.pumpAndSettle();
    final reply = Completer<MfaAccountChange>(); f.api.change = (_) => reply.future;
    await tester.enterText(find.byKey(const Key('mfa-account-factor')), '123456');
    await tester.tap(find.byKey(const Key('mfa-account-submit'))); await tester.pump();
    expect(tester.widget<TextButton>(find.byKey(const Key('mfa-account-close'))).onPressed, isNull);
    expect(closes, 0); f.authenticated = false; f.controller.authenticationRetired();
    reply.complete(mfaChange()); await tester.pumpAndSettle();
    await _mount(tester, MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () => closes++)); await tester.pumpAndSettle();
    expect(find.byKey(const Key('mfa-completed-codes')), findsOneWidget);
    // Setting it up counts 1/3, 2/3 and this 3/3.
    expect(find.text('3/3 · 복구 코드 보관'), findsOneWidget);
    expect(tester.widget<FilledButton>(find.byKey(const Key('mfa-completed-done'))).onPressed, isNull);
    await tester.ensureVisible(find.byKey(const Key('mfa-completed-saved')));
    await tester.tap(find.byKey(const Key('mfa-completed-saved'))); await tester.pump();
    await tester.tap(find.byKey(const Key('mfa-completed-done'))); await tester.pump();
    expect(f.inbox.completed, isNull); expect(closes, 1); expect(f.api.calls, ['status', 'setup', 'enable']);
    await _remove(tester);
  });
  testWidgets('discard requires explicit second confirmation and never sends any API request', (tester) async {
    final f = MfaTestOwner(); addTearDown(f.dispose); var finished = false;
    f.inbox.accept(f.inbox.reserve(f.owner), mfaChange());
    await _mount(tester, MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () => finished = true));
    await tester.tap(find.byKey(const Key('mfa-completed-discard'))); await tester.pumpAndSettle();
    expect(f.inbox.completed, isNotNull); expect(finished, isFalse);
    await tester.tap(find.text('돌아가기')); await tester.pumpAndSettle();
    expect(f.inbox.completed, isNotNull);
    await tester.tap(find.byKey(const Key('mfa-completed-discard'))); await tester.pumpAndSettle();
    await tester.tap(find.text('보관하지 않고 닫기').last); await tester.pumpAndSettle();
    expect(f.inbox.completed, isNull); expect(finished, isTrue); expect(f.api.calls, isEmpty);
    await _remove(tester);
  });
  testWidgets('presentation-owner retirement removes acknowledged codes immediately', (tester) async {
    final f = MfaTestOwner(); addTearDown(f.dispose);
    f.inbox.accept(f.inbox.reserve(f.owner), mfaChange());
    await _mount(tester, MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () {}));
    expect(find.byKey(const Key('mfa-completed-codes')), findsOneWidget);
    f.presentation = false; f.inbox.retire(f.owner); await tester.pump();
    expect(find.byKey(const Key('mfa-completed-codes')), findsNothing); expect(f.inbox.completed, isNull);
    await _remove(tester);
  });
  testWidgets('320px200percent keeps scrollable codes above separate48dp actions', (tester) async {
    final f = MfaTestOwner(); addTearDown(f.dispose);
    tester.view.physicalSize = const Size(320, 740); tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize); addTearDown(tester.view.resetDevicePixelRatio);
    f.inbox.accept(f.inbox.reserve(f.owner), mfaChange());
    await tester.pumpWidget(MaterialApp(theme: buildAppTheme(), home: MediaQuery(
      data: const MediaQueryData(size: Size(320, 740), textScaler: TextScaler.linear(2)),
      child: Scaffold(body: MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () {})))));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('mfa-completed-done')).hitTestable(), findsOneWidget);
    expect(tester.getSize(find.byKey(const Key('mfa-completed-done'))).height, greaterThanOrEqualTo(48));
    expect(find.byType(SingleChildScrollView), findsWidgets); expect(tester.takeException(), isNull);
    await _remove(tester);
  });
}

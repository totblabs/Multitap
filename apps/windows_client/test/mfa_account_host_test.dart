import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/mfa_account.dart';
import 'package:multitap_client/data/mfa_account_controller.dart';
import 'package:multitap_client/data/mfa_account_host.dart';

import 'helpers/mfa_account_fake.dart';

class _Fixture {
  final api = MfaTestApi();
  DateTime now = DateTime.utc(2026, 11, 7);
  late final host = MfaAccountHost(clock: () => now);
  bool authenticated = true, current = true;
  late final MfaAccountController controller = host.begin(api: api, token: 'synthetic-only',
    accountId: '11111111-1111-4111-8111-111111111111', username: 'synthetic', actor: 'a' * 32,
    currentPresentation: () => current, currentAuthentication: () => authenticated,
    onCommitted: (_) {}, onAuthenticationRejected: (_) {})!;
  Future<void> enrollment() async { await controller.refresh(); controller.choose(MfaAccountAction.enable); await controller.submitProof('synthetic-password'); }
  void dispose() { host.finished(controller); host.dispose(); }
}
void main() {
  test('only exact pending mutation survives generic401, then late acknowledgement remains unauthenticated', () async {
    final f = _Fixture(); addTearDown(f.dispose); await f.enrollment();
    final reply = Completer<MfaAccountChange>(); f.api.change = (_) => reply.future;
    final work = f.controller.enable('123456');
    f.authenticated = false; f.host.sessionRetired('synthetic-only', 'session_expired');
    expect(f.controller.phase, MfaAccountPhase.closed); expect(f.host.inbox.pendingFor(f.controller.owner), isTrue);
    reply.complete(mfaChange()); await work;
    expect(f.host.inbox.completed!.codes, mfaCodes); expect(f.authenticated, isFalse);
    expect(f.api.calls, ['status', 'setup', 'enable']);
  });
  for (final reason in ['pc_login_replaced', 'session_idle_timeout', 'unrecognized_reason']) {
    test('$reason after generic401 invalidates pending ticket before lateACK', () async {
      final f = _Fixture(); addTearDown(f.dispose); await f.enrollment();
      final reply = Completer<MfaAccountChange>(); f.api.change = (_) => reply.future;
      final work = f.controller.enable('123456'); f.authenticated = false;
      f.host.sessionRetired('synthetic-only', 'session_expired');
      f.host.sessionRetired('synthetic-only', reason);
      final value = mfaChange(); reply.complete(value); await work;
      expect(f.host.inbox.completed, isNull); expect(value.takeCodes(), isEmpty);
    });
  }
  test('wrong token or native actor never retires a current completed receipt', () async {
    final f = _Fixture(); addTearDown(f.dispose); await f.enrollment(); await f.controller.enable('123456');
    f.host.sessionRetired('unrelated-successor', 'pc_login_replaced');
    f.host.authenticationRetired('b' * 32, 'session_idle_timeout');
    expect(f.host.inbox.completed!.codes, mfaCodes);
    f.host.authenticationRetired('a' * 32, 'pc_login_replaced'); expect(f.host.inbox.completed, isNull);
  });
  test('generic401 without a reserved commit cannot preserve setup or mint later codes', () async {
    final f = _Fixture(); addTearDown(f.dispose); await f.enrollment();
    final setup = f.controller.enrollment!;
    f.host.sessionRetired('synthetic-only', 'authentication_required');
    expect(setup.secret, isEmpty); expect(f.controller.owner.current, isFalse);
    await f.controller.enable('123456'); expect(f.api.calls, ['status', 'setup']);
  });
  for (final change in ['manual logout', 'origin switch', 'account replacement', 'successor login']) {
    test('$change discards completed codes and old ticket', () async {
      final f = _Fixture(); addTearDown(f.dispose); await f.enrollment(); await f.controller.enable('123456');
      f.host.retire(); expect(f.host.inbox.completed, isNull); expect(f.controller.owner.current, isFalse);
    });
  }
  testWidgets('network notifications and resume checks never reset completed30min deadline', (tester) async {
    final f = _Fixture(); addTearDown(f.dispose); await f.enrollment(); await f.controller.enable('123456');
    for (var minute = 0; minute < 29; minute++) {
      f.now = f.now.add(const Duration(minutes: 1));
      // A real root rebuild/resume can check, but cannot produce human input.
      expect(f.host.checkPrivacyIdle(), isTrue); await f.controller.refresh();
      await tester.pump(const Duration(minutes: 1));
    }
    expect(f.host.inbox.completed, isNotNull); f.now = f.now.add(const Duration(minutes: 1));
    await tester.pump(const Duration(minutes: 1));
    expect(f.host.inbox.completed, isNull); expect(f.api.calls, ['status', 'setup', 'enable']);
  });
  testWidgets('actual completed human interaction extends only the unauthenticated code deadline', (tester) async {
    final f = _Fixture(); addTearDown(f.dispose); await f.enrollment(); await f.controller.enable('123456');
    f.now = f.now.add(const Duration(minutes: 29)); await tester.pump(const Duration(minutes: 29)); f.host.human();
    f.now = f.now.add(const Duration(minutes: 29)); await tester.pump(const Duration(minutes: 29));
    expect(f.host.inbox.completed, isNotNull); expect(f.api.calls, ['status', 'setup', 'enable']);
    f.now = f.now.add(const Duration(minutes: 1)); await tester.pump(const Duration(minutes: 1)); expect(f.host.inbox.completed, isNull);
  });
  test('clock rollback discards codes instead of extending their lifetime', () async {
    final f = _Fixture(); addTearDown(f.dispose); await f.enrollment(); await f.controller.enable('123456');
    f.now = f.now.subtract(const Duration(seconds: 1)); expect(f.host.checkPrivacyIdle(), isFalse); expect(f.host.inbox.completed, isNull);
  });
}

import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/mfa_account_controller.dart';

import 'helpers/mfa_account_fake.dart';

void main() {
  for (final stage in ['read', 'setup', 'reauth', 'mutation']) {
    test('own ${stage}401 notifies exact held root and cannot manufacture completed codes', () async {
      final f = MfaTestOwner(); addTearDown(f.dispose);
      const rejected = BrokerException('session_expired', 401);
      if (stage == 'read') {
        f.api.read = () => Future.error(rejected); await f.controller.refresh();
      } else if (stage == 'setup') {
        await f.controller.refresh(); f.controller.choose(MfaAccountAction.enable);
        f.api.setup = () => Future.error(rejected); await f.controller.submitProof('synthetic-password');
      } else if (stage == 'reauth') {
        f.api.status = const MfaAccountStatus(enabled: true, recoveryCodesRemaining: 3);
        await f.controller.refresh(); f.controller.choose(MfaAccountAction.disable);
        f.api.reauth = () => Future.error(rejected); await f.controller.submitProof('synthetic-password', code: '123456');
      } else {
        await f.enrollment(); f.api.change = (_) => Future.error(rejected); await f.controller.enable('123456');
      }
      expect(f.authRejections, 1); expect(f.controller.phase, MfaAccountPhase.closed); expect(f.inbox.completed, isNull);
      expect(f.inbox.pendingFor(f.owner), isFalse);
    });
  }
  test('late own401 never invokes retirement for a successor owner', () async {
    final f = MfaTestOwner(); addTearDown(f.dispose);
    final read = Completer<MfaAccountStatus>(); f.api.read = () => read.future;
    final work = f.controller.refresh(); f.presentation = false; f.authenticated = false;
    read.completeError(const BrokerException('session_expired', 401)); await work;
    expect(f.authRejections, 0); expect(f.inbox.completed, isNull);
  });
  test('recovery normalization admits ONLY raw32 or exact eight groups of four', () {
    const raw = 'ABCDEF0123456789ABCDEF0123456789';
    expect(normalizeMfaRecoveryCode(raw), raw.toLowerCase());
    expect(normalizeMfaRecoveryCode('ABCD-EF01-2345-6789-ABCD-EF01-2345-6789'), raw.toLowerCase());
    for (final value in ['$raw ', ' $raw', raw.substring(1), '$raw-', 'ABCD_EF01_2345_6789_ABCD_EF01_2345_6789', 'ABCD--EF0123456789ABCDEF0123456789']) {
      expect(normalizeMfaRecoveryCode(value), isNull);
    }
  });
  test('status uses actual fields and never interprets unavailable as MFA OFF', () async {
    expect(() => MfaAccountStatus.parse({'protocol': 'mfa-v1', 'enabled': false, 'recoveryCodesRemaining': 1}), throwsFormatException);
    expect(() => MfaAccountStatus.parse({'protocol': 'mfa-v1', 'enabled': false, 'recoveryCodesRemaining': 0, 'available': true}), throwsFormatException);
    final f = MfaTestOwner(); addTearDown(f.dispose);
    f.api.read = () => Future.error(const BrokerException('not_found', 404));
    await f.controller.refresh(); f.controller.choose(MfaAccountAction.enable);
    expect(f.controller.status, isNull); expect(f.controller.phase, MfaAccountPhase.unavailable);
    expect(f.api.calls, ['status']);
  });
  test('enrollment rejects substituted URI secret and retains no secret after forget', () {
    final setup = mfaEnrollment();
    expect(setup.remaining, greaterThan(Duration.zero));
    expect(setup.uri!.queryParameters['secret'], setup.secret);
    expect(setup.toString(), isNot(contains(setup.secret)));
    expect(() => MfaEnrollment.parse({'protocol': 'mfa-v1', 'setupToken': 'a' * 43,
      'expiresAt': DateTime.now().add(const Duration(minutes: 2)).toIso8601String(),
      'secret': setup.secret, 'uri': setup.uri.toString().replaceFirst('secret=${setup.secret}', 'secret=${'A' * 32}')}), throwsFormatException);
    setup.forget(); expect(setup.secret, isEmpty); expect(setup.token, isEmpty); expect(setup.uri, isNull); expect(setup.remaining, Duration.zero);
  });
  test('strict change requires actual requiresLogin and exactly ten unique codes', () {
    expect(() => MfaAccountChange.parse({'enabled': true, 'requiresLogin': false, 'recoveryCodes': mfaCodes}, MfaAccountAction.enable), throwsFormatException);
    expect(() => MfaAccountChange.parse({'enabled': true, 'requiresLogin': true, 'recoveryCodes': List.filled(10, mfaCodes.first)}, MfaAccountAction.enable), throwsFormatException);
    expect(() => MfaAccountChange.parse({'enabled': false, 'requiresLogin': true, 'recoveryCodes': []}, MfaAccountAction.disable), throwsFormatException);
  });
  test('same pending ticket keeps acknowledged codes after authorizing401 and controller disposal', () async {
    final f = MfaTestOwner();
    addTearDown(f.inbox.dispose);
    await f.enrollment();
    final reply = Completer<MfaAccountChange>(); f.api.change = (_) => reply.future;
    final work = f.controller.enable('123456');
    expect(f.controller.mutating, isTrue); expect(f.controller.closeByUser(), isFalse);
    f.authenticated = false; f.controller.authenticationRetired(); f.controller.dispose();
    reply.complete(mfaChange()); await work;
    expect(f.inbox.completed!.codes, mfaCodes); expect(f.inbox.completed!.requiresLogin, isTrue);
    expect(f.inbox.completed!.owner, same(f.owner));
    expect(f.api.calls, ['status', 'setup', 'enable']);
    expect(f.inbox.acknowledgeSaved(saved: false), isFalse); expect(f.inbox.completed, isNotNull);
    expect(f.inbox.acknowledgeSaved(saved: true), isTrue); expect(f.inbox.completed, isNull);
  });
  for (final reason in ['account change', 'origin change', 'manual signout', 'successor login']) {
    test('late acknowledged codes never enter $reason owner', () async {
      final f = MfaTestOwner(); addTearDown(f.dispose); await f.enrollment();
      final reply = Completer<MfaAccountChange>(); f.api.change = (_) => reply.future;
      final work = f.controller.enable('123456');
      f.presentation = false; f.inbox.retire(f.owner);
      final returned = mfaChange(); reply.complete(returned); await work;
      expect(f.inbox.completed, isNull); expect(returned.takeCodes(), isEmpty);
      expect(f.controller.phase, MfaAccountPhase.closed);
    });
  }
  test('reauth completes before mutation and retired auth between calls prevents second POST', () async {
    final f = MfaTestOwner(); addTearDown(f.dispose);
    f.api.status = const MfaAccountStatus(enabled: true, recoveryCodesRemaining: 7);
    await f.controller.refresh(); f.controller.choose(MfaAccountAction.regenerate);
    final proof = Completer<void>(); f.api.reauth = () => proof.future;
    final work = f.controller.submitProof('synthetic-password', code: '123456');
    expect(f.api.calls, ['status', 'reauth']);
    f.authenticated = false; f.controller.authenticationRetired(); proof.complete(); await work;
    expect(f.api.calls, ['status', 'reauth']); expect(f.inbox.completed, isNull);
  });
  test('disable is real ordered reauth then change and preserves actual requiresLogin', () async {
    final f = MfaTestOwner(); addTearDown(f.dispose);
    f.api.status = const MfaAccountStatus(enabled: true, recoveryCodesRemaining: 4);
    await f.controller.refresh(); f.controller.choose(MfaAccountAction.disable);
    await f.controller.submitProof('synthetic-password', recoveryCode: mfaCodes.first);
    expect(f.api.calls, ['status', 'reauth', 'disable']);
    expect(f.inbox.completed!.enabled, isFalse); expect(f.inbox.completed!.codes, isEmpty);
    expect(f.inbox.completed!.requiresLogin, isTrue);
  });
  test('network uncertainty cannot auto replay or issue another request in same owner', () async {
    final f = MfaTestOwner(); addTearDown(f.dispose); await f.enrollment();
    f.api.change = (_) => Future.error(const FormatException('synthetic lost reply'));
    await f.controller.enable('123456');
    expect(f.controller.phase, MfaAccountPhase.uncertain); expect(f.inbox.completed, isNull);
    await f.controller.enable('123456'); await f.controller.refresh(); f.controller.choose(MfaAccountAction.enable);
    expect(f.api.calls, ['status', 'setup', 'enable']);
  });
  test('closing completed management route does not discard root once-only codes', () async {
    final f = MfaTestOwner(); addTearDown(f.dispose); await f.enrollment(); await f.controller.enable('123456');
    expect(f.controller.closeByUser(), isTrue); expect(f.inbox.completed!.codes, mfaCodes);
    expect(f.owner.current, isTrue);
  });
  test('stale setup reply is forgotten after auth retirement before any enable', () async {
    final f = MfaTestOwner(); addTearDown(f.dispose);
    await f.controller.refresh(); f.controller.choose(MfaAccountAction.enable);
    final reply = Completer<MfaEnrollment>(); f.api.setup = () => reply.future;
    final work = f.controller.submitProof('synthetic-password');
    f.authenticated = false; f.controller.authenticationRetired();
    final value = mfaEnrollment(); reply.complete(value); await work;
    expect(value.token, isEmpty); expect(value.secret, isEmpty); expect(f.api.calls, ['status', 'setup']);
  });
  testWidgets('setup timer retires key and returns to proof without POST', (tester) async {
    final f = MfaTestOwner(); addTearDown(f.dispose); await f.enrollment();
    await tester.pump(const Duration(minutes: 2));
    expect(f.controller.enrollment, isNull); expect(f.controller.phase, MfaAccountPhase.proof);
    expect(f.api.calls, ['status', 'setup']);
  });
}

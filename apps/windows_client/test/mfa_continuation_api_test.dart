// Private source fixture. Injected HTTPS HttpClient proves request structure,
// not actual TLS or server enrollment. Root owns SDK execution.
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';

import 'account_login_context_parse_test.dart' show loginChallengeFixture;
import 'mfa_api_test.dart' show pending, signedIn;
import 'pc_login_policy_api_test.dart' show AckClient;

final _origin = Uri.parse('https://mfa-continuation.fixture.invalid');
Map<String, dynamic> _confirmation({bool malformed = false}) => {
  'error': {
    'code': 'login_confirmation_required',
    'message': 'login_confirmation_required',
    'details': {
      ...loginChallengeFixture(),
      if (malformed) 'unexpected': true,
    },
  },
};
Future<MfaChallenge> _begin(HttpBrokerApi api, {bool manual = true}) async {
  try {
    if (manual) {
      await api.loginInteractively('alice', 'synthetic-password');
    } else {
      await api.login('alice', 'synthetic-password');
    }
  } on MfaRequiredException catch (required) {
    return required.challenge;
  }
  throw StateError('Expected the actual typed MFA challenge');
}

void main() {
  test('post-MFA409 carries a NEW typed confirmation and explicit confirm is the only third POST', () async {
    final client = AckClient()..queue.addAll([
      (202, pending(), null),
      (409, _confirmation(), null),
      (200, signedIn(), windowsPcSessionPolicy),
    ]);
    final api = HttpBrokerApi(origin: _origin, client: client, requestPcLoginReplacement: true);
    addTearDown(api.close);
    final mfa = await _begin(api);
    expect(mfa.pcLoginIntent, PcLoginIntent.confirmable);
    LoginConfirmationRequired? continuation;
    try { await api.verifyMfa(mfa, code: '123456'); }
    on LoginConfirmationRequired catch (required) { continuation = required; }
    expect(continuation, isNotNull);
    expect(mfa.token, isEmpty);
    expect(client.requests, hasLength(2), reason: 'MFA does not automatically confirm or retry password login');
    final verification = client.requests.last;
    expect(verification.uri.path, '/api/v1/auth/mfa/verify');
    expect(verification.uri.query, isEmpty);
    expect(verification.followRedirects, isFalse);
    expect(verification.headers.values.containsKey('authorization'), isFalse);
    expect(verification.headers.values[pcLoginIntentHeader], 'confirmable-v1');
    expect(jsonDecode(verification.body.toString()).keys.toSet(), {'challengeToken', 'code'});
    final session = await api.confirmLogin(continuation!.challenge);
    expect(session.token, 'final-session');
    expect(session.pcSessionPolicy, windowsPcSessionPolicy);
    expect(client.requests.map((request) => request.uri.path), [
      '/api/v1/auth/login', '/api/v1/auth/mfa/verify', '/api/v1/auth/login/confirm',
    ]);
    expect(jsonDecode(client.requests.last.body.toString()), {
      'challengeToken': loginChallengeFixture()['challengeToken'], 'confirm': true,
    });
    expect(continuation.challenge.forgotten, isTrue);
  });
  test('malformed post-MFA confirmation never manufactures authority', () async {
    final client = AckClient()..queue.addAll([(202, pending(), null), (409, _confirmation(malformed: true), null)]);
    final api = HttpBrokerApi(origin: _origin, client: client, requestPcLoginReplacement: true);
    addTearDown(api.close);
    final mfa = await _begin(api);
    await expectLater(api.verifyMfa(mfa, code: '123456'), throwsA(
      isA<BrokerException>().having((error) => error.code, 'code', 'invalid_response'),
    ));
    expect(client.requests, hasLength(2));
  });
  for (final optedIn in [false, true]) {
    test('recovery/legacy MFA never upgrades into manual replacement optedIn=$optedIn', () async {
      final client = AckClient()..queue.addAll([(202, pending(), null), (409, _confirmation(), null)]);
      final api = HttpBrokerApi(origin: _origin, client: client, requestPcLoginReplacement: optedIn);
      addTearDown(api.close);
      final mfa = await _begin(api, manual: false);
      Object? failure;
      try { await api.verifyMfa(mfa, recoveryCode: 'a' * 32); } catch (error) { failure = error; }
      expect(failure, isA<BrokerException>());
      expect(failure, isNot(isA<LoginConfirmationRequired>()));
      expect(mfa.token, isEmpty, reason: 'a consumed server MFA step is not retried as a password/OTP POST');
      expect(client.requests, hasLength(2));
      expect(client.requests.last.headers.values[pcLoginIntentHeader], optedIn ? 'recovery-v1' : isNull);
    });
  }
}

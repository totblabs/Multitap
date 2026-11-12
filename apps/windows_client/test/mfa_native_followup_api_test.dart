// PRIVATE UNCOMPILED regression source. Real HttpBrokerApi, injected HTTP only.
import 'dart:convert';
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'account_login_context_parse_test.dart' show loginChallengeFixture;
import 'mfa_api_test.dart' show pending, signedIn;
import 'pc_login_policy_api_test.dart' show AckClient;

final _origin = Uri.parse('https://mfa-followup.fixture.invalid');
const _lower = '0123456789abcdef0123456789abcdef';
const _grouped = '0123-4567-89AB-CDEF-0123-4567-89AB-CDEF';
class _CountingClient extends AckClient {
  int opens = 0;
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) {
    opens++;
    return super.openUrl(method, uri);
  }
}
Future<MfaChallenge> _begin(HttpBrokerApi api) async {
  try { await api.loginInteractively('alice', 'synthetic-password'); }
  on MfaRequiredException catch (required) { return required.challenge; }
  throw StateError('Expected actual negotiated pending MFA');
}
Map<String, Object?> _consent(Object? details) => {
  'error': {'code': 'login_confirmation_required', 'message': 'login_confirmation_required', 'details': details},
};

void main() {
  for (final value in [_lower, _lower.toUpperCase(), _grouped]) {
    test('recovery ${value == _lower ? 'raw' : value == _grouped ? 'grouped' : 'uppercase'} is sent as exact lower32 with no extra authority', () async {
      final client = AckClient()..queue.addAll([(202, pending(), null), (200, signedIn(), windowsPcSessionPolicy)]);
      final api = HttpBrokerApi(origin: _origin, client: client, requestPcLoginReplacement: true);
      addTearDown(api.close);
      final challenge = await _begin(api);
      final session = await api.verifyMfa(challenge, recoveryCode: value);
      expect(session.token, 'final-session');
      expect(client.requests, hasLength(2));
      final request = client.requests.last;
      expect(request.uri.path, '/api/v1/auth/mfa/verify');
      expect(request.uri.query, isEmpty);
      expect(jsonDecode(request.body.toString()), {'challengeToken': 'a' * 43, 'recoveryCode': _lower});
      expect(request.headers.values[pcLoginIntentHeader], 'confirmable-v1');
      expect(request.headers.values.containsKey('authorization'), isFalse);
      expect(challenge.token, isEmpty);
    });
  }
  test('bad recovery shapes and mutually exclusive proofs fail400 locally without an RPC', () async {
    final bad = <String>[
      '', 'a' * 31, 'a' * 33, 'a' * 64, ' $_lower', '$_lower ',
      '$_lower\n', 'Ａ' * 32, 'a' * 31 + 'g', '$_lower-',
      '01234567-89ab-cdef-0123-456789abcdef', //32hex, wrong grouping
      _grouped.replaceFirst('-', '--'), _grouped.replaceFirst('-', '_'),
    ];
    final client = _CountingClient();
    final api = HttpBrokerApi(origin: _origin, client: client, requestPcLoginReplacement: true);
    addTearDown(api.close);
    for (final value in bad) {
      final challenge = MfaChallenge.fromJson(pending()['mfa'], origin: _origin, pcLoginIntent: PcLoginIntent.confirmable);
      await expectLater(api.verifyMfa(challenge, recoveryCode: value), throwsA(
        isA<BrokerException>().having((e) => e.code, 'code', 'invalid_mfa_fields').having((e) => e.status, 'status', 400),
      ));
      expect(client.requests, isEmpty);
      expect(client.opens, 0);
      expect(challenge.token, isNotEmpty, reason: 'A local format rejection has not spent this challenge.');
      challenge.forget();
    }
    final challenge = MfaChallenge.fromJson(pending()['mfa'], origin: _origin);
    await expectLater(api.verifyMfa(challenge, code: '123456', recoveryCode: _grouped), throwsA(
      isA<BrokerException>().having((e) => e.code, 'code', 'invalid_mfa_fields'),
    ));
    expect(client.requests, isEmpty);
    expect(client.opens, 0);
  });
  for (final (label, details) in <(String, Object?)>[
    ('unknown field', {...loginChallengeFixture(), 'unexpected': true}),
    ('non-map', 'not-a-consent-proof'), ('list', <Object?>[]), ('missing', null),
  ]) {
    test('matched post-MFA malformed409 $label retires token while preserving invalid_response', () async {
      final client = AckClient()..queue.addAll([(202, pending(), null), (409, _consent(details), null)]);
      final api = HttpBrokerApi(origin: _origin, client: client, requestPcLoginReplacement: true);
      addTearDown(api.close);
      final challenge = await _begin(api);
      await expectLater(api.verifyMfa(challenge, code: '123456'), throwsA(
        isA<BrokerException>().having((e) => e.code, 'code', 'invalid_response').having((e) => e.status, 'status', 409),
      ));
      expect(challenge.token, isEmpty);
      expect(challenge.remaining, Duration.zero);
      expect(client.requests, hasLength(2));
      await expectLater(api.verifyMfa(challenge, recoveryCode: _lower), throwsA(
        isA<BrokerException>().having((e) => e.code, 'code', 'mfa_challenge_expired'),
      ));
      expect(client.requests, hasLength(2), reason: 'Manual resubmit cannot replay a possibly consumed MFA token.');
    });
  }
  test('ordinary invalid OTP remains a retryable MFA error rather than fresh-login retirement', () async {
    final client = AckClient()..queue.addAll([(202, pending(), null), (400, {'error': {'code': 'mfa_invalid_code'}}, null)]);
    final api = HttpBrokerApi(origin: _origin, client: client, requestPcLoginReplacement: true);
    addTearDown(api.close);
    final challenge = await _begin(api);
    await expectLater(api.verifyMfa(challenge, code: '123456'), throwsA(
      isA<BrokerException>().having((e) => e.code, 'code', 'mfa_invalid_code'),
    ));
    expect(challenge.token, isNotEmpty);
    expect(client.requests, hasLength(2));
    challenge.forget();
  });
}

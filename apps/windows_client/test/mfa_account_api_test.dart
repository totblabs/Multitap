// Injected HTTPS request/response structure. No TLS, enrollment or OS account.
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_transport.dart';

import 'helpers/mfa_account_fake.dart';
import 'pc_login_policy_api_test.dart' show AckClient;

void main() {
  setUp(BrokerTransport.clear); tearDown(BrokerTransport.clear);
  test('typed account status and six HTTPS endpoints preserve bearer, exact bodies and no redirects', () async {
    final setup = mfaEnrollment();
    final client = AckClient()..queue.addAll([
      (200, {'protocol': 'mfa-v1', 'enabled': false, 'recoveryCodesRemaining': 0}, null),
      (200, {'protocol': 'mfa-v1', 'setupToken': setup.token, 'secret': setup.secret, 'uri': setup.uri.toString(), 'expiresAt': setup.expiresAt.toIso8601String()}, null),
      (200, {'enabled': true, 'requiresLogin': true, 'recoveryCodes': mfaCodes}, null),
      (200, {'ok': true}, null),
      (200, {'enabled': false, 'requiresLogin': true}, null),
      (200, {'enabled': true, 'requiresLogin': true, 'recoveryCodes': mfaCodes}, null),
    ]);
    final api = HttpBrokerApi(origin: Uri.parse('https://synthetic.invalid'), client: client);
    addTearDown(api.close); addTearDown(setup.forget);
    expect((await api.readMfaAccount('synthetic-bearer')).enabled, isFalse);
    final candidate = await api.setupMfaAccount('synthetic-bearer', 'synthetic-password');
    addTearDown(candidate.forget);
    expect((await api.enableMfaAccount('synthetic-bearer', candidate, '123456')).requiresLogin, isTrue);
    await api.reauthenticateMfaAccount('synthetic-bearer', currentPassword: 'synthetic-password', recoveryCode: 'ABCD-EF01-2345-6789-ABCD-EF01-2345-6789');
    expect((await api.disableMfaAccount('synthetic-bearer', 'synthetic-password')).enabled, isFalse);
    expect((await api.regenerateMfaRecovery('synthetic-bearer', 'synthetic-password')).requiresLogin, isTrue);
    expect(client.requests.map((r) => r.method), ['GET', 'POST', 'POST', 'POST', 'POST', 'POST']);
    expect(client.requests.map((r) => r.uri.path), ['', '/setup', '/enable', '/reauth', '/disable', '/recovery'].map((suffix) => '/api/v1/auth/mfa$suffix'));
    for (final request in client.requests) {
      expect(request.followRedirects, isFalse); expect(request.uri.query, isEmpty);
      expect(request.headers.values['authorization'], 'Bearer synthetic-bearer');
    }
    expect(client.requests.first.body.toString(), isEmpty);
    expect(jsonDecode(client.requests[1].body.toString()), {'currentPassword': 'synthetic-password'});
    expect(jsonDecode(client.requests[2].body.toString()), {'setupToken': candidate.token, 'code': '123456'});
    expect(jsonDecode(client.requests[3].body.toString()), {'currentPassword': 'synthetic-password', 'recoveryCode': 'abcdef0123456789abcdef0123456789'});
    expect(jsonDecode(client.requests[4].body.toString()), {'currentPassword': 'synthetic-password'});
    expect(jsonDecode(client.requests[5].body.toString()), {'currentPassword': 'synthetic-password'});
  });
  test('HTTP is refused before ANY network request or supplied current password', () async {
    final client = AckClient(), origin = Uri.parse('http://127.0.0.1:19999');
    final consent = BrokerTransport.approve(origin, origin.origin), setup = mfaEnrollment();
    final api = HttpBrokerApi(origin: origin, client: client);
    addTearDown(consent.release);
    addTearDown(api.close); addTearDown(setup.forget);
    final calls = <Future<void> Function()>[
      () async { await api.readMfaAccount('synthetic'); },
      () async { await api.setupMfaAccount('synthetic', 'synthetic-password'); },
      () async { await api.enableMfaAccount('synthetic', setup, '123456'); },
      () => api.reauthenticateMfaAccount('synthetic', currentPassword: 'synthetic-password', code: '123456'),
      () async { await api.disableMfaAccount('synthetic', 'synthetic-password'); },
      () async { await api.regenerateMfaRecovery('synthetic', 'synthetic-password'); },
    ];
    for (final call in calls) {
      await expectLater(call(), throwsA(isA<BrokerException>().having((e) => e.code, 'code', 'mfa_requires_https')));
    }
    expect(client.requests, isEmpty);
  });
  test('malformed success is uncertain response and never invents requiresLogin or codes', () async {
    final client = AckClient()..queue.add((200, {'enabled': true, 'recoveryCodes': mfaCodes}, null));
    final api = HttpBrokerApi(origin: Uri.parse('https://synthetic.invalid'), client: client);
    final setup = mfaEnrollment(); addTearDown(api.close); addTearDown(setup.forget);
    await expectLater(api.enableMfaAccount('synthetic', setup, '123456'), throwsA(isA<BrokerException>().having((e) => e.code, 'code', 'invalid_response')));
    expect(client.requests, hasLength(1));
  });
  test('invalid or mixed recovery proof never reaches transport', () async {
    final client = AckClient(); final api = HttpBrokerApi(origin: Uri.parse('https://synthetic.invalid'), client: client); addTearDown(api.close);
    await expectLater(api.reauthenticateMfaAccount('synthetic', currentPassword: 'synthetic-password', code: '123456', recoveryCode: 'a' * 32), throwsA(isA<BrokerException>()));
    await expectLater(api.reauthenticateMfaAccount('synthetic', currentPassword: 'synthetic-password', recoveryCode: 'abcd--ef0123456789abcdef0123456789'), throwsA(isA<BrokerException>()));
    expect(client.requests, isEmpty);
  });
}

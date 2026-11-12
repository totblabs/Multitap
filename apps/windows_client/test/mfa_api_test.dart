import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';

import 'power_api_test.dart' show ApiClient;

final origin = Uri.parse('https://mfa.fixture.invalid');
const challengeToken = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa';
Map<String, dynamic> pending({Map<String, Object?> patch = const {}}) => {
  'mfa': {
    'protocol': 'mfa-v1',
    'challengeToken': challengeToken,
    'expiresAt': DateTime.now()
        .toUtc()
        .add(const Duration(seconds: 120))
        .toIso8601String(),
    'methods': ['totp', 'recovery'],
    ...patch,
  },
};
Map<String, dynamic> signedIn() => {
  'sessionToken': 'final-session',
  'expiresAt': DateTime.now()
      .toUtc()
      .add(const Duration(hours: 1))
      .toIso8601String(),
  'user': {'username': 'alice'},
};
Future<MfaChallenge> begin(HttpBrokerApi api) async {
  try {
    await api.login('alice', 'fixture-password');
  } on MfaRequiredException catch (required) {
    return required.challenge;
  }
  throw StateError('Fixture expected pending challenge');
}

void main() {
  test(
    'MFA-OFF legacy200 keeps exact password body and negotiates only header',
    () async {
      final client = ApiClient()..responses.add((200, signedIn()));
      final api = HttpBrokerApi(origin: origin, client: client);
      addTearDown(api.close);
      final session = await api.login('alice', 'fixture-password');
      expect(session.token, 'final-session');
      final request = client.requests.single;
      expect(request.headers.values['X-Multitap-Auth-Protocol'], 'mfa-v1');
      expect(request.headers.values.containsKey('authorization'), isFalse);
      expect(jsonDecode(request.body.toString()), {
        'username': 'alice',
        'password': 'fixture-password',
      });
    },
  );
  for (final recovery in [false, true]) {
    test(
      '202 has no session, final ${recovery ? 'recovery' : 'OTP'} verification is body-only and clears challenge',
      () async {
        final client = ApiClient()
          ..responses.addAll([(202, pending()), (200, signedIn())]);
        final api = HttpBrokerApi(origin: origin, client: client);
        addTearDown(api.close);
        final challenge = await begin(api);
        expect(challenge.remaining.inSeconds, inInclusiveRange(118, 120));
        expect(challenge.toString(), isNot(contains(challengeToken)));
        expect(
          MfaRequiredException(challenge).toString(),
          isNot(contains(challengeToken)),
        );
        final session = await api.verifyMfa(
          challenge,
          code: recovery ? null : '123456',
          recoveryCode: recovery ? 'A' * 32 : null,
        );
        expect(session.token, 'final-session');
        final request = client.requests.last;
        expect(request.uri.path, '/api/v1/auth/mfa/verify');
        expect(request.uri.query, isEmpty);
        expect(request.headers.values.containsKey('authorization'), isFalse);
        expect(jsonDecode(request.body.toString()), {
          'challengeToken': challengeToken,
          if (recovery) 'recoveryCode': 'a' * 32 else 'code': '123456',
        });
        expect(challenge.token, isEmpty);
        expect(challenge.remaining, Duration.zero);
      },
    );
  }
  test('invalid202 protocol/token/methods/mixed session never establishes authentication', () async {
    for (final body in [
      pending(patch: {'protocol': 'future'}),
      pending(patch: {'challengeToken': 'short'}),
      pending(
        patch: {
          'methods': ['unknown'],
        },
      ),
      pending(patch: {'expiresAt': 'bad'}),
      {...pending(), ...signedIn()},
    ]) {
      final client = ApiClient()..responses.add((202, body));
      final api = HttpBrokerApi(origin: origin, client: client);
      await expectLater(
        api.login('alice', 'fixture-password'),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'invalid_response',
          ),
        ),
      );
      api.close();
    }
  });
  test('expired/cancelled/cross-origin challenge and mutually exclusive proof fail before network', () async {
    final client = ApiClient();
    final api = HttpBrokerApi(origin: origin, client: client);
    addTearDown(api.close);
    final expired = MfaChallenge.fromJson(
      (pending(
        patch: {
          'expiresAt': DateTime.now()
              .toUtc()
              .subtract(const Duration(seconds: 1))
              .toIso8601String(),
        },
      ))['mfa'],
      origin: origin,
    );
    await expectLater(
      api.verifyMfa(expired, code: '123456'),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'mfa_challenge_expired',
        ),
      ),
    );
    expect(expired.token, isEmpty);
    final foreign = MfaChallenge.fromJson(
      pending()['mfa'],
      origin: Uri.parse('https://other.invalid'),
    );
    await expectLater(
      api.verifyMfa(foreign, code: '123456'),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'mfa_requires_https',
        ),
      ),
    );
    final challenge = MfaChallenge.fromJson(pending()['mfa'], origin: origin);
    await expectLater(
      api.verifyMfa(challenge, code: '123456', recoveryCode: 'A' * 32),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'invalid_mfa_fields',
        ),
      ),
    );
    challenge.forget();
    await expectLater(
      api.verifyMfa(challenge, code: '123456'),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'mfa_challenge_expired',
        ),
      ),
    );
    expect(client.requests, isEmpty);
  });
  for (final (status, code) in [
    (400, 'mfa_invalid_code'),
    (410, 'mfa_challenge_expired'),
    (429, 'too_many_attempts'),
  ]) {
    test(
      '$status $code remains a fixed authentication error without a session',
      () async {
        final client = ApiClient()
          ..responses.add((
            status,
            {
              'error': {'code': code, 'message': challengeToken},
            },
          ));
        final api = HttpBrokerApi(origin: origin, client: client);
        addTearDown(api.close);
        final challenge = MfaChallenge.fromJson(
          pending()['mfa'],
          origin: origin,
        );
        await expectLater(
          api.verifyMfa(challenge, code: '123456'),
          throwsA(
            isA<BrokerException>()
                .having((e) => e.code, 'code', code)
                .having(
                  (e) => e.message,
                  'message',
                  isNot(contains(challengeToken)),
                ),
          ),
        );
        expect(challenge.token.isEmpty, status == 410);
      },
    );
  }
}

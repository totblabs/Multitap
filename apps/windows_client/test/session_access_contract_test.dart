import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/session_renewal.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/session_display_mode.dart';

import 'pc_login_policy_api_test.dart' show AckClient;
import 'mfa_api_test.dart' show signedIn;

void main() {
  test(
    'manual login is confirmable and never legacy destructive interactive',
    () async {
      final client = AckClient()
        ..queue.add((200, signedIn(), windowsPcSessionPolicy));
      final api = HttpBrokerApi(
        origin: Uri.parse('https://access.fixture.invalid'),
        client: client,
        requestPcLoginReplacement: true,
      );
      addTearDown(api.close);
      await api.loginInteractively('alice', 'synthetic');
      expect(
        client.requests.single.headers.values[pcLoginIntentHeader],
        'confirmable-v1',
      );
      expect(jsonDecode(client.requests.single.body.toString()), {
        'username': 'alice',
        'password': 'synthetic',
      });
    },
  );

  test('opaque confirmation is origin-bound, single-use and sends only explicit consent', () async {
    final client = AckClient()
      ..queue.add((
        409,
        {
          'error': {
            'code': 'login_confirmation_required',
            'details': {
              'protocol': sessionAccessProtocol,
              'challengeToken': 'synthetic_challenge_00000001',
              'expiresInMs': 120000,
            },
          },
        },
        null,
      ))
      ..queue.add((200, signedIn(), windowsPcSessionPolicy));
    final origin = Uri.parse('https://access.fixture.invalid');
    final api = HttpBrokerApi(
      origin: origin,
      client: client,
      requestPcLoginReplacement: true,
    );
    addTearDown(api.close);
    LoginConfirmation? challenge;
    try {
      await api.loginInteractively('alice', 'synthetic');
    } on LoginConfirmationRequired catch (required) {
      challenge = required.challenge;
    }
    expect(challenge, isNotNull);
    expect(
      client.requests,
      hasLength(1),
    ); // No eviction/confirm from parsing a conflict.
    await api.confirmLogin(challenge!);
    final confirm = client.requests.last;
    expect(confirm.uri.path, '/api/v1/auth/login/confirm');
    expect(confirm.headers.values[pcLoginIntentHeader], 'confirmable-v1');
    expect(jsonDecode(confirm.body.toString()), {
      'challengeToken': 'synthetic_challenge_00000001',
      'confirm': true,
    });
    expect(challenge.forgotten, isTrue);
    await expectLater(
      api.confirmLogin(challenge),
      throwsA(isA<BrokerException>()),
    );
    expect(client.requests, hasLength(2));
    final other = LoginConfirmation.parse(
      {
        'protocol': sessionAccessProtocol,
        'challengeToken': 'synthetic_other_0000000001',
        'expiresInMs': 120000,
      },
      Uri.parse('https://other.fixture.invalid'),
      DateTime.now(),
    );
    await expectLater(api.confirmLogin(other), throwsA(isA<BrokerException>()));
    expect(client.requests, hasLength(2));
  });

  test('cancel forgets challenge without a request; legacy intent rejection never retries destructively', () async {
    final client = AckClient()
      ..queue.add((
        400,
        {
          'error': {'code': 'invalid_login_intent'},
        },
        null,
      ));
    final api = HttpBrokerApi(
      origin: Uri.parse('https://access.fixture.invalid'),
      client: client,
      requestPcLoginReplacement: true,
    );
    addTearDown(api.close);
    await expectLater(
      api.loginInteractively('alice', 'synthetic'),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.code,
          'update required',
          'session_access_update_required',
        ),
      ),
    );
    expect(client.requests, hasLength(1));
    final challenge = LoginConfirmation.parse(
      {
        'protocol': sessionAccessProtocol,
        'challengeToken': 'synthetic_cancel_00000001',
        'expiresInMs': 120000,
      },
      api.origin,
      DateTime.now(),
    );
    challenge.forget();
    expect(challenge.forgotten, isTrue);
    expect(client.requests, hasLength(1));
  });

  test('accepted sessionAccess survives expiry and child launch; activity submits no client timestamp', () async {
    final deadline = DateTime.now()
        .toUtc()
        .add(const Duration(minutes: 30))
        .toIso8601String();
    final response = signedIn()
      ..['sessionAccess'] = {
        'protocol': sessionAccessProtocol,
        'idleTimeoutMs': 1800000,
        'warningMs': 60000,
        'idleEnabled': true,
        'idleDeadline': deadline,
        'serverTime': DateTime.parse(deadline)
            .subtract(const Duration(minutes: 30))
            .toIso8601String(),
        'warningAt': DateTime.parse(deadline)
            .subtract(const Duration(minutes: 1))
            .toIso8601String(),
      };
    final client = AckClient()
      ..queue.add((200, response, windowsPcSessionPolicy))
      ..queue.add((
        200,
        Map<String, dynamic>.from(response['sessionAccess'] as Map),
        null,
      ));
    final api = HttpBrokerApi(
      origin: Uri.parse('https://access.fixture.invalid'),
      client: client,
      requestPcLoginReplacement: true,
    );
    addTearDown(api.close);
    final session = await api.loginInteractively('alice', 'synthetic');
    expect(
      session.withExpiry(session.expiresAt).sessionAccess!.idleDeadline,
      DateTime.parse(deadline),
    );
    final child = SessionWindowLaunch.fromMap(
      SessionWindowLaunch(
        origin: api.origin,
        token: session.token,
        expiresAt: session.expiresAt,
        desktopId: 'pve-170',
        name: 'VM1',
        mode: SessionDisplayMode.windowed,
        options: StreamOptions.recommended,
        sessionAccess: session.sessionAccess,
        authenticationOwner: 'a' * 32,
      ).toMap(),
    );
    expect(child.sessionAccess!.idleDeadline, DateTime.parse(deadline));
    expect(child.authenticationOwner, 'a' * 32);
    expect(
      (await api.recordHumanActivity(session.token)).idleDeadline,
      DateTime.parse(deadline),
    );
    expect(client.requests.last.uri.path, '/api/v1/auth/activity');
    expect(jsonDecode(client.requests.last.body.toString()), isEmpty);
  });

  test(
    'malformed challenge fails closed and never exposes a confirmation handle',
    () async {
      for (final details in [
        {
          'protocol': 'other',
          'challengeToken': 'synthetic_challenge_00000001',
          'expiresInMs': 120000,
        },
        {
          'protocol': sessionAccessProtocol,
          'challengeToken': 'synthetic_challenge_00000001',
          'expiresInMs': 120001,
        },
        {
          'protocol': sessionAccessProtocol,
          'challengeToken': 'x',
          'expiresInMs': 120000,
        },
      ]) {
        final client = AckClient()
          ..queue.add((
            409,
            {
              'error': {
                'code': 'login_confirmation_required',
                'details': details,
              },
            },
            null,
          ));
        final api = HttpBrokerApi(
          origin: Uri.parse('https://access.fixture.invalid'),
          client: client,
          requestPcLoginReplacement: true,
        );
        addTearDown(api.close);
        await expectLater(
          api.loginInteractively('alice', 'synthetic'),
          throwsA(
            isA<BrokerException>().having(
              (e) => e.code,
              'invalid',
              'invalid_response',
            ),
          ),
        );
        expect(client.requests, hasLength(1));
      }
    },
  );

  test('definitive newer 401 invalidates older successful renewal', () async {
    final pending = <Completer<DateTime>>[];
    final renewed = <DateTime>[];
    final lapses = <RenewalLapse>[];
    final now = DateTime.utc(2026, 11, 5);
    final keeper = SessionRenewer(
      clock: () => now,
      renew: () {
        final call = Completer<DateTime>();
        pending.add(call);
        return call.future;
      },
      onRenewed: renewed.add,
      onLapse: lapses.add,
    )..start(now.add(const Duration(hours: 1)));
    addTearDown(keeper.stop);
    final older = keeper.renewNow();
    final newer = keeper.renewNow();
    pending[1].completeError(const BrokerException('session_expired', 401));
    expect(await newer, isFalse);
    pending[0].complete(now.add(const Duration(hours: 2)));
    expect(await older, isFalse);
    expect(renewed, isEmpty);
    expect(keeper.expiresAt, isNull);
    expect(lapses, [RenewalLapse.rejected]);
  });
}

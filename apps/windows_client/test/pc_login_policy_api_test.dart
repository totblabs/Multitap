import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'power_api_test.dart'
    show ApiClient, ApiHeaders, ApiRequest, ApiResponse;
import 'mfa_api_test.dart' show pending, signedIn;

class AckHeaders extends ApiHeaders {
  AckHeaders(this.ack);
  final Object? ack;
  @override
  String? value(String name) {
    if (name.toLowerCase() != pcSessionPolicyHeader.toLowerCase()) return null;
    if (ack is List) throw const HttpException('duplicate response header');
    return ack as String?;
  }
}

class AckResponse extends ApiResponse {
  AckResponse(super.statusCode, super.value, Object? ack)
    : headers = AckHeaders(ack);
  @override
  final HttpHeaders headers;
}

class AckClient extends ApiClient {
  final queue = <(int, Map<String, dynamic>, Object?)>[];
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    final next = queue.removeAt(0);
    final request = ApiRequest(
      method,
      uri,
      AckResponse(next.$1, next.$2, next.$3),
    );
    requests.add(request);
    return request;
  }
}

void main() {
  final origin = Uri.parse('https://pc-login.fixture.invalid');
  for (final interactive in [false, true]) {
    test(
      'Windows ${interactive ? 'manual' : 'automatic'} login keeps exact body and distinct intent',
      () async {
        final client = AckClient()
          ..queue.add((200, signedIn(), windowsPcSessionPolicy));
        final api = HttpBrokerApi(
          origin: origin,
          client: client,
          requestPcLoginReplacement: true,
        );
        addTearDown(api.close);
        final s = interactive
            ? await api.loginInteractively('alice', 'fixture-password')
            : await api.login('alice', 'fixture-password');
        final req = client.requests.single;
        expect(
          req.headers.values[pcSessionPolicyHeader],
          windowsPcSessionPolicy,
        );
        expect(
          req.headers.values[pcLoginIntentHeader],
          interactive ? 'confirmable-v1' : 'recovery-v1',
        );
        expect(jsonDecode(req.body.toString()), {
          'username': 'alice',
          'password': 'fixture-password',
        });
        expect(s.pcSessionPolicy, windowsPcSessionPolicy);
        expect(
          s
              .withExpiry(s.expiresAt.add(const Duration(hours: 1)))
              .pcSessionPolicy,
          windowsPcSessionPolicy,
        );
      },
    );
  }
  test('default Android/Host API neither sends nor adopts PC policy, even unsolicited ACK', () async {
    final client = AckClient()
      ..queue.add((200, signedIn(), windowsPcSessionPolicy));
    final api = HttpBrokerApi(origin: origin, client: client);
    addTearDown(api.close);
    final s = await api.loginInteractively('alice', 'fixture-password');
    expect(
      client.requests.single.headers.values.containsKey(pcSessionPolicyHeader),
      isFalse,
    );
    expect(
      client.requests.single.headers.values.containsKey(pcLoginIntentHeader),
      isFalse,
    );
    expect(s.pcSessionPolicy, isNull);
  });
  for (final ack in <Object?>[
    null,
    'future-v2',
    ' windows-pc-v1',
    ['windows-pc-v1', 'windows-pc-v1'],
  ]) {
    test(
      'absent/malformed policy ACK $ack establishes no guarantee and keeps old Broker login',
      () async {
        final client = AckClient()..queue.add((200, signedIn(), ack));
        final api = HttpBrokerApi(
          origin: origin,
          client: client,
          requestPcLoginReplacement: true,
        );
        addTearDown(api.close);
        final s = await api.loginInteractively('alice', 'fixture-password');
        expect(s.token, 'final-session');
        expect(s.pcSessionPolicy, isNull);
        expect(
          jsonDecode(client.requests.single.body.toString()).keys.toSet(),
          {'username', 'password'},
        );
      },
    );
  }
  for (final interactive in [false, true]) {
    test(
      'MFA retains original ${interactive ? 'interactive' : 'recovery'} intent only in memory through final verify',
      () async {
        final client = AckClient()
          ..queue.addAll([
            (202, pending(), null),
            (200, signedIn(), windowsPcSessionPolicy),
          ]);
        final api = HttpBrokerApi(
          origin: origin,
          client: client,
          requestPcLoginReplacement: true,
        );
        addTearDown(api.close);
        MfaChallenge? challenge;
        try {
          if (interactive) {
            await api.loginInteractively('alice', 'fixture-password');
          } else {
            await api.login('alice', 'fixture-password');
          }
        } on MfaRequiredException catch (e) {
          challenge = e.challenge;
        }
        expect(challenge, isNotNull);
        final s = await api.verifyMfa(challenge!, code: '123456');
        expect(
          client.requests.map((r) => r.headers.values[pcLoginIntentHeader]),
          [
            interactive ? 'confirmable-v1' : 'recovery-v1',
            interactive ? 'confirmable-v1' : 'recovery-v1',
          ],
        );
        expect(jsonDecode(client.requests.last.body.toString()).keys.toSet(), {
          'challengeToken',
          'code',
        });
        expect(s.pcSessionPolicy, windowsPcSessionPolicy);
        expect(challenge.token, isEmpty);
      },
    );
  }
  test('child optional policy roundtrip and legacy absent bootstrap remain compatible', () {
    SessionWindowLaunch make(String? policy) => SessionWindowLaunch(
      origin: origin,
      token: 'fixture-token',
      expiresAt: DateTime.now().add(const Duration(hours: 1)),
      desktopId: 'pve-149',
      name: 'fixture',
      mode: SessionDisplayMode.windowed,
      options: const StreamOptions(),
      pcSessionPolicy: policy,
    );
    for (final policy in [null, windowsPcSessionPolicy, 'unknown']) {
      final wire = make(policy).toMap();
      expect(
        SessionWindowLaunch.fromMap(wire).pcSessionPolicy,
        policy == windowsPcSessionPolicy ? policy : null,
      );
    }
  });
}

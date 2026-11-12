import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';

import 'power_api_test.dart' show ApiClient;
import 'mfa_api_test.dart' show signedIn;
import 'account_login_context_parse_test.dart' show loginChallengeFixture;

Map<String, Object?> _offer({String id = 'pve-170'}) => {
  'protocol': streamTakeoverProtocol,
  'context': 'C' * 43,
  'expiresInSeconds': 120,
  'desktop': {'id': id, 'name': 'WorkPC'},
  'sessions': [
    {'username': 'alice', 'createdAt': null, 'deviceLabel': null},
  ],
};

Map<String, dynamic> _busy({Object? offer}) => {
  'error': {
    'code': 'desktop_busy',
    'details': {
      'reason': 'controller_in_use',
      'takeover': true,
      'takeoverOffer': ?offer,
    },
  },
};

void main() {
  final origin = Uri.parse('https://stream-context.fixture.invalid');
  test(
    'default legacy client does not make an optional preflight request',
    () async {
      final client = ApiClient();
      final api = HttpBrokerApi(origin: origin, client: client);
      addTearDown(api.close);
      await api.checkStreamTakeover('synthetic-token', 'pve-170');
      expect(client.requests, isEmpty);
    },
  );
  test(
    'read-only preflight uses exact path, empty body, bearer and opt-in',
    () async {
      final client = ApiClient()..responses.add((200, {'busy': false}));
      final api = HttpBrokerApi(
        origin: origin,
        client: client,
        requestStreamTakeoverContext: true,
      );
      addTearDown(api.close);
      await api.checkStreamTakeover('synthetic-token', 'pve-170');
      final request = client.requests.single;
      expect(request.method, 'POST');
      expect(request.uri.path, '/api/v1/desktops/pve-170/takeover-context');
      expect(request.headers.values[streamTakeoverHeader], 'v1');
      expect(request.headers.values['authorization'], 'Bearer synthetic-token');
      expect(jsonDecode(request.body.toString()), isEmpty);
    },
  );
  test(
    'occupied preflight carries the same typed bound offer without connect',
    () async {
      final client = ApiClient()..responses.add((409, _busy(offer: _offer())));
      final api = HttpBrokerApi(
        origin: origin,
        client: client,
        requestStreamTakeoverContext: true,
      );
      addTearDown(api.close);
      await expectLater(
        api.checkStreamTakeover('synthetic-token', 'pve-170'),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.takeoverOffer?.desktopId,
            'bound offer',
            'pve-170',
          ),
        ),
      );
      expect(client.requests.single.uri.path, endsWith('/takeover-context'));
    },
  );
  for (final refusal in [
    (401, 'session_expired'),
    (403, 'forbidden'),
    (404, 'desktop_not_found'),
  ]) {
    test('preflight never ignores ${refusal.$1}/${refusal.$2}', () async {
      final client = ApiClient()
        ..responses.add((
          refusal.$1,
          {
            'error': {'code': refusal.$2},
          },
        ));
      final api = HttpBrokerApi(
        origin: origin,
        client: client,
        requestStreamTakeoverContext: true,
      );
      addTearDown(api.close);
      await expectLater(
        api.checkStreamTakeover('synthetic-token', 'pve-170'),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'authoritative refusal',
            refusal.$2,
          ),
        ),
      );
      expect(client.requests, hasLength(1));
    });
  }
  test(
    'only an old-server unknown endpoint continues to ordinary gated connect',
    () async {
      final client = ApiClient()
        ..responses.add((
          404,
          {
            'error': {'code': 'not_found'},
          },
        ))
        ..responses.add((409, _busy()));
      final api = HttpBrokerApi(
        origin: origin,
        client: client,
        requestStreamTakeoverContext: true,
      );
      addTearDown(api.close);
      await api.checkStreamTakeover('synthetic-token', 'pve-170');
      await expectLater(
        api.connect('synthetic-token', 'pve-170'),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.takeoverOffer,
            'no legacy context or release',
            isNull,
          ),
        ),
      );
      expect(client.requests.map((r) => r.uri.path).toList(), [
        '/api/v1/desktops/pve-170/takeover-context',
        '/api/v1/desktops/pve-170/connect',
      ]);
    },
  );
  test('unoccupied old server still admits the ordinary stream after unknown preflight', () async {
    final client = ApiClient()
      ..responses.add((
        404,
        {
          'error': {'code': 'not_found'},
        },
      ))
      ..responses.add((
        200,
        {
          'sessionId': 'synthetic-session',
          'webSocketUrl': 'wss://stream-context.fixture.invalid/api/v1/sessions/synthetic-session',
          'mode': 'stream',
          'transport': 'media-v1',
        },
      ));
    final api = HttpBrokerApi(
      origin: origin,
      client: client,
      requestStreamTakeoverContext: true,
    );
    addTearDown(api.close);
    await api.checkStreamTakeover('synthetic-token', 'pve-170');
    final connection = await api.connect('synthetic-token', 'pve-170');
    expect(connection.sessionId, 'synthetic-session');
    expect(client.requests.map((r) => r.uri.path).toList(), [
      '/api/v1/desktops/pve-170/takeover-context',
      '/api/v1/desktops/pve-170/connect',
    ]);
  });
  for (final response in [
    {'busy': true},
    {'busy': false, 'extra': true},
    <String, Object?>{},
  ]) {
    test('invalid successful preflight is closed: $response', () async {
      final client = ApiClient()..responses.add((200, response));
      final api = HttpBrokerApi(
        origin: origin,
        client: client,
        requestStreamTakeoverContext: true,
      );
      addTearDown(api.close);
      await expectLater(
        api.checkStreamTakeover('synthetic-token', 'pve-170'),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'invalid response',
            'invalid_response',
          ),
        ),
      );
      expect(client.requests, hasLength(1));
    });
  }
  test(
    'opted-in connect preserves bearer/body and carries a typed bound offer',
    () async {
      final client = ApiClient()..responses.add((409, _busy(offer: _offer())));
      final api = HttpBrokerApi(
        origin: origin,
        client: client,
        requestStreamTakeoverContext: true,
      );
      addTearDown(api.close);
      await expectLater(
        api.connect('synthetic-token', 'pve-170'),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.takeoverOffer?.desktopId,
            'bound offer',
            'pve-170',
          ),
        ),
      );
      final request = client.requests.single;
      expect(request.headers.values[streamTakeoverHeader], 'v1');
      expect(request.headers.values['authorization'], 'Bearer synthetic-token');
      expect(jsonDecode(request.body.toString()), {
        'mode': 'stream',
        'resizeProtocol': 'epoch-v1',
        'clipboardProtocol': 'clipboard-sync-v1',
        'options': {
          'maxWidth': 3840,
          'maxHeight': 2160,
          'fps': 60,
          'bitrate': 20000000,
          'audio': true,
          'monitor': '0',
        },
      });
    },
  );

  test('confirmed takeover sends exactly context with opt-in, never empty fallback', () async {
    final client = ApiClient()..responses.add((200, {'released': true}));
    final api = HttpBrokerApi(origin: origin, client: client);
    addTearDown(api.close);
    final offer = StreamTakeoverOffer.parse(_offer());
    expect(
      await api.confirmTakeover('synthetic-token', 'pve-170', offer),
      isTrue,
    );
    final request = client.requests.single;
    expect(request.method, 'POST');
    expect(request.uri.path, '/api/v1/desktops/pve-170/takeover');
    expect(request.headers.values[streamTakeoverHeader], 'v1');
    expect(jsonDecode(request.body.toString()), {'context': offer.context});
  });

  for (final code in ['takeover_context_stale', 'takeover_context_expired']) {
    test(
      'server $code is surfaced without a legacy or automatic retry',
      () async {
        final client = ApiClient()
          ..responses.add((
            409,
            {
              'error': {'code': code},
            },
          ));
        final api = HttpBrokerApi(origin: origin, client: client);
        addTearDown(api.close);
        await expectLater(
          api.confirmTakeover(
            'synthetic-token',
            'pve-170',
            StreamTakeoverOffer.parse(_offer()),
          ),
          throwsA(isA<BrokerException>().having((e) => e.code, 'code', code)),
        );
        expect(client.requests, hasLength(1));
        expect(client.requests.single.body.toString(), isNot('{}'));
      },
    );
  }

  test('wrong target and locally expired offer send no request', () async {
    final client = ApiClient();
    final api = HttpBrokerApi(origin: origin, client: client);
    addTearDown(api.close);
    await expectLater(
      api.confirmTakeover(
        'synthetic-token',
        'pve-171',
        StreamTakeoverOffer.parse(_offer()),
      ),
      throwsA(isA<BrokerException>()),
    );
    await expectLater(
      api.confirmTakeover(
        'synthetic-token',
        'pve-170',
        StreamTakeoverOffer.parse(_offer(), receivedAt: DateTime.utc(2020)),
      ),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'takeover_context_expired',
        ),
      ),
    );
    expect(client.requests, isEmpty);
  });

  test('a mismatched or malformed offered desktop is rejected', () async {
    final client = ApiClient()
      ..responses.add((409, _busy(offer: _offer(id: 'pve-171'))));
    final api = HttpBrokerApi(
      origin: origin,
      client: client,
      requestStreamTakeoverContext: true,
    );
    addTearDown(api.close);
    await expectLater(
      api.connect('synthetic-token', 'pve-170'),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'invalid_response',
        ),
      ),
    );
    expect(client.requests, hasLength(1));
  });

  test('legacy missing offer never synthesizes context and default does not opt in', () async {
    final client = ApiClient()..responses.add((409, _busy()));
    final api = HttpBrokerApi(origin: origin, client: client);
    addTearDown(api.close);
    await expectLater(
      api.connect('synthetic-token', 'pve-170'),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.takeoverOffer,
          'no fake offer',
          isNull,
        ),
      ),
    );
    expect(
      client.requests.single.headers.values.containsKey(streamTakeoverHeader),
      isFalse,
    );
  });

  for (final manual in [false, true]) {
    test('login context opt-in is manual-only: $manual', () async {
      final client = ApiClient()..responses.add((200, signedIn()));
      final api = HttpBrokerApi(
        origin: origin,
        client: client,
        requestPcLoginReplacement: true,
      );
      addTearDown(api.close);
      if (manual) {
        await api.loginInteractively('alice', 'synthetic-password');
      } else {
        await api.login('alice', 'synthetic-password');
      }
      expect(
        client.requests.single.headers.values['X-Multitap-Login-Context'],
        manual ? 'v1' : isNull,
      );
    });
  }

  test('manual409 context parses and confirm sends exactly token/consent without a context header', () async {
    final client = ApiClient()
      ..responses.add((
        409,
        {
          'error': {
            'code': 'login_confirmation_required',
            'details': loginChallengeFixture(),
          },
        },
      ))
      ..responses.add((200, signedIn()));
    final api = HttpBrokerApi(
      origin: origin,
      client: client,
      requestPcLoginReplacement: true,
    );
    addTearDown(api.close);
    LoginConfirmation? challenge;
    try {
      await api.loginInteractively('alice', 'synthetic-password');
      fail('Explicit duplicate login must require confirmation');
    } on LoginConfirmationRequired catch (pending) {
      challenge = pending.challenge;
    }
    expect(challenge, isNotNull);
    expect(challenge.existingLogin?.username, 'alice');
    await api.confirmLogin(challenge);
    expect(client.requests, hasLength(2));
    expect(
      client.requests.first.headers.values['X-Multitap-Login-Context'],
      'v1',
    );
    expect(
      client.requests.last.headers.values.containsKey(
        'X-Multitap-Login-Context',
      ),
      isFalse,
    );
    expect(jsonDecode(client.requests.last.body.toString()), {
      'challengeToken': 'synthetic_context_challenge_00001',
      'confirm': true,
    });
  });
}

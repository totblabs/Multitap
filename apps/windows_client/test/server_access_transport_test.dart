// Actual shared Windows/Android policies and HTTP probe/API with memory clients.
// No sockets, settings, credentials, platform channel or native APIs are used.
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/data/broker_transport.dart';
import 'package:multitap_client/data/host_settings_api.dart';

import 'broker_probe_test.dart' as fixture;

Map<String, Object?> health(Uri origin) => {
  'service': 'multitap-broker',
  'status': 'ok',
  'authentication': 'id-password',
  'media': 'relay-v1',
  'version': '0.5.83',
  'transport': {
    'protocol': 'broker-transport-v1',
    'publicOrigin': origin.origin,
    'scheme': origin.scheme,
    'allowInsecureHttp': origin.scheme == 'http',
    'directTls': false,
  },
};
fixture.ProbeClient client(int status, Object value) => fixture.ProbeClient(
  fixture.ProbeRequest(fixture.ProbeResponse(status, jsonEncode(value))),
);

class DenialClient extends fixture.ProbeClient {
  DenialClient(Object value)
    : super(
        fixture.ProbeRequest(fixture.ProbeResponse(403, jsonEncode(value))),
      );
  int authenticationAttempts = 0;
  @override
  Future<HttpClientRequest> openUrl(String method, Uri url) async {
    authenticationAttempts++;
    throw StateError(
      'Authentication must not start after a denied health check',
    );
  }
}

void main() {
  final http = Uri.parse('http://192.0.2.10:18400');
  final other = Uri.parse('http://192.0.2.10:18401');
  setUp(BrokerTransport.clear);
  tearDown(BrokerTransport.clear);
  test('per-peer HTTP health accepts only chosen origin, ignores alias URLs as authority', () async {
    final value = health(http)
      ..['access'] = {
        'protocol': 'server-access-v1',
        'primaryOrigin': 'https://public.example',
        'allowedOrigins': ['https://public.example', http.origin, other.origin],
      };
    final fake = client(200, value);
    final result = await probeBroker(
      http,
      httpConsentOrigin: http.origin,
      clientFactory: () => fake,
    );
    expect(result.httpApproval?.origin, http);
    expect(fake.requested, http.resolve('/healthz'));
    expect(fake.request.followRedirects, false);
    expect(
      fake.request.headers.values.containsKey(HttpHeaders.authorizationHeader),
      false,
    );
    expect(() => BrokerTransport.token(other), throwsFormatException);
    expect(
      brokerRelayMatches(Uri.parse('ws://192.0.2.10:18401/relay/x'), http),
      false,
    );
    result.httpApproval?.release();
  });
  test('old HTTP, HTTP disabled, wrong origin and malformed modern health have distinct reasons', () async {
    final valid = health(http);
    final transport = Map<String, Object?>.from(valid['transport']! as Map);
    final cases = <(Object, String)>[
      ({...valid}..remove('transport'), 'http_unsupported'),
      (
        {
          ...valid,
          'transport': {...transport, 'allowInsecureHttp': false},
        },
        'http_disabled',
      ),
      (
        {
          ...valid,
          'transport': {...transport, 'publicOrigin': other.origin},
        },
        'origin_not_allowed',
      ),
      (
        {
          ...valid,
          'transport': {...transport, 'directTls': true},
        },
        'http_profile_mismatch',
      ),
      (
        {
          ...valid,
          'transport': null,
          'access': {'protocol': 'server-access-v1'},
        },
        'http_profile_mismatch',
      ),
      (
        {
          ...valid,
          'transport': {...transport, 'allowInsecureHttp': false},
          'access': {'protocol': 'server-access-v1', 'bootstrapOnly': true},
        },
        'setup_only',
      ),
    ];
    for (final entry in cases) {
      final fake = client(200, entry.$1);
      await expectLater(
        probeBroker(
          http,
          httpConsentOrigin: http.origin,
          clientFactory: () => fake,
        ),
        throwsA(
          isA<BrokerProbeException>().having(
            (e) => e.code,
            'fixed reason',
            entry.$2,
          ),
        ),
      );
      expect(fake.closed, true);
      expect(brokerOriginAllowed(http), false);
    }
  });
  test(
    'denied server health string and nested errors never begin authentication',
    () async {
      for (final entry in [
        ({'error': 'http_disabled'}, 'http_disabled'),
        ({'error': 'origin_not_allowed'}, 'origin_not_allowed'),
        (
          {
            'error': {'code': 'unregistered_origin'},
          },
          'origin_not_allowed',
        ),
      ]) {
        BrokerTransport.restore(http, http.origin);
        final fake = DenialClient(entry.$1),
            api = HttpBrokerApi(origin: http, client: fake);
        try {
          await expectLater(
            api.desktops('synthetic-token'),
            throwsA(
              isA<BrokerException>().having(
                (e) => e.code,
                'fixed reason',
                entry.$2,
              ),
            ),
          );
          expect(fake.authenticationAttempts, 0);
          expect(fake.request.followRedirects, false);
        } finally {
          api.close();
          BrokerTransport.clear();
        }
      }
    },
  );
  test(
    'local consent is still mandatory even with exact allowed server metadata',
    () async {
      final fake = client(200, health(http));
      await expectLater(
        probeBroker(http, clientFactory: () => fake),
        throwsA(
          isA<BrokerProbeException>().having(
            (e) => e.code,
            'reason',
            'http_consent',
          ),
        ),
      );
      expect(fake.requested, isNull);
      BrokerTransport.restore(other, other.origin);
      expect(
        () => HttpBrokerApi(origin: http, client: fake),
        throwsArgumentError,
      );
    },
  );
  test(
    'HTTPS remains independent from optional HTTP/access metadata',
    () async {
      final https = Uri.parse('https://public.example:18443');
      final value = health(https)
        ..['access'] = {'protocol': 'server-access-v1', 'bootstrapOnly': true};
      value['transport'] = null;
      final fake = client(200, value);
      final result = await probeBroker(https, clientFactory: () => fake);
      expect(result.httpApproval, isNull);
      expect(fake.requested, https.resolve('/healthz'));
    },
  );
  test(
    'Windows and Android refusal/timeouts do not become old-server diagnoses',
    () {
      for (final code in [61, 111, 10061]) {
        expect(
          brokerSocketFailureCode(
            SocketException('synthetic', osError: OSError('synthetic', code)),
          ),
          'connection_refused',
        );
      }
      for (final code in [60, 110, 10060]) {
        expect(
          brokerSocketFailureCode(
            SocketException('synthetic', osError: OSError('synthetic', code)),
          ),
          'timeout',
        );
      }
      expect(
        const BrokerException('network_error', 0, 'tls').message,
        contains('HTTPS로 안전하게'),
      );
      expect(hostTransportFailure('broker_http_disabled'), contains('꺼져'));
      expect(hostTransportFailure('broker_origin_not_allowed'), contains('등록'));
      expect(hostTransportFailure('broker_setup_only'), contains('초기'));
      expect(
        hostTransportFailure('Enrollment HTTP 403: origin_not_allowed'),
        contains('등록'),
      );
    },
  );
}

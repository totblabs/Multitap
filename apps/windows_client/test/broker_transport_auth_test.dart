import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/data/broker_transport.dart';

class _Headers extends Fake implements HttpHeaders {
  final values = <String, Object>{};
  @override
  void set(String name, Object value, {bool preserveHeaderCase = false}) {
    values[name] = value;
  }

  @override
  String? value(String name) => null;
}

class _Response extends Stream<List<int>> implements HttpClientResponse {
  _Response(this.statusCode, this.body);
  @override
  final int statusCode;
  final String body;
  @override
  final _Headers headers = _Headers();
  @override
  StreamSubscription<List<int>> listen(
    void Function(List<int>)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => Stream.value(utf8.encode(body)).listen(
    onData,
    onError: onError,
    onDone: onDone,
    cancelOnError: cancelOnError,
  );
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class _Request extends Fake implements HttpClientRequest {
  _Request(this.result);
  final Future<HttpClientResponse> result;
  @override
  final _Headers headers = _Headers();
  @override
  bool followRedirects = true;
  @override
  Future<HttpClientResponse> close() => result;
  @override
  void abort([Object? exception, StackTrace? stackTrace]) {}
}

class _Client extends Fake implements HttpClient {
  _Client(this.health);
  final Future<HttpClientResponse> health;
  final order = <String>[];
  _Request? healthRequest, authRequest;
  bool closed = false;
  @override
  Duration? connectionTimeout;
  @override
  Future<HttpClientRequest> getUrl(Uri url) async {
    order.add('health:${url.origin}${url.path}');
    return healthRequest = _Request(health);
  }

  @override
  Future<HttpClientRequest> openUrl(String method, Uri url) async {
    order.add('api:$method:${url.origin}${url.path}');
    return authRequest = _Request(
      Future.value(_Response(200, '{"desktops":[]}')),
    );
  }

  @override
  void close({bool force = false}) {
    closed = true;
  }
}

final _origin = Uri.parse('http://example.test:18400');
String _health({String? publicOrigin}) => jsonEncode({
  'service': 'multitap-broker',
  'status': 'ok',
  'authentication': 'id-password',
  'media': 'relay-v1',
  'version': '0.5.77',
  'transport': {
    'protocol': 'broker-transport-v1',
    'publicOrigin': publicOrigin ?? _origin.origin,
    'scheme': 'http',
    'allowInsecureHttp': true,
    'directTls': false,
  },
});

void main() {
  setUp(BrokerTransport.clear);
  tearDown(BrokerTransport.clear);
  test('HTTP API cannot exist without exact local consent and never starts authentication', () {
    final client = _Client(Future.value(_Response(200, _health())));
    expect(
      () => HttpBrokerApi(origin: _origin, client: client),
      throwsArgumentError,
    );
    expect(client.order, isEmpty);
    expect(client.closed, isTrue);
  });
  test('actual API health admission precedes Bearer inventory and keeps redirects disabled', () async {
    BrokerTransport.restore(_origin, _origin.origin);
    final client = _Client(Future.value(_Response(200, _health())));
    final api = HttpBrokerApi(origin: _origin, client: client);
    addTearDown(api.close);
    expect(await api.desktops('fixture-token'), isEmpty);
    expect(client.order, [
      'health:http://example.test:18400/healthz',
      'api:GET:http://example.test:18400/api/v1/desktops',
    ]);
    expect(
      client.healthRequest!.headers.values.containsKey(
        HttpHeaders.authorizationHeader,
      ),
      isFalse,
    );
    expect(
      client.authRequest!.headers.values[HttpHeaders.authorizationHeader],
      'Bearer fixture-token',
    );
    expect(client.healthRequest!.followRedirects, isFalse);
    expect(client.authRequest!.followRedirects, isFalse);
  });
  test('profile mismatch prevents every Bearer request', () async {
    BrokerTransport.restore(_origin, _origin.origin);
    final client = _Client(
      Future.value(
        _Response(200, _health(publicOrigin: 'http://other.test:18400')),
      ),
    );
    final api = HttpBrokerApi(origin: _origin, client: client);
    addTearDown(api.close);
    await expectLater(
      api.desktops('fixture-token'),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'origin_not_allowed',
        ),
      ),
    );
    expect(client.authRequest, isNull);
  });
  test(
    'a health redirect is never followed and cannot authorize HTTP',
    () async {
      BrokerTransport.restore(_origin, _origin.origin);
      final client = _Client(Future.value(_Response(302, '{}')));
      final api = HttpBrokerApi(origin: _origin, client: client);
      addTearDown(api.close);
      await expectLater(
        api.desktops('fixture-token'),
        throwsA(isA<BrokerException>()),
      );
      expect(client.healthRequest!.followRedirects, isFalse);
      expect(client.authRequest, isNull);
    },
  );
  test(
    'withdrawal during held health cannot resurrect old authentication',
    () async {
      BrokerTransport.restore(_origin, _origin.origin);
      final gate = Completer<HttpClientResponse>();
      final held = _Client(gate.future),
          api = HttpBrokerApi(origin: _origin, client: held);
      addTearDown(api.close);
      final pending = api.desktops('fixture-token');
      await Future<void>.delayed(Duration.zero);
      BrokerTransport.clear();
      gate.complete(_Response(200, _health()));
      await expectLater(pending, throwsA(isA<BrokerException>()));
      expect(held.authRequest, isNull);
    },
  );
  test('observer timeout never admits a late successful HTTP probe', () async {
    final gate = Completer<HttpClientResponse>();
    final held = _Client(gate.future);
    await expectLater(
      probeBroker(
        _origin,
        httpConsentOrigin: _origin.origin,
        timeout: const Duration(milliseconds: 1),
        clientFactory: () => held,
      ),
      throwsA(
        isA<BrokerProbeException>().having((e) => e.code, 'code', 'timeout'),
      ),
    );
    gate.complete(_Response(200, _health()));
    await Future<void>.delayed(Duration.zero);
    expect(() => BrokerTransport.token(_origin), throwsFormatException);
  });
}

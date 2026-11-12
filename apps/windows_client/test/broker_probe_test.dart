import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_probe.dart';

class ProbeHeaders extends Fake implements HttpHeaders {
  final values = <String, Object>{};
  @override
  void set(String name, Object value, {bool preserveHeaderCase = false}) {
    values[name] = value;
  }
}

class ProbeResponse extends Fake implements HttpClientResponse {
  ProbeResponse(this.statusCode, this.body);
  @override
  final int statusCode;
  final String body;
  @override
  StreamSubscription<List<int>> listen(
    void Function(List<int>)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => Stream<List<int>>.value(utf8.encode(body)).listen(
    onData,
    onError: onError,
    onDone: onDone,
    cancelOnError: cancelOnError,
  );
}

class ProbeRequest extends Fake implements HttpClientRequest {
  ProbeRequest(this.response);
  final ProbeResponse response;
  @override
  final ProbeHeaders headers = ProbeHeaders();
  @override
  bool followRedirects = true;
  @override
  Future<HttpClientResponse> close() async => response;
}

class ProbeClient extends Fake implements HttpClient {
  ProbeClient(this.request, {this.error, this.pending});
  final ProbeRequest request;
  final Object? error;
  final Future<HttpClientRequest>? pending;
  Uri? requested;
  bool closed = false;
  @override
  Duration? connectionTimeout;
  @override
  Future<HttpClientRequest> getUrl(Uri url) async {
    requested = url;
    if (error != null) throw error!;
    return pending ?? request;
  }

  @override
  void close({bool force = false}) {
    closed = force;
  }
}

void main() {
  const health =
      '{"service":"multitap-broker","status":"ok","authentication":"id-password","version":"0.3.2"}';
  final origin = Uri.parse('https://vdi.example.com:18443');
  test('Connection probe checks the entered HTTPS health endpoint without credentials and closes its client', () async {
    final client = ProbeClient(ProbeRequest(ProbeResponse(200, health)));
    final result = await probeBroker(origin, clientFactory: () => client);
    expect(
      client.requested.toString(),
      'https://vdi.example.com:18443/healthz',
    );
    expect(client.request.followRedirects, isFalse);
    expect(client.request.headers.values, {
      HttpHeaders.acceptHeader: 'application/json',
      'X-Multitap-Wire': '1',
    });
    expect(result.version, '0.3.2');
    expect(result.elapsedMs, greaterThanOrEqualTo(0));
    expect(client.closed, isTrue);
  });
  test('HTTP errors and unrelated successful pages do not pass as a Multitap server', () async {
    for (final entry in [
      (502, '<html>Bad Gateway</html>', 'http'),
      (200, '<html>Some other website</html>', 'wrong_server'),
      (200, '{"service":"other-service","status":"ok"}', 'wrong_server'),
      (200, health.replaceFirst('"ok"', '"down"'), 'wrong_server'),
    ]) {
      final client = ProbeClient(
        ProbeRequest(ProbeResponse(entry.$1, entry.$2)),
      );
      await expectLater(
        probeBroker(origin, clientFactory: () => client),
        throwsA(
          isA<BrokerProbeException>().having((e) => e.code, 'code', entry.$3),
        ),
      );
      expect(client.closed, isTrue);
    }
  });
  test('DNS, TLS, connection failures and actual timeout are distinguished and closed', () async {
    for (final entry in [
      (
        const SocketException(
          'Failed host lookup',
          osError: OSError('unknown host', 11001),
        ),
        'dns',
      ),
      (const HandshakeException('certificate rejected'), 'tls'),
      (
        const SocketException(
          'connection refused',
          osError: OSError('refused', 10061),
        ),
        'connection_refused',
      ),
    ]) {
      final client = ProbeClient(
        ProbeRequest(ProbeResponse(200, health)),
        error: entry.$1,
      );
      await expectLater(
        probeBroker(origin, clientFactory: () => client),
        throwsA(
          isA<BrokerProbeException>().having((e) => e.code, 'code', entry.$2),
        ),
      );
      expect(client.closed, isTrue);
    }
    final client = ProbeClient(
      ProbeRequest(ProbeResponse(200, health)),
      pending: Completer<HttpClientRequest>().future,
    );
    await expectLater(
      probeBroker(
        origin,
        timeout: const Duration(milliseconds: 10),
        clientFactory: () => client,
      ),
      throwsA(
        isA<BrokerProbeException>().having((e) => e.code, 'code', 'timeout'),
      ),
    );
    expect(client.closed, isTrue);
  });
}

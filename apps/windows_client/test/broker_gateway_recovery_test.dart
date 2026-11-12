// Real loopback HTTP parsing only; no proxy/TLS or production outage claim.
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_transport.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/desktop_machine.dart';

import 'stream_display_entry_test.dart' show entryMachine;

Map<String, Object?> _health(Uri origin) => {
  'service': 'multitap-broker',
  'status': 'ok',
  'authentication': 'id-password',
  'media': 'relay-v1',
  'version': '0.5.92',
  'transport': {
    'protocol': 'broker-transport-v1',
    'publicOrigin': origin.origin,
    'scheme': 'http',
    'allowInsecureHttp': true,
    'directTls': false,
  },
};

/// A loopback broker whose list answers with [list]; diagnostics are kept.
Future<
  ({
    HttpBrokerApi api,
    List<Map<String, dynamic>> reports,
    Future<void> Function() close,
  })
>
_loopback(Future<void> Function(HttpRequest request) list) async {
  final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
  final origin = Uri.parse('http://127.0.0.1:${server.port}');
  final approval = BrokerTransport.approve(origin, origin.origin);
  final reports = <Map<String, dynamic>>[];
  final subscription = server.listen((request) async {
    final body = await utf8.decodeStream(request);
    final response = request.response;
    if (request.uri.path == '/healthz') {
      response.headers.contentType = ContentType.json;
      response.write(jsonEncode(_health(origin)));
    } else if (request.uri.path == '/api/v1/client/diagnostics') {
      final report = jsonDecode(body) as Map<String, dynamic>;
      reports.add(report);
      response.statusCode = 202;
      response.headers.contentType = ContentType.json;
      response.write(
        jsonEncode({
          'accepted': true,
          'protocol': report['protocol'],
          'runId': report['runId'],
          'sequence': report['sequence'],
        }),
      );
    } else {
      await list(request);
      return;
    }
    await response.close();
  });
  final api = HttpBrokerApi(origin: origin, enableDiagnostics: true);
  return (
    api: api,
    reports: reports,
    close: () async {
      api.close();
      approval.release();
      await server.close(force: true);
      await subscription.cancel();
    },
  );
}

void main() {
  setUp(BrokerTransport.clear);
  tearDown(BrokerTransport.clear);

  test('A proxy 502 on the list is diagnosed as server_restarting, not invalid_response', () async {
    final broker = await _loopback((request) async {
      request.response.statusCode = 502;
      request.response.headers.contentType = ContentType.html;
      request.response.write('<html>502 Bad Gateway</html>');
      await request.response.close();
    });
    try {
      await expectLater(
        broker.api.desktops('fixture-only'),
        throwsA(
          isA<BrokerException>()
              .having((e) => e.code, 'code', 'server_restarting')
              .having((e) => e.status, 'status', 502)
              .having((e) => e.serverRestarting, 'restarting', isTrue),
        ),
      );
      await broker.api.flushDiagnostics();
      final rows = [
        for (final report in broker.reports)
          for (final row in report['events'] as List)
            Map<String, dynamic>.from(row as Map),
      ];
      final inventory = rows.singleWhere((row) => row['event'] == 'inventory');
      expect(inventory['code'], 'server_restarting');
      expect((inventory['values'] as Map)['stage'], 0);
      expect((inventory['values'] as Map)['error'], 502);
      expect(jsonEncode(broker.reports), isNot(contains('invalid_response')));
    } finally {
      await broker.close();
    }
  });

  test('A broker that goes away mid-request is a restart too', () async {
    final broker = await _loopback((request) async {
      final socket = await request.response.detachSocket(writeHeaders: false);
      socket.destroy();
    });
    try {
      await expectLater(
        broker.api.desktops('fixture-only'),
        throwsA(
          isA<BrokerException>()
              .having((e) => e.code, 'code', 'network_error')
              .having((e) => e.reason, 'reason', 'connection_reset')
              .having((e) => e.serverRestarting, 'restarting', isTrue),
        ),
      );
    } finally {
      await broker.close();
    }
  });

  Future<void> responseCase(
    int status,
    String body, {
    String code = 'invalid_response',
    bool logout = false,
    bool recover = false,
  }) async {
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    final origin = Uri.parse('http://127.0.0.1:${server.port}');
    final approval = BrokerTransport.approve(origin, origin.origin);
    final requests = <String>[];
    final subscription = server.listen((request) async {
      requests.add('${request.method} ${request.uri.path}');
      await request.drain<void>();
      if (request.uri.path == '/healthz') {
        expect(request.headers.value(HttpHeaders.authorizationHeader), isNull);
        request.response.headers.contentType = ContentType.json;
        request.response.write(jsonEncode(_health(origin)));
        await request.response.close();
        return;
      }
      final recovered = recover && requests.length > 2;
      request.response.statusCode = recovered ? 200 : status;
      request.response.headers.contentType = recovered
          ? ContentType.json
          : ContentType.html;
      request.response.write(recovered ? '{"desktops":[]}' : body);
      await request.response.close();
    });
    final api = HttpBrokerApi(origin: origin);
    try {
      if (recover) {
        final ready = await waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async {
            await api.desktops('fixture-only');
            return entryMachine(1);
          },
          current: () => true,
          recovering: true,
          refreshFirst: true,
          retryGatewayErrors: true,
          maxWait: const Duration(seconds: 3),
        );
        expect(ready.id, entryMachine(1).id);
      } else {
        await expectLater(
          logout ? api.logout('fixture-only') : api.desktops('fixture-only'),
          throwsA(
            isA<BrokerException>()
                .having((error) => error.code, 'code', code)
                .having((error) => error.status, 'HTTP status', status),
          ),
        );
      }
      expect(requests, [
        'GET /healthz',
        logout ? 'POST /api/v1/auth/logout' : 'GET /api/v1/desktops',
        if (recover) 'GET /api/v1/desktops',
      ], reason: 'Only the opted-in read-only recovery can repeat GET.');
    } finally {
      api.close();
      approval.release();
      await server.close(force: true);
      await subscription.cancel();
    }
  }

  for (final status in [502, 503, 504]) {
    test('Actual HTTP$status recovers through the read-only helper', () async {
      await responseCase(status, '<html>Unavailable</html>', recover: true);
    });
    for (final body in ['', '<html>Gateway temporarily unavailable</html>']) {
      test(
        'Actual HTTP $status ${body.isEmpty ? 'empty' : 'HTML'} is a server restart with its status',
        () async {
          await responseCase(status, body, code: 'server_restarting');
        },
      );
    }
  }
  test('A proxy HTTP500 page stays invalid_response', () async {
    await responseCase(500, '<html>Internal Server Error</html>');
  });
  test('A broker JSON HTTP502 keeps its own code', () async {
    await responseCase(
      502,
      jsonEncode({
        'error': {'code': 'proxmox_unreachable'},
      }),
      code: 'proxmox_unreachable',
    );
  });
  for (final body in ['<html>malformed success</html>', '[]']) {
    test(
      'Malformed HTTP200 ${body.startsWith('<') ? 'HTML' : 'JSON shape'} stays invalid_response',
      () async {
        await responseCase(200, body);
      },
    );
  }
  for (final item in [
    (401, 'session_expired'),
    (403, 'desktop_not_found'),
    (503, 'invalid_stream_info'),
  ]) {
    test('Actual HTTP ${item.$1} preserves authoritative ${item.$2}', () async {
      await responseCase(
        item.$1,
        jsonEncode({
          'error': {'code': item.$2},
        }),
        code: item.$2,
      );
    });
  }
  test('A POST receiving HTML503 is sent once and never replayed', () async {
    await responseCase(
      503,
      '<html>Unavailable</html>',
      logout: true,
      code: 'server_restarting',
    );
  });

  // A recovering connection waits out a lost
  // network path (no DNS answer, a timeout) the way it waits out a restart;
  // a first preparation still reports it at once.
  group('network outage during recovery', () {
    for (final reason in ['dns', 'timeout', 'network']) {
      test('network_error/$reason is retried while recovering', () async {
        var reads = 0;
        final ready = await waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async {
            if (++reads < 3) throw BrokerException('network_error', 0, reason);
            return entryMachine(1);
          },
          current: () => true,
          recovering: true,
          refreshFirst: true,
          retryGatewayErrors: true,
          maxWait: const Duration(seconds: 5),
        );
        expect(ready.id, entryMachine(1).id);
        expect(reads, 3);
      });
    }
    test('a first preparation is not held by a network failure', () async {
      await expectLater(
        waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async => throw const BrokerException('network_error', 0, 'dns'),
          current: () => true,
          recovering: false,
          refreshFirst: true,
          retryGatewayErrors: true,
          maxWait: const Duration(seconds: 5),
        ),
        throwsA(isA<BrokerException>().having((e) => e.reason, 'reason', 'dns')),
      );
    });
  });

  // Windows restarting inside the VM keeps the
  // Host away past the ordinary recovery wait.
  group('Host away with the VM running', () {
    DesktopMachine away({String power = 'running'}) => DesktopMachine(
      id: entryMachine(1).id,
      name: 'TESTER',
      description: '',
      kind: MachineKind.vm,
      status: MachineStatus.awaitingHost,
      specification: 'Windows',
      accent: Colors.blue,
      hostOnline: false,
      powerState: power,
      powerStale: false,
    );

    test('extends the wait past maxWait and tells once', () async {
      var reads = 0, told = 0;
      final ready = await waitForStreamDisplayControl(
        entryMachine(1),
        refresh: () async => ++reads < 6 ? away() : entryMachine(1),
        current: () => true,
        recovering: true,
        refreshFirst: true,
        retryGatewayErrors: true,
        maxWait: const Duration(milliseconds: 700),
        hostAwayWait: const Duration(seconds: 10),
        onHostAway: () => told++,
      );
      expect(ready.id, entryMachine(1).id);
      expect(reads, 6, reason: 'about 2.5 s: past the 0.7 s maxWait');
      expect(told, 1);
    });

    test('a VM that was shut down is not waited for', () async {
      var reads = 0;
      final watch = Stopwatch()..start();
      await expectLater(
        waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async => ++reads < 2 ? away() : away(power: 'stopped'),
          current: () => true,
          recovering: true,
          refreshFirst: true,
          retryGatewayErrors: true,
          maxWait: const Duration(seconds: 30),
          hostAwayWait: const Duration(minutes: 3),
        ),
        completes,
      );
      expect(watch.elapsed, lessThan(const Duration(seconds: 5)));
    });

    test('without hostAwayWait the ordinary bound still ends it', () async {
      await expectLater(
        waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async => away(),
          current: () => true,
          recovering: true,
          refreshFirst: true,
          retryGatewayErrors: true,
          maxWait: const Duration(milliseconds: 700),
        ),
        throwsA(isA<BrokerException>().having((e) => e.code, 'code', 'host_offline')),
      );
    });
  });
}
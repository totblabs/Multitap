// Private fake HTTP and in-memory diagnostics only. No socket, native engine,
// clipboard, user file, account or product/driver process is accessed.
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/product_log.dart';

import 'power_api_test.dart'
    show ApiClient, ApiHeaders, ApiRequest, ApiResponse;

const _oldSession = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa';
const _newSession = 'cccccccc-cccc-cccc-cccc-cccccccccccc';
const _newRequest = 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb';
const _oldRequest = 'dddddddd-dddd-dddd-dddd-dddddddddddd';
const _desktop = 'fixture-vm';
const _token = 'PRIVATE_SYNTHETIC_TOKEN';

class _Reply {
  const _Reply({
    this.sessionId,
    this.requestId = _newRequest,
    this.code = 'desktop_busy',
    this.networkFailure = false,
    this.reason,
  });
  final String? sessionId, requestId, reason;
  final String code;
  final bool networkFailure;
}

class _Headers extends ApiHeaders {
  _Headers(this.requestId);
  final String? requestId;
  @override
  String? value(String name) => name == 'x-request-id' ? requestId : null;
}

class _Response extends ApiResponse {
  _Response(super.statusCode, super.value, String? requestId)
    : headers = _Headers(requestId);
  @override
  final _Headers headers;
}

class _Request extends ApiRequest {
  _Request(String method, Uri uri, this.owner)
    : super(method, uri, ApiResponse(200, {}));
  final _Client owner;
  @override
  void abort([Object? exception, StackTrace? stackTrace]) {}

  @override
  Future<HttpClientResponse> close() async {
    if (uri.path == '/api/v1/client/diagnostics') {
      final report = jsonDecode(body.toString()) as Map<String, dynamic>;
      owner.reports.add(report);
      return _Response(202, {
        'accepted': true,
        'protocol': report['protocol'],
        'runId': report['runId'],
        'sequence': report['sequence'],
      }, null);
    }
    if (!uri.path.endsWith('/connect')) {
      throw StateError('Unexpected fake route');
    }
    final reply = owner.replies.removeAt(0);
    if (reply.networkFailure) throw const SocketException('Synthetic failure');
    final id = reply.sessionId;
    return _Response(
      id == null ? 409 : 200,
      id == null
          ? {
              'error': {
                'code': reply.code,
                if (reply.reason != null) 'details': {'reason': reply.reason},
              },
            }
          : {
              'sessionId': id,
              'webSocketUrl': 'wss://configured.invalid/api/v1/sessions/$id',
              'mode': 'stream',
            },
      reply.requestId,
    );
  }
}

class _Client extends ApiClient {
  final replies = <_Reply>[];
  final reports = <Map<String, dynamic>>[];
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    final request = _Request(method, uri, this);
    requests.add(request);
    return request;
  }

  List<Map<String, dynamic>> get events => [
    for (final report in reports)
      for (final row in report['events'] as List)
        Map<String, dynamic>.from(row as Map),
  ];
  List<Map<String, dynamic>> get admissions => events
      .where(
        (row) =>
            row['event'] == 'brokerRequest' &&
            (row['values'] as Map)['stage'] == 3,
      )
      .toList();
}

HttpBrokerApi _api(_Client client) => HttpBrokerApi(
  origin: Uri.parse('https://configured.invalid'),
  client: client,
  enableDiagnostics: true,
  diagnosticsDesktopId: _desktop,
  diagnosticsMode: 'stream',
);

void main() {
  test('A supported host reason survives; other reason labels preserve the grouped failure', () async {
    for (final reason in ['windows_session_locked', 'network', 'PRIVATE_REASON']) {
      final client = _Client()
        ..replies.add(_Reply(code: 'network_error', reason: reason));
      final api = _api(client);
      try {
        await expectLater(api.connect(_token, _desktop), throwsA(isA<BrokerException>()));
        await api.flushDiagnostics();
        expect(client.admissions.single['code'],
            reason == 'windows_session_locked' ? reason : 'network_error');
        expect(jsonEncode(client.reports), isNot(contains('PRIVATE_REASON')));
      } finally {
        api.close();
      }
    }
  });

  for (final network in [false, true]) {
    test(
      'Same-VM failed ${network ? 'transport' : 'HTTP'} reconnect is request-scoped and preserves old session events',
      () async {
        final client = _Client()
          ..replies.addAll([
            const _Reply(sessionId: _oldSession, requestId: _oldRequest),
            _Reply(networkFailure: network),
          ]);
        final api = _api(client);
        addTearDown(api.close);
        await api.connect(_token, _desktop);
        await expectLater(
          api.connect(_token, _desktop),
          throwsA(isA<BrokerException>()),
        );
        ProductLog.instance.record(LogEvent.clipboard, {
          'ok': false,
          'stage': 3,
        });
        await api.flushDiagnostics();
        final failed = client.admissions.last;
        expect(client.admissions.first['requestId'], _oldRequest);
        expect(failed['values']['ok'], isFalse);
        expect(failed['desktopId'], _desktop);
        expect(
          failed.containsKey('sessionId'),
          isFalse,
          reason:
              'A new failed admission did not establish the previous session',
        );
        expect(failed['requestId'], network ? isNull : _newRequest);
        expect(failed['code'], network ? 'network_error' : 'desktop_busy');
        expect(client.events.last['event'], 'clipboard');
        expect(
          client.events.last['sessionId'],
          _oldSession,
          reason: 'Request scoping must not clear the existing session binding',
        );
        expect(jsonEncode(client.reports), isNot(contains(_token)));
      },
    );
  }

  test(
    'A failed different-VM admission never inherits another desktop session',
    () async {
      final client = _Client()
        ..replies.addAll([
          const _Reply(sessionId: _oldSession, requestId: _oldRequest),
          const _Reply(),
        ]);
      final api = _api(client);
      addTearDown(api.close);
      await api.connect(_token, _desktop);
      await expectLater(
        api.connect(_token, 'fixture-other'),
        throwsA(isA<BrokerException>()),
      );
      await api.flushDiagnostics();
      final failed = client.admissions.last;
      expect(failed['desktopId'], 'fixture-other');
      expect(failed.containsKey('sessionId'), isFalse);
      expect(failed['requestId'], _newRequest);
    },
  );

  test('Each successful same-VM admission reports its own session and retains earlier row ownership', () async {
    final client = _Client()
      ..replies.addAll([
        const _Reply(sessionId: _oldSession, requestId: _oldRequest),
        const _Reply(sessionId: _newSession),
      ]);
    final api = _api(client);
    addTearDown(api.close);
    await api.connect(_token, _desktop);
    await api.connect(_token, _desktop);
    ProductLog.instance.record(LogEvent.clipboard, {'ok': true});
    await api.flushDiagnostics();
    expect(client.admissions.map((row) => row['sessionId']), [
      _oldSession,
      _newSession,
    ]);
    expect(client.events.last['sessionId'], _newSession);
  });

  test('Legacy non-UUID response and invalid request IDs never borrow the previous UUID or bypass privacy projection', () async {
    final client = _Client()
      ..replies.addAll([
        const _Reply(sessionId: _oldSession, requestId: _oldRequest),
        const _Reply(
          sessionId: 'PRIVATE-NONUUID',
          requestId: 'PRIVATE-REQUEST',
        ),
        const _Reply(code: 'PRIVATE-FAILURE', requestId: 'PRIVATE-REQUEST'),
      ]);
    final api = _api(client);
    addTearDown(api.close);
    await api.connect(_token, _desktop);
    await api.connect(_token, _desktop);
    await expectLater(
      api.connect(_token, _desktop),
      throwsA(isA<BrokerException>()),
    );
    ProductLog.instance.record(LogEvent.clipboard, {
      'ok': false,
      'token': 'PRIVATE-PAYLOAD',
      'filename': 'PRIVATE-PAYLOAD',
      'error': 'PRIVATE-PAYLOAD',
    });
    await api.flushDiagnostics();
    for (final row in client.admissions.skip(1)) {
      expect(row.containsKey('sessionId'), isFalse);
      expect(row.containsKey('requestId'), isFalse);
      expect(
        (row['values'] as Map).values.every(
          (value) => value is num || value is bool,
        ),
        isTrue,
      );
    }
    expect(client.admissions.last['code'], 'unknown_failure');
    expect(client.events.last['sessionId'], _oldSession);
    expect(client.events.last['values'], {'ok': false});
    expect(jsonEncode(client.reports), isNot(contains('PRIVATE')));
  });
}

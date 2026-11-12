import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';

import 'power_api_test.dart' show ApiClient, taskId;

Map<String, dynamic> displayResponse({
  String id = taskId,
  String status = 'pending',
  int active = 2,
  bool stale = true,
  bool busy = true,
}) => {
  'display': {
    'supported': true,
    'activeCount': active,
    'checkedAt': '2026-10-22T13:00:00Z',
    'stale': stale,
    'busy': busy,
    'error': null,
    'operation': {
      'id': id,
      'status': status,
      'requestedCount': 1,
      'submittedAt': '2026-10-22T13:00:00Z',
      'checkedAt': '2026-10-22T13:00:00Z',
      'error': null,
    },
  },
};

void main() {
  test('Desktop JSON requires explicit runtime control capability and retains the observed display state', () {
    final json = <String, dynamic>{
      'id': 'assigned-vm',
      'name': 'VM',
      'kind': 'vm',
      'description': '',
      'os': 'Windows',
      'status': 'ready',
      'canConnect': true,
      'display': displayResponse(
        status: 'succeeded',
        active: 1,
        stale: false,
        busy: false,
      )['display'],
      'host': {'connected': true, 'capabilities': <String, dynamic>{}},
    };
    expect(BrokerDesktop.fromJson(json).virtualMonitorControl, isFalse);
    (json['host'] as Map)['capabilities']['virtualMonitorControl'] = true;
    final desktop = BrokerDesktop.fromJson(json);
    expect(desktop.virtualMonitorControl, isTrue);
    expect(desktop.display.activeCount, 1);
    expect(desktop.display.applied(1, taskId), isTrue);
    final bad = displayResponse(
      status: 'succeeded',
      active: 1,
      stale: false,
      busy: false,
    );
    (bad['display'] as Map)['operation']['error'] = 'display_count_mismatch';
    expect(
      VirtualDisplayState.fromJson(bad['display'] as Map<String, dynamic>)
          .applied(1, taskId),
      isFalse,
    );
  });
  test('Display API sends only count and polls the exact opaque operation with bearer auth', () async {
    final client = ApiClient()
      ..responses.addAll([
        (202, displayResponse()),
        (
          200,
          displayResponse(
            status: 'succeeded',
            active: 1,
            stale: false,
            busy: false,
          ),
        ),
      ]);
    final api = HttpBrokerApi(
      origin: Uri.parse('https://cloud.example.com'),
      client: client,
    );
    final pending = await api.configureDisplays(
      'fixture-token',
      'assigned-vm',
      1,
    );
    expect(pending.applied(1, taskId), isFalse);
    final applied = await api.display(
      'fixture-token',
      'assigned-vm',
      operationId: pending.operation!.id,
    );
    expect(applied.applied(1, taskId), isTrue);
    expect(
      client.requests.first.uri.path,
      '/api/v1/desktops/assigned-vm/display',
    );
    expect(client.requests.first.method, 'POST');
    expect(jsonDecode(client.requests.first.body.toString()), {'count': 1});
    expect(
      client.requests.last.uri.path,
      '/api/v1/desktops/assigned-vm/display/operations/$taskId',
    );
    for (final request in client.requests) {
      expect(
        request.headers.values[HttpHeaders.authorizationHeader],
        'Bearer fixture-token',
      );
      expect(request.uri.hasQuery, isFalse);
      expect(request.followRedirects, isFalse);
    }
    for (final count in [0, 3, -1]) {
      await expectLater(
        api.configureDisplays('fixture-token', 'assigned-vm', count),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'invalid_display_count',
          ),
        ),
      );
    }
    await expectLater(
      api.display('fixture-token', 'assigned-vm', operationId: '../other'),
      throwsA(isA<BrokerException>()),
    );
    expect(client.requests, hasLength(2));
  });
  test('Command acceptance, unknown, stale or mismatched observations never count as applied', () async {
    for (final response in [
      displayResponse(),
      displayResponse(status: 'unknown'),
      displayResponse(status: 'succeeded', active: 1),
      displayResponse(
        status: 'succeeded',
        active: 2,
        stale: false,
        busy: false,
      ),
    ]) {
      final state = VirtualDisplayState.fromJson(
        response['display'] as Map<String, dynamic>,
      );
      expect(state.applied(1, taskId), isFalse);
    }
    final client = ApiClient()
      ..responses.add((
        200,
        displayResponse(id: '6f140094-f814-4a0a-8858-9744018458a7'),
      ));
    final api = HttpBrokerApi(
      origin: Uri.parse('https://cloud.example.com'),
      client: client,
    );
    await expectLater(
      api.display('fixture-token', 'assigned-vm', operationId: taskId),
      throwsA(
        isA<BrokerException>().having(
          (e) => e.code,
          'code',
          'invalid_response',
        ),
      ),
    );
    expect(
      () => VirtualDisplayState.fromJson({'activeCount': 3}),
      throwsFormatException,
    );
    final malformed = displayResponse();
    (malformed['display'] as Map)['operation']['requestedCount'] = 1.0;
    expect(
      () => VirtualDisplayState.fromJson(
        malformed['display'] as Map<String, dynamic>,
      ),
      throwsFormatException,
    );
  });
}

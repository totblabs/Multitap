import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';

const taskId = '614c03e4-5595-4d77-a075-f6a61cfc4e8b';
Map<String, dynamic> powerResponse({
  String task = taskId,
  String desktop = 'qa-vm',
  String state = 'stopped',
  String status = 'submitted',
  String? exitStatus,
}) => {
  'power': {
    'state': state,
    'checkedAt': '2026-10-22T12:00:00Z',
    'stale': false,
    'error': null,
  },
  'powerTask': {
    'id': task,
    'desktopId': desktop,
    'action': 'start',
    'status': status,
    'exitStatus': exitStatus,
    'submittedAt': '2026-10-22T12:00:00Z',
    'checkedAt': '2026-10-22T12:00:00Z',
  },
};

class ApiHeaders extends Fake implements HttpHeaders {
  final values = <String, Object>{};
  @override
  ContentType? contentType;
  @override
  void set(String name, Object value, {bool preserveHeaderCase = false}) {
    values[name] = value;
  }
}

class ApiResponse extends Fake implements HttpClientResponse {
  ApiResponse(this.statusCode, this.value);
  @override
  final int statusCode;
  final Map<String, dynamic> value;
  @override
  Stream<List<int>> timeout(
    Duration timeLimit, {
    void Function(EventSink<List<int>>)? onTimeout,
  }) =>
      Stream<List<int>>.value(utf8.encode(jsonEncode(value)))
          .timeout(timeLimit, onTimeout: onTimeout);
  @override
  StreamSubscription<List<int>> listen(
    void Function(List<int>)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => Stream<List<int>>.value(utf8.encode(jsonEncode(value))).listen(
    onData,
    onError: onError,
    onDone: onDone,
    cancelOnError: cancelOnError,
  );
}

class ApiRequest extends Fake implements HttpClientRequest {
  ApiRequest(this.method, this.uri, this.response);
  @override
  final String method;
  @override
  final Uri uri;
  final ApiResponse response;
  @override
  final ApiHeaders headers = ApiHeaders();
  @override
  bool followRedirects = true;
  final body = StringBuffer();
  @override
  void write(Object? value) => body.write(value);
  @override
  Future<HttpClientResponse> close() async => response;
}

class ApiClient extends Fake implements HttpClient {
  final responses = <(int, Map<String, dynamic>)>[];
  final requests = <ApiRequest>[];
  @override
  Duration? connectionTimeout;
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    final response = responses.removeAt(0),
        request = ApiRequest(
          method,
          uri,
          ApiResponse(response.$1, response.$2),
        );
    requests.add(request);
    return request;
  }

  @override
  void close({bool force = false}) {}
}

void main() {
  test('Power submits bounded VM actions and polls the opaque task through bearer-authenticated endpoints', () async {
    final client = ApiClient()
      ..responses.addAll([
        (202, powerResponse()),
        (
          200,
          powerResponse(state: 'running', status: 'stopped', exitStatus: 'OK'),
        ),
      ]);
    final api = HttpBrokerApi(
      origin: Uri.parse('https://cloud.example.com'),
      client: client,
    );
    final requested = await api.requestPower('qa-token', 'qa-vm', 'start');
    expect(requested.task!.isActive, isTrue);
    final complete = await api.power(
      'qa-token',
      'qa-vm',
      taskId: requested.task!.id,
    );
    expect(complete.task!.exitStatus, 'OK');
    expect(complete.power.state, 'running');
    expect(client.requests.first.uri.path, '/api/v1/desktops/qa-vm/power');
    expect(client.requests.first.method, 'POST');
    expect(jsonDecode(client.requests.first.body.toString()), {
      'action': 'start',
    });
    expect(
      client.requests.last.uri.path,
      '/api/v1/desktops/qa-vm/power/tasks/$taskId',
    );
    for (final request in client.requests) {
      expect(
        request.headers.values[HttpHeaders.authorizationHeader],
        'Bearer qa-token',
      );
      expect(request.followRedirects, isFalse);
      expect(request.uri.hasQuery, isFalse);
    }
    for (final action in [
      'reset',
      'suspend',
      'resume',
      'pause',
      'destroy',
      'invalid',
    ]) {
      await expectLater(
        api.requestPower('qa-token', 'qa-vm', action),
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'invalid_power_action',
          ),
        ),
      );
    }
    expect(client.requests, hasLength(2));
  });
  test('stop (정지) is submitted as the action stop; its tasks and the stop permission decode', () async {
    final response = powerResponse(state: 'running');
    (response['powerTask'] as Map)['action'] = 'stop';
    response['canForceStop'] = false;
    final client = ApiClient()
      ..responses.addAll([
        (200, {...response, 'canForceStop': true}),
        (202, response),
      ]);
    final api = HttpBrokerApi(
      origin: Uri.parse('https://cloud.example.com'),
      client: client,
    );
    final before = await api.power('fixture-token', 'qa-vm');
    expect(before.canForceStop, isTrue);
    expect(client.requests.first.uri.path, '/api/v1/desktops/qa-vm/power');
    expect(client.requests.first.method, 'GET');
    final result = await api.requestPower('fixture-token', 'qa-vm', 'stop');
    expect(result.task!.action, 'stop');
    expect(result.canForceStop, isFalse);
    expect(client.requests.last.uri.path, '/api/v1/desktops/qa-vm/power');
    expect(client.requests.last.method, 'POST');
    expect(jsonDecode(client.requests.last.body.toString()), {
      'action': 'stop',
    });
    expect(client.requests, hasLength(2));
    for (final request in client.requests) {
      expect(
        request.headers.values[HttpHeaders.authorizationHeader],
        'Bearer fixture-token',
      );
    }
    expect(powerActionCodes['stop'], 4);
    expect(clientVmPowerActions, {'start', 'shutdown', 'reboot', 'stop'});
    for (final invalid in [
      'true',
      1,
      {'value': true},
    ]) {
      expect(
        () => BrokerPowerResult.fromJson({
          ...response,
          'canForceStop': invalid,
        }, desktopId: 'qa-vm'),
        throwsA(isA<BrokerException>()),
      );
    }
    final superseded = powerResponse(status: 'superseded');
    (superseded['powerTask'] as Map)['action'] = 'shutdown';
    final oldTask = BrokerPowerResult.fromJson(
      superseded,
      desktopId: 'qa-vm',
    ).task!;
    expect(oldTask.isActive, isFalse);
    expect(oldTask.exitStatus, isNull);
  });
  test('Task ownership identifiers and unknown result states are validated without claiming success', () {
    expect(
      () => BrokerPowerResult.fromJson(
        powerResponse(desktop: 'another-vm'),
        desktopId: 'qa-vm',
      ),
      throwsA(isA<BrokerException>()),
    );
    expect(
      () => BrokerPowerResult.fromJson(
        powerResponse(task: 'UPID:private-node:task'),
        desktopId: 'qa-vm',
      ),
      throwsA(isA<BrokerException>()),
    );
    expect(
      () => BrokerPowerResult.fromJson(
        powerResponse(),
        desktopId: 'qa-vm',
        taskId: 'different',
      ),
      throwsA(isA<BrokerException>()),
    );
    final unknown = BrokerPowerResult.fromJson(
      powerResponse(status: 'unknown'),
      desktopId: 'qa-vm',
    );
    expect(unknown.task!.status, 'unknown');
    expect(unknown.task!.exitStatus, isNull);
  });
  test('Connect API retains only a safe static reason and never echoes server text or secrets', () async {
    final client = ApiClient()
      ..responses.add((
        409,
        {
          'error': {
            'code': 'host_not_ready',
            'details': {'reason': 'virtual_display_required'},
            'message': 'private-token-do-not-display',
          },
        },
      ));
    final api = HttpBrokerApi(
      origin: Uri.parse('https://cloud.example.com'),
      client: client,
    );
    await expectLater(
      api.connect('qa-token', 'qa-vm'),
      throwsA(
        isA<BrokerException>()
            .having((e) => e.reason, 'reason', 'virtual_display_required')
            .having((e) => e.message, 'message', contains('가상 화면'))
            .having(
              (e) => e.toString(),
              'safe text',
              isNot(contains('private-token')),
            ),
      ),
    );
    client.responses.add((
      409,
      {
        'error': {
          'code': 'host_not_ready',
          'details': {'reason': 'Bearer private-token https://private/'},
        },
      },
    ));
    await expectLater(
      api.connect('qa-token', 'qa-vm'),
      throwsA(
        isA<BrokerException>()
            .having((e) => e.reason, 'reason', isNull)
            .having(
              (e) => e.message,
              'safe text',
              isNot(contains('private-token')),
            ),
      ),
    );
  });
}

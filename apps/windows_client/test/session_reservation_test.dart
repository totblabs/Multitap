import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/client_diagnostics.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/session_window.dart';

import 'auth_flow_test.dart' show TestBroker;
import 'client_diagnostics_test.dart' show DiagnosticResponse;
import 'console_resume_flow_test.dart' show desktop;
import 'power_api_test.dart' show ApiClient, ApiRequest, taskId;
import 'remote_session_test.dart' show SessionSocket, SessionMedia;
import 'session_window_console_resume_test.dart' show launch;
import 'session_windows_test.dart' show Guard;

final origin = Uri.parse('https://fixture.example');
Map<String, dynamic> reply({Object? protocol = 'session-cancel-v1'}) => {
  'sessionId': taskId,
  'webSocketUrl': 'wss://fixture.example/api/v1/sessions/$taskId',
  'sessionCancellationProtocol': ?protocol,
};

BrokerConnection reserved(Future<void> Function() cancel) => BrokerConnection(
  sessionId: taskId,
  webSocketUrl: Uri.parse('wss://fixture.example/api/v1/sessions/$taskId'),
  reservation: BrokerSessionReservation(cancel),
);

class HeaderRequest extends ApiRequest {
  HeaderRequest(super.method, super.uri, super.response);
  @override
  Future<HttpClientResponse> close() async =>
      DiagnosticResponse(response.statusCode, response.value);
}

class CancelClient extends ApiClient {
  bool closed = false;
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    if (closed) throw StateError('Client closed');
    final next = responses.removeAt(0),
        request = HeaderRequest(
          method,
          uri,
          DiagnosticResponse(next.$1, next.$2),
        );
    requests.add(request);
    return request;
  }

  @override
  void close({bool force = false}) {
    closed = true;
  }
}

class PendingBroker extends TestBroker {
  final response = Completer<BrokerConnection>();
  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
  }) {
    connects++;
    return response.future;
  }
}

void main() {
  test(
    'a late cleanup diagnostic retains the retired session correlation',
    () async {
      final log = ProductLog(), sent = <Map<String, Object?>>[];
      final reporter = ClientDiagnostics(
        desktopId: 'qa-vm',
        mode: 'stream',
        log: log,
        send: (batch) async {
          sent.add(batch);
          return DiagnosticDelivery.accepted;
        },
      );
      addTearDown(() {
        reporter.close();
        log.dispose();
      });
      reporter.bindSession('11111111-1111-1111-1111-111111111111');
      reporter.record(
        'brokerRequest',
        sessionId: taskId,
        values: {'ok': true, 'stage': 4},
      );
      await reporter.flush();
      final event = (sent.single['events'] as List).cast<Map>().singleWhere(
        (e) => e['event'] == 'brokerRequest',
      );
      expect(event['sessionId'], taskId);
      expect(event['desktopId'], 'qa-vm');
    },
  );
  test('negotiated cleanup is same-origin authenticated DELETE and survives closing the API', () async {
    final client = CancelClient()..responses.add((200, reply())),
        cleanup = CancelClient()
          ..responses.add((200, {'sessionId': taskId, 'released': true}));
    final api = HttpBrokerApi(
      origin: origin,
      client: client,
      cleanupClientFactory: () => cleanup,
    );
    final connection = await api.connect('own-login-token', 'qa-vm');
    api.close();
    await Future.wait([connection.release(), connection.release()]);
    expect(cleanup.requests.length, 1);
    final request = cleanup.requests.single;
    expect(request.method, 'DELETE');
    expect(request.uri, origin.resolve('/api/v1/sessions/$taskId'));
    expect(
      request.headers.values[HttpHeaders.authorizationHeader],
      'Bearer own-login-token',
    );
    expect(request.followRedirects, isFalse);
    expect(request.body.toString(), isEmpty);
    expect(cleanup.closed, isTrue);
  });
  test(
    'legacy replies never probe unsupported cancellation endpoints',
    () async {
      var cancellations = 0;
      final connection = BrokerConnection.fromJson(
        reply(protocol: null),
        origin,
        cancel: (_) async {
          cancellations++;
        },
      );
      await connection.release();
      expect(cancellations, 0);
      for (final value in [true, 'unknown', {}, 1]) {
        expect(
          () => BrokerConnection.fromJson(reply(protocol: value), origin),
          throwsA(isA<BrokerException>()),
        );
      }
    },
  );
  test('cancellation rejects mismatched acknowledgement and never follows a redirect', () async {
    for (final acknowledgement in [
      (200, <String, dynamic>{'sessionId': taskId, 'released': 'true'}),
      (
        200,
        <String, dynamic>{
          'sessionId': '11111111-1111-1111-1111-111111111111',
          'released': true,
        },
      ),
      (302, <String, dynamic>{'location': 'https://foreign.invalid'}),
    ]) {
      final client = CancelClient()..responses.add((200, reply())),
          cleanup = CancelClient()..responses.add(acknowledgement);
      final api = HttpBrokerApi(
        origin: origin,
        client: client,
        cleanupClientFactory: () => cleanup,
      );
      final connection = await api.connect('own-token', 'qa-vm');
      await expectLater(connection.release(), throwsA(isA<BrokerException>()));
      expect(cleanup.requests.single.followRedirects, isFalse);
      expect(cleanup.closed, isTrue);
      api.close();
    }
  });
  test('failed release never automatically retries or releases another reservation', () async {
    var calls = 0;
    final connection = reserved(() async {
      calls++;
      throw const BrokerException('network_error');
    });
    await expectLater(connection.release(), throwsA(isA<BrokerException>()));
    await expectLater(connection.release(), throwsA(isA<BrokerException>()));
    expect(calls, 1);
  });
  test('an explicit retry after a failed release tries once more; a '
      'successful release is never repeated', () async {
    var calls = 0, fail = true;
    final connection = reserved(() async {
      calls++;
      if (fail) throw const BrokerException('network_error');
    });
    await expectLater(connection.release(), throwsA(isA<BrokerException>()));
    fail = false;
    await connection.releaseAgain();
    expect(calls, 2);
    await connection.releaseAgain();
    await connection.release();
    expect(calls, 2);
  });  testWidgets(
    'reservation cleanup is bounded even if its transport never completes',
    (t) async {
      final blocked = Completer<void>(),
          connection = reserved(() => blocked.future);
      final done = expectLater(
        connection.release(),
        throwsA(isA<TimeoutException>()),
      );
      await t.pump(const Duration(seconds: 4));
      await done;
      blocked.complete();
      await t.pump();
    },
  );
  test('closing an allocated stream before start releases it without dialing or decoding', () async {
    var cancelled = 0, dials = 0;
    final media = SessionMedia();
    final session = RemoteSession(
      connection: reserved(() async {
        cancelled++;
      }),
      token: 'fixture',
      media: media,
      dialer: (_, _) async {
        dials++;
        return SessionSocket();
      },
    );
    await session.close();
    await session.start();
    expect(cancelled, 1);
    expect(dials, 0);
    expect(media.starts, 0);
  });
  test('closing during stream WebSocket dial cancels immediately and retires its late socket', () async {
    var cancelled = 0;
    final dial = Completer<WebSocket>(),
        socket = SessionSocket(),
        media = SessionMedia();
    final session = RemoteSession(
      connection: reserved(() async {
        cancelled++;
      }),
      token: 'fixture',
      media: media,
      dialer: (_, _) => dial.future,
    );
    final start = session.start();
    await Future<void>.value();
    await session.close();
    expect(cancelled, 1);
    expect(media.starts, 0);
    dial.complete(socket);
    await start;
    expect(socket.wasClosed, isTrue);
    expect(media.starts, 0);
  });
  testWidgets(
    'a connect response after closing the native remote window releases its own ticket',
    (t) async {
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      for (final name in [
        'multitap/window',
        'multitap/window/events',
        'multitap/session-windows',
      ]) {
        messenger.setMockMethodCallHandler(
          MethodChannel(name),
          (_) async => false,
        );
        addTearDown(
          () => messenger.setMockMethodCallHandler(MethodChannel(name), null),
        );
      }
      final api = PendingBroker()..assigned = [desktop(ready: true)];
      var cancelled = 0;
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: launch(), api: api, guard: Guard()),
        ),
      );
      await t.pump();
      await t.pump();
      await t.pump();
      expect(api.connects, 1);
      await t.pumpWidget(const SizedBox());
      await t.pump();
      api.response.complete(
        reserved(() async {
          cancelled++;
        }),
      );
      await t.pump();
      await t.pump();
      expect(cancelled, 1);
      expect(t.takeException(), isNull);
    },
  );
}

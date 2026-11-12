import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:fake_async/fake_async.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/emergency_console_session.dart';

const _id = '01234567-89ab-cdef-8123-0123456789ab';
final _origin = Uri.parse('https://console.invalid:8443');
final _relay = Uri.parse('wss://console.invalid:8443/relay/console/$_id');

class _Headers extends Fake implements HttpHeaders {
  final values = <String, String>{};
  @override
  void set(String name, Object value, {bool preserveHeaderCase = false}) {
    values[name.toLowerCase()] = value.toString();
  }

  @override
  String? value(String name) => values[name.toLowerCase()];
}

class _Response extends Fake implements HttpClientResponse {
  _Response(this.statusCode, this.headers);
  @override
  final int statusCode;
  @override
  final _Headers headers;
  final detached = Completer<Socket>();
  var detachments = 0;
  @override
  Future<Socket> detachSocket() {
    detachments++;
    return detached.future;
  }
}

class _Request extends Fake implements HttpClientRequest {
  _Request({this.status = 101, this.forgedAccept = false, this.extension});
  final int status;
  final bool forgedAccept;
  final String? extension;
  final _Headers requestHeaders = _Headers();
  late _Response response;
  var aborts = 0;
  @override
  bool followRedirects = true;
  @override
  HttpHeaders get headers => requestHeaders;
  @override
  Future<HttpClientResponse> close() async {
    final key = requestHeaders.value('Sec-WebSocket-Key')!;
    final headers = _Headers()
      ..set('Connection', 'keep-alive, Upgrade')
      ..set('Upgrade', 'websocket')
      ..set(
        'Sec-WebSocket-Accept',
        forgedAccept
            ? 'not-the-request-key'
            : base64Encode(
                sha1
                    .convert(
                      ascii.encode(
                        '${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11',
                      ),
                    )
                    .bytes,
              ),
      );
    if (extension != null) {
      headers.set('Sec-WebSocket-Extensions', extension!);
    }
    response = _Response(status, headers);
    return response;
  }

  @override
  void abort([Object? exception, StackTrace? stackTrace]) {
    aborts++;
  }
}

class _Client extends Fake implements HttpClient {
  _Client(this.request);
  final _Request request;
  final urls = <Uri>[];
  var closes = 0;
  bool? forced;
  @override
  Duration? connectionTimeout;
  @override
  Future<HttpClientRequest> getUrl(Uri url) async {
    urls.add(url);
    return request;
  }

  @override
  void close({bool force = false}) {
    closes++;
    forced = force;
  }
}

class _Socket extends Fake implements Socket {
  var destroys = 0;
  @override
  void destroy() {
    destroys++;
  }
}

Future<T> _withClient<T>(_Client client, Future<T> Function() body) =>
    HttpOverrides.runZoned(body, createHttpClient: (_) => client);

BrokerConsoleGrant _grant({
  String id = _id,
  String ticket = 'synthetic-ticket',
  Uri? relay,
  DateTime? expiry,
}) => BrokerConsoleGrant(
  id: id,
  ticket: ticket,
  relay: relay ?? _relay,
  expiresAt: expiry ?? DateTime.utc(2035),
);

void main() {
  test('actual upgrade detach has a ten-second deadline and destroys only its late socket', () {
    fakeAsync((clock) {
      final request = _Request();
      final http = _Client(request);
      Object? failure;
      var completed = false;
      unawaited(
        _withClient(
          http,
          () => openConsoleTransport(_relay, 'synthetic'),
        ).then<void>(
          (_) {
            completed = true;
          },
          onError: (Object error) {
            completed = true;
            failure = error;
          },
        ),
      );
      clock.flushMicrotasks();
      expect(request.response.detachments, 1);
      clock.elapse(const Duration(milliseconds: 9999));
      expect(completed, isFalse);
      clock.elapse(const Duration(milliseconds: 1));
      clock.flushMicrotasks();
      expect(completed, isTrue);
      expect(failure, isA<TimeoutException>());
      expect(http.forced, isTrue);
      expect(request.aborts, 1);
      final exact = _Socket(), unrelated = _Socket();
      request.response.detached.complete(exact);
      clock.flushMicrotasks();
      expect(exact.destroys, 1);
      expect(unrelated.destroys, 0);
      expect(http.urls, [_relay.replace(scheme: 'https')]);
    });
  });

  test(
    'late detach error after its deadline is consumed without a second failure',
    () {
      fakeAsync((clock) {
        final request = _Request();
        final selected = _Client(request);
        var failures = 0;
        unawaited(
          _withClient(
            selected,
            () => openConsoleTransport(_relay, 'synthetic'),
          ).then<void>(
            (_) {},
            onError: (Object _) {
              failures++;
            },
          ),
        );
        clock.flushMicrotasks();
        clock.elapse(const Duration(seconds: 10));
        clock.flushMicrotasks();
        expect(failures, 1);
        request.response.detached.completeError(StateError('synthetic detach'));
        clock.flushMicrotasks();
        expect(failures, 1);
        expect(selected.closes, 1);
      });
    },
  );

  test('cancel during actual detach retires one grant and destroys the late socket without ready or auth', () {
    fakeAsync((clock) {
      final request = _Request();
      final selected = _Client(request);
      final releases = <String>[];
      var startsFinished = 0;
      final session = EmergencyConsoleSession(
        acquire: () async => _grant(),
        release: (id) async {
          releases.add(id);
        },
        origin: _origin,
        token: 'synthetic',
        transportFactory: (relay, token) =>
            _withClient(selected, () => openConsoleTransport(relay, token)),
      );
      unawaited(
        session.start().then((_) {
          startsFinished++;
        }),
      );
      clock.flushMicrotasks();
      expect(request.response.detachments, 1);
      unawaited(session.close());
      clock.flushMicrotasks();
      expect(session.closed, isTrue);
      expect(releases, [_id]);
      clock.elapse(const Duration(seconds: 10));
      clock.flushMicrotasks();
      expect(startsFinished, 1);
      final late = _Socket();
      request.response.detached.complete(late);
      clock.flushMicrotasks();
      expect(late.destroys, 1);
      expect(session.connected, isFalse);
      expect(session.failureCode, isNull);
      expect(releases, [_id]);
      session.dispose();
      clock.flushMicrotasks();
    });
  });

  for (final status in [401, 403]) {
    test(
      'unknown REST $status denial is terminal without opening a transport',
      () async {
        var opened = 0, claims = 0;
        final session = EmergencyConsoleSession(
          acquire: () async {
            claims++;
            throw BrokerException('future-denial-code', status);
          },
          release: (_) async {},
          origin: _origin,
          token: 'synthetic',
          transportFactory: (_, _) async {
            opened++;
            throw StateError('must not open');
          },
        );
        addTearDown(session.dispose);
        await session.start();
        await session.close();
        expect(
          session.failureCode,
          status == 401 ? 'session_expired' : 'console_permission_denied',
        );
        expect(session.canRetry, isFalse);
        expect(claims, 1);
        expect(opened, 0);
      },
    );

    test(
      'actual HTTPS upgrade $status denial is terminal and never detaches',
      () async {
        final request = _Request(status: status);
        final selected = _Client(request);
        final releases = <String>[];
        final session = EmergencyConsoleSession(
          acquire: () async => _grant(),
          release: (id) async {
            releases.add(id);
          },
          origin: _origin,
          token: 'synthetic',
          transportFactory: (relay, token) =>
              _withClient(selected, () => openConsoleTransport(relay, token)),
        );
        addTearDown(session.dispose);
        await session.start();
        await session.close();
        expect(
          session.failureCode,
          status == 401 ? 'session_expired' : 'console_permission_denied',
        );
        expect(session.canRetry, isFalse);
        expect(request.response.detachments, 0);
        expect(releases, [_id]);
      },
    );
  }

  for (final condition in [
    (302, false, null),
    (101, true, null),
    (101, false, 'permessage-deflate'),
  ]) {
    test(
      'invalid upgrade $condition never detaches or follows a redirect',
      () async {
        final request = _Request(
          status: condition.$1,
          forgedAccept: condition.$2,
          extension: condition.$3,
        );
        final http = _Client(request);
        await expectLater(
          _withClient(http, () => openConsoleTransport(_relay, 'synthetic')),
          throwsA(isA<WebSocketException>()),
        );
        expect(request.followRedirects, isFalse);
        expect(request.response.detachments, 0);
        expect(http.urls, [_relay.replace(scheme: 'https')]);
        expect(http.forced, isTrue);
      },
    );
  }

  final invalidGrants = <String, BrokerConsoleGrant>{
    'noncanonical UUID': _grant(
      id: 'not-a-uuid',
      relay: Uri.parse('wss://console.invalid:8443/relay/console/not-a-uuid'),
    ),
    'empty ticket': _grant(ticket: ''),
    'oversized ticket': _grant(ticket: 'x' * 513),
    'non-UTC expiry': _grant(expiry: DateTime(2035)),
    'expired grant': _grant(expiry: DateTime.utc(2000)),
    'foreign origin': _grant(
      relay: Uri.parse('wss://foreign.invalid:8443/relay/console/$_id'),
    ),
    'query on relay': _grant(relay: Uri.parse('${_relay.toString()}?secret=x')),
  };
  for (final entry in invalidGrants.entries) {
    test(
      'direct grant rejects ${entry.key} before any bearer transport',
      () async {
        var opened = 0;
        final session = EmergencyConsoleSession(
          acquire: () async => entry.value,
          release: (_) async {},
          origin: _origin,
          token: 'synthetic',
          transportFactory: (_, _) async {
            opened++;
            throw StateError('must not open');
          },
        );
        addTearDown(session.dispose);
        await session.start();
        await session.close();
        expect(opened, 0);
        expect(session.failureCode, 'console_auth_failed');
        expect(session.canRetry, isFalse);
      },
    );
  }
}

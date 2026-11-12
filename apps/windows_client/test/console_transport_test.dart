import 'dart:io';
import 'dart:convert';
import 'dart:async';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/emergency_console_session.dart';

class Headers extends Fake implements HttpHeaders {
  final entries = <String, String>{};
  @override
  void set(String name, Object value, {bool preserveHeaderCase = false}) =>
      entries[name.toLowerCase()] = value.toString();
  @override
  String? value(String name) => entries[name.toLowerCase()];
}

class Response extends Fake implements HttpClientResponse {
  Response(this.statusCode, this.headers, this.gate);
  final Completer<Socket>? gate;
  @override
  final int statusCode;
  @override
  final Headers headers;
  int detachments = 0;
  @override
  Future<Socket> detachSocket() async {
    detachments++;
    if (gate != null) return gate!.future;
    throw StateError('not a real socket');
  }
}

class Request extends Fake implements HttpClientRequest {
  Request(this.status, this.validAccept, this.extension);
  final int status;
  final bool validAccept;
  final String? extension;
  Response? response;
  Completer<Socket>? socketGate;
  bool aborted = false;
  @override
  bool followRedirects = true;
  @override
  final headers = Headers();
  @override
  Future<HttpClientResponse> close() async {
    final key = headers.value('Sec-WebSocket-Key')!;
    final reply = Headers()
      ..set('Connection', 'keep-alive, Upgrade')
      ..set('Upgrade', 'websocket')
      ..set(
        'Sec-WebSocket-Accept',
        validAccept
            ? base64Encode(
                sha1
                    .convert(
                      ascii.encode(
                        '${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11',
                      ),
                    )
                    .bytes,
              )
            : 'wrong-accept',
      );
    if (extension != null) reply.set('Sec-WebSocket-Extensions', extension!);
    return response = Response(status, reply, socketGate);
  }

  @override
  void abort([Object? exception, StackTrace? stackTrace]) {
    aborted = true;
  }
}

class Client extends Fake implements HttpClient {
  Client(this.request);
  final Request request;
  final urls = <Uri>[];
  bool closed = false;
  @override
  Duration? connectionTimeout;
  @override
  Future<HttpClientRequest> getUrl(Uri url) async {
    urls.add(url);
    return request;
  }

  @override
  void close({bool force = false}) {
    closed = true;
  }
}

class LateSocket extends Fake implements Socket {
  bool destroyed = false;
  @override
  void destroy() {
    destroyed = true;
  }
}

void main() {
  final relay = Uri.parse(
    'wss://fixture.invalid:8443/relay/console/01234567-89ab-cdef-8123-0123456789ab',
  );
  test('redirect and forged websocket upgrade never detach or forward bearer; each attempt has a fresh 16-byte nonce', () async {
    final nonces = <String>{};
    for (final condition in [
      (302, true, null),
      (101, false, null),
      (101, true, 'permessage-deflate'),
    ]) {
      final request = Request(condition.$1, condition.$2, condition.$3);
      // Each fake has one exact target only; no follow-up URL can be opened.
      final http = Client(request);
      await expectLater(
        openConsoleTransport(relay, 'fixture-token', client: http),
        throwsA(isA<WebSocketException>()),
      );
      expect(http.urls, [relay.replace(scheme: 'https')]);
      expect(request.followRedirects, isFalse);
      expect(
        request.headers.value(HttpHeaders.authorizationHeader),
        'Bearer fixture-token',
      );
      final nonce = request.headers.value('Sec-WebSocket-Key')!;
      expect(base64Decode(nonce), hasLength(16));
      expect(nonces.add(nonce), isTrue);
      expect(request.response!.detachments, 0);
      expect(request.aborted, isTrue);
      expect(http.closed, isTrue);
    }
  });
  testWidgets(
    'stalled socket hand-off times out and destroys its exact late socket',
    (tester) async {
      final gate = Completer<Socket>();
      final request = Request(101, true, null)..socketGate = gate;
      final http = Client(request);
      final rejected = expectLater(
        openConsoleTransport(relay, 'fixture-token', client: http),
        throwsA(isA<TimeoutException>()),
      );
      await tester.pump();
      await tester.pump(const Duration(seconds: 11));
      await rejected;
      expect(http.closed, isTrue);
      expect(request.aborted, isTrue);
      final late = LateSocket();
      gate.complete(late);
      await tester.pump();
      expect(late.destroyed, isTrue);
    },
  );
  test('cleartext, credentialed, query and wrong relay paths fail before creating a request', () async {
    for (final url in [
      'ws://fixture.invalid/relay/console/x',
      'wss://user@fixture.invalid/relay/console/x',
      '${relay.toString()}?token=x',
      'wss://fixture.invalid/other',
    ]) {
      final http = Client(Request(101, true, null));
      await expectLater(
        openConsoleTransport(Uri.parse(url), 'fixture-token', client: http),
        throwsA(isA<WebSocketException>()),
      );
      expect(http.urls, isEmpty);
    }
  });
}

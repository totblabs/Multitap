import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_transport.dart';
import 'package:multitap_client/data/relay_socket.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/android/android_updates.dart';

void main() {
  setUp(BrokerTransport.clear);
  tearDown(BrokerTransport.clear);

  test('real Android catalog and APK transport select current wire without bearer credentials', () async {
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    final origin = Uri.parse('http://127.0.0.1:${server.port}');
    final approval = BrokerTransport.approve(origin, origin.origin);
    final seen = <({String path, String? wire, String? bearer})>[];
    server.listen((request) async {
      final wire = request.headers.value('x-multitap-wire');
      seen.add((
        path: request.uri.path,
        wire: wire,
        bearer: request.headers.value('authorization'),
      ));
      request.response.headers.contentType = ContentType.json;
      if (request.uri.path == '/healthz') {
        request.response.write(
          jsonEncode({
            'service': 'multitap-broker',
            'status': 'ok',
            'authentication': 'id-password',
            'media': 'relay-v1',
            'version': '1.0.9',
            'transport': {
              'protocol': 'broker-transport-v1',
              'publicOrigin': origin.origin,
              'scheme': 'http',
              'allowInsecureHttp': true,
              'directTls': false,
            },
          }),
        );
      } else if (request.uri.path == '/api/v1/downloads') {
        const prefix = 'Multitap';
        request.response.write(
          jsonEncode({
            'releases': [
              {
                'product': 'android-client',
                'format': 'apk',
                'abi': 'arm64-v8a',
                'version': '1.0.5',
                'filename': '$prefix-1.0.5-android-arm64-v8a.apk',
                'size': 24,
                'sha256': List.filled(64, 'a').join(),
                'url': '/downloads/$prefix-1.0.5-android-arm64-v8a.apk',
              },
            ],
          }),
        );
      } else {
        request.response.add(List.filled(24, 0));
      }
      await request.response.close();
    });
    final updates = AndroidUpdates(origin);
    final transport = AndroidUpdateTransport();
    addTearDown(() async {
      updates.close();
      transport.close();
      approval.release();
      await server.close(force: true);
    });
    final release = await updates.latest('arm64-v8a');
    expect(release, isNotNull);
    expect(
      release!.url.path,
      '/downloads/Multitap-1.0.5-android-arm64-v8a.apk',
    );
    final response = await transport.get(release.url);
    expect(response.status, HttpStatus.ok);
    expect(await response.body.expand((bytes) => bytes).length, 24);
    expect(seen.every((request) => request.wire == '1'), isTrue);
    expect(seen.every((request) => request.bearer == null), isTrue);
    expect(
      seen.where((request) => request.path.startsWith('/downloads/')),
      hasLength(1),
    );
  });

  test('real update catalog and resumed download select the current wire without bearer credentials', () async {
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    final origin = Uri.parse('http://127.0.0.1:${server.port}');
    final approval = BrokerTransport.approve(origin, origin.origin);
    final seen =
        <
          ({
            String path,
            String? wire,
            String? bearer,
            String? range,
            String? entity,
          })
        >[];
    final signature = base64Encode(List.filled(64, 1));
    server.listen((request) async {
      final wire = request.headers.value('x-multitap-wire');
      seen.add((
        path: request.uri.path,
        wire: wire,
        bearer: request.headers.value('authorization'),
        range: request.headers.value('range'),
        entity: request.headers.value('if-range'),
      ));
      request.response.headers.contentType = ContentType.json;
      if (request.uri.path == '/healthz') {
        request.response.write(
          jsonEncode({
            'service': 'multitap-broker',
            'status': 'ok',
            'authentication': 'id-password',
            'media': 'relay-v1',
            'version': '1.0.9',
            'transport': {
              'protocol': 'broker-transport-v1',
              'publicOrigin': origin.origin,
              'scheme': 'http',
              'allowInsecureHttp': true,
              'directTls': false,
            },
          }),
        );
      } else if (request.uri.path == '/api/v1/downloads') {
        const prefix = 'Multitap';
        request.response.write(
          jsonEncode({
            'releases': [
              {
                'product': 'client',
                'format': 'installer',
                'version': '1.0.5',
                'filename': '$prefix-1.0.5-windows-x64.exe',
                'size': 32,
                'sha256': List.filled(64, 'a').join(),
                'url': '/downloads/$prefix-1.0.5-windows-x64.exe',
                'signature': signature,
              },
            ],
          }),
        );
      } else {
        request.response.statusCode = HttpStatus.partialContent;
        request.response.headers.set('content-range', 'bytes 8-31/32');
        request.response.add(List.filled(24, 0));
      }
      await request.response.close();
    });
    final updates = ProductUpdates(origin.origin);
    final transport = HttpsUpdateTransport();
    addTearDown(() async {
      updates.close();
      transport.close();
      approval.release();
      await server.close(force: true);
    });
    final release = await updates.check('client', '1.0.4');
    expect(release?.filename, 'Multitap-1.0.5-windows-x64.exe');
    expect(release?.signature, signature);
    final response = await transport.getFrom(
      origin.resolve('/downloads/Multitap-1.0.5-windows-x64.exe'),
      8,
      '"sha256-fixture"',
    );
    expect(response.status, HttpStatus.partialContent);
    await response.body.drain<void>();
    expect(seen.every((request) => request.wire == '1'), isTrue);
    expect(seen.every((request) => request.bearer == null), isTrue);
    final download = seen.singleWhere(
      (request) => request.path.startsWith('/downloads/'),
    );
    expect(download.range, 'bytes=8-');
    expect(download.entity, '"sha256-fixture"');
  });

  test('connect POST and session/auxiliary sockets identify the new wire without changing bearer authentication', () async {
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    final origin = Uri.parse('http://127.0.0.1:${server.port}');
    final approval = BrokerTransport.approve(origin, origin.origin);
    final requests =
        <({String path, String method, String? wire, String? bearer})>[];
    final peers = <WebSocket>[];
    server.listen((request) async {
      requests.add((
        path: request.uri.path,
        method: request.method,
        wire: request.headers.value('x-multitap-wire'),
        bearer: request.headers.value(HttpHeaders.authorizationHeader),
      ));
      if (WebSocketTransformer.isUpgradeRequest(request)) {
        final peer = await WebSocketTransformer.upgrade(request);
        peers.add(peer);
        peer.listen((_) {});
        return;
      }
      await request.drain<void>();
      request.response.headers.contentType = ContentType.json;
      if (request.uri.path == '/healthz') {
        request.response.write(
          jsonEncode({
            'service': 'multitap-broker',
            'status': 'ok',
            'authentication': 'id-password',
            'media': 'relay-v1',
            'version': '1.0.8',
            'transport': {
              'protocol': 'broker-transport-v1',
              'publicOrigin': origin.origin,
              'scheme': 'http',
              'allowInsecureHttp': true,
              'directTls': false,
            },
          }),
        );
      } else {
        request.response.statusCode = 409;
        request.response.write('{"error":"desktop_busy"}');
      }
      await request.response.close();
    });
    final api = HttpBrokerApi(origin: origin);
    addTearDown(() async {
      api.close();
      for (final peer in peers) {
        await peer.close();
      }
      approval.release();
      await server.close(force: true);
    });
    await expectLater(
      api.connect('fixture-token', 'pve-149'),
      throwsA(isA<BrokerException>()),
    );
    for (final path in ['/relay/session', '/relay/clipboard', '/api/v1/live']) {
      final socket = await connectRelaySocket(
        origin.replace(scheme: 'ws', path: path),
        'fixture-token',
        origin: origin,
        headers: const {'x-multitap-wire': 'stale-caller-value'},
      );
      await socket.close();
    }
    expect(requests.where((r) => r.method == 'POST'), hasLength(1));
    expect(requests.where((r) => r.path != '/healthz'), hasLength(4));
    expect(requests.every((r) => r.wire == '1'), isTrue);
    expect(
      requests
          .where((r) => r.path == '/healthz')
          .every((r) => r.bearer == null),
      isTrue,
    );
    expect(
      requests
          .where((r) => r.path != '/healthz')
          .every((r) => r.bearer == 'Bearer fixture-token'),
      isTrue,
    );
  });
}

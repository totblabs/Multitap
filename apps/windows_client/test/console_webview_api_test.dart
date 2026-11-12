import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_transport.dart';

import 'power_api_test.dart' show ApiClient;

void main() {
  test('native bootstrap uses authenticated same-origin empty JSON and scoped close', () async {
    final origin = Uri.parse('https://fixture.invalid:8443');
    final client = ApiClient()
      ..responses.addAll([
        (
          200,
          {
            'protocol': 'console-webview-v1',
            'path': '/console/pve-149/attach',
            'code': 'c' * 43,
            'expiresAt': DateTime.now()
                .add(const Duration(seconds: 30))
                .toUtc()
                .toIso8601String(),
          },
        ),
        (200, {'closed': true}),
        (200, {'closed': true}),
      ]);
    final api = HttpBrokerApi(origin: origin, client: client);
    final grant = await api.createConsoleWebView('parent-token', 'pve-149');
    expect(grant.path, '/console/pve-149/attach');
    await api.closeConsoleWebViews('parent-token', 'pve-149');
    await api.closeConsoleWebViews(
      'parent-token',
      'pve-149',
      ownerId: 'a' * 64,
    );
    expect(client.requests.map((request) => request.uri.toString()), [
      '$origin/api/v1/desktops/pve-149/console-webview',
      '$origin/api/v1/desktops/pve-149/console-webviews/close',
      '$origin/api/v1/desktops/pve-149/console-webviews/close',
    ]);
    for (final request in client.requests) {
      expect(request.method, 'POST');
      expect(request.followRedirects, isFalse);
      expect(
        request.headers.values[HttpHeaders.authorizationHeader],
        'Bearer parent-token',
      );
      expect(
        jsonDecode(request.body.toString()),
        request == client.requests.last
            ? <String, Object?>{'ownerId': 'a' * 64}
            : <String, Object?>{},
      );
      expect(request.uri.hasQuery, isFalse);
    }
    api.close();
  });
  test('old endpoint gets update guidance while revoked assignment keeps authorization failure', () async {
    for (final response in [
      (404, {'code': 'not_found'}),
      (404, {'code': 'desktop_not_found'}),
    ]) {
      final client = ApiClient()
        ..responses.add((response.$1, {'error': response.$2}));
      final api = HttpBrokerApi(
        origin: Uri.parse('https://fixture.invalid'),
        client: client,
      );
      await expectLater(
        api.createConsoleWebView('parent', 'pve-149'),
        throwsA(
          isA<BrokerException>().having(
            (error) => error.code,
            'code',
            response.$2['code'] == 'not_found'
                ? 'console_webview_update_required'
                : 'desktop_not_found',
          ),
        ),
      );
      api.close();
    }
  });
  test(
    'console bootstrap rejects HTTP before sending any parent authentication',
    () async {
      final client = ApiClient();
      final origin = Uri.parse('http://fixture.invalid');
      BrokerTransport.restore(origin, origin.origin);
      final api = HttpBrokerApi(origin: origin, client: client);
      await expectLater(
        api.createConsoleWebView('parent', 'pve-149'),
        throwsA(
          isA<BrokerException>().having(
            (error) => error.code,
            'code',
            'console_browser_https_required',
          ),
        ),
      );
      expect(client.requests, isEmpty);
      api.close();
    },
  );
}

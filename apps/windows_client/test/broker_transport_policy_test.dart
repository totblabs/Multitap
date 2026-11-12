import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_transport.dart';
import 'package:multitap_client/data/client_settings.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';

final httpOrigin = Uri.parse('http://example.test:18400');
Map<String, Object> profile(Uri origin) => {
  'protocol': 'broker-transport-v1',
  'publicOrigin': origin.origin,
  'scheme': 'http',
  'allowInsecureHttp': true,
  'directTls': false,
};
HttpTransportApproval approve(Uri origin) {
  validateHttpTransportHealth(origin, profile(origin));
  return BrokerTransport.approve(origin, origin.origin);
}

void main() {
  setUp(BrokerTransport.clear);
  tearDown(BrokerTransport.clear);
  test('canonical vectors share scheme host effective port across desktop and Android', () {
    for (final entry in {
      'HTTPS://EXAMPLE.test:443/': 'https://example.test',
      'http://192.0.2.10:18400/': 'http://192.0.2.10:18400',
      'http://example.test:80': 'http://example.test',
      'http://192.0.2.10.': 'http://192.0.2.10',
      'https://example.test.': 'https://example.test.',
      'http://[2001:0DB8:0:0:0:0:0:1]:18400': 'http://[2001:db8::1]:18400',
      'http://[::ffff:192.0.2.1]': 'http://[::ffff:c000:201]',
      'https://BÜCHER.example': 'https://xn--bcher-kva.example',
    }.entries) {
      final result = normalizeBrokerOrigin(entry.key);
      expect(result.toString(), entry.value);
      expect(normalizeBrokerOrigin(result.toString()), result);
    }
    expect(
      normalizeBrokerOrigin('example.test', defaultScheme: 'http').port,
      80,
    );
    expect(normalizeBrokerOrigin('example.test').scheme, 'https');
    expect(normalizeBrokerOrigin('http://example.test:1').port, 1);
    expect(normalizeBrokerOrigin('http://example.test:65535').port, 65535);
  });
  test(
    'ambiguous addresses and invalid ports never become another authority',
    () {
      for (final value in [
        '',
        'https://',
        'https://example.test:',
        'http://example.test:0',
        'http://example.test:65536',
        'http://example.test:+80',
        'http://example.test:1e3',
        'https://user:pass@example.test',
        'https://example.test/path',
        'https://example.test?',
        'https://example.test#',
        ' https://example.test',
        'https://exam\tple.test',
        r'https://example.test\evil',
        'http://127.1',
        'http://0177.0.0.1',
        'http://0x7f.0.0.1',
        'http://2130706433',
        'http://[fe80::1%25eth0]',
        'http://[::ffff:192.000.2.1]',
      ]) {
        expect(
          () => normalizeBrokerOrigin(value),
          throwsFormatException,
          reason: value,
        );
      }
    },
  );
  test('HTTP has no permission before consent plus matching health; HTTPS remains available', () {
    expect(() => BrokerTransport.token(httpOrigin), throwsFormatException);
    expect(BrokerTransport.token(Uri.parse('https://example.test')), isNull);
    expect(
      () => BrokerTransport.approve(httpOrigin, 'http://example.test:18401'),
      throwsFormatException,
    );
    final approval = approve(httpOrigin);
    expect(approval.current, isTrue);
    approval.release();
    expect(() => BrokerTransport.token(httpOrigin), throwsFormatException);
  });
  test('HTTP health exact types origin and protocol cannot be replaced by old backend health', () {
    validateHttpTransportHealth(httpOrigin, profile(httpOrigin));
    for (final value in [
      null,
      {},
      {...profile(httpOrigin), 'extra': true},
      {...profile(httpOrigin), 'allowInsecureHttp': 'true'},
      {...profile(httpOrigin), 'directTls': 0},
      {...profile(httpOrigin), 'directTls': true},
      {...profile(httpOrigin), 'publicOrigin': 'http://example.test:18401'},
      {...profile(httpOrigin), 'protocol': 'unknown'},
    ]) {
      expect(
        () => validateHttpTransportHealth(httpOrigin, value),
        throwsFormatException,
      );
    }
    validateHttpTransportHealth(Uri.parse('https://example.test'), null);
  });
  test('old temporary result cannot withdraw another current confirmation', () {
    final old = approve(httpOrigin), current = approve(httpOrigin);
    old.release();
    expect(current.current, isTrue);
    current.release();
    expect(() => BrokerTransport.token(httpOrigin), throwsFormatException);
  });
  test('saved origin changes retire old asynchronous authority without stealing fresh same-origin consent', () {
    final old = approve(httpOrigin), token = BrokerTransport.token(httpOrigin);
    BrokerTransport.stored(httpOrigin);
    old.release();
    expect(BrokerTransport.current(httpOrigin, token), isTrue);
    BrokerTransport.stored(Uri.parse('https://example.test:18400'));
    expect(BrokerTransport.current(httpOrigin, token), isFalse);
    final fresh = approve(httpOrigin);
    old.release();
    expect(fresh.current, isTrue);
  });
  test('all relay shapes retain exact scheme origin and port', () {
    approve(httpOrigin);
    for (final suffix in [
      '',
      '/clipboard',
      '/drive',
      '/print',
      '/usb',
      '/tracks',
    ]) {
      final path = '/api/v1/sessions/fixture$suffix';
      expect(
        brokerRelayMatches(
          Uri.parse('ws://example.test:18400$path'),
          httpOrigin,
          path: path,
        ),
        isTrue,
      );
      for (final prefix in [
        'wss://example.test:18400',
        'ws://other.test:18400',
        'ws://example.test:18401',
      ]) {
        expect(
          brokerRelayMatches(Uri.parse('$prefix$path'), httpOrigin, path: path),
          isFalse,
        );
      }
      expect(
        brokerRelayMatches(
          Uri.parse('ws://example.test:18400$path?x=1'),
          httpOrigin,
        ),
        isFalse,
      );
    }
    expect(
      brokerRelayMatches(
        Uri.parse('ws://example.test:443/relay'),
        Uri.parse('https://example.test'),
      ),
      isFalse,
    );
  });
  test('legacy HTTPS bytes load unchanged; HTTP persistence needs v2 exact consent', () async {
    final directory = await Directory.systemTemp.createTemp(
      'multitap-transport-policy-',
    );
    addTearDown(() => directory.delete(recursive: true));
    final file = File('${directory.path}/settings.json');
    const legacy = '{"serverOrigin":"https://example.test:8443"}';
    await file.writeAsString(legacy);
    final settings = ClientSettings(file);
    expect((await settings.load())?.origin, 'https://example.test:8443');
    expect(await file.readAsString(), legacy);
    await expectLater(settings.save(httpOrigin), throwsFormatException);
    final approved = approve(httpOrigin);
    await settings.save(httpOrigin);
    approved.release();
    expect(jsonDecode(await file.readAsString()), {
      'version': 2,
      'serverOrigin': httpOrigin.origin,
      'httpConsentOrigin': httpOrigin.origin,
    });
    BrokerTransport.clear();
    expect(await settings.load(), httpOrigin);
    expect(BrokerTransport.verified(httpOrigin), isFalse);
    await file.writeAsString(
      jsonEncode({
        'version': 2,
        'serverOrigin': httpOrigin.origin,
        'httpConsentOrigin': 'http://example.test:18401',
      }),
    );
    expect(await settings.load(), isNull);
    expect(() => BrokerTransport.token(httpOrigin), throwsFormatException);
  });
  test('v2 stored origins must already be canonical on every platform', () {
    for (final origin in [
      'http://EXAMPLE.test:18400/',
      'http://example.test:18400/',
      'https://EXAMPLE.test:443/',
    ]) {
      expect(
        () => decodeBrokerTransportSettings({
          'version': 2,
          'serverOrigin': origin,
          if (origin.startsWith('http:'))
            'httpConsentOrigin': httpOrigin.origin,
        }),
        throwsFormatException,
      );
    }
    expect(
      decodeBrokerTransportSettings({
        'serverOrigin': 'https://EXAMPLE.test:443/',
      }),
      Uri.parse('https://example.test'),
    );
  });
  test(
    'HTTP console grants and child windows carry only the same scoped consent',
    () {
      approve(httpOrigin);
      final grant = BrokerConsoleGrant.fromJson({
        'id': '11111111-2222-4333-8444-555555555555',
        'ticket': List.filled(43, 'a').join(),
        'relayPath': '/relay/console/11111111-2222-4333-8444-555555555555',
        'expiresAt': DateTime.now()
            .toUtc()
            .add(const Duration(seconds: 20))
            .toIso8601String(),
      }, httpOrigin);
      expect(grant.relay.scheme, 'ws');
      expect(grant.validFor(httpOrigin), isTrue);
      final launch = SessionWindowLaunch(
        origin: httpOrigin,
        token: 'fixture-token',
        expiresAt: DateTime.now().toUtc().add(const Duration(minutes: 1)),
        desktopId: 'pve-149',
        name: 'fixture',
        mode: SessionDisplayMode.windowed,
        options: const StreamOptions(),
      );
      final map = launch.toMap();
      expect(map['httpConsentOrigin'], httpOrigin.origin);
      BrokerTransport.clear();
      expect(SessionWindowLaunch.fromMap(map).origin, httpOrigin);
      map['httpConsentOrigin'] = 'http://example.test:18401';
      expect(() => SessionWindowLaunch.fromMap(map), throwsFormatException);
    },
  );
}

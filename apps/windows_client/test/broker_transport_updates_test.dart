import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_transport.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/android/android_updates.dart';

class _NoTransport implements UpdateTransport {
  int calls = 0;
  @override
  Future<UpdateResponse> get(Uri uri) async {
    calls++;
    throw StateError('A withdrawn update cannot send a request.');
  }

  @override
  void close() {}
}

void main() {
  setUp(BrokerTransport.clear);
  tearDown(BrokerTransport.clear);
  final origin = Uri.parse('http://example.test:18400');
  test(
    'Windows HTTP catalog keeps the exact release path and signature bytes',
    () {
      BrokerTransport.approve(origin, origin.origin);
      final signature = base64Encode(List.filled(64, 0));
      final data = {
        'product': 'client',
        'format': 'installer',
        'version': '0.4.107',
        'filename': 'Multitap-0.4.107-windows-x64.exe',
        'size': 2048,
        'sha256': List.filled(64, 'a').join(),
        'url': '/downloads/Multitap-0.4.107-windows-x64.exe',
        'signature': signature,
      };
      final release = ProductRelease.parse(data, origin, 'client')!;
      expect(release.url, origin.resolve(data['url'] as String));
      expect(release.signature, signature);
      for (final path in [
        'http://other.test/downloads/Multitap-0.4.107-windows-x64.exe',
        '/downloads/../secret',
        '/downloads/Multitap-0.4.107-windows-x64.exe?x=1',
      ]) {
        expect(
          () => ProductRelease.parse({...data, 'url': path}, origin, 'client'),
          throwsA(isA<UpdateFailure>()),
        );
      }
    },
  );
  test(
    'Android HTTP update preserves exact scheme origin path and ABI validation',
    () {
      BrokerTransport.approve(origin, origin.origin);
      AndroidRelease release(Uri url) => AndroidRelease(
        version: '0.4.42',
        abi: 'arm64-v8a',
        size: 2048,
        sha256: List.filled(64, 'a').join(),
        url: url,
      );
      final path = '/downloads/Multitap-0.4.42-android-arm64-v8a.apk';
      expect(
        () => release(origin.resolve(path)).validateFor(origin),
        returnsNormally,
      );
      for (final target in [
        Uri.parse('http://other.test:18400$path'),
        Uri.parse('https://example.test:18400$path'),
        Uri.parse('http://example.test:18401$path'),
        Uri.parse('${origin.origin}$path?x=1'),
      ]) {
        expect(
          () => release(target).validateFor(origin),
          throwsA(isA<UpdateFailure>()),
        );
      }
    },
  );
  test('changing away from HTTP blocks existing Windows and Android update objects before transport', () async {
    BrokerTransport.approve(origin, origin.origin);
    BrokerTransport.stored(origin);
    final windowsTransport = _NoTransport(), androidTransport = _NoTransport();
    final windows = ProductUpdates(origin.origin, transport: windowsTransport),
        android = AndroidUpdates(origin, transport: androidTransport);
    addTearDown(windows.close);
    addTearDown(android.close);
    BrokerTransport.stored(Uri.parse('https://example.test:18400'));
    await expectLater(
      windows.check('client', '0.4.100'),
      throwsA(isA<UpdateFailure>().having((e) => e.code, 'code', 'cancelled')),
    );
    await expectLater(
      android.latest('arm64-v8a'),
      throwsA(isA<UpdateFailure>()),
    );
    expect(windowsTransport.calls, 0);
    expect(androidTransport.calls, 0);
  });
}

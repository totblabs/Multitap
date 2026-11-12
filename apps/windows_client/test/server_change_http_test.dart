// Changing the server on the sign-in card from one HTTP address to another,
// with the real settings file and the real connection test (a loopback
// server): the file on disk, the form and the app must agree.
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_transport.dart';
import 'package:multitap_client/data/client_settings.dart';
import 'package:multitap_client/data/product_log.dart';

const _open = Key('open-server-settings');
const _address = Key('server-origin');
const _test = Key('test-server-connection');
const _consent = Key('confirm-http-consent');
const _save = Key('save-server');
const _success = Key('server-test-success');
const _error = Key('server-setting-error');
const _label = Key('login-server-label');

/// A Multitap server's /healthz for its own plain-HTTP origin.
Future<HttpServer> _broker() async {
  final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
  final origin = 'http://127.0.0.1:${server.port}';
  server.listen((request) async {
    request.response.headers.contentType = ContentType.json;
    request.response.write(
      jsonEncode({
        'service': 'multitap-broker',
        'status': 'ok',
        'authentication': 'id-password',
        'version': '1.1.1',
        'media': 'relay-v1',
        'transport': {
          'protocol': 'broker-transport-v1',
          'publicOrigin': origin,
          'scheme': 'http',
          'allowInsecureHttp': true,
          'directTls': false,
        },
      }),
    );
    await request.response.close();
  });
  return server;
}

/// Real sockets and file writes finish outside the fake clock.
Future<void> _until(WidgetTester tester, bool Function() done) async {
  for (var i = 0; i < 200 && !done(); i++) {
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 10)),
    );
    await tester.pump(const Duration(milliseconds: 50));
  }
}

void main() {
  setUp(BrokerTransport.clear);
  tearDown(BrokerTransport.clear);

  testWidgets('HTTP server A to HTTP server B before any sign-in: the address '
      'is saved, the form closes and the app uses B', (tester) async {
    tester.view.physicalSize = const Size(1440, 900);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    // The loopback connection test is a real request.
    HttpOverrides.global = null;

    final previous = Uri.parse('http://192.0.2.10:18480');
    late Directory temp;
    late HttpServer broker;
    late ClientSettings settings;
    Uri? started;
    await tester.runAsync(() async {
      temp = await Directory.systemTemp.createTemp('multitap-server-change-');
      broker = await _broker();
      final file = File('${temp.path}/Client/settings.json');
      await file.parent.create(recursive: true);
      await file.writeAsString(
        jsonEncode({
          'version': 2,
          'serverOrigin': previous.origin,
          'httpConsentOrigin': previous.origin,
        }),
      );
      // As main() starts: the stored address and its HTTP consent.
      settings = ClientSettings(file);
      started = await settings.load();
    });
    addTearDown(() async {
      await broker.close(force: true);
      await temp.delete(recursive: true);
    });
    expect(started, previous);
    final next = Uri.parse('http://127.0.0.1:${broker.port}');

    await tester.pumpWidget(
      MultitapApp(serverOrigin: started, onServerChanged: settings.save),
    );
    await tester.pumpAndSettle();
    expect(
      tester.widget<Text>(find.byKey(_label)).data,
      '192.0.2.10:18480',
    );

    await tester.tap(find.byKey(_open));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(_address), next.origin);
    await tester.pump();
    await tester.tap(find.byKey(_test));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(_consent));
    await _until(tester, () => find.byKey(_success).evaluate().isNotEmpty);
    expect(find.byKey(_success), findsOneWidget);

    final logged = ProductLog.instance.entries.length;
    await tester.ensureVisible(find.byKey(_save));
    await tester.tap(find.byKey(_save));
    await _until(
      tester,
      () =>
          find.byKey(_save).evaluate().isEmpty ||
          find.byKey(_error).evaluate().isNotEmpty,
    );
    await tester.pumpAndSettle();

    final stored = await tester.runAsync(
      () => File('${temp.path}/Client/settings.json').readAsString(),
    );
    expect((jsonDecode(stored!) as Map)['serverOrigin'], next.origin);
    // The file holds the new address, so the form must not say it failed.
    expect(find.byKey(_error), findsNothing);
    expect(find.byKey(_save), findsNothing);
    expect(
      tester.widget<Text>(find.byKey(_label)).data,
      '127.0.0.1:${broker.port}',
    );
    expect(
      ProductLog.instance.entries.skip(logged).map((entry) => entry.event),
      isNot(contains(LogEvent.serverSave)),
    );
    // Only the saved address keeps an HTTP approval.
    expect(brokerOriginAllowed(next), isTrue);
    expect(brokerOriginAllowed(previous), isFalse);
    expect(tester.takeException(), isNull);

    // Leaving the app closes the API that exists; none is made for an
    // address whose HTTP approval is gone.
    await tester.pumpWidget(const SizedBox());
    expect(tester.takeException(), isNull);
  });

  testWidgets('A settings file that cannot be written: the form says so, the '
      'log keeps why and the app stays on the saved address', (tester) async {
    tester.view.physicalSize = const Size(1440, 900);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    HttpOverrides.global = null;

    final previous = Uri.parse('http://192.0.2.10:18480');
    late Directory temp;
    late HttpServer broker;
    late ClientSettings unwritable;
    late File file;
    Uri? started;
    await tester.runAsync(() async {
      temp = await Directory.systemTemp.createTemp('multitap-server-change-');
      broker = await _broker();
      file = File('${temp.path}/Client/settings.json');
      await file.parent.create(recursive: true);
      await file.writeAsString(
        jsonEncode({
          'version': 2,
          'serverOrigin': previous.origin,
          'httpConsentOrigin': previous.origin,
        }),
      );
      started = await ClientSettings(file).load();
      // A file stands where the settings folder would be made.
      await File('${temp.path}/blocked').writeAsString('');
      unwritable = ClientSettings(File('${temp.path}/blocked/settings.json'));
    });
    addTearDown(() async {
      await broker.close(force: true);
      await temp.delete(recursive: true);
    });
    final next = Uri.parse('http://127.0.0.1:${broker.port}');

    await tester.pumpWidget(
      MultitapApp(serverOrigin: started, onServerChanged: unwritable.save),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(_open));
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(_address), next.origin);
    await tester.pump();
    await tester.tap(find.byKey(_test));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(_consent));
    await _until(tester, () => find.byKey(_success).evaluate().isNotEmpty);

    final logged = ProductLog.instance.entries.length;
    await tester.ensureVisible(find.byKey(_save));
    await tester.tap(find.byKey(_save));
    await _until(tester, () => find.byKey(_error).evaluate().isNotEmpty);
    await tester.pumpAndSettle();

    expect(
      find.descendant(
        of: find.byKey(_error),
        matching: find.text('서버 주소를 저장하지 못했어요'),
      ),
      findsOneWidget,
    );
    final record = ProductLog.instance.entries.skip(logged).single;
    expect(record.event, LogEvent.serverSave);
    expect(record.values['ok'], isFalse);
    expect(record.values['stage'], 1);
    expect(record.values['error'], 1);
    expect(record.values['osError'], isA<int>());
    expect(record.code, isNull);
    final written = jsonEncode(record.toJson());
    expect(written, isNot(contains('127.0.0.1')));
    expect(written, isNot(contains('blocked')));

    // Nothing was written, and the app is still on the saved address, which
    // keeps its HTTP approval.
    final stored = await tester.runAsync(file.readAsString);
    expect((jsonDecode(stored!) as Map)['serverOrigin'], previous.origin);
    await tester.tap(find.byKey(const Key('cancel-server-settings')));
    await tester.pumpAndSettle();
    expect(
      tester.widget<Text>(find.byKey(_label)).data,
      '192.0.2.10:18480',
    );
    expect(brokerOriginAllowed(previous), isTrue);
    expect(brokerOriginAllowed(next), isFalse);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox());
    expect(tester.takeException(), isNull);
  });
}

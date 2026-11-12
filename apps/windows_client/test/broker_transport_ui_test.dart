import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/data/broker_transport.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/server_settings_dialog.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';
import 'package:multitap_client/android/android_app.dart';

import 'host_settings_test.dart' as host;

BrokerProbeResult _verified(Uri origin) {
  validateHttpTransportHealth(origin, {
    'protocol': 'broker-transport-v1',
    'publicOrigin': origin.origin,
    'scheme': 'http',
    'allowInsecureHttp': true,
    'directTls': false,
  });
  return BrokerProbeResult(
    version: '0.5.77',
    elapsedMs: 1,
    httpApproval: BrokerTransport.approve(origin, origin.origin),
  );
}

Future<void> _form(
  WidgetTester tester, {
  required Future<BrokerProbeResult> Function(Uri) probe,
  required Future<void> Function(Uri) save,
}) async {
  tester.view.physicalSize = const Size(960, 800);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(),
      home: Scaffold(
        body: Center(
          child: SizedBox(
            width: 400,
            child: ServerSettingsForm(
              origin: null,
              onSave: save,
              onTest: probe,
              onClose: (_) {},
            ),
          ),
        ),
      ),
    ),
  );
  await tester.pumpAndSettle();
}

bool _canSave(WidgetTester tester) =>
    tester
        .widget<FilledButton>(find.byKey(const Key('save-server')))
        .onPressed !=
    null;
void main() {
  setUp(BrokerTransport.clear);
  tearDown(BrokerTransport.clear);
  testWidgets(
    'declining exact HTTP consent makes no probe or save and keeps HTTPS available',
    (tester) async {
      var probes = 0, saves = 0;
      await _form(
        tester,
        probe: (origin) async {
          probes++;
          return _verified(origin);
        },
        save: (_) async {
          saves++;
        },
      );
      await tester.enterText(
        find.byKey(const Key('server-origin')),
        'http://example.test:18400',
      );
      await tester.tap(find.byKey(const Key('test-server-connection')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('broker-http-consent')), findsOneWidget);
      expect(probes, 0);
      await tester.tap(find.byKey(const Key('cancel-http-consent')));
      await tester.pumpAndSettle();
      expect(_canSave(tester), isFalse);
      expect(probes, 0);
      expect(saves, 0);
      expect(
        () => BrokerTransport.token(Uri.parse('http://example.test:18400')),
        throwsFormatException,
      );
      await tester.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'changing a port invalidates verification and requires its own decision',
    (tester) async {
      var probes = 0, saves = 0;
      await _form(
        tester,
        probe: (origin) async {
          probes++;
          return _verified(origin);
        },
        save: (_) async {
          saves++;
        },
      );
      await tester.enterText(
        find.byKey(const Key('server-origin')),
        'http://example.test:18400',
      );
      await tester.tap(find.byKey(const Key('test-server-connection')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('confirm-http-consent')));
      await tester.pumpAndSettle();
      expect(probes, 1);
      expect(_canSave(tester), isTrue);
      await tester.enterText(
        find.byKey(const Key('server-origin')),
        'http://example.test:18401',
      );
      await tester.pump();
      expect(_canSave(tester), isFalse);
      expect(
        () => BrokerTransport.token(Uri.parse('http://example.test:18400')),
        throwsFormatException,
      );
      await tester.tap(find.byKey(const Key('test-server-connection')));
      await tester.pumpAndSettle();
      expect(
        find.descendant(
          of: find.byKey(const Key('broker-http-consent')),
          matching: find.text('http://example.test:18401'),
        ),
        findsOneWidget,
      );
      expect(probes, 1);
      expect(saves, 0);
      await tester.tap(find.byKey(const Key('cancel-http-consent')));
      await tester.pumpAndSettle();
      await tester.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'late verified result after form disposal releases only its temporary approval',
    (tester) async {
      final pending = Completer<BrokerProbeResult>();
      await _form(tester, probe: (_) => pending.future, save: (_) async {});
      final origin = Uri.parse('http://example.test:18400');
      await tester.enterText(
        find.byKey(const Key('server-origin')),
        origin.origin,
      );
      await tester.tap(find.byKey(const Key('test-server-connection')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('confirm-http-consent')));
      await tester.pump();
      await tester.pumpWidget(const SizedBox());
      pending.complete(_verified(origin));
      await tester.pump();
      expect(() => BrokerTransport.token(origin), throwsFormatException);
    },
  );
  testWidgets(
    'Android dialog uses the same exact HTTP decision before probing',
    (tester) async {
      var probes = 0, saves = 0;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: Scaffold(
            body: AndroidServerDialog(
              initial: null,
              save: (_) async {
                saves++;
              },
              probe: (origin) async {
                probes++;
                return _verified(origin);
              },
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.enterText(
        find.byType(TextField),
        'http://example.test:18400',
      );
      await tester.tap(find.text('연결 테스트'));
      await tester.pumpAndSettle();
      expect(probes, 0);
      await tester.tap(find.byKey(const Key('cancel-http-consent')));
      await tester.pumpAndSettle();
      expect(probes, 0);
      expect(saves, 0);
      await tester.pumpWidget(const SizedBox());
    },
  );
  testWidgets(
    'Host test and enrollment pass only the confirmed exact local origin',
    (tester) async {
      final api = host.FakeHostSettings();
      tester.view.physicalSize = const Size(1366, 900);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: HostSettingsScreen(api: api, closeGuard: host.FakeCloseGuard()),
        ),
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('host-nav-1')));
      await tester.pumpAndSettle();
      await tester.enterText(
        find.byKey(const Key('host-server-origin')),
        'http://example.test:18400',
      );
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('host-test-server')));
      await tester.pumpAndSettle();
      expect(api.requests.where((v) => v['action'] == 'testBroker'), isEmpty);
      await tester.tap(find.byKey(const Key('confirm-http-consent')));
      await tester.pumpAndSettle();
      final tested = api.requests.singleWhere(
        (v) => v['action'] == 'testBroker',
      );
      expect(tested['origin'], 'http://example.test:18400');
      expect(tested['httpConsentOrigin'], tested['origin']);
      await tester.tap(find.byKey(const Key('host-request-connection')));
      await tester.pumpAndSettle();
      final requested = api.requests.singleWhere(
        (v) => v['action'] == 'requestConnection',
      );
      expect(requested['httpConsentOrigin'], 'http://example.test:18400');
      await tester.pumpWidget(const SizedBox());
    },
  );
}

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/console_browser.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/login_memory.dart';

import 'auth_flow_test.dart' show TestBroker, TestLoginMemory;

final origin = Uri.parse('https://console.fixture.invalid:8443');
BrokerDesktop desktop({
  String? marker = 'console-browser-v1',
  String? nativeMarker,
  String kind = 'vm',
  String id = 'lab_149',
}) => BrokerDesktop(
  id: id,
  name: 'Fixture VM',
  kind: kind,
  description: '',
  os: 'Windows',
  status: 'host_not_registered',
  canConnect: false,
  consoleBrowserProtocol: marker,
  consoleWebViewProtocol: marker == 'console-browser-v1'
      ? 'console-webview-v1'
      : null,
  consoleRelayProtocol: nativeMarker,
);

class BrowserFixtureBroker extends TestBroker
    implements EmergencyConsoleBrokerApi, ConsoleWebViewBrokerApi {
  BrowserFixtureBroker() {
    assigned = [desktop()];
  }
  Completer<List<BrokerDesktop>>? fresh;
  int nativeGrants = 0;
  int webViewGrants = 0, webViewCloses = 0;
  @override
  Future<BrokerConsoleWebViewGrant> createConsoleWebView(
    String token,
    String desktopId,
  ) async {
    webViewGrants++;
    return BrokerConsoleWebViewGrant(
      path: '/console/$desktopId/attach',
      code: 'c' * 43,
      expiresAt: DateTime.now().add(const Duration(seconds: 30)),
    );
  }

  @override
  Future<void> closeConsoleWebViews(
    String token,
    String desktopId, {
    String? ownerId,
  }) async {
    webViewCloses++;
  }

  @override
  Future<BrokerConsoleGrant> createConsole(
    String token,
    String desktopId,
  ) async {
    nativeGrants++;
    throw StateError('The launcher must not acquire a native console.');
  }

  @override
  Future<void> releaseConsole(
    String token,
    String desktopId,
    String id,
  ) async => throw StateError('The launcher does not own a native console.');
  @override
  Future<List<BrokerDesktop>> desktops(String token) =>
      fresh?.future ?? Future.value(assigned);
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  tearDown(() {
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/window'),
      null,
    );
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/android/application'),
      null,
    );
    messenger.setMockMethodCallHandler(AndroidStore.channel, null);
  });
  test('optional capability means assignedVM browser route, never native or screen readiness; every clone preserves it', () {
    final value = desktop(), m = machineFromBroker(value);
    expect(value.browserConsoleSupported, isTrue);
    expect(value.emergencyConsoleSupported, isFalse);
    expect(m.hasEmergencyConsole, isTrue);
    expect(m.emergencyConsoleSupported, isFalse);
    expect(m.isReady, isFalse);
    final copies = [
      m.withPower(state: 'running', checkedAt: null, stale: false, busy: false),
      m.withDisplay(m.display),
      m.withResources(m.resources),
      m.withOrganize(m.organize),
      m.withHostUpdate(m.hostUpdate),
    ];
    expect(copies.every((v) => v.browserConsoleSupported), isTrue);
    for (final d in [
      desktop(marker: null),
      desktop(marker: 'future'),
      desktop(kind: 'pc'),
      desktop(id: '../other'),
      desktop(id: 'a' * 81),
    ]) {
      expect(d.browserConsoleSupported, isFalse);
      expect(machineFromBroker(d).browserConsoleSupported, isFalse);
    }
  });
  test('strict HTTPS target carries no native credentials/query and rejects arbitrary routes', () {
    expect(
      ConsoleBrowser.target(origin, 'lab_149').toString(),
      'https://console.fixture.invalid:8443/console/lab_149',
    );
    for (final value in [
      'http://host.test',
      'https://user:secret@host.test',
      'https://host.test/path',
      'https://host.test?ticket=secret',
      'https://host.test#secret',
      'https://host.test:0',
      'https://host.test:65536',
    ]) {
      expect(
        () => ConsoleBrowser.target(Uri.parse(value), 'lab_149'),
        throwsA(isA<BrokerException>()),
      );
    }
    for (final id in [
      '../other',
      'x?token=y',
      'x#y',
      'x/y',
      '_x',
      'x%2fy',
      'a' * 81,
    ]) {
      expect(
        () => ConsoleBrowser.target(origin, id),
        throwsA(isA<BrokerException>()),
      );
    }
  });
  for (final launcher in [
    const ConsoleBrowser.windows(),
    const ConsoleBrowser.android(),
  ]) {
    test(
      'platform channel ${launcher.channel.name} receives only normalized origin+desktopId; failure is fixed guidance',
      () async {
        MethodCall? sent;
        messenger.setMockMethodCallHandler(launcher.channel, (call) async {
          sent = call;
          return true;
        });
        final broker = BrowserFixtureBroker();
        await launcher.open(
          origin,
          'lab_149',
          api: broker,
          token: 'parent-stays-in-Dart',
          current: () => true,
        );
        expect(sent!.method, 'openConsoleWebView');
        expect(sent!.arguments, {
          'origin': origin.origin,
          'desktopId': 'lab_149',
          'attachPath': '/console/lab_149/attach',
          'code': 'c' * 43,
        });
        messenger.setMockMethodCallHandler(
          launcher.channel,
          (_) async => false,
        );
        await expectLater(
          launcher.open(
            origin,
            'lab_149',
            api: broker,
            token: 'parent-stays-in-Dart',
            current: () => true,
          ),
          throwsA(
            isA<BrokerException>().having(
              (e) => e.code,
              'code',
              'console_webview_unavailable',
            ),
          ),
        );
      },
    );
  }
  Future<void> tap(WidgetTester t, String key) async {
    final f = find.byKey(Key(key));
    await t.ensureVisible(f);
    await t.tap(f);
    await t.pumpAndSettle();
  }

  Future<void> windows(
    WidgetTester t,
    BrowserFixtureBroker broker, {
    Uri? server,
  }) async {
    final configured = server ?? origin;
    t.view.physicalSize = const Size(1366, 768);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    final memory = TestLoginMemory()
      ..entries[configured.origin] = const RememberedLogin(
        username: 'alice',
        password: 'fixture',
      );
    await t.pumpWidget(
      MultitapApp(api: broker, serverOrigin: configured, loginMemory: memory),
    );
    await t.pumpAndSettle();
    await tap(t, 'login-submit');
  }

  testWidgets(
    'Windows console confirms, rechecks current assignment, opens tokenless browser once and keeps launcher',
    (t) async {
      final opened = <Map>[];
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/window'),
        (call) async {
          if (call.method == 'openConsoleWebView') {
            opened.add(call.arguments as Map);
            return true;
          }
          return null;
        },
      );
      final broker = BrowserFixtureBroker();
      await windows(t, broker);
      await tap(t, 'stream-modes-lab_149');
      await tap(t, 'emergency-console-lab_149');
      expect(opened, isEmpty);
      expect(find.textContaining('앱 안에서 이 데스크톱의 원래 화면'), findsOneWidget);
      await t.tap(find.text('Console로 열기').last);
      await t.pumpAndSettle();
      expect(opened, [
        {
          'origin': origin.origin,
          'desktopId': 'lab_149',
          'attachPath': '/console/lab_149/attach',
          'code': 'c' * 43,
        },
      ]);
      expect(broker.connects, 0);
      expect(broker.nativeGrants, 0);
      expect(find.byKey(const Key('desktop-more-lab_149')), findsOneWidget);
    },
  );
  for (final marker in <String?>[null, 'future']) {
    testWidgets(
      'Windows legacy native capability and browser marker $marker require update without fallback',
      (t) async {
        final calls = <String>[];
        messenger.setMockMethodCallHandler(
          const MethodChannel('multitap/window'),
          (call) async {
            calls.add(call.method);
            return true;
          },
        );
        final broker = BrowserFixtureBroker()
          ..assigned = [
            desktop(id: 'pve-149', marker: marker, nativeMarker: 'rfb-none-v1'),
          ];
        expect(broker.assigned.single.emergencyConsoleSupported, isTrue);
        await windows(t, broker);
        await tap(t, 'stream-modes-pve-149');
        await tap(t, 'emergency-console-pve-149');
        await t.tap(find.text('Console로 열기').last);
        await t.pumpAndSettle();
        expect(find.textContaining('관리자에게 알려 주세요'), findsOneWidget);
        expect(calls, isNot(contains('openConsoleWebView')));
        expect(calls, isNot(contains('openSessionWindow')));
        expect(broker.nativeGrants, 0);
        expect(broker.connects, 0);
      },
    );
  }
  testWidgets(
    'Windows HTTP console explains HTTPS and never opens native or browser',
    (t) async {
      final calls = <String>[];
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/window'),
        (call) async {
          calls.add(call.method);
          return true;
        },
      );
      final broker = BrowserFixtureBroker();
      await windows(
        t,
        broker,
        server: Uri.parse('http://console.fixture.invalid:8080'),
      );
      await tap(t, 'stream-modes-lab_149');
      await tap(t, 'emergency-console-lab_149');
      await t.tap(find.text('Console로 열기').last);
      await t.pumpAndSettle();
      expect(find.textContaining('HTTPS 주소로 다시 로그인'), findsOneWidget);
      expect(calls, isNot(contains('openConsoleWebView')));
      expect(calls, isNot(contains('openSessionWindow')));
      expect(broker.nativeGrants, 0);
    },
  );
  testWidgets(
    'Windows fresh capability revocation fails with update guidance and pending duplicate opens are ignored',
    (t) async {
      var opens = 0;
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/window'),
        (call) async {
          if (call.method == 'openConsoleWebView') opens++;
          return true;
        },
      );
      final broker = BrowserFixtureBroker();
      await windows(t, broker);
      await tap(t, 'stream-modes-lab_149');
      await tap(t, 'emergency-console-lab_149');
      broker.fresh = Completer<List<BrokerDesktop>>();
      await t.tap(find.text('Console로 열기').last);
      await t.pumpAndSettle();
      await tap(t, 'stream-modes-lab_149');
      await tap(t, 'emergency-console-lab_149');
      expect(find.textContaining('앱 안에서 이 데스크톱의 원래 화면'), findsNothing);
      broker.fresh!.complete([desktop(marker: null)]);
      await t.pumpAndSettle();
      expect(find.textContaining('관리자에게 알려 주세요'), findsOneWidget);
      expect(opens, 0);
      expect(broker.nativeGrants, 0);
    },
  );
  testWidgets(
    'Windows browser launch failure never falls back to a native console',
    (t) async {
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/window'),
        (_) async => false,
      );
      final broker = BrowserFixtureBroker();
      await windows(t, broker);
      await tap(t, 'stream-modes-lab_149');
      await tap(t, 'emergency-console-lab_149');
      await t.tap(find.text('Console로 열기').last);
      await t.pumpAndSettle();
      expect(find.textContaining('네트워크를 확인하고 다시 시도'), findsOneWidget);
      expect(broker.nativeGrants, 0);
      expect(broker.connects, 0);
    },
  );
  for (final changed in [
    <BrokerDesktop>[desktop(kind: 'pc')],
    <BrokerDesktop>[desktop(), desktop()],
  ]) {
    testWidgets(
      'Windows fresh ${changed.length == 2 ? 'duplicate IDs' : 'PC assignment'} cannot launch any console',
      (t) async {
        var opens = 0;
        messenger.setMockMethodCallHandler(
          const MethodChannel('multitap/window'),
          (call) async {
            if (call.method == 'openConsoleWebView') opens++;
            return true;
          },
        );
        final broker = BrowserFixtureBroker();
        await windows(t, broker);
        await tap(t, 'stream-modes-lab_149');
        await tap(t, 'emergency-console-lab_149');
        broker.assigned = changed;
        await t.tap(find.text('Console로 열기').last);
        await t.pumpAndSettle();
        expect(opens, 0);
        expect(broker.nativeGrants, 0);
        expect(broker.connects, 0);
      },
    );
  }
  testWidgets(
    'Windows late fresh inventory after sign-out cannot open an old-account console',
    (t) async {
      var opens = 0;
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/window'),
        (call) async {
          if (call.method == 'openConsoleWebView') opens++;
          return true;
        },
      );
      final broker = BrowserFixtureBroker();
      await windows(t, broker);
      await tap(t, 'stream-modes-lab_149');
      await tap(t, 'emergency-console-lab_149');
      broker.fresh = Completer<List<BrokerDesktop>>();
      await t.tap(find.text('Console로 열기').last);
      await t.pumpAndSettle();
      await tap(t, 'workspace-account');
      await tap(t, 'sign-out');
      await tap(t, 'confirm-sign-out');
      broker.fresh!.complete([desktop()]);
      await t.pumpAndSettle();
      expect(find.byKey(const Key('login-submit')), findsOneWidget);
      expect(opens, 0);
      expect(broker.nativeGrants, 0);
    },
  );
  testWidgets(
    'Windows cancel and fresh assignment loss never launch a browser',
    (t) async {
      var opens = 0;
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/window'),
        (call) async {
          if (call.method == 'openConsoleWebView') opens++;
          return true;
        },
      );
      final broker = BrowserFixtureBroker();
      await windows(t, broker);
      await tap(t, 'stream-modes-lab_149');
      await tap(t, 'emergency-console-lab_149');
      await t.tap(find.text('취소').last);
      await t.pumpAndSettle();
      expect(opens, 0);
      await tap(t, 'stream-modes-lab_149');
      await tap(t, 'emergency-console-lab_149');
      broker.assigned = [];
      await t.tap(find.text('Console로 열기').last);
      await t.pumpAndSettle();
      expect(opens, 0);
      expect(broker.connects, 0);
    },
  );
  testWidgets(
    'Android browser capability confirms and opens only after fresh assignedVM check',
    (t) async {
      final opened = <Map>[];
      messenger.setMockMethodCallHandler(
        AndroidStore.channel,
        (call) async => switch (call.method) {
          'origin' => origin.toString(),
          'login' => {'username': 'alice', 'password': 'fixture'},
          'preferences' => <String, String>{},
          'appVersion' => 'fixture',
          _ => null,
        },
      );
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/android/application'),
        (call) async {
          if (call.method == 'openConsoleWebView') {
            opened.add(call.arguments as Map);
            return true;
          }
          return false;
        },
      );
      t.view.physicalSize = const Size(390, 844);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      final broker = BrowserFixtureBroker();
      await t.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
      await t.pumpAndSettle();
      await tap(t, 'login-submit');
      // ▾ holds 콘솔 on Android as on Windows (2026-11-03).
      await tap(t, 'stream-modes-lab_149');
      await tap(t, 'emergency-console-lab_149');
      expect(opened, isEmpty);
      await tap(t, 'confirm-console-lab_149');
      expect(opened, [
        {
          'origin': origin.origin,
          'desktopId': 'lab_149',
          'attachPath': '/console/lab_149/attach',
          'code': 'c' * 43,
        },
      ]);
      expect(broker.connects, 0);
      await t.pumpWidget(const SizedBox());
      await t.pumpAndSettle();
    },
  );
}

// Public launcher/console intent ownership against fake broker and transport.
// No native console, Proxmox, network connection or actual credentials.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_console_view.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';

import '../emergency_console_session_test.dart' show FakeConsoleTransport;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore;

BrokerDesktop desktop({
  String id = 'pve-149',
  String kind = 'vm',
  String? protocol = 'rfb-none-v1',
  String? browserProtocol = 'console-browser-v1',
}) => BrokerDesktop(
  id: id,
  name: 'Fixture $id',
  kind: kind,
  description: '',
  os: 'Windows',
  status: 'host_offline',
  canConnect: false,
  canStream: false,
  hostConnected: false,
  consoleRelayProtocol: protocol,
  consoleBrowserProtocol: browserProtocol,
  consoleWebViewProtocol: browserProtocol == 'console-browser-v1'
      ? 'console-webview-v1'
      : null,
  power: const BrokerPowerSnapshot(state: 'running', stale: false),
);

class ConsoleBroker extends FakeBroker
    implements EmergencyConsoleBrokerApi, ConsoleWebViewBrokerApi {
  ConsoleBroker(super.inventory, {this.origin = 'https://fixture.invalid'});
  final String origin;
  final creates = <(String, String)>[], releases = <(String, String)>[];
  Completer<List<BrokerDesktop>>? nextRead;
  int _nonce = 0;
  @override
  Future<BrokerConsoleWebViewGrant> createConsoleWebView(
    String token,
    String desktopId,
  ) async => BrokerConsoleWebViewGrant(
    path: '/console/$desktopId/attach',
    code: 'c' * 43,
    expiresAt: DateTime.now().add(const Duration(seconds: 30)),
  );
  @override
  Future<void> closeConsoleWebViews(
    String token,
    String desktopId, {
    String? ownerId,
  }) async {}
  @override
  Future<List<BrokerDesktop>> desktops(String token) {
    final pending = nextRead;
    if (pending == null) return super.desktops(token);
    nextRead = null;
    calls.add('desktops');
    return pending.future;
  }

  @override
  Future<BrokerConsoleGrant> createConsole(
    String token,
    String desktopId,
  ) async {
    final id =
        '01234567-89ab-cdef-8123-${(++_nonce).toRadixString(16).padLeft(12, '0')}';
    creates.add((desktopId, id));
    return BrokerConsoleGrant(
      id: id,
      ticket: 'fixture-one-use-ticket',
      relay: Uri.parse(origin)
          .replace(scheme: 'wss', path: '/relay/console/$id'),
      expiresAt: DateTime.utc(2035),
    );
  }

  @override
  Future<void> releaseConsole(String token, String desktopId, String id) async {
    releases.add((desktopId, id));
  }
}

Future<void> bounded(WidgetTester t) async {
  for (var frame = 0; frame < 10; frame++) {
    await t.pump(const Duration(milliseconds: 40));
  }
}

Future<void> tap(WidgetTester t, String key) async {
  final target = find.byKey(Key(key));
  await t.ensureVisible(target);
  await t.pump();
  expect(target.hitTestable(), findsOneWidget, reason: key);
  await t.tap(target);
  await bounded(t);
}

Future<void> boot(
  WidgetTester t,
  FakeBroker broker, {
  String origin = 'https://fixture.invalid',
  Key? key,
  List<FakeConsoleTransport>? transports,
}) async {
  t.view.physicalSize = const Size(390, 844);
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.reset);
  MemoryStore(origin: origin, username: 'tester').install();
  if (Uri.parse(origin).scheme == 'http') {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          AndroidStore.channel,
          (call) async => switch (call.method) {
            'transportSettings' => {
              'version': 2,
              'serverOrigin': origin,
              'httpConsentOrigin': origin,
            },
            'login' => {'username': 'tester', 'password': null},
            'preferences' => <String, String>{},
            'appVersion' => 'fixture',
            _ => null,
          },
        );
  }
  await t.pumpWidget(
    AndroidVdiApp(
      key: key,
      apiFactory: (_) => broker,
      consoleTransportFactory: (_, _) async {
        final transport = FakeConsoleTransport();
        transports?.add(transport);
        return transport;
      },
    ),
  );
  await t.pumpAndSettle();
  await t.enterText(find.byKey(const Key('sign-in-password')), 'fixture-only');
  await tap(t, 'login-submit');
}

/// ▾ beside the main button holds 긴급 콘솔, as on Windows (2026-11-03); a
/// tile with no console to offer may have no ▾ at all.
Future<void> menu(WidgetTester t, [String id = 'pve-149']) async {
  if (find.byKey(Key('stream-modes-$id')).evaluate().isEmpty) return;
  await tap(t, 'stream-modes-$id');
}

Future<void> request(WidgetTester t, [String id = 'pve-149']) async {
  await menu(t, id);
  await tap(t, 'emergency-console-$id');
  expect(
    find.byKey(const Key('console-connect-confirm-dialog')),
    findsOneWidget,
  );
}

Future<void> finish(WidgetTester t) async {
  await t.pumpWidget(const SizedBox.shrink());
  await t.pump(const Duration(seconds: 1));
  expect(t.takeException(), isNull);
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final browserCalls = <Map>[];
  var browserAvailable = true;
  setUp(() {
    browserCalls.clear();
    browserAvailable = true;
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          const MethodChannel('multitap/android/application'),
          (call) async {
            if (call.method != 'openConsoleWebView') return null;
            browserCalls.add(call.arguments as Map);
            return browserAvailable;
          },
        );
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          AndroidSessionKeepalive.channel,
          (call) async => call.method == 'start' ? 'started' : true,
        );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(AndroidStore.channel, null);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/android/application'),
      null,
    );
  });

  testWidgets(
    'physical PCs have no console entry even with console capabilities',
    (t) async {
      for (final broker in <FakeBroker>[
        ConsoleBroker([desktop(kind: 'pc')]),
      ]) {
        await boot(t, broker);
        await menu(t);
        expect(
          find.byKey(const Key('emergency-console-pve-149')),
          findsNothing,
        );
        expect(broker.calls.contains('connect'), isFalse);
        await finish(t);
      }
    },
  );

  for (final marker in <String?>[null, 'future']) {
    testWidgets(
      'legacy native console with browser marker $marker offers update guidance only',
      (t) async {
        final broker = ConsoleBroker([desktop(browserProtocol: marker)]);
        final transports = <FakeConsoleTransport>[];
        await boot(t, broker, transports: transports);
        await request(t);
        await tap(t, 'confirm-console-pve-149');
        expect(find.textContaining('Console을 열 수 없어요'), findsOneWidget);
        expect(browserCalls, isEmpty);
        expect(broker.creates, isEmpty);
        expect(transports, isEmpty);
        expect(find.byType(AndroidConsoleView), findsNothing);
        await finish(t);
      },
    );
  }

  testWidgets('HTTP console explains HTTPS without native fallback', (t) async {
    final broker = ConsoleBroker([desktop()]);
    await boot(t, broker, origin: 'http://fixture.invalid');
    await menu(t);
    await tap(t, 'emergency-console-pve-149');
    expect(find.textContaining('HTTPS 주소로 다시 로그인'), findsOneWidget);
    expect(browserCalls, isEmpty);
    expect(broker.creates, isEmpty);
    expect(find.byType(AndroidConsoleView), findsNothing);
    await finish(t);
  });

  testWidgets(
    'Host-offline running VM offers a console but cancel never creates a grant',
    (t) async {
      final broker = ConsoleBroker([desktop()]);
      await boot(t, broker);
      expect(find.byType(AndroidRemoteView), findsNothing);
      await request(t);
      expect(broker.creates, isEmpty);
      await tap(t, 'cancel-console-connect');
      expect(broker.creates, isEmpty);
      expect(broker.releases, isEmpty);
      expect(browserCalls, isEmpty);
      expect(find.byType(AndroidConsoleView), findsNothing);
      expect(broker.calls.contains('connect'), isFalse);
      await finish(t);
    },
  );

  for (final removedAssignment in [true, false]) {
    testWidgets(
      'fresh read rejects ${removedAssignment ? 'removed assignment' : 'revoked capability'} before grant creation',
      (t) async {
        final broker = ConsoleBroker([desktop()]);
        await boot(t, broker);
        await request(t);
        broker.inventory = removedAssignment
            ? []
            : [desktop(browserProtocol: null)];
        final reads = broker.calls.where((call) => call == 'desktops').length;
        await tap(t, 'confirm-console-pve-149');
        expect(
          broker.calls.where((call) => call == 'desktops').length,
          greaterThan(reads),
        );
        expect(broker.creates, isEmpty);
        expect(browserCalls, isEmpty);
        if (!removedAssignment) {
          expect(find.textContaining('Console을 열 수 없어요'), findsOneWidget);
        }
        expect(find.byType(AndroidConsoleView), findsNothing);
        expect(broker.calls.contains('connect'), isFalse);
        await finish(t);
      },
    );
  }

  testWidgets(
    'confirmed opens use only tokenless browser launch and preserve the launcher',
    (t) async {
      final broker = ConsoleBroker([desktop(), desktop(id: 'pve-150')]);
      final transports = <FakeConsoleTransport>[];
      await boot(t, broker, transports: transports);
      for (final id in ['pve-149', 'pve-149', 'pve-150']) {
        final before = browserCalls.length;
        await request(t, id);
        expect(browserCalls, hasLength(before));
        await tap(t, 'confirm-console-$id');
        expect(browserCalls, hasLength(before + 1));
        expect(browserCalls.last, {
          'origin': 'https://fixture.invalid',
          'desktopId': id,
          'attachPath': '/console/$id/attach',
          'code': 'c' * 43,
        });
        expect(find.byType(AndroidConsoleView), findsNothing);
        expect(find.byType(AndroidRemoteView), findsNothing);
        expect(broker.creates, isEmpty);
        expect(transports, isEmpty);
        expect(broker.releases, isEmpty);
        expect(find.text('내 데스크톱'), findsOneWidget);
      }
      expect(browserCalls, hasLength(3));
      expect(broker.calls.contains('connect'), isFalse);
      await finish(t);
    },
  );

  testWidgets(
    'a late fresh inventory after sign-out and another login cannot create an old-account grant',
    (t) async {
      final broker = ConsoleBroker([desktop()]);
      await boot(t, broker);
      await request(t);
      final gate = Completer<List<BrokerDesktop>>();
      broker.nextRead = gate;
      await tap(t, 'confirm-console-pve-149');
      expect(broker.creates, isEmpty);
      expect(browserCalls, isEmpty);
      await tap(t, 'account-menu');
      await tap(t, 'sign-out');
      await tap(t, 'confirm-sign-out');
      await t.enterText(find.byKey(const Key('sign-in-id')), 'another-account');
      await t.enterText(
        find.byKey(const Key('sign-in-password')),
        'fixture-only',
      );
      await tap(t, 'login-submit');
      gate.complete([desktop()]);
      await bounded(t);
      expect(broker.calls, contains('login:another-account'));
      expect(broker.creates, isEmpty);
      expect(browserCalls, isEmpty);
      expect(find.byType(AndroidConsoleView), findsNothing);
      await finish(t);
    },
  );

  testWidgets(
    'replacing the server scope while a fresh read is pending cannot open either server console',
    (t) async {
      final old = ConsoleBroker([desktop()]);
      final current = ConsoleBroker([
        desktop(),
      ], origin: 'https://other.invalid');
      await boot(t, old, key: const ValueKey('old-server'));
      await request(t);
      final gate = Completer<List<BrokerDesktop>>();
      old.nextRead = gate;
      await tap(t, 'confirm-console-pve-149');
      await boot(
        t,
        current,
        origin: 'https://other.invalid',
        key: const ValueKey('current-server'),
      );
      gate.complete([desktop()]);
      await bounded(t);
      expect(old.creates, isEmpty);
      expect(current.creates, isEmpty);
      expect(browserCalls, isEmpty);
      expect(find.byType(AndroidConsoleView), findsNothing);
      expect(find.text('내 데스크톱'), findsOneWidget);
      await finish(t);
    },
  );

  testWidgets(
    'fresh-read permission failure makes no grant or streaming request',
    (t) async {
      final broker = ConsoleBroker([desktop()]);
      await boot(t, broker);
      await request(t);
      final gate = Completer<List<BrokerDesktop>>();
      broker.nextRead = gate;
      await tap(t, 'confirm-console-pve-149');
      gate.completeError(
        const BrokerException('console_permission_denied', 403),
      );
      await bounded(t);
      expect(broker.creates, isEmpty);
      expect(browserCalls, isEmpty);
      expect(broker.calls.contains('connect'), isFalse);
      expect(find.byType(AndroidConsoleView), findsNothing);
      await finish(t);
    },
  );

  testWidgets(
    'browser failure remains actionable and never creates native transport',
    (t) async {
      browserAvailable = false;
      final broker = ConsoleBroker([desktop()]);
      final transports = <FakeConsoleTransport>[];
      await boot(t, broker, transports: transports);
      await request(t);
      await tap(t, 'confirm-console-pve-149');
      expect(browserCalls, hasLength(1));
      expect(find.textContaining('네트워크를 확인하고 다시 시도'), findsOneWidget);
      expect(broker.creates, isEmpty);
      expect(broker.releases, isEmpty);
      expect(transports, isEmpty);
      expect(broker.calls.contains('connect'), isFalse);
      await finish(t);
    },
  );
}

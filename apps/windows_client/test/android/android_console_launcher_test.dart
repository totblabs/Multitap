// Public launcher/console intent ownership against a fake broker.
// No Proxmox, network connection or actual credentials.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';

import 'android_launcher_test.dart' show FakeBroker, MemoryStore;

BrokerDesktop desktop({
  String id = 'pve-149',
  String kind = 'vm',
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
  consoleBrowserProtocol: browserProtocol,
  consoleWebViewProtocol: browserProtocol == 'console-browser-v1'
      ? 'console-webview-v1'
      : null,
  power: const BrokerPowerSnapshot(state: 'running', stale: false),
);

class ConsoleBroker extends FakeBroker implements ConsoleWebViewBrokerApi {
  ConsoleBroker(super.inventory);
  Completer<List<BrokerDesktop>>? nextRead;
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
    AndroidVdiApp(key: key, apiFactory: (_) => broker),
  );
  await t.pumpAndSettle();
  await t.enterText(find.byKey(const Key('sign-in-password')), 'fixture-only');
  await tap(t, 'login-submit');
}

/// ▾ beside the main button holds 긴급 콘솔, as on Windows; a
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
      'a server without the WebView console marker ($marker) offers update guidance only',
      (t) async {
        final broker = ConsoleBroker([desktop(browserProtocol: marker)]);
        await boot(t, broker);
        await request(t);
        await tap(t, 'confirm-console-pve-149');
        expect(find.textContaining('Console을 열 수 없어요'), findsOneWidget);
        expect(browserCalls, isEmpty);
        await finish(t);
      },
    );
  }

  testWidgets('HTTP console explains HTTPS', (t) async {
    final broker = ConsoleBroker([desktop()]);
    await boot(t, broker, origin: 'http://fixture.invalid');
    await menu(t);
    await tap(t, 'emergency-console-pve-149');
    expect(find.textContaining('HTTPS 주소로 다시 로그인'), findsOneWidget);
    expect(browserCalls, isEmpty);
    await finish(t);
  });

  testWidgets(
    'Host-offline running VM offers a console but cancel opens nothing',
    (t) async {
      final broker = ConsoleBroker([desktop()]);
      await boot(t, broker);
      expect(find.byType(AndroidRemoteView), findsNothing);
      await request(t);
      await tap(t, 'cancel-console-connect');
      expect(browserCalls, isEmpty);
      expect(broker.calls.contains('connect'), isFalse);
      await finish(t);
    },
  );

  for (final removedAssignment in [true, false]) {
    testWidgets(
      'fresh read rejects ${removedAssignment ? 'removed assignment' : 'revoked capability'} before opening',
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
        expect(browserCalls, isEmpty);
        if (!removedAssignment) {
          expect(find.textContaining('Console을 열 수 없어요'), findsOneWidget);
        }
        expect(broker.calls.contains('connect'), isFalse);
        await finish(t);
      },
    );
  }

  testWidgets(
    'confirmed opens use only tokenless browser launch and preserve the launcher',
    (t) async {
      final broker = ConsoleBroker([desktop(), desktop(id: 'pve-150')]);
      await boot(t, broker);
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
        expect(find.byType(AndroidRemoteView), findsNothing);
        expect(find.text('내 데스크톱'), findsOneWidget);
      }
      expect(browserCalls, hasLength(3));
      expect(broker.calls.contains('connect'), isFalse);
      await finish(t);
    },
  );

  testWidgets(
    'a late fresh inventory after sign-out and another login cannot open an old-account console',
    (t) async {
      final broker = ConsoleBroker([desktop()]);
      await boot(t, broker);
      await request(t);
      final gate = Completer<List<BrokerDesktop>>();
      broker.nextRead = gate;
      await tap(t, 'confirm-console-pve-149');
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
      expect(browserCalls, isEmpty);
      await finish(t);
    },
  );

  testWidgets(
    'replacing the server scope while a fresh read is pending cannot open either server console',
    (t) async {
      final old = ConsoleBroker([desktop()]);
      final current = ConsoleBroker([desktop()]);
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
      expect(browserCalls, isEmpty);
      expect(find.text('내 데스크톱'), findsOneWidget);
      await finish(t);
    },
  );

  testWidgets(
    'fresh-read permission failure opens nothing and makes no streaming request',
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
      expect(browserCalls, isEmpty);
      expect(broker.calls.contains('connect'), isFalse);
      await finish(t);
    },
  );

  testWidgets(
    'browser failure remains actionable',
    (t) async {
      browserAvailable = false;
      final broker = ConsoleBroker([desktop()]);
      await boot(t, broker);
      await request(t);
      await tap(t, 'confirm-console-pve-149');
      expect(browserCalls, hasLength(1));
      expect(find.textContaining('네트워크를 확인하고 다시 시도'), findsOneWidget);
      expect(broker.calls.contains('connect'), isFalse);
      await finish(t);
    },
  );
}

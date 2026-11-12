import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_watch.dart';
import 'package:multitap_client/models/vm_resources.dart';

import '../live_resources_test.dart' show LiveFeeds;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore, vm;
import 'android_updates_test.dart' show FakePlatform, FakeTransport, apk;

const _gib = 1024 * 1024 * 1024;

VmResources _sample(double cpu, {Duration age = Duration.zero}) {
  final at = DateTime.now().toUtc().subtract(age);
  return VmResources(
    cpuUsagePercent: cpu,
    vcpus: 4,
    memoryUsedBytes: 2 * _gib,
    memoryTotalBytes: 8 * _gib,
    uptimeSeconds: 600,
    checkedAt: at,
    receivedAt: at,
    stale: false,
  );
}

BrokerDesktop _withResources(VmResources resources) {
  final base = vm();
  return BrokerDesktop(
    id: base.id,
    name: base.name,
    kind: base.kind,
    description: base.description,
    os: base.os,
    status: base.status,
    canConnect: base.canConnect,
    canStream: base.canStream,
    hostConnected: base.hostConnected,
    power: base.power,
    powerCanForceStop: base.powerCanForceStop,
    resources: resources,
  );
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  Future<void> open(
    WidgetTester tester,
    FakeBroker broker,
    LiveFeeds feeds, {
    FakeTransport? transport,
  }) async {
    tester.view.physicalSize = const Size(1080, 2400);
    tester.view.devicePixelRatio = 3;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    MemoryStore(origin: 'https://vdi.example', username: 'tester').install();
    await tester.pumpWidget(
      AndroidVdiApp(
        apiFactory: (_) => broker,
        desktopWatchFactory: (_, _) => feeds,
        updatesFactory: transport == null
            ? null
            : (o) => AndroidUpdates(o, transport: transport),
        updatePlatform: FakePlatform(),
      ),
    );
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'secret-pass',
    );
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
    feeds.ready();
    await tester.pump();
    await tester.pump();
  }

  Finder cpu(String text) => find.descendant(
    of: find.byKey(const Key('resources-pve-149')),
    matching: find.text(text),
  );
  int reads(FakeBroker broker) =>
      broker.calls.where((c) => c == 'desktops').length;

  testWidgets(
    'pushed samples update the card without a list read and go stale after 20 s',
    (tester) async {
      final broker = FakeBroker([
        _withResources(_sample(23, age: const Duration(seconds: 40))),
      ]);
      final feeds = LiveFeeds();
      await open(tester, broker, feeds);
      await tester.pumpAndSettle();
      expect(cpu('23%'), findsOneWidget);
      final before = reads(broker);
      feeds.samples.add([LiveResourceSample('pve-149', _sample(58))]);
      await tester.pumpAndSettle();
      expect(cpu('58%'), findsOneWidget);
      expect(reads(broker), before, reason: 'a sample is not a list re-read');
      // A list entry older than the pushed sample does not bring old numbers back.
      feeds.changed();
      await tester.pump();
      await tester.pump();
      await tester.pumpAndSettle();
      expect(cpu('58%'), findsOneWidget);
      // 15 s without news is still current, 25 s is not.
      feeds.samples.add([
        LiveResourceSample(
          'pve-149',
          _sample(61, age: const Duration(seconds: 15)),
        ),
      ]);
      await tester.pump();
      await tester.pump(const Duration(seconds: 6));
      await tester.pumpAndSettle();
      expect(cpu('61%'), findsOneWidget);
      feeds.samples.add([
        LiveResourceSample(
          'pve-149',
          _sample(62, age: const Duration(seconds: 25)),
        ),
      ]);
      await tester.pump();
      await tester.pump(const Duration(seconds: 6));
      await tester.pumpAndSettle();
      expect(cpu('62%'), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'a catalog push reads the update catalog at once and badges the header; reconnecting reads it once',
    (tester) async {
      final route = 'https://vdi.example/api/v1/downloads';
      final transport = FakeTransport({
        route: (200, utf8.encode(jsonEncode({'releases': <Object>[]})), null),
      });
      final broker = FakeBroker([vm()]);
      final feeds = LiveFeeds();
      await open(tester, broker, feeds, transport: transport);
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 100)),
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('client-update-badge')), findsNothing);
      final before = transport.requested.length;
      expect(before, greaterThan(0));
      transport.routes[route] = (
        200,
        utf8.encode(
          jsonEncode({
            'releases': [apk('9.9.9', 'arm64-v8a')],
          }),
        ),
        null,
      );
      feeds.catalogNotices.add(null);
      await tester.pump();
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 100)),
      );
      await tester.pumpAndSettle();
      expect(transport.requested.length, greaterThan(before));
      expect(find.byKey(const Key('client-update-badge')), findsOneWidget);
      // With the channel up the 10-minute timer does not read.
      final connected = transport.requested.length;
      await tester.pump(const Duration(minutes: 11));
      expect(transport.requested.length, connected);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );
}

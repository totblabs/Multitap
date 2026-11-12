import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_watch.dart';
import 'package:multitap_client/models/vm_resources.dart';

import '../live_resources_test.dart' show LiveFeeds;
import '../pc_power_test.dart' show pcJson, pcId;
import '../pc_resources_test.dart' show hostSample;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore, vm;
import 'android_updates_test.dart' show FakePlatform;

// A physical PC's Android card: CPU and memory from its own Host, through the
// broker's list JSON and the live push, in the same strip as a VM's.
BrokerDesktop _pc({bool online = true}) => BrokerDesktop.fromJson({
  ...pcJson(online: online),
  'resources': online ? hostSample() : null,
});

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  Future<void> open(WidgetTester t, FakeBroker broker, LiveFeeds feeds) async {
    t.view.physicalSize = const Size(1080, 2400);
    t.view.devicePixelRatio = 3;
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    MemoryStore(origin: 'https://vdi.example', username: 'tester').install();
    await t.pumpWidget(
      AndroidVdiApp(
        apiFactory: (_) => broker,
        desktopWatchFactory: (_, _) => feeds,
        updatePlatform: FakePlatform(),
      ),
    );
    await t.pumpAndSettle();
    await t.enterText(find.byKey(const Key('sign-in-password')), 'secret-pass');
    await t.tap(find.byKey(const Key('login-submit')));
    await t.pumpAndSettle();
    feeds.ready();
    await t.pump();
    await t.pump();
    await t.pumpAndSettle();
  }

  Finder inStrip(Finder matching) => find.descendant(
    of: find.byKey(const Key('resources-$pcId')),
    matching: matching,
  );

  testWidgets(
    'a connected PC card shows live CPU and memory, follows pushed samples and keeps the VM card size',
    (t) async {
      final broker = FakeBroker([_pc(), vm()]);
      final feeds = LiveFeeds();
      await open(t, broker, feeds);
      expect(inStrip(find.text('CPU 8코어')), findsOneWidget);
      expect(inStrip(find.text('38%')), findsOneWidget);
      expect(inStrip(find.text('메모리 16 GB')), findsOneWidget);
      expect(inStrip(find.byKey(const ValueKey('gauge'))), findsNWidgets(2));
      expect(
        t.getSize(find.byKey(const Key('resources-$pcId'))),
        t.getSize(find.byKey(const Key('resources-pve-149'))),
      );
      feeds.samples.add([
        LiveResourceSample(
          pcId,
          VmResources.fromJson({...hostSample(), 'cpuUsagePercent': 81.0}),
        ),
      ]);
      await t.pumpAndSettle();
      expect(inStrip(find.text('81%')), findsOneWidget);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets('an offline PC card shows no measurement', (t) async {
    final broker = FakeBroker([_pc(online: false)]);
    await open(t, broker, LiveFeeds());
    expect(inStrip(find.textContaining('%')), findsNothing);
    expect(inStrip(find.byKey(const ValueKey('gauge'))), findsNothing);
    expect(t.takeException(), isNull);
    await t.pumpWidget(const SizedBox.shrink());
  });
}

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_application.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/ui/desktop_tile.dart';

import 'android_launcher_test.dart' show FakeBroker, MemoryStore;
import 'android_menu_parity_test.dart' show tap, disposeApp;
import 'android_ui_audit.dart' show auditTargets;

BrokerDesktop desktop() => BrokerDesktop(
  id: 'pve-245',
  kind: 'vm',
  name: '작업용 Windows',
  description: '',
  os: 'Windows 11',
  status: 'online',
  canConnect: true,
  canStream: true,
  hostConnected: true,
  hostUpdate: HostUpdateState(
    present: true,
    connected: true,
    supported: true,
    version: '1.0.0',
    latestVersion: '1.0.13',
    newer: true,
    available: true,
  ),
);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(AndroidStore.channel, null);
    messenger.setMockMethodCallHandler(AndroidApplication.channel, null);
  });

  for (final scale in [1.0, 2.0]) {
    testWidgets('sign-in and the desktop menu fit 320dp at text ${scale * 100}%', (tester) async {
      tester.platformDispatcher.textScaleFactorTestValue = scale;
      addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
      // Deliberately no HostUpdateBrokerApi: this broker cannot install.
      final broker = FakeBroker([desktop()]);
      tester.view.physicalSize = const Size(320, 568);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      MemoryStore(origin: 'https://vdi.example', username: 'tester').install();
      await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
      await tester.pumpAndSettle();
      await tester.enterText(
        find.byKey(const Key('sign-in-password')), 'fixture-password');
      // Text entry schedules a caret reveal after the next frame. Complete
      // that reveal before scrolling to the action; doing it afterwards moved
      // the 200% login target below this short viewport in the old fixture.
      await tester.pumpAndSettle();
      final submit = find.byKey(const Key('login-submit'));
      await tester.ensureVisible(submit);
      await tester.pumpAndSettle();
      final rect = tester.getRect(submit);
      final viewport = tester.view.physicalSize / tester.view.devicePixelRatio;
      expect(rect.left, greaterThanOrEqualTo(0));
      expect(rect.top, greaterThanOrEqualTo(0));
      expect(rect.right, lessThanOrEqualTo(viewport.width));
      expect(rect.bottom, lessThanOrEqualTo(viewport.height));
      expect(rect.width, greaterThanOrEqualTo(48));
      expect(rect.height, greaterThanOrEqualTo(48));
      expect(submit.hitTestable(), findsOneWidget);
      await tester.tap(submit);
      await tester.pumpAndSettle();
      expect(find.byType(DesktopTile), findsOneWidget);
      final before = List<String>.of(broker.calls);
      await tap(tester, 'desktop-more-pve-245');
      expect(find.byKey(const Key('host-update-pve-245')), findsNothing);
      expect(tester.takeException(), isNull);
      expect(auditTargets(tester), isEmpty);
      expect(broker.calls, before);
      await disposeApp(tester);
    });
  }
}

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/stream_preparation.dart';

import '../workspace_live_state_test.dart' show LiveUpdates;
import 'android_launcher_test.dart'
    show FakeBroker, MemoryStore, vm, mainButton;

BrokerDesktop state(String vmState, {String power = 'running'}) {
  final base = vm(power: power, stream: false);
  return BrokerDesktop(
    id: base.id,
    name: base.name,
    kind: base.kind,
    description: base.description,
    os: base.os,
    status: 'host_offline',
    canConnect: false,
    hostConnected: false,
    power: base.power,
    vmState: vmState,
    vmStateReason: 'test',
    vmStateRevision: 1,
  );
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  Future<FakeBroker> open(
    WidgetTester tester,
    List<BrokerDesktop> desktops, {
    LiveUpdates? updates,
  }) async {
    tester.view.physicalSize = const Size(1080, 2400);
    tester.view.devicePixelRatio = 3;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    MemoryStore(origin: 'https://vdi.example', username: 'tester').install();
    final broker = FakeBroker(desktops);
    await tester.pumpWidget(
      AndroidVdiApp(
        apiFactory: (_) => broker,
        desktopWatchFactory: updates == null ? null : (_, _) => updates,
      ),
    );
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'secret-pass',
    );
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
    return broker;
  }

  int reads(FakeBroker broker) =>
      broker.calls.where((c) => c == 'desktops').length;

  testWidgets(
    'connected channel: no polling at all, a push is read once and the tile follows the broker',
    (tester) async {
      final updates = LiveUpdates();
      final broker = await open(tester, [
        vm(power: 'stopped', stream: false),
      ], updates: updates);
      expect(updates.starts, 1);
      updates.ready();
      await tester.pump();
      await tester.pump();
      final before = reads(broker);
      expect(find.text('켜기'), findsOneWidget);
      await tester.pump(const Duration(minutes: 2));
      expect(
        reads(broker),
        before,
        reason: 'No timer reads while the channel is up.',
      );
      // The broker pushes: Windows is starting, then the Host connects.
      broker.inventory = [state('windows_starting')];
      updates.changed();
      await tester.pump();
      await tester.pump();
      expect(reads(broker), before + 1);
      expect(find.text('Windows를 시작하고 있어요'), findsWidgets);
      expect(find.text('켜기'), findsNothing);
      broker.inventory = [vm()];
      updates.changed();
      await tester.pump();
      await tester.pump();
      expect(reads(broker), before + 2);
      final primary = mainButton('pve-149');
      expect(
        find.descendant(of: primary, matching: find.text('스트리밍 연결')),
        findsOneWidget,
      );
      expect(tester.widget<FilledButton>(primary).onPressed, isNotNull);
      expect(broker.calls.contains('connect'), isFalse);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'channel down: the list is read every 15 s; it stops when the channel returns and reads once',
    (tester) async {
      final updates = LiveUpdates();
      final broker = await open(tester, [vm()], updates: updates);
      final before = reads(broker);
      await tester.pump(const Duration(seconds: 31));
      expect(reads(broker), before + 2, reason: 'Gentle fallback while down.');
      updates.ready();
      await tester.pump();
      await tester.pump();
      final connected = reads(broker);
      expect(connected, before + 3, reason: 'One read on (re)connection.');
      updates.connected = false;
      updates.notifyListeners();
      await tester.pump(const Duration(seconds: 16));
      expect(reads(broker), connected + 1);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets('every running-without-Host state of the broker is drawn', (
    tester,
  ) async {
    for (final (vmState, headline) in [
      ('windows_starting', 'Windows를 시작하고 있어요'),
      ('host_wait', '연결을 준비하고 있어요'),
      ('host_lost', 'Host 연결이 끊겼어요'),
    ]) {
      await open(tester, [state(vmState)]);
      expect(find.text(headline), findsWidgets, reason: vmState);
      await tester.pumpWidget(const SizedBox.shrink());
    }
  });

  testWidgets(
    'boot steps with the Host connected are progress on the Android tile, not warnings',
    (tester) async {
      // windows_login is not a waiting step: the tile offers Console at
      // once, see the test below.
      for (final (reason, prep, label) in [
        ('display_preparing', StreamPreparation.display, '화면 준비 중'),
        ('session_preparing', StreamPreparation.session, '연결 준비 중'),
      ]) {
        final base = state('preparing');
        final desktop = BrokerDesktop(
          id: base.id,
          name: base.name,
          kind: base.kind,
          description: base.description,
          os: base.os,
          status: 'ready',
          canConnect: false,
          hostConnected: true,
          interactiveSession: false,
          streamPreparation: prep,
          power: base.power,
          vmState: 'preparing',
          vmStateReason: reason,
          vmStateRevision: 1,
        );
        await open(tester, [desktop]);
        expect(find.text(label), findsWidgets, reason: reason);
        expect(find.text('Windows에 로그인돼 있지 않아요'), findsNothing, reason: reason);
        expect(find.text('가상 화면이 준비되지 않았어요'), findsNothing, reason: reason);
        final button = tester.widget<FilledButton>(mainButton(desktop.id));
        expect(button.onPressed, isNull, reason: reason);
        await tester.pumpWidget(const SizedBox.shrink());
      }
    },
  );

  testWidgets(
    'a VM signed out of Windows offers Console on its main button at once',
    (tester) async {
      final base = state('preparing');
      final desktop = BrokerDesktop(
        id: base.id,
        name: base.name,
        kind: base.kind,
        description: base.description,
        os: base.os,
        status: 'ready',
        canConnect: false,
        hostConnected: true,
        interactiveSession: false,
        streamPreparation: StreamPreparation.login,
        power: base.power,
        vmState: 'preparing',
        vmStateReason: 'windows_login',
        vmStateRevision: 1,
      );
      await open(tester, [desktop]);
      expect(find.text('Windows에 로그인돼 있지 않아요'), findsWidgets);
      expect(find.text('Console로 열기'), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );
}

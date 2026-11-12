// PRIVATE universal-confirmation RED. Real AndroidApp and real dialog, fake API.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/stream_options.dart' show RemoteMonitor;
import 'package:multitap_client/models/session_cleanup.dart';

import '../helpers/stream_connect_consent.dart' show acceptStreamConnect;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore, mainButton;

BrokerDesktop _desktop({bool pending = false, bool off = false}) => BrokerDesktop(
  id: 'pve-149', name: 'ToTBCode', kind: 'vm', description: '', os: 'Windows',
  status: off ? 'powered_off' : 'online', canConnect: !off, canStream: !pending && !off,
  hostConnected: !off, interactiveSession: !off, virtualDisplayReady: !off,
  power: BrokerPowerSnapshot(state: off ? 'stopped' : 'running', stale: false),
  // A connect-ready synthetic Host has one already-owned, fresh output.
  display: VirtualDisplayState(supported: true, activeCount: 1, stale: false,
    checkedAt: DateTime.now()),
  monitors: const [RemoteMonitor(id: '0', width: 1920, height: 1080)],
  sessionCleanup: pending ? SessionCleanupState(pending: true, phase: 'pending',
    checkedAt: DateTime.now()) : const SessionCleanupState(),
);

/// List reads wait while [hold] is set.
class _HeldListBroker extends FakeBroker {
  _HeldListBroker(super.inventory);
  Completer<void>? hold;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    calls.add('desktops');
    await hold?.future;
    return inventory;
  }
}

class _Fixture {
  _Fixture({bool pending = false, bool off = false})
      : api = FakeBroker([_desktop(pending: pending, off: off)]);
  _Fixture.of(this.api);
  final FakeBroker api;
  DateTime now = DateTime.now();
  int get connects => api.calls.where((v) => v == 'connect').length;
  Future<void> mount(WidgetTester t) async {
    t.view.physicalSize = const Size(1080, 2400);
    t.view.devicePixelRatio = 3;
    addTearDown(t.view.reset);
    MemoryStore(origin: 'https://fixture.invalid', username: 'tester',
      prefs: {'launcher.view': 'cards'}).install();
    await t.pumpWidget(AndroidVdiApp(apiFactory: (_) => api, clock: () => now));
    await t.pumpAndSettle();
    await t.enterText(find.byKey(const Key('sign-in-password')), 'synthetic-only');
    final login = find.byKey(const Key('login-submit'));
    await t.ensureVisible(login);
    await t.tap(login);
    await t.pumpAndSettle();
  }
  Future<void> enter(WidgetTester t, String entry) async {
    if (entry == 'quality') {
      await t.tap(find.byKey(const Key('stream-modes-pve-149')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('stream-settings-pve-149')));
      await t.pumpAndSettle();
      final button = find.descendant(of: find.byType(AndroidStreamDialog),
        matching: find.widgetWithText(FilledButton, '연결'));
      await t.ensureVisible(button);
      await t.tap(button);
    } else {
      final button = mainButton('pve-149');
      await t.ensureVisible(button);
      await t.tap(button);
    }
    await t.pump(const Duration(milliseconds: 350));
  }
  Future<void> close(WidgetTester t) async {
    await t.pumpWidget(const SizedBox.shrink());
    await t.pump(const Duration(seconds: 1));
  }
}

void main() {
  setUp(() {
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(const MethodChannel('multitap/pen-input'), (_) async => null);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? 'started' : true);
  });
  tearDown(() {
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(const MethodChannel('multitap/pen-input'), null);
    messenger.setMockMethodCallHandler(AndroidStore.channel, null);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });

  for (final entry in ['card', 'quality', 'queued']) {
    testWidgets('Android $entry fresh Connect asks before any route or reservation', (t) async {
      final f = _Fixture(pending: entry == 'queued');
      try {
        await f.mount(t);
        await f.enter(t, entry);
        final dialog = find.byKey(const Key('connect-confirm-dialog'));
        expect(dialog, findsOneWidget, reason: 'Idle and occupied fresh stream intents both require the first question');
        expect(find.text('ToTBCode에 연결할까요?'), findsOneWidget);
        expect(f.connects, 0);
        expect(find.byType(AndroidRemoteView), findsNothing);
        expect(find.byKey(const Key('cancel-start-pve-149')), findsNothing);
        await t.tap(find.byKey(const Key('cancel-connect-pve-149')));
        await t.pump();
        await t.pump(const Duration(milliseconds: 350));
        await t.pump();
        expect(dialog, findsNothing);
        expect(f.connects, 0);
        expect(find.byType(AndroidRemoteView), findsNothing);
        expect(find.byKey(const Key('cancel-start-pve-149')), findsNothing);
        await f.enter(t, entry);
        await acceptStreamConnect(t, 'pve-149');
        if (entry == 'queued') {
          expect(f.connects, 0);
          expect(find.byKey(const Key('cancel-start-pve-149')), findsOneWidget);
          f.api.inventory = [_desktop()];
          final refresh = find.byKey(const Key('refresh-desktops'));
          await t.ensureVisible(refresh);
          await t.tap(refresh);
          await t.pump();
        }
        for (var n = 0; n < 12 && f.connects == 0; n++) {
          await t.pump(const Duration(milliseconds: 50));
        }
        expect(f.connects, 1);
        expect(dialog, findsNothing);
      } finally { await f.close(t); }
    });
  }

  testWidgets('Android expiry while confirmation waits cannot reserve or open a stream', (t) async {
    final f = _Fixture();
    try {
      await f.mount(t);
      await f.enter(t, 'card');
      expect(find.byKey(const Key('connect-confirm-dialog')), findsOneWidget);
      f.now = f.now.add(const Duration(hours: 2));
      await acceptStreamConnect(t, 'pve-149');
      expect(f.connects, 0);
      expect(find.byType(AndroidRemoteView), findsNothing);
    } finally { await f.close(t); }
  });

  testWidgets('Android stopped target has no stream dispatch or streaming question', (t) async {
    final f = _Fixture(off: true);
    try {
      await f.mount(t);
      expect(find.byKey(const Key('open-pve-149')), findsNothing);
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(f.connects, 0);
    } finally { await f.close(t); }
  });

  // The wait must not stay until the list has been read again: until then
  // the card would show its step line with the whole session's time and
  // 취소.
  testWidgets('Android queued connection: its wait is gone as soon as the stream ends', (t) async {
    final api = _HeldListBroker([_desktop(pending: true)]);
    final f = _Fixture.of(api);
    try {
      await f.mount(t);
      await f.enter(t, 'queued');
      await acceptStreamConnect(t, 'pve-149');
      expect(find.byKey(const Key('cancel-start-pve-149')), findsOneWidget);
      api.inventory = [_desktop()];
      final refresh = find.byKey(const Key('refresh-desktops'));
      await t.ensureVisible(refresh);
      await t.tap(refresh);
      await t.pump();
      for (var n = 0; n < 12 && f.connects == 0; n++) {
        await t.pump(const Duration(milliseconds: 50));
      }
      expect(f.connects, 1);
      await t.pump(const Duration(milliseconds: 400));
      expect(find.byType(AndroidRemoteView), findsOneWidget);
      // The stream ends; the list read that follows is held.
      api.hold = Completer<void>();
      final reads = api.calls.where((v) => v == 'desktops').length;
      await t.tap(find.byKey(const Key('android-leave')));
      for (var n = 0; n < 40 && find.byType(AndroidRemoteView).evaluate().isNotEmpty; n++) {
        await t.pump(const Duration(milliseconds: 100));
      }
      expect(find.byType(AndroidRemoteView), findsNothing);
      expect(api.calls.where((v) => v == 'desktops').length, reads + 1,
        reason: 'the list is being read again');
      expect(find.byKey(const Key('cancel-start-pve-149')), findsNothing);
      expect(mainButton('pve-149'), findsOneWidget);
      api.hold!.complete();
      await t.pump();
      await t.pump(const Duration(milliseconds: 100));
      expect(find.byKey(const Key('cancel-start-pve-149')), findsNothing);
      expect(f.connects, 1);
    } finally { await f.close(t); }
  });
}

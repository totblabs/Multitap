import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_watch.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestBroker;

BrokerDesktop liveDesktop(String state, {bool host = false}) => BrokerDesktop(
  id: 'live-vm',
  name: '실시간 검증 VM',
  kind: 'vm',
  description: '',
  os: 'Windows',
  status: host ? 'ready' : 'host_offline',
  canConnect: host && state == 'running',
  hostConnected: host,
  power: BrokerPowerSnapshot(
    state: state,
    stale: false,
    checkedAt: DateTime.utc(2026, 10, 23),
  ),
  powerCanForceStop: state == 'running',
);

class LiveBroker extends TestBroker {
  int reads = 0;
  Completer<List<BrokerDesktop>>? delayed;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    reads++;
    return delayed?.future ?? assigned;
  }
}

class LiveUpdates extends ChangeNotifier implements DesktopUpdates {
  final controller = StreamController<void>.broadcast();
  int starts = 0;
  bool closed = false;
  @override
  bool connected = false;
  @override
  Stream<void> get changes => controller.stream;
  @override
  void start() {
    starts++;
  }

  void ready() {
    connected = true;
    notifyListeners();
    controller.add(null);
  }

  void changed() {
    controller.add(null);
  }

  @override
  Future<void> close() async {
    closed = true;
    connected = false;
    await controller.close();
  }
}

void main() {
  Future<void> login(
    WidgetTester t,
    LiveBroker api, {
    DesktopWatchFactory? watch,
  }) async {
    t.view.physicalSize = const Size(1440, 900);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    await t.pumpWidget(
      MultitapApp(
        api: api,
        desktopWatchFactory: watch,
        serverOrigin: Uri.parse('https://cloud.example.com'),
      ),
    );
    await t.pumpAndSettle();
    await t.enterText(find.byKey(const Key('sign-in-id')), 'qa-user');
    await t.enterText(find.byKey(const Key('sign-in-password')), 'qa-password');
    await t.tap(find.byKey(const Key('login-submit')));
    await t.pumpAndSettle();
  }

  WorkspaceShell workspace(WidgetTester t) =>
      t.widget<WorkspaceShell>(find.byType(WorkspaceShell));

  testWidgets(
    'Authenticated desktop events refresh immediately, drain a newer change and close on confirmed logout',
    (t) async {
      final api = LiveBroker()..assigned = [liveDesktop('running', host: true)];
      final updates = LiveUpdates();
      await login(
        t,
        api,
        watch: (origin, token) {
          expect(origin, Uri.parse('https://cloud.example.com'));
          expect(token, 'test-token');
          return updates;
        },
      );
      expect(updates.starts, 1);
      updates.ready();
      for (var i = 0; i < 4; i++) {
        await t.pump();
      }
      final reads = api.reads;
      api.assigned = [liveDesktop('stopped')];
      updates.changed();
      for (var i = 0; i < 4; i++) {
        await t.pump();
      }
      expect(api.reads, reads + 1);
      expect(workspace(t).machines.single.powerState, 'stopped');
      api.delayed = Completer<List<BrokerDesktop>>();
      updates.changed();
      await t.pump();
      final delayedReads = api.reads;
      updates.changed();
      await t.pump();
      expect(api.reads, delayedReads);
      final delayed = api.delayed!;
      api.delayed = null;
      api.assigned = [liveDesktop('running')];
      delayed.complete([liveDesktop('stopped')]);
      for (var i = 0; i < 8; i++) {
        await t.pump();
      }
      expect(api.reads, delayedReads + 1);
      expect(workspace(t).machines.single.powerState, 'running');
      await t.tap(find.byKey(const Key('workspace-account')));
      await t.pumpAndSettle();
      await t.tap(find.text('로그아웃'));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('confirm-sign-out')));
      await t.pumpAndSettle();
      expect(updates.closed, isTrue);
      expect(find.byType(WorkspaceShell), findsNothing);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'While the live channel is connected nothing polls: a pushed change is read once, silence reads nothing',
    (t) async {
      final api = LiveBroker()..assigned = [liveDesktop('running', host: true)];
      final updates = LiveUpdates();
      await login(t, api, watch: (_, _) => updates);
      updates.ready();
      for (var i = 0; i < 4; i++) {
        await t.pump();
      }
      final before = api.reads;
      await t.pump(const Duration(seconds: 60));
      expect(api.reads, before, reason: 'No timer reads while the push channel is up.');
      // Stopped, then started, then the Host connects: each one push, one read.
      for (final next in [
        liveDesktop('stopped'),
        liveDesktop('running'),
        liveDesktop('running', host: true),
      ]) {
        api.assigned = [next];
        updates.changed();
        for (var i = 0; i < 4; i++) {
          await t.pump();
        }
      }
      expect(api.reads, before + 3);
      expect(workspace(t).machines.single.isReady, isTrue);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Without a live channel the list is read gently (every 10 s), also while the window is merely unfocused',
    (t) async {
      final api = LiveBroker()..assigned = [liveDesktop('running', host: true)];
      await login(t, api);
      final before = api.reads;
      api.assigned = [liveDesktop('stopped')];
      await t.pump(const Duration(seconds: 6));
      expect(api.reads, before, reason: 'Not every 2 s any more.');
      // Windows reports an unfocused window as inactive; it is still visible.
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      await t.pump();
      final unfocused = api.reads;
      await t.pump(const Duration(seconds: 10));
      await t.pump();
      expect(api.reads, greaterThan(unfocused), reason: 'Unfocused is not hidden.');
      expect(api.reads - unfocused, lessThanOrEqualTo(2));
      expect(workspace(t).machines.single.powerState, 'stopped');
      expect(workspace(t).machines.single.canStart, isTrue);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'A hidden window pauses reading and returning reads once without overlapping a delayed request',
    (t) async {
      final api = LiveBroker()..assigned = [liveDesktop('running')];
      await login(t, api);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      final before = api.reads;
      await t.pump(const Duration(seconds: 30));
      expect(api.reads, before);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await t.pump();
      expect(api.reads, before + 1);
      api.delayed = Completer<List<BrokerDesktop>>();
      await t.pump(const Duration(seconds: 12));
      final pendingReads = api.reads;
      expect(pendingReads, before + 2);
      await t.pump(const Duration(seconds: 12));
      expect(
        api.reads,
        pendingReads,
        reason: 'A slow broker must not accumulate polling requests.',
      );
      api.delayed!.complete([liveDesktop('stopped')]);
      for (var i = 0; i < 4; i++) {
        await t.pump();
      }
      expect(workspace(t).machines.single.powerState, 'stopped');
      await t.pumpWidget(const SizedBox.shrink());
    },
  );
}

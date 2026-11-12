import 'dart:async';
import 'dart:collection';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_watch.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestBroker;
import 'workspace_live_state_test.dart' show LiveUpdates, liveDesktop;

class RaceBroker extends TestBroker {
  int reads = 0;
  bool failLists = false;
  bool shortFirstSession = false;
  Completer<void>? passwordGate, logoutGate;
  final powerGates = <String, Completer<BrokerPowerResult>>{};
  final queue = Queue<Completer<List<BrokerDesktop>>>();
  final pending = <Completer<List<BrokerDesktop>>>[];
  final started = <Completer<List<BrokerDesktop>>>{};
  Completer<List<BrokerDesktop>> delayNext() {
    final value = Completer<List<BrokerDesktop>>();
    queue.add(value);
    pending.add(value);
    return value;
  }

  @override
  Future<BrokerSession> login(String username, String password) async {
    final session = await super.login(username, password);
    if (!shortFirstSession || logins != 1) return session;
    return BrokerSession(
      token: session.token,
      username: session.username,
      expiresAt: DateTime.now().add(const Duration(seconds: 1)),
    );
  }

  @override
  Future<void> logout(String token) async {
    await super.logout(token);
    await logoutGate?.future;
  }

  @override
  Future<void> changePassword(
    String token,
    String currentPassword,
    String newPassword,
    String confirmation,
  ) async {
    await super.changePassword(
      token,
      currentPassword,
      newPassword,
      confirmation,
    );
    await passwordGate?.future;
  }

  @override
  Future<BrokerPowerResult> requestPower(
    String token,
    String desktopId,
    String action,
  ) =>
      powerGates[desktopId]?.future ??
      super.requestPower(token, desktopId, action);

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    reads++;
    if (queue.isNotEmpty) {
      final value = queue.removeFirst();
      started.add(value);
      return value.future;
    }
    if (failLists) throw const BrokerException('network_error', 503);
    return assigned;
  }

  void resolvePending() {
    for (final value in pending) {
      if (!value.isCompleted) value.complete(assigned);
    }
    for (final entry in powerGates.entries) {
      if (!entry.value.isCompleted) {
        entry.value.complete(completedPower(entry.key, 'running'));
      }
    }
  }
}

BrokerDesktop namedDesktop(String id, String state) => BrokerDesktop(
  id: id,
  name: id,
  kind: 'vm',
  description: '',
  os: 'Windows',
  status: 'host_offline',
  canConnect: false,
  power: BrokerPowerSnapshot(state: state, stale: false),
);

BrokerPowerResult completedPower(String id, String state) => BrokerPowerResult(
  power: BrokerPowerSnapshot(state: state, stale: false),
  task: BrokerPowerTask(
    id: '70caf0b5-b625-445d-82cc-5363107a4c86',
    desktopId: id,
    action: 'shutdown',
    status: 'stopped',
    exitStatus: 'OK',
  ),
);

class ThrowingListenStream extends Stream<void> {
  @override
  StreamSubscription<void> listen(
    void Function(void)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => throw StateError('watch listen unavailable');
}

class FailingUpdates extends LiveUpdates {
  FailingUpdates(this.stage);
  final String stage;

  @override
  Stream<void> get changes =>
      stage == 'listen' ? ThrowingListenStream() : super.changes;

  @override
  void start() {
    if (stage == 'start') throw StateError('watch start unavailable');
    super.start();
  }
}

Future<void> drain(WidgetTester tester, [int count = 8]) async {
  for (var i = 0; i < count; i++) {
    await tester.pump();
  }
}

WorkspaceShell workspace(WidgetTester tester) =>
    tester.widget<WorkspaceShell>(find.byType(WorkspaceShell));

Future<void> mount(
  WidgetTester tester,
  RaceBroker api,
  DesktopWatchFactory factory,
) async {
  tester.view.physicalSize = const Size(1440, 900);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  addTearDown(() async {
    api.resolvePending();
    await drain(tester);
    await tester.pumpWidget(const SizedBox.shrink());
    await drain(tester);
  });
  await tester.pumpWidget(
    MultitapApp(
      api: api,
      serverOrigin: Uri.parse('https://cloud.example.com'),
      desktopWatchFactory: factory,
    ),
  );
  await tester.pumpAndSettle();
}

Future<SignInResult> signIn(
  WidgetTester tester, [
  String name = 'first-user',
]) async {
  SignInResult? result;
  final request = tester
      .widget<SignInScreen>(find.byType(SignInScreen))
      .onLogin(name, 'test-password');
  unawaited(request.then((value) => result = value));
  await drain(tester);
  expect(result, isNotNull);
  return result!;
}

void main() {
  testWidgets(
    'an older quiet response cannot restore a VM removed by a newer manual refresh',
    (tester) async {
      final api = RaceBroker()..assigned = [liveDesktop('running')];
      final updates = LiveUpdates();
      await mount(tester, api, (_, _) => updates);
      expect(await signIn(tester), SignInResult.success);
      updates.ready();
      await drain(tester);
      final older = api.delayNext();
      updates.changed();
      await drain(tester);
      expect(api.started, contains(older));
      final newer = api.delayNext();
      workspace(tester).onRefresh();
      await drain(tester);
      // Accept either serialized/coalesced fetches or generation-guarded overlap.
      if (api.started.contains(newer)) {
        newer.complete([]);
        await drain(tester);
        expect(workspace(tester).machines, isEmpty);
        older.complete([liveDesktop('running')]);
        await drain(tester);
      } else {
        older.complete([liveDesktop('running')]);
        await drain(tester);
        expect(api.started, contains(newer));
        newer.complete([]);
        await drain(tester);
      }
      expect(
        workspace(tester).machines,
        isEmpty,
        reason: 'A response begun before the removal must not restore an old assignment card.',
      );
    },
  );

  testWidgets(
    'a pending quiet read from a signed-out account cannot block the new watch',
    (tester) async {
      final api = RaceBroker()..assigned = [liveDesktop('running')];
      final watches = <LiveUpdates>[];
      await mount(tester, api, (_, _) {
        final watch = LiveUpdates();
        watches.add(watch);
        return watch;
      });
      expect(await signIn(tester), SignInResult.success);
      final old = api.delayNext();
      watches.single.changed();
      await drain(tester);
      expect(api.started, contains(old));
      workspace(tester).onSignOut();
      await drain(tester);
      api.assigned = [liveDesktop('stopped')];
      expect(await signIn(tester, 'second-user'), SignInResult.success);
      final before = api.reads;
      watches.last.ready();
      await drain(tester);
      expect(
        api.reads,
        greaterThan(before),
        reason: 'A stale read must not hold a global quiet-refresh latch for the next login.',
      );
      old.complete([liveDesktop('running')]);
      await drain(tester);
      expect(workspace(tester).displayName, 'second-user');
      expect(workspace(tester).machines.single.powerState, 'stopped');
    },
  );

  testWidgets(
    'optional watch factory failure cannot turn successful authentication into failed login',
    (tester) async {
      final api = RaceBroker()..assigned = [liveDesktop('running')];
      await mount(tester, api, (_, _) => throw StateError('watch unavailable'));
      expect(await signIn(tester), SignInResult.success);
      expect(workspace(tester).machines.single.id, 'live-vm');
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'watch factory failure on resume still runs fallback refresh without observer errors',
    (tester) async {
      final api = RaceBroker()..assigned = [liveDesktop('running')];
      var factories = 0;
      await mount(tester, api, (_, _) {
        if (++factories > 1) throw StateError('resume watch unavailable');
        return LiveUpdates();
      });
      expect(await signIn(tester), SignInResult.success);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
      await drain(tester);
      final before = api.reads;
      api.assigned = [liveDesktop('stopped')];
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await drain(tester);
      expect(tester.takeException(), isNull);
      expect(api.reads, greaterThan(before));
      expect(workspace(tester).machines.single.powerState, 'stopped');
    },
  );

  testWidgets(
    'repeated watch invalidations cannot create a quiet HTTP failure storm',
    (tester) async {
      final api = RaceBroker()..assigned = [liveDesktop('running')];
      final updates = LiveUpdates();
      await mount(tester, api, (_, _) => updates);
      expect(await signIn(tester), SignInResult.success);
      updates.ready();
      await drain(tester);
      api.failLists = true;
      final before = api.reads;
      for (var i = 0; i < 20; i++) {
        updates.changed();
        await drain(tester, 3);
      }
      expect(
        api.reads - before,
        lessThanOrEqualTo(2),
        reason: 'An unavailable HTTP API must use backoff even while its event socket is alive.',
      );
      expect(workspace(tester).machines.single.id, 'live-vm');
    },
  );

  for (final stage in ['listen', 'start']) {
    testWidgets(
      'optional watch $stage failure closes resources and keeps fallback polling',
      (tester) async {
        final api = RaceBroker()..assigned = [liveDesktop('running')];
        final updates = FailingUpdates(stage);
        await mount(tester, api, (_, _) => updates);
        expect(await signIn(tester), SignInResult.success);
        expect(updates.closed, isTrue);
        expect(tester.takeException(), isNull);
        final before = api.reads;
        api.assigned = [liveDesktop('stopped')];
        await tester.pump(const Duration(seconds: 12));
        await drain(tester);
        expect(api.reads, greaterThan(before));
        expect(workspace(tester).machines.single.powerState, 'stopped');
      },
    );
  }

  testWidgets(
    'old password change completion cannot sign out a replacement account',
    (tester) async {
      final api = RaceBroker()
        ..assigned = [liveDesktop('running')]
        ..shortFirstSession = true
        ..passwordGate = Completer<void>();
      final watches = <LiveUpdates>[];
      await mount(tester, api, (_, _) {
        final watch = LiveUpdates();
        watches.add(watch);
        return watch;
      });
      expect(await signIn(tester), SignInResult.success);
      final pending = workspace(tester).onChangePassword!(
        'old-password',
        'replacement-password',
        'replacement-password',
      );
      await drain(tester);
      await tester.pump(const Duration(seconds: 2));
      await drain(tester);
        await tester.tap(find.byKey(const ValueKey('session-end-ack')));
        await drain(tester);
      expect(find.byType(SignInScreen), findsOneWidget);
      expect(await signIn(tester, 'second-user'), SignInResult.success);
      api.passwordGate!.complete();
      await pending;
      await drain(tester);
      expect(workspace(tester).displayName, 'second-user');
      expect(watches.last.closed, isFalse);
    },
  );

  testWidgets(
    'old logout 401 cannot expire a replacement account after original expiry',
    (tester) async {
      final api = RaceBroker()
        ..assigned = [liveDesktop('running')]
        ..shortFirstSession = true
        ..logoutGate = Completer<void>();
      final watches = <LiveUpdates>[];
      await mount(tester, api, (_, _) {
        final watch = LiveUpdates();
        watches.add(watch);
        return watch;
      });
      expect(await signIn(tester), SignInResult.success);
      workspace(tester).onSignOut();
      await drain(tester);
      await tester.pump(const Duration(seconds: 2));
      await drain(tester);
        await tester.tap(find.byKey(const ValueKey('session-end-ack')));
        await drain(tester);
      expect(find.byType(SignInScreen), findsOneWidget);
      expect(await signIn(tester, 'second-user'), SignInResult.success);
      api.logoutGate!.completeError(
        const BrokerException('session_expired', 401),
      );
      await drain(tester);
      expect(find.byType(WorkspaceShell), findsOneWidget);
      expect(workspace(tester).displayName, 'second-user');
      expect(watches.last.closed, isFalse);
    },
  );

  testWidgets(
    'completion of a second VM power task invalidates a list begun by the first',
    (tester) async {
      final api = RaceBroker()
        ..assigned = [
          namedDesktop('vm-a', 'running'),
          namedDesktop('vm-b', 'running'),
        ];
      api.powerGates['vm-a'] = Completer<BrokerPowerResult>();
      api.powerGates['vm-b'] = Completer<BrokerPowerResult>();
      await mount(tester, api, (_, _) => LiveUpdates());
      expect(await signIn(tester), SignInResult.success);
      final first = workspace(tester).onPower!('vm-a', 'shutdown');
      final second = workspace(tester).onPower!('vm-b', 'shutdown');
      await drain(tester);
      final older = api.delayNext();
      api.powerGates['vm-a']!.complete(completedPower('vm-a', 'stopped'));
      await drain(tester);
      expect(api.started, contains(older));
      api.assigned = [
        namedDesktop('vm-a', 'stopped'),
        namedDesktop('vm-b', 'stopped'),
      ];
      api.powerGates['vm-b']!.complete(completedPower('vm-b', 'stopped'));
      await drain(tester);
      expect(workspace(tester).machines.last.powerState, 'stopped');
      older.complete([
        namedDesktop('vm-a', 'stopped'),
        namedDesktop('vm-b', 'running'),
      ]);
      await Future.wait([first, second]);
      await drain(tester);
      expect(
        workspace(tester).machines.last.powerState,
        'stopped',
        reason: 'A list begun before the second completed power task must not undo its verified state.',
      );
    },
  );
}

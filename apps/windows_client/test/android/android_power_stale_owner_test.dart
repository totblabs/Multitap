// SOURCE ONLY: this private addition has not been compiled or run by auxiliary.
// Stage at test/android/android_power_stale_owner_test.dart in the main-owned
// app snapshot. Exercise production AndroidVdiApp, never a copied power flow.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/data/broker_api.dart';

import '../pc_login_replacement_lifetime_test.dart' show AuthWatch;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore, vm;
import 'android_updates_test.dart' show FakePlatform, FakeTransport;

const _vmId = 'pve-149';
const _pcId = 'pc-a1234567-89ab-cdef-8123-0123456789ab';
const _taskId = '6f1b4f7e-2c9d-4b8a-9e3f-0123456789ab';

class _Clock {
  DateTime now = DateTime.utc(2026, 11, 7);
  DateTime call() => now;
}

class _PowerCall {
  _PowerCall(this.token, this.desktopId, this.action);
  final String token, desktopId, action;
  final reply = Completer<BrokerPowerResult>();
}

BrokerPowerResult _result(
  String desktopId,
  String action, {
  bool finished = true,
}) => BrokerPowerResult(
  power: const BrokerPowerSnapshot(state: 'running', stale: false),
  task: BrokerPowerTask(
    id: _taskId,
    desktopId: desktopId,
    action: action,
    status: finished ? 'stopped' : 'running',
    exitStatus: finished ? 'OK' : null,
  ),
);

BrokerDesktop _offlinePc({bool wake = true}) => BrokerDesktop(
  id: _pcId,
  name: 'SYNTHETIC-PC',
  kind: 'pc',
  description: '',
  os: 'Windows',
  status: 'host_offline',
  canConnect: false,
  localPcApproved: true,
  hostConnected: false,
  pcPower: PcPowerState(
    supported: true,
    actions: const {'start'},
    wake: wake,
  ),
);

class _Broker extends FakeBroker implements SessionRenewalBrokerApi {
  _Broker(super.inventory, this.clock);
  final _Clock clock;
  final requests = <_PowerCall>[];
  final polls = <_PowerCall>[];
  final logins = <BrokerSession>[];
  final logoutTokens = <String>[];
  final renewalTokens = <String>[];
  final inventoryTokens = <String>[];
  bool pollBeforeCompletion = false;

  @override
  Future<BrokerSession> login(String username, String password) async {
    final session = BrokerSession(
      token: 'synthetic-owner-${logins.length + 1}',
      accountId: 'synthetic-account-$username',
      username: username,
      expiresAt: clock.now.add(const Duration(minutes: 10)),
    );
    logins.add(session);
    return session;
  }

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    inventoryTokens.add(token);
    return inventory;
  }

  @override
  Future<void> logout(String token) async => logoutTokens.add(token);

  @override
  Future<BrokerPowerResult> requestPower(
    String token,
    String desktopId,
    String action,
  ) {
    final call = _PowerCall(token, desktopId, action);
    requests.add(call);
    if (pollBeforeCompletion) {
      call.reply.complete(_result(desktopId, action, finished: false));
    }
    return call.reply.future;
  }

  @override
  Future<BrokerPowerResult> power(
    String token,
    String desktopId, {
    String? taskId,
  }) {
    expect(taskId, _taskId, reason: 'Production must poll the admitted task.');
    final request = requests.lastWhere((call) => call.desktopId == desktopId);
    final call = _PowerCall(token, desktopId, request.action);
    polls.add(call);
    return call.reply.future;
  }

  @override
  Future<DateTime> renewSession(String token) async {
    renewalTokens.add(token);
    return clock.now.add(const Duration(minutes: 10));
  }

  @override
  void recordSessionRenewal(
    String token, {
    required int stage,
    required bool ok,
    String? code,
  }) {}

  void drainFixtureReplies() {
    for (final call in [...requests, ...polls]) {
      if (!call.reply.isCompleted) {
        call.reply.complete(_result(call.desktopId, call.action));
      }
    }
  }
}

// Bounded frame pumps also work while production shows an indeterminate power
// spinner. pumpAndSettle would otherwise wait for a deliberately held reply.
Future<void> _frames(WidgetTester tester) async {
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 400));
  await tester.pump();
}

Future<void> _tap(WidgetTester tester, Finder finder) async {
  expect(finder, findsOneWidget);
  await tester.ensureVisible(finder);
  await tester.tap(finder);
  await _frames(tester);
}

class _App {
  _App(this.broker);
  final _Broker broker;
  final watches = <AuthWatch>[];

  Future<void> mount(WidgetTester tester) async {
    tester.view.physicalSize = const Size(1080, 2400);
    tester.view.devicePixelRatio = 3;
    addTearDown(tester.view.reset);
    const pen = MethodChannel('multitap/pen-input');
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(pen, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? true : null,
    );
    MemoryStore(
      origin: 'https://vdi.example',
      username: 'old-user',
      prefs: {'launcher.view': 'cards'},
    ).install();
    final platform = FakePlatform();
    addTearDown(() async {
      await tester.pumpWidget(const SizedBox.shrink());
      broker.drainFixtureReplies();
      await _frames(tester);
      await platform.status.close();
      for (final channel in [pen, AndroidSessionKeepalive.channel, AndroidStore.channel]) {
        messenger.setMockMethodCallHandler(channel, null);
      }
    });
    await tester.pumpWidget(AndroidVdiApp(
      apiFactory: (_) => broker,
      clock: broker.clock.call,
      desktopWatchFactory: (_, _) {
        final watch = AuthWatch();
        watches.add(watch);
        return watch;
      },
      updatesFactory: (origin) => AndroidUpdates(
        origin,
        transport: FakeTransport({}),
      ),
      updatePlatform: platform,
    ));
    await tester.pumpAndSettle();
    await login(tester, 'old-user');
    expect(watches, hasLength(1));
  }

  Future<void> login(WidgetTester tester, String username) async {
    await tester.enterText(find.byKey(const Key('sign-in-id')), username);
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'synthetic-only',
    );
    await _tap(tester, find.byKey(const Key('login-submit')));
    expect(find.byKey(const Key('login-submit')), findsNothing);
    expect(broker.logins.last.username, username);
  }

  Future<void> replaceBySignOut(WidgetTester tester) async {
    await _tap(tester, find.byKey(const Key('account-menu')));
    await _tap(tester, find.byKey(const Key('sign-out')));
    await _tap(tester, find.byKey(const Key('confirm-sign-out')));
    expect(find.byKey(const Key('login-submit')), findsOneWidget);
    await login(tester, 'replacement-user');
    expect(watches, hasLength(2));
    expect(broker.logoutTokens, [broker.logins.first.token]);
  }

  Future<void> confirmation(
    WidgetTester tester,
    String desktopId,
    String action,
  ) async {
    await _tap(tester, find.byKey(Key('desktop-power-$desktopId')));
    await _tap(tester, find.byKey(Key('power-$action-$desktopId')));
    expect(find.byKey(Key('confirm-power-$action-$desktopId')), findsOneWidget);
  }

  Future<void> start(
    WidgetTester tester,
    String desktopId,
    String action,
  ) async {
    await confirmation(tester, desktopId, action);
    await _tap(tester, find.byKey(Key('confirm-power-$action-$desktopId')));
    expect(broker.requests, hasLength(1));
  }

  void expectCurrentOwnerUnaffected() {
    expect(find.byKey(const Key('login-submit')), findsNothing);
    expect(find.byKey(const ValueKey('session-end-ack')), findsNothing);
    expect(find.byKey(const Key('pc-wake-help')), findsNothing);
    expect(find.byType(SnackBar), findsNothing);
    expect(broker.logins, hasLength(2));
    expect(broker.logoutTokens, [broker.logins.first.token]);
    expect(broker.requests, hasLength(1));
    expect(broker.requests.single.token, broker.logins.first.token);
    expect(broker.polls.every((call) => call.token == broker.logins.first.token), isTrue);
    expect(broker.renewalTokens, isEmpty);
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  for (final poll in [false, true]) {
    for (final outcome in ['success', 'expired', 'network', 'wake']) {
      testWidgets('late old ${poll ? 'poll' : 'request'} $outcome cannot affect replacement account', (tester) async {
        final pc = outcome == 'wake';
        final desktopId = pc ? _pcId : _vmId;
        final action = pc ? 'start' : 'reboot';
        final broker = _Broker([pc ? _offlinePc() : vm()], _Clock())
          ..pollBeforeCompletion = poll;
        final app = _App(broker);
        await app.mount(tester);
        await app.start(tester, desktopId, action);
        if (poll) {
          await tester.pump(const Duration(seconds: 1));
          await _frames(tester);
          expect(broker.polls, hasLength(1));
        }
        final held = poll ? broker.polls.single : broker.requests.single;
        expect(held.reply.isCompleted, isFalse);
        await app.replaceBySignOut(tester);
        final pollsBeforeReply = broker.polls.length;
        switch (outcome) {
          case 'success':
            held.reply.complete(_result(desktopId, action));
            break;
          case 'expired':
            held.reply.completeError(const BrokerException('session_expired', 401));
            break;
          case 'network':
            held.reply.completeError(const BrokerException('network_error'));
            break;
          case 'wake':
            held.reply.completeError(const BrokerException('wake_send_failed'));
            break;
        }
        await _frames(tester);
        app.expectCurrentOwnerUnaffected();
        expect(broker.polls, hasLength(pollsBeforeReply),
          reason: 'Late completion must not poll under either owner again.');
        // Exact old-operation cleanup must also release the tile for a fresh,
        // explicitly confirmed operation. Do not fix authority by leaking busy.
        broker.pollBeforeCompletion = false;
        await app.confirmation(tester, desktopId, action);
        await _tap(tester, find.byKey(Key('confirm-power-$action-$desktopId')));
        expect(broker.requests, hasLength(2));
        expect(broker.requests.last.token, broker.logins.last.token);
        broker.requests.last.reply.complete(_result(desktopId, action));
        await _frames(tester);
        expect(find.byType(SnackBar), findsNothing);
        expect(find.byKey(const ValueKey('session-end-ack')), findsNothing);
        expect(tester.takeException(), isNull);
      });
    }
  }

  testWidgets('a power confirmation open during real authentication retirement never starts under the replacement', (tester) async {
    final broker = _Broker([vm()], _Clock());
    final app = _App(broker);
    await app.mount(tester);
    await app.confirmation(tester, _vmId, 'reboot');
    expect(broker.requests, isEmpty);
    app.watches.first.terminal.add('pc_login_replaced');
    await _frames(tester);
    expect(find.byKey(const Key('confirm-power-reboot-pve-149')), findsNothing);
    await _tap(tester, find.byKey(const ValueKey('session-end-ack')));
    await app.login(tester, 'replacement-user');
    expect(broker.requests, isEmpty);
    expect(broker.polls, isEmpty);
    expect(find.byKey(const ValueKey('session-end-ack')), findsNothing);
    expect(find.byType(SnackBar), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('current owner general power failure retains actionable error feedback', (tester) async {
    final broker = _Broker([vm()], _Clock());
    final app = _App(broker);
    await app.mount(tester);
    await app.start(tester, _vmId, 'reboot');
    broker.requests.single.reply.completeError(const BrokerException('network_error'));
    await _frames(tester);
    expect(find.byType(SnackBar), findsOneWidget);
    expect(find.text(const BrokerException('network_error').message), findsOneWidget);
    expect(find.byKey(const ValueKey('session-end-ack')), findsNothing);
    expect(broker.logoutTokens, isEmpty);
    expect(tester.takeException(), isNull);
  });

  testWidgets('current owner definitive power 401 still retires that owner', (tester) async {
    final broker = _Broker([vm()], _Clock());
    final app = _App(broker);
    await app.mount(tester);
    await app.start(tester, _vmId, 'reboot');
    broker.requests.single.reply.completeError(const BrokerException('session_expired', 401));
    await _frames(tester);
    expect(find.byKey(const ValueKey('session-end-ack')), findsOneWidget);
    expect(broker.logins, hasLength(1));
    expect(broker.renewalTokens, isEmpty);
    expect(tester.takeException(), isNull);
  });

  testWidgets('current owner PC wake failure still explains requirements', (tester) async {
    final broker = _Broker([_offlinePc()], _Clock());
    final app = _App(broker);
    await app.mount(tester);
    await app.start(tester, _pcId, 'start');
    broker.requests.single.reply.completeError(const BrokerException('wake_send_failed'));
    await _frames(tester);
    expect(find.byKey(const Key('pc-wake-help')), findsOneWidget);
    expect(find.byKey(const Key('pc-wake-reason')), findsOneWidget);
    await _tap(tester, find.byKey(const Key('pc-wake-details')));
    expect(find.textContaining('Wake-on-LAN'), findsWidgets);
    await _tap(tester, find.byKey(const Key('pc-wake-help-close')));
    expect(find.byKey(const ValueKey('session-end-ack')), findsNothing);
    expect(find.byType(SnackBar), findsNothing);
    expect(tester.takeException(), isNull);
  });

  for (final duringConfirmation in [false, true]) {
    testWidgets('same-token renewal preserves ${duringConfirmation ? 'held confirmation' : 'held power result'} and no success notice', (tester) async {
      final clock = _Clock();
      final broker = _Broker([vm()], clock);
      final app = _App(broker);
      await app.mount(tester);
      if (duringConfirmation) {
        await app.confirmation(tester, _vmId, 'reboot');
      } else {
        await app.start(tester, _vmId, 'reboot');
      }
      clock.now = clock.now.add(const Duration(minutes: 5, seconds: 1));
      await tester.pump(const Duration(minutes: 5, seconds: 1));
      await _frames(tester);
      expect(broker.renewalTokens, [broker.logins.single.token]);
      expect(broker.logins, hasLength(1));
      if (duringConfirmation) {
        await _tap(tester, find.byKey(const Key('confirm-power-reboot-pve-149')));
      }
      expect(broker.requests, hasLength(1));
      expect(broker.requests.single.token, broker.logins.single.token);
      broker.requests.single.reply.complete(_result(_vmId, 'reboot'));
      await _frames(tester);
      expect(find.byKey(const ValueKey('session-end-ack')), findsNothing);
      expect(find.byKey(const Key('login-submit')), findsNothing);
      expect(find.byType(SnackBar), findsNothing);
      expect(tester.takeException(), isNull);
    });
  }

  for (final action in ['start', 'reboot', 'shutdown', 'stop']) {
    testWidgets('healthy current-owner $action has no success SnackBar', (tester) async {
      final broker = _Broker([
        vm(power: action == 'start' ? 'stopped' : 'running', stream: action != 'start'),
      ], _Clock());
      final app = _App(broker);
      await app.mount(tester);
      await app.start(tester, _vmId, action);
      broker.requests.single.reply.complete(_result(_vmId, action));
      await _frames(tester);
      expect(find.byType(SnackBar), findsNothing);
      expect(find.byKey(const ValueKey('session-end-ack')), findsNothing);
      expect(broker.inventoryTokens, contains(broker.logins.single.token));
      expect(tester.takeException(), isNull);
    });
  }
}

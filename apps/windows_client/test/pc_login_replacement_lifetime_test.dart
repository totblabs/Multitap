import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_watch.dart';
import 'package:multitap_client/data/session_failure.dart';
import 'package:multitap_client/data/session_renewal.dart';
import 'package:multitap_client/data/session_windows.dart';

import 'auth_flow_test.dart' show TestLoginMemory, fixtureOrigin;
import 'remote_session_test.dart' show SessionSocket;
import 'session_renewal_test.dart' show RenewingBroker, VirtualClock;
import 'helpers/pump_work.dart';

class ReplacedBroker extends RenewingBroker {
  ReplacedBroker(super.clock);
  bool replaced = false;
  @override
  Future<DateTime> renewSession(String token) async {
    if (replaced) throw const BrokerException('pc_login_replaced', 401);
    return super.renewSession(token);
  }
}

class PolicyBroker extends ReplacedBroker implements InteractiveLoginBrokerApi {
  PolicyBroker(super.clock);
  int manual = 0, recoveries = 0;
  bool recoveryConflict = false;
  @override
  Future<BrokerSession> loginInteractively(String name, String password) {
    manual++;
    return super.login(name, password);
  }

  @override
  Future<BrokerSession> login(String name, String password) {
    recoveries++;
    if (recoveryConflict) {
      throw const BrokerException('pc_login_requires_interaction', 409);
    }
    return super.login(name, password);
  }
}

class NativePolicyBroker extends HttpBrokerApi {
  NativePolicyBroker(this.clock) : super(origin: fixtureOrigin);
  final VirtualClock clock;
  int manual = 0;
  bool replaced = false, holdRead = false;
  final oldRead = Completer<List<BrokerDesktop>>();
  @override
  Future<BrokerSession> loginInteractively(
    String name,
    String password,
  ) async => BrokerSession(
    token: 'owner-${++manual}',
    expiresAt: clock.now.add(const Duration(minutes: 10)),
    username: name,
    pcSessionPolicy: windowsPcSessionPolicy,
  );
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    if (token == 'owner-1' && holdRead) return oldRead.future;
    return [];
  }

  @override
  Future<DateTime> renewSession(String token) async {
    if (replaced && token == 'owner-1') {
      throw const BrokerException('pc_login_replaced', 401);
    }
    return clock.now.add(const Duration(minutes: 10));
  }

  @override
  void recordSessionRenewal(
    String token, {
    required int stage,
    required bool ok,
    String? code,
  }) {}
  @override
  Future<void> logout(String token) async {}
}

class QuietWatch extends ChangeNotifier implements DesktopUpdates {
  final changesController = StreamController<void>.broadcast();
  @override
  bool get connected => false;
  @override
  Stream<void> get changes => changesController.stream;
  @override
  void start() {}
  @override
  Future<void> close() async {
    await changesController.close();
    dispose();
  }
}

class AuthWatch extends QuietWatch implements TerminalDesktopAuthFeed {
  final terminal = StreamController<String>.broadcast();
  @override
  Stream<String> get terminalAuthentication => terminal.stream;
  @override
  Future<void> close() async {
    await terminal.close();
    await super.close();
  }
}

Future<void> startApp(
  WidgetTester t,
  BrokerApi api,
  VirtualClock clock,
  TestLoginMemory memory, {
  DesktopWatchFactory? watchFactory,
}) async {
  t.view.physicalSize = const Size(1440, 900);
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  await t.pumpWidget(
    MultitapApp(
      api: api,
      serverOrigin: fixtureOrigin,
      loginMemory: memory,
      clock: clock.call,
      desktopWatchFactory: watchFactory ?? (_, _) => QuietWatch(),
    ),
  );
  await t.pumpAndSettle();
  await t.enterText(find.byKey(const Key('sign-in-id')), 'alice');
  await t.enterText(find.byKey(const Key('sign-in-password')), 'secret-pass');
  await t.tap(find.byKey(const Key('remember-password')));
  await t.tap(find.byKey(const Key('login-submit')));
  await t.pumpAndSettle();
}

void main() {
  testWidgets(
    'fixed live-watch terminal notification retires only its current launcher owner',
    (t) async {
      final clock = VirtualClock();
      final api = PolicyBroker(clock);
      final memory = TestLoginMemory();
      final watches = <AuthWatch>[];
      await startApp(
        t,
        api,
        clock,
        memory,
        watchFactory: (_, _) {
          final watch = AuthWatch();
          watches.add(watch);
          return watch;
        },
      );
      expect(watches, hasLength(1));
      watches.single.terminal.add('pc_login_replaced');
      await t.pumpAndSettle();
      await t.tap(find.byKey(const ValueKey('session-end-ack')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('login-submit')), findsOneWidget);
      expect(api.recoveries, 0);
      expect(memory.entries[fixtureOrigin.origin]?.password, 'secret-pass');
      await t.enterText(
        find.byKey(const Key('sign-in-password')),
        'secret-pass',
      );
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      expect(api.manual, 2);
      expect(watches, hasLength(2));
      expect(find.byKey(const Key('workspace-account')), findsOneWidget);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );
  testWidgets(
    'manual login is interactive, ordinary expiry recovery cannot displace another owner, explicit return stays allowed',
    (t) async {
      final clock = VirtualClock();
      final api = PolicyBroker(clock);
      final memory = TestLoginMemory();
      await startApp(t, api, clock, memory);
      expect(api.manual, 1);
      expect(api.recoveries, 0);
      api.renewRejected = true;
      api.recoveryConflict = true;
      await clock.advance(t, const Duration(minutes: 5, seconds: 1));
      await t.pumpAndSettle();
      expect(api.recoveries, 0);
      await t.tap(find.byKey(const ValueKey('session-end-ack')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('login-submit')), findsOneWidget);
      expect(memory.entries[fixtureOrigin.origin]?.password, 'secret-pass');
      await clock.advance(t, const Duration(seconds: 45));
      expect(
        api.recoveries,
        0,
        reason: 'terminal conflict never retries silently',
      );
      await t.enterText(
        find.byKey(const Key('sign-in-password')),
        'secret-pass',
      );
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      expect(api.manual, 2);
      expect(find.byKey(const Key('workspace-account')), findsOneWidget);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'old child close is completed before another explicit login; late old inventory cannot replace successor',
    (t) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.windows;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final close = Completer<void>();
      var closes = 0;
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        if (call.method == 'retireAuthentication') {
          closes++;
          await close.future;
        }
        return null;
      });
      addTearDown(
        () => messenger.setMockMethodCallHandler(SessionWindows.channel, null),
      );
      final clock = VirtualClock();
      final api = NativePolicyBroker(clock);
      final memory = TestLoginMemory();
      await startApp(t, api, clock, memory);
      api.holdRead = true;
      await t.tap(find.byKey(const Key('refresh-desktops')));
      await t.pump();
      api.replaced = true;
      await clock.advance(t, const Duration(minutes: 5, seconds: 1));
      // The notice moves while the old windows close, so it never settles
      // (a full settle would also run the clock into the close's time limit).
      await pumpWork(t);
      expect(closes, 1);
      expect(
        t
            .widget<FilledButton>(find.byKey(const ValueKey('session-end-ack')))
            .onPressed,
        isNull,
      );
      // 확인 is greyed for a reason the notice says, beside a moving mark.
      const waiting = Key('session-end-busy');
      expect(t.widget<Text>(find.byKey(waiting)).data, '열린 연결을 닫는 중이에요');
      expect(find.byKey(const Key('app-busy-line-spinner')), findsOneWidget);
      expect(api.manual, 1, reason: 'new owner must not race old native close');
      close.complete();
      await t.pumpAndSettle();
      // Closed: the wait's line is gone and 확인 can be pressed.
      expect(find.byKey(waiting), findsNothing);
      expect(find.byKey(const Key('app-busy-line-spinner')), findsNothing);
      expect(
        t
            .widget<FilledButton>(find.byKey(const ValueKey('session-end-ack')))
            .onPressed,
        isNotNull,
      );
      await t.tap(find.byKey(const ValueKey('session-end-ack')));
      await t.pumpAndSettle();
      await t.enterText(
        find.byKey(const Key('sign-in-password')),
        'secret-pass',
      );
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      expect(api.manual, 2);
      api.oldRead.complete([]);
      await t.pumpAndSettle();
      expect(find.byKey(const Key('workspace-account')), findsOneWidget);
      expect(closes, 1, reason: 'late old callback never closes new children');
      await t.pumpWidget(const SizedBox.shrink());
      debugDefaultTargetPlatformOverride = null;
    },
  );
  test(
    'renewal preserves an explicit PC replacement as a terminal lapse',
    () async {
      final reasons = <RenewalLapse>[];
      var calls = 0;
      final r = SessionRenewer(
        renew: () async {
          calls++;
          throw const BrokerException('pc_login_replaced', 401);
        },
        onRenewed: (_) => fail('retired owner cannot renew'),
        onLapse: reasons.add,
      )..start(DateTime.now().add(const Duration(hours: 1)));
      await r.renewNow();
      r.stop();
      expect(calls, 1);
      expect(reasons.map((r) => r.name), ['replaced']);
    },
  );

  test('PC replacement is not a retryable transport handover', () {
    final ended = SessionFailure.ended('pc_login_replaced');
    expect(ended.canRetry, isFalse);
    expect(ended.code, 'pc_login_replaced');
    expect(SessionFailure.blocksAutomaticReconnect(ended.code), isTrue);
    expect(
      SessionFailure.transportCloseReason('pc_login_replaced'),
      'pc_login_replaced',
    );
  });

  test(
    'terminal renewal fences concurrent late success and repeated renewNow',
    () async {
      final late = Completer<DateTime>();
      var calls = 0, renewed = 0, terminal = 0;
      final r = SessionRenewer(
        renew: () {
          if (++calls == 1) return late.future;
          throw const BrokerException('pc_login_replaced', 401);
        },
        onRenewed: (_) => renewed++,
        onLapse: (_) => terminal++,
      )..start(DateTime.now().add(const Duration(hours: 1)));
      final first = r.renewNow();
      await r.renewNow();
      late.complete(DateTime.now().add(const Duration(hours: 2)));
      await first;
      await r.renewNow();
      expect(terminal, 1);
      expect(renewed, 0);
      expect(calls, 2);
      r.stop();
    },
  );

  testWidgets(
    'replaced launcher preserves saved password and never silently reclaims login',
    (t) async {
      final clock = VirtualClock();
      final api = ReplacedBroker(clock);
      final memory = TestLoginMemory();
      t.view.physicalSize = const Size(1440, 900);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      await t.pumpWidget(
        MultitapApp(
          api: api,
          serverOrigin: fixtureOrigin,
          loginMemory: memory,
          clock: clock.call,
        ),
      );
      await t.pumpAndSettle();
      await t.enterText(find.byKey(const Key('sign-in-id')), 'alice');
      await t.enterText(
        find.byKey(const Key('sign-in-password')),
        'secret-pass',
      );
      await t.tap(find.byKey(const Key('remember-password')));
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      expect(api.loggedInAs.length, 1);
      api.replaced = true;
      await clock.advance(t, const Duration(minutes: 5, seconds: 1));
      await t.pumpAndSettle();
      expect(
        api.loggedInAs.length,
        1,
        reason: 'replacement must not perform saved-password login',
      );
      await t.tap(find.byKey(const ValueKey('session-end-ack')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('login-submit')), findsOneWidget);
      expect(memory.entries[fixtureOrigin.origin]?.password, 'secret-pass');
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'replacement close stops desktop-watch redial, ordinary close still retries',
    (t) async {
      for (final reason in ['pc_login_replaced', 'server_stopping']) {
        final socket = SessionSocket();
        var dials = 0;
        final watch = BrokerDesktopWatch(
          origin: fixtureOrigin,
          token: 'fixture-token',
          dialer: (_, _, _) async {
            dials++;
            return socket;
          },
        );
        final notices = <String>[];
        final subscription = watch.terminalAuthentication.listen(notices.add);
        watch.start();
        await t.pump();
        socket.incoming.add('{"type":"desktops.ready"}');
        await t.pump();
        socket.closeCode = reason == 'pc_login_replaced' ? 1008 : 1001;
        socket.closeReason = reason;
        unawaited(socket.incoming.close());
        await t.pump();
        await t.pump(const Duration(seconds: 1));
        expect(dials, reason == 'pc_login_replaced' ? 1 : 2, reason: reason);
        expect(
          notices,
          reason == 'pc_login_replaced' ? ['pc_login_replaced'] : isEmpty,
        );
        // Detach the diagnostic observer before waiting for broadcast close;
        // a fake-async test does not run another event turn while awaiting it.
        unawaited(subscription.cancel());
        final done = watch.close();
        await t.pump();
        await t.pump(const Duration(seconds: 5));
        await done;
      }
    },
  );
}

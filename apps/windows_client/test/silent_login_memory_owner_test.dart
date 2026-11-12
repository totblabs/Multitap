// Actual production MultitapApp widget regression; authoritative RED and GREEN are recorded privately.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/login_memory.dart';

import 'auth_flow_test.dart' show TestLoginMemory, fixtureOrigin;
import 'session_renewal_test.dart' show VirtualClock;
import 'pc_login_replacement_lifetime_test.dart'
    show PolicyBroker, AuthWatch, startApp;

class HeldRecovery extends PolicyBroker {
  HeldRecovery(super.clock);
  final pending = Completer<BrokerSession>();
  @override
  Future<DateTime> renewSession(String token) {
    // Exercise the retained legacy recovery seam inside a valid lifetime.
    // Definitive 401 must instead terminate authentication without recovery.
    if (renewRejected) throw const BrokerException('not_found', 404);
    return super.renewSession(token);
  }
  @override
  Future<BrokerSession> login(String username, String password) {
    recoveries++;
    return pending.future;
  }
}

class ConditionalMemory extends TestLoginMemory
    implements ConditionalLoginMemory {
  int conditionalErases = 0, unconditionalErases = 0;
  bool? ownerWasCurrent;
  RememberedLogin? compared;
  @override
  Future<bool> forgetPasswordIfMatches(
    Uri origin,
    RememberedLogin expected, {
    required bool Function() current,
  }) async {
    conditionalErases++;
    ownerWasCurrent = current();
    compared = expected;
    // A real atomic provider may observe a changed record when it gets its
    // turn in the queue. A refused compare must never trigger fallback erase.
    entries[origin.origin] = const RememberedLogin(
      username: 'bobby',
      password: 'fixture-provider-new',
    );
    return false;
  }

  @override
  Future<void> forgetPassword(Uri origin) async {
    unconditionalErases++;
    await super.forgetPassword(origin);
  }
}

void main() {
  Future<void> acknowledge(WidgetTester t) async {
    final ack=find.byKey(const ValueKey('session-end-ack'));
    if(ack.evaluate().isNotEmpty){await t.tap(ack);await t.pumpAndSettle();}
  }
  testWidgets(
    'Windows app honors atomic provider compare refusal without unconditional fallback',
    (t) async {
      final clock = VirtualClock(), memory = ConditionalMemory();
      final api = HeldRecovery(clock);
      await startApp(t, api, clock, memory);
      final erasedBefore = memory.unconditionalErases;
      api.renewRejected = true;
      await clock.advance(t, const Duration(minutes: 5, seconds: 1));
      await t.pumpAndSettle();
      api.pending.completeError(
        const BrokerException('invalid_credentials', 401),
      );
      await t.pumpAndSettle();
      expect(memory.conditionalErases, 1);
      expect(memory.ownerWasCurrent, isTrue);
      expect(memory.compared?.username, 'alice');
      expect(memory.compared?.password, 'secret-pass');
      expect(memory.unconditionalErases, erasedBefore);
      expect(
        memory.entries[fixtureOrigin.origin]?.password,
        'fixture-provider-new',
      );
      await t.pumpWidget(const SizedBox.shrink());
    },
  );
  for (final code in ['invalid_credentials', 'credentials_change_required']) {
    testWidgets(
      'Current exact rejected credential still forgets normally for $code',
      (t) async {
        final clock = VirtualClock(), memory = TestLoginMemory();
        final api = HeldRecovery(clock);
        await startApp(t, api, clock, memory);
        api.renewRejected = true;
        await clock.advance(t, const Duration(minutes: 5, seconds: 1));
        await t.pumpAndSettle();
        api.pending.completeError(BrokerException(code, 401));
        await t.pumpAndSettle();
        expect(memory.entries[fixtureOrigin.origin]?.username, 'alice');
        expect(memory.entries[fixtureOrigin.origin]?.password, isNull);
        await acknowledge(t);
        expect(find.byKey(const Key('login-submit')), findsOneWidget);
        await t.pumpWidget(const SizedBox.shrink());
      },
    );
  }
  for (final changedUsername in [false, true]) {
    testWidgets(
      'Current owner rejection preserves a newer ${changedUsername ? 'case-distinct username' : 'password'} record',
      (t) async {
        final clock = VirtualClock(), memory = TestLoginMemory();
        final api = HeldRecovery(clock);
        await startApp(t, api, clock, memory);
        api.renewRejected = true;
        await clock.advance(t, const Duration(minutes: 5, seconds: 1));
        await t.pumpAndSettle();
        final latest = RememberedLogin(
          username: changedUsername ? 'ALICE' : 'alice',
          password: changedUsername ? 'secret-pass' : 'fixture-new-password',
        );
        memory.entries[fixtureOrigin.origin] = latest;
        api.pending.completeError(
          const BrokerException('invalid_credentials', 401),
        );
        await t.pumpAndSettle();
        expect(memory.entries[fixtureOrigin.origin]?.username, latest.username);
        expect(memory.entries[fixtureOrigin.origin]?.password, latest.password);
        await acknowledge(t);
        expect(find.byKey(const Key('login-submit')), findsOneWidget);
        await t.pumpWidget(const SizedBox.shrink());
      },
    );
  }
  for (final code in ['invalid_credentials', 'credentials_change_required']) {
    testWidgets('Owner changes during credential reload before $code cleanup', (
      t,
    ) async {
      final clock = VirtualClock(), memory = TestLoginMemory();
      final api = HeldRecovery(clock), watches = <AuthWatch>[];
      await startApp(
        t,
        api,
        clock,
        memory,
        watchFactory: (_, _) {
          final w = AuthWatch();
          watches.add(w);
          return w;
        },
      );
      api.renewRejected = true;
      await clock.advance(t, const Duration(minutes: 5, seconds: 1));
      await t.pumpAndSettle();
      final reload = Completer<RememberedLogin>();
      var reloading = false;
      memory.onLoad = (origin) {
        if (!reloading) {
          reloading = true;
          return reload.future;
        }
        return Future.value(
          memory.entries[origin.origin] ?? const RememberedLogin(),
        );
      };
      api.pending.completeError(BrokerException(code, 401));
      await t.pump();
      expect(reloading, isTrue);
      watches.first.terminal.add('pc_login_replaced');
      await t.pumpAndSettle();
      await acknowledge(t);
      await t.enterText(
        find.byKey(const Key('sign-in-password')),
        'fixture-latest-password',
      );
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      expect(api.manual, 2);
      expect(
        memory.entries[fixtureOrigin.origin]?.password,
        'fixture-latest-password',
      );
      reload.complete(
        const RememberedLogin(username: 'alice', password: 'secret-pass'),
      );
      await t.pumpAndSettle();
      expect(
        memory.entries[fixtureOrigin.origin]?.password,
        'fixture-latest-password',
      );
      expect(find.byKey(const Key('workspace-account')), findsOneWidget);
      await t.pumpWidget(const SizedBox.shrink());
    });
  }
  testWidgets(
    'Disposal before an old authentication rejection preserves remembered credentials',
    (t) async {
      final clock = VirtualClock(), memory = TestLoginMemory();
      final api = HeldRecovery(clock);
      await startApp(t, api, clock, memory);
      api.renewRejected = true;
      await clock.advance(t, const Duration(minutes: 5, seconds: 1));
      await t.pumpAndSettle();
      await t.pumpWidget(const SizedBox.shrink());
      api.pending.completeError(
        const BrokerException('invalid_credentials', 401),
      );
      await t.pumpAndSettle();
      expect(memory.entries[fixtureOrigin.origin]?.password, 'secret-pass');
    },
  );
  for (final username in ['alice', 'bobby']) {
    testWidgets(
      'Old automatic rejection preserves $username successor saved password',
      (t) async {
        final clock = VirtualClock();
        // The app and broker must share exactly the same clock.
        final broker = HeldRecovery(clock), memory = TestLoginMemory();
        final watches = <AuthWatch>[];
        await startApp(
          t,
          broker,
          clock,
          memory,
          watchFactory: (_, _) {
            final watch = AuthWatch();
            watches.add(watch);
            return watch;
          },
        );
        broker.renewRejected = true;
        await clock.advance(t, const Duration(minutes: 5, seconds: 1));
        await t.pumpAndSettle();
        expect(broker.recoveries, 1);
        watches.first.terminal.add('pc_login_replaced');
        await t.pumpAndSettle();
        await acknowledge(t);
        expect(find.byKey(const Key('login-submit')), findsOneWidget);
        await t.enterText(find.byKey(const Key('sign-in-id')), username);
        await t.enterText(
          find.byKey(const Key('sign-in-password')),
          'fixture-new-password',
        );
        await t.tap(find.byKey(const Key('login-submit')));
        await t.pumpAndSettle();
        expect(broker.manual, 2);
        expect(
          memory.entries[fixtureOrigin.origin]?.password,
          'fixture-new-password',
        );
        broker.pending.completeError(
          const BrokerException('invalid_credentials', 401),
        );
        await t.pumpAndSettle();
        expect(find.byKey(const Key('workspace-account')), findsOneWidget);
        expect(memory.entries[fixtureOrigin.origin]?.username, username);
        expect(
          memory.entries[fixtureOrigin.origin]?.password,
          'fixture-new-password',
        );
        await t.pumpWidget(const SizedBox.shrink());
      },
    );
  }
}

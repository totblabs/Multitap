import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/app.dart';

import 'auth_flow_test.dart' show TestBroker, TestLoginMemory, fixtureOrigin;
import 'pc_login_replacement_lifetime_test.dart' show startApp, PolicyBroker;
import 'session_renewal_test.dart' show VirtualClock;

class ConfirmBroker extends TestBroker
    implements InteractiveLoginBrokerApi, LoginConfirmationBrokerApi {
  int attempts = 0, confirmations = 0;
  String liveOwner = 'other-device';
  LoginConfirmation? challenge;
  @override
  Future<BrokerSession> loginInteractively(
    String username,
    String password,
  ) async {
    attempts++;
    challenge = LoginConfirmation.parse(
      {
        'protocol': sessionAccessProtocol,
        'challengeToken': 'synthetic_widget_challenge_0001',
        'expiresInMs': 120000,
      },
      fixtureOrigin,
      DateTime.now(),
    );
    throw LoginConfirmationRequired(challenge!);
  }

  @override
  Future<BrokerSession> confirmLogin(LoginConfirmation value) async {
    value.consume(fixtureOrigin, DateTime.now());
    confirmations++;
    liveOwner = 'this-device';
    return super.login('alice', 'secret-pass');
  }
}

class IdleBroker extends PolicyBroker implements SessionActivityBrokerApi {
  IdleBroker(super.clock);
  int activities = 0;
  @override
  Future<SessionAccess> setIdleEnabled(String token, bool enabled) async =>
      SessionAccess(
        serverTime: clock.now,
        idleEnabled: enabled,
        idleDeadline: enabled
            ? clock.now.add(const Duration(seconds: 30))
            : null,
        idleTimeout: const Duration(seconds: 30),
        warning: const Duration(seconds: 10),
      );
  @override
  Future<BrokerSession> loginInteractively(
    String username,
    String password,
  ) async {
    final base = await super.loginInteractively(username, password);
    return BrokerSession(
      token: base.token,
      username: base.username,
      expiresAt: base.expiresAt,
      sessionAccess: SessionAccess(
        serverTime: clock.now,
        idleTimeout: const Duration(seconds: 30),
        warning: const Duration(seconds: 10),
        idleDeadline: clock.now.add(const Duration(seconds: 30)),
      ),
    );
  }

  @override
  Future<SessionAccess> recordHumanActivity(String token) async {
    activities++;
    return SessionAccess(
      serverTime: clock.now,
      idleDeadline: clock.now.add(const Duration(seconds: 30)),
      idleTimeout: const Duration(seconds: 30),
      warning: const Duration(seconds: 10),
    );
  }
}

void main() {
  testWidgets(
    'actual manual confirmation cancel preserves other owner; explicit yes replaces it',
    (t) async {
      final broker = ConfirmBroker(),
          clock = VirtualClock(),
          memory = TestLoginMemory();
      t.view.physicalSize = const Size(1440, 900);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      await t.pumpWidget(
        MultitapApp(
          api: broker,
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
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 350));
      expect(find.text('다른 곳에서 로그아웃하고 여기서 로그인할까요?'), findsOneWidget);
      expect(broker.liveOwner, 'other-device');
      expect(broker.confirmations, 0);
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(broker.challenge!.forgotten, isTrue);
      expect(broker.liveOwner, 'other-device');
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 350));
      await t.tap(find.text('로그아웃하고 로그인'));
      await t.pumpAndSettle();
      expect(broker.confirmations, 1);
      expect(broker.liveOwner, 'this-device');
      expect(find.byKey(const Key('workspace-account')), findsOneWidget);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'active remote and console routes are hidden/removed on idle before reason ACK and manual login',
    (t) async {
      final clock = VirtualClock(),
          broker = IdleBroker(clock),
          memory = TestLoginMemory();
      await startApp(t, broker, clock, memory);
      final navigator = Navigator.of(
        t.element(find.byKey(const Key('workspace-account'))),
      );
      unawaited(
        navigator.push(
          MaterialPageRoute<void>(
            builder: (_) => const Scaffold(body: Text('remote-video')),
          ),
        ),
      );
      await t.pumpAndSettle();
      unawaited(
        navigator.push(
          MaterialPageRoute<void>(
            builder: (_) => const Scaffold(body: Text('console-video')),
          ),
        ),
      );
      await t.pumpAndSettle();
      expect(find.text('console-video'), findsOneWidget);
      final activityCount = broker.activities;
      await clock.advance(t, const Duration(seconds: 21));
      // The notice now counts the seconds left (review 2026-11-12).
      expect(find.byKey(const Key('idle-warning')), findsOneWidget);
      expect(find.textContaining('초 뒤 로그아웃돼요. 연결도 모두 끊겨요.'), findsOneWidget);
      await clock.advance(t, const Duration(seconds: 10));
      await t.pumpAndSettle();
      expect(find.text('remote-video'), findsNothing);
      expect(find.text('console-video'), findsNothing);
      expect(
        find.byKey(const Key('login-submit')),
        findsNothing,
        reason: 'reason precedes login',
      );
      expect(find.text('오랫동안 입력이 없어 로그아웃됐어요.'), findsOneWidget);
      expect(
        broker.activities,
        activityCount,
        reason: 'route/media/timer activity never counts',
      );
      expect(broker.recoveries, 0);
      expect(memory.entries[fixtureOrigin.origin]?.password, 'secret-pass');
      await t.tap(find.byKey(const ValueKey('session-end-ack')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('login-submit')), findsOneWidget);
      expect(
        t
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        'secret-pass',
        reason: '비밀번호 기억 is on: the saved password is filled, not sent',
      );
      expect(broker.manual, 1);
      expect(memory.entries[fixtureOrigin.origin]?.password, 'secret-pass');
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'actual warning continue action extends only through human activity response',
    (t) async {
      final clock = VirtualClock(),
          broker = IdleBroker(clock),
          memory = TestLoginMemory();
      await startApp(t, broker, clock, memory);
      await clock.advance(t, const Duration(seconds: 21));
      await t.pump();
      expect(find.text('계속 사용'), findsOneWidget);
      // Review 2026-11-12: it was a full-width strip over the header without
      // the time left. A compact notice under the header that counts down.
      final notice = t.getRect(find.byKey(const Key('idle-warning')));
      final window = t.getRect(find.byType(MaterialApp));
      expect(notice.top, greaterThanOrEqualTo(64), reason: 'under the header');
      expect(notice.width, lessThan(window.width - 200), reason: 'compact');
      expect((notice.center.dx - window.center.dx).abs(), lessThan(1));
      String line() => t
          .widget<Text>(find.byKey(const Key('idle-warning-text')))
          .data!;
      final shown = RegExp(r'^입력이 없으면 (\d+)초 뒤 로그아웃돼요\. 연결도 모두 끊겨요\.$');
      final first = int.parse(shown.firstMatch(line())!.group(1)!);
      expect(first, inInclusiveRange(1, 10));
      await clock.advance(t, const Duration(seconds: 3));
      await t.pump();
      final later = int.parse(shown.firstMatch(line())!.group(1)!);
      expect(later, lessThan(first), reason: 'it counts down');
      expect(later, greaterThanOrEqualTo(1));
      final before = broker.activities;
      await t.tap(find.text('계속 사용'));
      await t.pumpAndSettle();
      expect(broker.activities, before + 1);
      await clock.advance(t, const Duration(seconds: 15));
      await t.pumpAndSettle();
      expect(find.text('오랫동안 입력이 없어 로그아웃됐어요.'), findsNothing);
      expect(find.byKey(const Key('workspace-account')), findsOneWidget);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );
}

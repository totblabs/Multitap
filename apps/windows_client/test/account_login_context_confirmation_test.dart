import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_access_boundary.dart';

import 'account_login_context_parse_test.dart' show loginChallengeFixture;
import 'auth_flow_test.dart' show TestBroker, fixtureOrigin;

class ContextBroker extends TestBroker implements LoginConfirmationBrokerApi {
  ContextBroker({this.withContext = true, this.held = false});
  final bool withContext, held;
  final events = <String>[];
  final retired = Completer<void>();
  int confirmations = 0;
  LoginConfirmation? challenge;
  Future<BrokerSession> attempt() async {
    events.add('attempt');
    challenge = LoginConfirmation.parse(
      loginChallengeFixture(includeContext: withContext),
      fixtureOrigin,
      DateTime.now(),
    );
    throw LoginConfirmationRequired(challenge!);
  }

  @override
  Future<BrokerSession> confirmLogin(LoginConfirmation challenge) async {
    challenge.consume(fixtureOrigin, DateTime.now());
    confirmations++;
    events.add('confirm');
    if (held) await retired.future;
    events.add('old-ended');
    return super.login('alice', 'synthetic-only');
  }
}

class ContextFixture {
  ContextFixture(this.broker);
  final ContextBroker broker;
  bool current = true;
  Object? error;
  BrokerSession? result;
  Future<void> mount(
    WidgetTester tester, {
    Size size = const Size(1280, 800),
    double scale = 1,
  }) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = size;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        builder: (_, child) => MediaQuery(
          data: MediaQueryData(
            size: size,
            textScaler: TextScaler.linear(scale),
          ),
          child: child!,
        ),
        home: Scaffold(
          body: Builder(
            builder: (context) => TextButton(
              key: const Key('manual-login'),
              onPressed: () async {
                try {
                  result = await confirmManualLogin(
                    context,
                    api: broker,
                    current: () => current,
                    attempt: broker.attempt,
                  );
                  if (result != null) broker.events.add('new-login');
                } catch (caught) {
                  error = caught;
                }
              },
              child: const Text('로그인'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.byKey(const Key('manual-login')));
    await tester.pumpAndSettle();
  }
}

void main() {
  testWidgets(
    'verified account and actual desktop shown before consent; cancel sends no confirmation',
    (tester) async {
      final broker = ContextBroker();
      final fixture = ContextFixture(broker);
      await fixture.mount(tester);
      expect(fixture.error, isNull);
      expect(find.textContaining('alice'), findsOneWidget);
      expect(find.textContaining('TESTER'), findsOneWidget);
      expect(find.textContaining('기기 정보 없음'), findsNothing);
      expect(
        find.textContaining('windows-pc-v1'),
        findsNothing,
        reason: 'Protocol is not a physical device identity',
      );
      expect(broker.confirmations, 0);
      expect(broker.events, ['attempt']);
      await tester.tap(find.text('취소'));
      await tester.pumpAndSettle();
      expect(fixture.result, isNull);
      expect(broker.confirmations, 0);
      expect(broker.challenge!.forgotten, isTrue);
    },
  );

  testWidgets(
    'explicit yes waits for confirmation response before accepting the new owner',
    (tester) async {
      final broker = ContextBroker(held: true);
      final fixture = ContextFixture(broker);
      await fixture.mount(tester);
      expect(fixture.error, isNull);
      await tester.tap(find.text('로그아웃하고 로그인'));
      await tester.pumpAndSettle();
      expect(broker.confirmations, 1);
      expect(broker.events, ['attempt', 'confirm']);
      expect(fixture.result, isNull);
      broker.retired.complete();
      await tester.pumpAndSettle();
      expect(broker.events, ['attempt', 'confirm', 'old-ended', 'new-login']);
      expect(fixture.result, isNotNull);
    },
  );

  for (final action in ['escape', 'stale', 'disposed']) {
    testWidgets('$action cannot spend a context confirmation challenge', (
      tester,
    ) async {
      final broker = ContextBroker();
      final fixture = ContextFixture(broker);
      await fixture.mount(tester);
      expect(fixture.error, isNull);
      if (action == 'stale') {
        fixture.current = false;
        await tester.tap(find.text('로그아웃하고 로그인'));
      } else if (action == 'disposed') {
        await tester.pumpWidget(const SizedBox.shrink());
      } else {
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      }
      await tester.pumpAndSettle();
      expect(broker.confirmations, 0);
      expect(fixture.result, isNull);
      expect(broker.challenge!.forgotten, isTrue);
    });
  }

  testWidgets(
    'old three-field broker still offers explicit generic consent without inventing identity',
    (tester) async {
      final broker = ContextBroker(withContext: false);
      final fixture = ContextFixture(broker);
      await fixture.mount(tester);
      expect(fixture.error, isNull);
      expect(find.text('다른 곳에서 로그아웃하고 여기서 로그인할까요?'), findsOneWidget);
      expect(find.textContaining('alice'), findsNothing);
      expect(broker.confirmations, 0);
      await tester.tap(find.text('취소'));
      await tester.pumpAndSettle();
      expect(broker.challenge!.forgotten, isTrue);
    },
  );

  testWidgets(
    'context and confirmation remain reachable at 320 and uncapped 200 percent',
    (tester) async {
      final broker = ContextBroker();
      final fixture = ContextFixture(broker);
      await fixture.mount(tester, size: const Size(320, 640), scale: 2);
      expect(fixture.error, isNull);
      expect(tester.takeException(), isNull);
      final cancel = find.text('취소');
      await tester.ensureVisible(cancel);
      await tester.pumpAndSettle();
      expect(cancel.hitTestable(), findsOneWidget);
      await tester.tap(cancel);
      await tester.pumpAndSettle();
      expect(broker.confirmations, 0);
    },
  );
}

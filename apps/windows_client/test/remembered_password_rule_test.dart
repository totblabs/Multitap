import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/login_memory.dart';

import 'auth_flow_test.dart' show TestLoginMemory, fixtureOrigin;
import 'pc_login_replacement_lifetime_test.dart' show AuthWatch;
import 'session_access_lifetime_test.dart' show IdleBroker;
import 'session_renewal_test.dart' show VirtualClock;

// The one rule of the Windows sign-in screen (2026-11-11): with "비밀번호 기억"
// on, the saved password is filled in every time the screen appears; with it
// off, nothing is saved and nothing is filled. There is no second option.

String _field(WidgetTester t, String key) =>
    t.widget<TextField>(find.byKey(Key(key))).controller!.text;

Future<void> _open(
  WidgetTester t,
  IdleBroker api,
  VirtualClock clock,
  TestLoginMemory memory,
) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = const Size(1440, 1000);
  addTearDown(t.view.reset);
  await t.pumpWidget(
    MultitapApp(
      api: api,
      serverOrigin: fixtureOrigin,
      loginMemory: memory,
      clock: clock.call,
      desktopWatchFactory: (_, _) => AuthWatch(),
    ),
  );
  await t.pumpAndSettle();
}

Future<void> _signIn(WidgetTester t, {String? typed}) async {
  if (typed != null) {
    await t.enterText(find.byKey(const Key('sign-in-password')), typed);
  }
  await t.tap(find.byKey(const Key('login-submit')));
  await t.pumpAndSettle();
  expect(find.byKey(const Key('workspace-account')), findsOneWidget);
}

Future<void> _signOut(WidgetTester t) async {
  await t.tap(find.byKey(const Key('workspace-account')));
  await t.pumpAndSettle();
  await t.tap(find.byKey(const Key('sign-out')));
  await t.pumpAndSettle();
  await t.tap(find.byKey(const Key('confirm-sign-out')));
  await t.pumpAndSettle();
}

Future<void> _acknowledge(WidgetTester t) async {
  expect(find.byKey(const ValueKey('session-end-ack')), findsOneWidget);
  await t.tap(find.byKey(const ValueKey('session-end-ack')));
  await t.pumpAndSettle();
}

/// The real 30 s idle countdown; the deadline is crossed on its own frame.
Future<void> _idleOut(WidgetTester t, VirtualClock clock) async {
  await clock.advance(t, const Duration(seconds: 29));
  await clock.advance(t, const Duration(seconds: 1));
  await t.pumpAndSettle();
  await _acknowledge(t);
}

Future<void> _expire(WidgetTester t, IdleBroker broker) async {
  broker.expired = true;
  await t.tap(find.byKey(const Key('refresh-desktops')));
  await t.pumpAndSettle();
  broker.expired = false;
  await _acknowledge(t);
}

void main() {
  for (final remember in [true, false]) {
    testWidgets(
      'Windows 비밀번호 기억 ${remember ? 'on fills the saved password' : 'off leaves the password empty'} '
      'after sign-out, automatic sign-out, an expired session and restart',
      (t) async {
        final memory = TestLoginMemory()
          ..entries[fixtureOrigin.origin] = RememberedLogin(
            username: 'Alice',
            password: remember ? 'secret-pass' : null,
          );
        final expected = remember ? 'secret-pass' : '';
        final typed = remember ? null : 'typed-pass';
        final clock = VirtualClock(), broker = IdleBroker(clock);
        await _open(t, broker, clock, memory);
        expect(
          find.byKey(const Key('retype-password-after-logout')),
          findsNothing,
        );
        expect(find.text('로그아웃 후 비밀번호 다시 입력'), findsNothing);
        expect(
          t.widget<Switch>(find.byKey(const Key('remember-password'))).value,
          remember,
        );
        expect(_field(t, 'sign-in-password'), expected);

        await _signIn(t, typed: typed);
        await _signOut(t);
        expect(_field(t, 'sign-in-id'), 'Alice');
        expect(_field(t, 'sign-in-password'), expected, reason: 'sign-out');

        await _signIn(t, typed: typed);
        await _idleOut(t, clock);
        expect(_field(t, 'sign-in-password'), expected, reason: 'idle');

        await _signIn(t, typed: typed);
        await _expire(t, broker);
        expect(_field(t, 'sign-in-password'), expected, reason: 'expired');

        expect(broker.manual, 3, reason: 'a filled password never signs in');
        expect(broker.recoveries, 0);
        expect(
          memory.entries[fixtureOrigin.origin]!.password,
          remember ? 'secret-pass' : isNull,
        );
        if (!remember) expect(memory.passwordSaves, 0);

        await t.pumpWidget(const SizedBox.shrink());
        await t.pumpAndSettle();
        final restarted = IdleBroker(clock);
        await _open(t, restarted, clock, memory);
        expect(_field(t, 'sign-in-id'), 'Alice');
        expect(_field(t, 'sign-in-password'), expected, reason: 'restart');
        expect(restarted.manual, 0);
        expect(restarted.recoveries, 0);
        await t.pumpWidget(const SizedBox.shrink());
      },
    );
  }
}

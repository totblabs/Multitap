// 첫 로그인 비밀번호 (user decision 2026-11-16): an account whose password an
// administrator set picks its own at the first sign-in, then signs in with it.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/login_memory.dart';
import 'package:multitap_client/l10n/l10n.dart';
import 'package:multitap_client/ui/initial_password_dialog.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';

import 'auth_flow_test.dart' show TestLoginMemory;

final origin = Uri.parse('https://cloud.example.com');

void main() {
  // The sign-in screen shows a running progress line while it waits, so a
  // fixed number of frames stands in for pumpAndSettle.
  Future<void> settle(WidgetTester tester) async {
    for (var i = 0; i < 12; i++) {
      await tester.pump(const Duration(milliseconds: 100));
    }
  }

  Future<void> tapRemember(WidgetTester tester) async {
    await tester.tap(find.byKey(const Key('remember-password')));
    await settle(tester);
  }

  Future<void> signIn(WidgetTester tester, String id, String password) async {
    await tester.enterText(find.byKey(const Key('sign-in-id')), id);
    await tester.enterText(find.byKey(const Key('sign-in-password')), password);
    await tester.testTextInput.receiveAction(TextInputAction.done);
    await settle(tester);
  }

  Future<void> choose(WidgetTester tester, String next, [String? confirm]) async {
    await tester.enterText(
      find.byKey(const Key('initial-new-password')),
      next,
    );
    await tester.enterText(
      find.byKey(const Key('initial-confirm-password')),
      confirm ?? next,
    );
    await tester.tap(find.byKey(const Key('save-initial-password')));
    await settle(tester);
  }

  testWidgets(
    'the first sign-in asks for a new password, signs in with it, and remembers the new one',
    (tester) async {
      final memory = TestLoginMemory();
      final logins = <(String, String)>[];
      final changes = <(String, String, String, String)>[];
      var current = '01012345678';
      await tester.pumpWidget(
        MaterialApp(
          home: SignInScreen(
            serverOrigin: origin,
            loginMemory: memory,
            onServerChanged: (_) async {},
            onLogin: (id, password) async {
              logins.add((id, password));
              if (password != current) return SignInResult.rejected;
              return password == '01012345678'
                  ? SignInResult.passwordChangeRequired
                  : SignInResult.success;
            },
            onInitialPassword: (id, password, next, confirmation) async {
              changes.add((id, password, next, confirmation));
              if (next == password) {
                throw const BrokerException('password_unchanged', 400);
              }
              current = next;
            },
          ),
        ),
      );
      await settle(tester);
      await tapRemember(tester);
      await signIn(tester, '홍길동', '01012345678');
      expect(find.text(l10n.setNewPasswordTitle), findsOneWidget);
      // Checked before anything is sent.
      await choose(tester, 'short');
      expect(find.text(l10n.passwordsMust10256Characters), findsOneWidget);
      await choose(tester, 'Picked-by-owner-71!', 'Something-else-71!');
      expect(find.text(l10n.newPasswordConfirmationDontMatch), findsOneWidget);
      expect(changes, isEmpty);
      // The server refuses the first password again.
      await choose(tester, '01012345678');
      expect(find.text(l10n.chooseDifferentFromFirstPassword), findsOneWidget);
      expect(find.text(l10n.setNewPasswordTitle), findsOneWidget);
      await choose(tester, 'Picked-by-owner-71!');
      expect(find.text(l10n.setNewPasswordTitle), findsNothing);
      expect(changes.last, (
        '홍길동',
        '01012345678',
        'Picked-by-owner-71!',
        'Picked-by-owner-71!',
      ));
      expect(logins, [
        ('홍길동', '01012345678'),
        ('홍길동', 'Picked-by-owner-71!'),
      ]);
      expect(
        memory.entries[origin.origin],
        isA<RememberedLogin>()
            .having((m) => m.username, 'username', '홍길동')
            .having((m) => m.password, 'password', 'Picked-by-owner-71!'),
      );
    },
  );

  testWidgets('cancelling keeps the account signed out and saves nothing', (
    tester,
  ) async {
    final memory = TestLoginMemory();
    var logins = 0, changes = 0;
    await tester.pumpWidget(
      MaterialApp(
        home: SignInScreen(
          serverOrigin: origin,
          loginMemory: memory,
          onServerChanged: (_) async {},
          onLogin: (_, _) async {
            logins++;
            return SignInResult.passwordChangeRequired;
          },
          onInitialPassword: (_, _, _, _) async => changes++,
        ),
      ),
    );
    await settle(tester);
    await tapRemember(tester);
    await signIn(tester, '홍길동', '01012345678');
    await tester.tap(find.byKey(const Key('cancel-initial-password')));
    await settle(tester);
    expect(find.text(l10n.setNewPasswordTitle), findsNothing);
    expect((logins, changes), (1, 0));
    expect(memory.entries[origin.origin]?.password, isNull);
  });

  testWidgets('the dialog returns the new password only after the server took it', (
    tester,
  ) async {
    String? result = 'unset';
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => TextButton(
            onPressed: () async => result = await showInitialPasswordDialog(
              context,
              onSubmit: (_, _) async =>
                  throw const BrokerException('credentials_changed', 409),
            ),
            child: const Text('open'),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await settle(tester);
    await choose(tester, 'Picked-by-owner-71!');
    expect(find.text(l10n.passwordAlreadyChangedSignIn), findsOneWidget);
    expect(result, 'unset');
    await tester.tap(find.byKey(const Key('cancel-initial-password')));
    await settle(tester);
    expect(result, isNull);
  });
}

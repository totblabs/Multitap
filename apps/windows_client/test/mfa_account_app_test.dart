// Actual App/Android menus, auth retirement and ROOT code overlay. All broker,
// persistence, catalog and native boundaries are synthetic; no live enrollment.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_watch.dart';
import 'package:multitap_client/data/login_memory.dart';
import 'package:multitap_client/data/product_updates.dart' show UpdateResponse;
import 'package:multitap_client/ui/mfa_account_dialog.dart';
import 'package:multitap_client/ui/session_access_boundary.dart';

import 'auth_flow_test.dart' show TestBroker, TestLoginMemory, fixtureOrigin;
import 'helpers/mfa_account_fake.dart';
import 'product_updates_test.dart' show Transport;
import 'android/android_updates_test.dart' show FakePlatform;

class _Watch extends ChangeNotifier implements DesktopUpdates {
  @override
  bool get connected => true;
  @override
  Stream<void> get changes => const Stream<void>.empty();
  @override
  void start() {}
  @override
  Future<void> close() async {}
}
class _Broker extends TestBroker implements MfaAccountBrokerApi {
  final management = MfaTestApi();
  @override
  Uri get origin => fixtureOrigin;
  @override
  Future<BrokerSession> login(String username, String password) async {
    logins++;
    return BrokerSession(token: 'synthetic-only-$logins', username: username,
      accountId: '11111111-1111-4111-8111-111111111111', expiresAt: DateTime.now().add(const Duration(hours: 1)));
  }
  @override
  Future<MfaAccountStatus> readMfaAccount(String token) => management.readMfaAccount(token);
  @override
  Future<MfaEnrollment> setupMfaAccount(String token, String currentPassword) => management.setupMfaAccount(token, currentPassword);
  @override
  Future<MfaAccountChange> enableMfaAccount(String token, MfaEnrollment enrollment, String code) => management.enableMfaAccount(token, enrollment, code);
  @override
  Future<void> reauthenticateMfaAccount(String token, {required String currentPassword, String? code, String? recoveryCode}) => management.reauthenticateMfaAccount(token, currentPassword: currentPassword, code: code, recoveryCode: recoveryCode);
  @override
  Future<MfaAccountChange> disableMfaAccount(String token, String currentPassword) => management.disableMfaAccount(token, currentPassword);
  @override
  Future<MfaAccountChange> regenerateMfaRecovery(String token, String currentPassword) => management.regenerateMfaRecovery(token, currentPassword);
}
Future<void> _wait(WidgetTester tester, bool Function() done) async {
  for (var turn = 0; turn < 60 && !done(); turn++) { await tester.pump(const Duration(milliseconds: 20)); }
  expect(done(), isTrue);
}
class _Fixture {
  final broker = _Broker(), memory = TestLoginMemory();
  final persistence = <String>[];
  Future<void> mount(WidgetTester tester, bool android) async {
    tester.view.physicalSize = android ? const Size(1080, 2400) : const Size(1366, 900);
    tester.view.devicePixelRatio = android ? 3 : 1; addTearDown(tester.view.reset);
    if (android) {
      final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(AndroidStore.channel, (call) async {
        persistence.add(call.method);
        return switch (call.method) {
          'origin' => fixtureOrigin.toString(), 'login' => {'username': 'alice', 'password': 'synthetic-password'},
          'preferences' => <String, String>{}, 'appVersion' => 'synthetic', _ => null,
        };
      });
      addTearDown(() => messenger.setMockMethodCallHandler(AndroidStore.channel, null));
      await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker, desktopWatchFactory: (_, _) => _Watch(),
        updatesFactory: (origin) => AndroidUpdates(origin, transport: Transport((_) {
          final bytes = utf8.encode('{"releases":[]}'); return UpdateResponse(200, bytes.length, Stream.value(bytes));
        })), updatePlatform: FakePlatform()));
    } else {
      memory.entries[fixtureOrigin.origin] = const RememberedLogin(username: 'alice', password: 'synthetic-password');
      await tester.pumpWidget(MultitapApp(api: broker, serverOrigin: fixtureOrigin, loginMemory: memory,
        desktopWatchFactory: (_, _) => _Watch()));
    }
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('login-submit'))); await tester.pumpAndSettle();
    expect(broker.logins, 1);
    // 2단계 인증 is in 설정 (2026-11-11): the header's own ⚙ on Windows and Android.
    await tester.tap(find.byKey(const Key('open-app-settings'))); await tester.pumpAndSettle();
    expect(find.byKey(const Key('account-mfa')), findsOneWidget);
    await tester.tap(find.byKey(const Key('account-mfa'))); await tester.pumpAndSettle();
  }
  Future<void> setup(WidgetTester tester) async {
    await tester.tap(find.byKey(const Key('mfa-account-enable'))); await tester.pump();
    await tester.enterText(find.byKey(const Key('mfa-account-password')), 'synthetic-password');
    await tester.tap(find.byKey(const Key('mfa-account-submit'))); await tester.pumpAndSettle();
  }
  Future<void> remove(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox.shrink()); await tester.pumpAndSettle();
  }
}
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final android in [false, true]) {
    testWidgets('${android ? 'Android' : 'Windows'} actual menu completes factor change into unauthenticated root receipt', (tester) async {
      final f = _Fixture(); await f.mount(tester, android); await f.setup(tester);
      expect(find.byType(MfaAccountDialog), findsOneWidget);
      // Switching to the authenticator does not retire the setup owner/key.
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive); await tester.pump();
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed); await tester.pump();
      expect(find.byKey(const Key('mfa-account-factor')), findsOneWidget);
      final writes = f.memory.passwordSaves;
      final held = Completer<MfaAccountChange>(); f.broker.management.change = (_) => held.future;
      await tester.enterText(find.byKey(const Key('mfa-account-factor')), '123456');
      await tester.tap(find.byKey(const Key('mfa-account-submit'))); await tester.pump();
      expect(tester.widget<TextButton>(find.byKey(const Key('mfa-account-close'))).onPressed, isNull);
      held.complete(mfaChange());
      await _wait(tester, () => find.byKey(const Key('mfa-completed-codes')).evaluate().isNotEmpty);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('mfa-completed-codes')), findsOneWidget);
      final boundary = tester.widget<SessionAccessBoundary>(find.byType(SessionAccessBoundary));
      expect(boundary.controller.reason, 'session_reauthentication_required');
      expect(f.broker.logins, 1, reason: 'no fake successor session or password login replay');
      expect(f.broker.management.calls, ['status', 'setup', 'enable']);
      expect(f.memory.passwordSaves, writes);
      expect(f.persistence.where((method) => method.toLowerCase().contains('mfa') || method.toLowerCase().contains('recovery')), isEmpty);
      await f.remove(tester);
    });
    for (final stage in ['read', 'setup']) {
      testWidgets('${android ? 'Android' : 'Windows'} own management ${stage}401 retires actual auth boundary', (tester) async {
        final f = _Fixture();
        if (stage == 'read') { f.broker.management.read = () => Future.error(const BrokerException('session_expired', 401)); }
        await f.mount(tester, android);
        if (stage == 'setup') {
          f.broker.management.setup = () => Future.error(const BrokerException('session_expired', 401)); await f.setup(tester);
        }
        await _wait(tester, () => tester.widget<SessionAccessBoundary>(find.byType(SessionAccessBoundary)).controller.reason != null);
        expect(tester.widget<SessionAccessBoundary>(find.byType(SessionAccessBoundary)).controller.reason, 'session_expired');
        expect(find.byKey(const Key('mfa-completed-codes')), findsNothing);
        expect(f.broker.logins, 1); expect(f.broker.management.calls, stage == 'read' ? ['status'] : ['status', 'setup']);
        await f.remove(tester);
      });
    }
  }
}

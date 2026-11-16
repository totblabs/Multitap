// 첫 로그인 비밀번호 (user decision 2026-11-16) on Android: an account whose
// password an administrator set picks its own, then signs in with it, and
// 비밀번호 기억 keeps the new one.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/l10n/l10n.dart';

import '../helpers/pump_work.dart';
import 'android_launcher_test.dart' show FakeBroker, MemoryStore;

class FirstPasswordBroker extends FakeBroker
    implements InitialPasswordBrokerApi {
  FirstPasswordBroker() : super(const []);
  String password = '01012345678';
  bool changeRequired = true;
  final changes = <List<String>>[];

  @override
  Future<BrokerSession> login(String username, String password) async {
    calls.add('login:$username:$password');
    if (password != this.password) {
      throw const BrokerException('invalid_credentials', 401);
    }
    if (changeRequired) {
      throw const BrokerException('password_change_required', 428);
    }
    return BrokerSession(
      token: 'not-persisted',
      expiresAt: DateTime.now().add(const Duration(hours: 1)),
      username: username,
    );
  }

  @override
  Future<void> setInitialPassword(
    String username,
    String password,
    String newPassword,
    String confirmation,
  ) async {
    changes.add([username, password, newPassword, confirmation]);
    if (password != this.password) {
      throw const BrokerException('invalid_credentials', 401);
    }
    if (newPassword == password) {
      throw const BrokerException('password_unchanged', 400);
    }
    this.password = newPassword;
    changeRequired = false;
  }
}

const _optionalPenChannel = MethodChannel('multitap/pen-input');

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUp(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_optionalPenChannel, (_) async => null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          AndroidSessionKeepalive.channel,
          (call) async => call.method == 'start' ? true : null,
        );
  });
  tearDown(() {
    for (final channel in [
      _optionalPenChannel,
      AndroidStore.channel,
      AndroidSessionKeepalive.channel,
    ]) {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null);
    }
  });

  Future<void> tap(WidgetTester tester, Finder target) async {
    await tester.ensureVisible(target);
    await tester.pump(const Duration(milliseconds: 300));
    await tester.tap(target);
    await pumpWork(tester);
  }

  Future<(FirstPasswordBroker, MemoryStore)> start(WidgetTester tester) async {
    tester.view.physicalSize = const Size(1080, 2400);
    tester.view.devicePixelRatio = 3;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final store = MemoryStore(origin: 'https://vdi.example', username: '홍길동');
    store.prefs['launcher.view'] = 'cards';
    store.install();
    final broker = FirstPasswordBroker();
    await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
    await pumpWork(tester);
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      '01012345678',
    );
    return (broker, store);
  }

  Future<void> choose(WidgetTester tester, String next) async {
    await tester.enterText(find.byKey(const Key('initial-new-password')), next);
    await tester.enterText(
      find.byKey(const Key('initial-confirm-password')),
      next,
    );
    await tap(tester, find.byKey(const Key('save-initial-password')));
  }

  testWidgets(
    'the first sign-in asks for a new password and signs in with it',
    (tester) async {
      final (broker, store) = await start(tester);
      await tap(tester, find.byKey(const Key('login-submit')));
      expect(find.text(l10n.setNewPasswordTitle), findsOneWidget);
      await choose(tester, '01012345678');
      expect(find.text(l10n.chooseDifferentFromFirstPassword), findsOneWidget);
      await choose(tester, 'Picked-by-owner-71!');
      expect(find.text(l10n.setNewPasswordTitle), findsNothing);
      expect(broker.calls.where((c) => c.startsWith('login:')), [
        'login:홍길동:01012345678',
        'login:홍길동:Picked-by-owner-71!',
      ]);
      expect(broker.changes.last, [
        '홍길동',
        '01012345678',
        'Picked-by-owner-71!',
        'Picked-by-owner-71!',
      ]);
      final saved = store.calls.lastWhere((c) => c.method == 'saveLogin');
      expect((saved.arguments as Map)['username'], '홍길동');
      expect(
        find.byKey(const Key('sign-in-password')),
        findsNothing,
        reason: 'signed in: the desktop list replaced the sign-in form',
      );
    },
  );

  testWidgets('cancelling stays signed out and says why', (tester) async {
    final (broker, store) = await start(tester);
    await tap(tester, find.byKey(const Key('login-submit')));
    await tap(tester, find.byKey(const Key('cancel-initial-password')));
    expect(find.text(l10n.setNewPasswordTitle), findsNothing);
    expect(find.text(l10n.setNewPasswordToSignIn), findsOneWidget);
    expect(broker.changes, isEmpty);
    expect(broker.calls.where((c) => c.startsWith('login:')).length, 1);
    expect(store.calls.where((c) => c.method == 'saveLogin'), isEmpty);
  });
}

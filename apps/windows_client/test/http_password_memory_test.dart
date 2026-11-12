// 비밀번호 기억 only for HTTPS servers: on an
// HTTP server the switch is off and disabled with one plain line, a stored
// password is neither filled in nor kept, and the next sign-in forgets it.
// Windows sign-in screen, Android sign-in card and Android store request.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_launcher.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/login_memory.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';

import 'auth_flow_test.dart' show TestLoginMemory;

const _line = 'HTTPS 서버에서만 비밀번호를 기억해요.';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final http = Uri.parse('http://10.0.0.5:18480');
  final https = Uri.parse('https://vdi.example.test');

  test('only an https origin remembers a password', () {
    expect(passwordMemoryAllowed(https), isTrue);
    expect(passwordMemoryAllowed(http), isFalse);
    expect(passwordMemoryAllowed(null), isFalse);
  });

  Future<TestLoginMemory> windows(WidgetTester tester, Uri origin) async {
    final memory = TestLoginMemory()
      ..entries[origin.origin] = const RememberedLogin(
        username: 'alice',
        password: 'stored-before',
      );
    await tester.pumpWidget(
      MaterialApp(
        home: SignInScreen(
          serverOrigin: origin,
          loginMemory: memory,
          onServerChanged: (_) async {},
          onLogin: (_, _) async => SignInResult.success,
        ),
      ),
    );
    await tester.pumpAndSettle();
    return memory;
  }

  String field(WidgetTester tester, String key) =>
      tester.widget<TextField>(find.byKey(Key(key))).controller!.text;

  testWidgets('Windows, HTTP server: the switch is off and disabled, nothing '
      'is filled, and the sign-in forgets the stored password', (tester) async {
    final memory = await windows(tester, http);
    final toggle = tester.widget<Switch>(find.byKey(const Key('remember-password')));
    expect(toggle.value, isFalse);
    expect(toggle.onChanged, isNull);
    expect(find.text(_line), findsOneWidget);
    expect(field(tester, 'sign-in-id'), 'alice');
    expect(field(tester, 'sign-in-password'), isEmpty);

    await tester.enterText(find.byKey(const Key('sign-in-password')), 'typed-now');
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
    expect(memory.passwordSaveAttempts, 0);
    expect(memory.entries[http.origin]!.password, isNull);
    expect(memory.entries[http.origin]!.username, 'alice');
  });

  testWidgets('Windows, HTTPS server: the switch works as before', (tester) async {
    await windows(tester, https);
    final toggle = tester.widget<Switch>(find.byKey(const Key('remember-password')));
    expect(toggle.value, isTrue);
    expect(toggle.onChanged, isNotNull);
    expect(find.text(_line), findsNothing);
    expect(field(tester, 'sign-in-password'), 'stored-before');
  });

  for (final origin in [http, https]) {
    testWidgets('Android sign-in card at ${origin.scheme}', (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(
            body: AndroidSignInView(
              origin: origin,
              id: TextEditingController(text: 'alice'),
              password: TextEditingController(),
              remember: true,
              busy: false,
              version: null,
              onServer: () {},
              onIdChanged: (_) {},
              onRemember: (_) {},
              onLogin: () {},
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final toggle = tester.widget<Switch>(find.byKey(const Key('remember-password')));
      final secure = origin.scheme == 'https';
      expect(toggle.value, secure);
      expect(toggle.onChanged == null, !secure);
      expect(find.text(_line), secure ? findsNothing : findsOneWidget);
    });
  }

  test('Android store: an HTTP origin never reads back or saves a password', () async {
    final calls = <MethodCall>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, (call) async {
          calls.add(call);
          if (call.method == 'login') {
            return {'username': 'alice', 'password': 'stored-before'};
          }
          return null;
        });
    addTearDown(
      () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(AndroidStore.channel, null),
    );
    final store = AndroidStore();
    final plain = await store.login(http);
    expect(plain.username, 'alice');
    expect(plain.password, isNull);
    expect((await store.login(https)).password, 'stored-before');
    await store.saveLogin(http, 'alice', 'typed-now', true);
    final saved = calls.last.arguments as Map;
    expect(saved['remember'], isFalse);
    expect(saved['password'], isNull);
    await store.saveLogin(https, 'alice', 'typed-now', true);
    expect((calls.last.arguments as Map)['remember'], isTrue);
  });
}

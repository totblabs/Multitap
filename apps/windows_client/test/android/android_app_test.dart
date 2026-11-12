import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_probe.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  testWidgets('fresh Android install has no preset server or credentials', (
    tester,
  ) async {
    final methods = <String>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, (call) async {
          methods.add(call.method);
          return null;
        });
    await tester.pumpWidget(const AndroidVdiApp());
    await tester.pumpAndSettle();
    expect(find.text('서버 설정 필요'), findsOneWidget);
    expect(find.textContaining('https://'), findsNothing);
    // Only reads: preferences/version, versioned consent settings and legacy
    // origin fallback. No credentials or server are created on a fresh install.
    expect(methods.toSet(), {
      'preferences',
      'appVersion',
      'transportSettings',
      'origin',
    });
    expect(
      tester.widget<TextField>(find.byKey(const Key('sign-in-id'))).enabled,
      false,
    );
    // Without a server the main button opens the server test, never login.
    await tester.ensureVisible(find.byKey(const Key('login-submit')));
    await tester.tap(find.text('서버 설정'));
    await tester.pumpAndSettle();
    expect(find.text('로그인 서버'), findsOneWidget);
    expect(methods.contains('login'), false);
    expect(tester.takeException(), isNull);
  });

  testWidgets('the keyboard on a Fold inner screen keeps the sign-in field '
      'focused', (tester) async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          AndroidStore.channel,
          (call) async =>
              call.method == 'origin' ? 'https://vdi.example' : null,
        );
    // Galaxy Z Fold inner screen: ≈932×704 dp.
    tester.view.devicePixelRatio = 2.625;
    tester.view.physicalSize = const Size(2448, 1848);
    addTearDown(tester.view.reset);
    await tester.pumpWidget(const AndroidVdiApp());
    await tester.pumpAndSettle();
    final id = find.descendant(
      of: find.byKey(const Key('sign-in-id')),
      matching: find.byType(EditableText),
    );
    await tester.tap(find.byKey(const Key('sign-in-id')));
    await tester.pump();
    final before = tester.state<EditableTextState>(id);
    expect(before.widget.focusNode.hasFocus, isTrue);
    // The keyboard takes ≈300 dp: the form must not switch layouts (that
    // rebuilt the field, dropped its focus and closed the keyboard again).
    tester.view.viewInsets = const FakeViewPadding(bottom: 300 * 2.625);
    await tester.pumpAndSettle();
    final after = tester.state<EditableTextState>(id);
    expect(identical(after, before), isTrue);
    expect(after.widget.focusNode.hasFocus, isTrue);
    // The form moved up by the keyboard's height: the field is above it.
    final visibleBottom = 1848 / 2.625 - 300;
    expect(
      tester.getRect(find.byKey(const Key('sign-in-id'))).bottom,
      lessThanOrEqualTo(visibleBottom),
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('server requires a completed current-input test before saving', (
    tester,
  ) async {
    final pending = Completer<BrokerProbeResult>();
    final saved = <Uri>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: AndroidServerDialog(
            initial: null,
            save: (uri) async => saved.add(uri),
            probe: (_) => pending.future,
          ),
        ),
      ),
    );
    await tester.enterText(find.byType(TextField), 'first.example');
    expect(
      tester
          .widget<FilledButton>(find.widgetWithText(FilledButton, '저장'))
          .onPressed,
      isNull,
    );
    await tester.tap(find.text('연결 테스트'));
    await tester.pump();
    expect(tester.widget<TextField>(find.byType(TextField)).enabled, false);
    expect(
      tester
          .widget<FilledButton>(find.widgetWithText(FilledButton, '저장'))
          .onPressed,
      isNull,
    );
    pending.complete(const BrokerProbeResult(version: '0.5.7', elapsedMs: 12));
    await tester.pumpAndSettle();
    expect(
      tester
          .widget<FilledButton>(find.widgetWithText(FilledButton, '저장'))
          .onPressed,
      isNotNull,
    );
    await tester.enterText(find.byType(TextField), 'other.example');
    await tester.pump();
    expect(
      tester
          .widget<FilledButton>(find.widgetWithText(FilledButton, '저장'))
          .onPressed,
      isNull,
    );
    expect(saved, isEmpty);
  });

  testWidgets('failed broker test cannot save and does not echo peer text', (
    tester,
  ) async {
    final saved = <Uri>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: AndroidServerDialog(
            initial: null,
            save: (uri) async => saved.add(uri),
            probe: (_) async =>
                throw const BrokerProbeException('wrong_server'),
          ),
        ),
      ),
    );
    await tester.enterText(find.byType(TextField), 'https://example.invalid');
    await tester.tap(find.text('연결 테스트'));
    await tester.pumpAndSettle();
    expect(
      tester
          .widget<FilledButton>(find.widgetWithText(FilledButton, '저장'))
          .onPressed,
      isNull,
    );
    expect(find.textContaining('이 주소는 Multitap 서버가 아니에요.'), findsOneWidget);
    expect(saved, isEmpty);
  });

  test('password opt-out never sends plaintext to storage', () async {
    final calls = <MethodCall>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, (call) async {
          calls.add(call);
          return null;
        });
    final store = AndroidStore();
    await store.saveLogin(
      Uri.parse('https://example.invalid'),
      '한글아이디',
      'do-not-store',
      false,
    );
    expect((calls.single.arguments as Map)['password'], isNull);
    expect((calls.single.arguments as Map)['remember'], false);
  });
}

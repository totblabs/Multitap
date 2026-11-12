import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_launcher.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'android_launcher_test.dart' show vm;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  test('manual rates through 100 Mbps survive storage and negotiation', () {
    expect(AndroidStreamQuality.bitrates.last, 100);
    for (final value in const [4, 8, 12, 16, 20, 30, 40, 50, 60, 80, 100]) {
      final wanted = AndroidStreamQuality(bitrate: value);
      expect(AndroidStreamQuality.parse(wanted.encode()).bitrate, value);
      expect(AndroidStreamQuality.of(wanted.options).bitrate, value);
      expect(wanted.options.negotiated(100000000).bitrate, value * 1000000);
      expect(
        wanted.options.negotiated(null).bitrate,
        (value * 1000000).clamp(4000000, 20000000),
      );
      expect(
        wanted.options.negotiated(50000000).bitrate,
        (value * 1000000).clamp(4000000, 50000000),
      );
    }
    const automatic = AndroidStreamQuality.recommended;
    expect(automatic.fps, 60);
    expect(automatic.options.bitrate, 20000000);
    expect(automatic.options.negotiated(100000000).bitrate, 100000000);
    expect(automatic.options.negotiated(null).automaticBitrate, false);
    expect(
      AndroidStreamQuality.parse('{"fps":60,"bitrate":101,"audio":true}')
          .bitrate,
      AndroidStreamQuality.automatic,
    );
  });

  testWidgets('preconnection and live panels offer the same 100 Mbps choice', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(900, 1000);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    StreamOptions? connected;
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => Scaffold(
            body: TextButton(
              onPressed: () async =>
                  connected = await showDialog<StreamOptions>(
                    context: context,
                    builder: (_) => AndroidStreamDialog(desktop: vm()),
                  ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();
    await tester.ensureVisible(find.text('고급 설정'));
    await tester.tap(find.text('고급 설정'));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('bitrate-100')), findsOneWidget);
    await tester.ensureVisible(find.byKey(const Key('bitrate-100')));
    await tester.tap(find.byKey(const Key('bitrate-100')));
    await tester.pump();
    await tester.tap(find.widgetWithText(FilledButton, '연결'));
    await tester.pumpAndSettle();
    expect(connected?.bitrate, 100000000);
    expect(connected?.negotiated(null).bitrate, 20000000);
    expect(connected?.negotiated(100000000).bitrate, 100000000);
    StreamOptions? applied;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: AndroidStreamSettingsPanel(
            options: const StreamOptions(bitrate: 8000000),
            maxBitrate: 100000000,
            onApply: (value) => applied = value,
            onCancel: () {},
          ),
        ),
      ),
    );
    final choice = find.byKey(const Key('stream-bitrate-100'));
    expect(choice, findsOneWidget);
    await tester.ensureVisible(choice);
    await tester.tap(choice);
    await tester.pump();
    await tester.ensureVisible(find.byKey(const Key('apply-stream-settings')));
    await tester.tap(find.byKey(const Key('apply-stream-settings')));
    expect(applied?.bitrate, 100000000);
    expect(applied?.automaticBitrate, false);
    expect(tester.takeException(), isNull);
  });

  for (final size in const [Size(412, 915), Size(844, 390), Size(932, 704)]) {
    testWidgets('password remains focused and reachable with IME at $size', (
      tester,
    ) async {
      tester.view.physicalSize = size;
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(
            AndroidStore.channel,
            (call) async =>
                call.method == 'origin' ? 'https://fixture.invalid' : null,
          );
      await tester.pumpWidget(const AndroidVdiApp());
      await tester.pumpAndSettle();
      final field = find.byKey(const Key('sign-in-password'));
      await tester.ensureVisible(field);
      await tester.tap(field);
      await tester.pump();
      final editable = find.descendant(
        of: field,
        matching: find.byType(EditableText),
      );
      final state = tester.state<EditableTextState>(editable);
      final inset = size.height < 500 ? 220.0 : 330.0;
      tester.view.viewInsets = FakeViewPadding(bottom: inset);
      await tester.pumpAndSettle();
      expect(identical(tester.state<EditableTextState>(editable), state), true);
      expect(state.widget.focusNode.hasFocus, true);
      expect(
        tester.getRect(field).bottom,
        lessThanOrEqualTo(size.height - inset),
      );
      await tester.ensureVisible(find.byKey(const Key('login-submit')));
      await tester.pumpAndSettle();
      expect(
        find.byKey(const Key('login-submit')).hitTestable(),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
    });

    testWidgets(
      'server form and test action remain reachable with IME at $size',
      (tester) async {
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.reset);
        await tester.pumpWidget(
          MaterialApp(
            home: Builder(
              builder: (context) => Scaffold(
                body: Center(
                  child: TextButton(
                    onPressed: () => showDialog<void>(
                      context: context,
                      builder: (_) => AndroidServerDialog(
                        initial: null,
                        save: (_) async {},
                        probe: (_) async => const BrokerProbeResult(
                          version: 'fixture',
                          elapsedMs: 2,
                        ),
                      ),
                    ),
                    child: const Text('open'),
                  ),
                ),
              ),
            ),
          ),
        );
        await tester.tap(find.text('open'));
        await tester.pumpAndSettle();
        await tester.tap(find.byType(TextField));
        final editable = tester.state<EditableTextState>(
          find.byType(EditableText),
        );
        final inset = size.height < 500 ? 220.0 : 330.0;
        tester.view.viewInsets = FakeViewPadding(bottom: inset);
        await tester.pumpAndSettle();
        expect(
          identical(
            tester.state<EditableTextState>(find.byType(EditableText)),
            editable,
          ),
          true,
        );
        expect(editable.widget.focusNode.hasFocus, true);
        await tester.ensureVisible(find.byType(TextField));
        expect(
          tester.getRect(find.byType(TextField)).bottom,
          lessThanOrEqualTo(size.height - inset),
        );
        await tester.ensureVisible(find.text('연결 테스트'));
        expect(find.text('연결 테스트').hitTestable(), findsOneWidget);
        await tester.enterText(find.byType(TextField), 'fixture.invalid');
        await tester.tap(find.text('연결 테스트'));
        await tester.pumpAndSettle();
        await tester.ensureVisible(find.widgetWithText(FilledButton, '저장'));
        expect(
          find.widgetWithText(FilledButton, '저장').hitTestable(),
          findsOneWidget,
        );
        expect(tester.takeException(), isNull);
      },
    );
  }

  for (final size in const [Size(412, 915), Size(844, 390), Size(932, 704)]) {
    testWidgets(
      'login keeps edited fields across IME cycles with 200% text at $size',
      (tester) async {
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.reset);
        final id = TextEditingController(), password = TextEditingController();
        addTearDown(id.dispose);
        addTearDown(password.dispose);
        await tester.pumpWidget(
          MaterialApp(
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(context)
                  .copyWith(textScaler: TextScaler.linear(2)),
              child: child!,
            ),
            home: Scaffold(
              body: AndroidSignInView(
                origin: Uri.parse('https://fixture.invalid'),
                id: id,
                password: password,
                remember: false,
                busy: false,
                version: 'fixture',
                error: '연결하지 못했어요.\n입력한 내용을 확인하세요.\n다시 시도할 수 있어요.',
                onServer: () {},
                onIdChanged: (_) {},
                onRemember: (_) {},
                onLogin: () {},
              ),
            ),
          ),
        );
        final idField = find.byKey(const Key('sign-in-id'));
        final passwordField = find.byKey(const Key('sign-in-password'));
        await tester.ensureVisible(idField);
        await tester.enterText(idField, 'fixture-user');
        final inset = size.height < 500 ? 220.0 : 330.0;
        tester.view.viewInsets = FakeViewPadding(bottom: inset);
        await tester.pumpAndSettle();
        await tester.ensureVisible(passwordField);
        await tester.enterText(passwordField, 'fixture-password');
        final state = tester.state<EditableTextState>(
          find.descendant(
            of: passwordField,
            matching: find.byType(EditableText),
          ),
        );
        tester.view.viewInsets = FakeViewPadding.zero;
        await tester.pumpAndSettle();
        tester.view.viewInsets = FakeViewPadding(bottom: inset);
        await tester.pumpAndSettle();
        expect(
          identical(
            tester.state<EditableTextState>(
              find.descendant(
                of: passwordField,
                matching: find.byType(EditableText),
              ),
            ),
            state,
          ),
          true,
        );
        expect(state.widget.focusNode.hasFocus, true);
        expect(id.text, 'fixture-user');
        expect(password.text, 'fixture-password');
        await tester.ensureVisible(passwordField);
        expect(
          tester.getRect(passwordField).bottom,
          lessThanOrEqualTo(size.height - inset),
        );
        await tester.ensureVisible(find.byKey(const Key('login-submit')));
        expect(
          find.byKey(const Key('login-submit')).hitTestable(),
          findsOneWidget,
        );
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  }

  testWidgets('server test error and actions scroll at 200% text with IME', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(844, 390);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(
      MaterialApp(
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context)
              .copyWith(textScaler: TextScaler.linear(2)),
          child: child!,
        ),
        home: Builder(
          builder: (context) => Scaffold(
            body: TextButton(
              onPressed: () => showDialog<void>(
                context: context,
                builder: (_) => AndroidServerDialog(
                  initial: null,
                  save: (_) async {},
                  probe: (_) async =>
                      throw const BrokerProbeException('network_error'),
                ),
              ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextField), 'fixture.invalid');
    tester.view.viewInsets = const FakeViewPadding(bottom: 220);
    await tester.pumpAndSettle();
    await tester.ensureVisible(find.text('연결 테스트'));
    expect(find.text('연결 테스트').hitTestable(), findsOneWidget);
    await tester.tap(find.text('연결 테스트'));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('server-test-result')), findsOneWidget);
    await tester.ensureVisible(find.text('취소'));
    expect(find.text('취소').hitTestable(), findsOneWidget);
    expect(
      tester
          .widget<FilledButton>(find.widgetWithText(FilledButton, '저장'))
          .onPressed,
      isNull,
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('closing a pending server form cannot save a late result', (
    tester,
  ) async {
    final pending = Completer<BrokerProbeResult>();
    final saved = <Uri>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => Scaffold(
            body: TextButton(
              onPressed: () => showDialog<void>(
                context: context,
                builder: (_) => AndroidServerDialog(
                  initial: null,
                  save: (uri) async => saved.add(uri),
                  probe: (_) => pending.future,
                ),
              ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextField), 'fixture.invalid');
    await tester.tap(find.text('연결 테스트'));
    await tester.pump();
    Navigator.of(tester.element(find.byType(AndroidServerDialog))).pop();
    await tester.pumpAndSettle();
    pending.complete(
      const BrokerProbeResult(version: 'late-fixture', elapsedMs: 1),
    );
    await tester.pumpAndSettle();
    expect(saved, isEmpty);
    expect(find.byType(AndroidServerDialog), findsNothing);
    expect(tester.takeException(), isNull);
  });
}

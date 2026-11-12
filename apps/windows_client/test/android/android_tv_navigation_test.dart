import '../helpers/stream_connect_consent.dart' show acceptStreamConnect;
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_application.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/ui/app_theme.dart';

import 'android_launcher_test.dart'
    show FakeBroker, MemoryStore, vm, mainButton;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUp(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          const MethodChannel('multitap/pen-input'),
          (_) async => null,
        );
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          AndroidSessionKeepalive.channel,
          (call) async => call.method == 'start' ? 'started' : true,
        );
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          AndroidApplication.channel,
          (call) async => call.method == 'isTelevision'
              ? true
              : call.method == 'tvEditText' &&
                    (call.arguments as Map)['type'] == 'password'
              ? 'test-password'
              : null,
        );
  });
  tearDown(() {
    for (final channel in [
      AndroidApplication.channel,
      AndroidStore.channel,
      AndroidSessionKeepalive.channel,
      const MethodChannel('multitap/pen-input'),
    ]) {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null);
    }
  });

  Future<void> native(WidgetTester tester, String method, bool value) async {
    final done = Completer<void>();
    tester.binding.defaultBinaryMessenger.handlePlatformMessage(
      AndroidApplication.channel.name,
      const StandardMethodCodec().encodeMethodCall(MethodCall(method, value)),
      (_) => done.complete(),
    );
    await done.future;
    await tester.pumpAndSettle();
  }

  testWidgets(
    'TV keeps native control focus without an extra overlay after relayout',
    (tester) async {
      late StateSetter change;
      var top = 20.0;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          builder: (_, child) =>
              AndroidTvNavigation(television: true, child: child!),
          home: Scaffold(
            body: StatefulBuilder(
              builder: (_, setState) {
                change = setState;
                return Column(
                  children: [
                    SizedBox(height: top),
                    FilledButton(
                      key: const Key('moving-control'),
                      autofocus: true,
                      onPressed: () {},
                      child: const Text('Action'),
                    ),
                  ],
                );
              },
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      final ring = find.byKey(const Key('android-tv-focus-ring'));
      expect(ring, findsNothing);
      final before = FocusManager.instance.primaryFocus;
      change(() => top = 120);
      await tester.pumpAndSettle();
      expect(ring, findsNothing);
      expect(FocusManager.instance.primaryFocus, same(before));
      expect(tester.binding.hasScheduledFrame, false);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'TV hardware keyboard types immediately; D-pad still changes fields',
    (tester) async {
      MemoryStore(origin: 'https://vdi.example', username: '').install();
      await tester.pumpWidget(
        const AndroidVdiApp(television: true, hardwareKeyboard: true),
      );
      await tester.pumpAndSettle();
      await native(tester, 'hardwareNavigation', false);
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await tester.pumpAndSettle();
      TextField field(String key) =>
          tester.widget<TextField>(find.byKey(Key(key)));
      expect(field('sign-in-id').readOnly, false);
      await tester.enterText(find.byKey(const Key('sign-in-id')), 'first');
      expect(field('sign-in-id').controller!.text, 'first');
      await native(tester, 'hardwareNavigation', true);
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowLeft);
      await tester.pumpAndSettle();
      expect(field('sign-in-id').focusNode!.hasFocus, true);
      expect(field('sign-in-id').controller!.selection.baseOffset, 4);
      await native(tester, 'hardwareNavigation', false);
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await tester.pumpAndSettle();
      expect(field('sign-in-password').focusNode!.hasFocus, true);
      await native(tester, 'hardwareKeyboardChanged', false);
      expect(field('sign-in-password').readOnly, true);
      await native(tester, 'hardwareKeyboardChanged', true);
      expect(field('sign-in-password').readOnly, false);
      await tester.enterText(find.byKey(const Key('sign-in-password')), 'a');
      expect(field('sign-in-password').controller!.text, 'a');
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('TV starts on a visible control and OK opens server settings', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1920, 1080);
    tester.view.devicePixelRatio = 2;
    addTearDown(tester.view.reset);
    MemoryStore().install();
    await tester.pumpWidget(const AndroidVdiApp(television: true));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-tv-focus-ring')), findsNothing);
    await tester.sendKeyEvent(LogicalKeyboardKey.select);
    await tester.pumpAndSettle();
    expect(find.text('로그인 서버'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  testWidgets('TV OK opens native editor once and cancel restores navigation', (
    tester,
  ) async {
    final reply = Completer<String?>();
    var opens = 0;
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidApplication.channel, (call) async {
          if (call.method == 'tvEditText') {
            opens++;
            return reply.future;
          }
          return null;
        });
    MemoryStore(origin: 'https://vdi.example', username: 'tester').install();
    await tester.pumpWidget(const AndroidVdiApp(television: true));
    await tester.pumpAndSettle();
    await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
    await tester.pumpAndSettle();
    await tester.sendKeyEvent(LogicalKeyboardKey.select);
    await tester.pumpAndSettle();
    expect(opens, 1);
    expect(tester.testTextInput.isVisible, false);
    for (final key in [
      LogicalKeyboardKey.arrowLeft,
      LogicalKeyboardKey.arrowDown,
      LogicalKeyboardKey.select,
    ]) {
      expect(await tester.sendKeyEvent(key), false);
      await tester.pump();
    }
    expect(opens, 1);
    expect(
      tester
          .widget<TextField>(find.byKey(const Key('sign-in-id')))
          .focusNode!
          .hasFocus,
      true,
    );
    reply.complete(null);
    await tester.pumpAndSettle();
    await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
    await tester.pumpAndSettle();
    expect(
      tester
          .widget<TextField>(find.byKey(const Key('sign-in-password')))
          .focusNode!
          .hasFocus,
      true,
    );
    expect(tester.takeException(), isNull);
  });

  // Any arrangement an earlier version saved opens the one card grid.
  for (final view in const ['cards', 'monitorWall', 'docking']) {
    testWidgets('TV $view: OK on the primary action connects once directly', (
      tester,
    ) async {
      final memory = MemoryStore(
        origin: 'https://vdi.example',
        username: 'tester',
        prefs: {'launcher.view': view},
      );
      memory.install();
      // A one-click API request requires the Host's actual owned-output
      // observation; the old confirmation-only fixture had no display report.
      final broker = FakeBroker([vm(ownedOutput: true)]);
      tester.view.physicalSize = const Size(1920, 1080);
      tester.view.devicePixelRatio = 2;
      addTearDown(tester.view.reset);
      await tester.pumpWidget(
        AndroidVdiApp(television: true, apiFactory: (_) => broker),
      );
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.select);
      await tester.pumpAndSettle();
      final primary = mainButton('pve-149');
      // Start at the actual primary control, then verify remote activation and
      // connection. Focus acquisition here simulates a traversal endpoint.
      final button = tester.widget<FilledButton>(primary);
      // Resolve through the built action's Focus context, never call onPressed.
      Focus.of(
        tester.element(
          find
              .descendant(of: primary, matching: find.byType(MouseRegion))
              .first,
        ),
      ).requestFocus();
      await tester.pumpAndSettle();
      expect(button.onPressed, isNotNull);
      expect(broker.calls.contains('connect'), false);
      await tester.sendKeyEvent(LogicalKeyboardKey.select);
      await acceptStreamConnect(tester, 'pve-149');
      await tester.pumpAndSettle();
      expect(find.text('연결 확인'), findsNothing);
      expect(broker.calls.where((call) => call == 'connect'), hasLength(1));
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    });
  }
}

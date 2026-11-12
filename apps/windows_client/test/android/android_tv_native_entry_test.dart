// Native dialog replies are faked. These checks cover Flutter ownership and
// application gates, not a physical TV remote or an Android IME key grid.
import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_application.dart';
import 'package:multitap_client/android/android_launcher.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/ui/app_theme.dart';

import 'android_launcher_test.dart' show FakeBroker, MemoryStore;

class _NativeEditor {
  final requests = <Map<String, Object?>>[];
  final pending = <Completer<String?>>[];

  void install() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidApplication.channel, (call) async {
          if (call.method == 'tvEditText') {
            requests.add(Map<String, Object?>.from(call.arguments as Map));
            final reply = Completer<String?>();
            pending.add(reply);
            return reply.future;
          }
          if (call.method == 'isTelevision') return true;
          if (call.method == 'hasHardwareKeyboard' ||
              call.method == 'imeVisible') {
            return false;
          }
          return null;
        });
  }

  void answer(String? text) => pending.removeAt(0).complete(text);
}

Widget _tvApp(Widget home, {Brightness brightness = Brightness.dark}) =>
    MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildAppTheme(brightness: brightness),
      builder: (_, child) =>
          AndroidTvNavigation(television: true, child: child!),
      home: Scaffold(body: home),
    );

Future<void> _openServer(
  WidgetTester tester, {
  Uri? initial,
  required Future<BrokerProbeResult> Function(Uri) probe,
  required Future<void> Function(Uri) save,
}) async {
  await tester.pumpWidget(
    _tvApp(
      Builder(
        builder: (context) => FilledButton(
          key: const Key('open-server'),
          onPressed: () => showDialog<Uri>(
            context: context,
            builder: (_) =>
                AndroidServerDialog(initial: initial, probe: probe, save: save),
          ),
          child: const Text('서버 설정'),
        ),
      ),
    ),
  );
  await tester.tap(find.byKey(const Key('open-server')));
  await tester.pumpAndSettle();
}

FilledButton _saveButton(WidgetTester tester) =>
    tester.widget<FilledButton>(find.widgetWithText(FilledButton, '저장'));

Future<void> _selectField(WidgetTester tester, Finder finder) async {
  tester.widget<TextField>(finder).focusNode!.requestFocus();
  await tester.pumpAndSettle();
  await tester.sendKeyEvent(LogicalKeyboardKey.select);
  await tester.pumpAndSettle();
}

Widget _signIn(
  TextEditingController id,
  TextEditingController password, {
  ValueChanged<String>? onChanged,
}) => AndroidSignInView(
  origin: Uri.parse('https://fixture.invalid'),
  id: id,
  password: password,
  remember: false,
  busy: false,
  version: null,
  onServer: () {},
  onIdChanged: onChanged ?? (_) {},
  onRemember: (_) {},
  onLogin: () {},
);

Future<void> _close(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pumpAndSettle();
}

Future<void> _capture(
  WidgetTester tester,
  GlobalKey boundary,
  String name,
) async {
  final directory = Platform.environment['TV_ENTRY_CAPTURE_DIR'];
  if (directory == null) return;
  await tester.runAsync(() async {
    final image =
        await (boundary.currentContext!.findRenderObject()
                as RenderRepaintBoundary)
            .toImage(pixelRatio: 1);
    try {
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      await Directory(directory).create(recursive: true);
      final phase = Platform.environment['TV_ENTRY_BASELINE'] == 'true'
          ? 'before'
          : 'after';
      await File('$directory/$phase-$name.png')
          .writeAsBytes(bytes!.buffer.asUint8List());
    } finally {
      image.dispose();
    }
  });
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  late _NativeEditor native;
  setUpAll(() async {
    final fonts = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      fonts.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await fonts.load();
    await (FontLoader(
      'MaterialIcons',
    )..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'))).load();
  });
  setUp(() {
    AndroidApplication.hardwareNavigation = false;
    AndroidApplication.imeVisible.value = false;
    AndroidApplication.imeInsetVisible.value = false;
    native = _NativeEditor()..install();
  });
  tearDown(() {
    for (final channel in [AndroidApplication.channel, AndroidStore.channel]) {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null);
    }
  });

  testWidgets(
    'TV34 native server address still requires its own successful test before save',
    (tester) async {
      final probes = <Uri>[];
      final answers = <Completer<BrokerProbeResult>>[];
      final saved = <Uri>[];
      await _openServer(
        tester,
        probe: (uri) {
          probes.add(uri);
          final answer = Completer<BrokerProbeResult>();
          answers.add(answer);
          return answer.future;
        },
        save: (uri) async {
          saved.add(uri);
        },
      );
      expect(_saveButton(tester).onPressed, isNull);
      expect(
        native.requests,
        isEmpty,
        reason: 'D-pad focus alone opens no editor',
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.select);
      await tester.pumpAndSettle();
      expect(native.requests.single['type'], 'url');
      expect(native.requests.single['action'], 'go');
      expect(native.requests.single['maxLength'], 2048);
      native.answer('first.example');
      await tester.pump();
      await tester.pump();
      expect(probes, [Uri.parse('https://first.example')]);
      expect(tester.widget<TextField>(find.byType(TextField)).enabled, isFalse);
      expect(_saveButton(tester).onPressed, isNull);
      answers
          .removeAt(0)
          .complete(const BrokerProbeResult(version: '0.5.71', elapsedMs: 12));
      await tester.pumpAndSettle();
      expect(_saveButton(tester).onPressed, isNotNull);
      await _selectField(tester, find.byType(TextField));
      native.answer('second.example');
      await tester.pump();
      await tester.pump();
      expect(probes.last, Uri.parse('https://second.example'));
      expect(
        _saveButton(tester).onPressed,
        isNull,
        reason: 'the previous successful address is no longer saveable',
      );
      answers
          .removeAt(0)
          .completeError(const BrokerProbeException('wrong_server'));
      await tester.pumpAndSettle();
      expect(_saveButton(tester).onPressed, isNull);
      expect(saved, isEmpty);
      await tester.tap(find.text('연결 테스트'));
      await tester.pump();
      answers
          .removeAt(0)
          .complete(const BrokerProbeResult(version: '0.5.71', elapsedMs: 9));
      await tester.pumpAndSettle();
      await tester.tap(find.text('저장'));
      await tester.pumpAndSettle();
      expect(saved, [Uri.parse('https://second.example')]);
      expect(find.text('로그인 서버'), findsNothing);
      expect(tester.takeException(), isNull);
      await _close(tester);
    },
  );

  testWidgets(
    'TV34 cancel preserves address and duplicate OK or tap opens only one native editor',
    (tester) async {
      var probes = 0;
      await _openServer(
        tester,
        initial: Uri.parse('https://saved.example'),
        probe: (_) async {
          probes++;
          return const BrokerProbeResult(version: '0.5.71', elapsedMs: 1);
        },
        save: (_) async {},
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.select);
      await tester.pumpAndSettle();
      await tester.sendKeyEvent(LogicalKeyboardKey.select);
      await tester.tap(find.byType(TextField));
      await tester.pumpAndSettle();
      expect(native.requests, hasLength(1));
      native.answer(null);
      await tester.pumpAndSettle();
      expect(
        tester.widget<TextField>(find.byType(TextField)).controller!.text,
        'https://saved.example',
      );
      expect(probes, 0);
      expect(_saveButton(tester).onPressed, isNull);
      await _selectField(tester, find.byType(TextField));
      expect(
        native.requests,
        hasLength(2),
        reason: 'cancel releases native request ownership',
      );
      native.answer(null);
      await tester.pumpAndSettle();
      await _close(tester);
    },
  );

  for (final replaceController in [false, true]) {
    testWidgets(
      'TV34 late native reply cannot overwrite ${replaceController ? 'a replacement controller' : 'a newer local edit'}',
      (tester) async {
        final original = TextEditingController(text: 'old');
        final replacement = TextEditingController(text: 'newer');
        final password = TextEditingController();
        addTearDown(original.dispose);
        addTearDown(replacement.dispose);
        addTearDown(password.dispose);
        var current = original, changed = 0;
        late StateSetter rebuild;
        await tester.pumpWidget(
          _tvApp(
            StatefulBuilder(
              builder: (_, setState) {
                rebuild = setState;
                return _signIn(
                  current,
                  password,
                  onChanged: (_) {
                    changed++;
                  },
                );
              },
            ),
          ),
        );
        await tester.pumpAndSettle();
        await _selectField(tester, find.byKey(const Key('sign-in-id')));
        if (replaceController) {
          rebuild(() => current = replacement);
          await tester.pump();
        } else {
          original.text = 'newer';
        }
        native.answer('stale');
        await tester.pumpAndSettle();
        expect(current.text, 'newer');
        expect(changed, 0);
        expect(tester.takeException(), isNull);
        await _close(tester);
      },
    );
  }

  testWidgets(
    'TV34 late native reply after unmount neither edits nor submits',
    (tester) async {
      final id = TextEditingController(text: 'original');
      final password = TextEditingController();
      addTearDown(id.dispose);
      addTearDown(password.dispose);
      var changed = 0;
      await tester.pumpWidget(
        _tvApp(
          _signIn(
            id,
            password,
            onChanged: (_) {
              changed++;
            },
          ),
        ),
      );
      await tester.pumpAndSettle();
      await _selectField(tester, find.byKey(const Key('sign-in-id')));
      expect(native.requests, hasLength(1));
      await _close(tester);
      native.answer('late');
      await tester.pumpAndSettle();
      expect(id.text, 'original');
      expect(changed, 0);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'TV34 native ID next focuses password and password done signs in through the existing broker',
    (tester) async {
      MemoryStore(origin: 'https://fixture.invalid').install();
      final broker = FakeBroker([]);
      await tester.pumpWidget(
        AndroidVdiApp(television: true, apiFactory: (_) => broker),
      );
      await tester.pumpAndSettle();
      await _selectField(tester, find.byKey(const Key('sign-in-id')));
      expect(native.requests.single['type'], 'text');
      expect(native.requests.single['action'], 'next');
      native.answer('tv-fixture');
      await tester.pumpAndSettle();
      final password = tester.widget<TextField>(
        find.byKey(const Key('sign-in-password')),
      );
      expect(password.focusNode!.hasFocus, isTrue);
      await tester.sendKeyEvent(LogicalKeyboardKey.select);
      await tester.pumpAndSettle();
      expect(native.requests.last['type'], 'password');
      expect(native.requests.last['action'], 'done');
      expect(native.requests.last['maxLength'], 256);
      native.answer('fixture-only-secret');
      await tester.pumpAndSettle();
      expect(broker.calls.where((call) => call.startsWith('login:')), [
        'login:tv-fixture',
      ]);
      expect(broker.calls, contains('desktops'));
      expect(find.text('내 데스크톱'), findsOneWidget);
      expect(tester.takeException(), isNull);
      await _close(tester);
    },
  );

  testWidgets(
    'TV34 unchanged native ID next preserves remembered password without forgetting it',
    (tester) async {
      final methods = <String>[];
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(AndroidStore.channel, (call) async {
            methods.add(call.method);
            return switch (call.method) {
              'origin' => 'https://fixture.invalid',
              'login' => {
                'username': 'tv-fixture',
                'password': 'fixture-only-secret',
              },
              'preferences' => <String, String>{},
              _ => null,
            };
          });
      await tester.pumpWidget(
        AndroidVdiApp(television: true, apiFactory: (_) => FakeBroker([])),
      );
      await tester.pumpAndSettle();
      final password = tester.widget<TextField>(
        find.byKey(const Key('sign-in-password')),
      );
      expect(password.controller!.text, 'fixture-only-secret');
      await _selectField(tester, find.byKey(const Key('sign-in-id')));
      native.answer('tv-fixture');
      await tester.pumpAndSettle();
      expect(password.controller!.text, 'fixture-only-secret');
      expect(password.focusNode!.hasFocus, isTrue);
      expect(methods, isNot(contains('forgetPassword')));
      expect(
        tester.widget<Switch>(find.byKey(const Key('remember-password'))).value,
        isTrue,
      );
      expect(tester.takeException(), isNull);
      await _close(tester);
    },
  );

  testWidgets(
    'TV34 unmount while Flutter keyboard hide is pending starts no native editor',
    (tester) async {
      final hidden = Completer<void>();
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(SystemChannels.textInput, (call) async {
            if (call.method == 'TextInput.hide') await hidden.future;
            return null;
          });
      addTearDown(tester.testTextInput.register);
      final id = TextEditingController(text: 'original');
      final password = TextEditingController();
      addTearDown(id.dispose);
      addTearDown(password.dispose);
      await tester.pumpWidget(_tvApp(_signIn(id, password)));
      await tester.pumpAndSettle();
      await _selectField(tester, find.byKey(const Key('sign-in-id')));
      expect(native.requests, isEmpty);
      await _close(tester);
      hidden.complete();
      await tester.pumpAndSettle();
      expect(native.requests, isEmpty);
      expect(tester.takeException(), isNull);
    },
  );

  for (final spec in [
    (const Size(960, 540), 'dark'),
    (const Size(1920, 1080), 'light'),
  ]) {
    testWidgets(
      'TV34 focus render ${spec.$1.width.toInt()} ${spec.$2} 150 percent',
      (tester) async {
        tester.view.physicalSize = spec.$1;
        tester.view.devicePixelRatio = 1;
        tester.platformDispatcher.textScaleFactorTestValue = 1.5;
        final shadows = debugDisableShadows;
        debugDisableShadows = false;
        addTearDown(() {
          tester.view.reset();
          tester.platformDispatcher.clearTextScaleFactorTestValue();
          debugDisableShadows = shadows;
        });
        try {
          MemoryStore(
            origin: 'https://fixture.invalid',
            prefs: {'appearance.theme': spec.$2},
          ).install();
          final boundary = GlobalKey();
          await tester.pumpWidget(
            RepaintBoundary(
              key: boundary,
              child: AndroidVdiApp(
                television: true,
                apiFactory: (_) => FakeBroker([]),
              ),
            ),
          );
          await tester.pumpAndSettle();
          final id = tester.widget<TextField>(
            find.byKey(const Key('sign-in-id')),
          );
          id.focusNode!.requestFocus();
          await tester.pumpAndSettle();
          expect(id.focusNode!.hasFocus, isTrue);
          if (Platform.environment['TV_ENTRY_BASELINE'] != 'true') {
            expect(
              find.byKey(const Key('android-tv-focus-ring')),
              findsNothing,
            );
            expect(find.byKey(const Key('tv-login-columns')), findsOneWidget);
            final card = tester.getRect(find.byKey(const Key('login-card')));
            expect(card.left, greaterThanOrEqualTo(48));
            expect(card.right, lessThanOrEqualTo(spec.$1.width - 48));
            expect(card.width, inInclusiveRange(420, 480));
            expect(find.text('비밀번호를 암호화해\n이 기기에 보관해요'), findsNothing);
            final update = tester.getRect(
              find.byKey(const Key('login-update-button')),
            );
            expect(update.top, greaterThanOrEqualTo(0));
            expect(update.bottom, lessThanOrEqualTo(spec.$1.height));
          }
          await _capture(
            tester,
            boundary,
            'login-${spec.$1.width.toInt()}-${spec.$2}-150',
          );
          await tester.tap(find.byKey(const Key('login-server-label')));
          await tester.pumpAndSettle();
          final address = tester.widget<TextField>(find.byType(TextField).last);
          expect(find.text('로그인 서버'), findsOneWidget);
          expect(address.focusNode!.hasFocus, isTrue);
          final save = tester.getRect(find.widgetWithText(FilledButton, '저장'));
          expect(save.left, greaterThanOrEqualTo(0));
          expect(save.right, lessThanOrEqualTo(spec.$1.width));
          expect(save.bottom, lessThanOrEqualTo(spec.$1.height));
          expect(
            native.requests,
            isEmpty,
            reason: 'rendering focus alone launches no native editor',
          );
          expect(tester.takeException(), isNull);
          await _capture(
            tester,
            boundary,
            'server-${spec.$1.width.toInt()}-${spec.$2}-150',
          );
          await _close(tester);
        } finally {
          debugDisableShadows = shadows;
        }
      },
    );
  }
}

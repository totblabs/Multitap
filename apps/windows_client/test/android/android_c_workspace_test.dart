import '../helpers/stream_connect_consent.dart' show acceptStreamConnect;
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_launcher.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_toolbar.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_tile.dart';
import 'package:multitap_client/ui/session_menu.dart';
import 'package:multitap_client/ui/vm_resource_bars.dart';

import 'android_launcher_test.dart'
    show FakeBroker, MemoryStore, vm, mainButton;

Future<void> _tap(WidgetTester t, Finder f) async {
  await t.ensureVisible(f);
  await t.tap(f);
  await t.pumpAndSettle();
}

Future<void> _capture(WidgetTester t, GlobalKey key, String name) async {
  final directory = Platform.environment['ANDROID_C_CAPTURE_DIR'];
  if (directory == null) return;
  await t.runAsync(() async {
    final image =
        await (key.currentContext!.findRenderObject() as RenderRepaintBoundary)
            .toImage(pixelRatio: 2);
    try {
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      await Directory(directory).create(recursive: true);
      await File('$directory/$name.png')
          .writeAsBytes(bytes!.buffer.asUint8List());
    } finally {
      image.dispose();
    }
  });
}

void main() {
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      (_) async => null,
    );
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? true : null,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/pen-input'),
      null,
    );
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    final font = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      font.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await font.load();
    await (FontLoader(
      'MaterialIcons',
    )..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'))).load();
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });

  for (final (size, scale) in const [
    (Size(360, 800), 1.0),
    (Size(320, 760), 1.0),
    (Size(844, 390), 1.0),
    (Size(360, 800), 1.6),
    (Size(320, 760), 1.6),
    (Size(844, 390), 1.6),
  ]) {
    testWidgets(
      'C login keeps named server and manual sign-in reachable at $size scale $scale',
      (t) async {
        t.view.physicalSize = size;
        t.view.devicePixelRatio = 1;
        addTearDown(t.view.reset);
        t.platformDispatcher.textScaleFactorTestValue = scale;
        addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
        var logins = 0, servers = 0;
        final id = TextEditingController(text: 'tester');
        final password = TextEditingController(text: 'fixture-vault-password');
        addTearDown(id.dispose);
        addTearDown(password.dispose);
        final boundary = GlobalKey();
        await t.pumpWidget(
          MaterialApp(
            theme: buildAppTheme(brightness: Brightness.dark),
            home: RepaintBoundary(
              key: boundary,
              child: Scaffold(
                body: AndroidSignInView(
                  origin: Uri.parse('https://vdi.example'),
                  id: id,
                  password: password,
                  remember: true,
                  busy: false,
                  version: 'future-source',
                  onServer: () => servers++,
                  onIdChanged: (_) {},
                  onRemember: (_) {},
                  onLogin: () => logins++,
                ),
              ),
            ),
          ),
        );
        await t.pumpAndSettle();
        // The card is the Windows one (design audit F-31): no heading of its
        // own, the server as one line at its head.
        expect(
          find.byKey(const Key('android-c-login-heading')),
          findsNothing,
        );
        expect(find.text('vdi.example'), findsOneWidget);
        expect(find.text('서버 변경'), findsOneWidget);
        expect(logins, 0);
        if (size.width == 360) {
          await _capture(t, boundary, 'login-c-360-$scale');
        }
        await _tap(t, find.byKey(const Key('open-server-settings')));
        expect(servers, 1);
        expect(logins, 0);
        await _tap(t, find.byKey(const Key('login-submit')));
        expect(logins, 1);
        expect(t.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'C workspace keeps original cards and resources with immediate explicit connect',
    (t) async {
      t.view.physicalSize = const Size(360, 800);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      final store = MemoryStore(
        origin: 'https://vdi.example',
        username: 'tester',
      );
      store.install();
      final broker = FakeBroker([vm()]);
      final boundary = GlobalKey();
      await t.pumpWidget(
        RepaintBoundary(
          key: boundary,
          child: AndroidVdiApp(apiFactory: (_) => broker),
        ),
      );
      await t.pumpAndSettle();
      await _capture(t, boundary, 'login-production-c-360');
      await t.enterText(
        find.byKey(const Key('sign-in-password')),
        'fixture-password',
      );
      await _tap(t, find.byKey(const Key('login-submit')));
      expect(find.text('내 데스크톱'), findsOneWidget);
      expect(find.byType(DesktopTile), findsOneWidget);
      expect(find.byType(VmResourceStrip), findsOneWidget);
      expect(find.byKey(const Key('account-menu')), findsOneWidget);
      expect(
        find.byKey(const Key('android-header-update-action')),
        findsOneWidget,
      );
      expect(broker.calls.contains('connect'), isFalse);
      await _capture(t, boundary, 'workspace-c-360');
      await t.ensureVisible(find.byType(VmResourceStrip));
      expect(find.byType(VmResourceStrip).hitTestable(), findsOneWidget);
      expect(
        broker.calls.where((v) => v == 'connect' || v.startsWith('power:')),
        isEmpty,
      );
      expect(find.text('컴퓨터 정보'), findsNothing);
      await _tap(t, mainButton('pve-149'));
      await acceptStreamConnect(t, 'pve-149');
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(find.byKey(const Key('android-session-panel')), findsOneWidget);
      expect(broker.calls.where((v) => v.startsWith('power:')), isEmpty);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
      await t.pumpAndSettle();
    },
  );

  testWidgets(
    'C stream details preserve custom quality and request audio automatically; reset is draft-only',
    (t) async {
      t.view.physicalSize = const Size(412, 915);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      StreamOptions? result;
      final boundary = GlobalKey();
      await t.pumpWidget(
        RepaintBoundary(
          key: boundary,
          child: MaterialApp(
            theme: buildAppTheme(brightness: Brightness.dark),
            home: Builder(
              builder: (context) => Scaffold(
                body: TextButton(
                  onPressed: () async =>
                      result = await showDialog<StreamOptions>(
                        context: context,
                        builder: (_) => AndroidStreamDialog(
                          desktop: vm(),
                          initial: const AndroidStreamQuality(
                            fps: 30,
                            bitrate: 100,
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
      await _tap(t, find.text('open'));
      expect(find.byKey(const Key('fps-30')).hitTestable(), findsNothing);
      expect(find.byKey(const Key('android-c-stream-audio')), findsNothing);
      expect(
        t.widget<Text>(find.byKey(const Key('android-c-stream-summary'))).data,
        contains('30 fps'),
      );
      await _capture(t, boundary, 'stream-basic-c-412');
      await _tap(t, find.text('고급 설정'));
      expect(
        t.widget<ChoiceChip>(find.byKey(const Key('fps-30'))).selected,
        true,
      );
      expect(
        t.widget<ChoiceChip>(find.byKey(const Key('bitrate-100'))).selected,
        true,
      );
      await _tap(t, find.text('고급 설정'));
      await _tap(t, find.widgetWithText(FilledButton, '연결'));
      expect(result?.fps, 30);
      expect(result?.bitrate, 100000000);
      expect(result?.audio, true);
      await _tap(t, find.text('open'));
      await _tap(t, find.byKey(const Key('android-c-use-recommended')));
      expect(
        t.widget<Text>(find.byKey(const Key('android-c-stream-summary'))).data,
        contains('60 fps'),
      );
      expect(find.byKey(const Key('android-c-stream-audio')), findsNothing);
      await _tap(t, find.widgetWithText(TextButton, '취소'));
      expect(result, isNull);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'C amber hamburger retains input keyboard order and Escape returns focus',
    (t) async {
      t.view.physicalSize = const Size(412, 915);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      final held = ValueNotifier(false);
      addTearDown(held.dispose);
      final boundary = GlobalKey();
      var input = 0, keyboard = 0, commands = 0;
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: RepaintBoundary(
            key: boundary,
            child: Scaffold(
              body: AndroidSessionBar(
                title: 'TESTER',
                modeLabel: '스트리밍',
                bubbleArea: const Rect.fromLTWH(0, 0, 412, 915),
                pointerHeld: held,
                onInteract: () {},
                onDisconnect: () {},
                latencyMs: 8,
                quick: [
                  AndroidQuickButton(
                    key: const Key('input'),
                    icon: Icons.mouse_rounded,
                    label: '입력',
                    tooltip: '입력 방식',
                    onPressed: () => input++,
                  ),
                  AndroidQuickButton(
                    key: const Key('keyboard'),
                    icon: Icons.keyboard_rounded,
                    tooltip: '키보드 열기',
                    onPressed: () => keyboard++,
                  ),
                ],
                actions: [
                  for (final (label, icon) in [
                    ('화면', Icons.fullscreen_rounded),
                    ('배율', Icons.text_fields_rounded),
                    ('화질', Icons.tune_rounded),
                    ('사용안내', Icons.help_outline_rounded),
                  ])
                    SessionToolbarMenu.command(
                      label: label,
                      icon: icon,
                      onPressed: () => commands++,
                      onInteract: () {},
                    ),
                ],
              ),
            ),
          ),
        ),
      );
      await t.pumpAndSettle();
      expect(find.text('메뉴'), findsOneWidget);
      expect(find.text('키보드'), findsOneWidget);
      expect(
        t.getCenter(find.byKey(const Key('expand-session-toolbar'))).dy,
        lessThan(t.getCenter(find.byKey(const Key('input'))).dy),
      );
      expect(
        t.getCenter(find.byKey(const Key('input'))).dy,
        lessThan(t.getCenter(find.byKey(const Key('keyboard'))).dy),
      );
      await _tap(t, find.byKey(const Key('expand-session-toolbar')));
      // 연결 품질 is the card's first row, with the latency and its word.
      expect(
        find.descendant(
          of: find.byKey(const Key('session-quality')),
          matching: find.text('연결 품질'),
        ),
        findsOneWidget,
      );
      expect(find.text('8 ms · 좋음'), findsOneWidget);
      // This flat list keeps the PC's groups: 화면 (its caption over 화면 and
      // 배율) and 설정 (the caption over 화질 and 사용안내); the former
      // captions are gone.
      expect(
        [
          for (final caption in t.widgetList<AndroidMenuCaption>(
            find.byType(AndroidMenuCaption),
          ))
            caption.label,
        ],
        ['화면', '설정'],
      );
      expect(find.text('화면'), findsNWidgets(2));
      expect(find.text('설정'), findsOneWidget);
      expect(find.text('화질'), findsOneWidget);
      for (final gone in ['화면·영상', '내 기기 공유', '입력·도움말']) {
        expect(find.text(gone), findsNothing, reason: gone);
      }
      expect(find.text('보기 · 배율 · 해상도'), findsOneWidget);
      expect(find.text('프레임·전송률'), findsOneWidget);
      expect(commands, 0);
      await _capture(t, boundary, 'toolbar-c-412');
      await t.sendKeyEvent(LogicalKeyboardKey.escape);
      await t.pumpAndSettle();
      expect(find.byKey(const Key('session-command-bar')), findsNothing);
      expect(
        FocusManager.instance.primaryFocus?.debugLabel,
        'Android menu toggle',
      );
      await _tap(t, find.byKey(const Key('input')));
      await _tap(t, find.byKey(const Key('keyboard')));
      expect(input, 1);
      expect(keyboard, 1);
      expect(commands, 0);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
      await t.pumpAndSettle();
    },
  );
}

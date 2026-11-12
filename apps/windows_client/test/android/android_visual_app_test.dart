import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/ui/desktop_tile.dart';

import 'android_launcher_test.dart' show FakeBroker, MemoryStore, vm;
import 'android_ui_audit.dart';

class _VisualBroker extends FakeBroker {
  _VisualBroker() : super([vm(name: '내 작업 PC'), vm(id: 'pve-150', name: '개발 PC')]);

  @override
  Future<BrokerSession> login(String username, String password) async {
    final now = DateTime.now().toUtc();
    return BrokerSession(
      token: 'synthetic-visual-session',
      username: username,
      expiresAt: now.add(const Duration(hours: 1)),
      sessionAccess: SessionAccess(
        idleTimeout: const Duration(minutes: 30),
        warning: const Duration(minutes: 1),
        serverTime: now,
        idleEnabled: true,
        idleDeadline: now.add(const Duration(minutes: 30)),
      ),
    );
  }
}

Future<void> _capture(WidgetTester t, GlobalKey boundary, String name) async {
  final directory = Platform.environment['ANDROID_VISUAL_APP_CAPTURE_DIR'];
  if (directory == null) return;
  await t.runAsync(() async {
    final image = await (boundary.currentContext!.findRenderObject()
            as RenderRepaintBoundary)
        .toImage(pixelRatio: 2);
    try {
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      await Directory(directory).create(recursive: true);
      await File('$directory/$name.png')
          .writeAsBytes(bytes!.buffer.asUint8List());
      final audit = auditScreen(t).map((finding) => '$finding').toList();
      await File('$directory/$name-audit.txt').writeAsString(
        audit.isEmpty ? 'No findings.\n' : '${audit.join('\n')}\n');
    } finally {
      image.dispose();
    }
  });
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    final font = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      font.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await font.load();
    await (FontLoader('MaterialIcons')
          ..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf')))
        .load();
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
  });
  for (final dark in [true, false]) {
    testWidgets('full app aligned login and preserved launcher cards ${dark ? 'dark' : 'light'}', (t) async {
      final semantics = t.ensureSemantics();
      final previousShadows = debugDisableShadows;
      debugDisableShadows = false;
      addTearDown(() => debugDisableShadows = previousShadows);
      t.view.physicalSize = const Size(360, 800);
      t.view.devicePixelRatio = 1;
      t.platformDispatcher.platformBrightnessTestValue =
          dark ? Brightness.dark : Brightness.light;
      addTearDown(t.view.reset);
      addTearDown(t.platformDispatcher.clearPlatformBrightnessTestValue);
      final store = MemoryStore(origin: 'https://vdi.example', username: 'tester');
      store.prefs['launcher.view'] = 'cards';
      store.prefs['stream.quality'] = '{"fps":30,"bitrate":100,"audio":false}';
      store.install();
      final broker = _VisualBroker();
      final boundary = GlobalKey();
      await t.pumpWidget(RepaintBoundary(
        key: boundary,
        child: AndroidVdiApp(apiFactory: (_) => broker),
      ));
      await t.pumpAndSettle();
      final server = t.getRect(find.byKey(const Key('open-server-settings')));
      final id = t.getRect(find.byKey(const Key('sign-in-id')));
      final password = t.getRect(find.byKey(const Key('sign-in-password')));
      expect(server.left, closeTo(id.left, .1));
      expect(server.width, closeTo(id.width, .1));
      expect(password.left, closeTo(id.left, .1));
      expect(password.width, closeTo(id.width, .1));
      expect(server.height, greaterThanOrEqualTo(48));
      await _capture(t, boundary, 'login-360-${dark ? 'dark' : 'light'}');
      await t.tap(find.byKey(const Key('open-server-settings')));
      await t.pumpAndSettle();
      expect(find.byType(AndroidServerDialog), findsOneWidget);
      await _capture(t, boundary, 'server-360-${dark ? 'dark' : 'light'}');
      await t.ensureVisible(find.widgetWithText(TextButton, '취소'));
      await t.tap(find.widgetWithText(TextButton, '취소'));
      await t.pumpAndSettle();
      await t.enterText(find.byKey(const Key('sign-in-password')), 'synthetic-password');
      await t.ensureVisible(find.byKey(const Key('login-submit')));
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      expect(find.byType(DesktopTile), findsNWidgets(2));
      expect(find.byKey(const Key('session-idle-badge')), findsOneWidget);
      final filter = t.getRect(find.byKey(const Key('desktop-status-row')));
      for (final key in ['desktop-search-open', 'desktop-previews']) {
        final target = t.getRect(find.byKey(Key(key)));
        expect(target.center.dy, closeTo(filter.center.dy, .1));
        expect(target.width, greaterThanOrEqualTo(48));
        expect(target.height, greaterThanOrEqualTo(48));
      }
      final status = t.getRect(find.byKey(const Key('desktop-status')));
      expect(status.right, lessThanOrEqualTo(filter.right + .1),
        reason: 'All three actual status segments must fit, including disabled ones.');
      expect(t.getRect(find.byKey(const Key('card-pve-149'))).top,
        lessThanOrEqualTo(220),
        reason: 'Compact chrome frees space above the unchanged card.');
      await _capture(t, boundary, 'launcher-360-${dark ? 'dark' : 'light'}');
      await t.tap(find.byKey(const Key('account-menu')));
      await t.pumpAndSettle();
      await _capture(t, boundary, 'account-360-${dark ? 'dark' : 'light'}');
      await t.sendKeyEvent(LogicalKeyboardKey.escape);
      await t.pumpAndSettle();
      await t.ensureVisible(find.byKey(const Key('stream-modes-pve-149')));
      await t.tap(find.byKey(const Key('stream-modes-pve-149')));
      await t.pumpAndSettle();
      await t.ensureVisible(find.byKey(const Key('stream-settings-pve-149')));
      await t.tap(find.byKey(const Key('stream-settings-pve-149')));
      await t.pumpAndSettle();
      expect(find.byType(AndroidStreamDialog), findsOneWidget);
      expect(t.widget<Text>(find.byKey(const Key('android-c-stream-summary'))).data,
        contains('30 fps · 100 Mbps'));
      expect(find.byKey(const Key('android-c-stream-audio')), findsNothing);
      await _capture(t, boundary, 'connection-360-${dark ? 'dark' : 'light'}');
      await t.ensureVisible(find.byKey(const Key('android-c-stream-advanced')));
      await t.tap(find.byKey(const Key('android-c-stream-advanced')));
      await t.pumpAndSettle();
      expect(t.widget<ChoiceChip>(find.byKey(const Key('fps-30'))).selected, true);
      expect(t.widget<ChoiceChip>(find.byKey(const Key('bitrate-100'))).selected, true);
      await t.ensureVisible(find.byKey(const Key('fps-30')));
      await t.pumpAndSettle();
      // Keep the preceding preset row whole in this readable advanced
      // capture instead of parking its touch target across the scroll edge.
      await t.ensureVisible(find.byKey(const Key('preset-work')));
      await t.pumpAndSettle();
      await _capture(t, boundary, 'connection-advanced-360-${dark ? 'dark' : 'light'}');
      await t.ensureVisible(find.widgetWithText(TextButton, '취소'));
      await t.tap(find.widgetWithText(TextButton, '취소'));
      await t.pumpAndSettle();
      t.view.physicalSize = const Size(915, 412);
      t.platformDispatcher.textScaleFactorTestValue = 1.3;
      addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
      await t.pumpAndSettle();
      expect(t.getRect(find.byKey(const Key('workspace-header'))).height,
        lessThanOrEqualTo(56));
      await _capture(t, boundary, 'launcher-landscape-915-130-${dark ? 'dark' : 'light'}');
      expect(broker.calls.where((call) => call == 'connect' || call.startsWith('power:')), isEmpty);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
      await t.pumpAndSettle();
      semantics.dispose();
      debugDisableShadows = previousShadows;
    });
  }
}

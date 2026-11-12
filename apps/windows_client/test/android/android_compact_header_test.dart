import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_launcher.dart';
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/session_activity.dart';
import 'package:multitap_client/ui/app_theme.dart';

const _account = 'very-long-account-name-that-belongs-in-the-profile-menu';
final _initial = DateTime.utc(2026, 11, 6, 5);

class _Policy {
  DateTime now = _initial;
  int records = 0;
  late final activity = SessionActivity(clock: () => now);

  SessionAccess value(bool enabled) => SessionAccess(
    idleTimeout: const Duration(minutes: 30),
    warning: const Duration(minutes: 1),
    serverTime: now,
    idleEnabled: enabled,
    idleDeadline: enabled ? now.add(const Duration(minutes: 30)) : null,
  );

  void start(bool enabled) => activity.start(
    value(enabled),
    owner: 'synthetic-header-owner',
    record: () async {
      records++;
      return value(enabled);
    },
    onTerminal: (_) {},
    setIdle: (enabled) async => value(enabled),
  );
}

Future<void> _capture(GlobalKey boundary, String name) async {
  final directory = Platform.environment['ANDROID_HEADER_CAPTURE_DIR'];
  if (directory == null) return;
  final image =
      await (boundary.currentContext!.findRenderObject()
              as RenderRepaintBoundary)
          .toImage(pixelRatio: 2);
  try {
    final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
    await Directory(directory).create(recursive: true);
    await File('$directory/$name.png')
        .writeAsBytes(bytes!.buffer.asUint8List());
  } finally {
    image.dispose();
  }
}

void _singleTimerLine(WidgetTester t) {
  final text = find.descendant(
    of: find.byKey(const Key('session-idle-badge')),
    matching: find.byType(Text),
  );
  final paragraph = t.renderObject<RenderParagraph>(text);
  final label = t.widget<Text>(text).data!;
  final boxes = paragraph.getBoxesForSelection(
    TextSelection(baseOffset: 0, extentOffset: label.length),
  );
  expect(boxes, isNotEmpty);
  expect(
    boxes.map((box) => box.top).toSet(),
    hasLength(1),
    reason: 'The real countdown must stay on one visual line: $label',
  );
}

Future<GlobalKey> _show(
  WidgetTester t,
  _Policy policy, {
  Size size = const Size(360, 180),
  double scale = 1,
  bool dark = true,
  bool middle = false,
  bool television = false,
  VoidCallback? onUpdates,
  VoidCallback? onPassword,
  VoidCallback? onSignOut,
}) async {
  t.view.physicalSize = size;
  t.view.devicePixelRatio = 1;
  t.platformDispatcher.textScaleFactorTestValue = scale;
  final boundary = GlobalKey();
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(
        brightness: dark ? Brightness.dark : Brightness.light,
      ),
      home: AndroidTvScope(
        television: television,
        child: RepaintBoundary(
          key: boundary,
          child: Scaffold(
            body: Column(
              children: [
                AndroidWorkspaceHeader(
                  side: 16,
                  columnWidth: 1280,
                  account: _account,
                  server: 'vdi.example',
                  version: '1.0.10',
                  update: '1.0.10-with-a-long-release-name',
                  hostUpdates: 12,
                  sessionAccess: policy.activity,
                  onUpdates: onUpdates ?? () {},
                  onChangePassword: onPassword ?? () {},
                  onSignOut: onSignOut ?? () {},
                  middle: middle ? const Text('컴퓨터', maxLines: 1) : null,
                ),
                const Expanded(child: SizedBox()),
              ],
            ),
          ),
        ),
      ),
    ),
  );
  await t.pumpAndSettle();
  return boundary;
}

void main() {
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
    final binding = TestWidgetsFlutterBinding.instance;
    binding.platformDispatcher.clearTextScaleFactorTestValue();
    binding.platformDispatcher.views.first.resetPhysicalSize();
    binding.platformDispatcher.views.first.resetDevicePixelRatio();
  });

  for (final width in [320.0, 360.0, 412.0]) {
    for (final scale in [1.0, 2.0]) {
      for (final enabled in [true, false]) {
        testWidgets(
          'compact real-policy header $width scale$scale idle$enabled',
          (t) async {
            final policy = _Policy()..start(enabled);
            addTearDown(policy.activity.dispose);
            final boundary = await _show(
              t,
              policy,
              size: Size(width, 180),
              scale: scale,
              dark: enabled,
            );
            final header = t.getRect(find.byKey(const Key('workspace-header')));
            expect(header.height, 56);
            final update = t.getRect(
              find.byKey(const Key('android-header-update-action')),
            );
            final account = t.getRect(find.byKey(const Key('account-menu')));
            final timer = t.getRect(
              find.byKey(const Key('android-header-countdown-slot')),
            );
            for (final rect in [update, account]) {
              expect(rect.width, greaterThanOrEqualTo(48));
              expect(rect.height, greaterThanOrEqualTo(48));
              expect(header.contains(rect.topLeft), isTrue);
              expect(header.contains(rect.bottomRight), isTrue);
            }
            expect(update.center.dy, closeTo(account.center.dy, .01));
            expect(timer.center.dy, closeTo(account.center.dy, .01));
            expect(update.overlaps(timer), isFalse);
            expect(timer.overlaps(account), isFalse);
            expect(find.byKey(const Key('session-idle-badge')), findsOneWidget);
            expect(find.text(enabled ? '30:00' : '꺼짐'), findsOneWidget);
            _singleTimerLine(t);
            expect(find.text('내 계정'), findsNothing);
            expect(find.text('내 작업 공간'), findsNothing);
            expect(find.text('업데이트'), findsNothing);
            expect(
              find.byWidgetPredicate(
                (w) =>
                    w is Tooltip &&
                    w.message ==
                        '새 버전 1.0.10-with-a-long-release-name · Host 업데이트 12대',
              ),
              findsOneWidget,
            );
            expect(policy.records, 0);
            expect(t.takeException(), isNull);
            if (width == 360 && scale == 1) {
              await t.runAsync(
                () => _capture(
                  boundary,
                  enabled ? 'header-360-dark-idle' : 'header-360-light-off',
                ),
              );
            }
            await t.pumpWidget(const SizedBox.shrink());
            policy.activity.stop();
          },
        );
      }
    }
  }

  testWidgets(
    'countdown ticks without moving controls or recording human activity',
    (t) async {
      final policy = _Policy()..start(true);
      addTearDown(policy.activity.dispose);
      await _show(t, policy);
      final before = t.getRect(find.byKey(const Key('account-menu')));
      final timerBefore = t.getRect(
        find.byKey(const Key('android-header-countdown-slot')),
      );
      policy.now = policy.now.add(const Duration(seconds: 1));
      await t.pump(const Duration(seconds: 1));
      expect(find.text('29:59'), findsOneWidget);
      _singleTimerLine(t);
      expect(t.getRect(find.byKey(const Key('account-menu'))), before);
      expect(
        t.getRect(find.byKey(const Key('android-header-countdown-slot'))),
        timerBefore,
      );
      expect(policy.records, 0);
      policy.now = _initial.add(const Duration(minutes: 29, seconds: 1));
      await t.pump(const Duration(seconds: 1));
      expect(find.text('0:59'), findsOneWidget);
      _singleTimerLine(t);
      expect(policy.records, 0);
      await t.pumpWidget(const SizedBox.shrink());
      policy.activity.stop();
    },
  );

  testWidgets(
    'unknown policy does not invent a countdown and manual actions remain available',
    (t) async {
      final policy = _Policy();
      addTearDown(policy.activity.dispose);
      var updates = 0, password = 0, signOut = 0;
      await _show(
        t,
        policy,
        size: const Size(412, 915),
        onUpdates: () => updates++,
        onPassword: () => password++,
        onSignOut: () => signOut++,
      );
      expect(find.byKey(const Key('session-idle-badge')), findsNothing);
      await t.tap(find.byKey(const Key('android-header-update-action')));
      expect(updates, 1);
      await t.tap(find.byKey(const Key('account-menu')));
      await t.pumpAndSettle();
      expect(find.text(_account), findsOneWidget);
      await t.sendKeyEvent(LogicalKeyboardKey.escape);
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('open-app-settings')));
      await t.pumpAndSettle();
      await t.ensureVisible(find.byKey(const Key('account-change-password')));
      await t.tap(find.byKey(const Key('account-change-password')));
      await t.pumpAndSettle();
      expect(password, 1);
      expect(signOut, 0);
      await t.tap(find.byKey(const Key('account-menu')));
      await t.pumpAndSettle();
      await t.ensureVisible(find.byKey(const Key('sign-out')));
      await t.tap(find.byKey(const Key('sign-out')));
      await t.pumpAndSettle();
      expect(signOut, 1);
      await t.pumpWidget(const SizedBox.shrink());
      policy.activity.stop();
    },
  );

  for (final size in [const Size(800, 360), const Size(960, 540)]) {
    testWidgets(
      'single-row landscape/TV header retains middle controls at200% $size',
      (t) async {
        final policy = _Policy()..start(true);
        addTearDown(policy.activity.dispose);
        final boundary = await _show(
          t,
          policy,
          size: size,
          scale: 2,
          middle: true,
          television: size.width == 960,
        );
        expect(t.getSize(find.byKey(const Key('workspace-header'))).height, 56);
        expect(find.text('컴퓨터'), findsOneWidget);
        expect(find.byKey(const Key('session-idle-badge')), findsOneWidget);
        _singleTimerLine(t);
        expect(t.takeException(), isNull);
        if (size.width == 800) {
          await t.runAsync(
            () => _capture(boundary, 'header-landscape-800-200'),
          );
        }
        await t.pumpWidget(const SizedBox.shrink());
        policy.activity.stop();
      },
    );
  }
}

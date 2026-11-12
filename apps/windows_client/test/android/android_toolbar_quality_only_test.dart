// Actual production panels with fake callbacks; no broker, device or stored data.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_store.dart'
    show AndroidStreamQuality;
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';

import 'android_launcher_test.dart' show vm;

const _captureRoot = String.fromEnvironment('MULTITAP_ANDROID110_CAPTURE');
const _captureKey = Key('android-quality-only-render');

Future<void> _capture(WidgetTester tester, String name) async {
  if (_captureRoot.isEmpty) return;
  await tester.runAsync(() async {
    final boundary = tester.renderObject<RenderRepaintBoundary>(
      find.byKey(_captureKey),
    );
    final image = await boundary.toImage(pixelRatio: 2);
    try {
      final data = await image.toByteData(format: ui.ImageByteFormat.png);
      await Directory(_captureRoot).create(recursive: true);
      await File('$_captureRoot/$name.png')
          .writeAsBytes(data!.buffer.asUint8List());
    } finally {
      image.dispose();
    }
  });
}

Future<void> _panel(
  WidgetTester tester, {
  required bool audio,
  required List<StreamOptions> applied,
  required VoidCallback cancel,
  String identity = 'quality-only',
}) async {
  tester.view.physicalSize = const Size(412, 915);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: RepaintBoundary(
        key: _captureKey,
        child: Scaffold(
          body: AndroidStreamSettingsPanel(
            key: ValueKey(identity),
            options: StreamOptions(fps: 30, bitrate: 40000000, audio: audio),
            maxBitrate: 100000000,
            onApply: applied.add,
            onCancel: cancel,
          ),
        ),
      ),
    ),
  );
  await tester.pumpAndSettle();
}

Future<void> _tap(WidgetTester tester, String key) async {
  final target = find.byKey(Key(key));
  await tester.ensureVisible(target);
  await tester.pumpAndSettle();
  await tester.tap(target);
  await tester.pumpAndSettle();
}

void main() {
  if (_captureRoot.isNotEmpty) {
    setUpAll(() async {
      await (FontLoader('Pretendard')
            ..addFont(rootBundle.load('assets/fonts/Pretendard-Regular.otf'))
            ..addFont(rootBundle.load('assets/fonts/Pretendard-SemiBold.otf')))
          .load();
    });
  }
  for (final audio in [true, false]) {
    // Sound is always on (2026-11-11): a reconnect asks for it even from a
    // connection without it.
    testWidgets('toolbar quality edits ask for sound (connection audio=$audio)', (
      tester,
    ) async {
      final applied = <StreamOptions>[];
      var cancelled = 0;
      await _panel(
        tester,
        audio: audio,
        applied: applied,
        cancel: () => cancelled++,
      );
      expect(find.text('오디오'), findsNothing);
      expect(find.byKey(const Key('stream-audio-on')), findsNothing);
      expect(find.byKey(const Key('stream-audio-off')), findsNothing);
      expect(find.byKey(const Key('android-session-sound-card')), findsNothing);
      await _capture(tester, 'toolbar-quality-audio-$audio');
      await _tap(tester, 'apply-stream-settings');
      expect(applied, isEmpty, reason: 'unchanged reconnect remains disabled');

      await _tap(tester, 'stream-fps-60');
      await _tap(tester, 'stream-bitrate-100');
      expect(
        applied,
        isEmpty,
        reason: 'editing is a draft, never auto-reconnect',
      );
      await _tap(tester, 'apply-stream-settings');
      expect(applied, hasLength(1));
      expect(applied.single.fps, 60);
      expect(applied.single.bitrate, 100000000);
      expect(applied.single.audio, isTrue);
      await _tap(tester, 'cancel-stream-settings');
      expect(cancelled, 1);
      expect(applied, hasLength(1));
      expect(tester.takeException(), isNull);
    });

    for (final preset in StreamPreset.values) {
      testWidgets('preset ${preset.name} asks for sound (connection audio=$audio)', (
        tester,
      ) async {
        final applied = <StreamOptions>[];
        var cancelled = 0;
        await _panel(
          tester,
          identity: '${preset.name}-$audio',
          audio: audio,
          applied: applied,
          cancel: () => cancelled++,
        );
        await _tap(tester, 'stream-preset-${preset.name}');
        expect(applied, isEmpty);
        await _tap(tester, 'apply-stream-settings');
        expect(applied, hasLength(1));
        expect(applied.single.audio, isTrue);
        expect(applied.single.fps, preset.options.fps);
        expect(
          applied.single.automaticBitrate,
          preset.options.automaticBitrate,
        );
        expect(cancelled, 0);
        expect(tester.takeException(), isNull);
      });
    }

    testWidgets('cancel quality draft never reconnects with audio=$audio', (
      tester,
    ) async {
      final applied = <StreamOptions>[];
      var cancelled = 0;
      await _panel(
        tester,
        audio: audio,
        applied: applied,
        cancel: () => cancelled++,
      );
      await _tap(tester, 'stream-fps-60');
      await _tap(tester, 'stream-bitrate-100');
      await _tap(tester, 'cancel-stream-settings');
      expect(cancelled, 1);
      expect(applied, isEmpty);
      expect(tester.takeException(), isNull);
    });
  }

  // 2026-11-10: Multitap adds no sound device. Where the Host reports none,
  // the panel says one plain line and offers nothing to press for it.
  for (final (name, size) in [
    ('upright', const Size(412, 915)),
    ('on its side', const Size(915, 412)),
  ]) {
    testWidgets('no sound device: one plain line in 화질, $name', (
      tester,
    ) async {
      tester.view.physicalSize = size;
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      final applied = <StreamOptions>[];
      Widget panel({required bool missing}) => MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: RepaintBoundary(
          key: _captureKey,
          child: Scaffold(
            body: AndroidStreamSettingsPanel(
              options: const StreamOptions(fps: 30, bitrate: 40000000),
              maxBitrate: 100000000,
              audioDeviceMissing: missing,
              onApply: applied.add,
              onCancel: () {},
            ),
          ),
        ),
      );
      await tester.pumpWidget(panel(missing: false));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('stream-audio-hint')), findsNothing);

      await tester.pumpWidget(panel(missing: true));
      await tester.pumpAndSettle();
      final hint = find.byKey(const Key('stream-audio-hint'));
      expect(hint, findsOneWidget);
      expect(
        tester.widget<Text>(hint).data,
        '이 데스크톱에는 소리 장치가 없어요. 관리자가 Proxmox에서 오디오 장치를 추가해야 해요.',
      );
      expect(tester.widget<Text>(hint).data, audioDeviceMissingText);
      // Whole, inside the panel, and nothing to press for it.
      await tester.ensureVisible(hint);
      await tester.pumpAndSettle();
      final screen = Offset.zero & size;
      final box = tester.getRect(hint);
      expect(screen.contains(box.topLeft), isTrue, reason: '$box');
      expect(screen.contains(box.bottomRight), isTrue, reason: '$box');
      expect(
        tester
            .renderObject<RenderParagraph>(
              find.descendant(of: hint, matching: find.byType(RichText)),
            )
            .didExceedMaxLines,
        isFalse,
      );
      expect(find.textContaining('사운드카드'), findsNothing);
      expect(find.textContaining('추가…'), findsNothing);
      expect(find.byKey(const Key('stream-add-sound-card')), findsNothing);
      expect(find.byKey(const Key('android-session-sound-card')), findsNothing);
      expect(find.byKey(const Key('stream-audio-on')), findsNothing);
      await _capture(tester, 'toolbar-quality-no-sound-device-$name');
      // The line changes no choice: 다시 연결 still needs one.
      await _tap(tester, 'apply-stream-settings');
      expect(applied, isEmpty);
      await _tap(tester, 'stream-fps-60');
      await _tap(tester, 'apply-stream-settings');
      expect(applied.single.fps, 60);
      expect(tester.takeException(), isNull);
    });
  }

  testWidgets('new connections use audio automatically without an option', (
    tester,
  ) async {
    StreamOptions? result;
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => Scaffold(
            body: TextButton(
              onPressed: () async {
                result = await showDialog<StreamOptions>(
                  context: context,
                  builder: (_) => AndroidStreamDialog(
                    desktop: vm(),
                    initial: const AndroidStreamQuality(
                      fps: 30,
                      bitrate: 100,
                    ),
                  ),
                );
              },
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-c-stream-audio')), findsNothing);
    await tester.tap(find.widgetWithText(FilledButton, '연결'));
    await tester.pumpAndSettle();
    expect(result?.audio, isTrue);
    expect(result?.fps, 30);
    expect(result?.bitrate, 100000000);
    expect(tester.takeException(), isNull);
  });
}

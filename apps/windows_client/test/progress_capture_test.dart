// Pictures of Host settings while a long request runs, for a look at the
// progress panel without installing anything: the real HostSettingsScreen on
// the in-process fixture (no Host tool, driver, installer or Windows setting).
// Widget renders, not device screenshots. Skipped unless asked for:
//
//   flutter test --no-pub test/progress_capture_test.dart
//     --dart-define=CAPTURE_UI=true --dart-define=CAPTURE_DIR=<folder>
//
// Files: <dir>/host-settings-<scene>-<ko|en>.png
import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/l10n/l10n.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

import 'helpers/pump_work.dart';
import 'host_settings_progress_test.dart' show TellingHost;
import 'host_settings_test.dart' show FakeCloseGuard, FakeHostSettings;

const _dir = String.fromEnvironment('CAPTURE_DIR');

Future<void> _fonts(WidgetTester t) => t.runAsync(() async {
  final windows = Platform.environment['WINDIR'] ?? 'C:/Windows';
  final root =
      Platform.environment['FLUTTER_ROOT'] ??
      '${Directory.current.path}/../../.tools/flutter';
  final families = {
    'Pretendard': [
      for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold'])
        'assets/fonts/Pretendard-$weight.otf',
    ],
    'MaterialIcons': [
      '$root/bin/cache/artifacts/material_fonts/MaterialIcons-Regular.otf',
    ],
    'Malgun Gothic': ['$windows/Fonts/malgun.ttf', '$windows/Fonts/malgunbd.ttf'],
    'Segoe UI': ['$windows/Fonts/segoeui.ttf', '$windows/Fonts/seguisb.ttf'],
    'Consolas': ['$windows/Fonts/consola.ttf'],
  };
  for (final family in families.entries) {
    final files = [
      for (final path in family.value)
        if (File(path).existsSync()) path,
    ];
    if (files.isEmpty) continue;
    final loader = FontLoader(family.key);
    for (final path in files) {
      loader.addFont(File(path).readAsBytes().then(ByteData.sublistView));
    }
    await loader.load();
  }
});

void main() {
  final boundary = GlobalKey();

  Future<void> capture(WidgetTester t, String name) async {
    await t.pump(const Duration(milliseconds: 400));
    await t.runAsync(() async {
      final image =
          await (boundary.currentContext!.findRenderObject()!
                  as RenderRepaintBoundary)
              .toImage(pixelRatio: 1);
      final data = await image.toByteData(format: ui.ImageByteFormat.png);
      final file = File('$_dir/$name.png');
      await file.parent.create(recursive: true);
      await file.writeAsBytes(data!.buffer.asUint8List());
      image.dispose();
    });
  }

  Future<void> open(
    WidgetTester t,
    FakeHostSettings api, {
    Brightness brightness = Brightness.dark,
  }) async {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = const Size(1120, 760);
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    await t.pumpWidget(
      RepaintBoundary(
        key: boundary,
        child: MaterialApp(
          key: UniqueKey(),
          debugShowCheckedModeBanner: false,
          theme: buildAppTheme(brightness: brightness),
          home: HostSettingsScreen(api: api, closeGuard: FakeCloseGuard()),
        ),
      ),
    );
    await pumpWork(t);
  }

  for (final language in ['ko', 'en']) {
    testWidgets(
      'Host settings progress pictures ($language)',
      (t) async {
        AppLanguageController.debugSystemLanguage = language;
        addTearDown(() => AppLanguageController.debugSystemLanguage = 'ko');
        await _fonts(t);

        // The finish of a first install: the driver, then WinFsp with
        // Windows Installer's own count, then the check afterwards.
        final fresh = TellingHost(registered: true)..settleReads = 4;
        await open(t, fresh);
        await capture(t, 'host-settings-1-finish-question-$language');
        fresh.nativePending = Completer<void>();
        fresh.devicePending = Completer<void>();
        await t.tap(find.byKey(const Key('host-confirm')));
        await t.pump();
        await t.pump();
        fresh.tell('installNativeDisplay', 'display_device');
        await t.pump();
        await capture(t, 'host-settings-2-driver-$language');
        fresh.nativePending!.complete();
        await t.pump();
        await t.pump();
        await t.pump();
        fresh.tell('installWinfsp', 'winfsp_install', done: 1320, total: 3100);
        await t.pump();
        await capture(t, 'host-settings-3-winfsp-$language');
        fresh.devicePending!.complete();
        await t.pump();
        await t.pump();
        await t.pump(const Duration(seconds: 1));
        await capture(t, 'host-settings-4-check-$language');
        await pumpWork(t, limit: const Duration(seconds: 30));
        await capture(t, 'host-settings-5-result-$language');

        // Waiting for the administrator's approval.
        final waiting = FakeHostSettings()
          ..connectionRequest = {
            'status': 'pending',
            'origin': 'https://cloud.example.com',
            'requestId': 'fixture-pending',
            'verificationCode': '49J2-HQP1',
            'expiresAt': DateTime.now()
                .add(const Duration(minutes: 10))
                .toIso8601String(),
          };
        await open(t, waiting, brightness: Brightness.light);
        final request = find.byKey(const Key('host-open-request'));
        await Scrollable.ensureVisible(t.element(request), alignment: .5);
        await pumpWork(t);
        await t.tap(request);
        await pumpWork(t);
        await capture(t, 'host-settings-6-approval-$language');
        expect(t.takeException(), isNull);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.windows),
      skip: !const bool.fromEnvironment('CAPTURE_UI') || _dir.isEmpty,
      timeout: const Timeout(Duration(minutes: 10)),
    );
  }
}

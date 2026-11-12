// Pictures for the product film (.artifacts/promo): the real Windows client
// widgets with invented data, at film size. Widget renders, not device
// screenshots; the remote desktop and the card previews are the drawn
// stand-ins of site_capture_test.dart.
//
//   flutter test --no-pub test/promo_capture_test.dart
//     --dart-define=CAPTURE_UI=true --dart-define=CAPTURE_DIR=<fresh folder>
//
// Files: <dir>/<scene>.png (Korean).
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart' show PointerDeviceKind;
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/desktop_previews.dart';
import 'package:multitap_client/data/desktop_thumbnails.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/l10n/l10n.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/desktop_use.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/virtual_display.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/themed_app.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'helpers/session_bar_fixture.dart' show ToolbarV2Bar, ToolbarV2Share;
import 'site_capture_test.dart' show DesktopPainter;

const _dir = String.fromEnvironment('CAPTURE_DIR');
const _epoch = '5173c0de5173c0de5173c0de5173c0de';
const _gb = 1024 * 1024 * 1024;

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
    'Segoe UI': [
      '$windows/Fonts/segoeui.ttf',
      '$windows/Fonts/seguisb.ttf',
      '$windows/Fonts/segoeuib.ttf',
    ],
    'Segoe UI Symbol': ['$windows/Fonts/seguisym.ttf'],
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

Future<Uint8List> _picture(int variant) async {
  const size = Size(640, 360);
  final recorder = ui.PictureRecorder();
  DesktopPainter(variant).paint(Canvas(recorder), size);
  final image = await recorder.endRecording().toImage(640, 360);
  final data = await image.toByteData(format: ui.ImageByteFormat.png);
  image.dispose();
  return data!.buffer.asUint8List();
}

class _Previews implements DesktopPreviewSource {
  _Previews(this.pictures);
  final Map<String, Uint8List> pictures;
  @override
  bool offered(String desktopId) => pictures.containsKey(desktopId);
  @override
  Future<DesktopPreview> read(String desktopId) async {
    final bytes = pictures[desktopId];
    if (bytes == null) return const DesktopPreview.unavailable();
    return DesktopPreview(
      DesktopPreviewState.captured,
      picture: DesktopThumbnail(
        image: MemoryImage(bytes),
        modified: DateTime.now().subtract(const Duration(minutes: 2)),
        remote: true,
      ),
      epoch: _epoch,
    );
  }
}

List<DesktopMachine> _machines() {
  DesktopMachine running(
    String id,
    String name, {
    double cpu = 20,
    int vcpus = 4,
    int memory = 8,
    double used = .5,
    Color accent = Colors.blue,
    DesktopUse use = DesktopUse.idle,
    String? account,
  }) => DesktopMachine(
    id: id,
    name: name,
    description: '',
    kind: MachineKind.vm,
    status: MachineStatus.ready,
    specification: 'Windows 11 Pro',
    accent: accent,
    powerState: 'running',
    powerStale: false,
    powerCanForceStop: true,
    hostOnline: true,
    interactiveSession: true,
    virtualDisplayReady: true,
    virtualMonitorControl: true,
    display: VirtualDisplayState(
      supported: true,
      activeCount: 1,
      checkedAt: DateTime.now(),
      stale: false,
    ),
    monitors: const [
      RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
    ],
    thumbnailPrivacy: const ThumbnailPrivacy(epoch: _epoch, allowed: true),
    use: use,
    useAccount: account,
    resources: VmResources(
      cpuUsagePercent: cpu,
      vcpus: vcpus,
      memoryUsedBytes: (memory * _gb * used).round(),
      memoryTotalBytes: memory * _gb,
      uptimeSeconds: 3 * 3600 + 1200,
      stale: false,
      checkedAt: DateTime.now(),
    ),
  );
  return [
    running('pve-101', '디자인 1', cpu: 23, used: .64),
    running(
      'pve-104',
      '개발 1',
      cpu: 41,
      vcpus: 8,
      memory: 16,
      used: .48,
      accent: Colors.indigo,
      use: DesktopUse.other,
      account: 'bob',
    ),
    running(
      'pve-109',
      '영상 편집',
      cpu: 67,
      vcpus: 8,
      memory: 32,
      used: .71,
      accent: Colors.deepOrange,
    ),
    const DesktopMachine(
      id: 'pve-111',
      name: '회계 1',
      description: '',
      kind: MachineKind.vm,
      status: MachineStatus.poweredOff,
      specification: 'Windows 11 Pro',
      accent: Colors.teal,
      powerState: 'stopped',
      powerStale: false,
      hostOnline: false,
      resources: VmResources(vcpus: 4, memoryTotalBytes: 8 * _gb),
    ),
    running('pve-130', '테스트', cpu: 4, vcpus: 2, memory: 4, used: .42),
    running(
      'pve-140',
      '집 게이밍 PC',
      cpu: 12,
      vcpus: 8,
      memory: 32,
      used: .3,
      accent: Colors.purple,
    ),
  ];
}

void main() {
  final boundary = GlobalKey();
  final faults = <String>[];

  Future<void> shot(WidgetTester t, String name) async {
    final fault = t.takeException();
    if (fault != null) faults.add('$name: $fault');
    await t.runAsync(() async {
      final image =
          await (boundary.currentContext!.findRenderObject()!
                  as RenderRepaintBoundary)
              .toImage(pixelRatio: 2);
      final data = await image.toByteData(format: ui.ImageByteFormat.png);
      final file = File('$_dir/$name.png');
      await file.parent.create(recursive: true);
      await file.writeAsBytes(data!.buffer.asUint8List());
      image.dispose();
    });
  }

  void size(WidgetTester t, Size value) {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = value;
    t.view.padding = FakeViewPadding.zero;
    t.view.viewPadding = FakeViewPadding.zero;
  }

  /// Taps [key] when it is there; a missing key is noted, not fatal.
  Future<bool> tap(WidgetTester t, String key) async {
    final target = find.byKey(Key(key));
    if (target.evaluate().isEmpty) {
      faults.add('missing $key');
      return false;
    }
    await t.ensureVisible(target.first);
    await t.pumpAndSettle();
    await t.tap(target.first);
    await t.pumpAndSettle();
    return true;
  }

  Future<void> close(WidgetTester t) async {
    await t.sendKeyEvent(LogicalKeyboardKey.escape);
    await t.pumpAndSettle();
  }

  testWidgets(
    'Film: the Windows client',
    (t) async {
      if (_dir.isEmpty) throw StateError('CAPTURE_DIR is required');
      await _fonts(t);
      debugDisableShadows = false;
      addTearDown(t.view.reset);
      AppLanguageController.debugSystemLanguage = 'ko';
      AppLanguageController.instance.debugReset(AppLanguage.ko);
      final pictures = await t.runAsync(
        () async => {
          'pve-101': await _picture(0),
          'pve-104': await _picture(1),
          'pve-109': await _picture(2),
          'pve-130': await _picture(3),
          'pve-140': await _picture(4),
        },
      );

      Future<void> launcher(Size at) async {
        size(t, at);
        await t.pumpWidget(
          RepaintBoundary(
            key: boundary,
            child: ThemedApp(
              key: UniqueKey(),
              appearance: AppAppearance(),
              home: WorkspaceShell(
                onSignOut: () {},
                onRefresh: () {},
                displayName: 'alice',
                serverOrigin: 'http://10.0.0.10:18480',
                machines: _machines(),
                localMonitors: 2,
                launcherMemory: SessionLauncherMemory(),
                previews: _Previews(pictures!),
                onConnect: (_, {options = const StreamOptions()}) async =>
                    throw StateError('render fixture does not connect'),
                onPower: (_, _) async {},
              ),
            ),
          ),
        );
        await t.pumpAndSettle();
        await t.runAsync(() async {
          final context = boundary.currentContext!;
          await precacheImage(
            const AssetImage('assets/backgrounds/alpine-dusk.png'),
            context,
          );
          for (final bytes in pictures.values) {
            await precacheImage(MemoryImage(bytes), context);
          }
        });
        for (var i = 0; i < 4; i++) {
          await t.pump(const Duration(milliseconds: 200));
        }
        await t.pumpAndSettle();
      }

      await launcher(const Size(1440, 810));
      await shot(t, 'launcher');

      // The powered-off card: the pointer over its power mark.
      final power = find.byKey(const Key('power-on-pve-111'));
      if (power.evaluate().isNotEmpty) {
        final mouse = await t.createGesture(kind: PointerDeviceKind.mouse);
        await mouse.addPointer(location: Offset.zero);
        addTearDown(mouse.removePointer);
        await mouse.moveTo(t.getCenter(power.first));
        await t.pumpAndSettle();
        await shot(t, 'launcher-power-hover');
        await mouse.moveTo(const Offset(4, 4));
        await t.pumpAndSettle();
      } else {
        faults.add('missing power-on-pve-111');
      }

      // The card's power menu and its 메뉴.
      if (await tap(t, 'desktop-power-pve-101')) {
        await shot(t, 'launcher-power-menu');
        await close(t);
      }
      if (await tap(t, 'desktop-more-pve-101')) {
        await shot(t, 'launcher-more-menu');
        await close(t);
      }

      // 연결: the confirmation before any request.
      if (await tap(t, 'play-pve-101')) {
        await shot(t, 'launcher-confirm');
        await close(t);
      }
      Future<bool> tapIn(String card, String text, String name) async {
        final target = find.descendant(
          of: find.byKey(Key(card)),
          matching: find.text(text),
        );
        if (target.evaluate().isEmpty) {
          faults.add('missing $card/$text');
          return false;
        }
        await t.ensureVisible(target.first);
        await t.pumpAndSettle();
        await t.drag(find.byType(Scrollable).first, const Offset(0, -120));
        await t.pumpAndSettle();
        await t.tap(target.first, warnIfMissed: false);
        await t.pumpAndSettle();
        await shot(t, name);
        return true;
      }

      // The desktop bob is using.
      await launcher(const Size(1440, 810));
      if (await tapIn('card-pve-104', '스트리밍 연결', 'launcher-confirm-other')) {
        await close(t);
      }

      // The session bar over the desktop at film size.
      final share = await ToolbarV2Share.start(t);
      Future<void> bar({Size at = const Size(1440, 810), int variant = 0, bool lock = false}) async {
        size(t, at);
        await t.pumpWidget(
          RepaintBoundary(
            key: boundary,
            child: MaterialApp(
              key: UniqueKey(),
              debugShowCheckedModeBanner: false,
              theme: buildAppTheme(),
              home: Scaffold(
                body: Stack(
                  fit: StackFit.expand,
                  children: [
                    CustomPaint(painter: DesktopPainter(variant)),
                    ToolbarV2Bar(
                      title: '디자인 1',
                      latencyMs: 12,
                      share: share,
                      gameMouse: lock,
                      modes: const [
                        SessionDisplayMode.windowed,
                        SessionDisplayMode.fullscreen,
                        SessionDisplayMode.allMonitors,
                      ],
                    ),
                  ],
                ),
              ),
            ),
          ),
        );
        await t.pump();
        await t.pumpAndSettle();
      }

      await bar();
      await shot(t, 'session-plain');
      await tap(t, 'expand-session-toolbar');
      await shot(t, 'session-bar');
      if (await tap(t, 'session-quality')) {
        await shot(t, 'session-quality');
        await close(t);
      }
      if (await tap(t, 'stream-menu-화면')) {
        await shot(t, 'session-screen');
        await close(t);
      }
      if (await tap(t, 'remote-share')) {
        await shot(t, 'session-share');
        await close(t);
      }
      if (await tap(t, 'remote-settings')) {
        await shot(t, 'session-settings');
        await close(t);
      }
      // 마우스 잠금 on.
      await bar(lock: true);
      await tap(t, 'expand-session-toolbar');
      if (await tap(t, 'remote-settings')) {
        await shot(t, 'session-settings-on');
        await close(t);
      }
      // The same desktop in a smaller and a wider window.
      for (final (name, at) in [
        ('session-small', const Size(1024, 768)),
        ('session-wide', const Size(1920, 810)),
      ]) {
        await bar(at: at);
        await shot(t, name);
      }
      await t.pumpWidget(const SizedBox.shrink());
      await t.pumpAndSettle();
      await share.close(t);
      debugDisableShadows = true;
      AppLanguageController.debugSystemLanguage = null;
      AppLanguageController.instance.debugReset();
      // Missing keys are reported, not fatal: the film uses what exists.
      if (faults.isNotEmpty) {
        File('$_dir/faults.txt').writeAsStringSync(faults.join('\n'));
      }
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
    skip: !const bool.fromEnvironment('CAPTURE_UI'),
    timeout: const Timeout(Duration(minutes: 20)),
  );
}

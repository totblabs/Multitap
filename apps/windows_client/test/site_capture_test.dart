// Pictures for the website gallery (multitap.me /ko/ and /en/): the real
// Windows client, Android client and Host settings widgets, each scene in
// Korean and in English, with invented data only. Widget renders, not device
// screenshots: the remote desktop and the card previews are drawn stand-ins
// (a real stream is a decoder texture a widget render cannot show).
//
//   flutter test --no-pub test/site_capture_test.dart
//     --dart-define=CAPTURE_UI=true --dart-define=CAPTURE_DIR=<fresh folder>
//
// Text renders with the fonts the app ships (Pretendard) and Flutter's
// Material Icons, both loaded before the first frame; the theme's Windows
// fallbacks (Malgun Gothic, Segoe UI, Segoe UI Symbol, Consolas) are loaded
// from %WINDIR%\Fonts when present. A family that is not loaded draws boxes.
//
// Files: <dir>/<lang>/<scene>.png.
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_launcher.dart'
    show androidTheme;
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_previews.dart';
import 'package:multitap_client/data/desktop_thumbnails.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/l10n/l10n.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';
import 'package:multitap_client/ui/themed_app.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'android/android_launcher_test.dart' show FakeBroker, MemoryStore;
import 'android/android_session_contract_test.dart' show AndroidNativeFake;
import 'android/android_updates_test.dart' show FakePlatform, catalog;
import 'auth_flow_test.dart' show TestBroker;
import 'host_settings_test.dart' show FakeCloseGuard, FakeHostSettings;
import 'remote_session_test.dart' show SessionSocket;
import 'helpers/session_bar_fixture.dart' show ToolbarV2Bar;

const _dir = String.fromEnvironment('CAPTURE_DIR');
const _epoch = '5173c0de5173c0de5173c0de5173c0de';
final _origin = Uri.parse('http://10.0.0.10:18480');

/// One scene's words in both languages.
String _w(String lang, String ko, String en) => lang == 'ko' ? ko : en;

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
    'Malgun Gothic': [
      '$windows/Fonts/malgun.ttf',
      '$windows/Fonts/malgunbd.ttf',
    ],
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
    if (files.isEmpty) {
      if (family.key == 'Pretendard' || family.key == 'MaterialIcons') {
        throw StateError('missing font ${family.value}');
      }
      continue;
    }
    final loader = FontLoader(family.key);
    for (final path in files) {
      loader.addFont(File(path).readAsBytes().then(ByteData.sublistView));
    }
    await loader.load();
  }
});

/// A stand-in Windows desktop: wallpaper, a window or two and the taskbar.
/// [variant] picks the wallpaper and the windows; no text is drawn.
class DesktopPainter extends CustomPainter {
  const DesktopPainter(this.variant);
  final int variant;

  static const _walls = [
    [Color(0xFF8EC5FF), Color(0xFF2F6FD6), Color(0xFF0C2557)],
    [Color(0xFF9BE3D3), Color(0xFF1F8A82), Color(0xFF0A3138)],
    [Color(0xFFF7C98B), Color(0xFFC9693A), Color(0xFF3A1730)],
    [Color(0xFFC6B8FF), Color(0xFF5B4BC4), Color(0xFF1A1442)],
    [Color(0xFFB8E28A), Color(0xFF3E8E4A), Color(0xFF10301C)],
  ];

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height, u = h / 60;
    final colors = _walls[variant % _walls.length];
    canvas.drawRect(
      Offset.zero & size,
      Paint()
        ..shader = RadialGradient(
          center: Alignment(variant.isEven ? .25 : -.3, .15),
          radius: 1.15,
          colors: colors,
        ).createShader(Offset.zero & size),
    );
    // A soft ribbon, as Windows wallpapers have.
    final ribbon = Path()
      ..moveTo(0, h * .72)
      ..cubicTo(
        w * .3,
        h * (.5 + variant % 3 * .05),
        w * .6,
        h * .95,
        w,
        h * .62,
      )
      ..lineTo(w, h)
      ..lineTo(0, h)
      ..close();
    canvas.drawPath(
      ribbon,
      Paint()..color = Colors.white.withValues(alpha: .08),
    );

    void window(Rect r, {bool dark = false, int kind = 0}) {
      final radius = Radius.circular(u * .7);
      canvas.drawRRect(
        RRect.fromRectAndRadius(r.shift(Offset(0, u * .4)), radius),
        Paint()
          ..color = Colors.black.withValues(alpha: .28)
          ..maskFilter = MaskFilter.blur(BlurStyle.normal, u * 1.2),
      );
      final body = dark ? const Color(0xFF202124) : const Color(0xFFF7F7F8);
      final bar = dark ? const Color(0xFF2B2C30) : const Color(0xFFE9EAEC);
      final ink = dark ? const Color(0xFF8A8D94) : const Color(0xFFB4B8BE);
      final strong = dark ? const Color(0xFFD5D7DB) : const Color(0xFF3D4148);
      canvas.drawRRect(
        RRect.fromRectAndRadius(r, radius),
        Paint()..color = body,
      );
      final title = Rect.fromLTWH(r.left, r.top, r.width, u * 2.6);
      canvas.drawRRect(
        RRect.fromRectAndCorners(title, topLeft: radius, topRight: radius),
        Paint()..color = bar,
      );
      for (var i = 0; i < 3; i++) {
        canvas.drawCircle(
          Offset(r.right - u * (1.4 + i * 1.8), title.center.dy),
          u * .35,
          Paint()..color = ink,
        );
      }
      final inner = Rect.fromLTRB(
        r.left + u * 1.4,
        title.bottom + u * 1.2,
        r.right - u * 1.4,
        r.bottom - u * 1.2,
      );
      final line = Paint();
      switch (kind) {
        case 0: // a document
          for (var i = 0; i * u * 1.5 < inner.height - u; i++) {
            line.color = i == 0 ? strong : ink;
            final width = i == 0
                ? inner.width * .45
                : inner.width * (.62 + ((i * 37 + variant * 11) % 30) / 100);
            canvas.drawRRect(
              RRect.fromRectAndRadius(
                Rect.fromLTWH(
                  inner.left,
                  inner.top + i * u * 1.5,
                  math.min(width, inner.width),
                  u * (i == 0 ? .9 : .55),
                ),
                Radius.circular(u * .3),
              ),
              line,
            );
          }
        case 1: // a chart and a table
          final chart = Rect.fromLTWH(
            inner.left,
            inner.top,
            inner.width,
            inner.height * .55,
          );
          for (var i = 0; i < 9; i++) {
            final bh =
                chart.height * (.25 + ((i * 53 + variant * 17) % 70) / 100);
            line.color = i.isEven
                ? colors[1]
                : colors[1].withValues(alpha: .55);
            canvas.drawRRect(
              RRect.fromRectAndRadius(
                Rect.fromLTWH(
                  chart.left + i * chart.width / 9 + u * .3,
                  chart.bottom - bh,
                  chart.width / 9 - u * .6,
                  bh,
                ),
                Radius.circular(u * .25),
              ),
              line,
            );
          }
          for (var i = 0; i < 4; i++) {
            line.color = ink;
            canvas.drawRRect(
              RRect.fromRectAndRadius(
                Rect.fromLTWH(
                  inner.left,
                  chart.bottom + u * (1.4 + i * 1.5),
                  inner.width * (.5 + i % 2 * .3),
                  u * .55,
                ),
                Radius.circular(u * .3),
              ),
              line,
            );
          }
        default: // a picture editor
          final picture = Rect.fromLTWH(
            inner.left + inner.width * .18,
            inner.top,
            inner.width * .82,
            inner.height,
          );
          canvas.drawRect(
            picture,
            Paint()
              ..shader = LinearGradient(
                begin: Alignment.topLeft,
                end: Alignment.bottomRight,
                colors: [colors[0], colors[2]],
              ).createShader(picture),
          );
          for (var i = 0; i < 7; i++) {
            line.color = ink;
            canvas.drawRRect(
              RRect.fromRectAndRadius(
                Rect.fromLTWH(
                  inner.left,
                  inner.top + i * u * 2,
                  inner.width * .13,
                  u * 1.2,
                ),
                Radius.circular(u * .3),
              ),
              line,
            );
          }
      }
    }

    switch (variant % 5) {
      case 0:
        window(Rect.fromLTWH(w * .1, h * .1, w * .55, h * .62), kind: 0);
        window(Rect.fromLTWH(w * .5, h * .28, w * .4, h * .5), kind: 1);
      case 1:
        window(
          Rect.fromLTWH(w * .14, h * .12, w * .72, h * .68),
          dark: true,
          kind: 1,
        );
      case 2:
        window(
          Rect.fromLTWH(w * .08, h * .09, w * .84, h * .74),
          dark: true,
          kind: 2,
        );
      case 3:
        window(Rect.fromLTWH(w * .22, h * .16, w * .56, h * .6), kind: 0);
      default:
        window(Rect.fromLTWH(w * .06, h * .14, w * .46, h * .6), kind: 1);
        window(Rect.fromLTWH(w * .44, h * .1, w * .5, h * .66), kind: 0);
    }
    // The taskbar.
    final bar = u * 3.4;
    canvas.drawRect(
      Rect.fromLTWH(0, h - bar, w, bar),
      Paint()..color = const Color(0xE61C1C1E),
    );
    final icons = [
      const Color(0xFF4CC2FF),
      const Color(0xFFFFC83D),
      const Color(0xFF3FA34D),
      const Color(0xFFE8E8E8),
      const Color(0xFF8A7CF0),
    ];
    for (var i = 0; i < icons.length; i++) {
      final x = w / 2 + (i - icons.length / 2) * bar * .82;
      canvas.drawRRect(
        RRect.fromRectAndRadius(
          Rect.fromLTWH(x + bar * .14, h - bar * .8, bar * .56, bar * .56),
          Radius.circular(bar * .12),
        ),
        Paint()..color = icons[i],
      );
    }
  }

  @override
  bool shouldRepaint(DesktopPainter old) => old.variant != variant;
}

/// A card picture (PNG bytes) of [DesktopPainter].
Future<Uint8List> _picture(
  int variant, {
  Size size = const Size(640, 360),
}) async {
  final recorder = ui.PictureRecorder();
  DesktopPainter(variant).paint(Canvas(recorder), size);
  final image = await recorder.endRecording().toImage(
    size.width.toInt(),
    size.height.toInt(),
  );
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

class _AndroidBroker extends FakeBroker implements DesktopPreviewBrokerApi {
  _AndroidBroker(super.inventory, this.pictures);
  final Map<String, Uint8List> pictures;
  @override
  bool previewOffered(String token, String desktopId) =>
      pictures.containsKey(desktopId);
  @override
  Future<DesktopPreview> desktopPreview(String token, String desktopId) async {
    final bytes = pictures[desktopId];
    if (bytes == null) return const DesktopPreview.unavailable();
    return DesktopPreview(
      DesktopPreviewState.captured,
      picture: DesktopThumbnail(
        image: MemoryImage(bytes),
        modified: DateTime.now().subtract(const Duration(minutes: 2)),
        remote: true,
      ),
    );
  }
}

const _gb = 1024 * 1024 * 1024;

/// The Windows launcher's desktops: names in the page's language.
List<DesktopMachine> _machines(String lang) {
  DesktopMachine running(
    String id,
    String name, {
    double cpu = 20,
    int vcpus = 4,
    int memory = 8,
    double used = .5,
    MachineKind kind = MachineKind.vm,
    Color accent = Colors.blue,
  }) => DesktopMachine(
    id: id,
    name: name,
    description: '',
    kind: kind,
    localPcApproved: kind == MachineKind.pc,
    status: MachineStatus.ready,
    specification: kind == MachineKind.pc ? 'Windows 11 Pro' : 'Windows 11 Pro',
    accent: accent,
    powerState: 'running',
    powerStale: false,
    powerCanForceStop: kind == MachineKind.vm,
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
    resources: kind == MachineKind.pc
        ? VmResources(
            cpuUsagePercent: cpu,
            vcpus: vcpus,
            memoryUsedBytes: (memory * _gb * used).round(),
            memoryTotalBytes: memory * _gb,
            stale: false,
            measuredByHost: true,
            checkedAt: DateTime.now(),
          )
        : VmResources(
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
    running('pve-101', _w(lang, '디자인 1', 'Design 1'), cpu: 23, used: .64),
    running(
      'pve-104',
      _w(lang, '개발 1', 'Dev 1'),
      cpu: 41,
      vcpus: 8,
      memory: 16,
      used: .48,
      accent: Colors.indigo,
    ),
    running(
      'pve-109',
      _w(lang, '영상 편집', 'Video'),
      cpu: 67,
      vcpus: 8,
      memory: 32,
      used: .71,
      accent: Colors.deepOrange,
    ),
    DesktopMachine(
      id: 'pve-111',
      name: _w(lang, '회계 1', 'Accounting 1'),
      description: '',
      kind: MachineKind.vm,
      status: MachineStatus.poweredOff,
      specification: 'Windows 11 Pro',
      accent: Colors.teal,
      powerState: 'stopped',
      powerStale: false,
      hostOnline: false,
      resources: const VmResources(vcpus: 4, memoryTotalBytes: 8 * _gb),
    ),
    running(
      'pc-1',
      _w(lang, '회의실 PC', 'Meeting room PC'),
      cpu: 9,
      vcpus: 8,
      memory: 32,
      used: .3,
      kind: MachineKind.pc,
      accent: Colors.purple,
    ),
    running(
      'pve-130',
      _w(lang, '테스트', 'Test'),
      cpu: 4,
      vcpus: 2,
      memory: 4,
      used: .42,
      accent: Colors.green,
    ),
  ];
}

/// The same desktops for the Android list.
List<BrokerDesktop> _androidDesktops(String lang) {
  BrokerDesktop desk(
    String id,
    String name, {
    String power = 'running',
    double cpu = 20,
    int vcpus = 4,
    int memory = 8,
    double used = .5,
  }) => BrokerDesktop(
    id: id,
    name: name,
    kind: 'vm',
    description: '',
    os: 'Windows 11 Pro',
    status: power == 'stopped' ? 'powered_off' : 'online',
    canConnect: power == 'running',
    canStream: power == 'running',
    hostConnected: power == 'running',
    interactiveSession: power == 'running' ? true : null,
    virtualDisplayReady: power == 'running' ? true : null,
    powerCanForceStop: true,
    thumbnailPrivacy: const ThumbnailPrivacy(epoch: _epoch, allowed: true),
    resources: power == 'running'
        ? VmResources(
            cpuUsagePercent: cpu,
            vcpus: vcpus,
            memoryUsedBytes: (memory * _gb * used).round(),
            memoryTotalBytes: memory * _gb,
            uptimeSeconds: 3 * 3600 + 1200,
            stale: false,
            checkedAt: DateTime.now(),
          )
        : VmResources(vcpus: vcpus, memoryTotalBytes: memory * _gb),
    power: BrokerPowerSnapshot(
      state: power,
      stale: false,
      checkedAt: DateTime.now(),
    ),
  );
  return [
    desk('pve-101', _w(lang, '디자인 1', 'Design 1'), cpu: 23, used: .64),
    desk(
      'pve-104',
      _w(lang, '개발 1', 'Dev 1'),
      cpu: 41,
      vcpus: 8,
      memory: 16,
      used: .48,
    ),
    desk(
      'pve-109',
      _w(lang, '영상 편집', 'Video'),
      cpu: 67,
      vcpus: 8,
      memory: 32,
      used: .71,
    ),
    desk('pve-111', _w(lang, '회계 1', 'Accounting 1'), power: 'stopped'),
    desk(
      'pve-130',
      _w(lang, '테스트', 'Test'),
      cpu: 4,
      vcpus: 2,
      memory: 4,
      used: .42,
    ),
  ];
}

Uint8List _video(int pts) {
  final bytes = Uint8List(17);
  bytes[0] = 1;
  bytes[1] = 1;
  for (var i = 0; i < 8; i++) {
    bytes[4 + i] = (pts >> (8 * i)) & 255;
  }
  bytes.setAll(12, [0, 0, 0, 1, 0x65]);
  return bytes;
}

void _language(String lang) {
  AppLanguageController.debugSystemLanguage = lang;
  AppLanguageController.instance.debugReset(
    lang == 'ko' ? AppLanguage.ko : AppLanguage.en,
  );
}

void main() {
  final boundary = GlobalKey();
  final faults = <String>[];

  Future<void> shot(
    WidgetTester t,
    String lang,
    String name, {
    double ratio = 1,
  }) async {
    final fault = t.takeException();
    if (fault != null) faults.add('$lang/$name: $fault');
    await t.runAsync(() async {
      final image =
          await (boundary.currentContext!.findRenderObject()!
                  as RenderRepaintBoundary)
              .toImage(pixelRatio: ratio);
      final data = await image.toByteData(format: ui.ImageByteFormat.png);
      final file = File('$_dir/$lang/$name.png');
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

  Future<void> tapKey(WidgetTester t, String key) async {
    final target = find.byKey(Key(key)).first;
    await t.ensureVisible(target);
    await t.pumpAndSettle();
    await t.tap(target);
    await t.pumpAndSettle();
  }

  tearDown(() {
    AppLanguageController.debugSystemLanguage = null;
    AppLanguageController.instance.debugReset();
  });

  testWidgets(
    'Website: the Windows client and Host settings',
    (t) async {
      if (_dir.isEmpty) throw StateError('CAPTURE_DIR is required');
      await _fonts(t);
      debugDisableShadows = false;
      addTearDown(t.view.reset);
      final pictures = await t.runAsync(
        () async => {
          'pve-101': await _picture(0),
          'pve-104': await _picture(1),
          'pve-109': await _picture(2),
          'pc-1': await _picture(4),
          'pve-130': await _picture(3),
        },
      );
      for (final lang in ['ko', 'en']) {
        _language(lang);

        // Sign-in.
        size(t, const Size(1440, 900));
        await t.pumpWidget(
          RepaintBoundary(
            key: boundary,
            child: MultitapApp(
              key: UniqueKey(),
              api: TestBroker(),
              serverOrigin: _origin,
              appearance: AppAppearance(),
            ),
          ),
        );
        await t.pumpAndSettle();
        await t.runAsync(
          () => precacheImage(
            const AssetImage('assets/backgrounds/alpine-dusk.png'),
            boundary.currentContext!,
          ),
        );
        await t.pumpAndSettle();
        await t.enterText(find.byKey(const Key('sign-in-id')), 'alice');
        await t.pumpAndSettle();
        FocusManager.instance.primaryFocus?.unfocus();
        await t.pumpAndSettle();
        await shot(t, lang, 'client-signin');

        // The desktop list with pictures, then ⚙ settings.
        Future<void> launcher() async {
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
                  serverOrigin: _origin.toString(),
                  machines: _machines(lang),
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

        await launcher();
        await shot(t, lang, 'client-launcher');
        await tapKey(t, 'open-app-settings');
        await shot(t, lang, 'client-settings');

        // The session bar over a remote desktop, its 화면 panel open.
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
                    const CustomPaint(painter: DesktopPainter(0)),
                    ToolbarV2Bar(
                      title: _w(lang, '디자인 1', 'Design 1'),
                      latencyMs: 12,
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
        await t.tap(find.byKey(const Key('expand-session-toolbar')));
        await t.pump();
        await t.pumpAndSettle();
        await shot(t, lang, 'client-toolbar-bar');
        await t.tap(find.byKey(const Key('stream-menu-화면')));
        await t.pumpAndSettle();
        await shot(t, lang, 'client-toolbar');
        await t.tap(find.byKey(const Key('remote-settings')));
        await t.pumpAndSettle();
        await shot(t, lang, 'client-toolbar-settings');

        // Host settings: the overview of an approved desktop.
        size(t, const Size(680, 760));
        final host = FakeHostSettings(registered: true)
          ..displays = 1
          ..credentialsConfigured = true
          ..credentialUsername = 'tester'
          ..statusOverrides = {
            'version': '1.0.55',
            'desktopName': _w(lang, '디자인 1', 'Design 1'),
            'desktopId': 'pve-101',
            'serverOrigin': _origin.toString(),
            'broker': {
              'state': 'online',
              'online': true,
              'heartbeatAgeSeconds': 1,
              'lastReadyAt': DateTime.now().millisecondsSinceEpoch,
              'lastHeartbeatAt': DateTime.now().millisecondsSinceEpoch,
              'failureReason': '',
            },
            'windowsAutologon': {
              'schema': 1,
              'supported': true,
              'administrator': true,
              'state': 'observed',
              'reason': 'none',
              'revision': List.filled(64, 'a').join(),
              'accountLabel': 'tester',
              'enabled': true,
              'loggedIn': true,
            },
          };
        await t.pumpWidget(
          RepaintBoundary(
            key: boundary,
            child: ThemedApp(
              key: UniqueKey(),
              appearance: AppAppearance(),
              title: 'Multitap Host',
              home: HostSettingsScreen(api: host, closeGuard: FakeCloseGuard()),
            ),
          ),
        );
        await t.pumpAndSettle();
        await shot(t, lang, 'host-overview');
        for (final (page, name) in [
          (1, 'host-server'),
          (2, 'host-display'),
          (6, 'host-devices'),
          (5, 'host-windows-login'),
        ]) {
          final item = find.byKey(Key('host-nav-$page'));
          if (item.evaluate().isEmpty) continue;
          await t.ensureVisible(item);
          await t.tap(item);
          await t.pumpAndSettle();
          await shot(t, lang, name);
        }
        // Before approval: the server page where the address is entered.
        final fresh = FakeHostSettings()
          ..displays = 1
          ..statusOverrides = {'version': '1.0.55'};
        await t.pumpWidget(
          RepaintBoundary(
            key: boundary,
            child: ThemedApp(
              key: UniqueKey(),
              appearance: AppAppearance(),
              title: 'Multitap Host',
              home: HostSettingsScreen(
                api: fresh,
                closeGuard: FakeCloseGuard(),
              ),
            ),
          ),
        );
        await t.pumpAndSettle();
        final ask = find.byKey(const Key('host-confirm-dialog'));
        if (ask.evaluate().isNotEmpty) {
          Navigator.of(t.element(ask)).pop();
          await t.pumpAndSettle();
        }
        final serverPage = find.byKey(const Key('host-nav-1'));
        if (serverPage.evaluate().isNotEmpty) {
          await t.tap(serverPage);
          await t.pumpAndSettle();
        }
        await shot(t, lang, 'host-server-new');
        await t.pumpWidget(const SizedBox.shrink());
        await t.pumpAndSettle();
      }
      debugDisableShadows = true;
      expect(faults, isEmpty);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
    skip: !const bool.fromEnvironment('CAPTURE_UI'),
    timeout: const Timeout(Duration(minutes: 20)),
  );

  testWidgets(
    'Website: the Android client',
    (t) async {
      if (_dir.isEmpty) throw StateError('CAPTURE_DIR is required');
      await _fonts(t);
      debugDisableShadows = false;
      addTearDown(t.view.reset);
      final pictures = await t.runAsync(
        () async => {
          'pve-101': await _picture(0),
          'pve-104': await _picture(1),
          'pve-109': await _picture(2),
          'pve-130': await _picture(3),
        },
      );

      Future<void> list(String lang, Size at) async {
        size(t, at);
        MemoryStore(
          origin: 'https://vdi.example.com',
          username: 'alice',
          prefs: {'appearance.theme': 'dark'},
        ).install();
        final broker = _AndroidBroker(_androidDesktops(lang), pictures!);
        await t.pumpWidget(
          RepaintBoundary(
            key: boundary,
            child: AndroidVdiApp(
              key: UniqueKey(),
              apiFactory: (_) => broker,
              updatesFactory: (o) =>
                  AndroidUpdates(o, transport: catalog(const [])),
              updatePlatform: FakePlatform(),
            ),
          ),
        );
        await t.pumpAndSettle();
        await t.runAsync(() async {
          final context = boundary.currentContext!;
          for (final asset in ['alpine-dusk.png', 'alpine-dusk-day.jpg']) {
            await precacheImage(
              AssetImage('assets/backgrounds/$asset'),
              context,
            );
          }
        });
        await t.pump();
        if (at == const Size(412, 915)) {
          // The sign-in screen first (the remembered server and ID).
          await shot(t, lang, 'android-signin', ratio: 2);
        }
        t
                .widget<TextField>(find.byKey(const Key('sign-in-password')))
                .controller!
                .text =
            'secret-pass';
        await t.ensureVisible(find.byKey(const Key('login-submit')));
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('login-submit')));
        await t.pumpAndSettle();
        await t.runAsync(() async {
          await Future<void>.delayed(const Duration(milliseconds: 60));
          final context = boundary.currentContext!;
          for (final bytes in pictures.values) {
            await precacheImage(MemoryImage(bytes), context);
          }
        });
        for (var i = 0; i < 4; i++) {
          await t.pump(const Duration(milliseconds: 200));
        }
        await t.pumpAndSettle();
      }

      Future<void> session(String lang, Size at) async {
        size(t, at);
        AndroidNativeFake().install();
        final messenger =
            TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
        messenger.setMockMethodCallHandler(
          const MethodChannel('multitap/pen-input'),
          (_) async => null,
        );
        messenger.setMockMethodCallHandler(
          AndroidSessionKeepalive.channel,
          (call) async => call.method == 'start' ? 'started' : true,
        );
        final sockets = <SessionSocket>[];
        await t.pumpWidget(
          RepaintBoundary(
            key: boundary,
            child: MaterialApp(
              key: UniqueKey(),
              debugShowCheckedModeBanner: false,
              theme: androidTheme(buildAppTheme(brightness: Brightness.dark)),
              builder: (context, child) =>
                  TooltipVisibility(visible: false, child: child!),
              home: AndroidRemoteView(
                name: _w(lang, '디자인 1', 'Design 1'),
                token: 'not-persisted',
                options: const StreamOptions(),
                density: () async => null,
                settings: const AndroidSessionSettings(helpSeen: true),
                displayScale: true,
                load: (options, _, scale) async => BrokerConnection(
                  sessionId: 'android-session',
                  webSocketUrl: Uri.parse(
                    'wss://fixture.invalid/api/v1/sessions/android-session',
                  ),
                  options: options,
                  displayScale: scale,
                ),
                sessionFactory: (connection, media, _) => RemoteSession(
                  connection: connection,
                  token: 'not-persisted',
                  media: media,
                  dialer: (_, _) async {
                    final socket = SessionSocket();
                    sockets.add(socket);
                    return socket;
                  },
                ),
                pictureBuilder: (_) =>
                    const CustomPaint(painter: DesktopPainter(0)),
              ),
            ),
          ),
        );
        for (var i = 0; i < 4; i++) {
          await t.pump(const Duration(milliseconds: 20));
        }
        if (sockets.isEmpty) throw StateError('no session socket');
        sockets.last.incoming.add(
          jsonEncode({
            'type': 'stream.info',
            'codec': 'h264',
            'width': at.width.toInt() ~/ 2 * 2,
            'height': at.height.toInt() ~/ 2 * 2,
          }),
        );
        await t.pump(const Duration(milliseconds: 20));
        sockets.last.incoming.add(_video(16667));
        for (var i = 0; i < 6; i++) {
          await t.pump(const Duration(milliseconds: 40));
        }
        await t.pump(const Duration(seconds: 1));
        final ping = sockets.last.sent.lastWhere(
          (m) => m['type'] == 'ping',
          orElse: () => const {},
        );
        if (ping.isNotEmpty) {
          await t.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 23)),
          );
          sockets.last.incoming.add(
            jsonEncode({'type': 'pong', 'id': ping['id']}),
          );
          await t.pump(const Duration(milliseconds: 20));
        }
      }

      Future<void> endSession() async {
        await t.pumpWidget(const SizedBox.shrink());
        await t.pump(const Duration(seconds: 1));
        AndroidNativeFake.uninstall();
      }

      for (final lang in ['ko', 'en']) {
        _language(lang);
        await list(lang, const Size(412, 915));
        await shot(t, lang, 'android-launcher', ratio: 2);
        await list(lang, const Size(1280, 800));
        await shot(t, lang, 'android-tablet');
        await t.pumpWidget(const SizedBox.shrink());
        await t.pump(const Duration(seconds: 1));

        await session(lang, const Size(915, 412));
        await shot(t, lang, 'android-session', ratio: 2);
        await t.tap(find.byKey(const Key('expand-session-toolbar')));
        await t.pumpAndSettle();
        await shot(t, lang, 'android-session-menu', ratio: 2);
        await endSession();
      }
      await t.pumpWidget(const SizedBox.shrink());
      await t.pump(const Duration(minutes: 2));
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(AndroidStore.channel, null);
      debugDisableShadows = true;
      expect(faults, isEmpty);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: !const bool.fromEnvironment('CAPTURE_UI'),
    timeout: const Timeout(Duration(minutes: 20)),
  );
}

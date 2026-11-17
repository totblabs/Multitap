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
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_drive.dart';
import 'package:multitap_client/android/android_launcher.dart'
    show androidTheme;
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_touch.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/data/desktop_previews.dart';
import 'package:multitap_client/data/desktop_thumbnails.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/relative_mouse.dart';
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
import 'drive_session_test.dart'
    as drive_fixture
    show DriveSocket, ready, sessionId;
import 'host_settings_test.dart' show FakeCloseGuard, FakeHostSettings;
import 'remote_session_test.dart' show SessionSocket;
import 'helpers/pump_work.dart';
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

/// The staff guide's server: an account whose first password an
/// administrator set, and one desktop that is switched on when asked.
class _StaffBroker extends _AndroidBroker implements InitialPasswordBrokerApi {
  _StaffBroker(super.inventory, super.pictures, this.running);
  final List<BrokerDesktop> running;
  bool changeRequired = true;

  @override
  Future<BrokerSession> login(String username, String password) async {
    if (changeRequired) {
      throw const BrokerException('password_change_required', 428);
    }
    return super.login(username, password);
  }

  @override
  Future<void> setInitialPassword(
    String username,
    String password,
    String newPassword,
    String confirmation,
  ) async => changeRequired = false;

  @override
  Future<BrokerPowerResult> requestPower(
    String token,
    String desktopId,
    String action,
  ) async {
    inventory = running;
    return super.requestPower(token, desktopId, action);
  }
}

/// A phone without a mouse (MainActivity's pointer capture, never engaged).
class _NoMouse implements MousePresenceNative {
  @override
  Future<RelativeMousePath?> engage(
    Rect? picture, {
    bool pin = false,
    bool conceal = false,
  }) async => RelativeMousePath.injected;
  @override
  Future<void> release() async {}
  @override
  Future<void> armKey(bool armed) async {}
  @override
  void listen(void Function(String event) onEvent) {}
  @override
  void dispose() {}
  @override
  Future<bool> mousePresent() async => false;
  @override
  set onMouse(void Function(bool present)? callback) {}
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

/// The staff guide's one desktop, 내 업무 PC, switched off or running.
BrokerDesktop _staffDesktop({required bool running}) => BrokerDesktop(
  id: 'pve-201',
  name: '내 업무 PC',
  kind: 'vm',
  description: '',
  os: 'Windows 11 Pro',
  status: running ? 'online' : 'powered_off',
  canConnect: running,
  canStream: running,
  hostConnected: running,
  interactiveSession: running ? true : null,
  virtualDisplayReady: running ? true : null,
  powerCanForceStop: true,
  thumbnailPrivacy: const ThumbnailPrivacy(epoch: _epoch, allowed: true),
  resources: running
      ? VmResources(
          cpuUsagePercent: 12,
          vcpus: 4,
          memoryUsedBytes: (8 * _gb * .46).round(),
          memoryTotalBytes: 8 * _gb,
          uptimeSeconds: 240,
          stale: false,
          checkedAt: DateTime.now(),
        )
      : const VmResources(vcpus: 4, memoryTotalBytes: 8 * _gb),
  power: BrokerPowerSnapshot(
    state: running ? 'running' : 'stopped',
    stale: false,
    checkedAt: DateTime.now(),
  ),
);

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
            'version': '1.1.8',
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
          ..statusOverrides = {'version': '1.1.8'};
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

  // The staff manual "Multitap 시작하기 – 휴대폰·태블릿": the Android app from
  // the first start to the end of a connection, Korean only, with the
  // rectangles (image pixels) of what the reader taps in <dir>/ko/rects.json.
  //
  //   flutter test --no-pub test/site_capture_test.dart --plain-name 'Staff guide'
  //     --dart-define=STAFF_ANDROID=true --dart-define=CAPTURE_DIR=<folder>
  testWidgets(
    'Staff guide: Android screens (ko)',
    (t) async {
      if (_dir.isEmpty) throw StateError('CAPTURE_DIR is required');
      await _fonts(t);
      debugDisableShadows = false;
      addTearDown(t.view.reset);
      const lang = 'ko', ratio = 2.0, id = 'pve-201';
      const phone = Size(412, 915), landscape = Size(915, 412);
      // The real server address for a staff guide comes in with
      // --dart-define=STAFF_SERVER=...; the source keeps an example.
      const server = String.fromEnvironment(
        'STAFF_SERVER',
        defaultValue: 'https://vdi.example.com',
      );
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      final pictures = await t.runAsync(() async => {id: await _picture(0)});
      _language(lang);

      // Not pumpAndSettle: a signed-in list and a session keep marks
      // turning; this stops once nothing is scheduled, or after 2 s.
      Future<void> settle() => pumpWork(t, limit: const Duration(seconds: 2));

      final rects = <String, Map<String, List<int>>>{};
      void mark(String scene, String label, Finder finder) {
        if (finder.evaluate().isEmpty) {
          faults.add('$scene: nothing for $label');
          return;
        }
        // What shows of it: a scrolled panel reaches past the screen.
        final r = t
            .getRect(finder.first)
            .intersect(
              Offset.zero & t.view.physicalSize / t.view.devicePixelRatio,
            );
        // Scrolled out of sight: nothing to point at.
        if (r.width <= 0 || r.height <= 0) return;
        rects.putIfAbsent(scene, () => {})[label] = [
          for (final v in [r.left, r.top, r.width, r.height])
            (v * ratio).round(),
        ];
      }

      Finder key(String value) => find.byKey(Key(value));
      Finder button(String label) => find
          .ancestor(
            of: find.text(label),
            matching: find.byWidgetPredicate((w) => w is ButtonStyleButton),
          )
          .last;
      // The card of the dialog on top (Dialog itself fills the screen).
      Finder dialogCard() => find
          .descendant(
            of: find.byType(Dialog).last,
            matching: find.byType(Material),
          )
          .first;
      Future<void> tapOn(Finder target) async {
        await t.ensureVisible(target.first);
        await settle();
        await t.tap(target.first);
        await settle();
      }

      // The backgrounds, and the desktop's picture (a switched-off card
      // shows its last saved screen).
      Future<void> precacheBackgrounds() => t.runAsync(() async {
        final context = boundary.currentContext!;
        for (final asset in ['alpine-dusk.png', 'alpine-dusk-day.jpg']) {
          await precacheImage(AssetImage('assets/backgrounds/$asset'), context);
        }
        await precacheImage(MemoryImage(pictures![id]!), context);
      });

      // The Android store channel as MemoryStore keeps it, saying the
      // published app version (1.1.22) in the sign-in footer.
      void store({String? origin}) {
        final prefs = {'appearance.theme': 'dark'};
        var username = '';
        messenger.setMockMethodCallHandler(AndroidStore.channel, (call) async {
          final args = (call.arguments as Map?) ?? const {};
          switch (call.method) {
            case 'origin':
              return origin;
            case 'saveOrigin':
              origin = args['origin'] as String;
            case 'login':
              return {'username': username, 'password': null};
            case 'saveLogin':
              username = args['username'] as String;
            case 'preferences':
              return Map<String, String>.of(prefs);
            case 'savePreference':
              prefs[args['key'] as String] = args['value'] as String;
            case 'appVersion':
              return '1.1.22';
          }
          return null;
        });
      }

      Future<void> app({String? origin, required BrokerApi broker}) async {
        size(t, phone);
        store(origin: origin);
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
        await settle();
        await precacheBackgrounds();
        await t.pump();
        await settle();
      }

      // ---------------------------------------------- 1, 2: the server
      final broker = _StaffBroker(
        [_staffDesktop(running: false)],
        pictures!,
        [_staffDesktop(running: true)],
      );
      await app(broker: broker);
      final dialog = find.byType(AndroidServerDialog);
      if (dialog.evaluate().isEmpty) await tapOn(key('open-server-settings'));
      Finder inDialog(bool Function(Widget) test) =>
          find.descendant(of: dialog, matching: find.byWidgetPredicate(test));
      final field = inDialog((w) => w is TextField);
      final test = inDialog((w) => w is OutlinedButton);
      final save = inDialog((w) => w is FilledButton);
      final cancel = inDialog((w) => w is TextButton).last;
      final scheme = inDialog(
        (w) => w.runtimeType.toString() == 'BrokerTransportChoice',
      );
      Future<void> typeServer() async {
        await t.enterText(field, server);
        await settle();
        FocusManager.instance.primaryFocus?.unfocus();
        await settle();
      }

      void markServer(String scene) {
        mark(scene, 'dialog', dialogCard());
        mark(scene, 'scheme', scheme);
        mark(scene, 'server-field', field);
        mark(scene, 'test-button', test);
        mark(scene, 'cancel', cancel);
        mark(scene, 'save', save);
      }

      await typeServer();
      await shot(t, lang, 'a01-server', ratio: ratio);
      markServer('a01-server');
      // The app's own dialog would ask the real network; the same dialog
      // with the broker's answer faked, as the dialog tests do.
      final signIn = t.element(key('open-server-settings'));
      Navigator.of(t.element(dialog)).pop();
      await settle();
      unawaited(
        showDialog<Uri>(
          context: signIn,
          barrierDismissible: false,
          builder: (_) => AndroidServerDialog(
            initial: null,
            save: (_) async {},
            probe: (_) async =>
                const BrokerProbeResult(version: '1.1.40', elapsedMs: 38),
          ),
        ),
      );
      await settle();
      await typeServer();
      await t.tap(test);
      await settle();
      await shot(t, lang, 'a02-server-tested', ratio: ratio);
      markServer('a02-server-tested');
      mark('a02-server-tested', 'result', key('server-test-result'));
      await t.pumpWidget(const SizedBox.shrink());
      await settle();

      // ------------------------------------ 3, 4: sign-in, new password
      await app(origin: server, broker: broker);
      await t.enterText(key('sign-in-id'), 'gildong');
      await t.enterText(key('sign-in-password'), 'first-pass-1');
      await settle();
      FocusManager.instance.primaryFocus?.unfocus();
      await settle();
      await shot(t, lang, 'a03-login', ratio: ratio);
      mark('a03-login', 'server', key('open-server-settings'));
      mark('a03-login', 'server-change', key('login-server-change'));
      mark('a03-login', 'id', key('sign-in-id'));
      mark('a03-login', 'password', key('sign-in-password'));
      mark('a03-login', 'remember', key('remember-password'));
      mark('a03-login', 'login', key('login-submit'));

      await t.ensureVisible(key('login-submit'));
      await settle();
      await t.tap(key('login-submit'));
      await pumpWork(t);
      await settle();
      await t.enterText(key('initial-new-password'), 'My-new-pass-71');
      await t.enterText(key('initial-confirm-password'), 'My-new-pass-71');
      await settle();
      FocusManager.instance.primaryFocus?.unfocus();
      await settle();
      await shot(t, lang, 'a04-newpw', ratio: ratio);
      mark('a04-newpw', 'dialog', dialogCard());
      mark('a04-newpw', 'new-password', key('initial-new-password'));
      mark('a04-newpw', 'confirm-password', key('initial-confirm-password'));
      mark('a04-newpw', 'cancel', key('cancel-initial-password'));
      mark('a04-newpw', 'save', key('save-initial-password'));

      // ------------------------------------------- 5 to 8: the list
      await t.tap(key('save-initial-password'));
      await pumpWork(t);
      await settle();
      await shot(t, lang, 'a05-list-off', ratio: ratio);
      mark('a05-list-off', 'card', key('card-$id'));
      mark('a05-list-off', 'power', key('power-on-$id'));
      mark('a05-list-off', 'start-button', key('start-$id'));
      mark('a05-list-off', 'power-menu', key('desktop-power-$id'));
      mark('a05-list-off', 'account', key('account-menu'));

      await tapOn(key('power-on-$id'));
      await shot(t, lang, 'a06-poweron', ratio: ratio);
      mark('a06-poweron', 'dialog', dialogCard());
      mark('a06-poweron', 'confirm', key('confirm-power-start-$id'));
      mark('a06-poweron', 'cancel', key('cancel-power-start-$id'));

      await t.tap(key('confirm-power-start-$id'));
      await pumpWork(t);
      await t.runAsync(() async {
        await Future<void>.delayed(const Duration(milliseconds: 60));
        await precacheImage(
          MemoryImage(pictures[id]!),
          boundary.currentContext!,
        );
      });
      for (var i = 0; i < 4; i++) {
        await t.pump(const Duration(milliseconds: 200));
      }
      await pumpWork(t);
      await shot(t, lang, 'a07-list-on', ratio: ratio);
      mark('a07-list-on', 'card', key('card-$id'));
      mark('a07-list-on', 'connect', key('open-$id'));
      mark('a07-list-on', 'play', key('play-$id'));
      mark('a07-list-on', 'power-menu', key('desktop-power-$id'));

      await t.ensureVisible(key('open-$id'));
      await settle();
      await t.tap(key('open-$id'));
      await t.pump();
      await t.pump(const Duration(milliseconds: 300));
      await settle();
      await shot(t, lang, 'a08-confirm', ratio: ratio);
      mark('a08-confirm', 'dialog', dialogCard());
      mark('a08-confirm', 'confirm', key('confirm-connect-$id'));
      mark('a08-confirm', 'cancel', key('cancel-connect-$id'));
      await t.tap(key('cancel-connect-$id'));
      await settle();
      await t.pumpWidget(const SizedBox.shrink());
      await t.pump(const Duration(seconds: 1));

      // ------------------------------------------ 9 to 17: a session
      // A phone on its side at twice the density, so 해상도 offers what a
      // phone gets; a connection that allows shared folders, with 화면 배율.
      t.view.devicePixelRatio = ratio;
      t.view.physicalSize = landscape * ratio;
      t.view.padding = FakeViewPadding.zero;
      t.view.viewPadding = FakeViewPadding.zero;
      AndroidNativeFake().install();
      const driveChannel = MethodChannel('multitap/android/drive-staff');
      messenger.setMockMethodCallHandler(
        driveChannel,
        (call) async => call.method == 'storageAccess'
            ? {'supported': true, 'granted': true}
            : null,
      );
      messenger.setMockMethodCallHandler(
        const MethodChannel('multitap/pen-input'),
        (_) async => null,
      );
      messenger.setMockMethodCallHandler(
        AndroidSessionKeepalive.channel,
        (call) async => call.method == 'start' ? 'started' : true,
      );
      final driveFiles = AndroidDriveFileSystem(channel: driveChannel);
      final sessionUrl =
          'wss://fixture.invalid/api/v1/sessions/${drive_fixture.sessionId}';
      final sockets = <SessionSocket>[];
      final driveSockets = <drive_fixture.DriveSocket>[];
      final asked = <StreamOptions>[];
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
              name: '내 업무 PC',
              token: 'not-persisted',
              options: const StreamOptions(),
              density: () async => null,
              decoderLimits: () async => null,
              settings: const AndroidSessionSettings(
                helpSeen: true,
                touch: AndroidTouchMode.trackpad,
              ),
              displayScale: true,
              driveFiles: driveFiles,
              relativeMouseNative: _NoMouse(),
              load: (options, _, scale) async {
                asked.add(options);
                return BrokerConnection(
                  sessionId: drive_fixture.sessionId,
                  webSocketUrl: Uri.parse(sessionUrl),
                  options: options,
                  epochMedia: true,
                  viewportProvisioning: true,
                  displayScale: scale,
                  cursor: true,
                  relativeMouse: true,
                  driveWebSocketUrl: Uri.parse('$sessionUrl/drive'),
                  driveFs: true,
                );
              },
              sessionFactory: (connection, media, stats) => RemoteSession(
                connection: connection,
                token: 'not-persisted',
                media: media,
                statsSink: stats,
                dialer: (_, _) async {
                  final socket = SessionSocket();
                  sockets.add(socket);
                  return socket;
                },
                driveFactory: (connection, token) => DriveSession(
                  url: connection.driveWebSocketUrl!,
                  token: token,
                  sessionId: connection.sessionId,
                  files: driveFiles,
                  fs: connection.driveFs,
                  dialer: (_, _) async {
                    final socket = drive_fixture.DriveSocket();
                    driveSockets.add(socket);
                    return socket;
                  },
                ),
              ),
              pictureBuilder: (_) =>
                  const CustomPaint(painter: DesktopPainter(0)),
            ),
          ),
        ),
      );
      for (var i = 0; i < 5; i++) {
        await t.pump(const Duration(milliseconds: 20));
      }
      if (sockets.isEmpty || asked.isEmpty) {
        throw StateError('no session socket');
      }
      final width = asked.last.maxWidth, height = asked.last.maxHeight;
      sockets.last.incoming.add(
        jsonEncode({
          'type': 'stream.info',
          'codec': 'h264',
          'width': width,
          'height': height,
          'sourceWidth': width,
          'sourceHeight': height,
        }),
      );
      await t.pump(const Duration(milliseconds: 20));
      sockets.last.incoming.add(_video(16667));
      for (var i = 0; i < 6; i++) {
        await t.pump(const Duration(milliseconds: 40));
      }
      // The Host applied the 100 % a new connection asks for.
      sockets.last.incoming.add(
        jsonEncode({
          'type': 'display.scale.applied',
          'sessionId': drive_fixture.sessionId,
          'protocol': 'display-scale-v1',
          'ok': true,
          'scale': 100,
          'maximum': 200,
          'code': 0,
        }),
      );
      await t.pump(const Duration(milliseconds: 40));
      if (driveSockets.isNotEmpty) {
        driveSockets.last.emit(drive_fixture.ready);
        await t.pump(const Duration(milliseconds: 40));
      } else {
        faults.add('a14-folder: no shared-folder channel');
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
      await settle();

      void markCluster(String scene) {
        mark(scene, 'menu-button', key('expand-session-toolbar'));
        mark(scene, 'input-button', key('android-remote-touch-mode-button'));
        mark(scene, 'keyboard-button', key('android-remote-keyboard-button'));
      }

      final menuCard = key('session-command-bar');
      // Everything folded, then the menu open on its rows.
      Future<void> openMenu() async {
        for (var i = 0; i < 3; i++) {
          if (key('session-menu-barrier').evaluate().isEmpty) break;
          await t.tap(key('expand-session-toolbar'));
          await settle();
        }
        await t.tap(key('expand-session-toolbar'));
        await settle();
        if (menuCard.evaluate().isEmpty) faults.add('the menu did not open');
      }

      await shot(t, lang, 'a09-session', ratio: ratio);
      markCluster('a09-session');

      await openMenu();
      await shot(t, lang, 'a10-menu', ratio: ratio);
      markCluster('a10-menu');
      mark('a10-menu', 'menu', menuCard);
      mark('a10-menu', 'row-quality', key('session-quality'));
      mark('a10-menu', 'row-screen', key('android-session-screen'));
      mark('a10-menu', 'row-folder', key('android-session-share'));
      mark('a10-menu', 'row-settings', key('android-session-more'));
      mark('a10-menu', 'row-disconnect', key('session-disconnect'));

      await tapOn(key('android-session-screen'));
      // The panel scrolls on a phone on its side: 해상도 brought into view,
      // 배율 above it stays.
      await t.ensureVisible(key('android-screen-resolution'));
      await settle();
      await shot(t, lang, 'a11-screen', ratio: ratio);
      mark('a11-screen', 'panel', key('android-screen-panel'));
      mark('a11-screen', 'back', key('session-menu-back'));
      mark('a11-screen', 'fit', key('android-screen-fit'));
      mark('a11-screen', 'scale-row', key('android-screen-scale'));
      mark('a11-screen', 'resolution-row', key('android-screen-resolution'));

      await openMenu();
      await tapOn(key('android-session-more'));
      await shot(t, lang, 'a12-settings', ratio: ratio);
      mark('a12-settings', 'panel', key('android-more-panel'));
      mark('a12-settings', 'back', key('session-menu-back'));
      mark('a12-settings', 'row-quality', key('android-session-settings'));
      mark('a12-settings', 'row-mouse-lock', key('android-session-game-mouse'));
      mark('a12-settings', 'row-input', key('android-session-input'));
      mark('a12-settings', 'row-guide', key('android-session-help'));

      await tapOn(key('android-session-settings'));
      await shot(t, lang, 'a13-quality', ratio: ratio);
      mark('a13-quality', 'panel', key('android-settings-panel'));
      mark('a13-quality', 'mode-row', key('stream-preset'));
      mark('a13-quality', 'fps-60', key('stream-fps-60'));
      mark('a13-quality', 'cancel', key('cancel-stream-settings'));
      mark('a13-quality', 'apply', key('apply-stream-settings'));

      await openMenu();
      await tapOn(key('android-session-share'));
      await shot(t, lang, 'a14-folder', ratio: ratio);
      mark('a14-folder', 'panel', key('android-share-panel'));
      mark('a14-folder', 'back', key('session-menu-back'));
      mark('a14-folder', 'add', key('drive-add'));

      await openMenu();
      await tapOn(key('android-session-more'));
      await tapOn(key('android-session-help'));
      await shot(t, lang, 'a15-guide', ratio: ratio);
      mark('a15-guide', 'panel', key('android-touch-help'));
      mark('a15-guide', 'close', key('android-touch-help-close'));

      await openMenu();
      await t.tap(key('session-disconnect'));
      await settle();
      await shot(t, lang, 'a16-disconnect', ratio: ratio);
      mark('a16-disconnect', 'dialog', dialogCard());
      mark('a16-disconnect', 'confirm', button(l10n.disconnect));
      mark('a16-disconnect', 'cancel', button(l10n.cancel));
      await t.tap(button(l10n.cancel));
      await settle();

      // The PC key row above the phone keyboard (the keyboard itself is
      // Android's and is not drawn here).
      for (var i = 0; i < 3; i++) {
        if (key('session-menu-barrier').evaluate().isEmpty) break;
        await t.tap(key('expand-session-toolbar'));
        await settle();
      }
      await t.tap(key('android-remote-keyboard-button'));
      await settle();
      await shot(t, lang, 'a17-keyrow', ratio: ratio);
      markCluster('a17-keyrow');
      mark('a17-keyrow', 'key-row', key('android-key-row'));
      mark('a17-keyrow', 'key-win', key('android-key-win'));
      mark('a17-keyrow', 'key-hide', key('android-key-hide'));

      await t.pumpWidget(const SizedBox.shrink());
      await t.pump(const Duration(seconds: 3));
      AndroidNativeFake.uninstall();
      await t.pump(const Duration(minutes: 2));
      for (final channel in [
        AndroidStore.channel,
        driveChannel,
        const MethodChannel('multitap/pen-input'),
        AndroidSessionKeepalive.channel,
      ]) {
        messenger.setMockMethodCallHandler(channel, null);
      }
      await t.runAsync(() async {
        final file = File('$_dir/$lang/rects.json');
        await file.parent.create(recursive: true);
        await file.writeAsString(
          const JsonEncoder.withIndent('  ').convert(rects),
        );
      });
      debugDisableShadows = true;
      expect(faults, isEmpty);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.android),
    skip: !const bool.fromEnvironment('STAFF_ANDROID'),
    timeout: const Timeout(Duration(minutes: 20)),
  );
}

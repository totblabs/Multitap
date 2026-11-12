// The bare session bar fixture of the toolbar tests: the real SessionToolbar
// wired to the real panels the way RemoteView's _menus() wires them, over
// sample values. No broker, Host, VM, folder or USB device is touched (fake
// socket, file system and USB backend).
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/display_scale.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/drive_memory.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_connection_panel.dart';
import 'package:multitap_client/ui/session_panels.dart';
import 'package:multitap_client/ui/session_toolbar.dart';
import 'package:multitap_client/ui/session_tools.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';

import '../drive_session_test.dart'
    show DriveSocket, FakeFiles, driveUrl, flush, ready, sessionId;
import '../usb_redirection_test.dart' show started, shared;


const toolbarV2Machine = DesktopMachine(
  id: 'toolbar-v2',
  name: 'VM173 디자인팀',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.blue,
  maxBitrate: 100000000,
);

/// Sample measurements in the shape sessionQualityValues returns.
const toolbarV2Values = <SessionQualityValue>[
  (id: 'resolution', label: '해상도 · 원격 화면', value: '2560 × 1440'),
  (id: 'fps', label: '화면 · PC 디코딩', value: '60.0 fps'),
  (id: 'rate', label: '수신 · 받은 영상', value: '11.8 Mbps'),
  (id: 'path', label: '영상 경로', value: 'P2P 직결'),
  (id: 'encode', label: '인코딩 · Host 처리', value: '2.10 ms'),
  (id: 'decode', label: '디코딩 · PC 처리', value: '1.35 ms'),
];

/// The same on a slow connection.
const toolbarV2PoorValues = <SessionQualityValue>[
  (id: 'resolution', label: '해상도 · 원격 화면', value: '2560 × 1440'),
  (id: 'fps', label: '화면 · PC 디코딩', value: '41.3 fps'),
  (id: 'rate', label: '수신 · 받은 영상', value: '6.2 Mbps'),
  (id: 'path', label: '영상 경로', value: 'TCP 중계'),
  (id: 'encode', label: '인코딩 · Host 처리', value: '3.42 ms'),
  (id: 'decode', label: '디코딩 · PC 처리', value: '2.08 ms'),
];

/// A pictured session as far as 배율 reads it: [scale] applied, automatic.
class ToolbarV2ScaleSession extends Fake implements RemoteSession {
  ToolbarV2ScaleSession({this.scale = 100});
  final int scale;
  @override
  DisplayScaleApplied? get displayScaleApplied =>
      DisplayScaleApplied(ok: true, scale: scale, maximum: 200, code: 0);
  @override
  int? get displayScaleWanted => null;
  @override
  bool get closed => false;
  @override
  int? get sourceWidth => 2560;
  @override
  int? get sourceHeight => 1440;
  @override
  void addListener(VoidCallback listener) {}
  @override
  void removeListener(VoidCallback listener) {}
}

/// 공유 폴더 and USB 장치 as the 폴더·USB panel shows them connected: two
/// folders the VM has mapped (the second read-only) and one USB stick the VM
/// has. The fake socket, file system and USB backend of the panels' own
/// tests (as in panels_capture_test.dart).
class ToolbarV2Share {
  ToolbarV2Share._(this.drive, this.usb);
  final DriveSession drive;
  final UsbSession usb;

  static Future<ToolbarV2Share> start(WidgetTester tester) async {
    late DriveSession drive;
    late UsbSession usb;
    await tester.runAsync(() async {
      final socket = DriveSocket(), files = FakeFiles();
      drive = DriveSession(
        url: driveUrl,
        token: 'not-persisted',
        sessionId: sessionId,
        files: files,
        memory: SessionDriveMemory(),
        server: 'https://fixture.invalid',
        desktopId: 'pve-149',
        dialer: (_, _) async => socket,
        log: ProductLog(),
      );
      await drive.start();
      socket.emit(ready);
      await flush();
      for (final pick in const [
        DrivePick('C:\\업무\\문서', '문서'),
        DrivePick('C:\\업무\\작업 자료', '작업 자료'),
      ]) {
        files.pick = pick;
        await drive.addFolder();
      }
      files.pick = null;
      if (drive.shares.length > 1) {
        await drive.setReadOnly(drive.shares.last.id, true);
      }
      // The Host's answer: every offered folder has its drive letter.
      socket.emit({
        'type': 'drive.mounted',
        'shares': [
          for (final (index, share) in drive.shares.indexed)
            {
              'id': share.id,
              'state': 'mapped',
              'letter': index == 0 ? 'Z' : 'Y',
            },
        ],
      });
      await flush();
      final parts = await started();
      usb = parts.$1;
      await shared(usb, parts.$2, parts.$3);
    });
    return ToolbarV2Share._(drive, usb);
  }

  Future<void> close(WidgetTester tester) => tester.runAsync(() async {
    await usb.close();
    await drive.close();
  });
}

/// The session bar as a stream window builds it, without a session: the real
/// [SessionToolbar] with RemoteView's three commands (the same keys, labels,
/// tooltips and 메뉴 rows), each opening the real panel with sample values,
/// and the 연결 panel under the chip. [onAction] hears what a panel asked
/// for ('mode:fullscreen', 'scale:125', 'quality', 'mouse', 'guide',
/// 'pin:true', 'stats', 'saver', 'minimize', 'restore', 'disconnect') and
/// 'interact'.
class ToolbarV2Bar extends StatefulWidget {
  const ToolbarV2Bar({
    super.key,
    this.title = 'VM173 디자인팀',
    this.connected = true,
    this.latencyMs = 9,
    this.shown = SessionDisplayMode.windowed,
    this.modes = const [
      SessionDisplayMode.windowed,
      SessionDisplayMode.fullscreen,
    ],
    this.windowButtons = false,
    this.gameMouse = false,
    this.mouseLockOffered = true,
    this.shareOffered = true,
    this.share,
    this.folders = 0,
    this.devices = 0,
    this.shareReason,
    this.values = toolbarV2Values,
    this.saver = true,
    this.touch = false,
    this.pointerHeld = false,
    this.onAction,
  });

  final String title;
  final bool connected;
  final double? latencyMs;

  /// The current screen mode and the modes the 화면 panel offers.
  final SessionDisplayMode shown;
  final List<SessionDisplayMode> modes;

  /// 최소화 and 창 모드로 on the bar, as in full screen.
  final bool windowButtons;

  /// 마우스 잠금 is on; [mouseLockOffered]: the 설정 panel has its row.
  final bool gameMouse, mouseLockOffered;

  /// The bar has 폴더·USB. Its panel opens only with [share] (live fakes);
  /// without one, [folders] and [devices] are the connected counts the
  /// button and the 메뉴 rows show.
  final bool shareOffered;
  final ToolbarV2Share? share;
  final int folders, devices;

  /// Why 폴더·USB cannot be used (the button is greyed and says so).
  final String? shareReason;

  /// What the 연결 panel lists; [saver]: its advice has 절약으로 다시 연결.
  final List<SessionQualityValue> values;
  final bool saver;
  final bool touch, pointerHeld;
  final ValueChanged<String>? onAction;

  @override
  State<ToolbarV2Bar> createState() => ToolbarV2BarState();
}

class ToolbarV2BarState extends State<ToolbarV2Bar> {
  final _toolbarKey = GlobalKey<SessionToolbarState>();
  final _popups = SessionPopups();
  final _scale = ToolbarV2ScaleSession();
  Rect? _bounds;

  /// The bar itself.
  SessionToolbarState? get toolbar => _toolbarKey.currentState;

  void _act(String name) => widget.onAction?.call(name);

  /// Under the whole bar: where a 메뉴 row opens its panel.
  Rect? _barAnchor() {
    final box = _toolbarKey.currentContext?.findRenderObject();
    final bounds = _bounds;
    if (box is! RenderBox || !box.attached || bounds == null) return null;
    return bounds.shift(box.localToGlobal(Offset.zero));
  }

  String? get _qualityReason => widget.connected ? null : '연결된 뒤 바꿀 수 있어요';
  String? get _guideReason => widget.connected ? null : '연결된 뒤 볼 수 있어요';

  Future<bool> _connection(Rect anchor) async {
    final choice = await showSessionPanel<String>(
      context,
      anchor: anchor,
      width: SessionConnectionPanel.width,
      minHeight: SessionConnectionPanel.minHeight,
      label: SessionTool.quality.label,
      popups: _popups,
      builder: (context) => SessionConnectionPanel(
        title: widget.title,
        mode: '스트리밍',
        latency: () => widget.connected ? widget.latencyMs : null,
        connected: () => widget.connected,
        // Nothing measured before the connection, as the session reports it.
        values: () => widget.connected
            ? widget.values
            : sessionQualityValues(null, device: 'PC'),
        advice: () => toolbar?.poorSustained == true,
        onSaver: widget.saver ? () => Navigator.pop(context, 'saver') : null,
        onStats: () => Navigator.pop(context, 'stats'),
      ),
    );
    if (choice == null) return false;
    _act(choice);
    return true;
  }

  Future<bool> _screen(Rect? anchor) async {
    final choice = await showSessionPanel<SessionScreenChoice>(
      context,
      anchor: anchor,
      width: SessionScreenPanel.widthFor(widget.modes.length),
      minHeight: 250,
      label: SessionTool.screen.label,
      popups: _popups,
      builder: (context) => SessionScreenPanel(
        modes: widget.modes,
        shown: widget.shown,
        session: widget.connected ? _scale : null,
        scaleAvailable: widget.connected,
        viewportPixels: const Size(2560, 1440),
        resolutionAvailable: widget.connected,
        onSelected: (choice) => Navigator.pop(context, choice),
      ),
    );
    if (choice == null) return false;
    final mode = choice.mode, scale = choice.scale;
    _act(
      mode != null
          ? 'mode:${mode.name}'
          : scale != null
          ? 'scale:$scale'
          : 'displays',
    );
    return true;
  }

  Future<bool> _share(Rect? anchor, {SessionShareTab? tab}) async {
    final share = widget.share;
    if (share == null || !mounted) return false;
    final dialogContext = Navigator.of(context).overlay!.context;
    await showSessionPanel<void>(
      context,
      anchor: anchor,
      width: SessionSharePanel.width,
      minHeight: SessionSharePanel.minHeight,
      label: SessionTool.share.label,
      popups: _popups,
      keepOpenForDialog: true,
      builder: (_) => SessionSharePanel(
        drive: share.drive,
        usb: share.usb,
        initial: tab ?? SessionShareTab.folders,
        dialogContext: dialogContext,
      ),
    );
    return false;
  }

  /// 화질: a panel under [anchor] (its back button returns to 설정),
  /// a dialog without one.
  Future<bool> _quality(Rect? anchor, {VoidCallback? onBack}) async {
    final options = await (anchor == null
        ? showDialog<StreamOptions>(
            context: context,
            builder: (_) => const StreamSettingsDialog(
              machine: toolbarV2Machine,
              initialOptions: StreamOptions.recommended,
              reconnect: true,
            ),
          )
        : showStreamSettingsPanel(
            context,
            anchor: anchor,
            machine: toolbarV2Machine,
            initialOptions: StreamOptions.recommended,
            popups: _popups,
            onBack: onBack,
          ));
    if (options == null) return false;
    _act('quality');
    return true;
  }

  Future<bool> _settings(Rect? anchor) async {
    while (mounted) {
      final choice = await showSessionPanel<SessionSettingsChoice>(
        context,
        anchor: anchor,
        width: SessionSettingsPanel.width,
        minHeight: SessionSettingsPanel.minHeight,
        label: SessionTool.more.label,
        popups: _popups,
        builder: (context) => SessionSettingsPanel(
          quality: true,
          qualitySummary: '작업 · 60 fps',
          qualityReason: _qualityReason,
          mouseLock: widget.mouseLockOffered ? widget.gameMouse : null,
          pinned: toolbar?.pinned ?? false,
          onPinned: (pinned) {
            toolbar?.setPinned(pinned);
            _act('pin:$pinned');
          },
          guide: true,
          guideReason: _guideReason,
          onSelected: (choice) => Navigator.pop(context, choice),
        ),
      );
      if (!mounted || choice == null) return false;
      switch (choice) {
        case SessionSettingsChoice.quality:
          var back = false;
          final applied = await _quality(anchor, onBack: () => back = true);
          if (applied) return true;
          if (!back) return false;
        case SessionSettingsChoice.mouseLock:
          _act('mouse');
          return true;
        case SessionSettingsChoice.guide:
          _act('guide');
          return false;
      }
    }
    return false;
  }

  List<Widget> _actions() {
    const palette = AppPalette(true);
    final shown = widget.shown;
    final share = widget.share;
    final folders = share == null
        ? widget.folders
        : SessionSharePanel.mappedFolders(share.drive);
    final devices = share == null
        ? widget.devices
        : SessionSharePanel.sharedUsb(share.usb);
    final shared = folders + devices;
    final usable = widget.shareReason == null;
    final quality = _qualityReason, guide = _guideReason;
    return [
      SessionToolbarMenu.command(
        key: const Key('stream-menu-화면'),
        label: SessionTool.screen.label,
        tooltip: '화면 모드 · ${shown.label}',
        icon: sessionDisplayModeIcon(shown),
        keepOpenInOverflow: true,
        onInteract: () => _act('interact'),
        onPressed: () => unawaited(_screen(_barAnchor())),
        onPressedAt: (anchor) => _screen(anchor),
      ),
      if (widget.shareOffered)
        SessionToolbarMenu.command(
          key: const Key('remote-share'),
          label: SessionTool.share.label,
          tooltip: !usable
              ? SessionTool.share.label
              : shared > 0
              ? [
                  if (folders > 0) '공유 폴더 $folders개 연결됨',
                  if (devices > 0) 'USB 장치 $devices개 연결됨',
                ].join(' · ')
              : '폴더·USB · PC의 폴더와 USB 장치를 데스크톱에서 쓰기',
          icon: usable ? SessionTool.drives.icon : Icons.folder_off_rounded,
          active: shared > 0,
          badge: shared > 0 ? palette.green : null,
          keepOpenInOverflow: true,
          onInteract: () => _act('interact'),
          onPressed: !usable ? null : () => unawaited(_share(_barAnchor())),
          onPressedAt: !usable ? null : (anchor) => _share(anchor),
          disabledReason: widget.shareReason,
          overflow: [
            SessionMenuItem(
              SessionTool.drives.label,
              key: const Key('remote-drives'),
              icon: usable ? SessionTool.drives.icon : Icons.folder_off_rounded,
              value: folders > 0 ? '$folders개 연결됨' : null,
              color: folders > 0 ? palette.green : null,
              keepToolbarOpen: true,
              disabledReason: widget.shareReason,
              onSelected: !usable
                  ? null
                  : () => unawaited(
                      _share(_barAnchor(), tab: SessionShareTab.folders),
                    ),
            ),
            SessionMenuItem(
              SessionTool.usb.label,
              key: const Key('remote-usb'),
              icon: usable ? SessionTool.usb.icon : Icons.usb_off_rounded,
              value: devices > 0 ? '$devices개 연결됨' : null,
              color: devices > 0 ? palette.green : null,
              keepToolbarOpen: true,
              disabledReason: widget.shareReason,
              onSelected: !usable
                  ? null
                  : () => unawaited(
                      _share(_barAnchor(), tab: SessionShareTab.usb),
                    ),
            ),
          ],
        ),
      SessionToolbarMenu.command(
        key: const Key('remote-settings'),
        label: SessionTool.more.label,
        tooltip: '설정 · 화질, 사용안내 등',
        icon: SessionTool.more.icon,
        keepOpenInOverflow: true,
        onInteract: () => _act('interact'),
        onPressed: () => unawaited(_settings(_barAnchor())),
        onPressedAt: (anchor) => _settings(anchor),
        overflow: [
          SessionMenuItem(
            SessionTool.settings.label,
            key: const Key('remote-stream-settings'),
            icon: SessionTool.settings.icon,
            keepToolbarOpen: true,
            disabledReason: quality,
            onSelected: quality != null
                ? null
                : () => unawaited(_quality(_barAnchor())),
          ),
          if (widget.mouseLockOffered)
            SessionMenuItem(
              SessionTool.gameMouse.label,
              key: const Key('session-game-mouse'),
              icon: SessionTool.gameMouse.icon,
              value: 'Ctrl+Shift+Alt+Z',
              checked: widget.gameMouse,
              onSelected: () => _act('mouse'),
            ),
          SessionMenuItem(
            SessionTool.guide.label,
            key: const Key('remote-session-guide'),
            icon: SessionTool.guide.icon,
            keepToolbarOpen: true,
            disabledReason: guide,
            onSelected: guide != null ? null : () => _act('guide'),
          ),
        ],
      ),
    ];
  }

  @override
  Widget build(BuildContext context) => SessionToolbar(
    key: _toolbarKey,
    popups: _popups,
    title: widget.title,
    modeLabel: '스트리밍',
    connected: widget.connected,
    latencyMs: widget.connected ? widget.latencyMs : null,
    fullscreen: widget.shown != SessionDisplayMode.windowed,
    gameMouse: widget.gameMouse,
    pointerHeld: widget.pointerHeld,
    touch: widget.touch,
    onInteract: () => _act('interact'),
    onMinimize: widget.windowButtons ? () => _act('minimize') : null,
    onRestore: widget.windowButtons ? () => _act('restore') : null,
    onDisconnect: () => _act('disconnect'),
    onQualityAt: _connection,
    onBounds: (bounds) => _bounds = bounds,
    actions: _actions(),
  );
}

// ---------------------------------------------------------------- renders

/// The shipped Pretendard and Flutter's Material icon font, loaded as in
/// windows_session_toolbar_visual_test.dart (a widget test otherwise draws
/// every glyph as a box).

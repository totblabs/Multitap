import 'dart:async';

import 'package:flutter/material.dart';

import '../data/remote_session.dart';
import 'session_menu.dart';
import '../l10n/l10n.dart';

/// The session bar's one vocabulary for Windows and Android: every
/// entry either platform shows, in the order both show it, with one name,
/// one icon and its group. Windows lays the groups out along its bar
/// (연결 | 화면 · 폴더·USB · 설정 | 창 버튼 | 연결 끊기) and opens each group
/// as a panel; Android lists the same groups down the amber ⋯ menu as rows
/// (연결 품질, 화면, 설정, 연결 끊기) and opens the same panels in the menu's
/// place. A platform leaves out what it cannot do
/// (test/session_tools_parity_test). Android adds nothing of its own: no
/// 클립보드 or 키 조합 entry, as the PC has none.
enum SessionTool {
  /// The quality chip: the measured latency, its values and the statistics.
  quality(Icons.network_check_rounded, 0),

  /// 화면: the screen mode and, on Windows, 배율 in the same panel.
  screen(Icons.fullscreen_rounded, 1),
  scale(Icons.text_fields_rounded, 1),

  /// 폴더·USB: the Windows bar's one button for both; its panel has a tab
  /// for each.
  share(Icons.folder_shared_rounded, 2),
  drives(Icons.folder_shared_rounded, 2),
  usb(Icons.usb_rounded, 2),

  /// 설정: the Windows bar's button for what is used now and then.
  more(Icons.settings_rounded, 3),

  /// 화질·소리: the streaming choices (frame rate, bitrate, sound), applied
  /// by reconnecting. The former 설정.
  settings(Icons.tune_rounded, 3),
  // 마우스 잠금: the only mouse lock (the identifier keeps its first name).
  gameMouse(Icons.mouse_rounded, 3),

  /// 바 열어 두기: the former pin.
  pin(Icons.push_pin_outlined, 3),
  guide(Icons.help_outline_rounded, 3),

  /// The window's own buttons, on the bar only in full screen (no title bar).
  minimize(Icons.remove_rounded, 4),
  restore(Icons.filter_none_rounded, 4),
  disconnect(Icons.logout_rounded, 5);

  const SessionTool(this.icon, this.group);
  String get label => switch (this) {
    SessionTool.quality => l10n.connectionQuality,
    SessionTool.screen => l10n.screen,
    SessionTool.scale => l10n.scale,
    SessionTool.share => l10n.foldersUsb,
    SessionTool.drives => l10n.sharedFolder,
    SessionTool.usb => l10n.usbDevices,
    SessionTool.more => l10n.settings,
    SessionTool.settings => l10n.pictureQuality,
    SessionTool.gameMouse => l10n.mouseLock,
    SessionTool.pin => l10n.keepToolbarOpen,
    SessionTool.guide => l10n.guide,
    SessionTool.minimize => l10n.minimize2,
    SessionTool.restore => l10n.windowed2,
    SessionTool.disconnect => l10n.disconnect,
  };

  final IconData icon;

  /// Both platforms use the same menu names; panels explain each setting.
  String get androidLabel => label;

  /// Entries of one group sit together; a caption (Android, the narrow
  /// Windows menu) or a divider (the Windows bar) separates the groups.
  final int group;

  /// The caption over a group's entries (null: the group has none).
  static String? caption(int group) => switch (group) {
    1 => l10n.screen,
    2 => l10n.share,
    3 => l10n.settings,
    _ => null,
  };

  /// The entry labelled [label], or null for one outside this vocabulary.
  static SessionTool? named(String label) {
    for (final tool in values) {
      if (tool.label == label || tool.androidLabel == label) return tool;
    }
    return null;
  }

  /// [entries] in the toolbar's order; entries outside the vocabulary keep
  /// their own order after the known ones.
  static List<T> ordered<T>(List<T> entries, String Function(T) label) {
    int rank(T entry) => named(label(entry))?.index ?? values.length;
    final indexed = entries.indexed.toList()
      ..sort((a, b) {
        final order = rank(a.$2).compareTo(rank(b.$2));
        return order == 0 ? a.$1.compareTo(b.$1) : order;
      });
    return [for (final (_, entry) in indexed) entry];
  }
}

/// The quality chip's rows after the round trip, on both platforms: what
/// the Host and [device] ('PC', '이 기기') measured, never invented, then
/// the full statistics.
List<SessionMenuItem> sessionQualityItems(
  RemoteSession? session, {
  required String device,
  required VoidCallback onStats,
  Color? statsColor,
  List<SessionMenuItem> actions = const [],
}) {
  final values = sessionQualityValues(session, device: device);
  return [
    for (final value in values) SessionMenuItem.info(value.label, value.value),
    ...actions,
    SessionMenuItem(
      l10n.showConnectionStats,
      key: const Key('quality-stats'),
      icon: Icons.insights_rounded,
      color: statsColor,
      onSelected: onStats,
    ),
  ];
}

/// One measured value of the connection: what it measures and what was
/// measured ('—' when nothing was).
typedef SessionQualityValue = ({String id, String label, String value});

/// The measured values behind the quality chip, in the order both platforms
/// list them: resolution, frames, received rate, the video path, then the
/// Host's and [device]'s processing times.
List<SessionQualityValue> sessionQualityValues(
  RemoteSession? session, {
  required String device,
}) {
  final active = session != null && !session.closed && !session.resizing;
  String measured(num? value, String unit, [int decimals = 1]) =>
      value != null && value.isFinite && value >= 0
      ? '${value.toStringAsFixed(decimals)} $unit'
      : '—';
  String size(int? width, int? height) =>
      width == null || height == null || width <= 0 || height <= 0
      ? '—'
      : '$width × $height';
  final pipeline = session?.hostTelemetryFresh == true
      ? session?.hostTelemetry?.pipeline
      : null;
  final tracks = session?.tracks ?? const [];
  return [
    // The remote Windows size the viewport actually produced.
    (
      id: 'resolution',
      label: l10n.resolutionRemoteScreen,
      value: !active
          ? '—'
          : session.independentOutputs && tracks.isNotEmpty
          ? tracks.map((track) => size(track.width, track.height)).join(' + ')
          : size(
              session.sourceWidth ?? session.width,
              session.sourceHeight ?? session.height,
            ),
    ),
    (
      id: 'fps',
      label: l10n.screenDecoding(device),
      value: measured(active ? session.decodedFps : null, 'fps'),
    ),
    (
      id: 'rate',
      label: l10n.receivedIncomingVideo,
      value: measured(active ? session.receivedMbps : null, 'Mbps'),
    ),
    (id: 'path', label: l10n.videoPath, value: session?.videoPathLabel ?? l10n.checking),
    (
      id: 'encode',
      label: l10n.encodingHost,
      value: measured(pipeline?.encodeMs, 'ms', 2),
    ),
    (
      id: 'decode',
      label: l10n.decoding(device),
      value: measured(active ? session.clientDecodeMs : null, 'ms', 2),
    ),
  ];
}

/// Whether the latency has stayed poor: true only once a round trip of
/// [SessionChrome.fairMs] or more has lasted [hold], and false again only
/// once better has lasted as long, so one spike or one good sample changes
/// nothing. The 연결 panel's advice (절약으로 다시 연결) follows it on both
/// platforms; the Windows bar keeps the same rule for its handle.
class SessionPoorLatency {
  SessionPoorLatency(this.onChanged);

  static const hold = Duration(seconds: 5);

  /// Called when [sustained] changed after its hold.
  final VoidCallback onChanged;
  bool _sustained = false, _now = false;
  Timer? _timer;

  bool get sustained => _sustained;

  /// The newest measurement ([ms] null or not positive: none).
  void update({required bool connected, required double? ms}) {
    if (!connected) {
      _timer?.cancel();
      _timer = null;
      _sustained = _now = false;
      return;
    }
    _now = ms != null && ms.isFinite && ms >= SessionChrome.fairMs;
    if (_now == _sustained) {
      _timer?.cancel();
      _timer = null;
      return;
    }
    _timer ??= Timer(hold, () {
      _timer = null;
      if (_now == _sustained) return;
      _sustained = _now;
      onChanged();
    });
  }

  void dispose() {
    _timer?.cancel();
    _timer = null;
  }
}

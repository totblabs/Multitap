import 'package:flutter/material.dart';

import '../data/decode_settings.dart';
import '../models/desktop_machine.dart';
import '../models/stream_options.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_menu.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import 'session_menu.dart' show SessionPopups;
import '../l10n/l10n.dart';

/// The streaming choices: target frame rate, video bitrate (automatic or
/// manual) and the automatic-resolution note. Sound is always on, on the PC
/// and on Android, with no switch; the system's
/// own volume stays the way to quiet it. Which virtual monitor
/// is shown is not a choice: the screen mode sets it (one
/// output for a window or full screen, both for 모든 모니터 사용), so there
/// is no selector and the request keeps the monitor it was opened with.
class StreamSettingsBody extends StatefulWidget {
  const StreamSettingsBody({
    super.key,
    required this.machine,
    required this.initialOptions,
    required this.onChanged,
    this.audioDeviceMissing = false,
    this.hardwareDecode = true,
    this.onHardwareDecodeChanged,
  });
  final DesktopMachine machine;
  final StreamOptions initialOptions;

  /// The "하드웨어 디코딩" choice shown (DecodeSettings), and its change. The
  /// choice is a client preference, not part of the stream request, so it does
  /// not travel in [onChanged]; it applies from the next connection.
  final bool hardwareDecode;
  final ValueChanged<bool>? onHardwareDecodeChanged;

  /// The Host reported that the desktop has no playback device: only then
  /// the one line saying so shows (no action: the administrator adds the
  /// device in Proxmox, 2026-11-10).
  final bool audioDeviceMissing;

  /// The request the current choices make, after every change.
  final ValueChanged<StreamOptions> onChanged;

  /// The request for [machine] before anything was changed.
  static StreamOptions initialRequest(
    DesktopMachine machine,
    StreamOptions initial,
  ) => _request(
    machine,
    initial.forMonitors(machine.monitors),
    fps: initial.fps,
    mbps: _manualMbps(machine, initial),
    auto: initial.automaticBitrate,
  );

  static int _maxMbps(DesktopMachine machine) =>
      machine.bitrateCeiling ~/ 1000000;
  static int _manualMbps(DesktopMachine machine, StreamOptions options) =>
      (options.bitrate ~/ 1000000).clamp(4, _maxMbps(machine));

  static StreamOptions _request(
    DesktopMachine machine,
    StreamOptions base, {
    required int fps,
    required int mbps,
    required bool auto,
  }) => StreamOptions(
    // This is a codec safety ceiling. Actual Windows output size follows the
    // active viewport through the authenticated resize protocol.
    maxWidth: StreamOptions.codecMaxWidth,
    maxHeight: StreamOptions.codecMaxHeight,
    fps: fps,
    bitrate: mbps * 1000000,
    // Always sound (2026-11-11): an earlier 소리 끔 choice is not carried on.
    audio: true,
    monitor: base.monitor,
    bitrateMode: auto ? StreamBitrateMode.auto : StreamBitrateMode.fixed,
  );

  /// Whether [a] and [b] ask for the same stream (the user-facing choices).
  static bool sameChoices(StreamOptions a, StreamOptions b) =>
      a.fps == b.fps &&
      a.bitrateMode == b.bitrateMode &&
      (a.automaticBitrate || a.bitrate == b.bitrate);

  @override
  State<StreamSettingsBody> createState() => _StreamSettingsBodyState();
}

class _StreamSettingsBodyState extends State<StreamSettingsBody> {
  AppPalette get _palette => AppPalette.of(context);
  // One label style for every setting row, in full ink (a muted label read
  // as a disabled setting).
  TextStyle get _label => uiText(
    size: 13.5,
    weight: FontWeight.w600,
    height: 1.4,
    color: _palette.ink,
  );
  late final StreamOptions _base = widget.initialOptions.forMonitors(
    widget.machine.monitors,
  );
  late int _fps = widget.initialOptions.fps;
  late int _mbps = StreamSettingsBody._manualMbps(
    widget.machine,
    widget.initialOptions,
  );
  late bool _hardware = widget.hardwareDecode;
  bool _advanced = false;
  // Automatic rate (bitrate-adaptive-v1). Offered only where the broker and
  // host support it; elsewhere the preference stays and the manual slider
  // keeps the released 4-20 Mbps.
  late bool _auto = widget.initialOptions.automaticBitrate;
  bool get _adaptive => widget.machine.maxBitrate != null;
  int get _maxMbps => StreamSettingsBody._maxMbps(widget.machine);
  bool get _showAuto => _auto && _adaptive;
  // Automatic preference on a released desktop: the request becomes 20 Mbps.
  int get _shownMbps =>
      _auto ? StreamOptions.legacyMaximumBitrate ~/ 1000000 : _mbps;

  StreamOptions get _request => StreamSettingsBody._request(
    widget.machine,
    _base,
    fps: _fps,
    mbps: _mbps,
    auto: _auto,
  );

  void _update(VoidCallback change) {
    setState(change);
    widget.onChanged(_request);
  }

  // A mode sets the frame rate and the rate below; changing either of them
  // afterwards reads 사용자 설정.
  void _choose(StreamPreset preset) => _update(() {
    _fps = preset.options.fps;
    _auto = preset.options.automaticBitrate;
    if (!_auto) _mbps = preset.options.bitrate ~/ 1000000;
  });

  @override
  Widget build(BuildContext context) {
    final p = _palette;
    const gap = SizedBox(height: 16);
    final choiceStyle = ButtonStyle(
      minimumSize: const WidgetStatePropertyAll(Size(0, 40)),
      padding: const WidgetStatePropertyAll(
        EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      ),
      textStyle: WidgetStatePropertyAll(
        uiText(size: 13, weight: FontWeight.w500),
      ),
      shape: WidgetStatePropertyAll(
        RoundedRectangleBorder(borderRadius: BorderRadius.circular(10)),
      ),
    );
    final preset = StreamPreset.of(_request, widget.machine.maxBitrate);
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Text.rich(
          key: const Key('stream-preset-label'),
          TextSpan(
            text: l10n.qualityMode2,
            children: [
              TextSpan(
                text: '  ${preset?.label ?? l10n.custom}',
                style: TextStyle(color: p.amberInk),
              ),
            ],
          ),
          style: _label,
        ),
        const SizedBox(height: 8),
        SegmentedButton<StreamPreset>(
          key: const Key('stream-preset'),
          style: choiceStyle,
          expandedInsets: EdgeInsets.zero,
          showSelectedIcon: false,
          emptySelectionAllowed: true,
          segments: [
            for (final mode in StreamPreset.values)
              ButtonSegment(
                value: mode,
                tooltip: mode.detail,
                label: KeepAllText(
                  mode.label,
                  key: Key('stream-preset-${mode.name}'),
                ),
              ),
          ],
          selected: {?preset},
          // A click on the chosen mode asks for no mode: it stays chosen.
          onSelectionChanged: (values) {
            if (values.isNotEmpty) _choose(values.single);
          },
        ),
        const SizedBox(height: 12),
        KeepAllText(
          '$_fps fps · ${_showAuto ? l10n.autoBitrateMaxMbps(_maxMbps) : '$_shownMbps Mbps'}',
          key: const Key('stream-current-summary'),
          style: uiText(size: 13, color: p.muted),
        ),
        const SizedBox(height: 6),
        // The Host can only send a device Windows has. One plain line, no
        // action: the administrator adds the device in Proxmox.
        if (widget.audioDeviceMissing)
          Padding(
            padding: const EdgeInsets.only(left: 2, top: 4, bottom: 4),
            child: KeepAllText(
              audioDeviceMissingText,
              key: const Key('stream-audio-hint'),
              style: uiText(size: 12, height: 1.5, color: p.muted),
            ),
          ),
        AppDisclosure(
          headerKey: const Key('stream-advanced'),
          label: l10n.advancedSettings,
          open: _advanced,
          onChanged: (open) => setState(() => _advanced = open),
        ),
        if (_advanced) ...[
          gap,
          KeepAllText(
            l10n.targetFrameRate,
            key: const Key('stream-fps-label'),
            style: _label,
          ),
          const SizedBox(height: 8),
          SegmentedButton<int>(
            style: choiceStyle,
            expandedInsets: EdgeInsets.zero,
            showSelectedIcon: false,
            segments: const [
              ButtonSegment(value: 30, label: KeepAllText('30 fps')),
              ButtonSegment(value: 60, label: KeepAllText('60 fps')),
            ],
            selected: {_fps},
            onSelectionChanged: (values) => _update(() => _fps = values.single),
          ),
          gap,
          Text.rich(
            key: const Key('stream-bitrate-label'),
            TextSpan(
              text: l10n.videoBitrate,
              children: [
                TextSpan(
                  text: _showAuto ? l10n.auto3 : '  $_shownMbps Mbps',
                  style: TextStyle(color: p.amberInk),
                ),
              ],
            ),
            style: _label,
          ),
          if (_adaptive) ...[
            const SizedBox(height: 8),
            SegmentedButton<bool>(
              key: const Key('stream-bitrate-mode'),
              style: choiceStyle,
              expandedInsets: EdgeInsets.zero,
              showSelectedIcon: false,
              segments: [
                ButtonSegment(value: true, label: KeepAllText(l10n.autoRecommended)),
                ButtonSegment(value: false, label: KeepAllText(l10n.manual2)),
              ],
              selected: {_auto},
              onSelectionChanged: (values) =>
                  _update(() => _auto = values.single),
            ),
          ],
          if (_showAuto)
            Padding(
              key: const Key('stream-bitrate-auto-note'),
              padding: const EdgeInsets.fromLTRB(2, 8, 2, 4),
              child: KeepAllText(
                l10n.adjustsBitrateConnectionMaxMbps(_maxMbps),
                style: uiText(size: 12, height: 1.45, color: p.muted),
              ),
            )
          else
            SliderTheme(
              data: SliderTheme.of(context).copyWith(
                overlayShape: const RoundSliderOverlayShape(overlayRadius: 16),
              ),
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 2),
                child: Slider(
                  key: const Key('stream-bitrate'),
                  value: _shownMbps.toDouble(),
                  min: 4,
                  max: _maxMbps.toDouble(),
                  divisions: _maxMbps - 4,
                  label: '$_shownMbps Mbps',
                  padding: const EdgeInsets.symmetric(
                    vertical: 14,
                    horizontal: 8,
                  ),
                  onChanged: (value) => _update(() {
                    _mbps = value.round();
                    _auto = false;
                  }),
                ),
              ),
            ),
          if (_adaptive && !_auto)
            Padding(
              padding: const EdgeInsets.only(left: 2, bottom: 4),
              child: KeepAllText(
                l10n.ifNetworkCantKeepUp,
                style: uiText(size: 12, color: p.muted),
              ),
            ),
          gap,
          Row(
            children: [
              Expanded(
                child: KeepAllText(
                  l10n.hardwareDecoding,
                  key: const Key('stream-hardware-label'),
                  style: _label,
                ),
              ),
              Switch(
                key: const Key('stream-hardware-decode'),
                value: _hardware,
                onChanged: (value) {
                  setState(() => _hardware = value);
                  widget.onHardwareDecodeChanged?.call(value);
                },
              ),
            ],
          ),
          Padding(
            padding: const EdgeInsets.only(left: 2, bottom: 4),
            child: KeepAllText(
              _hardware
                  ? l10n.decodesGpuFallsBackCpu
                  : l10n.decodesCpuOnlyTryIf,
              key: const Key('stream-hardware-note'),
              style: uiText(size: 12, height: 1.5, color: p.muted),
            ),
          ),
        ],
        gap,
        KeepAllText(
          _base.monitor == 'all'
              ? l10n.eachMonitorMatchesItsOwn
              : l10n.remoteResolutionFollowsWindowSize,
          key: const Key('stream-dynamic-resolution'),
          style: uiText(size: 12, height: 1.45, color: p.muted),
        ),
      ],
    );
  }
}

/// The settings as a dialog: the launcher's "연결 전 설정" and the session
/// panel's fallback when the window is too small for the panel.
class StreamSettingsDialog extends StatefulWidget {
  const StreamSettingsDialog({
    super.key,
    required this.machine,
    this.initialOptions = StreamOptions.recommended,
    this.reconnect = false,
    this.allowUnchangedReconnect = false,
    this.audioDeviceMissing = false,
  });
  final DesktopMachine machine;
  final StreamOptions initialOptions;
  final bool reconnect, audioDeviceMissing;

  /// A completed display change already closed its connection. The user's
  /// explicit reconnect can keep the same quality choices in that state.
  final bool allowUnchangedReconnect;
  @override
  State<StreamSettingsDialog> createState() => _StreamSettingsDialogState();
}

class _StreamSettingsDialogState extends State<StreamSettingsDialog> {
  late final StreamOptions _initial = StreamSettingsBody.initialRequest(
    widget.machine,
    widget.initialOptions,
  );
  late StreamOptions _options = _initial;
  final bool _initialHardware = DecodeSettings.hardwareDecode;
  late bool _hardware = _initialHardware;

  bool get _changed =>
      !StreamSettingsBody.sameChoices(_options, _initial) ||
      _hardware != _initialHardware;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return AppDialog(
      icon: Icons.tune_rounded,
      title: widget.reconnect ? l10n.pictureQuality : l10n.streamingSettings,
      subtitle: widget.machine.name,
      maxWidth: 440,
      body: [
        StreamSettingsBody(
          machine: widget.machine,
          initialOptions: widget.initialOptions,
          audioDeviceMissing: widget.audioDeviceMissing,
          hardwareDecode: _hardware,
          onHardwareDecodeChanged: (value) => _hardware = value,
          onChanged: (options) => setState(() => _options = options),
        ),
      ],
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          style: AppButtons.quiet(p),
          child: KeepAllText(l10n.cancel),
        ),
        FilledButton(
          key: const Key('apply-stream-settings'),
          autofocus: true,
          onPressed:
              widget.reconnect && !widget.allowUnchangedReconnect && !_changed
              ? null
              : () {
                  if (_hardware != DecodeSettings.hardwareDecode) {
                    DecodeSettings.choose(_hardware);
                  }
                  Navigator.pop(context, _options);
                },
          style: AppButtons.filled(p),
          child: KeepAllText(widget.reconnect ? l10n.reconnect : l10n.connect2),
        ),
      ],
    );
  }
}

/// The panel's width and the room it needs below its button; a window with
/// less room shows the dialog instead.
const streamSettingsPanelWidth = 360.0, streamSettingsPanelMinHeight = 300.0;

/// Opens the in-session settings as a panel that drops down from the session
/// bar's button at [anchor] (2026-10-26: a combo-style panel, not a centred
/// dialog). It closes on an outside click, Escape, or when another menu or
/// dialog opens; 재연결 applies the changed choices (it stays off until
/// something changed). Falls back to [StreamSettingsDialog] when the panel
/// cannot fit, e.g. a narrow window. Resolves to the options to reconnect
/// with, or null. [popups] tracks the panel with the bar's menus, so a
/// change under it (a resize, another screen mode, the end of the
/// connection) closes it without applying anything.
Future<StreamOptions?> showStreamSettingsPanel(
  BuildContext context, {
  required Rect anchor,
  required DesktopMachine machine,
  required StreamOptions initialOptions,
  bool audioDeviceMissing = false,
  SessionPopups? popups,
  VoidCallback? onBack,
}) {
  final navigator = Navigator.of(context);
  final overlay = navigator.overlay!.context;
  final media = MediaQuery.of(overlay);
  final area = AnchoredPanelRoute.areaFor(media, anchor);
  final fits =
      area.width >= streamSettingsPanelWidth + 2 * AnchoredPanelRoute.margin &&
      area.bottom - anchor.bottom - AnchoredPanelRoute.margin >=
          streamSettingsPanelMinHeight;
  if (!fits) {
    final track = popups?.tracker();
    return showDialog<StreamOptions>(
      context: context,
      builder: (_) {
        final panel = StreamSettingsDialog(
          machine: machine,
          initialOptions: initialOptions,
          reconnect: true,
          audioDeviceMissing: audioDeviceMissing,
        );
        return track == null ? panel : track(panel);
      },
    );
  }
  final track = popups?.tracker();
  return navigator.push(
    AnchoredPanelRoute<StreamOptions>(
      anchor: anchor,
      reduceMotion: MediaQuery.disableAnimationsOf(context),
      width: streamSettingsPanelWidth,
      label: l10n.pictureQuality,
      // Dark like the session bar it drops from, in either app appearance.
      theme: buildAppTheme(brightness: Brightness.dark),
      builder: (context) {
        final panel = _StreamSettingsPanel(
          machine: machine,
          initialOptions: initialOptions,
          audioDeviceMissing: audioDeviceMissing,
          onBack: onBack,
        );
        return track == null ? panel : track(panel);
      },
    ),
  );
}

class _StreamSettingsPanel extends StatefulWidget {
  const _StreamSettingsPanel({
    required this.machine,
    required this.initialOptions,
    required this.audioDeviceMissing,
    this.onBack,
  });
  final DesktopMachine machine;
  final StreamOptions initialOptions;
  final bool audioDeviceMissing;

  /// Opened from the 설정 panel: a back button returns there, applying
  /// nothing.
  final VoidCallback? onBack;

  @override
  State<_StreamSettingsPanel> createState() => _StreamSettingsPanelState();
}

class _StreamSettingsPanelState extends State<_StreamSettingsPanel> {
  late final StreamOptions _initial = StreamSettingsBody.initialRequest(
    widget.machine,
    widget.initialOptions,
  );
  late StreamOptions _options = _initial;
  final bool _initialHardware = DecodeSettings.hardwareDecode;
  late bool _hardware = _initialHardware;

  bool get _changed =>
      !StreamSettingsBody.sameChoices(_options, _initial) ||
      _hardware != _initialHardware;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return Column(
      key: const Key('stream-settings-panel'),
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Padding(
          padding: EdgeInsets.fromLTRB(
            widget.onBack == null ? 16 : 8,
            widget.onBack == null ? 16 : 10,
            16,
            0,
          ),
          child: Row(
            children: [
              if (widget.onBack == null)
                Icon(Icons.tune_rounded, size: 18, color: p.muted)
              else
                IconButton(
                  key: const Key('stream-settings-back'),
                  tooltip: l10n.backSettings,
                  visualDensity: VisualDensity.compact,
                  onPressed: () {
                    widget.onBack!();
                    Navigator.pop(context);
                  },
                  icon: const Icon(Icons.chevron_left_rounded, size: 20),
                ),
              SizedBox(width: widget.onBack == null ? 10 : 2),
              Expanded(
                child: KeepAllText(
                  l10n.pictureQuality,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: uiText(
                    size: 14,
                    weight: FontWeight.w600,
                    color: p.ink,
                  ),
                ),
              ),
            ],
          ),
        ),
        Flexible(
          child: SingleChildScrollView(
            padding: const EdgeInsets.fromLTRB(16, 16, 16, 4),
            child: StreamSettingsBody(
              machine: widget.machine,
              initialOptions: widget.initialOptions,
              audioDeviceMissing: widget.audioDeviceMissing,
              hardwareDecode: _hardware,
              onHardwareDecodeChanged: (value) =>
                  setState(() => _hardware = value),
              onChanged: (options) => setState(() => _options = options),
            ),
          ),
        ),
        // The shared button bar: side by side, stacked when large text
        // would wrap a label.
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 12, 16, 16),
          child: AppDialogActions(
            children: [
              TextButton(
                key: const Key('cancel-stream-settings'),
                onPressed: () => Navigator.pop(context),
                style: AppButtons.quiet(p),
                child: KeepAllText(l10n.cancel),
              ),
              Tooltip(
                message: _changed ? l10n.reconnectChangedSettings : l10n.noSettingsChanged,
                child: FilledButton(
                  key: const Key('apply-stream-settings'),
                  onPressed: _changed
                      ? () {
                          if (_hardware != _initialHardware) {
                            DecodeSettings.choose(_hardware);
                          }
                          Navigator.pop(context, _options);
                        }
                      : null,
                  style: AppButtons.filled(p),
                  child: KeepAllText(l10n.reconnect),
                ),
              ),
            ],
          ),
        ),
      ],
    );
  }
}

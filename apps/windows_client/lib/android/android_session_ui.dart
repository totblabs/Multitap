import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/display_scale.dart';
import '../models/session_guide.dart';
import '../models/stream_options.dart';
import '../ui/korean_text.dart';
import '../ui/session_menu.dart' show SessionChrome;
import '../ui/session_tools.dart';
import 'android_keyboard.dart';
import 'android_screen.dart';
import 'android_sharp_picture.dart' show androidSharpDetail, androidSharpHint;
import 'android_store.dart' show AndroidStreamQuality;
import 'android_toolbar.dart';
import 'android_touch.dart';
import '../l10n/l10n.dart';

TextStyle _text(
  double size, {
  FontWeight weight = FontWeight.w500,
  Color? color,
  double? height,
}) => TextStyle(
  fontFamily: 'Pretendard',
  fontSize: size,
  fontWeight: weight,
  color: color,
  height: height,
  fontFeatures: const [FontFeature.tabularFigures()],
);

/// Windows modifier keys the key row can hold for the next key.
enum AndroidModifier {
  ctrl('Ctrl', 0xa2),
  alt('Alt', 0xa4),
  shift('Shift', 0xa0),
  win('Win', 0x5b);

  const AndroidModifier(this.label, this.vk);
  final String label;
  final int vk;
}

/// The row of PC keys above the phone keyboard (ToTBDesk's accessory row,
/// WINDOWS-INPUT-2.3.0): Ctrl, Alt and Shift stay at the left when space allows
/// (on a narrow screen they join the scrolling keys). A tap holds
/// one for the next key, a long press keeps it until tapped again; held
/// keys are lit, kept ones filled), then a scrolling strip of Esc, Tab,
/// arrows, Win, 한/영 and editing keys, and Fn switching the strip to F1–F12
/// and the navigation keys. Arrows and Delete repeat while held. Every key
/// is an ordinary down/up pair; nothing stays pressed on the remote.
/// Ctrl+Alt+Del is not sent; its key explains why ([onSecure]).
/// Usable by any remote view: it only reports keys.
class AndroidKeyRow extends StatefulWidget {
  const AndroidKeyRow({
    super.key,
    required this.onKey,
    required this.onModifier,
    required this.onHide,
    this.onSecure,
    this.latched = const {},
    this.locked = const {},
  });
  final ValueChanged<AndroidKeyStroke> onKey;

  /// Toggle (tap) or lock (long press) a modifier.
  final void Function(AndroidModifier modifier, {required bool lock})
  onModifier;
  final VoidCallback onHide;

  /// The Ctrl+Alt+Del key: explain why it is not sent.
  final VoidCallback? onSecure;
  final Set<AndroidModifier> latched, locked;

  static const height = 48.0;

  @override
  State<AndroidKeyRow> createState() => _AndroidKeyRowState();
}

class _AndroidKeyRowState extends State<AndroidKeyRow> {
  bool _fn = false;
  Timer? _repeat;

  @override
  void dispose() {
    _repeat?.cancel();
    super.dispose();
  }

  void _stopRepeat() {
    _repeat?.cancel();
    _repeat = null;
  }

  void _press(AndroidKeyStroke stroke, {bool repeat = false}) {
    unawaited(HapticFeedback.selectionClick());
    widget.onKey(stroke);
    if (!repeat) return;
    _stopRepeat();
    _repeat = Timer(const Duration(milliseconds: 450), () {
      _repeat = Timer.periodic(
        const Duration(milliseconds: 70),
        (_) => widget.onKey(stroke),
      );
    });
  }

  Widget _key(
    SessionChrome chrome, {
    required String id,
    String? label,
    IconData? icon,
    VoidCallback? onTap,
    VoidCallback? onLongPress,
    AndroidKeyStroke? repeat,
    bool active = false,
    bool locked = false,
    bool muted = false,
    double width = 50,
    String? tooltip,
  }) {
    final fg = locked
        ? const Color(0xFF241600)
        : active
        ? chrome.accent
        : muted
        ? chrome.faint
        : chrome.ink;
    // Larger text widens the caps (the strip scrolls); a label that still
    // does not fit shrinks rather than being cut.
    final scale = MediaQuery.textScalerOf(context).scale(1).clamp(1.0, 1.4);
    final content = icon != null
        ? Icon(
            icon,
            size: 19,
            color: locked ? fg : (active ? chrome.accent : chrome.icon),
          )
        : FittedBox(
            fit: BoxFit.scaleDown,
            child: Text(
              label!,
              maxLines: 1,
              style: _text(12.5, weight: FontWeight.w600, color: fg),
            ),
          );
    return Semantics(
      button: true,
      selected: active || locked,
      label: tooltip ?? label,
      excludeSemantics: true,
      child: _KeyCap(
        inkKey: Key('android-key-$id'),
        width: icon == null ? width * scale : width,
        fill: locked
            ? chrome.accent
            : active
            ? chrome.accent.withValues(alpha: .16)
            : const Color(0xFF22282D),
        edge: locked || active
            ? chrome.accent.withValues(alpha: .8)
            : const Color(0x14FFFFFF),
        pressed: chrome.pressed,
        onTap: repeat == null ? onTap : null,
        onTapDown: repeat == null ? null : (_) => _press(repeat, repeat: true),
        onTapUp: repeat == null ? null : (_) => _stopRepeat(),
        onTapCancel: repeat == null ? null : _stopRepeat,
        onLongPress: onLongPress,
        child: content,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    AndroidKeyStroke vk(int code) =>
        AndroidKeyStroke(code, extended: androidExtendedKeys.contains(code));
    Widget plain(String id, String label, int code, {double width = 50}) =>
        _key(
          chrome,
          id: id,
          label: label,
          width: width,
          onTap: () => _press(vk(code)),
        );
    Widget repeating(
      String id,
      int code, {
      String? label,
      IconData? icon,
      String? tooltip,
    }) => _key(
      chrome,
      id: id,
      label: label,
      icon: icon,
      tooltip: tooltip,
      repeat: vk(code),
    );
    Widget modifier(AndroidModifier m) => _key(
      chrome,
      id: m.name,
      label: m.label,
      width: m == AndroidModifier.shift ? 54 : 48,
      tooltip: widget.locked.contains(m)
          ? l10n.lockedTapRelease(m.label)
          : widget.latched.contains(m)
          ? l10n.pressedCombinesNextKey(m.label)
          : l10n.longPressLock(m.label),
      active: widget.latched.contains(m),
      locked: widget.locked.contains(m),
      onTap: () {
        unawaited(HapticFeedback.selectionClick());
        widget.onModifier(m, lock: false);
      },
      onLongPress: () {
        unawaited(HapticFeedback.mediumImpact());
        widget.onModifier(m, lock: true);
      },
    );
    final keys = _fn
        ? [
            for (var i = 1; i <= 12; i++)
              plain('f$i', 'F$i', 0x6f + i, width: 44),
            plain('insert', 'Ins', 0x2d, width: 46),
            repeating('delete', 0x2e, label: 'Del'),
            plain('home', 'Home', 0x24, width: 56),
            plain('end', 'End', 0x23),
            plain('pageUp', 'PgUp', 0x21, width: 54),
            plain('pageDown', 'PgDn', 0x22, width: 54),
          ]
        : [
            plain('escape', 'Esc', 0x1b),
            plain('tab', 'Tab', 0x09),
            repeating(
              'left',
              0x25,
              icon: Icons.arrow_back_rounded,
              tooltip: '←',
            ),
            repeating(
              'up',
              0x26,
              icon: Icons.arrow_upward_rounded,
              tooltip: '↑',
            ),
            repeating(
              'down',
              0x28,
              icon: Icons.arrow_downward_rounded,
              tooltip: '↓',
            ),
            repeating(
              'right',
              0x27,
              icon: Icons.arrow_forward_rounded,
              tooltip: '→',
            ),
            _key(
              chrome,
              id: 'win',
              icon: Icons.window_rounded,
              tooltip: l10n.windowsKey,
              onTap: () => _press(vk(0x5b)),
            ),
            plain('hangul', l10n.hanEng, 0x15, width: 54),
            repeating('delete', 0x2e, label: 'Del'),
            plain('home', 'Home', 0x24, width: 56),
            plain('end', 'End', 0x23),
            _key(
              chrome,
              id: 'secure',
              label: 'Ctrl+Alt+Del',
              width: 96,
              muted: true,
              tooltip: l10n.ctrlAltDelCantSent2,
              onTap: widget.onSecure,
            ),
          ];
    return Material(
      key: const Key('android-key-row'),
      color: chrome.surface,
      child: Container(
        height: AndroidKeyRow.height,
        decoration: BoxDecoration(
          border: Border(top: BorderSide(color: chrome.edge)),
        ),
        child: LayoutBuilder(
          builder: (context, constraints) {
            // Keep modifiers fixed when there is room for the key strip. On a
            // narrow split view they scroll with the keys; Fn and Hide stay put.
            final compact =
                constraints.maxWidth <
                320 * MediaQuery.textScalerOf(context).scale(1).clamp(1.0, 1.4);
            final modifiers = <Widget>[
              for (final m in const [
                AndroidModifier.ctrl,
                AndroidModifier.alt,
                AndroidModifier.shift,
              ])
                modifier(m),
              Container(width: 1, height: 26, color: chrome.edge),
            ];
            return Row(
              children: [
                const SizedBox(width: 2),
                if (!compact) ...modifiers,
                Expanded(
                  child: ListView(
                    key: const Key('android-key-row-scroll'),
                    scrollDirection: Axis.horizontal,
                    padding: const EdgeInsets.symmetric(horizontal: 4),
                    children: [if (compact) ...modifiers, ...keys],
                  ),
                ),
                Container(width: 1, height: 26, color: chrome.edge),
                _key(
                  chrome,
                  id: 'fn',
                  label: 'Fn',
                  width: 42,
                  active: _fn,
                  tooltip: _fn ? l10n.basicKeys : l10n.f1F12NavigationKeys,
                  onTap: () {
                    unawaited(HapticFeedback.selectionClick());
                    setState(() => _fn = !_fn);
                  },
                ),
                _key(
                  chrome,
                  id: 'hide',
                  icon: Icons.keyboard_hide_rounded,
                  width: 42,
                  tooltip: l10n.closeKeyboard,
                  onTap: widget.onHide,
                ),
                const SizedBox(width: 2),
              ],
            );
          },
        ),
      ),
    );
  }
}

/// One key of [AndroidKeyRow]: a 38 dp cap drawn inside the row's whole
/// 48 dp height, which all takes the touch (a finger just above or below
/// the cap still presses it).
class _KeyCap extends StatefulWidget {
  const _KeyCap({
    required this.inkKey,
    required this.width,
    required this.fill,
    required this.edge,
    required this.pressed,
    required this.child,
    this.onTap,
    this.onTapDown,
    this.onTapUp,
    this.onTapCancel,
    this.onLongPress,
  });
  final Key inkKey;
  final double width;
  final Color fill, edge, pressed;
  final Widget child;
  final VoidCallback? onTap, onTapCancel, onLongPress;
  final GestureTapDownCallback? onTapDown;
  final GestureTapUpCallback? onTapUp;

  @override
  State<_KeyCap> createState() => _KeyCapState();
}

class _KeyCapState extends State<_KeyCap> {
  bool _down = false;

  @override
  Widget build(BuildContext context) => InkWell(
    key: widget.inkKey,
    splashFactory: NoSplash.splashFactory,
    highlightColor: Colors.transparent,
    onHighlightChanged: (down) => setState(() => _down = down),
    onTap: widget.onTap,
    onTapDown: widget.onTapDown,
    onTapUp: widget.onTapUp,
    onTapCancel: widget.onTapCancel,
    onLongPress: widget.onLongPress,
    child: Padding(
      padding: const EdgeInsets.symmetric(horizontal: 2, vertical: 5),
      child: Container(
        width: widget.width,
        height: AndroidKeyRow.height - 10,
        padding: const EdgeInsets.symmetric(horizontal: 4),
        decoration: BoxDecoration(
          color: _down
              ? Color.alphaBlend(widget.pressed, widget.fill)
              : widget.fill,
          borderRadius: BorderRadius.circular(9),
          border: Border.all(color: widget.edge),
        ),
        child: Center(child: widget.child),
      ),
    ),
  );
}

/// A group of [AndroidScreenOptions].
enum AndroidScreenPart { scale, resolution, orientation }

/// Host-observed Windows DPI is separate from the requested automatic/manual choice.
String androidWindowsTextScaleStatus({
  required bool automatic,
  required bool supported,
  int? wanted,
  DisplayScaleApplied? applied,
}) {
  if (!supported) return l10n.actualValueUnknownNotSupported;
  final mode = automatic ? l10n.auto : l10n.manual;
  if (applied == null) {
    return wanted == null ? l10n.actualValueUnknown(mode) : l10n.requestedActualValueUnknown(mode, wanted);
  }
  if (!applied.ok) {
    return l10n.cantChangeActualValueUnknown(mode, wanted == null ? '' : l10n.requested3(wanted));
  }
  if (applied.scale <= 0) return l10n.actualValueUnknown(mode);
  return wanted != null && wanted != applied.scale
      ? l10n.nowRequested(applied.scale, mode, wanted)
      : l10n.now(applied.scale, mode);
}

/// Screen choices remembered per device class, with actual state kept in the notes.
class AndroidScreenOptions extends StatelessWidget {
  const AndroidScreenOptions({
    super.key,
    required this.deviceClass,
    required this.choice,
    required this.autoScale,
    required this.resolutions,
    required this.onChoice,
    this.scaleAvailable = true,
    this.resolutionAvailable = true,
    this.scaleNote,
    this.resolutionNote,
    this.parts = const {...AndroidScreenPart.values},
  });
  final Set<AndroidScreenPart> parts;
  final AndroidDeviceClass deviceClass;
  final AndroidScreenChoice choice;

  /// What 자동 asks for now (percent).
  final int? autoScale;
  final List<AndroidResolutionOption> resolutions;
  final ValueChanged<AndroidScreenChoice> onChoice;

  /// The Windows scale can be set for this connection (display-scale-v1);
  /// the 해상도 can change (a resizable stream).
  final bool scaleAvailable, resolutionAvailable;

  /// What the Host applied (it may allow less), or when it applies.
  final String? scaleNote, resolutionNote;

  @override
  Widget build(BuildContext context) {
    String size(Size s) => '${s.width.toInt()}×${s.height.toInt()}';
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        if (resolutionAvailable &&
            resolutions.length > 1 &&
            parts.contains(AndroidScreenPart.resolution))
          AndroidPanelChoices(
            key: const Key('android-screen-resolution'),
            label: l10n.resolution,
            note: resolutionNote,
            minWidth: 92,
            items: [
              for (final option in resolutions)
                AndroidPanelItem(
                  key: Key(
                    'android-screen-resolution-${option.resolution.encode()}',
                  ),
                  label: option.label,
                  detail: switch (option.resolution.kind) {
                    AndroidResolutionKind.auto =>
                      option.size == null ? l10n.recommended2 : size(option.size!),
                    AndroidResolutionKind.device => size(option.size!),
                    AndroidResolutionKind.half => size(option.size!),
                    AndroidResolutionKind.preset => l10n.fitted,
                    AndroidResolutionKind.longEdge =>
                      option.deviceSize ? size(option.size!) : l10n.screensRatio,
                    AndroidResolutionKind.desktop => size(option.size!),
                    AndroidResolutionKind.workspace => size(option.size!),
                    // What it costs, said before it is chosen.
                    AndroidResolutionKind.sharp => androidSharpDetail,
                  },
                  selected:
                      choice.resolution == option.resolution ||
                      choice.resolution.kind ==
                              AndroidResolutionKind.longEdge &&
                          option.resolution.kind ==
                              AndroidResolutionKind.longEdge &&
                          resolutions
                              .where(
                                (item) => item.resolution == choice.resolution,
                              )
                              .isEmpty &&
                          option.size == resolutions.firstOrNull?.size,
                  tooltip: switch (option.resolution.kind) {
                    AndroidResolutionKind.auto =>
                      deviceClass == AndroidDeviceClass.television
                          ? l10n.usesDevicesOwnPixelsHost
                          : l10n.recommendedSizeKeepsDeviceRatio,
                    AndroidResolutionKind.device => l10n.sharpest,
                    AndroidResolutionKind.half =>
                      l10n.quarterPixelsDrawnCrisply2,
                    AndroidResolutionKind.preset => l10n.fixedSizeScreensRatio,
                    AndroidResolutionKind.longEdge =>
                      l10n.sizeKeepsDeviceRatioSmall,
                    AndroidResolutionKind.desktop =>
                      l10n.remoteWorkspaceSizeCheckApplied,
                    AndroidResolutionKind.workspace =>
                      l10n.remoteWorkspaceFittedDevicesScreen,
                    AndroidResolutionKind.sharp => androidSharpHint,
                  },
                  onPressed: () =>
                      onChoice(choice.copyWith(resolution: option.resolution)),
                ),
            ],
          ),
        if (scaleAvailable && parts.contains(AndroidScreenPart.scale))
          AndroidPanelChoices(
            key: const Key('android-screen-scale'),
            label: deviceClass == AndroidDeviceClass.television
                ? l10n.windowsScale3(deviceClass.label)
                : l10n.windowsScale3(deviceClass.label),
            note: scaleNote,
            items: [
              if (deviceClass == AndroidDeviceClass.television)
                AndroidPanelItem(
                  key: const Key('android-screen-scale-0'),
                  label: l10n.auto,
                  detail: autoScale == null ? l10n.calculating : '$autoScale%',
                  selected: choice.scale == 0,
                  tooltip: l10n.choosesSuitTvsResolution,
                  onPressed: () => onChoice(choice.copyWith(scale: 0)),
                ),
              for (final percent
                  in deviceClass == AndroidDeviceClass.television
                      ? androidTvScreenScales
                      : androidScreenScales)
                AndroidPanelItem(
                  key: Key('android-screen-scale-$percent'),
                  label: '$percent',
                  detail: '%',
                  selected:
                      choice.scale == percent ||
                      deviceClass != AndroidDeviceClass.television &&
                          choice.scale == 0 &&
                          autoScale == percent,
                  onPressed: () => onChoice(choice.copyWith(scale: percent)),
                ),
            ],
          ),
        if (parts.contains(AndroidScreenPart.orientation))
          AndroidPanelChoices(
            key: const Key('android-screen-orientation'),
            label: l10n.orientation,
            minWidth: 76,
            items: [
              for (final orientation in AndroidOrientation.values)
                AndroidPanelItem(
                  key: Key('android-screen-orientation-${orientation.name}'),
                  label: orientation.label,
                  icon: switch (orientation) {
                    AndroidOrientation.auto => Icons.screen_rotation_rounded,
                    AndroidOrientation.landscape =>
                      Icons.stay_current_landscape_rounded,
                    AndroidOrientation.portrait =>
                      Icons.stay_current_portrait_rounded,
                  },
                  selected: choice.orientation == orientation,
                  onPressed: () =>
                      onChoice(choice.copyWith(orientation: orientation)),
                ),
            ],
          ),
      ],
    );
  }
}

/// The session menu's 화질 panel: 화질 모드, 목표 프레임 and 영상 전송률.
/// Sound is no choice (every connection asks for it). 재연결
/// applies them, once something changed, by reconnecting. What the
/// connection really got shows in 연결 품질 and its statistics.
class AndroidStreamSettingsPanel extends StatefulWidget {
  const AndroidStreamSettingsPanel({
    super.key,
    required this.options,
    required this.onApply,
    required this.onCancel,
    this.maxBitrate,
    this.audioDeviceMissing = false,
  });
  final StreamOptions options;

  /// The desktop's bitrate-adaptive-v1 ceiling, for which 화질 모드 the
  /// choices read as.
  final int? maxBitrate;

  /// The Host reported that the desktop has no playback device: the one
  /// line saying so shows under the choices, with no action (Android has no
  /// sound switch; the administrator adds the device in Proxmox).
  final bool audioDeviceMissing;

  final ValueChanged<StreamOptions> onApply;
  final VoidCallback onCancel;

  @override
  State<AndroidStreamSettingsPanel> createState() =>
      _AndroidStreamSettingsPanelState();
}

class _AndroidStreamSettingsPanelState
    extends State<AndroidStreamSettingsPanel> {
  late final _initial = AndroidStreamQuality.of(widget.options);
  late var _choice = _initial;

  bool get _changed =>
      _choice.fps != _initial.fps || _choice.bitrate != _initial.bitrate;

  void _set({int? fps, int? bitrate}) => setState(
    () => _choice = AndroidStreamQuality(
      fps: fps ?? _choice.fps,
      bitrate: bitrate ?? _choice.bitrate,
    ),
  );

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    const automatic = AndroidStreamQuality.automatic;
    final preset = StreamPreset.of(_choice.options, widget.maxBitrate);
    return AndroidToolbarPanel(
      title: SessionTool.settings.androidLabel,
      // On its side: 화질 모드 and 목표 프레임 left, the rest right.
      besideSplit: 2,
      footer: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          AndroidMenuStyle.divider(chrome),
          AndroidPanelRow(
            items: [
              AndroidPanelItem(
                key: const Key('cancel-stream-settings'),
                label: l10n.cancel,
                onPressed: widget.onCancel,
              ),
              AndroidPanelItem(
                key: const Key('apply-stream-settings'),
                label: l10n.reconnect,
                icon: Icons.refresh_rounded,
                primary: true,
                selected: _changed,
                tooltip: _changed ? l10n.reconnectChangedSettings : l10n.noSettingsChanged,
                onPressed: _changed
                    ? () => widget.onApply(_choice.options)
                    : null,
              ),
            ],
          ),
        ],
      ),
      children: [
        AndroidPanelChoices(
          key: const Key('stream-preset'),
          label: l10n.qualityMode2,
          note: preset?.label ?? l10n.custom,
          minWidth: 92,
          items: [
            for (final mode in StreamPreset.values)
              AndroidPanelItem(
                key: Key('stream-preset-${mode.name}'),
                label: mode.label,
                tooltip: mode.detail,
                selected: preset == mode,
                onPressed: () => _set(
                  fps: mode.options.fps,
                  bitrate: mode.options.automaticBitrate
                      ? automatic
                      : mode.options.bitrate ~/ 1000000,
                ),
              ),
          ],
        ),
        AndroidPanelChoices(
          label: l10n.targetFrameRate,
          minWidth: 92,
          items: [
            for (final fps in const [30, 60])
              AndroidPanelItem(
                key: Key('stream-fps-$fps'),
                label: '$fps fps',
                selected: _choice.fps == fps,
                onPressed: () => _set(fps: fps),
              ),
          ],
        ),
        AndroidPanelChoices(
          label: l10n.videoBitrateMbps,
          note: AndroidStreamQuality.bitrateLabel(_choice.bitrate),
          minWidth: 64,
          items: [
            for (final value in AndroidStreamQuality.bitrates)
              AndroidPanelItem(
                key: Key('stream-bitrate-$value'),
                label: value == automatic ? l10n.auto : '$value',
                tooltip: AndroidStreamQuality.bitrateLabel(value),
                detail: value == automatic ? l10n.recommended2 : null,
                selected: _choice.bitrate == value,
                onPressed: () => _set(bitrate: value),
              ),
          ],
        ),
        if (widget.audioDeviceMissing)
          Padding(
            padding: const EdgeInsets.fromLTRB(
              AndroidMenuStyle.inset,
              10,
              AndroidMenuStyle.inset,
              4,
            ),
            child: KeepAllText(
              audioDeviceMissingText,
              key: const Key('stream-audio-hint'),
              style: _text(12, color: chrome.muted, height: 1.45),
            ),
          ),
      ],
    );
  }
}

/// One row of the session menu sheet.
class AndroidMenuEntry {
  const AndroidMenuEntry(
    this.id,
    this.label, {
    required this.icon,
    this.detail,
    this.color,
    this.enabled = true,
    this.checked = false,
    this.danger = false,
  });
  final String id, label;
  final String? detail;
  final IconData icon;
  final Color? color;
  final bool enabled, checked, danger;
}

/// A titled group of [AndroidMenuEntry] rows or one choice row.
class AndroidMenuSection {
  const AndroidMenuSection(this.title, this.entries, {this.note, this.choice});
  final String title;
  final List<AndroidMenuEntry> entries;
  final String? note;

  /// A segmented choice shown above the entries: (id prefix, options as
  /// (value, label), selected value).
  final (String, List<(String, String)>, String)? choice;
}

/// The session menu as a bottom sheet with 48 dp rows, in the session's
/// own dark chrome. Returns the chosen entry id (a choice returns
/// `prefix:value`).
Future<String?> showAndroidSessionMenu(
  BuildContext context, {
  required String title,
  required String subtitle,
  required List<AndroidMenuSection> sections,
}) {
  final chrome = SessionChrome.of(context);
  return showModalBottomSheet<String>(
    context: context,
    isScrollControlled: true,
    useSafeArea: true,
    backgroundColor: chrome.surface,
    barrierColor: Colors.black.withValues(alpha: .45),
    constraints: const BoxConstraints(maxWidth: 560),
    shape: RoundedRectangleBorder(
      borderRadius: const BorderRadius.vertical(top: Radius.circular(22)),
      side: BorderSide(color: chrome.popupEdge),
    ),
    builder: (context) {
      final height = MediaQuery.sizeOf(context).height;
      return ConstrainedBox(
        constraints: BoxConstraints(maxHeight: height * .86),
        child: SingleChildScrollView(
          key: const Key('android-session-menu-sheet'),
          padding: const EdgeInsets.fromLTRB(8, 10, 8, 16),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              Center(
                child: Container(
                  width: 36,
                  height: 4,
                  decoration: BoxDecoration(
                    color: chrome.faint.withValues(alpha: .6),
                    borderRadius: BorderRadius.circular(2),
                  ),
                ),
              ),
              Padding(
                padding: const EdgeInsets.fromLTRB(12, 14, 12, 4),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    KeepAllText(
                      title,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: _text(
                        17,
                        weight: FontWeight.w600,
                        color: chrome.ink,
                      ),
                    ),
                    const SizedBox(height: 2),
                    KeepAllText(
                      subtitle,
                      maxLines: 2,
                      overflow: TextOverflow.ellipsis,
                      style: _text(12.5, color: chrome.muted),
                    ),
                  ],
                ),
              ),
              for (final section in sections)
                _section(context, chrome, section),
            ],
          ),
        ),
      );
    },
  );
}

Widget _section(
  BuildContext context,
  SessionChrome chrome,
  AndroidMenuSection section,
) {
  final choice = section.choice;
  return Column(
    crossAxisAlignment: CrossAxisAlignment.stretch,
    children: [
      Padding(
        padding: const EdgeInsets.fromLTRB(12, 16, 12, 6),
        child: KeepAllText(
          section.title,
          style: _text(12, weight: FontWeight.w600, color: chrome.muted),
        ),
      ),
      if (choice != null)
        Padding(
          padding: const EdgeInsets.fromLTRB(8, 0, 8, 6),
          child: Row(
            children: [
              for (final (index, option) in choice.$2.indexed) ...[
                if (index > 0) const SizedBox(width: 6),
                Expanded(
                  child: _ChoiceChip(
                    key: Key('android-menu-${choice.$1}-${option.$1}'),
                    label: option.$2,
                    selected: option.$1 == choice.$3,
                    onTap: () =>
                        Navigator.pop(context, '${choice.$1}:${option.$1}'),
                  ),
                ),
              ],
            ],
          ),
        ),
      for (final entry in section.entries)
        _MenuRow(
          entry: entry,
          onTap: entry.enabled ? () => Navigator.pop(context, entry.id) : null,
        ),
      if (section.note != null)
        Padding(
          padding: const EdgeInsets.fromLTRB(12, 4, 12, 0),
          child: KeepAllText(
            section.note!,
            style: _text(12, color: chrome.muted, height: 1.45),
          ),
        ),
    ],
  );
}

class _ChoiceChip extends StatelessWidget {
  const _ChoiceChip({
    super.key,
    required this.label,
    required this.selected,
    required this.onTap,
  });
  final String label;
  final bool selected;
  final VoidCallback onTap;
  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    return Material(
      color: selected
          ? chrome.accent.withValues(alpha: .16)
          : const Color(0xFF1F2429),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: BorderSide(
          color: selected ? chrome.accent.withValues(alpha: .7) : chrome.edge,
        ),
      ),
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: onTap,
        child: ConstrainedBox(
          constraints: const BoxConstraints(minHeight: 48),
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 8),
            child: Center(
              child: KeepAllText(
                label,
                textAlign: TextAlign.center,
                style: _text(
                  13,
                  weight: FontWeight.w600,
                  color: selected ? chrome.accent : chrome.ink,
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _MenuRow extends StatelessWidget {
  const _MenuRow({required this.entry, this.onTap});
  final AndroidMenuEntry entry;
  final VoidCallback? onTap;
  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    final enabled = onTap != null;
    final color = !enabled
        ? chrome.faint
        : entry.danger
        ? chrome.danger
        : chrome.ink;
    return InkWell(
      key: Key('android-menu-${entry.id}'),
      borderRadius: BorderRadius.circular(12),
      onTap: onTap,
      child: ConstrainedBox(
        constraints: const BoxConstraints(minHeight: 50),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
          child: Row(
            children: [
              Icon(
                entry.icon,
                size: 21,
                color: !enabled
                    ? chrome.faint
                    : entry.danger
                    ? chrome.danger
                    : entry.color ?? chrome.muted,
              ),
              const SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    KeepAllText(
                      entry.label,
                      style: _text(14.5, weight: FontWeight.w600, color: color),
                    ),
                    if (entry.detail != null)
                      Padding(
                        padding: const EdgeInsets.only(top: 2),
                        child: KeepAllText(
                          entry.detail!,
                          style: _text(12, color: chrome.muted, height: 1.4),
                        ),
                      ),
                  ],
                ),
              ),
              if (entry.checked)
                Icon(Icons.check_rounded, size: 18, color: chrome.good),
            ],
          ),
        ),
      ),
    );
  }
}

/// 사용안내: the gestures of the current touch mode, in the session menu's
/// place as Windows' 사용안내 panel opens beside its bar, with its 확인.
///
/// 터치 has two meanings. On a Host that granted touch-input-v1 ([realTouch])
/// every finger is sent to Windows as a real touch contact and Windows and the
/// app decide what it does: no local pinch zoom, no two-finger right click, no
/// double-tap drag, no delayed tap. Without it 터치 is the mouse at the finger,
/// with the phone's own gestures. The panel used to describe only the second.
class AndroidTouchHelp extends StatelessWidget {
  const AndroidTouchHelp({
    super.key,
    required this.mode,
    required this.onClose,
    this.realTouch = false,
    this.tips = const [],
  });
  final AndroidTouchMode mode;
  final VoidCallback onClose;

  /// The session has real Windows touch (RemoteSession.touchInput).
  final bool realTouch;

  /// What this connection can use from the device (프린터, 복사·붙여넣기),
  /// as the Windows 사용안내 lists it, after the touch gestures.
  final List<SessionGuideTip> tips;

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    final trackpad = mode == AndroidTouchMode.trackpad;
    final windowsTouch = !trackpad && realTouch;
    final rows = <(IconData, String, String)>[
      if (windowsTouch) ...[
        (Icons.touch_app_rounded, l10n.tapSwipe, l10n.sentAsWindowsTouchWorks),
        (Icons.pan_tool_alt_rounded, l10n.longPress, l10n.rightClick),
        (Icons.zoom_in_rounded, l10n.twoFingers, l10n.zoomScrollInsideAppsWhen),
      ] else if (trackpad) ...[
        (Icons.open_with_rounded, l10n.slideOneFinger, l10n.moveCursor),
        (Icons.touch_app_rounded, l10n.tap, l10n.clickCursorDoubleTapDouble),
        (Icons.gesture_rounded, l10n.doubleTapMoveWhileHolding, l10n.dragSelectMoveWindows),
        (Icons.swap_vert_rounded, l10n.twoFingersUpDown, l10n.scroll),
      ] else ...[
        (Icons.touch_app_rounded, l10n.tap, l10n.clickWhereYouTapDouble),
        (Icons.swap_vert_rounded, l10n.oneFingerUpDown, l10n.scroll),
        (Icons.gesture_rounded, l10n.doubleTapMoveWhileHolding2, l10n.dragSelectMoveWindows),
      ],
      if (!windowsTouch) ...[
        (Icons.pan_tool_alt_rounded, l10n.longPressTwoFingerTap, l10n.rightClick),
        (Icons.zoom_in_rounded, l10n.spreadTwoFingers, l10n.zoomPinchBackSeeWhole),
      ],
      (Icons.keyboard_rounded, l10n.threeFingerTap, l10n.openCloseKeyboard),
      (Icons.swap_horiz_rounded, l10n.fourFingerTap, l10n.switchInputModePointerTouch),
    ];
    return AndroidToolbarPanel(
      key: const Key('android-touch-help'),
      title: SessionTool.guide.label,
      footer: Padding(
        padding: const EdgeInsets.fromLTRB(4, 8, 4, 0),
        child: FilledButton(
          key: const Key('android-touch-help-close'),
          onPressed: onClose,
          style: FilledButton.styleFrom(
            backgroundColor: chrome.popupHighlight,
            foregroundColor: chrome.ink,
            minimumSize: const Size.fromHeight(48),
            textStyle: _text(14, weight: FontWeight.w600),
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(10),
            ),
          ),
          child: Text(l10n.ok),
        ),
      ),
      children: [
        AndroidMenuCaption(l10n.inputMode2(mode.label)),
        Padding(
          padding: const EdgeInsets.fromLTRB(
            AndroidMenuStyle.inset,
            0,
            AndroidMenuStyle.inset,
            6,
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              for (final row in rows)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 8),
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      SizedBox(
                        width: 24,
                        child: Icon(row.$1, size: 20, color: chrome.muted),
                      ),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            KeepAllText(
                              row.$2,
                              style: _text(
                                13.5,
                                weight: FontWeight.w600,
                                color: chrome.ink,
                              ),
                            ),
                            const SizedBox(height: 3),
                            KeepAllText(
                              row.$3,
                              style: _text(12.5, color: chrome.muted),
                            ),
                          ],
                        ),
                      ),
                    ],
                  ),
                ),
              const SizedBox(height: 8),
              KeepAllText(
                // Three short lines (2026-10-29: concise reading). Real
                // touch has no delayed tap and no zoom of the picture itself.
                l10n.longPressInputModeButton(windowsTouch ? l10n.zoomScreenItselfSwitchInput : l10n.tapClicksLittleSlowWhile),
                style: _text(12, color: chrome.muted, height: 1.45),
              ),
            ],
          ),
        ),
        // What this connection can use, as the Windows 사용안내 lists it.
        if (tips.isNotEmpty) ...[
          AndroidMenuCaption(l10n.featuresConnection),
          Padding(
            padding: const EdgeInsets.fromLTRB(
              AndroidMenuStyle.inset,
              0,
              AndroidMenuStyle.inset,
              6,
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                for (final tip in tips)
                  Padding(
                    key: Key('android-guide-tip-${tip.feature.name}'),
                    padding: const EdgeInsets.symmetric(vertical: 8),
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        SizedBox(
                          width: 24,
                          child: Icon(
                            tip.feature == SessionGuideFeature.printer
                                ? Icons.print_outlined
                                : Icons.content_paste_rounded,
                            size: 20,
                            color: chrome.muted,
                          ),
                        ),
                        const SizedBox(width: 12),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Wrap(
                                spacing: 8,
                                crossAxisAlignment: WrapCrossAlignment.center,
                                children: [
                                  KeepAllText(
                                    tip.title,
                                    style: _text(
                                      13.5,
                                      weight: FontWeight.w600,
                                      color: chrome.ink,
                                    ),
                                  ),
                                  KeepAllText(
                                    switch (tip.availability) {
                                      SessionGuideAvailability.ready => l10n.available,
                                      SessionGuideAvailability.preparing =>
                                        l10n.checkingConnection,
                                      SessionGuideAvailability.unavailable =>
                                        l10n.notAvailableConnection,
                                    },
                                    style: _text(
                                      12,
                                      color:
                                          tip.availability ==
                                              SessionGuideAvailability.ready
                                          ? chrome.good
                                          : chrome.muted,
                                    ),
                                  ),
                                ],
                              ),
                              const SizedBox(height: 3),
                              KeepAllText(
                                tip.message,
                                style: _text(
                                  12.5,
                                  color: chrome.muted,
                                  height: 1.45,
                                ),
                              ),
                            ],
                          ),
                        ),
                      ],
                    ),
                  ),
              ],
            ),
          ),
        ],
      ],
    );
  }
}

/// The centred card while the picture is not there yet or the connection
/// ended: what is happening, and the actions that apply now.
class AndroidSessionPanel extends StatelessWidget {
  const AndroidSessionPanel({
    super.key,
    required this.title,
    required this.message,
    this.busy = false,
    this.icon = Icons.link_off_rounded,
    this.actions = const [],
    this.detail,
    this.progress,
  });
  final String title, message;
  final String? detail;
  final Widget? progress;
  final bool busy;
  final IconData icon;
  final List<Widget> actions;

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    return Container(
      key: const Key('android-session-panel'),
      constraints: const BoxConstraints(maxWidth: 400),
      margin: const EdgeInsets.all(20),
      padding: const EdgeInsets.fromLTRB(22, 24, 22, 20),
      decoration: BoxDecoration(
        color: chrome.surface,
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: chrome.popupEdge),
        boxShadow: [
          BoxShadow(
            color: chrome.shadow,
            blurRadius: 30,
            offset: const Offset(0, 14),
          ),
        ],
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          SizedBox.square(
            dimension: 52,
            child: DecoratedBox(
              decoration: BoxDecoration(
                color: (busy ? chrome.accent : chrome.danger).withValues(
                  alpha: .14,
                ),
                shape: BoxShape.circle,
              ),
              child: Center(
                child: busy
                    ? SizedBox.square(
                        dimension: 24,
                        child: CircularProgressIndicator(
                          strokeWidth: 2.6,
                          color: chrome.accent,
                        ),
                      )
                    : Icon(icon, color: chrome.danger, size: 26),
              ),
            ),
          ),
          const SizedBox(height: 14),
          KeepAllText(
            title,
            textAlign: TextAlign.center,
            maxLines: 2,
            overflow: TextOverflow.ellipsis,
            style: _text(18, weight: FontWeight.w600, color: chrome.ink),
          ),
          const SizedBox(height: 8),
          KeepAllText(
            message,
            key: const Key('android-session-message'),
            textAlign: TextAlign.center,
            style: _text(13.5, color: chrome.muted, height: 1.55),
          ),
          if (detail != null) ...[
            const SizedBox(height: 6),
            KeepAllText(
              detail!,
              textAlign: TextAlign.center,
              style: _text(12, color: chrome.muted),
            ),
          ],
          // The stages of the connection, as on Windows: under what is said.
          if (progress != null) ...[const SizedBox(height: 14), progress!],
          if (actions.isNotEmpty) ...[
            const SizedBox(height: 20),
            Wrap(
              spacing: 10,
              runSpacing: 10,
              alignment: WrapAlignment.center,
              children: actions,
            ),
          ],
        ],
      ),
    );
  }
}

/// Button styles for the session panels (dark chrome in both appearances).
abstract final class AndroidSessionButtons {
  static ButtonStyle quiet(SessionChrome chrome) => TextButton.styleFrom(
    backgroundColor: const Color(0xFF22282D),
    foregroundColor: chrome.ink,
    minimumSize: const Size(112, 48),
    padding: const EdgeInsets.symmetric(horizontal: 18),
    textStyle: _text(14, weight: FontWeight.w600),
    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
  );

  static ButtonStyle filled(SessionChrome chrome) => FilledButton.styleFrom(
    backgroundColor: chrome.accent,
    foregroundColor: const Color(0xFF241600),
    minimumSize: const Size(128, 48),
    padding: const EdgeInsets.symmetric(horizontal: 18),
    textStyle: _text(14, weight: FontWeight.w600),
    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(12)),
  );
}

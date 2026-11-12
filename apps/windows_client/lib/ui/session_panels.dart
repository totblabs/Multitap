import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import '../data/drive_session.dart';
import '../data/remote_session.dart';
import '../data/usb_redirection.dart';
import '../models/session_display_mode.dart';
import 'app_menu.dart';
import 'app_theme.dart';
import 'display_scale_panel.dart';
import 'drive_panel.dart';
import 'korean_text.dart';
import 'session_menu.dart' show SessionPopups, SessionTouchScope;
import 'session_tools.dart';
import 'usb_panel.dart';
import '../l10n/l10n.dart';

/// The session bar's panels (2026-11-10): 화면, 폴더·USB and 설정 each drop
/// down from their button, dark like the bar in either app appearance. A
/// window too small for a panel gets the same content as a dialog. [popups]
/// tracks them with the bar's menus, so a change under them (a resize,
/// another screen mode, the end of the connection) closes them.
Future<T?> showSessionPanel<T>(
  BuildContext context, {
  required Rect? anchor,
  required double width,
  required double minHeight,
  required String label,
  required WidgetBuilder builder,
  SessionPopups? popups,
  bool keepOpenForDialog = false,
}) {
  final navigator = Navigator.of(context);
  final media = MediaQuery.of(navigator.overlay!.context);
  final track = popups?.tracker();
  Widget tracked(Widget child) => track == null ? child : track(child);
  if (anchor != null) {
    final area = AnchoredPanelRoute.areaFor(media, anchor);
    if (area.width >= width + 2 * AnchoredPanelRoute.margin &&
        area.bottom - anchor.bottom - AnchoredPanelRoute.margin >= minHeight) {
      return navigator.push(
        AnchoredPanelRoute<T>(
          anchor: anchor,
          reduceMotion: MediaQuery.disableAnimationsOf(context),
          width: width,
          label: label,
          keepOpenForDialog: keepOpenForDialog,
          theme: buildAppTheme(brightness: Brightness.dark),
          builder: (context) => tracked(builder(context)),
        ),
      );
    }
  }
  return showDialog<T>(
    context: context,
    builder: (context) => tracked(
      Theme(
        data: buildAppTheme(brightness: Brightness.dark),
        child: Dialog(
          key: const Key('session-panel-dialog'),
          backgroundColor: const AppPalette(true).surface,
          child: ConstrainedBox(
            constraints: BoxConstraints(maxWidth: width + 40, maxHeight: 600),
            child: Builder(builder: builder),
          ),
        ),
      ),
    ),
  );
}

/// A panel's title with what it is about now at the right.
class SessionPanelHeader extends StatelessWidget {
  const SessionPanelHeader(this.title, {super.key, this.status, this.onBack});
  final String title;
  final String? status;
  final VoidCallback? onBack;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return Row(
      children: [
        if (onBack != null)
          Padding(
            padding: const EdgeInsets.only(right: 4),
            child: IconButton(
              key: const Key('session-panel-back'),
              tooltip: l10n.back,
              onPressed: onBack,
              visualDensity: VisualDensity.compact,
              icon: const Icon(Icons.chevron_left_rounded, size: 20),
            ),
          ),
        Expanded(
          child: KeepAllText(
            title,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: uiText(size: 14, weight: FontWeight.w600, color: p.ink),
          ),
        ),
        if (status != null) ...[
          const SizedBox(width: 10),
          Expanded(
            child: KeepAllText(
              status!,
              maxLines: 1,
              textAlign: TextAlign.end,
              overflow: TextOverflow.ellipsis,
              style: uiText(size: 12, color: p.muted),
            ),
          ),
        ],
      ],
    );
  }
}

/// A short label that breaks between words only. Where its longest word is
/// wider than the room (large text, a longer language) the label is drawn
/// smaller, instead of a word being split ("Window/ed") or cut ("monitor").
/// With room for every word it is the plain text. [width] is the room the
/// label has (given by its parent: the label itself sits where intrinsic
/// sizes are asked for, so it cannot measure its own).
class WholeWordText extends StatelessWidget {
  const WholeWordText(
    this.text, {
    super.key,
    required this.width,
    required this.style,
    this.textAlign,
    this.maxLines,
  });
  final String text;
  final double width;
  final TextStyle style;
  final TextAlign? textAlign;
  final int? maxLines;

  @override
  Widget build(BuildContext context) {
    final full = style.fontSize ?? 14;
    var size = full;
    if (width.isFinite && width > 0) {
      final scaler = MediaQuery.textScalerOf(context);
      // As the text itself is drawn: what the style leaves open comes from
      // the surrounding default (letter spacing).
      final drawn = DefaultTextStyle.of(context).style.merge(style);
      final words = text.split(RegExp(r'\s+'));
      double widest(double fontSize) {
        var most = 0.0;
        for (final word in words) {
          final painter = TextPainter(
            text: TextSpan(
              text: word,
              style: drawn.copyWith(fontSize: fontSize),
            ),
            textDirection: TextDirection.ltr,
            textScaler: scaler,
            maxLines: 1,
          )..layout();
          if (painter.width > most) most = painter.width;
          painter.dispose();
        }
        return most;
      }

      // Letter spacing does not shrink with the size: a few steps settle it.
      for (var step = 0; step < 4; step++) {
        final most = widest(size);
        if (most <= width) break;
        // A little under the exact fit: the laid-out line rounds up.
        size *= width / most * .98;
      }
    }
    return KeepAllText(
      text,
      textAlign: textAlign,
      maxLines: maxLines,
      style: size < full ? style.copyWith(fontSize: size) : style,
    );
  }
}

/// A small muted caption over a group of a panel; [note] at its right.
class SessionPanelCaption extends StatelessWidget {
  const SessionPanelCaption(this.label, {super.key, this.note});
  final String label;
  final String? note;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return Row(
      children: [
        KeepAllText(
          label,
          style: uiText(size: 12, weight: FontWeight.w600, color: p.muted),
        ),
        if (note != null) ...[
          const SizedBox(width: 10),
          Expanded(
            // One line where it fits; at large text it wraps, never "…".
            child: KeepAllText(
              note!,
              textAlign: TextAlign.end,
              style: uiText(size: 12, height: 1.35, color: p.muted),
            ),
          ),
        ],
      ],
    );
  }
}

/// A key combination drawn as key caps: Ctrl Shift Alt Z.
class SessionKeycaps extends StatelessWidget {
  const SessionKeycaps(this.keys, {super.key, this.color});
  final List<String> keys;
  final Color? color;

  /// 마우스 잠금's keys.
  static const mouseLock = ['Ctrl', 'Shift', 'Alt', 'Z'];

  @override
  Widget build(BuildContext context) {
    final ink = color ?? AppPalette.of(context).ink;
    // One line where there is room; in a narrow row or at large text the
    // caps wrap instead of running off.
    return Wrap(
      spacing: 3,
      runSpacing: 3,
      children: [
        for (final key in keys)
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 5, vertical: 1.5),
            decoration: BoxDecoration(
              color: const Color(0x0DFFFFFF),
              borderRadius: BorderRadius.circular(5),
              border: Border.all(color: const Color(0x29FFFFFF)),
            ),
            child: Text(
              key,
              style: uiText(
                size: 12,
                height: 1.15,
                weight: FontWeight.w600,
                color: ink,
              ),
            ),
          ),
      ],
    );
  }
}

/// One row of a session panel: an icon, a title, a short line under it and
/// what it does at the right (a chevron for a row that opens something, a
/// switch for one that turns something on). A row without [onTap] is greyed
/// and says [disabledReason] on a tap or under the pointer.
class SessionPanelRow extends StatelessWidget {
  const SessionPanelRow({
    super.key,
    required this.icon,
    required this.title,
    this.subtitle,
    this.subtitleWidget,
    this.onTap,
    this.opens = false,
    this.toggled,
    this.disabledReason,
    this.destructive = false,
  });
  final IconData icon;
  final String title;
  final String? subtitle;
  final Widget? subtitleWidget;
  final VoidCallback? onTap;

  /// It opens a page or another panel: a chevron at its end.
  final bool opens;

  /// It is a switch, showing this value.
  final bool? toggled;
  final String? disabledReason;
  final bool destructive;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final enabled = onTap != null;
    final ink = !enabled
        ? p.faint
        : destructive
        ? p.red
        : p.ink;
    final row = Semantics(
      button: true,
      enabled: enabled,
      toggled: toggled,
      child: Material(
        color: Colors.transparent,
        borderRadius: BorderRadius.circular(10),
        clipBehavior: Clip.antiAlias,
        child: InkWell(
          onTap: onTap,
          hoverColor: p.hover,
          splashFactory: NoSplash.splashFactory,
          child: ConstrainedBox(
            // By finger (the Android menu card) a row is a 48 dp target.
            constraints: BoxConstraints(
              minHeight: SessionTouchScope.of(context) ? 52 : 46,
            ),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
              child: Row(
                children: [
                  Icon(
                    icon,
                    size: 18,
                    color: !enabled
                        ? p.faint
                        : destructive
                        ? p.red
                        : p.muted,
                  ),
                  const SizedBox(width: 11),
                  Expanded(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        KeepAllText(
                          title,
                          style: uiText(
                            size: 13,
                            height: 1.3,
                            weight: FontWeight.w600,
                            color: ink,
                          ),
                        ),
                        if (subtitleWidget != null) ...[
                          const SizedBox(height: 3),
                          subtitleWidget!,
                        ] else if (subtitle != null) ...[
                          const SizedBox(height: 2),
                          KeepAllText(
                            subtitle!,
                            style: uiText(
                              size: 12,
                              height: 1.35,
                              color: enabled ? p.muted : p.faint,
                            ),
                          ),
                        ],
                      ],
                    ),
                  ),
                  if (toggled != null) ...[
                    const SizedBox(width: 10),
                    // The whole row is the switch: one target, one focus.
                    ExcludeSemantics(
                      child: IgnorePointer(
                        child: Switch(
                          value: toggled!,
                          onChanged: enabled ? (_) {} : null,
                          materialTapTargetSize:
                              MaterialTapTargetSize.shrinkWrap,
                        ),
                      ),
                    ),
                  ] else if (opens) ...[
                    const SizedBox(width: 6),
                    Icon(Icons.chevron_right_rounded, size: 18, color: p.faint),
                  ],
                ],
              ),
            ),
          ),
        ),
      ),
    );
    final reason = disabledReason;
    return enabled || reason == null
        ? row
        : Tooltip(
            message: '$title · $reason',
            triggerMode: TooltipTriggerMode.tap,
            child: row,
          );
  }
}

// ---------------------------------------------------------------- 화면

/// What was chosen in the 화면 panel.
class SessionScreenChoice {
  const SessionScreenChoice.mode(SessionDisplayMode this.mode)
    : scale = null,
      displays = false;
  const SessionScreenChoice.scale(int this.scale)
    : mode = null,
      displays = false;

  /// 가상 모니터 1개·2개 구성….
  const SessionScreenChoice.displays()
    : mode = null,
      scale = null,
      displays = true;
  final SessionDisplayMode? mode;
  final int? scale;
  final bool displays;
}

IconData sessionDisplayModeIcon(SessionDisplayMode mode) => switch (mode) {
  SessionDisplayMode.windowed => Icons.web_asset_rounded,
  SessionDisplayMode.fullscreen => Icons.fullscreen_rounded,
  SessionDisplayMode.allMonitors => Icons.view_column_rounded,
};

Key sessionDisplayModeKey(SessionDisplayMode mode) => Key(switch (mode) {
  SessionDisplayMode.windowed => 'stream-windowed',
  SessionDisplayMode.fullscreen => 'stream-fullscreen',
  SessionDisplayMode.allMonitors => 'stream-all-monitors',
});

/// 화면: how the remote picture sits on this PC (the screen mode; 모든
/// 모니터 사용 only where it is offered) and how large Windows draws in it
/// (배율, five steps). The resolution follows the window by itself and is
/// only reported.
class SessionScreenPanel extends StatelessWidget {
  const SessionScreenPanel({
    super.key,
    required this.modes,
    required this.shown,
    required this.onSelected,
    this.session,
    this.changing = false,
    this.scaleAvailable = false,
    String? scaleReason,
    this.viewportPixels,
    this.automaticResolution = true,
    this.resolutionAvailable = false,
    this.displaysOffered = false,
    this.displaysEnabled = false,
  }) : _scaleReason = scaleReason; // ignore: prefer_initializing_formals

  /// The modes offered here, in order.
  final List<SessionDisplayMode> modes;
  final SessionDisplayMode shown;

  /// A reconnect or a screen change is under way: the modes wait.
  final bool changing;

  /// The session whose 배율 this is; null before a picture is up.
  final RemoteSession? session;
  final bool scaleAvailable, automaticResolution, resolutionAvailable;
  final String? _scaleReason;
  String get scaleReason => _scaleReason ?? l10n.youCanChangeOnceConnected;
  final Size? viewportPixels;

  /// 가상 모니터 1개·2개 구성…, where the session does not set the count
  /// from the screen mode.
  final bool displaysOffered, displaysEnabled;

  /// Null: the panel closes without a change (the current mode was chosen).
  final ValueChanged<SessionScreenChoice?> onSelected;

  static double widthFor(int modes) => modes > 2 ? 372 : 316;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final session = this.session;
    Widget tile(SessionDisplayMode mode, double labelWidth) {
      final on = mode == shown;
      return Expanded(
        child: Semantics(
          button: true,
          selected: on,
          enabled: !changing,
          child: Material(
            color: on ? p.amber.withValues(alpha: .11) : p.tileWell,
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(11),
              side: BorderSide(color: on ? p.amber : p.tileLine),
            ),
            clipBehavior: Clip.antiAlias,
            child: InkWell(
              key: sessionDisplayModeKey(mode),
              // The current mode stays a readable tile; choosing it changes
              // nothing.
              onTap: changing
                  ? null
                  : () =>
                        onSelected(on ? null : SessionScreenChoice.mode(mode)),
              hoverColor: p.hover,
              splashFactory: NoSplash.splashFactory,
              child: ConstrainedBox(
                constraints: const BoxConstraints(minHeight: 78),
                child: Stack(
                  alignment: Alignment.center,
                  children: [
                    Padding(
                      padding: const EdgeInsets.symmetric(
                        horizontal: 6,
                        vertical: 12,
                      ),
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          Icon(
                            sessionDisplayModeIcon(mode),
                            size: 26,
                            color: changing
                                ? p.faint
                                : on
                                ? p.amber
                                : p.muted,
                          ),
                          const SizedBox(height: 8),
                          WholeWordText(
                            mode.label,
                            width: labelWidth,
                            textAlign: TextAlign.center,
                            maxLines: 2,
                            style: uiText(
                              size: 12.5,
                              height: 1.25,
                              weight: FontWeight.w600,
                              color: changing ? p.faint : p.ink,
                            ),
                          ),
                        ],
                      ),
                    ),
                    if (on)
                      Positioned(
                        right: 7,
                        top: 7,
                        child: Container(
                          width: 16,
                          height: 16,
                          decoration: BoxDecoration(
                            color: p.amber,
                            shape: BoxShape.circle,
                          ),
                          child: Icon(
                            Icons.check_rounded,
                            size: 12,
                            color: p.onAmber,
                          ),
                        ),
                      ),
                  ],
                ),
              ),
            ),
          ),
        ),
      );
    }

    return SingleChildScrollView(
      padding: const EdgeInsets.all(14),
      child: Column(
        key: const Key('session-screen-panel'),
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          SessionPanelHeader(SessionTool.screen.label, status: shown.label),
          const SizedBox(height: 12),
          LayoutBuilder(
            builder: (context, box) {
              // What a tile's label has: its share of the row less the
              // tile's own side padding.
              final labelWidth =
                  (box.maxWidth - 8 * (modes.length - 1)) / modes.length - 12;
              return IntrinsicHeight(
                child: Row(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    for (final (index, mode) in modes.indexed) ...[
                      if (index > 0) const SizedBox(width: 8),
                      tile(mode, labelWidth),
                    ],
                  ],
                ),
              );
            },
          ),
          if (displaysOffered) ...[
            const SizedBox(height: 6),
            SessionPanelRow(
              key: const Key('remote-virtual-displays'),
              icon: Icons.monitor_rounded,
              title: l10n.monitors2,
              opens: true,
              onTap: displaysEnabled
                  ? () => onSelected(const SessionScreenChoice.displays())
                  : null,
            ),
          ],
          const SizedBox(height: 14),
          KeyedSubtree(
            key: const Key('remote-display-scale'),
            child: SessionPanelCaption(
              SessionTool.scale.label,
              note: l10n.windowsTextIconSize,
            ),
          ),
          const SizedBox(height: 8),
          if (session == null)
            KeepAllText(
              scaleReason,
              key: const Key('display-scale-unavailable'),
              style: uiText(size: 12, height: 1.45, color: p.muted),
            )
          else
            DisplayScalePanel(
              session: session,
              available: scaleAvailable,
              viewportPixels: viewportPixels,
              automaticResolution: automaticResolution,
              resolutionAvailable: resolutionAvailable,
              showTitle: false,
              onSelected: (choice) {
                final scale = choice.scale;
                if (scale != null) {
                  onSelected(SessionScreenChoice.scale(scale));
                }
              },
            ),
        ],
      ),
    );
  }
}

// ------------------------------------------------------------ 폴더·USB

enum SessionShareTab { folders, usb }

/// 폴더·USB for a session whose channels can be replaced while the panel is
/// open (channel-resume-v1 swaps in a new drive or usb object): [builder]
/// always gets the session's current ones, so a channel that came back is
/// not shown as the ended one.
class SessionShareFollower extends StatefulWidget {
  const SessionShareFollower({
    super.key,
    required this.session,
    required this.drive,
    required this.usb,
    required this.builder,
  });

  /// Notifies when [drive] or [usb] may have changed (the remote session).
  final Listenable session;
  final DriveSession? Function() drive;
  final UsbSession? Function() usb;
  final Widget Function(DriveSession? drive, UsbSession? usb) builder;

  @override
  State<SessionShareFollower> createState() => _SessionShareFollowerState();
}

class _SessionShareFollowerState extends State<SessionShareFollower> {
  late DriveSession? _drive = widget.drive();
  late UsbSession? _usb = widget.usb();

  @override
  void initState() {
    super.initState();
    widget.session.addListener(_changed);
  }

  @override
  void dispose() {
    widget.session.removeListener(_changed);
    super.dispose();
  }

  /// Only a replaced object rebuilds; the session also notifies in the
  /// middle of this panel's build (see [_SessionSharePanelState._changed]).
  void _changed() {
    if (identical(widget.drive(), _drive) && identical(widget.usb(), _usb)) {
      return;
    }
    void apply() {
      if (!mounted) return;
      setState(() {
        _drive = widget.drive();
        _usb = widget.usb();
      });
    }

    if (SchedulerBinding.instance.schedulerPhase ==
        SchedulerPhase.persistentCallbacks) {
      WidgetsBinding.instance.addPostFrameCallback((_) => apply());
    } else {
      apply();
    }
  }

  @override
  Widget build(BuildContext context) => widget.builder(_drive, _usb);
}

/// 폴더·USB: this PC's folders and USB devices in the VM, one tab each.
/// A side this connection cannot use stays as a tab that says why; with
/// only one side there are no tabs. The bodies are the 공유 폴더 and USB
/// 장치 panels themselves.
class SessionSharePanel extends StatefulWidget {
  const SessionSharePanel({
    super.key,
    this.drive,
    this.usb,
    this.driveUnavailable,
    this.usbUnavailable,
    this.initial = SessionShareTab.folders,
    this.dialogContext,
    this.onTab,
    this.driveWording = const DriveWording(),
    this.embedded = false,
  });
  final DriveSession? drive;
  final UsbSession? usb;

  /// The device's word and messages in the 공유 폴더 tab (Windows by default).
  final DriveWording driveWording;

  /// Inside another card that has its own title and scrolling (the Android
  /// menu card).
  final bool embedded;

  /// Why this connection has no shared folders or no USB although the
  /// administrator's or the Host's side offers it; null where it does not
  /// apply at all.
  final String? driveUnavailable, usbUnavailable;
  final SessionShareTab initial;
  final BuildContext? dialogContext;

  /// The tab now shown (remembered for the next opening).
  final ValueChanged<SessionShareTab>? onTab;

  static const width = 400.0, minHeight = 300.0;

  /// The folders the VM has mapped now.
  static int mappedFolders(DriveSession? drive) =>
      drive == null || !drive.ready || drive.closed
      ? 0
      : drive.shares
            .where(
              (share) =>
                  share.state == DriveShareState.mapped && !share.offline,
            )
            .length;

  /// The USB devices the VM has now.
  static int sharedUsb(UsbSession? usb) =>
      usb == null || !usb.ready || usb.closed
      ? 0
      : usb.entries
            .where((entry) => entry.state == UsbEntryState.shared)
            .length;

  @override
  State<SessionSharePanel> createState() => _SessionSharePanelState();
}

class _SessionSharePanelState extends State<SessionSharePanel> {
  late SessionShareTab _tab = _has(widget.initial)
      ? widget.initial
      : _has(SessionShareTab.folders)
      ? SessionShareTab.folders
      : SessionShareTab.usb;

  bool _has(SessionShareTab tab) => tab == SessionShareTab.folders
      ? widget.drive != null || widget.driveUnavailable != null
      : widget.usb != null || widget.usbUnavailable != null;

  void _select(SessionShareTab tab) {
    if (tab == _tab) return;
    setState(() => _tab = tab);
    widget.onTab?.call(tab);
  }

  @override
  void initState() {
    super.initState();
    widget.drive?.addListener(_changed);
    widget.usb?.addListener(_changed);
  }

  @override
  void didUpdateWidget(SessionSharePanel oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!identical(oldWidget.drive, widget.drive)) {
      oldWidget.drive?.removeListener(_changed);
      widget.drive?.addListener(_changed);
    }
    if (!identical(oldWidget.usb, widget.usb)) {
      oldWidget.usb?.removeListener(_changed);
      widget.usb?.addListener(_changed);
    }
  }

  @override
  void dispose() {
    widget.drive?.removeListener(_changed);
    widget.usb?.removeListener(_changed);
    super.dispose();
  }

  /// The tabs' counts follow the sessions. The USB panel starts listing as
  /// it is built and its session notifies at once, in the middle of this
  /// panel's own build: that change waits for the frame to end.
  void _changed() {
    if (!mounted) return;
    if (SchedulerBinding.instance.schedulerPhase ==
        SchedulerPhase.persistentCallbacks) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) setState(() {});
      });
    } else {
      setState(() {});
    }
  }

  Widget _tabButton(AppPalette p, SessionShareTab tab) {
    final folders = tab == SessionShareTab.folders;
    final tool = folders ? SessionTool.drives : SessionTool.usb;
    final on = tab == _tab;
    final available = folders ? widget.drive != null : widget.usb != null;
    final listenable = folders ? widget.drive : widget.usb;
    Widget button(int count) => Expanded(
      child: Semantics(
        button: true,
        selected: on,
        child: Material(
          color: on ? p.quiet : Colors.transparent,
          borderRadius: BorderRadius.circular(8),
          clipBehavior: Clip.antiAlias,
          child: InkWell(
            key: Key(folders ? 'remote-drives' : 'remote-usb'),
            onTap: () => _select(tab),
            hoverColor: p.hover,
            splashFactory: NoSplash.splashFactory,
            child: ConstrainedBox(
              constraints: const BoxConstraints(minHeight: 34),
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
                child: Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    Icon(
                      available
                          ? tool.icon
                          : folders
                          ? Icons.folder_off_rounded
                          : Icons.usb_off_rounded,
                      size: 16,
                      color: on ? p.ink : p.muted,
                    ),
                    const SizedBox(width: 7),
                    Flexible(
                      // The whole name, drawn smaller where large text
                      // leaves no room: never "Shared…".
                      child: FittedBox(
                        fit: BoxFit.scaleDown,
                        child: KeepAllText(
                          tool.label,
                          maxLines: 1,
                          style: uiText(
                            size: 13,
                            weight: FontWeight.w600,
                            color: on ? p.ink : p.muted,
                          ),
                        ),
                      ),
                    ),
                    if (count > 0) ...[
                      const SizedBox(width: 7),
                      Container(
                        key: Key(
                          folders ? 'remote-drives-count' : 'remote-usb-count',
                        ),
                        constraints: const BoxConstraints(minWidth: 17),
                        padding: const EdgeInsets.symmetric(
                          horizontal: 5,
                          vertical: 1,
                        ),
                        decoration: BoxDecoration(
                          color: p.green.withValues(alpha: .16),
                          borderRadius: BorderRadius.circular(9),
                        ),
                        child: Text(
                          '$count',
                          textAlign: TextAlign.center,
                          style: uiText(
                            size: 12,
                            height: 1.2,
                            weight: FontWeight.w600,
                            color: p.green,
                          ),
                        ),
                      ),
                    ],
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
    // The counts follow the sessions through [_changed], never through a
    // builder of their own (see there).
    return button(
      listenable == null
          ? 0
          : folders
          ? SessionSharePanel.mappedFolders(widget.drive)
          : SessionSharePanel.sharedUsb(widget.usb),
    );
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final both = _has(SessionShareTab.folders) && _has(SessionShareTab.usb);
    final folders = _tab == SessionShareTab.folders;
    final reason = folders ? widget.driveUnavailable : widget.usbUnavailable;
    // One thought: the statement, then the reason as its own sentence (a short
    // reason such as adminPermissionNeeded has no full stop of its own).
    final why = reason == null || reason.isEmpty
        ? ''
        : ' ${reason.endsWith('.') ? reason : '$reason.'}';
    final Widget body = folders && widget.drive != null
        ? DrivePanel(
            drive: widget.drive!,
            dialogContext: widget.dialogContext,
            wording: widget.driveWording,
            embedded: widget.embedded,
          )
        : !folders && widget.usb != null
        ? UsbPanel(usb: widget.usb!)
        : Padding(
            padding: const EdgeInsets.fromLTRB(16, 14, 16, 18),
            child: KeepAllText(
              folders
                  ? l10n.cantUseSharedFolders(why)
                  : l10n.cantUseUsbDevices(why),
              key: Key(folders ? 'share-drives-reason' : 'share-usb-reason'),
              style: uiText(size: 13, height: 1.5, color: p.muted),
            ),
          );
    return Column(
      key: const Key('session-share-panel'),
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        if (both)
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 12, 12, 0),
            child: Container(
              padding: const EdgeInsets.all(3),
              decoration: BoxDecoration(
                color: p.tileWell,
                borderRadius: BorderRadius.circular(11),
                border: Border.all(color: p.tileLine),
              ),
              child: Row(
                children: [
                  _tabButton(p, SessionShareTab.folders),
                  const SizedBox(width: 2),
                  _tabButton(p, SessionShareTab.usb),
                ],
              ),
            ),
          ),
        Flexible(child: body),
      ],
    );
  }
}

// ----------------------------------------------------------------- 설정

/// What a row of the 설정 panel asked for (바 열어 두기 applies in place).
enum SessionSettingsChoice { quality, mouseLock, guide }

/// 설정: what is used now and then. 화질·소리 opens the streaming choices,
/// 마우스 잠금 and 바 열어 두기 are switches, 사용안내 opens the guide.
/// A row that does not apply to this connection is left out; one that
/// cannot be used yet is greyed with the reason.
class SessionSettingsPanel extends StatefulWidget {
  const SessionSettingsPanel({
    super.key,
    required this.onSelected,
    this.quality = false,
    this.qualitySummary,
    this.qualityReason,
    this.mouseLock,
    this.mouseLockUsb = false,
    this.pinned = false,
    this.onPinned,
    this.guide = false,
    this.guideReason,
    String? guideSubtitle,
    this.embedded = false,
    this.showPin = true,
    this.mouseLockReason,
    this.mouseLockHint,
    this.extra = const [],
    this.qualityKey = const Key('remote-stream-settings'),
    this.mouseLockKey = const Key('session-game-mouse'),
    this.guideKey = const Key('remote-session-guide'),
  }) : _guideSubtitle = guideSubtitle; // ignore: prefer_initializing_formals
  final ValueChanged<SessionSettingsChoice> onSelected;

  /// Inside another card that has its own title, padding and scrolling (the
  /// Android menu card): only the rows.
  final bool embedded;

  /// 툴바 열어 두기 applies (not where the tools are always there).
  final bool showPin;

  /// Why 마우스 잠금 cannot be switched now (e.g. no mouse is connected).
  final String? mouseLockReason;

  /// What stands under 마우스 잠금 instead of the Windows key combination.
  final String? mouseLockHint;

  /// Rows of the platform's own (Android's 입력 방식), after the switches
  /// and before 사용안내.
  final List<Widget> extra;
  final String? _guideSubtitle;
  String get guideSubtitle => _guideSubtitle ?? l10n.printerUsbSharedFoldersCopy;
  final Key qualityKey, mouseLockKey, guideKey;

  /// 화질·소리 is offered; [qualityReason] greys it.
  final bool quality;
  final String? qualitySummary, qualityReason;

  /// 마우스 잠금's state; null where this connection has none.
  final bool? mouseLock;
  final bool mouseLockUsb;
  final bool pinned;
  final ValueChanged<bool>? onPinned;

  /// 사용안내 is offered; [guideReason] greys it.
  final bool guide;
  final String? guideReason;

  static const width = 336.0, minHeight = 250.0;

  @override
  State<SessionSettingsPanel> createState() => _SessionSettingsPanelState();
}

class _SessionSettingsPanelState extends State<SessionSettingsPanel> {
  late bool _pinned = widget.pinned;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final lock = widget.mouseLock;
    final rows = Column(
      key: const Key('session-settings-panel'),
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        if (!widget.embedded)
          Padding(
            padding: const EdgeInsets.fromLTRB(8, 0, 8, 8),
            child: SessionPanelHeader(SessionTool.more.label),
          ),
        if (widget.quality)
          SessionPanelRow(
            key: widget.qualityKey,
            icon: SessionTool.settings.icon,
            title: SessionTool.settings.label,
            subtitle: widget.qualitySummary,
            opens: true,
            disabledReason: widget.qualityReason,
            onTap: widget.qualityReason != null
                ? null
                : () => widget.onSelected(SessionSettingsChoice.quality),
          ),
        if (lock != null)
          SessionPanelRow(
            key: widget.mouseLockKey,
            icon: SessionTool.gameMouse.icon,
            title: SessionTool.gameMouse.label,
            subtitle: widget.mouseLockHint,
            disabledReason: widget.mouseLockReason,
            subtitleWidget: widget.mouseLockHint != null
                ? null
                : Wrap(
                    spacing: 5,
                    runSpacing: 3,
                    crossAxisAlignment: WrapCrossAlignment.center,
                    children: [
                      const SessionKeycaps(SessionKeycaps.mouseLock),
                      KeepAllText(
                        lock
                            ? widget.mouseLockUsb
                                  ? l10n.turnOffUsbMouse
                                  : l10n.turnOffNormalInput
                            : l10n.turnOff,
                        style: uiText(size: 12, height: 1.35, color: p.muted),
                      ),
                    ],
                  ),
            toggled: lock,
            onTap: widget.mouseLockReason != null
                ? null
                : () => widget.onSelected(SessionSettingsChoice.mouseLock),
          ),
        if (widget.showPin)
          SessionPanelRow(
            key: const Key('pin-session-toolbar'),
            icon: _pinned ? Icons.push_pin : SessionTool.pin.icon,
            title: SessionTool.pin.label,
            subtitle: l10n.staysOpenWhenYouClick,
            toggled: _pinned,
            onTap: widget.onPinned == null
                ? null
                : () {
                    setState(() => _pinned = !_pinned);
                    widget.onPinned!(_pinned);
                  },
          ),
        ...widget.extra,
        if (widget.guide) ...[
          Container(
            height: 1,
            margin: const EdgeInsets.symmetric(vertical: 6),
            color: p.tileLine,
          ),
          SessionPanelRow(
            key: widget.guideKey,
            icon: SessionTool.guide.icon,
            title: SessionTool.guide.label,
            subtitle: widget.guideSubtitle,
            opens: true,
            disabledReason: widget.guideReason,
            onTap: widget.guideReason != null
                ? null
                : () => widget.onSelected(SessionSettingsChoice.guide),
          ),
        ],
      ],
    );
    return widget.embedded
        ? rows
        : SingleChildScrollView(
            padding: const EdgeInsets.fromLTRB(8, 14, 8, 8),
            child: rows,
          );
  }
}

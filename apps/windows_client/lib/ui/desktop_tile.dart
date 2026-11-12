import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../data/desktop_thumbnails.dart';
import '../models/desktop_machine.dart';
import '../models/session_display_mode.dart';
import '../models/stream_options.dart'
    show audioDeviceMissingText, audioDevicePendingText;
import 'app_theme.dart';
import 'desktop_art.dart';
import 'desktop_menu.dart';
import 'desktop_readiness.dart';
import 'korean_text.dart';
import 'vm_resource_bars.dart';
import 'host_update_diagnostics_dialog.dart';
import '../l10n/l10n.dart';

/// A running "power on, then connect" request for one desktop.
class StartProgress {
  const StartProgress({required this.stage, required this.elapsed});

  /// 0 = VM 켜기, 1 = Windows 시작, 2 = 연결.
  final int stage;
  final Duration elapsed;

  static List<String> get stages => [l10n.power2, l10n.preparing4, l10n.connect2];

  String get clock {
    final seconds = elapsed.inSeconds;
    return '${seconds ~/ 60}:${(seconds % 60).toString().padLeft(2, '0')}';
  }

  String get waiting => switch (stage) {
    0 => l10n.starting2,
    1 => l10n.preparing3,
    _ => l10n.connecting,
  };
}

String desktopOsLabel(DesktopMachine m) {
  final spec = m.specification.split(' · ').first.trim();
  return spec.isEmpty ? m.kindLabel : spec;
}

String streamModeLabel(SessionDisplayMode mode) => switch (mode) {
  SessionDisplayMode.windowed => l10n.windowed,
  SessionDisplayMode.fullscreen => l10n.fullScreen,
  SessionDisplayMode.allMonitors => l10n.allMonitors,
};

/// The body below a tile picture, sized from the text scale so every tile
/// keeps the same height whatever its state: the name row, the CPU and
/// memory strip, the status card and the buttons. By [touch]
/// (Android) ⏻, ⋯ and the buttons are 48 dp.
double tileBodyHeight(TextScaler scaler, {bool touch = false}) {
  double line(double size, double height) => scaler.scale(size) * height;
  final name = math.max(touch ? 48.0 : 32.0, line(17, 1.35));
  final resources = VmResourceStrip.heightFor(scaler, touch: touch);
  final detailLines = scaler.scale(12.5) > 12.5 * 1.3 ? 2 : 1;
  final status = math.max(
    54.0,
    line(13, 1.4) + line(12.5, 1.4) * detailLines + 18,
  );
  final buttons = math.max(touch ? 48.0 : 44.0, line(14, 1.3) + 20);
  return 14 +
      name +
      _resourceGap.top +
      resources +
      _resourceGap.bottom +
      status +
      12 +
      buttons +
      16;
}

/// Space above and below the CPU and memory strip.
const _resourceGap = (top: 6.0, bottom: 12.0);

/// The launcher's tile grid, one rule on Windows and Android (one card
/// design with automatic responsive columns; Android renders the same
/// tile): as many columns of at least 288 (growing with
/// the text, at most 3) as fit [width], [desktopTileGap] apart, and one
/// size for every tile: the picture, 5:3 of the tile but at most 40 % of
/// the [viewport]'s height (a fifth under 500: a phone on its side keeps the
/// main button on screen), and the body below it ([tileBodyHeight]).
({int columns, double width, double picture, double height}) desktopTileGrid(
  double width,
  TextScaler scaler, {
  required Size viewport,
  bool previews = true,
  bool touch = false,
}) {
  const gap = desktopTileGap;
  final minimum = 288.0 * scaler.scale(1).clamp(1.0, 1.6);
  final columns = ((width + gap) / (minimum + gap)).floor().clamp(1, 3);
  final tile = (width - (columns - 1) * gap) / columns;
  // At most 40 % of the window's height (without the limit, one column at
  // 150 % text in an 800 x 600 window draws a 444 px picture and pushes the
  // main button off screen).
  final picture = !previews
      ? 0.0
      : math.min(
          tile * 3 / 5,
          viewport.height * (viewport.height < 500 ? .2 : .4),
        );
  return (
    columns: columns,
    width: tile,
    picture: picture,
    // The tile's 1 px edge above and below.
    height: picture + tileBodyHeight(scaler, touch: touch) + 2,
  );
}

const desktopTileGap = 24.0;

Color _phaseDot(AppPalette p, TilePhase phase) => switch (phase) {
  TilePhase.ready => p.green,
  TilePhase.attention => p.warn,
  TilePhase.power => p.amber,
  _ => p.faint,
};

/// One assigned desktop as a large tile. Only the main button and the ▶ on
/// the picture connect (a click elsewhere on the tile does not); no key on
/// the focused tile does. ▾ chooses how to
/// connect, ⏻ holds the power actions, ⋯ manages the VM, and a right click
/// shows the connection and management parts.
///
/// Android renders this same tile with
/// [touch]: 48 dp targets, the ▶ shown without a hover, the whole status
/// card opening 자세히, and menus with 48 dp rows that say why a row is
/// greyed under its label instead of in a hover tooltip.
class DesktopTile extends StatefulWidget {
  const DesktopTile({
    super.key,
    required this.machine,
    required this.readiness,
    required this.actions,
    required this.mode,
    this.progress,
    this.focusNode,
    this.preview = false,
    this.thumbnail,
    this.showPreview = true,
    this.previewNote,
    this.pictureHeight,
    this.touch = false,
    this.focusStyle,
  });

  final DesktopMachine machine;
  final DesktopReadiness readiness;
  final DesktopActions actions;

  /// The default connection mode; null where there are no window modes
  /// (Android: one full screen).
  final SessionDisplayMode? mode;
  final StartProgress? progress;
  final FocusNode? focusNode;
  final bool preview;

  /// The picture's height from [desktopTileGrid]; null: 5:3 of the width.
  final double? pictureHeight;

  /// By finger (Android), see above.
  final bool touch;

  /// Paints keyboard focus on the tile's buttons (the Android TV remote).
  final ButtonStyle Function(BuildContext context, ButtonStyle style)?
  focusStyle;

  /// The last streamed screen of this desktop, or the broker's recent picture
  /// of it (desktop-preview-v1), when the client has one.
  final DesktopThumbnail? thumbnail;
  final bool showPreview;

  /// Why there is no picture (잠겨 있어요, 로그인 화면, ...); null: today's text.
  final String? previewNote;

  @override
  State<DesktopTile> createState() => _DesktopTileState();
}

class _DesktopTileState extends State<DesktopTile> {
  final _menu = MenuController();
  final _surface = GlobalKey();
  bool _hovered = false, _focused = false;
  DesktopMenuKind _menuKind = DesktopMenuKind.all;
  Rect? _menuAnchor;
  int _menuRequest = 0;

  bool get _hot => _hovered || _focused || _menu.isOpen;

  void _openMenuAt(Rect anchor, DesktopMenuKind kind) {
    if (!widget.actions.menu) {
      return;
    }
    final request = ++_menuRequest, id = widget.machine.id;
    if (_menu.isOpen) {
      _menu.close();
      if (kind == _menuKind) {
        return;
      }
    }
    setState(() {
      _menuKind = kind;
      _menuAnchor = anchor;
    });
    // The exact-size anchor has to be laid out before the framework computes
    // its edge flip. Opening against the old full-card anchor reproduces the
    // upper-row popup even when the supplied local point itself was correct.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted &&
          request == _menuRequest &&
          widget.machine.id == id &&
          widget.actions.menu) {
        _menu.open();
      }
    });
  }

  void _openMenuBelow(BuildContext anchor, DesktopMenuKind kind) {
    final box = anchor.findRenderObject() as RenderBox?;
    final surface = _surface.currentContext?.findRenderObject() as RenderBox?;
    if (box == null || surface == null || !box.hasSize || !surface.hasSize) {
      return;
    }
    _openMenuAt(
      Rect.fromPoints(
        surface.globalToLocal(box.localToGlobal(Offset.zero)),
        surface.globalToLocal(
          box.localToGlobal(box.size.bottomRight(Offset.zero)),
        ),
      ),
      kind,
    );
  }

  void _openContextMenu(Offset globalPosition) {
    final surface = _surface.currentContext?.findRenderObject() as RenderBox?;
    if (surface == null || !surface.hasSize) {
      return;
    }
    final point = surface.globalToLocal(globalPosition);
    _openMenuAt(Rect.fromLTWH(point.dx, point.dy, 1, 1), DesktopMenuKind.all);
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final m = widget.machine;
    final r = widget.readiness;
    final a = widget.actions;
    final progress = widget.progress;
    final scaler = MediaQuery.textScalerOf(context);
    final hot = _hot && (a.primary != null || r.phase == TilePhase.attention);
    final touch = widget.touch;
    final body = SizedBox(
      height: tileBodyHeight(scaler, touch: touch),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(18, 14, 18, 16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            DesktopNameRow(
              machine: m,
              menu: a.menu,
              power: a.menu && a.showPower,
              update: a.hostUpdate != null,
              showFavorite: !widget.showPreview,
              touch: touch,
              focusStyle: widget.focusStyle,
              onMenu: (anchor) =>
                  _openMenuBelow(anchor, DesktopMenuKind.manage),
              onPower: (anchor) =>
                  _openMenuBelow(anchor, DesktopMenuKind.power),
            ),
            SizedBox(height: _resourceGap.top),
            VmResourceStrip(
              key: Key('resources-${m.id}'),
              sample: m.resources,
              running: m.resourcesRunning,
              pc: m.kind == MachineKind.pc,
              example: widget.preview,
              touch: touch,
            ),
            SizedBox(height: _resourceGap.bottom),
            DesktopStatusArea(
              machine: m,
              readiness: r,
              mode: widget.mode,
              progress: progress,
              preview: widget.preview,
              onDetails: a.details,
              touch: touch,
            ),
            const Spacer(),
            _ActionRow(
              machine: m,
              readiness: r,
              actions: a,
              mode: widget.mode,
              progress: progress,
              hot: hot,
              preview: widget.preview,
              onMenu: _openMenuBelow,
              minimumHeight: touch ? 48 : 44,
              focusStyle: widget.focusStyle,
              touch: touch,
            ),
          ],
        ),
      ),
    );
    final picture = DesktopThumbnailView(
      machine: m,
      readiness: r,
      progress: progress,
      // No hover by finger: the ▶ is always there on a ready tile.
      hot: (hot || touch) && r.phase == TilePhase.ready,
      preview: widget.preview,
      picture: widget.thumbnail,
      previewNote: widget.previewNote,
      // Android keeps no picture of its own connections.
      emptyHint: !touch,
      onPlay:
          progress == null &&
              (r.phase == TilePhase.ready || r.phase == TilePhase.off)
          ? a.primary
          : null,
    );
    final tile = AnimatedContainer(
      duration: const Duration(milliseconds: 150),
      transform: Matrix4.translationValues(0, hot ? -3 : 0, 0),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(18),
        boxShadow: [
          if (hot) BoxShadow(color: p.amberSoft, spreadRadius: 3),
          BoxShadow(
            color: p.shadow.withValues(alpha: hot ? .55 : .22),
            blurRadius: hot ? 44 : 18,
            spreadRadius: hot ? -16 : -10,
            offset: Offset(0, hot ? 22 : 10),
          ),
        ],
      ),
      child: Container(
        key: Key('card-${m.id}'),
        clipBehavior: Clip.antiAlias,
        // Frosted over the launcher scene: a translucent fill, no blur.
        decoration: BoxDecoration(
          color: p.tileGlass,
          borderRadius: BorderRadius.circular(18),
          border: Border.all(color: hot ? p.amberLine : p.glassEdge),
        ),
        child: Material(
          type: MaterialType.transparency,
          child: _TileSurface(
            key: Key('tile-${m.id}'),
            focusNode: widget.focusNode,
            focusable: !touch,
            onHover: (value) => setState(() => _hovered = value),
            onFocus: (value) => setState(() => _focused = value),
            onSecondaryTapUp: a.menu && !touch
                ? (details) => _openContextMenu(details.globalPosition)
                : null,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                if (widget.showPreview)
                  widget.pictureHeight == null
                      ? AspectRatio(aspectRatio: 5 / 3, child: picture)
                      : SizedBox(height: widget.pictureHeight, child: picture),
                body,
              ],
            ),
          ),
        ),
      ),
    );
    return Semantics(
      container: true,
      label: '${m.name} · ${r.headline}',
      child: KeyedSubtree(
        key: _surface,
        child: DesktopMenu(
          machine: m,
          actions: a,
          mode: widget.mode,
          controller: _menu,
          kind: _menuKind,
          anchorRect: _menuAnchor ?? Rect.zero,
          touch: touch,
          child: tile,
        ),
      ),
    );
  }
}

/// The tile or row surface: hover, keyboard focus and the right-click menu.
/// Neither a click nor a key connects here, only the buttons do: Enter on a
/// focused tile starts no connection.
class _TileSurface extends StatelessWidget {
  const _TileSurface({
    super.key,
    required this.focusNode,
    required this.onHover,
    required this.onFocus,
    required this.onSecondaryTapUp,
    required this.child,
    this.focusable = true,
  });
  final bool focusable;
  final FocusNode? focusNode;
  final ValueChanged<bool> onHover, onFocus;
  final GestureTapUpCallback? onSecondaryTapUp;
  final Widget child;

  @override
  Widget build(BuildContext context) => Focus(
    focusNode: focusNode,
    // By finger (Android, its TV remote included) focus moves between the
    // tile's buttons only.
    canRequestFocus: focusable,
    skipTraversal: !focusable,
    onFocusChange: onFocus,
    child: MouseRegion(
      onEnter: (_) => onHover(true),
      onExit: (_) => onHover(false),
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onSecondaryTapUp: onSecondaryTapUp,
        child: child,
      ),
    ),
  );
}

class DesktopNameRow extends StatelessWidget {
  const DesktopNameRow({
    super.key,
    required this.machine,
    required this.menu,
    required this.power,
    required this.onMenu,
    required this.onPower,
    this.update = false,
    this.touch = false,
    this.compact = false,
    this.showFavorite = false,
    this.focusStyle,
  });
  final DesktopMachine machine;

  /// A newer Host can be installed now: ⋯ carries a badge.
  final bool menu, power, update;

  /// Touch screens (Android): 48 dp buttons instead of the desktop's 32 px.
  final bool touch;
  final bool compact, showFavorite;
  final void Function(BuildContext anchor) onMenu, onPower;

  /// Paints keyboard focus on ⏻ and ⋯ (the Android TV remote).
  final ButtonStyle Function(BuildContext context, ButtonStyle style)?
  focusStyle;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final side = touch ? 48.0 : 32.0;
    final base = IconButton.styleFrom(
      foregroundColor: p.muted,
      minimumSize: Size(side, side),
      maximumSize: Size(side, side),
      padding: EdgeInsets.zero,
      tapTargetSize: MaterialTapTargetSize.shrinkWrap,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(9)),
    );
    final iconStyle = focusStyle?.call(context, base) ?? base;
    return Row(
      children: [
        Expanded(
          // Let the badge keep its natural width; only the name may truncate.
          child: LayoutBuilder(
            builder: (context, bounds) {
              final localPc = machine.kind == MachineKind.pc;
              final badgeStyle = uiText(
                size: 12,
                weight: FontWeight.w500,
                color: p.muted,
              );
              final nameStyle = TextStyle(
                color: p.ink,
                fontSize: compact ? 14 : 17,
                height: 1.25,
                fontWeight: FontWeight.w600,
                // Small text is never tightened (compact 14 px).
                letterSpacing: compact ? 0 : -.2,
              );
              final osStyle = TextStyle(color: p.muted, fontSize: 12.5);
              final name = Tooltip(
                message: machine.name,
                child: KeepAllText(
                  machine.name,
                  key: Key('desktop-name-${machine.id}'),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: nameStyle,
                ),
              );
              // The name comes first: when it and the OS label do not both
              // fit, the OS label gives way (otherwise a long name and its
              // OS are both cut, "회계팀 업무용 Windows… Windows 11 …").
              double natural(String text, TextStyle style) {
                final painter = TextPainter(
                  text: TextSpan(
                    text: keepAll(text),
                    style: DefaultTextStyle.of(context).style.merge(style),
                  ),
                  textDirection: TextDirection.ltr,
                  textScaler: MediaQuery.textScalerOf(context),
                  maxLines: 1,
                )..layout();
                final width = painter.width;
                painter.dispose();
                return width;
              }

              final star = showFavorite && machine.organize.favorite ? 20 : 0;
              final osShown =
                  localPc ||
                  star +
                          natural(machine.name, nameStyle) +
                          10 +
                          natural(desktopOsLabel(machine), osStyle) <=
                      bounds.maxWidth;
              return Row(
                crossAxisAlignment: CrossAxisAlignment.baseline,
                textBaseline: TextBaseline.alphabetic,
                children: [
                  if (showFavorite && machine.organize.favorite) ...[
                    Icon(
                      Icons.star_rounded,
                      key: Key('favorite-star-${machine.id}'),
                      size: 15,
                      color: p.amber,
                    ),
                    const SizedBox(width: 5),
                  ],
                  if (localPc || !osShown) Expanded(child: name) else name,
                  if (osShown) SizedBox(width: localPc ? 8 : 10),
                  if (!osShown)
                    const SizedBox.shrink()
                  else if (localPc)
                    Container(
                      key: Key('desktop-kind-${machine.id}'),
                      padding: const EdgeInsets.symmetric(
                        horizontal: 6,
                        vertical: 3,
                      ),
                      decoration: BoxDecoration(
                        border: Border.all(color: p.tileLine),
                        borderRadius: BorderRadius.circular(4),
                      ),
                      child: KeepAllText(
                        l10n.localPc,
                        maxLines: 1,
                        style: badgeStyle,
                      ),
                    )
                  else
                    Expanded(
                      child: KeepAllText(
                        desktopOsLabel(machine),
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: osStyle,
                      ),
                    ),
                ],
              );
            },
          ),
        ),
        if (power)
          Builder(
            builder: (anchor) => IconButton(
              key: Key('desktop-power-${machine.id}'),
              tooltip: l10n.power,
              onPressed: () => onPower(anchor),
              style: iconStyle,
              icon: Icon(
                Icons.power_settings_new_rounded,
                size: 18,
                color: powerColor(p, machine),
              ),
            ),
          ),
        if (menu)
          Builder(
            builder: (anchor) => IconButton(
              key: Key('desktop-more-${machine.id}'),
              tooltip: machine.kind == MachineKind.pc
                  ? (update ? l10n.manageLocalPcHostUpdate : l10n.manageLocalPc)
                  : (update ? l10n.manageDesktopHostUpdateAvailable : l10n.manageDesktop),
              onPressed: () => onMenu(anchor),
              style: iconStyle,
              icon: Badge(
                key: Key('host-update-badge-${machine.id}'),
                isLabelVisible: update,
                smallSize: 7,
                backgroundColor: p.red,
                child: const Icon(Icons.more_horiz_rounded, size: 20),
              ),
            ),
          ),
      ],
    );
  }
}

/// The ⏻ button's tint: green while running, amber while a power action
/// runs, grey otherwise.
Color powerColor(AppPalette p, DesktopMachine m) => m.powerBusy
    ? p.amber
    // A PC is on while its Host is connected (pc-power-v1).
    : (m.kind == MachineKind.pc
          ? m.hostOnline == true
          : m.powerState == 'running' && !m.powerStale)
    ? p.green
    : p.muted;

class DesktopStatusArea extends StatelessWidget {
  const DesktopStatusArea({
    super.key,
    required this.machine,
    required this.readiness,
    required this.mode,
    required this.progress,
    required this.preview,
    required this.onDetails,
    this.detailOverride,
    this.touch = false,
  });
  final DesktopMachine machine;
  final DesktopReadiness readiness;

  /// The default connection mode named on a ready tile; null (Android, which
  /// has no window modes) leaves the readiness detail.
  final SessionDisplayMode? mode;
  final StartProgress? progress;
  final bool preview;
  final VoidCallback? onDetails;

  /// A caller-owned second line (e.g. "waiting for the previous cleanup").
  final String? detailOverride;

  /// By finger (Android): the whole card opens 자세히, not only its small
  /// word, so the target is the card's 54 dp+.
  final bool touch;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final r = readiness;
    final scaler = MediaQuery.textScalerOf(context);
    final progress = this.progress;
    final warn = progress == null && r.phase == TilePhase.attention;
    final id = machine.id;
    Widget lead;
    Color headColor;
    switch (progress == null ? r.phase : TilePhase.power) {
      case TilePhase.ready:
        lead = Icon(Icons.check_rounded, size: 15, color: p.green);
        headColor = p.green;
      case TilePhase.attention:
        lead = Icon(Icons.warning_amber_rounded, size: 15, color: p.warn);
        headColor = p.warn;
      case TilePhase.off:
        lead = Container(
          width: 9,
          height: 9,
          decoration: BoxDecoration(
            shape: BoxShape.circle,
            border: Border.all(color: p.faint, width: 2),
          ),
        );
        headColor = p.ink;
      case TilePhase.power:
      case TilePhase.waiting:
        lead = Icon(Icons.hourglass_top_rounded, size: 14, color: p.muted);
        headColor = p.ink;
    }
    final Widget headline;
    final String detail;
    if (progress != null) {
      headline = Text.rich(
        TextSpan(
          children: [
            for (var i = 0; i < StartProgress.stages.length; i++) ...[
              if (i > 0)
                TextSpan(
                  text: '  →  ',
                  style: TextStyle(color: p.faint),
                ),
              if (i < progress.stage)
                WidgetSpan(
                  alignment: PlaceholderAlignment.middle,
                  child: Padding(
                    padding: const EdgeInsets.only(right: 3),
                    child: Icon(Icons.check_rounded, size: 14, color: p.green),
                  ),
                ),
              TextSpan(
                text: StartProgress.stages[i],
                style: TextStyle(
                  color: i < progress.stage
                      ? p.green
                      : i == progress.stage
                      ? p.ink
                      : p.faint,
                  fontWeight: i == progress.stage
                      ? FontWeight.w600
                      : FontWeight.w600,
                ),
              ),
            ],
          ],
        ),
        key: Key('start-progress-$id'),
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
        style: const TextStyle(fontSize: 13),
      );
      detail =
          l10n.step3ElapsedDesktopStays(progress.stage + 1, progress.clock);
      lead = Icon(Icons.bolt_rounded, size: 15, color: p.amber);
    } else {
      headline = KeepAllText(
        r.headline,
        key: Key('status-$id'),
        maxLines: 1,
        overflow: TextOverflow.ellipsis,
        style: TextStyle(
          color: headColor,
          fontSize: 13,
          fontWeight: FontWeight.w600,
        ),
      );
      // An administrator's remote Host update replaces the detail line
      // while it runs and briefly after it finished; a connection that
      // holds the VM comes before the default connection mode.
      // While the update keeps the Host away, the readiness line already
      // says how far it got; once the Host did not come back, what to check.
      detail =
          (r.hostUpdating || r.hostMissing
              ? r.detail
              : machine.hostUpdate.tileStatus()) ??
          detailOverride ??
          (r.phase == TilePhase.ready && !preview && !r.console
              ? machine.use.labelFor(machine.useAccount) ??
                    (mode == null
                        ? r.detail
                        : l10n.defaultStreaming(streamModeLabel(mode!)))
              : r.detail);
    }
    final hostUpdateLine =
        progress == null &&
        !r.hostMissing &&
        machine.hostUpdate.tileStatus() != null;
    final use =
        progress == null &&
            !hostUpdateLine &&
            r.phase == TilePhase.ready &&
            !preview &&
            !r.console
        ? machine.use
        : DesktopUse.idle;
    final useColor = use == DesktopUse.other ? p.amberInk : p.blue;
    final detailColor = use.active
        ? useColor
        : !hostUpdateLine
        ? (warn ? p.ink.withValues(alpha: .85) : p.muted)
        : machine.hostUpdate.failed
        ? p.red
        : machine.hostUpdate.completed
        ? p.green
        : p.amberInk;
    final hasDetails =
        progress == null &&
        r.phase != TilePhase.ready &&
        r.phase != TilePhase.off &&
        onDetails != null;
    final detailsLabel = Padding(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          if (r.cleanupStalled) ...[
            Icon(
              Icons.warning_amber_rounded,
              key: Key('stream-state-$id-warning'),
              size: 12,
              color: p.warn,
            ),
            const SizedBox(width: 3),
          ],
          KeepAllText(
            l10n.details,
            style: TextStyle(
              fontSize: 12,
              fontWeight: FontWeight.w600,
              color: warn ? p.warn : p.amberInk,
            ),
          ),
        ],
      ),
    );
    final details = !hasDetails
        ? null
        : Padding(
            padding: const EdgeInsets.only(left: 6),
            child: touch
                ? detailsLabel
                : InkWell(
                    key: Key('stream-state-$id'),
                    onTap: onDetails,
                    borderRadius: BorderRadius.circular(6),
                    child: detailsLabel,
                  ),
          );
    final card = Container(
      constraints: BoxConstraints(
        minHeight: math.max(
          54,
          scaler.scale(13) * 1.4 +
              scaler.scale(12.5) *
                  1.4 *
                  (scaler.scale(12.5) > 12.5 * 1.3 ? 2 : 1) +
              18,
        ),
      ),
      padding: EdgeInsets.symmetric(
        horizontal: 12,
        vertical: hostUpdateLine && machine.hostUpdate.failed ? 4 : 8,
      ),
      decoration: BoxDecoration(
        color: warn ? p.warnSoft : p.tileWell,
        borderRadius: BorderRadius.circular(11),
        border: warn ? Border.all(color: p.warnLine) : null,
      ),
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              SizedBox(width: 16, child: Center(child: lead)),
              const SizedBox(width: 6),
              Expanded(child: headline),
              ?details,
            ],
          ),
          const SizedBox(height: 2),
          Tooltip(
            message: use.hintFor(machine.useAccount) ?? detail,
            waitDuration: const Duration(milliseconds: 700),
            child: Row(
              children: [
                if (use.active) ...[
                  Icon(
                    use == DesktopUse.mine
                        ? Icons.person_rounded
                        : Icons.people_alt_rounded,
                    key: Key('desktop-in-use-$id'),
                    size: 14,
                    color: useColor,
                  ),
                  const SizedBox(width: 5),
                ],
                Expanded(
                  child: KeepAllText(
                    detail,
                    key: Key(
                      hostUpdateLine
                          ? 'host-update-status-$id'
                          : 'status-detail-$id',
                    ),
                    maxLines: scaler.scale(12.5) > 12.5 * 1.3 ? 2 : 1,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      fontSize: 12.5,
                      height: 1.4,
                      color: detailColor,
                      fontWeight: use.active ? FontWeight.w600 : null,
                    ),
                  ),
                ),
                if (hostUpdateLine && machine.hostUpdate.failed)
                  TextButton(
                    key: Key('host-update-diagnostics-$id'),
                    onPressed: () =>
                        showHostUpdateDiagnostics(context, machine),
                    style: TextButton.styleFrom(
                      foregroundColor: p.red,
                      padding: const EdgeInsets.symmetric(horizontal: 6),
                      minimumSize: const Size(0, 26),
                      tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    ),
                    child: KeepAllText(l10n.diagnostics),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
    return touch && hasDetails
        ? Material(
            type: MaterialType.transparency,
            child: InkWell(
              key: Key('stream-state-$id'),
              onTap: onDetails,
              borderRadius: BorderRadius.circular(11),
              child: card,
            ),
          )
        : card;
  }
}

/// The main button's wording for an attention state. A reboot is only ever
/// offered for a restart-required state, so it says what it does.
String footerActionLabel(ReadinessAction action) => switch (action) {
  ReadinessAction.start => l10n.start,
  ReadinessAction.reboot => l10n.restart2,
  ReadinessAction.refresh => l10n.refresh,
  ReadinessAction.console => l10n.openConsole2,
  ReadinessAction.releaseConsole => l10n.disconnectConsoleConnect2,
};

IconData footerActionIcon(ReadinessAction action) => switch (action) {
  ReadinessAction.start => Icons.power_settings_new_rounded,
  ReadinessAction.reboot => Icons.restart_alt_rounded,
  ReadinessAction.refresh => Icons.refresh_rounded,
  ReadinessAction.console => Icons.settings_remote_outlined,
  ReadinessAction.releaseConsole => Icons.swap_horiz_rounded,
};

/// The fixed-height button row, one structure for every state: the main
/// button (stream / 켜기 / progress / the concrete fix, only its label,
/// icon and action change) plus the split ▾ for other connection methods.
/// While powering on, a cancel button takes its place.
/// The existing state-aware launch control, shared by every launcher view.
/// Menus and connection confirmations stay with the caller's actions.
class DesktopLaunchButton extends StatelessWidget {
  const DesktopLaunchButton({
    super.key,
    required this.machine,
    required this.readiness,
    required this.actions,
    required this.mode,
    required this.onMenu,
    this.progress,
    this.preview = false,
  });
  final DesktopMachine machine;
  final DesktopReadiness readiness;
  final DesktopActions actions;
  final SessionDisplayMode mode;
  final StartProgress? progress;
  final bool preview;
  final void Function(BuildContext anchor, DesktopMenuKind kind) onMenu;

  static double heightFor(TextScaler scaler) =>
      math.max(48.0, scaler.scale(14) * 1.3 + 20);

  @override
  Widget build(BuildContext context) => _ActionRow(
    machine: machine,
    readiness: readiness,
    actions: actions,
    mode: mode,
    progress: progress,
    hot: false,
    preview: preview,
    onMenu: onMenu,
    minimumHeight: 48,
  );
}

class _ActionRow extends StatelessWidget {
  const _ActionRow({
    required this.machine,
    required this.readiness,
    required this.actions,
    required this.mode,
    required this.progress,
    required this.hot,
    required this.preview,
    required this.onMenu,
    this.compact = false,
    this.minimumHeight = 44,
    this.focusStyle,
    this.touch = false,
  });
  final DesktopMachine machine;
  final DesktopReadiness readiness;
  final DesktopActions actions;

  /// Null: no window modes (Android).
  final SessionDisplayMode? mode;
  final StartProgress? progress;
  final bool hot, preview, compact;
  final double minimumHeight;
  final void Function(BuildContext anchor, DesktopMenuKind kind) onMenu;
  final ButtonStyle Function(BuildContext context, ButtonStyle style)?
  focusStyle;

  /// By finger (Android): a 48 dp ▾.
  final bool touch;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final r = readiness;
    final id = machine.id;
    final mode = this.mode;
    final scaler = MediaQuery.textScalerOf(context);
    final height = compact
        ? math.max(34.0, scaler.scale(13) * 1.3 + 12)
        : math.max(minimumHeight, scaler.scale(14) * 1.3 + 20);
    final text = Theme.of(context).textTheme.labelLarge
        ?.copyWith(fontSize: compact ? 13 : 14, fontWeight: FontWeight.w600);
    ButtonStyle focus(ButtonStyle style) =>
        focusStyle?.call(context, style) ?? style;
    ButtonStyle filled(bool emphasized, {bool split = false}) =>
        FilledButton.styleFrom(
          minimumSize: Size(0, height),
          maximumSize: Size(double.infinity, height),
          padding: const EdgeInsets.symmetric(horizontal: 10),
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          backgroundColor: emphasized ? p.amber : p.quiet,
          foregroundColor: emphasized ? p.onAmber : p.ink,
          disabledBackgroundColor: p.quiet,
          disabledForegroundColor: p.faint,
          textStyle: text,
          shape: RoundedRectangleBorder(
            borderRadius: split
                ? const BorderRadius.horizontal(left: Radius.circular(12))
                : BorderRadius.circular(12),
          ),
        );
    if (progress != null) {
      return SizedBox(
        height: height,
        child: FilledButton.icon(
          key: Key('cancel-start-$id'),
          onPressed: actions.cancel,
          style: focus(filled(false)),
          icon: const Icon(Icons.close_rounded, size: 17),
          label: KeepAllText(l10n.cancel),
        ),
      );
    }
    // One footer for every state: the main button keeps its size,
    // place and amber style; only its label, icon and action follow the state.
    // ▾ (the other-connection menu) rides beside it where the console can be
    // reached.
    final off = r.phase == TilePhase.off;
    final attention = r.phase == TilePhase.attention && !preview;
    final blocked =
        (r.phase == TilePhase.waiting || r.phase == TilePhase.power) &&
        !preview;
    final fix = r.primary ?? ReadinessAction.refresh;
    final String label;
    final IconData icon;
    final VoidCallback? onPressed;
    final String tooltip;
    final String keyName;
    if (attention) {
      keyName = 'attention-';
      label = footerActionLabel(fix);
      icon = footerActionIcon(fix);
      onPressed = fix == ReadinessAction.start
          ? actions.start
          : fix == ReadinessAction.console
          ? actions.console
          : actions.attention;
      tooltip = onPressed != null
          ? r.headline
          : actions.powerReason ?? r.headline;
    } else if (blocked) {
      keyName = 'open-';
      label =
          r.progressLabel ?? (r.phase == TilePhase.power ? l10n.working : l10n.preparing5);
      icon = Icons.hourglass_top_rounded;
      onPressed = actions.primary;
      tooltip = r.headline;
    } else {
      keyName = off ? 'start-' : 'open-';
      label = preview
          ? l10n.screenPreview
          : off
          ? l10n.start
          : compact
          ? l10n.connect2
          : switch (mode) {
              null || SessionDisplayMode.windowed => l10n.streamingConnection,
              SessionDisplayMode.fullscreen => l10n.streamingFullScreen,
              SessionDisplayMode.allMonitors => l10n.streamingAllMonitors,
            };
      icon = off ? Icons.power_settings_new_rounded : Icons.play_arrow_rounded;
      onPressed = actions.primary;
      tooltip = off
          ? l10n.startsDesktopYouCanConnect
          : onPressed == null
          ? r.headline
          : mode == null
          ? l10n.streamingConnection
          // The tip names what the button does, not the key.
          : l10n.streamingConnection2(streamModeLabel(mode));
    }
    final more =
        actions.menu &&
        !off &&
        (blocked || attention ? actions.console != null : true);
    final enabled = onPressed != null;
    final arrow = touch ? 48.0 : 46.0;
    // The main action is always shown as the main action; a grey resting
    // state reads as "disabled".
    final emphasized = enabled;
    final main = Builder(
      builder: (anchor) => Tooltip(
        message: tooltip,
        child: FilledButton.icon(
          key: Key(keyName + id),
          onPressed: onPressed,
          style: focus(filled(emphasized, split: more)),
          icon: Icon(icon, size: 18),
          label: KeepAllText(
            label,
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
          ),
        ),
      ),
    );
    if (!more) {
      return SizedBox(height: height, width: double.infinity, child: main);
    }
    return SizedBox(
      height: height,
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Expanded(child: main),
          Container(
            width: 1,
            color: emphasized ? p.onAmber.withValues(alpha: .18) : p.tileLine,
          ),
          Builder(
            builder: (anchor) => SizedBox(
              width: arrow,
              child: FilledButton(
                key: Key('stream-modes-$id'),
                onPressed: () => onMenu(anchor, DesktopMenuKind.connect),
                style: focus(
                  FilledButton.styleFrom(
                    minimumSize: Size(arrow, height),
                    padding: EdgeInsets.zero,
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    backgroundColor: emphasized ? p.amber : p.quiet,
                    foregroundColor: emphasized ? p.onAmber : p.muted,
                    shape: const RoundedRectangleBorder(
                      borderRadius: BorderRadius.horizontal(
                        right: Radius.circular(12),
                      ),
                    ),
                  ),
                ),
                // Android has no other way to connect: its ▾ holds 화질
                // 옵션 and 긴급 콘솔.
                child: Tooltip(
                  message: mode == null ? l10n.connectionOptions : l10n.otherWaysConnect,
                  child: const Icon(
                    Icons.keyboard_arrow_down_rounded,
                    size: 20,
                  ),
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class DesktopThumbnailView extends StatelessWidget {
  const DesktopThumbnailView({
    super.key,
    required this.machine,
    required this.readiness,
    required this.progress,
    required this.hot,
    required this.preview,
    this.picture,
    this.onPlay,
    this.emptyHint = true,
    this.fit = BoxFit.cover,
    this.previewNote,
  });
  final DesktopMachine machine;
  final DesktopReadiness readiness;
  final StartProgress? progress;
  final bool hot, preview;
  final DesktopThumbnail? picture;

  /// Says "미리보기 없음" on a ready tile without a stored picture;
  /// off where this client never stores one (Android).
  final bool emptyHint;

  /// The broker's reason for no picture (desktop-preview-v1): shown in place of
  /// the hint above, also where that hint is off.
  final String? previewNote;
  final BoxFit fit;

  /// The ▶ button (shown while the tile is hovered or focused); the main
  /// button below is the keyboard route, so ▶ takes no focus of its own.
  final VoidCallback? onPlay;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final r = readiness;
    final progress = this.progress;
    final phase = progress == null ? r.phase : TilePhase.power;
    final dimmed = phase != TilePhase.ready;
    final count = machine.verifiedOwnedDisplayCount;
    final showCount = !preview && count != null;
    final Widget overlay;
    switch (phase) {
      case TilePhase.ready:
        final play = onPlay;
        overlay = Center(
          child: IgnorePointer(
            ignoring: !hot || play == null,
            child: AnimatedOpacity(
              opacity: hot ? 1 : 0,
              duration: const Duration(milliseconds: 140),
              child: Container(
                width: 60,
                height: 60,
                decoration: const BoxDecoration(
                  shape: BoxShape.circle,
                  boxShadow: [
                    BoxShadow(
                      color: Color(0x73000000),
                      blurRadius: 30,
                      offset: Offset(0, 12),
                    ),
                  ],
                ),
                child: Tooltip(
                  message: l10n.streamingConnection,
                  child: Material(
                    color: p.amber,
                    shape: const CircleBorder(),
                    clipBehavior: Clip.antiAlias,
                    child: InkWell(
                      key: Key('play-${machine.id}'),
                      onTap: play,
                      canRequestFocus: false,
                      child: Icon(
                        Icons.play_arrow_rounded,
                        size: 34,
                        color: p.onAmber,
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
        );
      case TilePhase.off:
        overlay = _ThumbMessage(
          icon: _PowerOnButton(id: machine.id, onTap: onPlay),
        );
      case TilePhase.power:
        overlay = _ThumbMessage(
          icon: const SizedBox(
            width: 44,
            height: 44,
            child: CircularProgressIndicator(
              strokeWidth: 3,
              color: Color(0xFFF5A623),
              backgroundColor: Color(0x29FFFFFF),
            ),
          ),
          label: progress == null
              ? r.headline
              : '${progress.waiting} · ${progress.clock}',
          note: progress == null ? null : l10n.connectsByStreamingAsSoon,
        );
      // The sign only: the status card right below says the same headline
      // (the sentence would otherwise show twice on one card).
      case TilePhase.attention:
        overlay = const _ThumbMessage(
          icon: Icon(
            Icons.warning_amber_rounded,
            size: 30,
            color: Color(0xFFFFD24A),
          ),
        );
      case TilePhase.waiting:
        overlay = const _ThumbMessage(
          icon: Icon(
            Icons.hourglass_top_rounded,
            size: 26,
            color: Color(0xFFDFE5E9),
          ),
        );
    }
    final picture = this.picture;
    final now = DateTime.now();
    // The broker read no sound card, or one not applied yet. Unknown and
    // present say nothing.
    final audio = preview ? null : machine.audioState;
    final soundBadge = audio == 'absent' || audio == 'pending';
    // Reserve the actual sound label plus its icon, padding and an 8px gap.
    // A fixed reservation used to cut the caption even when both would fit.
    // The VM's Proxmox QEMU guest agent option is off: a badge
    // in the picture's bottom-right corner, above the sound badge when that
    // one is there.
    final agentBadge = machine.qemuAgentOff && !preview;
    final agentLabel = l10n.qemuAgentOff;
    final captionRight = soundBadge
        ? 60.0 +
              _ImageChip.labelWidth(
                context,
                audio == 'pending' ? l10n.restartNeeded : l10n.noSound,
              )
        : agentBadge
        ? 60.0 + _ImageChip.labelWidth(context, agentLabel)
        : 12.0;
    // Not a live view: an old picture, or any desktop that is not running
    // and ready, reads as a stored snapshot (an undimmed old frame makes
    // the app look frozen). The ▶ overlay stays undimmed above it.
    final snapshot =
        picture != null &&
        (phase != TilePhase.ready || thumbnailIsOld(picture.modified, now));
    // How long the VM has run, beside 실행 중 (also while the Host is not up
    // yet: "방금 켜짐" explains the wait).
    final uptime =
        progress == null &&
            !preview &&
            !machine.powerBusy &&
            machine.resourcesRunning
        ? machine.resources.uptimeAt(now)
        : null;
    final pictureView = Stack(
      fit: StackFit.expand,
      children: [
        // The operating system is already shown beside the name.
        DesktopArt(
          seed: machine.id,
          dimmed: dimmed,
          snapshot: snapshot,
          thumbnail: picture?.image,
          fit: fit,
        ),
        overlay,
        if (picture != null)
          Positioned(
            left: 12,
            bottom: 12,
            right: captionRight,
            child: Align(
              alignment: Alignment.bottomLeft,
              child: Tooltip(
                message: picture.remote && !snapshot
                    ? l10n.screenFromMayDifferNow(screenAge(picture.modified, now), machine.kind == MachineKind.pc ? l10n.localPcInSentence : l10n.desktopInSentence)
                    : l10n.lastSavedScreenScreenMay(machine.kind == MachineKind.pc ? l10n.localPcInSentence : l10n.desktopInSentence),
                child: _ImageChip(
                  key: Key('thumbnail-caption-${machine.id}'),
                  icon: snapshot ? Icons.photo_outlined : Icons.history_rounded,
                  // A Host picture says only its age ("1분 전").
                  label: picture.remote && !snapshot
                      ? screenAge(picture.modified, now)
                      : lastScreenCaption(
                          picture.modified,
                          now,
                          snapshot: snapshot,
                        ),
                  bounded: true,
                ),
              ),
            ),
          ),
        // A missing picture does not imply that a first connection is needed.
        if (picture == null &&
            phase == TilePhase.ready &&
            (emptyHint || previewNote != null))
          Positioned(
            left: 12,
            bottom: 12,
            right: captionRight,
            child: Align(
              alignment: Alignment.bottomLeft,
              child: _ImageChip(
                key: Key('thumbnail-empty-${machine.id}'),
                icon: previewNote == null
                    ? Icons.photo_outlined
                    : previewNote == l10n.locked
                    ? Icons.lock_outline_rounded
                    : previewNote == l10n.signScreen
                    ? Icons.login_rounded
                    : Icons.dark_mode_outlined,
                label: previewNote ?? l10n.noPreview,
                bounded: true,
                collapseWhenTight: true,
              ),
            ),
          ),
        Positioned(
          top: 12,
          left: 12,
          right:
              12 + (showCount ? 52 : 0) + (machine.organize.favorite ? 32 : 0),
          child: Align(
            alignment: Alignment.topLeft,
            child: _uptimeTooltip(
              uptime,
              _ImageChip(
                key: Key('power-state-${machine.id}'),
                dot: progress != null
                    ? const Color(0xFFFFD24A)
                    : switch (phase) {
                        TilePhase.ready => const Color(0xFF62E3A9),
                        TilePhase.attention => const Color(0xFFFFD24A),
                        TilePhase.power => const Color(0xFFFFD24A),
                        _ => null,
                      },
                label: progress != null ? l10n.preparing3 : r.chip,
                detail: uptime == null ? null : uptimeText(uptime),
                detailKey: Key('uptime-${machine.id}'),
                bounded: true,
              ),
              pc: machine.kind == MachineKind.pc,
            ),
          ),
        ),
        // The star sits in the top corner (right of the monitor count, if
        // any); it is information only and takes no pointer.
        if (machine.organize.favorite)
          Positioned(
            top: 12,
            right: showCount ? 64 : 12,
            child: IgnorePointer(
              child: Container(
                key: Key('favorite-star-${machine.id}'),
                width: 24,
                height: 24,
                decoration: const BoxDecoration(
                  color: Color(0x8C000000),
                  shape: BoxShape.circle,
                ),
                child: const Icon(
                  Icons.star_rounded,
                  size: 16,
                  color: Color(0xFFF5A623),
                ),
              ),
            ),
          ),
        if (soundBadge)
          Positioned(
            right: 12,
            bottom: 12,
            child: _SoundBadge(
              key: Key('sound-badge-${machine.id}'),
              pending: audio == 'pending',
            ),
          ),
        if (agentBadge)
          Positioned(
            right: 12,
            bottom: soundBadge ? 48 : 12,
            child: Tooltip(
              message: l10n.qemuGuestAgentOffProxmox,
              child: _ImageChip(
                key: Key('qemu-agent-off-${machine.id}'),
                icon: Icons.extension_off_outlined,
                label: agentLabel,
              ),
            ),
          ),
        if (showCount)
          Positioned(
            top: 12,
            right: 12,
            child: Tooltip(
              message: machine.display.label,
              child: _ImageChip(
                key: Key('owned-display-count-${machine.id}'),
                icon: count > 1
                    ? Icons.view_column_rounded
                    : Icons.crop_landscape_rounded,
                label: '$count',
              ),
            ),
          ),
      ],
    );
    return phase == TilePhase.off && onPlay != null
        ? GestureDetector(
            key: Key('power-preview-${machine.id}'),
            behavior: HitTestBehavior.opaque,
            onTap: onPlay,
            child: pictureView,
          )
        : pictureView;
  }
}

/// [pc]: a PC's Host counts from Windows' last start or wake.
Widget _uptimeTooltip(Duration? uptime, Widget chip, {bool pc = false}) =>
    uptime == null
    ? chip
    : Tooltip(
        message: pc
            ? l10n.startedAgo(uptimeLongText(uptime))
            : l10n.startedAgoAccordingProxmox(uptimeLongText(uptime)),
        waitDuration: const Duration(milliseconds: 500),
        child: chip,
      );

/// The power sign on a switched-off tile. Where this user may switch the
/// desktop on it is also a button for 켜기 and fills amber under the pointer,
/// as ▶ does on a running tile (it looks like a button, so it answers the
/// pointer). The main button below is the keyboard route, so
/// this one takes no focus.
class _PowerOnButton extends StatefulWidget {
  const _PowerOnButton({required this.id, required this.onTap});
  final String id;
  final VoidCallback? onTap;
  @override
  State<_PowerOnButton> createState() => _PowerOnButtonState();
}

class _PowerOnButtonState extends State<_PowerOnButton> {
  bool _over = false;
  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final onTap = widget.onTap;
    final lit = _over && onTap != null;
    final sign = AnimatedContainer(
      duration: const Duration(milliseconds: 140),
      width: 50,
      height: 50,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        color: lit ? p.amber : p.amber.withValues(alpha: 0),
        border: Border.all(
          color: lit ? p.amber : const Color(0x59FFFFFF),
          width: 1.5,
        ),
      ),
      child: Icon(
        Icons.power_settings_new_rounded,
        size: 22,
        color: lit ? p.onAmber : const Color(0xFFDFE5E9),
      ),
    );
    if (onTap == null) return sign;
    return Tooltip(
      message: l10n.start,
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        onEnter: (_) => setState(() => _over = true),
        onExit: (_) => setState(() => _over = false),
        child: GestureDetector(
          key: Key('power-on-${widget.id}'),
          behavior: HitTestBehavior.opaque,
          onTap: onTap,
          child: sign,
        ),
      ),
    );
  }
}

class _ThumbMessage extends StatelessWidget {
  const _ThumbMessage({required this.icon, this.label, this.note});
  final Widget icon;
  final String? label, note;
  @override
  Widget build(BuildContext context) {
    final scaler = MediaQuery.textScalerOf(context);
    final needed =
        54 +
        50 +
        (label == null ? 0 : 9 + scaler.scale(13) * 1.4) +
        (note == null ? 0 : 3 + scaler.scale(12) * 1.4);
    return LayoutBuilder(
      builder: (context, box) {
        // A short picture (a phone on its side, desktopTileGrid) keeps only
        // the sign; the words are in the status card below it.
        if (box.maxHeight < needed) {
          return Padding(
            padding: const EdgeInsets.all(8),
            child: Center(
              child: FittedBox(fit: BoxFit.scaleDown, child: icon),
            ),
          );
        }
        return Padding(
          padding: const EdgeInsets.fromLTRB(16, 30, 16, 24),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              icon,
              if (label != null) ...[
                const SizedBox(height: 9),
                KeepAllText(
                  label!,
                  textAlign: TextAlign.center,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    color: Color(0xFFF1F4F6),
                    fontSize: 13,
                    fontWeight: FontWeight.w600,
                    shadows: [Shadow(color: Color(0x99000000), blurRadius: 8)],
                  ),
                ),
              ],
              if (note != null) ...[
                const SizedBox(height: 3),
                KeepAllText(
                  note!,
                  textAlign: TextAlign.center,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    color: Color(0xCCF1F4F6),
                    fontSize: 12,
                  ),
                ),
              ],
            ],
          ),
        );
      },
    );
  }
}

/// 소리 없음 (no sound device) or 다시 시작 필요 (one was added in Proxmox and
/// is not applied yet) on the picture, in the chip style with a warning
/// colour. An indicator only: Multitap adds no sound device, so
/// it opens nothing; its tooltip says what the state means.
class _SoundBadge extends StatelessWidget {
  const _SoundBadge({super.key, required this.pending});
  final bool pending;

  @override
  Widget build(BuildContext context) {
    final label = pending ? l10n.restartNeeded : l10n.noSound;
    final message = pending ? audioDevicePendingText : audioDeviceMissingText;
    // One node for a screen reader: the label, then the same statement the
    // tooltip shows. Nothing to press.
    return Tooltip(
      message: message,
      excludeFromSemantics: true,
      child: Semantics(
        container: true,
        label: label,
        tooltip: message,
        excludeSemantics: true,
        child: _ImageChip(
          icon: pending ? Icons.restart_alt_rounded : Icons.volume_off_rounded,
          iconColor: const Color(0xFFFFD24A),
          label: label,
        ),
      ),
    );
  }
}

/// A dark chip laid over the picture (fixed colours: it sits on art).
class _ImageChip extends StatelessWidget {
  const _ImageChip({
    super.key,
    required this.label,
    this.dot,
    this.icon,
    this.detail,
    this.detailKey,
    this.bounded = false,
    this.collapseWhenTight = false,
    this.iconColor = const Color(0xFFF1F4F6),
  });
  final String label;
  final Color? dot;
  final IconData? icon;
  final Color iconColor;

  /// A quieter second part after a dot ("3시간" beside 실행 중).
  final String? detail;
  final Key? detailKey;

  /// Laid out with a width limit, so the label may shorten with an ellipsis;
  /// with too little room for even its sign it is left out instead of
  /// overflowing (a narrow card at 200 % beside the sound badge).
  final bool bounded;
  final bool collapseWhenTight;

  static const _labelStyle = TextStyle(
    color: Color(0xFFF1F4F6),
    fontSize: 12,
    fontWeight: FontWeight.w600,
  );

  static double labelWidth(BuildContext context, String label) {
    final painter = TextPainter(
      text: TextSpan(
        text: label,
        style: DefaultTextStyle.of(context).style.merge(_labelStyle),
      ),
      textScaler: MediaQuery.textScalerOf(context),
      textDirection: Directionality.of(context),
    )..layout();
    final width = painter.width;
    painter.dispose();
    return width;
  }

  @override
  Widget build(BuildContext context) => !bounded
      ? _chip(context)
      : LayoutBuilder(
          builder: (context, box) {
            if (collapseWhenTight &&
                box.maxWidth < labelWidth(context, label) + 40) {
              return box.maxWidth < 34
                  ? const SizedBox.shrink()
                  : Tooltip(
                      message: label,
                      triggerMode: TooltipTriggerMode.tap,
                      child: _chip(context, compact: true),
                    );
            }
            return box.maxWidth < 64 ? const SizedBox.shrink() : _chip(context);
          },
        );

  Widget _chip(BuildContext context, {bool compact = false}) => Container(
    // Grows with the text (a fixed height clips at 150 % on Windows).
    height: math.max(
      26.0,
      MediaQuery.textScalerOf(context).scale(12) * 1.4 + 8,
    ),
    padding: const EdgeInsets.symmetric(horizontal: 10),
    decoration: BoxDecoration(
      color: const Color(0xC20C0F12),
      borderRadius: BorderRadius.circular(13),
    ),
    child: Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        if (icon != null) ...[
          Icon(icon, size: 14, color: iconColor),
          if (!compact) const SizedBox(width: 6),
        ] else ...[
          Container(
            width: 7,
            height: 7,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: dot,
              border: dot == null
                  ? Border.all(color: const Color(0xFF9AA5AD), width: 1.5)
                  : null,
            ),
          ),
          const SizedBox(width: 7),
        ],
        if (!compact)
          if (bounded) Flexible(child: text) else text,
        if (detail != null) ...[
          Container(
            width: 3,
            height: 3,
            margin: const EdgeInsets.symmetric(horizontal: 6),
            decoration: const BoxDecoration(
              shape: BoxShape.circle,
              color: Color(0x8CF1F4F6),
            ),
          ),
          KeepAllText(
            detail!,
            key: detailKey,
            maxLines: 1,
            style: const TextStyle(
              color: Color(0xC7F1F4F6),
              fontSize: 12,
              fontWeight: FontWeight.w500,
              fontFeatures: [FontFeature.tabularFigures()],
            ),
          ),
        ],
      ],
    ),
  );

  Text get text => KeepAllText(
    label,
    maxLines: bounded ? 1 : null,
    overflow: bounded ? TextOverflow.ellipsis : null,
    style: _labelStyle,
  );
}

/// A desktop in the user-selected list or mini view.
/// List and mini share one action surface; density never changes permission or
/// click behavior. Resource values use the same fresh/stale rules as cards.
class DesktopRow extends StatefulWidget {
  const DesktopRow({
    super.key,
    required this.machine,
    required this.readiness,
    required this.actions,
    required this.mode,
    this.progress,
    this.focusNode,
    this.preview = false,
    this.dense = false,
  });
  final DesktopMachine machine;
  final DesktopReadiness readiness;
  final DesktopActions actions;
  final SessionDisplayMode mode;
  final StartProgress? progress;
  final FocusNode? focusNode;
  final bool preview, dense;

  @override
  State<DesktopRow> createState() => _DesktopRowState();
}

class _DesktopRowState extends State<DesktopRow> {
  final _menu = MenuController();
  final _surface = GlobalKey();
  bool _hovered = false, _focused = false;
  DesktopMenuKind _menuKind = DesktopMenuKind.all;
  Rect? _menuAnchor;
  int _menuRequest = 0;

  void _openMenu(Rect anchor, DesktopMenuKind kind) {
    if (!widget.actions.menu) {
      return;
    }
    final request = ++_menuRequest, id = widget.machine.id;
    if (_menu.isOpen) {
      _menu.close();
      if (kind == _menuKind) {
        return;
      }
    }
    setState(() {
      _menuKind = kind;
      _menuAnchor = anchor;
    });
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted &&
          request == _menuRequest &&
          widget.machine.id == id &&
          widget.actions.menu) {
        _menu.open();
      }
    });
  }

  void _openBelow(BuildContext anchor, DesktopMenuKind kind) {
    final box = anchor.findRenderObject() as RenderBox?;
    final surface = _surface.currentContext?.findRenderObject() as RenderBox?;
    if (box == null || surface == null || !box.hasSize || !surface.hasSize) {
      return;
    }
    _openMenu(
      Rect.fromPoints(
        surface.globalToLocal(box.localToGlobal(Offset.zero)),
        surface.globalToLocal(
          box.localToGlobal(box.size.bottomRight(Offset.zero)),
        ),
      ),
      kind,
    );
  }

  void _openContextMenu(Offset globalPosition) {
    final surface = _surface.currentContext?.findRenderObject() as RenderBox?;
    if (surface == null || !surface.hasSize) {
      return;
    }
    final point = surface.globalToLocal(globalPosition);
    _openMenu(Rect.fromLTWH(point.dx, point.dy, 1, 1), DesktopMenuKind.all);
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final m = widget.machine, r = widget.readiness, a = widget.actions;
    final dense = widget.dense, progress = widget.progress;
    final scale = MediaQuery.textScalerOf(context).scale(1).clamp(1.0, 2.0);
    final hot = _hovered || _focused || _menu.isOpen;
    final phase = progress == null ? r.phase : TilePhase.power;
    final headline = progress != null
        ? '${progress.waiting} · ${progress.clock}'
        : r.phase == TilePhase.ready && !r.console
        ? m.use.shortFor(m.useAccount) ?? r.chip
        : r.headline;
    final uptime = m.resourcesRunning
        ? m.resources.uptimeAt(DateTime.now())
        : null;
    final status = [
      headline,
      if (uptime != null) uptimeText(uptime),
      if (!r.hostUpdating && !r.hostMissing) ?m.hostUpdate.tileStatus(),
      if (!dense && r.phase != TilePhase.ready && progress == null) r.detail,
    ].join(' · ');
    final info = Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        DesktopNameRow(
          machine: m,
          menu: a.menu,
          power: a.menu && a.showPower,
          update: a.hostUpdate != null,
          compact: dense,
          showFavorite: true,
          onMenu: (anchor) => _openBelow(anchor, DesktopMenuKind.manage),
          onPower: (anchor) => _openBelow(anchor, DesktopMenuKind.power),
        ),
        Row(
          children: [
            Container(
              width: 7,
              height: 7,
              decoration: BoxDecoration(
                color: _phaseDot(p, phase),
                shape: BoxShape.circle,
              ),
            ),
            const SizedBox(width: 7),
            Expanded(
              child: Tooltip(
                message: '${r.headline} · ${r.detail}',
                child: KeepAllText(
                  status,
                  key: Key('status-${m.id}'),
                  maxLines: dense ? 1 : 2,
                  overflow: TextOverflow.ellipsis,
                  style: uiText(
                    size: 12,
                    color: phase == TilePhase.attention ? p.warn : p.muted,
                  ),
                ),
              ),
            ),
            if (phase != TilePhase.ready &&
                phase != TilePhase.off &&
                progress == null &&
                a.details != null)
              IconButton(
                key: Key('stream-state-${m.id}'),
                tooltip: l10n.statusDetails,
                onPressed: a.details,
                visualDensity: VisualDensity.compact,
                padding: EdgeInsets.zero,
                constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
                icon: Icon(
                  Icons.info_outline_rounded,
                  size: 17,
                  color: p.amberInk,
                ),
              ),
          ],
        ),
      ],
    );
    final resources = VmResourceStrip(
      key: Key('resources-${m.id}'),
      sample: m.resources,
      running: m.resourcesRunning,
      pc: m.kind == MachineKind.pc,
      example: widget.preview,
    );
    final action = _ActionRow(
      machine: m,
      readiness: r,
      actions: a,
      mode: widget.mode,
      progress: progress,
      hot: hot,
      preview: widget.preview,
      compact: dense,
      onMenu: _openBelow,
    );
    final row = Container(
      key: Key('card-${m.id}'),
      constraints: BoxConstraints(minHeight: dense ? 72 : 104),
      decoration: BoxDecoration(
        color: hot ? p.tile : p.tileGlass,
        borderRadius: BorderRadius.circular(dense ? 10 : 12),
        border: Border.all(color: hot ? p.amberLine : p.glassEdge),
      ),
      child: Material(
        type: MaterialType.transparency,
        child: _TileSurface(
          key: Key('tile-${m.id}'),
          focusNode: widget.focusNode,
          onHover: (value) => setState(() => _hovered = value),
          onFocus: (value) => setState(() => _focused = value),
          onSecondaryTapUp: a.menu
              ? (details) => _openContextMenu(details.globalPosition)
              : null,
          child: Padding(
            padding: EdgeInsets.symmetric(
              horizontal: dense ? 12 : 16,
              vertical: dense ? 8 : 12,
            ),
            child: LayoutBuilder(
              builder: (context, bounds) {
                final horizontal =
                    bounds.maxWidth >= (dense ? 720 : 880) * scale;
                if (horizontal) {
                  return Row(
                    children: [
                      Expanded(child: info),
                      const SizedBox(width: 20),
                      SizedBox(
                        width: (dense ? 240 : 280) * scale.clamp(1.0, 1.4),
                        child: resources,
                      ),
                      const SizedBox(width: 20),
                      SizedBox(
                        width: (dense ? 150 : 230) * scale.clamp(1.0, 1.6),
                        child: action,
                      ),
                    ],
                  );
                }
                return Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    info,
                    const SizedBox(height: 8),
                    resources,
                    SizedBox(height: dense ? 10 : 14),
                    action,
                  ],
                );
              },
            ),
          ),
        ),
      ),
    );
    return Semantics(
      container: true,
      label: '${m.name} · ${r.headline}',
      child: KeyedSubtree(
        key: _surface,
        child: DesktopMenu(
          machine: m,
          actions: a,
          mode: widget.mode,
          controller: _menu,
          kind: _menuKind,
          anchorRect: _menuAnchor ?? Rect.zero,
          child: row,
        ),
      ),
    );
  }
}

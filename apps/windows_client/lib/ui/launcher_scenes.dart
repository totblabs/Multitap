import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../data/desktop_thumbnails.dart';
import '../data/launcher_memory.dart';
import '../models/desktop_machine.dart';
import '../models/desktop_organize.dart';
import '../models/session_display_mode.dart';
import 'app_theme.dart';
import 'desktop_art.dart';
import 'desktop_menu.dart';
import 'desktop_readiness.dart';
import 'desktop_tile.dart';
import 'korean_text.dart';
import 'vm_resource_bars.dart';
import '../l10n/l10n.dart';

/// Presentation of one already-authorized desktop. No new connection or power
/// policy is introduced by a scene: every control uses the launcher's actions.
class LauncherDesktop {
  const LauncherDesktop({
    required this.machine,
    required this.readiness,
    required this.actions,
    required this.mode,
    required this.focusNode,
    this.progress,
    this.thumbnail,
  });
  final DesktopMachine machine;
  final DesktopReadiness readiness;
  final DesktopActions actions;
  final SessionDisplayMode mode;
  final FocusNode focusNode;
  final StartProgress? progress;
  final DesktopThumbnail? thumbnail;

  VoidCallback? get activate => progress != null
      ? null
      : readiness.phase == TilePhase.ready || readiness.phase == TilePhase.off
      ? actions.primary
      : actions.details;
}

/// Three spatial launchers share the same data, selection and action controls.
/// Selection only changes the inspected desktop. Enter and the explicit launch
/// controls retain the existing confirmation/authorization path.
class LauncherScenes extends StatefulWidget {
  const LauncherScenes({
    super.key,
    required this.view,
    required this.desktops,
    required this.chips,
    required this.selectedChip,
    required this.onChip,
    this.initialDesktop,
    this.preview = false,
    this.showPreviews = true,
  });
  final LauncherView view;
  final List<LauncherDesktop> desktops;
  final List<(DesktopChip, int)> chips;
  final DesktopChip selectedChip;
  final ValueChanged<DesktopChip> onChip;
  final String? initialDesktop;
  final bool preview, showPreviews;

  static bool compactFor(MediaQueryData media) =>
      media.size.height < 900 * media.textScaler.scale(1).clamp(1.0, 1.6);

  static bool hasSidebar(
    BuildContext context,
    double width,
    LauncherView view,
  ) {
    final media = MediaQuery.of(context);
    return (view == LauncherView.explorer ||
            view == LauncherView.monitorWall) &&
        !compactFor(media) &&
        width >= 1080 * media.textScaler.scale(1).clamp(1.0, 1.6);
  }

  @override
  State<LauncherScenes> createState() => _LauncherScenesState();
}

class _LauncherScenesState extends State<LauncherScenes> {
  String? _selected;
  final _explorerScroll = ScrollController();
  final _wallScroll = ScrollController();
  final _dockScroll = ScrollController();
  ScrollController? _activeSelectionScroll;
  bool _initialRevealScheduled = false;

  @override
  void dispose() {
    _explorerScroll.dispose();
    _wallScroll.dispose();
    _dockScroll.dispose();
    super.dispose();
  }

  Future<void> _revealInitial() async {
    if (!mounted || _selected != null) return;
    final index = widget.desktops.indexWhere(
      (desktop) => desktop.machine.id == widget.initialDesktop,
    );
    final scroll = _activeSelectionScroll;
    if (index <= 0 || scroll == null || !scroll.hasClients) return;
    scroll.jumpTo(
      scroll.position.maxScrollExtent * index / (widget.desktops.length - 1),
    );
    await WidgetsBinding.instance.endOfFrame;
    if (!mounted) return;
    final target = widget.desktops
        .where((desktop) => desktop.machine.id == widget.initialDesktop)
        .firstOrNull
        ?.focusNode
        .context;
    if (target != null && target.mounted) {
      await Scrollable.ensureVisible(target, alignment: .15);
    }
  }

  LauncherDesktop get _current => widget.desktops.firstWhere(
    (desktop) => desktop.machine.id == (_selected ?? widget.initialDesktop),
    orElse: () => widget.desktops.first,
  );

  void _select(LauncherDesktop desktop) {
    if (_selected != desktop.machine.id) {
      setState(() => _selected = desktop.machine.id);
    }
  }

  @override
  Widget build(BuildContext context) {
    if (widget.desktops.isEmpty) return const SizedBox.shrink();
    if (!_initialRevealScheduled) {
      _initialRevealScheduled = true;
      WidgetsBinding.instance.addPostFrameCallback((_) => _revealInitial());
    }
    return LayoutBuilder(
      builder: (context, bounds) {
        final sidebar = LauncherScenes.hasSidebar(
          context,
          bounds.maxWidth,
          widget.view,
        );
        final height = math.max(600.0, MediaQuery.sizeOf(context).height - 256);
        return switch (widget.view) {
          LauncherView.explorer => _explorer(
            context,
            bounds.maxWidth,
            height,
            sidebar,
          ),
          LauncherView.monitorWall => _wall(
            context,
            bounds.maxWidth,
            height,
            sidebar,
          ),
          _ => _dock(context, bounds.maxWidth),
        };
      },
    );
  }

  Widget _navigation(BuildContext context) {
    final p = AppPalette.of(context);
    return _Panel(
      key: const Key('scene-navigation'),
      padding: const EdgeInsets.all(8),
      child: ListView(
        primary: false,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 16, 16, 8),
            child: KeepAllText(l10n.category, style: uiText(size: 12, color: p.muted)),
          ),
          for (final (chip, count) in widget.chips)
            Padding(
              padding: const EdgeInsets.only(bottom: 8),
              child: TextButton(
                key: Key('scene-chip-${chip.id}'),
                onPressed: () => widget.onChip(chip),
                style: TextButton.styleFrom(
                  foregroundColor: chip == widget.selectedChip
                      ? p.amberInk
                      : p.ink,
                  backgroundColor: chip == widget.selectedChip
                      ? p.amberSoft
                      : null,
                  minimumSize: const Size(0, 48),
                  padding: const EdgeInsets.symmetric(
                    horizontal: 16,
                    vertical: 12,
                  ),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(8),
                  ),
                ),
                child: Row(
                  children: [
                    Icon(switch (chip.kind) {
                      DesktopChipKind.all => Icons.desktop_windows_outlined,
                      DesktopChipKind.favorites => Icons.star_outline_rounded,
                      _ => Icons.folder_outlined,
                    }, size: 18),
                    const SizedBox(width: 8),
                    Expanded(
                      child: KeepAllText(
                        chip.label,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                      ),
                    ),
                    const SizedBox(width: 8),
                    Text('$count', style: uiText(size: 12, color: p.muted)),
                  ],
                ),
              ),
            ),
        ],
      ),
    );
  }

  Widget _explorer(
    BuildContext context,
    double width,
    double height,
    bool sidebar,
  ) {
    final scale = MediaQuery.textScalerOf(context).scale(1).clamp(1.0, 1.6);
    final wide = width >= 864 * scale;
    if (!wide || LauncherScenes.compactFor(MediaQuery.of(context))) {
      return _compactScene(context);
    }
    _activeSelectionScroll = _explorerScroll;
    return SizedBox(
      key: const Key('scene-explorer'),
      height: height,
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (sidebar) ...[
            SizedBox(width: 184, child: _navigation(context)),
            const SizedBox(width: 16),
          ],
          SizedBox(
            width: 320,
            child: _Panel(
              key: const Key('scene-explorer-list'),
              padding: const EdgeInsets.all(8),
              child: ListView.separated(
                primary: false,
                controller: _explorerScroll,
                itemCount: widget.desktops.length,
                separatorBuilder: (_, _) => const SizedBox(height: 8),
                itemBuilder: (context, index) =>
                    _selector(widget.desktops[index], picture: true),
              ),
            ),
          ),
          const SizedBox(width: 16),
          Expanded(
            child: _Panel(
              key: const Key('scene-explorer-detail'),
              padding: EdgeInsets.zero,
              child: SingleChildScrollView(
                primary: false,
                child: _DesktopOverview(
                  desktop: _current,
                  preview: widget.preview,
                  showPreview: widget.showPreviews,
                  framed: false,
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _wall(
    BuildContext context,
    double width,
    double height,
    bool sidebar,
  ) {
    final scale = MediaQuery.textScalerOf(context).scale(1).clamp(1.0, 1.6);
    _activeSelectionScroll = _wallScroll;
    if (LauncherScenes.compactFor(MediaQuery.of(context))) {
      return Column(
        key: const Key('scene-monitor-wall'),
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _DesktopControlPanel(
            desktop: _current,
            preview: widget.preview,
            horizontal: true,
            compact: true,
          ),
          const SizedBox(height: 16),
          SizedBox(
            height: MediaQuery.sizeOf(context).height * .4,
            child: _wallGrid(context, scale),
          ),
        ],
      );
    }
    return SizedBox(
      key: const Key('scene-monitor-wall'),
      height: math.max(height, 540 * scale),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (sidebar) ...[
            SizedBox(width: 184, child: _navigation(context)),
            const SizedBox(width: 16),
          ],
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Expanded(child: _wallGrid(context, scale)),
                const SizedBox(height: 16),
                // The footer spans precisely the grid column, never the sidebar.
                _DesktopControlPanel(
                  desktop: _current,
                  preview: widget.preview,
                  horizontal: true,
                  compact: true,
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _wallGrid(BuildContext context, double scale) => LayoutBuilder(
    builder: (context, bounds) {
      final columns = ((bounds.maxWidth + 16) / (240 * scale + 16))
          .floor()
          .clamp(1, 4);
      final tileWidth = (bounds.maxWidth - (columns - 1) * 16) / columns;
      final labelHeight = MediaQuery.textScalerOf(context).scale(13) * 1.4 + 40;
      return GridView.builder(
        key: const Key('scene-wall-grid'),
        primary: false,
        controller: _wallScroll,
        gridDelegate: SliverGridDelegateWithFixedCrossAxisCount(
          crossAxisCount: columns,
          mainAxisExtent:
              (widget.showPreviews ? tileWidth * 9 / 16 : 56) + labelHeight,
          mainAxisSpacing: 16,
          crossAxisSpacing: 16,
        ),
        itemCount: widget.desktops.length,
        itemBuilder: (context, index) =>
            _selector(widget.desktops[index], wall: true),
      );
    },
  );

  Widget _compactScene(BuildContext context) => Column(
    key: Key('scene-compact-${widget.view.name}'),
    crossAxisAlignment: CrossAxisAlignment.stretch,
    children: [
      _selectorStrip(context, compact: true),
      const SizedBox(height: 16),
      _DesktopControlPanel(
        desktop: _current,
        preview: widget.preview,
        horizontal: true,
        compact: true,
      ),
      if (widget.showPreviews) ...[
        const SizedBox(height: 16),
        _Panel(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              KeepAllText(
                _current.thumbnail == null
                    ? l10n.noSavedScreen
                    : lastScreenCaption(
                        _current.thumbnail!.modified,
                        DateTime.now(),
                        snapshot: true,
                      ),
                key: Key('scene-snapshot-${_current.machine.id}'),
                style: uiText(size: 12, color: AppPalette.of(context).muted),
              ),
              const SizedBox(height: 16),
              SizedBox(
                height: MediaQuery.sizeOf(context).height * .22,
                child: _HeroPicture(desktop: _current, preview: widget.preview),
              ),
            ],
          ),
        ),
      ],
    ],
  );

  Widget _dock(BuildContext context, double width) {
    final scale = MediaQuery.textScalerOf(context).scale(1).clamp(1.0, 1.6);
    final wide =
        width >= 752 * scale &&
        !LauncherScenes.compactFor(MediaQuery.of(context));
    final current = _current;
    Widget detail;
    if (wide) {
      final previewWidth = width - 304 - 16;
      final height = math.max(504.0, previewWidth * 9 / 16 + 96);
      detail = SizedBox(
        height: height,
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Expanded(
              child: _PreviewPanel(
                desktop: current,
                preview: widget.preview,
                showPreview: widget.showPreviews,
              ),
            ),
            const SizedBox(width: 16),
            SizedBox(
              width: 304,
              child: _Panel(
                key: const Key('scene-dock-inspector'),
                padding: EdgeInsets.zero,
                child: SingleChildScrollView(
                  primary: false,
                  child: _DesktopControlPanel(
                    desktop: current,
                    preview: widget.preview,
                    framed: false,
                  ),
                ),
              ),
            ),
          ],
        ),
      );
    } else {
      return _compactScene(context);
    }
    return Column(
      key: const Key('scene-dock'),
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [detail, const SizedBox(height: 16), _selectorStrip(context)],
    );
  }

  Widget _selectorStrip(BuildContext context, {bool compact = false}) {
    final scaler = MediaQuery.textScalerOf(context);
    // The monitor includes its 2px frame and 3px gap/stand. Both single-line
    // labels use the same explicit height as the selector below, so every
    // slot grows together with the actual text scaler.
    final pictureHeight = widget.showPreviews
        ? (96 - 4) * 9 / 16 + 4 + 3 + 3
        : 40.0;
    final contentHeight =
        16 +
        pictureHeight +
        8 +
        scaler.scale(13) * 1.6 +
        4 +
        scaler.scale(12) * 1.6;
    _activeSelectionScroll = _dockScroll;
    return _Panel(
      key: const Key('scene-desktop-dock'),
      padding: const EdgeInsets.all(8),
      child: SizedBox(
        height: compact
            ? math.max(56, (scaler.scale(13) * 1.6 + 16).ceilToDouble())
            : math.max(112.0, contentHeight.ceilToDouble()),
        child: LayoutBuilder(
          builder: (context, bounds) {
            final minimum = compact ? 184.0 : 120.0;
            final width = math.max(
              minimum,
              bounds.maxWidth / widget.desktops.length,
            );
            return ListView.builder(
              primary: false,
              controller: _dockScroll,
              scrollDirection: Axis.horizontal,
              itemExtent: width,
              itemCount: widget.desktops.length,
              itemBuilder: (context, index) => Padding(
                padding: const EdgeInsets.symmetric(horizontal: 4),
                child: _selector(
                  widget.desktops[index],
                  dock: !compact,
                  compact: compact,
                ),
              ),
            );
          },
        ),
      ),
    );
  }

  Widget _selector(
    LauncherDesktop desktop, {
    bool picture = false,
    bool dock = false,
    bool wall = false,
    bool compact = false,
  }) => _DesktopSelector(
    key: ValueKey('scene-selector-${desktop.machine.id}'),
    desktop: desktop,
    selected: desktop.machine.id == _current.machine.id,
    onSelect: () => _select(desktop),
    preview: widget.preview,
    showPreview: widget.showPreviews,
    picture: picture,
    dock: dock,
    wall: wall,
    compact: compact,
  );
}

class _Panel extends StatelessWidget {
  const _Panel({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.all(24),
  });
  final Widget child;
  final EdgeInsets padding;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return Container(
      padding: padding,
      decoration: BoxDecoration(
        color: p.tileGlass,
        border: Border.all(color: p.glassEdge),
        borderRadius: BorderRadius.circular(12),
      ),
      clipBehavior: Clip.antiAlias,
      child: child,
    );
  }
}

class _DesktopSelector extends StatefulWidget {
  const _DesktopSelector({
    super.key,
    required this.desktop,
    required this.selected,
    required this.onSelect,
    required this.preview,
    required this.showPreview,
    this.picture = false,
    this.dock = false,
    this.wall = false,
    this.compact = false,
  });
  final LauncherDesktop desktop;
  final bool selected, preview, showPreview, picture, dock, wall, compact;
  final VoidCallback onSelect;

  @override
  State<_DesktopSelector> createState() => _DesktopSelectorState();
}

class _DesktopSelectorState extends State<_DesktopSelector> {
  bool _focused = false, _hovered = false;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final d = widget.desktop;
    final name = KeepAllText(
      d.machine.name,
      maxLines: 1,
      overflow: TextOverflow.ellipsis,
      style: uiText(
        size: 13,
        weight: FontWeight.w600,
        color: p.ink,
        height: widget.dock || widget.compact ? 1.6 : null,
      ),
    );
    final state = KeepAllText(
      d.readiness.chip,
      maxLines: 1,
      overflow: TextOverflow.ellipsis,
      style: uiText(
        size: 12,
        color: _stateColor(p, d),
        height: widget.dock ? 1.6 : null,
      ),
    );
    final picture = ClipRRect(
      borderRadius: BorderRadius.circular(6),
      child: AspectRatio(
        aspectRatio: 16 / 9,
        child: widget.wall
            ? DesktopThumbnailView(
                machine: d.machine,
                readiness: d.readiness,
                progress: d.progress,
                hot: _focused || _hovered,
                preview: widget.preview,
                picture: widget.showPreview ? d.thumbnail : null,
                fit: BoxFit.contain,
                onPlay:
                    d.readiness.phase == TilePhase.ready && d.progress == null
                    ? d.actions.primary
                    : null,
              )
            : Tooltip(
                message: d.thumbnail == null
                    ? l10n.noSavedScreen
                    : lastScreenCaption(
                        d.thumbnail!.modified,
                        DateTime.now(),
                        snapshot: true,
                      ),
                child: ColoredBox(
                  color: p.tileWell,
                  child: DesktopArt(
                    seed: d.machine.id,
                    thumbnail: widget.showPreview ? d.thumbnail?.image : null,
                    fit: BoxFit.contain,
                    dimmed: d.readiness.phase != TilePhase.ready,
                    snapshot: true,
                  ),
                ),
              ),
      ),
    );
    Widget content;
    if (widget.compact) {
      content = Padding(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
        child: Row(
          children: [
            Icon(Icons.desktop_windows_outlined, size: 18, color: p.muted),
            const SizedBox(width: 8),
            Expanded(child: name),
            const SizedBox(width: 8),
            Icon(Icons.circle, size: 8, color: _stateColor(p, d)),
          ],
        ),
      );
    } else if (widget.wall) {
      content = Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Expanded(
            child: widget.showPreview
                ? picture
                : Icon(
                    Icons.desktop_windows_outlined,
                    size: 32,
                    color: p.muted,
                  ),
          ),
          ColoredBox(
            color: p.tileGlass,
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Row(
                children: [
                  Expanded(child: name),
                  const SizedBox(width: 8),
                  Icon(Icons.circle, size: 8, color: _stateColor(p, d)),
                ],
              ),
            ),
          ),
        ],
      );
    } else if (widget.dock) {
      content = Padding(
        padding: const EdgeInsets.all(8),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            if (widget.showPreview)
              SizedBox(
                width: 96,
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    DecoratedBox(
                      decoration: BoxDecoration(
                        border: Border.all(
                          color: widget.selected ? p.amberLine : p.glassEdge,
                          width: 2,
                        ),
                        borderRadius: BorderRadius.circular(6),
                      ),
                      child: Padding(
                        padding: const EdgeInsets.all(2),
                        child: picture,
                      ),
                    ),
                    const SizedBox(height: 3),
                    Container(
                      width: 24,
                      height: 3,
                      color: widget.selected ? p.amberInk : p.muted,
                    ),
                  ],
                ),
              )
            else
              Icon(
                Icons.desktop_windows_outlined,
                size: 40,
                color: widget.selected ? p.amberInk : p.muted,
              ),
            const SizedBox(height: 8),
            name,
            const SizedBox(height: 4),
            state,
          ],
        ),
      );
    } else {
      final scaler = MediaQuery.textScalerOf(context);
      content = ConstrainedBox(
        key: Key('scene-explorer-row-${d.machine.id}'),
        constraints: BoxConstraints(
          minHeight: math.max(
            96,
            24 + scaler.scale(13) * 1.6 + 4 + scaler.scale(12) * 1.6,
          ),
        ),
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Row(
            children: [
              if (widget.picture && widget.showPreview) ...[
                SizedBox(
                  key: Key('scene-explorer-thumb-${d.machine.id}'),
                  width: 112,
                  height: 63,
                  child: picture,
                ),
                const SizedBox(width: 12),
              ],
              Expanded(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [name, const SizedBox(height: 4), state],
                ),
              ),
              if (d.machine.organize.favorite) ...[
                const SizedBox(width: 8),
                Icon(Icons.star_rounded, size: 16, color: p.amberInk),
              ],
            ],
          ),
        ),
      );
    }
    return Semantics(
      selected: widget.selected,
      button: true,
      label: l10n.select(d.machine.name),
      child: Focus(
        focusNode: d.focusNode,
        onFocusChange: (value) {
          setState(() => _focused = value);
          if (value) widget.onSelect();
        },
        // No key connects from here; the connect button does.
        child: MouseRegion(
          onEnter: (_) => setState(() => _hovered = true),
          onExit: (_) => setState(() => _hovered = false),
          child: Material(
            color: widget.selected ? p.amberSoft : p.tileGlass,
            shape: RoundedRectangleBorder(
              side: BorderSide(
                color: widget.selected || _focused ? p.amberLine : p.glassEdge,
                width: _focused ? 2 : 1,
              ),
              borderRadius: BorderRadius.circular(10),
            ),
            clipBehavior: Clip.antiAlias,
            child: InkWell(
              key: Key('select-desktop-${d.machine.id}'),
              canRequestFocus: false,
              onTap: () {
                widget.onSelect();
                d.focusNode.requestFocus();
              },
              child: content,
            ),
          ),
        ),
      ),
    );
  }
}

Color _stateColor(AppPalette p, LauncherDesktop desktop) =>
    switch (desktop.progress == null
    ? desktop.readiness.phase
    : TilePhase.power) {
      TilePhase.ready => p.green,
      TilePhase.attention => p.warn,
      TilePhase.power => p.amberInk,
      _ => p.muted,
    };

class _PreviewPanel extends StatelessWidget {
  const _PreviewPanel({
    required this.desktop,
    required this.preview,
    required this.showPreview,
  });
  final LauncherDesktop desktop;
  final bool preview, showPreview;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context), d = desktop;
    final caption = !showPreview
        ? l10n.screenPreviewOff
        : d.thumbnail == null
        ? l10n.noSavedScreen
        : lastScreenCaption(
            d.thumbnail!.modified,
            DateTime.now(),
            snapshot: true,
          );
    final picture = showPreview
        ? _HeroPicture(desktop: d, preview: preview)
        : Center(
            child: Icon(
              Icons.image_not_supported_outlined,
              size: 40,
              color: p.muted,
            ),
          );
    final content = Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            Expanded(
              child: KeepAllText(
                d.machine.name,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: uiText(size: 20, weight: FontWeight.w600, color: p.ink),
              ),
            ),
          ],
        ),
        const SizedBox(height: 8),
        KeepAllText(
          caption,
          key: Key('scene-snapshot-${d.machine.id}'),
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: uiText(size: 12, color: p.muted),
        ),
        const SizedBox(height: 16),
        Expanded(
          child: Center(
            child: AspectRatio(
              aspectRatio: 16 / 9,
              child: ClipRRect(
                borderRadius: BorderRadius.circular(8),
                child: ColoredBox(color: p.tileWell, child: picture),
              ),
            ),
          ),
        ),
      ],
    );
    return _Panel(key: const Key('scene-preview-panel'), child: content);
  }
}

class _DesktopOverview extends StatelessWidget {
  const _DesktopOverview({
    required this.desktop,
    required this.preview,
    required this.showPreview,
    this.framed = true,
  });
  final LauncherDesktop desktop;
  final bool preview, showPreview, framed;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context), d = desktop;
    final content = Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        if (showPreview) ...[
          Padding(
            padding: const EdgeInsets.all(24),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                KeepAllText(
                  d.thumbnail == null
                      ? l10n.noSavedScreen
                      : lastScreenCaption(
                          d.thumbnail!.modified,
                          DateTime.now(),
                          snapshot: true,
                        ),
                  key: Key('scene-snapshot-${d.machine.id}'),
                  style: uiText(size: 12, color: p.muted),
                ),
                const SizedBox(height: 16),
                ClipRRect(
                  borderRadius: BorderRadius.circular(8),
                  child: AspectRatio(
                    aspectRatio: 16 / 9,
                    child: ColoredBox(
                      color: p.tileWell,
                      child: _HeroPicture(desktop: d, preview: preview),
                    ),
                  ),
                ),
              ],
            ),
          ),
        ],
        _DesktopControlPanel(
          desktop: desktop,
          preview: preview,
          horizontal: true,
          framed: false,
        ),
      ],
    );
    return framed ? _Panel(padding: EdgeInsets.zero, child: content) : content;
  }
}

class _HeroPicture extends StatefulWidget {
  const _HeroPicture({required this.desktop, required this.preview});
  final LauncherDesktop desktop;
  final bool preview;

  @override
  State<_HeroPicture> createState() => _HeroPictureState();
}

class _HeroPictureState extends State<_HeroPicture> {
  bool _hovered = false;

  @override
  Widget build(BuildContext context) {
    final d = widget.desktop;
    return MouseRegion(
      onEnter: (_) => setState(() => _hovered = true),
      onExit: (_) => setState(() => _hovered = false),
      child: DesktopThumbnailView(
        machine: d.machine,
        readiness: d.readiness,
        progress: d.progress,
        hot: _hovered,
        preview: widget.preview,
        picture: d.thumbnail,
        fit: BoxFit.contain,
        onPlay: d.progress == null && d.readiness.phase == TilePhase.ready
            ? d.actions.primary
            : null,
      ),
    );
  }
}

class _DesktopControlPanel extends StatefulWidget {
  const _DesktopControlPanel({
    required this.desktop,
    required this.preview,
    this.horizontal = false,
    this.framed = true,
    this.compact = false,
  });
  final LauncherDesktop desktop;
  final bool preview, horizontal, framed, compact;

  @override
  State<_DesktopControlPanel> createState() => _DesktopControlPanelState();
}

class _DesktopControlPanelState extends State<_DesktopControlPanel> {
  final _menu = MenuController();
  final _surface = GlobalKey();
  DesktopMenuKind _kind = DesktopMenuKind.manage;

  void _open(BuildContext anchor, DesktopMenuKind kind) {
    if (!widget.desktop.actions.menu) return;
    if (_menu.isOpen) _menu.close();
    final box = anchor.findRenderObject() as RenderBox?;
    final surface = _surface.currentContext?.findRenderObject() as RenderBox?;
    setState(() => _kind = kind);
    _menu.open(
      position: box == null || surface == null
          ? null
          : surface.globalToLocal(
              box.localToGlobal(Offset(0, box.size.height + 8)),
            ),
    );
  }

  @override
  void didUpdateWidget(_DesktopControlPanel oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.desktop.machine.id != widget.desktop.machine.id &&
        _menu.isOpen) {
      _menu.close();
    }
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context), d = widget.desktop;
    final height = DesktopLaunchButton.heightFor(
      MediaQuery.textScalerOf(context),
    );
    final info = Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        KeepAllText(
          d.machine.name,
          key: Key('scene-desktop-name-${d.machine.id}'),
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: uiText(
            size: widget.compact ? 18 : 20,
            weight: FontWeight.w600,
            color: p.ink,
          ),
        ),
        const SizedBox(height: 8),
        if (!widget.compact) ...[
          KeepAllText(
            desktopOsLabel(d.machine),
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
            style: uiText(size: 12, color: p.muted),
          ),
          const SizedBox(height: 16),
        ],
        VmResourceStrip(
          sample: d.machine.resources,
          running: d.machine.resourcesRunning,
          pc: d.machine.kind == MachineKind.pc,
          example: widget.preview,
        ),
        if (!widget.compact ||
            d.progress != null ||
            d.readiness.phase != TilePhase.ready) ...[
          const SizedBox(height: 16),
          DesktopStatusArea(
            machine: d.machine,
            readiness: d.readiness,
            mode: d.mode,
            progress: d.progress,
            preview: widget.preview,
            onDetails: d.actions.details,
          ),
        ],
      ],
    );
    Widget secondary(
      IconData icon,
      String tooltip,
      DesktopMenuKind kind, {
      bool enabled = true,
      bool badge = false,
    }) => Builder(
      builder: (anchor) => OutlinedButton(
        key: Key('scene-${kind.name}-${d.machine.id}'),
        onPressed: d.actions.menu && enabled ? () => _open(anchor, kind) : null,
        style: OutlinedButton.styleFrom(
          minimumSize: Size(0, height),
          maximumSize: Size(double.infinity, height),
          padding: const EdgeInsets.symmetric(horizontal: 8),
          foregroundColor: p.ink,
          side: BorderSide(color: p.glassEdge),
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(12),
          ),
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
        ),
        child: Tooltip(
          message: tooltip,
          child: Badge(
            isLabelVisible: badge,
            backgroundColor: p.red,
            child: Icon(icon, size: 20),
          ),
        ),
      ),
    );
    final launch = DesktopLaunchButton(
      machine: d.machine,
      readiness: d.readiness,
      actions: d.actions,
      mode: d.mode,
      progress: d.progress,
      preview: widget.preview,
      onMenu: _open,
    );
    final power = secondary(
      Icons.power_settings_new_rounded,
      l10n.power,
      DesktopMenuKind.power,
      enabled: d.actions.showPower,
    );
    final manage = secondary(
      Icons.more_horiz_rounded,
      d.actions.hostUpdate == null ? l10n.manageVm : l10n.manageVmHostUpdateAvailable,
      DesktopMenuKind.manage,
      badge: d.actions.hostUpdate != null,
    );
    final scaler = MediaQuery.textScalerOf(context);
    final launchWidth = math.max(224.0, scaler.scale(14) * 8 + 96);
    final compactActionWidth = launchWidth + height * 2 + 16;
    Widget compactActions(double width) {
      if (width >= compactActionWidth) {
        return Row(
          children: [
            Expanded(child: launch),
            const SizedBox(width: 8),
            SizedBox(width: height, child: power),
            const SizedBox(width: 8),
            SizedBox(width: height, child: manage),
          ],
        );
      }
      return Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          launch,
          const SizedBox(height: 8),
          Row(
            children: [
              Expanded(child: power),
              const SizedBox(width: 8),
              Expanded(child: manage),
            ],
          ),
        ],
      );
    }

    final actions = Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        launch,
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(child: power),
            const SizedBox(width: 8),
            Expanded(child: manage),
          ],
        ),
      ],
    );
    final body = Padding(
      key: _surface,
      padding: EdgeInsets.all(widget.compact ? 16 : 24),
      child: LayoutBuilder(
        builder: (context, bounds) {
          final scale = MediaQuery.textScalerOf(context)
              .scale(1)
              .clamp(1.0, 1.6);
          if (widget.compact) {
            final infoWidth = 280 * scale;
            if (widget.horizontal &&
                bounds.maxWidth >= infoWidth + 24 + compactActionWidth) {
              return ConstrainedBox(
                constraints: const BoxConstraints(minHeight: 80),
                child: Row(
                  children: [
                    Expanded(child: info),
                    const SizedBox(width: 24),
                    SizedBox(
                      width: compactActionWidth,
                      child: compactActions(compactActionWidth),
                    ),
                  ],
                ),
              );
            }
            return Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                info,
                const SizedBox(height: 16),
                compactActions(bounds.maxWidth),
              ],
            );
          }
          if (widget.horizontal && bounds.maxWidth >= 600 * scale) {
            return Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Expanded(child: info),
                const SizedBox(width: 24),
                SizedBox(width: 264, child: actions),
              ],
            );
          }
          return Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [info, const SizedBox(height: 24), actions],
          );
        },
      ),
    );
    final wrapped = DesktopMenu(
      machine: d.machine,
      actions: d.actions,
      mode: d.mode,
      controller: _menu,
      kind: _kind,
      child: body,
    );
    return widget.framed
        ? _Panel(
            key: const Key('scene-control-panel'),
            padding: EdgeInsets.zero,
            child: wrapped,
          )
        : wrapped;
  }
}

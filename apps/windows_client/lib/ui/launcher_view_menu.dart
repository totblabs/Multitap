import 'package:flutter/material.dart';

import '../data/launcher_memory.dart';
import 'desktop_organizer.dart' show LauncherHeaderButton;
import '../l10n/l10n.dart';

String launcherViewLabel(LauncherView view) => switch (view) {
  LauncherView.cards => l10n.cards,
  LauncherView.list => l10n.list,
  LauncherView.mini => l10n.mini,
  LauncherView.explorer => l10n.explorer,
  LauncherView.monitorWall => l10n.monitorWall,
  LauncherView.dock => l10n.dock,
};

IconData launcherViewIcon(LauncherView view) => switch (view) {
  LauncherView.cards => Icons.grid_view_rounded,
  LauncherView.list => Icons.view_list_rounded,
  LauncherView.mini => Icons.density_small_rounded,
  LauncherView.explorer => Icons.view_sidebar_outlined,
  LauncherView.monitorWall => Icons.grid_on_rounded,
  LauncherView.dock => Icons.dock_outlined,
};

/// Card layout is fixed. Only the existing saved-screen preference is exposed:
/// 미리보기 is a toggle in the title row's one button style, amber while on
/// (2026-11-03). [compact] is its icon alone, for a narrow title row.
class LauncherViewMenu extends StatelessWidget {
  const LauncherViewMenu({
    super.key,
    required this.controller,
    required this.view,
    required this.showPreviews,
    required this.onSelected,
    required this.onPreviews,
    this.compact = false,
    this.compactChrome = false,
  });
  final MenuController controller;
  final LauncherView view;
  final bool showPreviews;
  final ValueChanged<LauncherView> onSelected;
  final ValueChanged<bool> onPreviews;
  final bool compact, compactChrome;

  @override
  Widget build(BuildContext context) => LauncherHeaderButton(
    key: const Key('desktop-previews'),
    compactChrome: compactChrome,
    icon: showPreviews ? Icons.image_outlined : Icons.hide_image_outlined,
    label: compact ? null : l10n.previews,
    tooltip: showPreviews ? l10n.screenPreviewsTapTurnOff : l10n.screenPreviewsOffTapTurn,
    selected: showPreviews,
    onPressed: () => onPreviews(!showPreviews),
  );
}

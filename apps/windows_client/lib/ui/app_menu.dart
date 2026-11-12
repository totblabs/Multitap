import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import 'app_theme.dart';
import 'korean_text.dart';

/// Local command surfaces respond on the next frame; their short transition
/// never substitutes for operation progress. Respect the window's motion setting.
abstract final class PopupMotion {
  static const enter = Duration(milliseconds: 100);
  static const exit = Duration(milliseconds: 80);

  static AnimationStyle style(BuildContext context) =>
      MediaQuery.disableAnimationsOf(context)
      ? AnimationStyle.noAnimation
      : const AnimationStyle(
          duration: enter,
          reverseDuration: exit,
          curve: Curves.easeOutCubic,
          reverseCurve: Curves.easeInCubic,
        );
}

/// One menu look for the whole client, the VM power menu's: a compact
/// surface popup (4 px vertical padding, 12 px corners,
/// hairline edge), 34 px rows with 12 px side padding, a small icon in a
/// meaningful palette colour and a short label. Disabled rows grey out both
/// the icon and the label. No long descriptions inside menus.
abstract final class AppMenu {
  static const rowHeight = 34.0;
  static const rowPadding = EdgeInsets.symmetric(horizontal: 12);
  static const menuPadding = EdgeInsets.symmetric(vertical: 4);
  static const iconSize = 17.0, gap = 10.0, radius = 12.0;

  static final _open = <MenuController>{};

  /// [MenuAnchor.onOpen]/[MenuAnchor.onClose] bookkeeping: a dialog or any
  /// other route closes every open menu instead of opening over it
  /// (a connect question must not stand over an open ⋯ menu).
  static void opened(MenuController menu) => _open.add(menu);
  static void closed(MenuController menu) => _open.remove(menu);
  static bool get anyOpen => _open.isNotEmpty;
  static void closeAll() {
    for (final menu in _open.toList()) {
      menu.close();
    }
    _open.clear();
  }

  static TextStyle label(AppPalette p, {bool enabled = true}) =>
      uiText(size: 12, color: enabled ? p.ink : p.muted.withValues(alpha: .55));

  static ShapeBorder shape(AppPalette p) => RoundedRectangleBorder(
    borderRadius: BorderRadius.circular(radius),
    side: BorderSide(color: p.line),
  );

  /// The popup surface for [MenuAnchor] menus.
  static MenuStyle menuStyle(AppPalette p, {double? minWidth}) => MenuStyle(
    backgroundColor: WidgetStatePropertyAll(p.surface),
    surfaceTintColor: const WidgetStatePropertyAll(Colors.transparent),
    elevation: const WidgetStatePropertyAll(10),
    padding: const WidgetStatePropertyAll(menuPadding),
    minimumSize: minWidth == null
        ? null
        : WidgetStatePropertyAll(Size(minWidth, 0)),
    side: WidgetStatePropertyAll(BorderSide(color: p.line)),
    shape: WidgetStatePropertyAll(
      RoundedRectangleBorder(borderRadius: BorderRadius.circular(radius)),
    ),
  );

  /// A [MenuItemButton] row.
  static ButtonStyle itemStyle(AppPalette p) => MenuItemButton.styleFrom(
    minimumSize: const Size(0, rowHeight),
    maximumSize: const Size(double.infinity, rowHeight),
    padding: rowPadding,
    foregroundColor: p.ink,
    disabledForegroundColor: p.muted.withValues(alpha: .55),
    iconColor: p.muted,
    disabledIconColor: p.muted,
    iconSize: iconSize,
    textStyle: uiText(size: 12),
    shape: const RoundedRectangleBorder(),
  );

  /// The popup theme for [PopupMenuButton] and [showMenu].
  static PopupMenuThemeData popupTheme(AppPalette p) => PopupMenuThemeData(
    color: p.surface,
    surfaceTintColor: Colors.transparent,
    elevation: 10,
    position: PopupMenuPosition.under,
    menuPadding: menuPadding,
    shape: shape(p),
    labelTextStyle: WidgetStateProperty.resolveWith(
      (states) => label(p, enabled: !states.contains(WidgetState.disabled)),
    ),
  );

  /// The leading icon: its colour when enabled, muted when not.
  static Widget icon(
    AppPalette p,
    IconData icon, {
    Color? color,
    bool enabled = true,
  }) => Icon(
    icon,
    size: iconSize,
    color: enabled ? color ?? p.muted : p.muted.withValues(alpha: .7),
  );

  /// The green "current choice" mark.
  static Widget check(AppPalette p) =>
      Icon(Icons.check_rounded, size: 15, color: p.green);

  /// An icon, a gap and a short label, with an optional trailing mark.
  static Widget row(
    AppPalette p, {
    required String label,
    IconData? icon,
    Color? color,
    bool enabled = true,
    Widget? trailing,
  }) => Row(
    mainAxisSize: MainAxisSize.min,
    children: [
      if (icon != null) ...[
        AppMenu.icon(p, icon, color: color, enabled: enabled),
        const SizedBox(width: gap),
      ],
      Flexible(
        child: KeepAllText(
          label,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: AppMenu.label(p, enabled: enabled),
        ),
      ),
      if (trailing != null) ...[const SizedBox(width: 14), trailing],
    ],
  );

  /// Pops a [PopupMenuButton]/[showMenu] menu that a dialog or page would
  /// otherwise open above (the PopupMenuRoute class itself is private).
  static bool _isPopupMenu(Route<dynamic>? route) =>
      route is AnchoredPanelRoute ||
      (route is PopupRoute &&
          route is! RawDialogRoute &&
          route.runtimeType.toString().startsWith('_PopupMenuRoute'));

  /// A [PopupMenuItem] row in the shared geometry.
  static PopupMenuItem<T> item<T>(
    AppPalette p, {
    Key? key,
    required T value,
    required String label,
    IconData? icon,
    Color? color,
    bool enabled = true,
    Widget? trailing,
  }) => PopupMenuItem<T>(
    key: key,
    value: value,
    enabled: enabled,
    height: rowHeight,
    padding: rowPadding,
    child: row(
      p,
      label: label,
      icon: icon,
      color: color,
      enabled: enabled,
      trailing: trailing,
    ),
  );
}

/// A popup that drops down from a button like a combo menu, in the shared
/// menu surface (AppMenu): below [anchor], horizontally centred on it and
/// kept inside the part of the window that holds the anchor (display
/// features, such as the other monitor of a window spanning two, are
/// avoided). An outside click or Escape closes it, and [MenuDismissObserver]
/// closes it when a dialog or another menu opens. [theme] overrides the
/// app theme (the session bar's panels are always dark, like its menus).
class AnchoredPanelRoute<T> extends PopupRoute<T> {
  AnchoredPanelRoute({
    required this.anchor,
    required this.width,
    required this.builder,
    required this.label,
    this.theme,
    this.reduceMotion = false,
    this.keepOpenForDialog = false,
  });
  final Rect anchor;
  final double width;
  final WidgetBuilder builder;
  final String label;
  final ThemeData? theme;
  final bool reduceMotion;

  /// A stateful panel may finish its own marked confirmation without losing
  /// the owner awaiting its result. Unrelated dialogs still dismiss it.
  final bool keepOpenForDialog;

  static RouteSettings ownedDialogSettings(BuildContext context) =>
      RouteSettings(arguments: _PanelDialogOwner(ModalRoute.of(context)));

  bool _takeDialog(Route<dynamic> route) {
    final owner = route.settings.arguments;
    final matches =
        keepOpenForDialog &&
        route is RawDialogRoute &&
        owner is _PanelDialogOwner &&
        !owner.consumed &&
        identical(owner.route, this);
    if (matches) owner.consumed = true;
    return matches;
  }

  static const margin = 8.0, gap = 6.0;

  /// The part of the window [anchor] is in: the whole window unless display
  /// features divide it.
  static Rect areaFor(MediaQueryData media, Rect anchor) {
    final screens = DisplayFeatureSubScreen.subScreensInBounds(
      Offset.zero & media.size,
      DisplayFeatureSubScreen.avoidBounds(media),
    ).toList();
    if (screens.isEmpty) return Offset.zero & media.size;
    final point = anchor.center;
    for (final screen in screens) {
      if (screen.contains(point)) return screen;
    }
    return screens.reduce(
      (a, b) =>
          (a.center - point).distance <= (b.center - point).distance ? a : b,
    );
  }

  @override
  Color? get barrierColor => null;
  @override
  bool get barrierDismissible => true;
  @override
  String? get barrierLabel => label;
  @override
  Duration get transitionDuration =>
      reduceMotion ? Duration.zero : PopupMotion.enter;
  @override
  Duration get reverseTransitionDuration =>
      reduceMotion ? Duration.zero : PopupMotion.exit;

  @override
  Widget buildPage(
    BuildContext context,
    Animation<double> animation,
    Animation<double> secondaryAnimation,
  ) {
    final media = MediaQuery.of(context);
    final area = areaFor(media, anchor);
    final panelWidth = math.max(0.0, math.min(width, area.width - 2 * margin));
    final left = (anchor.center.dx - panelWidth / 2)
        .clamp(
          area.left + margin,
          math.max(area.left + margin, area.right - margin - panelWidth),
        )
        .toDouble();
    final top = math.min(anchor.bottom + gap, area.bottom - margin);
    final maxHeight = math.max(0.0, area.bottom - margin - top);
    Widget panel = Builder(
      builder: (context) {
        final p = AppPalette.of(context);
        return Material(
          key: const Key('anchored-panel'),
          color: p.surface,
          surfaceTintColor: Colors.transparent,
          elevation: 10,
          shadowColor: p.shadow,
          clipBehavior: Clip.antiAlias,
          shape: AppMenu.shape(p),
          child: builder(context),
        );
      },
    );
    if (theme != null) panel = Theme(data: theme!, child: panel);
    return MediaQuery.removePadding(
      context: context,
      removeTop: true,
      removeBottom: true,
      removeLeft: true,
      removeRight: true,
      child: Stack(
        children: [
          Positioned(
            left: left,
            top: top,
            width: panelWidth,
            child: ConstrainedBox(
              constraints: BoxConstraints(maxHeight: maxHeight),
              child: panel,
            ),
          ),
        ],
      ),
    );
  }

  @override
  Widget buildTransitions(
    BuildContext context,
    Animation<double> animation,
    Animation<double> secondaryAnimation,
    Widget child,
  ) {
    if (reduceMotion || MediaQuery.disableAnimationsOf(context)) return child;
    final curve = CurvedAnimation(
      parent: animation,
      curve: Curves.easeOutCubic,
      reverseCurve: Curves.easeInCubic,
    );
    return FadeTransition(
      opacity: curve,
      child: SlideTransition(
        position: Tween(
          begin: const Offset(0, -.02),
          end: Offset.zero,
        ).animate(curve),
        child: child,
      ),
    );
  }
}

class _PanelDialogOwner {
  _PanelDialogOwner(this.route);
  final Route<dynamic>? route;
  bool consumed = false;
}

/// One per app navigator: whatever opens next — a dialog, a page or another
/// popup — first closes the menus already open, so no menu is left standing
/// under it.
class MenuDismissObserver extends NavigatorObserver {
  final _ownedDialogs = <Route<dynamic>, AnchoredPanelRoute<dynamic>>{};

  void _removeLater(Iterable<Route<dynamic>> routes) {
    SchedulerBinding.instance.addPostFrameCallback((_) {
      for (final route in routes) {
        if (route.isActive) route.navigator?.removeRoute(route);
      }
    });
    SchedulerBinding.instance.ensureVisualUpdate();
  }

  void _retired(Route<dynamic> route) {
    _ownedDialogs.remove(route);
    final children = _ownedDialogs.entries
        .where((entry) => identical(entry.value, route))
        .map((entry) => entry.key)
        .toList();
    for (final child in children) {
      _ownedDialogs.remove(child);
    }
    if (children.isNotEmpty) _removeLater(children);
  }

  @override
  void didPop(Route<dynamic> route, Route<dynamic>? previousRoute) =>
      _retired(route);

  @override
  void didRemove(Route<dynamic> route, Route<dynamic>? previousRoute) =>
      _retired(route);

  @override
  void didReplace({Route<dynamic>? newRoute, Route<dynamic>? oldRoute}) {
    if (oldRoute == null) return;
    final owner = _ownedDialogs.remove(oldRoute);
    if (owner != null) _removeLater([owner]);
    _retired(oldRoute);
  }

  @override
  void didPush(Route<dynamic> route, Route<dynamic>? previousRoute) {
    final supersededOwner = _ownedDialogs.remove(previousRoute);
    if (supersededOwner != null) {
      // An unrelated route replaces the current confirmation's lifetime.
      // Neither its dialog nor its owner may later apply a stale answer.
      _removeLater([previousRoute!, supersededOwner]);
    }
    final owned =
        previousRoute is AnchoredPanelRoute && previousRoute._takeDialog(route);
    if (owned) _ownedDialogs[route] = previousRoute;
    final menu =
        AppMenu._isPopupMenu(previousRoute) &&
            !AppMenu._isPopupMenu(route) &&
            !owned
        ? previousRoute
        : null;
    if (AppMenu._open.isEmpty && menu == null) return;
    // The navigator is mid-push; close once it has settled.
    SchedulerBinding.instance.addPostFrameCallback((_) {
      AppMenu.closeAll();
      if (menu != null && menu.isActive) menu.navigator?.removeRoute(menu);
    });
    SchedulerBinding.instance.ensureVisualUpdate();
  }
}

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import 'korean_text.dart';
import 'app_menu.dart' show PopupMotion;
import '../l10n/l10n.dart';

/// The menus and panels one session bar has open, so they close when what
/// they show no longer applies: the window was resized or lost focus, the
/// screen mode or the remote resolution changed, the connection ended or
/// was replaced (a pinned bar's quality menu must not stay open over a new
/// resolution with the old numbers). Closing removes them at once,
/// without their exit animation; their futures complete with null, so a
/// panel applies nothing. A presenter's native popup (e.g. a Windows menu
/// over a native picture) closes too when its presenter [holdNative]s it.
class SessionPopups {
  final _open = <Route<dynamic>>{};
  final _native = <Object, VoidCallback>{};
  int _generation = 0;

  /// Whether a tracked menu or panel is up.
  bool get isEmpty => _open.isEmpty && _native.isEmpty;

  /// A native popup is up that Flutter cannot remove: [close] dismisses it
  /// (its presenter then completes with null). Call the returned function
  /// once it has closed on its own.
  VoidCallback holdNative(VoidCallback close) {
    final token = Object();
    _native[token] = close;
    return () => _native.remove(token);
  }

  /// Called as a popup is shown: wraps its content so its route is tracked
  /// while it is up. One that a [closeAll] overtook before its first build
  /// closes as soon as it is built.
  Widget Function(Widget child) tracker() {
    final generation = _generation;
    return (child) =>
        _TrackedPopup(popups: this, generation: generation, child: child);
  }

  void closeAll() {
    _generation++;
    final routes = _open.toList();
    _open.clear();
    routes.forEach(_remove);
    final native = _native.values.toList();
    _native.clear();
    for (final close in native) {
      close();
    }
  }

  static void _remove(Route<dynamic> route) {
    void remove() {
      final navigator = route.navigator;
      if (route.isActive && navigator != null) navigator.removeRoute(route);
    }

    // At once between frames (a minimized window has none until it is
    // restored, which would show the stale menu for a frame); never during
    // a build or layout, where the navigator cannot rebuild its overlay.
    final scheduler = SchedulerBinding.instance;
    if (scheduler.schedulerPhase == SchedulerPhase.idle ||
        scheduler.schedulerPhase == SchedulerPhase.postFrameCallbacks) {
      remove();
      return;
    }
    scheduler.addPostFrameCallback((_) => remove());
    scheduler.ensureVisualUpdate();
  }
}

class _TrackedPopup extends StatefulWidget {
  const _TrackedPopup({
    required this.popups,
    required this.generation,
    required this.child,
  });
  final SessionPopups popups;
  final int generation;
  final Widget child;

  @override
  State<_TrackedPopup> createState() => _TrackedPopupState();
}

class _TrackedPopupState extends State<_TrackedPopup> {
  Route<dynamic>? _route;

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    final route = ModalRoute.of(context);
    if (route == null || identical(route, _route)) return;
    final popups = widget.popups;
    if (_route != null) popups._open.remove(_route);
    _route = route;
    if (widget.generation != popups._generation) {
      SessionPopups._remove(route);
    } else {
      popups._open.add(route);
    }
  }

  @override
  void dispose() {
    final route = _route;
    if (route != null) widget.popups._open.remove(route);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => widget.child;
}

/// The in-session bar's own chrome, shared by its buttons, menus and
/// tooltips: a short dark icon bar like ToTB Link's in both app appearances
/// (light mode lightens the launcher, sign-in and window frame, not the
/// session chrome). Always an opaque surface: no live blur over remote video.
/// Orange only marks focus and active commands, green "on", and the latency
/// colour uses ToTB Link's 40/80 ms thresholds.
class SessionChrome {
  const SessionChrome._(this.dark);

  /// The chrome of an explicit brightness (the transfer card follows what the window draws).
  const SessionChrome.forDark(bool dark) : this._(dark);
  final bool dark;

  static SessionChrome of(BuildContext context) => const SessionChrome._(true);

  Color get surface => dark ? const Color(0xFF161A1E) : Colors.white;
  Color get edge => dark ? const Color(0x17FFFFFF) : const Color(0x1F0F1419);
  Color get ink => dark ? const Color(0xFFE4E9EC) : const Color(0xFF1A2025);
  Color get icon => dark ? const Color(0xFFD3DADF) : const Color(0xFF2A3239);
  Color get muted => dark ? const Color(0xFF8E9AA3) : const Color(0xFF66717A);
  Color get faint => dark ? const Color(0xFF5E6A73) : const Color(0xFFA3ACB3);
  Color get accent => dark ? const Color(0xFFF5A623) : const Color(0xFFA86200);
  Color get danger => dark ? const Color(0xFFFF8A7A) : const Color(0xFFC23E36);
  Color get good => dark ? const Color(0xFF62E3A9) : const Color(0xFF13865A);
  Color get fair => dark ? const Color(0xFFFFD24A) : const Color(0xFF8F6200);
  Color get poor => dark ? const Color(0xFFFF8A7A) : const Color(0xFFC23E36);
  Color get hover => dark ? const Color(0x14FFFFFF) : const Color(0x110F1419);
  Color get pressed => dark ? const Color(0x26FFFFFF) : const Color(0x1F0F1419);
  Color get dangerHover => danger.withValues(alpha: .12);
  Color get dangerPressed => danger.withValues(alpha: .2);
  Color get popup => surface;
  Color get popupEdge =>
      dark ? const Color(0xFF262C32) : const Color(0xFFDCE1E5);
  Color get popupHighlight =>
      dark ? const Color(0xFF1F2429) : const Color(0xFFF0F2F4);
  Color get popupLine =>
      dark ? const Color(0xFF262C32) : const Color(0xFFE4E8EB);
  Color get shadow => dark ? const Color(0x8C000000) : const Color(0x40101820);

  /// One command of the bar; the bar's row is exactly this tall.
  static const button = 34.0;

  /// The open bar: it hangs from the top edge of the picture.
  static const barHeight = 44.0;

  /// From a command's bottom edge to the bar's bottom edge.
  static const belowButton = (barHeight - button) / 2;
  static const goodMs = 40.0, fairMs = 80.0;

  /// A measured RTT's colour; an unmeasured one stays neutral.
  Color latencyColor(double? ms) => ms == null
      ? faint
      : ms < goodMs
      ? good
      : ms < fairMs
      ? fair
      : poor;

  /// A plain word for a measured RTT; null when nothing was measured.
  static String? latencyWord(double? ms) => ms == null
      ? null
      : ms < goodMs
      ? l10n.good
      : ms < fairMs
      ? l10n.fair
      : l10n.slow;

  /// Lit signal bars for a measured RTT (3 good, 2 fair, 1 poor, 0 unknown).
  static int latencyLevel(double? ms) => ms == null
      ? 0
      : ms < goodMs
      ? 3
      : ms < fairMs
      ? 2
      : 1;

  // Command tooltips open below the bar.
  TooltipThemeData get tooltipTheme => TooltipThemeData(
    waitDuration: const Duration(milliseconds: 450),
    verticalOffset: 30,
    preferBelow: true,
    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
    textStyle: TextStyle(
      fontFamily: 'Pretendard',
      fontFamilyFallback: const ['Malgun Gothic', 'Segoe UI'],
      fontSize: 12,
      height: 1.35,
      color: ink,
    ),
    decoration: BoxDecoration(
      color: surface,
      borderRadius: BorderRadius.circular(8),
      border: Border.all(color: popupEdge),
      boxShadow: [
        BoxShadow(color: shadow, blurRadius: 12, offset: const Offset(0, 4)),
      ],
    ),
  );

  /// A flat square command: a rounded light under the pointer, a stronger one
  /// while pressed, an amber ring for keyboard focus and dimmed when disabled.
  ButtonStyle buttonStyle({
    bool danger = false,
    bool active = false,
    double width = button,
  }) {
    final size = Size(width, button);
    return ButtonStyle(
      fixedSize: WidgetStatePropertyAll(size),
      minimumSize: WidgetStatePropertyAll(size),
      maximumSize: WidgetStatePropertyAll(size),
      padding: const WidgetStatePropertyAll(EdgeInsets.zero),
      tapTargetSize: MaterialTapTargetSize.shrinkWrap,
      visualDensity: VisualDensity.standard,
      iconSize: const WidgetStatePropertyAll(19),
      splashFactory: NoSplash.splashFactory,
      animationDuration: const Duration(milliseconds: 120),
      backgroundColor: WidgetStatePropertyAll(
        active ? hover : Colors.transparent,
      ),
      foregroundColor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.disabled)
            ? icon.withValues(alpha: .38)
            : active
            ? accent
            : danger
            ? this.danger
            : icon,
      ),
      overlayColor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.pressed)
            ? (danger ? dangerPressed : pressed)
            : states.contains(WidgetState.hovered) ||
                  states.contains(WidgetState.focused)
            ? (danger ? dangerHover : hover)
            : null,
      ),
      shape: WidgetStateProperty.resolveWith(
        (states) => RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(10),
          side: states.contains(WidgetState.focused)
              ? BorderSide(color: accent, width: 1.5)
              : BorderSide.none,
        ),
      ),
    );
  }
}

class SessionMenuItem {
  const SessionMenuItem(
    this.label, {
    this.key,
    this.icon,
    this.onSelected,
    this.checked = false,
    this.heading = false,
    this.destructive = false,
    this.keepToolbarOpen = false,
    this.value,
    this.color,
    this.disabledReason,
  });

  /// A read-only measured value, e.g. '왕복 지연' · '12 ms'.
  const SessionMenuItem.info(this.label, String this.value, {this.key})
    : icon = null,
      onSelected = null,
      checked = false,
      heading = false,
      destructive = false,
      keepToolbarOpen = false,
      color = null,
      disabledReason = null;

  /// Why a row without [onSelected] cannot be chosen now (its tooltip).
  final String? disabledReason;

  final String label;
  final Key? key;
  final IconData? icon;

  /// The icon's meaningful colour (muted when unset or disabled).
  final Color? color;
  final VoidCallback? onSelected;
  final bool checked, heading, destructive;

  /// A command can open a panel anchored to the still-expanded toolbar.
  final bool keepToolbarOpen;
  final String? value;

  /// The label with its value, for native menus that show one string.
  String get text => value == null ? label : '$label  $value';
}

typedef SessionMenuPresenter = Future<int?> Function(
  Rect anchor,
  List<SessionMenuItem> items,
);

class SessionToolbarMenu extends StatefulWidget {
  const SessionToolbarMenu({
    super.key,
    required this.label,
    required this.icon,
    required this.items,
    required this.onInteract,
    this.presenter,
    this.onOpenChanged,
    this.compact = false,
    this.denseLabel = false,
    this.commandBar = false,
    this.badge,
    this.active = false,
    this.tooltip,
    this.onUsed,
    this.combo = false,
    this.popups,
    this.rightGroup = false,
  }) : command = false,
       overflow = null,
       keepOpenInOverflow = false,
       onPressed = null,
       onPressedAt = null,
       disabledReason = null;

  /// One click, one action (a dialog that explains itself), not a one-item
  /// menu. Without [onPressed] it is shown disabled with [disabledReason] in
  /// its tooltip. In the narrow overflow it becomes one menu row.
  const SessionToolbarMenu.command({
    super.key,
    required this.label,
    required this.icon,
    required this.onPressed,
    required this.onInteract,
    this.onPressedAt,
    this.disabledReason,
    this.presenter,
    this.onOpenChanged,
    this.compact = false,
    this.denseLabel = false,
    this.commandBar = false,
    this.badge,
    this.active = false,
    this.tooltip,
    this.onUsed,
    this.keepOpenInOverflow = false,
    this.rightGroup = false,
    this.overflow,
  }) : command = true,
       combo = false,
       popups = null,
       items = const [];
  final String label;
  final IconData icon;
  final List<SessionMenuItem> items;
  final VoidCallback onInteract;
  final SessionMenuPresenter? presenter;
  final ValueChanged<bool>? onOpenChanged;
  final bool compact, denseLabel, commandBar;

  /// Where its Flutter menu is tracked, so the bar can close it (the bar
  /// passes its own). A command's panel is tracked by whoever opens it.
  final SessionPopups? popups;

  /// Called after one of its rows was chosen or the command ran (the bar
  /// folds again after use).
  final VoidCallback? onUsed;

  /// A direct command ([SessionToolbarMenu.command]) instead of a menu.
  final bool command;

  /// A command that stands for a group (e.g. 폴더·USB): its rows in the
  /// narrow bar's 메뉴, where the group's entries are listed under its
  /// caption instead of one row for the group.
  final List<SessionMenuItem>? overflow;
  final bool keepOpenInOverflow;

  /// A utility beside the pin/exit group, after the window commands. Defaults
  /// off so shared touch and existing toolbars keep their ordering.
  final bool rightGroup;

  /// A choice among modes: its icon shows the current one, with a small
  /// chevron, like a combo box.
  final bool combo;
  static const comboWidth = 56.0;

  /// Its width on the session bar.
  double get barWidth => combo ? comboWidth : SessionChrome.button + 2;
  final VoidCallback? onPressed;

  /// Used instead of [onPressed] on the bar: receives the button's rect
  /// (down to the bar's bottom edge) for a panel that drops down from it,
  /// and resolves true when the panel applied something.
  final Future<bool> Function(Rect anchor)? onPressedAt;
  final String? disabledReason;

  /// The hover text when it says more than [label] (still a short phrase).
  final String? tooltip;

  /// A small status dot on the icon (e.g. a clipboard transfer in progress).
  final Color? badge;

  /// A currently confirmed connection, independent of whether its menu is open.
  /// Defaults off so existing shared toolbar callers keep their appearance.
  final bool active;

  bool get enabled => !command || onPressed != null;

  String get hint {
    final text = tooltip ?? label;
    final reason = disabledReason;
    if (enabled || reason == null) return text;
    // A short reason follows the name; a sentence gets its own line.
    return reason.endsWith('.') ? '$text\n$reason' : '$text · $reason';
  }

  /// The overflow menu's rows for this command or menu.
  List<SessionMenuItem> get overflowItems => overflow != null
      ? [SessionMenuItem(label, heading: true), ...overflow!]
      : command
      ? [
          SessionMenuItem(
            label,
            key: key,
            icon: icon,
            onSelected: onPressed,
            keepToolbarOpen: keepOpenInOverflow,
            checked: active,
            color: active ? badge : null,
            disabledReason: disabledReason,
          ),
        ]
      : [SessionMenuItem(label, heading: true), ...items];
  static const _denseText = TextStyle(
    fontFamily: 'Pretendard',
    fontFamilyFallback: ['Malgun Gothic'],
    fontSize: 13,
    fontWeight: FontWeight.w600,
    letterSpacing: -.1,
  );
  double toolbarWidth(BuildContext context) {
    final text = TextPainter(
      text: TextSpan(
        text: label,
        style: DefaultTextStyle.of(context).style.merge(_denseText),
      ),
      textDirection: Directionality.of(context),
      textScaler: MediaQuery.textScalerOf(context),
      maxLines: 1,
    )..layout();
    // Padding 10, an 18 px icon, a 7 px gap, the label, padding 10.
    final width = (text.width + 45).ceilToDouble() + 1;
    text.dispose();
    return width;
  }

  SessionToolbarMenu withLifecycle(
    ValueChanged<bool> callback, {
    bool compact = false,
    bool denseLabel = false,
    bool commandBar = false,
    VoidCallback? used,
    SessionPopups? popups,
  }) => command
      ? SessionToolbarMenu.command(
          key: key,
          label: label,
          icon: icon,
          onPressed: onPressed,
          onPressedAt: onPressedAt,
          keepOpenInOverflow: keepOpenInOverflow,
          rightGroup: rightGroup,
          disabledReason: disabledReason,
          overflow: overflow,
          onInteract: onInteract,
          presenter: presenter,
          onOpenChanged: callback,
          compact: compact,
          denseLabel: denseLabel,
          commandBar: commandBar,
          badge: badge,
          active: active,
          tooltip: tooltip,
          onUsed: used ?? onUsed,
        )
      : SessionToolbarMenu(
          key: key,
          label: label,
          icon: icon,
          items: items,
          onInteract: onInteract,
          presenter: presenter,
          onOpenChanged: callback,
          compact: compact,
          denseLabel: denseLabel,
          commandBar: commandBar,
          badge: badge,
          active: active,
          tooltip: tooltip,
          onUsed: used ?? onUsed,
          combo: combo,
          rightGroup: rightGroup,
          popups: popups ?? this.popups,
        );

  @override
  State<SessionToolbarMenu> createState() => _SessionToolbarMenuState();
}

/// Marks a session bar used by finger (Android): its menus use 44 dp rows
/// instead of the pointer's 34 px ones.
class SessionTouchScope extends InheritedWidget {
  const SessionTouchScope({super.key, required super.child});

  static bool of(BuildContext context) =>
      context.dependOnInheritedWidgetOfExactType<SessionTouchScope>() != null;

  @override
  bool updateShouldNotify(SessionTouchScope oldWidget) => false;
}

/// Shows [items] below [anchor] in the session menu style and returns the
/// selected index; [popups] tracks it so a session change can close it.
Future<int?> showSessionMenu(
  BuildContext context,
  Rect anchor,
  List<SessionMenuItem> items, {
  SessionPopups? popups,
}) {
  final chrome = SessionChrome.of(context);
  final touch = SessionTouchScope.of(context);
  final rowHeight = touch ? 46.0 : 34.0;
  final scaler = MediaQuery.textScalerOf(context);
  final maxWidth = (MediaQuery.sizeOf(context).width - 32)
      .clamp(120.0, 340.0 * scaler.scale(12) / 12)
      .toDouble();
  final overlay = Overlay.of(context).context.findRenderObject() as RenderBox;
  // Keep every command label on one text edge when some have icons, e.g.
  // hidden-menu items merged into the overflow menu.
  final iconGutter = items.any((item) => !item.heading && item.icon != null);
  // Values remain readable beside their labels on a desktop. On a narrow
  // screen, move the value below its label instead of cutting either off.
  bool stackValue(SessionMenuItem item) {
    if (item.value == null) return false;
    double width(String text, {FontWeight? weight}) {
      final painter = TextPainter(
        text: TextSpan(
          text: text,
          style: TextStyle(
            fontFamily: 'Pretendard',
            fontFamilyFallback: const ['Malgun Gothic', 'Segoe UI'],
            fontSize: 12,
            fontWeight: weight,
          ),
        ),
        textScaler: scaler,
        textDirection: Directionality.of(context),
      )..layout();
      final result = painter.width;
      painter.dispose();
      return result;
    }

    return width(item.label) +
            width(item.value!, weight: FontWeight.w600) +
            24 +
            14 +
            (iconGutter ? 27 : 0) +
            (item.checked ? 29 : 0) >
        maxWidth;
  }

  final track = popups?.tracker();
  // The shared menu geometry (the power menu's) in the session chrome.
  return showMenu<int>(
    context: context,
    color: chrome.popup,
    surfaceTintColor: Colors.transparent,
    elevation: 10,
    shadowColor: chrome.shadow,
    clipBehavior: Clip.antiAlias,
    menuPadding: const EdgeInsets.symmetric(vertical: 4),
    popUpAnimationStyle: PopupMotion.style(context),
    shape: RoundedRectangleBorder(
      borderRadius: BorderRadius.circular(12),
      side: BorderSide(color: chrome.popupEdge),
    ),
    constraints: BoxConstraints(
      minWidth: maxWidth < 180 ? maxWidth : 180,
      maxWidth: maxWidth,
    ),
    position: RelativeRect.fromRect(
      Rect.fromPoints(
        overlay.globalToLocal(Offset(anchor.left, anchor.bottom + 6)),
        overlay.globalToLocal(Offset(anchor.right, anchor.bottom + 6)),
      ),
      Offset.zero & overlay.size,
    ),
    items: [
      for (var i = 0; i < items.length; i++)
        PopupMenuItem<int>(
          key: items[i].key,
          value: i,
          height: items[i].heading ? (i == 0 ? 26 : 32) : rowHeight,
          padding: EdgeInsets.zero,
          enabled: items[i].onSelected != null && !items[i].heading,
          child: _tracked(
            i == 0 ? track : null,
            _SessionMenuRow(
              item: items[i],
              first: i == 0,
              iconGutter: iconGutter,
              height: rowHeight,
              stackValue: stackValue(items[i]),
            ),
          ),
        ),
    ],
  );
}

Widget _tracked(Widget Function(Widget)? track, Widget child) =>
    track == null ? child : track(child);

class _SessionToolbarMenuState extends State<SessionToolbarMenu> {
  bool _open = false;

  // The popup inherits this theme: the item's own rectangular ink is hidden
  // so each row can draw ToTB Link's inset rounded highlight instead.
  static ThemeData _menuTheme(ThemeData base, SessionChrome chrome) =>
      base.copyWith(
        hoverColor: Colors.transparent,
        focusColor: Colors.transparent,
        highlightColor: Colors.transparent,
        splashColor: Colors.transparent,
        splashFactory: NoSplash.splashFactory,
        tooltipTheme: chrome.tooltipTheme,
      );

  Future<void> _show(BuildContext context) async {
    final widget = this.widget;
    widget.onInteract();
    widget.onOpenChanged?.call(true);
    if (mounted) setState(() => _open = true);
    try {
      final anchor = _anchor(context);
      final items = widget.items;
      final selected = widget.presenter != null
          ? await widget.presenter!(anchor, items)
          : await showSessionMenu(
              context,
              anchor,
              items,
              popups: widget.popups,
            );
      if (context.mounted &&
          selected != null &&
          selected >= 0 &&
          selected < items.length) {
        widget.onInteract();
        items[selected].onSelected?.call();
        widget.onUsed?.call();
      }
    } finally {
      if (mounted) setState(() => _open = false);
      widget.onOpenChanged?.call(false);
    }
  }

  @override
  Widget build(BuildContext context) => Theme(
    data: _menuTheme(Theme.of(context), SessionChrome.of(context)),
    child: Builder(builder: _button),
  );

  /// The button in window coordinates; on the bar, down to its bottom edge
  /// so a menu or panel opens just under the bar.
  Rect _anchor(BuildContext context) {
    final box = context.findRenderObject() as RenderBox;
    final button = Rect.fromPoints(
      box.localToGlobal(Offset.zero),
      box.localToGlobal(box.size.bottomRight(Offset.zero)),
    );
    return widget.commandBar
        ? Rect.fromLTWH(
            button.left,
            button.top,
            button.width,
            button.height + SessionChrome.belowButton,
          )
        : button;
  }

  VoidCallback? _pressed(BuildContext context) {
    final widget = this.widget;
    if (!widget.command) return () => _show(context);
    final action = widget.onPressed;
    if (action == null) return null;
    final at = widget.onPressedAt;
    if (at == null) {
      return () {
        widget.onInteract();
        action();
        widget.onUsed?.call();
      };
    }
    // A panel: the button stays lit and the bar open while it is up, like a
    // menu; the bar folds only once the panel applied something.
    return () async {
      widget.onInteract();
      widget.onOpenChanged?.call(true);
      if (mounted) setState(() => _open = true);
      var used = false;
      try {
        used = await at(_anchor(context));
      } finally {
        if (mounted) setState(() => _open = false);
        widget.onOpenChanged?.call(false);
      }
      if (used) widget.onUsed?.call();
    };
  }

  Widget _button(BuildContext context) {
    final widget = this.widget;
    final chrome = SessionChrome.of(context);
    if (widget.compact) {
      final width = widget.commandBar ? widget.barWidth : 38.0;
      final icon = widget.combo
          ? Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                Icon(widget.icon),
                const SizedBox(width: 1),
                Icon(
                  Icons.keyboard_arrow_down_rounded,
                  size: 15,
                  color: _open ? chrome.accent : chrome.muted,
                ),
              ],
            )
          : Icon(widget.icon);
      return SizedBox(
        width: width,
        height: SessionChrome.button,
        child: Tooltip(
          message: widget.hint,
          // A disabled command says why on a tap too (by finger, no hover).
          triggerMode: widget.enabled ? null : TooltipTriggerMode.tap,
          child: TextButton(
            onPressed: _pressed(context),
            style: chrome.buttonStyle(
              active: _open || widget.active,
              width: width,
            ),
            child: Stack(
              clipBehavior: Clip.none,
              alignment: Alignment.center,
              children: [
                icon,
                if (widget.badge != null)
                  Positioned(
                    right: -4,
                    top: -3,
                    child: Container(
                      width: 7,
                      height: 7,
                      decoration: BoxDecoration(
                        color: widget.badge,
                        shape: BoxShape.circle,
                        border: Border.all(color: chrome.surface, width: 1.5),
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ),
      );
    }
    final width = widget.denseLabel ? widget.toolbarWidth(context) : 112.0;
    final windowsCommand = widget.denseLabel && widget.commandBar;
    return SizedBox(
      width: width,
      child: Tooltip(
        message: widget.hint,
        triggerMode: widget.enabled ? null : TooltipTriggerMode.tap,
        child: TextButton(
          onPressed: _pressed(context),
          style: windowsCommand
              ? chrome
                    .buttonStyle(active: _open || widget.active, width: width)
                    .copyWith(
                      padding: const WidgetStatePropertyAll(
                        EdgeInsets.symmetric(horizontal: 6),
                      ),
                      iconSize: const WidgetStatePropertyAll(18),
                      foregroundColor: WidgetStateProperty.resolveWith(
                        (states) => states.contains(WidgetState.disabled)
                            ? chrome.icon.withValues(alpha: .38)
                            : _open || widget.active
                            ? chrome.accent
                            : chrome.ink,
                      ),
                      backgroundColor: WidgetStatePropertyAll(
                        _open
                            ? chrome.accent.withValues(alpha: .13)
                            : Colors.transparent,
                      ),
                    )
              : TextButton.styleFrom(
                  foregroundColor: widget.active ? chrome.accent : chrome.icon,
                  minimumSize: Size(width, 36),
                  maximumSize: Size(width, 36),
                  padding: EdgeInsets.symmetric(
                    horizontal: widget.denseLabel ? 6 : 10,
                  ),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(9),
                  ),
                ),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              Stack(
                clipBehavior: Clip.none,
                children: [
                  Icon(
                    widget.icon,
                    size: windowsCommand ? 18 : (widget.denseLabel ? 16 : 17),
                  ),
                  // Something of this group is connected now.
                  if (windowsCommand && widget.badge != null)
                    Positioned(
                      right: -4,
                      top: -3,
                      child: Container(
                        key: const Key('session-command-badge'),
                        width: 7,
                        height: 7,
                        decoration: BoxDecoration(
                          color: widget.badge,
                          shape: BoxShape.circle,
                          border: Border.all(color: chrome.surface, width: 1.5),
                        ),
                      ),
                    ),
                ],
              ),
              SizedBox(width: windowsCommand ? 7 : (widget.denseLabel ? 5 : 6)),
              KeepAllText(
                widget.label,
                maxLines: 1,
                style: widget.denseLabel
                    ? SessionToolbarMenu._denseText
                    : const TextStyle(
                        fontSize: 12,
                        fontWeight: FontWeight.w600,
                      ),
              ),
              // The bar's commands all open something under themselves: no
              // chevron on each.
              if (!windowsCommand &&
                  (!widget.command || widget.onPressedAt != null)) ...[
                SizedBox(width: widget.denseLabel ? 2 : 3),
                Icon(
                  Icons.expand_more_rounded,
                  size: widget.denseLabel ? 12 : 14,
                ),
              ],
            ],
          ),
        ),
      ),
    );
  }
}

/// A popup row with a 34px minimum height, 12px side padding and a 17px icon.
/// Expanded text can wrap; a value moves below its label on narrow screens.
/// Group headings are small muted captions without separator lines.
class _SessionMenuRow extends StatefulWidget {
  const _SessionMenuRow({
    required this.item,
    required this.first,
    required this.iconGutter,
    this.height = 34,
    this.stackValue = false,
  });
  final SessionMenuItem item;
  final bool first, iconGutter;
  final double height;
  final bool stackValue;

  @override
  State<_SessionMenuRow> createState() => _SessionMenuRowState();
}

class _SessionMenuRowState extends State<_SessionMenuRow> {
  bool _hovered = false;

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    final item = widget.item;
    if (item.heading) {
      return Align(
        alignment: Alignment.bottomLeft,
        child: Padding(
          padding: const EdgeInsets.fromLTRB(12, 0, 12, 5),
          child: KeepAllText(
            item.label,
            style: TextStyle(
              fontSize: 12,
              height: 1.2,
              letterSpacing: .2,
              fontWeight: FontWeight.w600,
              color: chrome.muted,
            ),
          ),
        ),
      );
    }
    final enabled = item.onSelected != null;
    final info = item.value != null && !enabled;
    final focused = Focus.maybeOf(context)?.hasFocus ?? false;
    final lit = enabled && (_hovered || focused);
    final color = info
        ? chrome.muted
        : !enabled
        ? chrome.faint
        : item.destructive
        ? chrome.danger
        : chrome.ink;
    final label = KeepAllText(
      item.label,
      style: TextStyle(
        fontFamily: 'Pretendard',
        fontFamilyFallback: const ['Malgun Gothic', 'Segoe UI'],
        fontSize: 12,
        height: 1.2,
        color: color,
      ),
    );
    final value = item.value == null
        ? null
        : KeepAllText(
            item.value!,
            style: TextStyle(
              fontFamily: 'Pretendard',
              fontFamilyFallback: const ['Malgun Gothic'],
              fontSize: 12,
              height: 1.2,
              fontWeight: FontWeight.w600,
              fontFeatures: const [FontFeature.tabularFigures()],
              color: chrome.ink,
            ),
          );
    final row = MouseRegion(
      onEnter: (_) => setState(() => _hovered = true),
      onExit: (_) => setState(() => _hovered = false),
      child: AnimatedContainer(
        duration: MediaQuery.disableAnimationsOf(context)
            ? Duration.zero
            : const Duration(milliseconds: 90),
        constraints: BoxConstraints(minHeight: widget.height),
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
        color: lit ? chrome.popupHighlight : Colors.transparent,
        child: Row(
          children: [
            if (item.icon != null) ...[
              Icon(
                item.icon,
                size: 17,
                color: !enabled
                    ? chrome.faint
                    : item.destructive
                    ? chrome.danger
                    : item.color ?? chrome.muted,
              ),
              const SizedBox(width: 10),
            ] else if (widget.iconGutter)
              const SizedBox(width: 27),
            Expanded(
              child: widget.stackValue
                  ? Column(
                      mainAxisSize: MainAxisSize.min,
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [label, const SizedBox(height: 4), ?value],
                    )
                  : label,
            ),
            if (value != null && !widget.stackValue) ...[
              const SizedBox(width: 14),
              value,
            ],
            if (item.checked) ...[
              const SizedBox(width: 14),
              Icon(
                Icons.check_rounded,
                size: 15,
                color: enabled
                    ? chrome.good
                    : chrome.good.withValues(alpha: .55),
              ),
            ],
          ],
        ),
      ),
    );
    // A greyed row says why, as its bar button does.
    final reason = item.disabledReason;
    return enabled || reason == null
        ? row
        : Tooltip(
            // A short reason follows the name; a sentence gets its own line.
            message: reason.endsWith('.')
                ? '${item.label}\n$reason'
                : '${item.label} · $reason',
            triggerMode: TooltipTriggerMode.tap,
            child: row,
          );
  }
}

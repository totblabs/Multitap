import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'session_menu.dart';
import 'session_panels.dart' show SessionKeycaps;
import 'session_tools.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';
export 'session_menu.dart';

enum _View { bar, handle }

/// What the folded handle says, one thing at a time (the first that
/// applies): the mouse is locked in the VM, the connection is coming back,
/// the latency has been poor for a while, or nothing in particular.
enum SessionHandleStatus { mouseLock, resuming, poor, normal }

/// Local session commands at the top centre of the remote picture, in the
/// place of Windows' remote desktop connection bar.
///
/// One handle opens and folds it. Folded, the handle is a small tongue
/// hanging from the top edge with a dot in the latency colour; under the
/// pointer it grows to show the measured latency. It also is the session's
/// status light: while 마우스 잠금 is on it shows the keys that release it,
/// while the connection is coming back it says so, and a latency that stays
/// poor keeps its number up ([poorHold] of hysteresis both ways). It opens
/// on a click, never on hover.
///
/// Open, the bar hangs from the top edge as one short row: the quality chip
/// | the session's commands (화면 · 폴더·USB · 설정, each opening a panel
/// under itself) | the window's own buttons where there is no title bar
/// (— 최소화, ❐ 창 모드로) | the one danger action, disconnect, last. The
/// same handle sits under the bar (⌃) and folds it. The bar also folds
/// after a command is used, when the user clicks back into the remote
/// picture ([pointerHeld]) and on Escape, unless 바 열어 두기 ([pinned],
/// switched in the session's 설정 panel) is on. In a window too narrow for
/// the row the commands become one 메뉴 that lists their entries.
///
/// Its menus and panels belong to the moment they opened: when
/// the bar's own area changes (a window resize or DPI change) or
/// [layoutKey] changes, they close and an unpinned bar folds; when
/// [contentKey] changes they close. A bar kept open stays open, inside its
/// area where it was dragged.
///
/// While a press that began in the remote picture is held, the toolbar takes
/// no pointer input at all, so a drag across it keeps reaching the remote.
class SessionToolbar extends StatefulWidget {
  const SessionToolbar({
    super.key,
    required this.title,
    required this.actions,
    required this.onInteract,
    this.inputCaptured = false,
    this.onBounds,
    this.connected = false,
    this.modeLabel,
    this.latencyMs,
    this.fullscreen = false,
    this.onMinimize,
    this.onRestore,
    this.onDisconnect,
    this.qualityItems,
    this.onQualityAt,
    this.gameMouse = false,
    this.gameMouseUsb = false,
    this.resuming = false,
    this.onMenuChanged,
    this.menuPresenter,
    this.pointerHeld = false,
    this.popups,
    this.contentKey,
    this.layoutKey,
    this.touch = false,
    this.onOpenChanged,
  });

  /// Used by finger: the handle keeps its look but its touch area is at
  /// least 44 dp, menus use 44 dp rows and there is no hover. The handle's
  /// own area is the only part of the bar that takes touches while folded;
  /// everything else reaches the remote picture.
  final bool touch;

  /// Called after a frame in which the bar opened or folded, so the session
  /// can close what hangs off the bar.
  final ValueChanged<bool>? onOpenChanged;

  /// Where the bar tracks its open Flutter menus; a caller that opens a
  /// panel for the bar (e.g. from a command's onPressedAt) passes the same
  /// one so it closes with them. Otherwise the bar keeps its own.
  final SessionPopups? popups;

  /// What the menus describe (the connection, its state, the remote
  /// resolution): a change closes open menus and panels, whose rows were
  /// read when they opened. The bar itself stays as it is.
  final Object? contentKey;

  /// Where and when the bar is used (the screen mode, the local monitors,
  /// a new or ended connection, a window that lost focus): a change closes
  /// open menus and panels and folds an unpinned bar.
  final Object? layoutKey;

  /// Presents the quality and 메뉴 menus (e.g. as native popups above a
  /// native surface); otherwise the first command menu's presenter, or
  /// Flutter.
  final SessionMenuPresenter? menuPresenter;
  final String title;
  final List<Widget> actions;
  final VoidCallback onInteract;
  final bool inputCaptured, connected, fullscreen;
  final String? modeLabel;

  /// Measured host RTT only. A missing or invalid sample is never invented.
  final double? latencyMs;
  final ValueChanged<Rect>? onBounds;

  /// The window's own buttons, passed only where the window has no title
  /// bar (full screen): — 최소화 and ❐ 창 모드로, side by side as on a title
  /// bar.
  final VoidCallback? onMinimize, onRestore;
  final VoidCallback? onDisconnect;

  /// "마우스 잠금" is on: the folded handle shows the keys that release it
  /// (the pointer is the VM's then and cannot reach the bar).
  final bool gameMouse;

  /// While [gameMouse]: the VM has the virtual USB mouse (true) or gets
  /// injected movement.
  final bool gameMouseUsb;

  /// session-resume-v1: the connection is coming back; the folded handle
  /// says so.
  final bool resuming;

  /// Called with true just before any bar menu opens and false after it
  /// closes, so the session can take the mouse back from the VM first.
  final ValueChanged<bool>? onMenuChanged;

  /// Measured values (and the full statistics entry) for the quality menu,
  /// read when the menu opens.
  final List<SessionMenuItem> Function()? qualityItems;

  /// Opens the session's own connection panel under the chip instead of the
  /// quality menu; resolves when the panel has closed, true when something
  /// in it was used (the bar then folds as after a command).
  final Future<bool> Function(Rect anchor)? onQualityAt;

  /// A press that began in the remote picture is still held. Until it ends
  /// the toolbar ignores the pointer entirely (no hover, no hit testing), so
  /// a drag across the handle or bar keeps going to the remote and the
  /// release still reaches it. The press itself folds an open bar: clicking
  /// back into the remote view closes it unless it is kept open.
  final bool pointerHeld;

  /// How long a latency has to stay poor before the folded handle keeps its
  /// number up, and how long it has to stay better before the number goes.
  static const poorHold = Duration(seconds: 5);

  @override
  State<SessionToolbar> createState() => SessionToolbarState();
}

class SessionToolbarState extends State<SessionToolbar> {
  /// Whether the full bar is showing.
  bool get isOpen => _view == _View.bar;

  /// Folds an unpinned bar (Android's Back).
  void fold() => _used();

  /// Opens the bar (the first-connection hint shows it once).
  void open() {
    if (mounted && _view == _View.handle && !widget.pointerHeld) _expand();
  }

  /// 바 열어 두기: the bar stays open when the remote picture is clicked or
  /// a command was used. Its switch lives in the session's 설정 panel.
  bool get pinned => _pinned;
  void setPinned(bool value) {
    if (mounted && _pinned != value) setState(() => _pinned = value);
  }

  /// The latency has stayed poor for [SessionToolbar.poorHold] (and has not
  /// yet stayed better that long).
  bool get poorSustained => _poorShown;

  /// What the folded handle says now.
  SessionHandleStatus get handleStatus => widget.gameMouse
      ? SessionHandleStatus.mouseLock
      : widget.resuming
      ? SessionHandleStatus.resuming
      : _poorShown
      ? SessionHandleStatus.poor
      : SessionHandleStatus.normal;

  bool _reportedOpen = false;

  static const _barHeight = SessionChrome.barHeight;
  static const _pad = 6.0, _separator = 13.0, _square = 38.0, _caption = 34.0;

  /// The handle: at rest, and under the bar; its tab when it says something.
  static const _restSize = Size(74, 15),
      _lipSize = Size(72, 14),
      _tabHeight = 25.0;

  /// By finger the folded handle sits in a hit area of at least this.
  static const _touchHit = Size(88, 44);

  double _offset = 0;
  _View _view = _View.handle;
  bool _pinned = false, _hovered = false, _dragging = false;
  bool _menuOpen = false, _handleHover = false;
  final _barFocus = FocusNode(debugLabel: 'session toolbar');
  Rect? _reported;
  late final _ownPopups = SessionPopups();
  SessionPopups get _popups => widget.popups ?? _ownPopups;

  /// The area the bar was last laid out in.
  Size? _area;

  // A poor latency keeps its number on the folded handle only once it has
  // lasted, and goes only once better has lasted: no flicker on a spike.
  bool _poorShown = false;
  Timer? _poorTimer;

  bool get _poorNow => _hasLatency && widget.latencyMs! >= SessionChrome.fairMs;

  void _trackLatency() {
    if (!widget.connected) {
      _poorTimer?.cancel();
      _poorTimer = null;
      _poorShown = false;
      return;
    }
    if (_poorNow == _poorShown) {
      _poorTimer?.cancel();
      _poorTimer = null;
      return;
    }
    _poorTimer ??= Timer(SessionToolbar.poorHold, () {
      _poorTimer = null;
      if (!mounted || _poorNow == _poorShown) return;
      setState(() => _poorShown = _poorNow);
    });
  }

  @override
  void didUpdateWidget(SessionToolbar oldWidget) {
    super.didUpdateWidget(oldWidget);
    // A click back into the remote picture folds the open bar.
    if (widget.pointerHeld &&
        !oldWidget.pointerHeld &&
        _view == _View.bar &&
        !_pinned) {
      _view = _View.handle;
      _hovered = false;
    }
    if (widget.pointerHeld) {
      _handleHover = false;
      _cancelRepeats();
    }
    if (widget.layoutKey != oldWidget.layoutKey) {
      _reset(fold: true);
    } else if (widget.contentKey != oldWidget.contentKey) {
      _reset(fold: false);
    }
    _trackLatency();
  }

  /// What was open no longer applies: menus and panels close and, with
  /// [fold], an unpinned bar folds to its handle. Runs during a build, so
  /// it changes state directly; the routes and focus follow after it.
  void _reset({required bool fold}) {
    _cancelRepeats();
    _menuScope++;
    _menuOpen = false;
    _popups.closeAll();
    if (!fold || _pinned || _view == _View.handle) return;
    _view = _View.handle;
    _hovered = false;
    if (_barFocus.hasFocus) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted && _view == _View.handle) _barFocus.unfocus();
      });
    }
  }

  @override
  void initState() {
    super.initState();
    GestureBinding.instance.pointerRouter.addGlobalRoute(_pointerSeen);
    _trackLatency();
  }

  @override
  void dispose() {
    _cancelRepeats();
    _poorTimer?.cancel();
    GestureBinding.instance.pointerRouter.removeGlobalRoute(_pointerSeen);
    _barFocus.dispose();
    super.dispose();
  }

  // Switching menus takes one click: a press that closes the
  // open menu or panel over another control of this bar also uses that
  // control. The popup's barrier takes the press, so it is noted here and
  // repeated on the control once the popup has gone. A press anywhere else
  // (the remote picture) only closes the popup, as before.
  final _controls = <String, GlobalKey>{};
  final _enabledControls = <String>{};
  String? _openedFrom;
  PointerDownEvent? _pressWhileOpen;
  bool _pressReleased = false;
  Timer? _pressExpiry;
  Object? _menuContent, _menuLayout;
  int _repeatGeneration = 0, _menuScope = 0;

  void _cancelRepeats() {
    _pressExpiry?.cancel();
    _pressExpiry = null;
    _repeatGeneration++;
    _pressReleased = false;
    _pressWhileOpen = null;
    _openedFrom = null;
  }

  ValueChanged<bool> _menuLifecycle(String from) {
    int? ownedScope;
    return (open) {
      if (open) {
        if (ownedScope != null) return;
        ownedScope = ++_menuScope;
        _menuChanged(true, from: from, scope: ownedScope!);
      } else {
        final scope = ownedScope;
        if (scope == null) return;
        ownedScope = null;
        _menuChanged(false, from: from, scope: scope);
      }
    };
  }

  static const _forwardDevice = 0x5E55;
  static int _forwarded = 1 << 24;

  void _pointerSeen(PointerEvent event) {
    if (event.device == _forwardDevice) return;
    if (event is PointerDownEvent) {
      _pressExpiry?.cancel();
      _pressExpiry = null;
      _pressWhileOpen = null;
      _pressReleased = false;
      if (event.buttons == kPrimaryButton && !widget.pointerHeld && _menuOpen) {
        _pressWhileOpen = event;
      }
      return;
    }
    final press = _pressWhileOpen;
    if (press == null || event.pointer != press.pointer) return;
    if (event is PointerCancelEvent) {
      _pressWhileOpen = null;
      _pressReleased = false;
    } else if (event is PointerUpEvent) {
      if (!_menuOpen ||
          event.buttons != 0 ||
          _controlAt(event.position) != _controlAt(press.position)) {
        _pressWhileOpen = null;
        _pressReleased = false;
        return;
      }
      _pressReleased = true;
      // Only a popup dismissed by this completed tap may forward it. A drag
      // cancelled by the barrier must not turn a later Escape into a command.
      _pressExpiry?.cancel();
      // Navigator.maybePop may finish through microtasks. Retire an unused
      // tap at the next event turn, after those completions, without a delay.
      _pressExpiry = Timer(Duration.zero, () {
        _pressExpiry = null;
        if (identical(_pressWhileOpen, press) && _menuOpen) {
          _pressWhileOpen = null;
          _pressReleased = false;
        }
      });
    }
  }

  /// A control of the bar, findable by [_controlAt].
  Widget _control(String id, Widget child, {bool enabled = true}) {
    if (enabled) {
      _enabledControls.add(id);
    } else {
      _enabledControls.remove(id);
    }
    return KeyedSubtree(
      key: _controls.putIfAbsent(id, () => GlobalKey(debugLabel: id)),
      child: child,
    );
  }

  String? _controlAt(Offset position) {
    for (final entry in _controls.entries) {
      final box = entry.value.currentContext?.findRenderObject();
      if (_enabledControls.contains(entry.key) &&
          box is RenderBox &&
          box.attached &&
          box.hasSize &&
          Rect.fromPoints(
            box.localToGlobal(Offset.zero),
            box.localToGlobal(box.size.bottomRight(Offset.zero)),
          ).contains(position)) {
        return entry.key;
      }
    }
    return null;
  }

  void _menuChanged(bool open, {required String from, required int scope}) {
    widget.onMenuChanged?.call(open);
    if (scope != _menuScope) return;
    if (open) {
      _openedFrom = from;
      _menuContent = widget.contentKey;
      _menuLayout = widget.layoutKey;
      _pressWhileOpen = null;
      _pressReleased = false;
    } else {
      final released = _pressReleased;
      _pressExpiry?.cancel();
      _pressExpiry = null;
      _pressReleased = false;
      final press = _pressWhileOpen, origin = _openedFrom;
      _pressWhileOpen = _openedFrom = null;
      final target = press == null ? null : _controlAt(press.position);
      // The control that opened it only closes it (a toggle).
      if (released && press != null && target != null && target != origin) {
        _repeatPress(target, press.kind, scope);
      }
    }
    if (mounted && _menuOpen != open) setState(() => _menuOpen = open);
  }

  /// One tap on [target] after the closing popup stops taking the pointer
  /// (its barrier ignores input once it animates out).
  void _repeatPress(String target, PointerDeviceKind kind, int scope) {
    final generation = _repeatGeneration;
    final content = _menuContent, layout = _menuLayout;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted ||
          generation != _repeatGeneration ||
          scope != _menuScope ||
          widget.contentKey != content ||
          widget.layoutKey != layout ||
          widget.pointerHeld ||
          _menuOpen ||
          !isOpen ||
          !_enabledControls.contains(target)) {
        return;
      }
      final box = _controls[target]?.currentContext?.findRenderObject();
      if (box is! RenderBox || !box.attached || !box.hasSize) return;
      final at = box.localToGlobal(box.size.center(Offset.zero));
      if (_controlAt(at) != target) return;
      final pointer = ++_forwarded;
      final binding = GestureBinding.instance;
      binding.handlePointerEvent(
        PointerDownEvent(
          pointer: pointer,
          device: _forwardDevice,
          kind: kind,
          buttons: kPrimaryButton,
          position: at,
        ),
      );
      binding.handlePointerEvent(
        PointerUpEvent(
          pointer: pointer,
          device: _forwardDevice,
          kind: kind,
          buttons: 0,
          position: at,
        ),
      );
    });
    WidgetsBinding.instance.ensureVisualUpdate();
  }

  void _expand() {
    widget.onInteract();
    setState(() {
      _view = _View.bar;
      _handleHover = false;
    });
  }

  /// Folds the bar; the handle under the bar does this even while the bar
  /// is kept open (바 열어 두기 only stops it folding by itself).
  void _collapse() {
    if (!mounted || _view == _View.handle) return;
    _cancelRepeats();
    if (_barFocus.hasFocus) _barFocus.unfocus();
    setState(() {
      _view = _View.handle;
      _hovered = false;
    });
  }

  /// A command was used: the bar folds again unless it is kept open.
  void _used() {
    if (!_pinned) _collapse();
  }

  // Over the open bar, input returns to this PC: its commands are about to
  // be used. A pointer that arrives with a button held is a drag from
  // somewhere else and changes nothing.
  void _enterBar(PointerEvent event) {
    if (event.buttons != 0 || _hovered || widget.pointerHeld) return;
    widget.onInteract();
    setState(() => _hovered = true);
  }

  void _exitBar(PointerEvent _) {
    if (mounted && _hovered) setState(() => _hovered = false);
  }

  void _togglePin() => setState(() => _pinned = !_pinned);

  void _dragStart(DragStartDetails _) {
    widget.onInteract();
    setState(() => _dragging = true);
  }

  void _dragUpdate(DragUpdateDetails details, double limit) => setState(
    () => _offset = (_offset + details.delta.dx).clamp(-limit, limit),
  );

  void _dragEnd() {
    if (mounted) setState(() => _dragging = false);
  }

  bool get _hasLatency =>
      widget.connected &&
      widget.latencyMs != null &&
      widget.latencyMs!.isFinite &&
      widget.latencyMs! > 0;

  double? get _measured => _hasLatency ? widget.latencyMs : null;

  String get _latencyText {
    final latency = widget.latencyMs!;
    return latency < 1 ? '<1 ms' : '${latency.toStringAsFixed(0)} ms';
  }

  String get _status {
    if (!widget.connected) return l10n.waiting3;
    final latency = widget.latencyMs;
    return _hasLatency
        ? latency! < 1
              ? l10n.latencyUnder1Ms
              : l10n.latencyAboutMs(latency.toStringAsFixed(0))
        : l10n.connected;
  }

  String get _summary => [widget.title, ?widget.modeLabel, _status].join(' · ');

  // The shared toolbar order (SessionTool); anything else keeps its order.
  List<SessionToolbarMenu> _orderedMenus() => SessionTool.ordered(
    widget.actions.whereType<SessionToolbarMenu>().toList(),
    (menu) => menu.label,
  );

  SessionMenuPresenter? get _presenter {
    if (widget.menuPresenter != null) return widget.menuPresenter;
    for (final menu in widget.actions.whereType<SessionToolbarMenu>()) {
      if (menu.presenter != null) return menu.presenter;
    }
    return null;
  }

  static const _pinKey = Key('pin-session-toolbar');

  /// The window's own buttons, as on a title bar: — then ❐.
  List<SessionMenuItem> _windowItems() => [
    if (widget.onMinimize != null)
      SessionMenuItem(
        SessionTool.minimize.label,
        key: const Key('session-minimize'),
        icon: SessionTool.minimize.icon,
        onSelected: widget.onMinimize,
      ),
    if (widget.onRestore != null)
      SessionMenuItem(
        SessionTool.restore.label,
        key: const Key('session-restore'),
        icon: SessionTool.restore.icon,
        onSelected: widget.onRestore,
      ),
  ];

  /// 바 열어 두기 as a row of the narrow bar's 메뉴 (in a wide bar its
  /// switch is in the session's 설정 panel).
  SessionMenuItem _pinItem() => SessionMenuItem(
    SessionTool.pin.label,
    key: _pinKey,
    icon: _pinned ? Icons.push_pin : SessionTool.pin.icon,
    checked: _pinned,
    keepToolbarOpen: true,
    onSelected: _togglePin,
  );

  List<SessionMenuItem> _qualityMenu() => [
    SessionMenuItem(l10n.connectionQuality, heading: true),
    SessionMenuItem.info(
      l10n.roundTripLatencyHostResponse,
      _hasLatency ? _latencyText : (widget.connected ? l10n.notMeasuredYet : l10n.waiting3),
    ),
    ...?widget.qualityItems?.call(),
  ];

  /// [context]'s box down to the bar's bottom edge: where a menu or panel
  /// drops from.
  Rect _anchorOf(BuildContext context) {
    final box = context.findRenderObject() as RenderBox;
    final button = Rect.fromPoints(
      box.localToGlobal(Offset.zero),
      box.localToGlobal(box.size.bottomRight(Offset.zero)),
    );
    return Rect.fromLTWH(
      button.left,
      button.top,
      button.width,
      button.height + SessionChrome.belowButton,
    );
  }

  Future<void> _openMenu(
    BuildContext context,
    List<SessionMenuItem> items, {
    required String from,
  }) async {
    widget.onInteract();
    final lifecycle = _menuLifecycle(from);
    lifecycle(true);
    try {
      final anchor = _anchorOf(context);
      final presenter = _presenter;
      final selected = presenter != null
          ? await presenter(anchor, items)
          : await showSessionMenu(context, anchor, items, popups: _popups);
      if (context.mounted &&
          selected != null &&
          selected >= 0 &&
          selected < items.length) {
        widget.onInteract();
        items[selected].onSelected?.call();
        if (!items[selected].keepToolbarOpen) _used();
      }
    } finally {
      lifecycle(false);
    }
  }

  /// The chip's own panel where the session has one, else the quality menu.
  Future<void> _openQuality(BuildContext context) async {
    final at = widget.onQualityAt;
    if (at == null) {
      return _openMenu(context, _qualityMenu(), from: 'chip');
    }
    widget.onInteract();
    final lifecycle = _menuLifecycle('chip');
    lifecycle(true);
    var used = false;
    try {
      used = await at(_anchorOf(context));
    } finally {
      lifecycle(false);
    }
    if (used) _used();
  }

  Widget _iconButton(
    SessionChrome chrome, {
    required Key key,
    required String tooltip,
    required IconData icon,
    required VoidCallback onPressed,
    bool active = false,
    bool keepOpen = false,
    double width = _caption,
    double iconSize = 17,
  }) => SizedBox(
    width: width,
    height: SessionChrome.button,
    child: IconButton(
      key: key,
      tooltip: tooltip,
      isSelected: active,
      style: chrome
          .buttonStyle(active: active, width: width)
          .copyWith(iconSize: WidgetStatePropertyAll(iconSize)),
      onPressed: () {
        widget.onInteract();
        onPressed();
        if (!keepOpen) _used();
      },
      icon: Icon(icon),
    ),
  );

  Widget _divider(SessionChrome chrome) => Container(
    width: 1,
    height: 18,
    margin: const EdgeInsets.symmetric(horizontal: (_separator - 1) / 2),
    color: chrome.edge,
  );

  String get _qualityLabel => _hasLatency
      ? _latencyText
      : widget.connected
      ? l10n.connected
      : l10n.waiting3;

  static const _latencySizer = '999 ms';

  static double _textWidth(BuildContext context, String text, TextStyle style) {
    final painter = TextPainter(
      // As KeepAllText renders it.
      text: TextSpan(
        text: keepAll(text),
        style: DefaultTextStyle.of(context).style.merge(style),
      ),
      textDirection: Directionality.of(context),
      textScaler: MediaQuery.textScalerOf(context),
      maxLines: 1,
    )..layout();
    final width = painter.width;
    painter.dispose();
    return width.ceilToDouble();
  }

  static TextStyle _qualityStyle(Color color) => TextStyle(
    fontFamily: 'Pretendard',
    fontFamilyFallback: const ['Malgun Gothic'],
    fontSize: 13,
    fontWeight: FontWeight.w600,
    fontFeatures: const [FontFeature.tabularFigures()],
    color: color,
  );

  static const _handleText = TextStyle(
    fontFamily: 'Pretendard',
    fontFamilyFallback: ['Malgun Gothic'],
    fontSize: 12,
    height: 1.2,
    fontWeight: FontWeight.w600,
    fontFeatures: [FontFeature.tabularFigures()],
  );

  // Bars and the number only: the colour says how good it is, the tooltip
  // and the chip's panel say it in words.
  Widget _quality(SessionChrome chrome) {
    final color = widget.connected
        ? chrome.latencyColor(_measured)
        : chrome.faint;
    final label = _qualityLabel;
    final word = SessionChrome.latencyWord(_measured);
    return Builder(
      builder: (context) => Tooltip(
        message: _hasLatency ? l10n.connectionQuality2('$word') : l10n.connectionQuality3(_summary),
        child: Semantics(
          label: widget.title,
          value: [?widget.modeLabel, _status].join(' · '),
          button: true,
          excludeSemantics: true,
          child: TextButton(
            key: const Key('session-quality'),
            onPressed: () => unawaited(_openQuality(context)),
            style: ButtonStyle(
              minimumSize: const WidgetStatePropertyAll(
                Size(0, SessionChrome.button),
              ),
              padding: const WidgetStatePropertyAll(
                EdgeInsets.fromLTRB(9, 0, 10, 0),
              ),
              tapTargetSize: MaterialTapTargetSize.shrinkWrap,
              // As tall as the commands beside it, also at Windows' compact
              // density.
              visualDensity: VisualDensity.standard,
              splashFactory: NoSplash.splashFactory,
              overlayColor: WidgetStateProperty.resolveWith(
                (states) => states.contains(WidgetState.pressed)
                    ? chrome.pressed
                    : states.contains(WidgetState.hovered) ||
                          states.contains(WidgetState.focused)
                    ? chrome.hover
                    : null,
              ),
              shape: WidgetStateProperty.resolveWith(
                (states) => RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(9),
                  side: states.contains(WidgetState.focused)
                      ? BorderSide(color: chrome.accent, width: 1.5)
                      : BorderSide.none,
                ),
              ),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                // Not connected yet reads as "waiting", not as a weak signal.
                widget.connected
                    ? _SignalBars(
                        level: SessionChrome.latencyLevel(_measured),
                        color: color,
                        idle: chrome.faint,
                      )
                    : Container(
                        key: const Key('session-waiting-indicator'),
                        width: 11,
                        height: 11,
                        margin: const EdgeInsets.all(1),
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          border: Border.all(color: chrome.accent, width: 1.8),
                        ),
                      ),
                const SizedBox(width: 8),
                // The number is left-aligned in a box as wide as "999 ms"
                // (tabular figures), so its digit count never moves the
                // controls beside it.
                ConstrainedBox(
                  constraints: BoxConstraints(
                    minWidth: _textWidth(
                      context,
                      _latencySizer,
                      _qualityStyle(chrome.muted),
                    ),
                  ),
                  child: KeepAllText(
                    label,
                    maxLines: 1,
                    style: _qualityStyle(_hasLatency ? color : chrome.muted),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  WidgetStateProperty<Color> _disconnectColor(SessionChrome chrome) =>
      WidgetStateProperty.resolveWith(
        (states) =>
            widget.connected ||
                states.contains(WidgetState.hovered) ||
                states.contains(WidgetState.focused) ||
                states.contains(WidgetState.pressed)
            ? chrome.danger
            : chrome.muted,
      );

  // An icon like every other command: danger red once there is
  // something to cut off, its own separator at the end, and the question.
  Widget _disconnect(SessionChrome chrome) {
    const size = Size(_square, SessionChrome.button);
    final style = ButtonStyle(
      fixedSize: const WidgetStatePropertyAll(size),
      minimumSize: const WidgetStatePropertyAll(size),
      maximumSize: const WidgetStatePropertyAll(size),
      padding: const WidgetStatePropertyAll(EdgeInsets.zero),
      tapTargetSize: MaterialTapTargetSize.shrinkWrap,
      splashFactory: NoSplash.splashFactory,
      iconSize: const WidgetStatePropertyAll(18),
      // Until the session is connected there is nothing to cut off yet: the
      // button stays quiet and turns red only on hover/focus.
      foregroundColor: _disconnectColor(chrome),
      iconColor: _disconnectColor(chrome),
      overlayColor: WidgetStateProperty.resolveWith(
        (states) => states.contains(WidgetState.pressed)
            ? chrome.dangerPressed
            : states.contains(WidgetState.hovered) ||
                  states.contains(WidgetState.focused)
            ? chrome.dangerHover
            : null,
      ),
      shape: WidgetStateProperty.resolveWith(
        (states) => RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(9),
          side: states.contains(WidgetState.focused)
              ? BorderSide(color: chrome.accent, width: 1.5)
              : BorderSide.none,
        ),
      ),
    );
    return SizedBox(
      width: _square,
      height: SessionChrome.button,
      child: Tooltip(
        message: SessionTool.disconnect.label,
        child: TextButton(
          key: const Key('session-disconnect'),
          onPressed: () {
            widget.onInteract();
            widget.onDisconnect!();
            _used();
          },
          style: style,
          child: Icon(SessionTool.disconnect.icon),
        ),
      ),
    );
  }

  static String get _menuLabel => l10n.menu;

  // chip | the session's commands (or one 메뉴) | the window's buttons |
  // disconnect: the one danger action stands apart at the end.
  ({Widget bar, double width}) _bar(
    BuildContext context,
    SessionChrome chrome,
    double available,
  ) {
    _enabledControls.clear();
    final menus = _orderedMenus();
    final others = widget.actions
        .where((action) => action is! SessionToolbarMenu)
        .toList();
    final windowItems = _windowItems();
    final disconnect = widget.onDisconnect != null;
    // Room for "999 ms" as on the handle: 7 → 70 → 700 ms must not resize
    // the bar.
    final chipWidth =
        9 +
        13 +
        8 +
        math.max(
          _textWidth(context, _qualityLabel, _qualityStyle(chrome.muted)),
          _textWidth(context, _latencySizer, _qualityStyle(chrome.muted)),
        ) +
        10 +
        2;
    final menuButton = SessionToolbarMenu.command(
      label: _menuLabel,
      icon: Icons.menu_rounded,
      onPressed: () {},
      onInteract: widget.onInteract,
    );
    double width({required bool compact, required bool window}) {
      final commands = compact
          ? menuButton.toolbarWidth(context)
          : menus.fold(0.0, (sum, menu) => sum + menu.toolbarWidth(context));
      final middle = commands + others.length * _square;
      final windowWidth = window ? windowItems.length * _caption : 0.0;
      return 2 * _pad +
          chipWidth +
          (middle > 0 ? _separator + middle : 0) +
          (windowWidth > 0 ? _separator + windowWidth : 0) +
          (disconnect ? _separator + _square : 0);
    }

    // The whole row; else the commands as one 메뉴; else the window's
    // buttons join that menu too.
    var compact = false, windowShown = windowItems.isNotEmpty;
    if (menus.isNotEmpty &&
        width(compact: false, window: windowShown) > available) {
      compact = true;
    }
    if (windowShown && width(compact: compact, window: true) > available) {
      windowShown = false;
      compact = compact || menus.isNotEmpty || windowItems.isNotEmpty;
    }
    final windowHidden = windowItems.isNotEmpty && !windowShown;
    // 바 열어 두기 joins the last group when that is 설정, else its own.
    final pinInGroup =
        menus.isNotEmpty && menus.last.label == SessionTool.more.label;
    final more = Builder(
      builder: (context) => SizedBox(
        width: menuButton.toolbarWidth(context),
        height: SessionChrome.button,
        child: Tooltip(
          message: l10n.menuScreenSettingsMore,
          child: TextButton(
            key: const Key('session-toolbar-overflow'),
            style: chrome
                .buttonStyle(width: menuButton.toolbarWidth(context))
                .copyWith(
                  padding: const WidgetStatePropertyAll(
                    EdgeInsets.symmetric(horizontal: 6),
                  ),
                  iconSize: const WidgetStatePropertyAll(18),
                  foregroundColor: WidgetStatePropertyAll(chrome.ink),
                ),
            onPressed: () => unawaited(
              _openMenu(context, [
                for (final menu in menus) ...menu.overflowItems,
                if (!pinInGroup) SessionMenuItem(l10n.toolbar, heading: true),
                _pinItem(),
                if (windowHidden) ...[
                  SessionMenuItem(l10n.window, heading: true),
                  ...windowItems,
                ],
              ], from: 'more'),
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                const Icon(Icons.menu_rounded),
                const SizedBox(width: 7),
                KeepAllText(
                  _menuLabel,
                  maxLines: 1,
                  style: _qualityStyle(chrome.ink),
                ),
              ],
            ),
          ),
        ),
      ),
    );
    Widget command(SessionMenuItem item) => _iconButton(
      chrome,
      key: item.key!,
      tooltip: item.label,
      icon: item.icon!,
      active: item.checked,
      onPressed: item.onSelected!,
    );
    final row = Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        _control('chip', _quality(chrome)),
        if (menus.isNotEmpty || others.isNotEmpty) ...[
          _divider(chrome),
          if (compact)
            _control('more', more)
          else
            for (final menu in menus)
              _control(
                'menu:${menu.label}',
                menu.withLifecycle(
                  _menuLifecycle('menu:${menu.label}'),
                  denseLabel: true,
                  commandBar: true,
                  used: _used,
                  popups: _popups,
                ),
                enabled: menu.enabled,
              ),
          ...others,
        ] else if (compact) ...[
          _divider(chrome),
          _control('more', more),
        ],
        if (windowShown) ...[
          _divider(chrome),
          for (final item in windowItems)
            _control('window:${item.key}', command(item)),
        ],
        if (disconnect) ...[
          _divider(chrome),
          _control('disconnect', _disconnect(chrome)),
        ],
      ],
    );
    final natural = width(compact: compact, window: windowShown);
    // Estimates can be a pixel off with font fallback; never overflow.
    final content = Padding(
      padding: const EdgeInsets.symmetric(horizontal: _pad),
      child: FittedBox(fit: BoxFit.scaleDown, child: row),
    );
    return (bar: content, width: math.min(natural, available));
  }

  String get _handleLabel => _hasLatency
      ? _latencyText
      : widget.connected
      ? l10n.connected
      : l10n.waiting3;

  Color get _signal => !widget.connected
      ? const Color(0xFF5E6A73)
      : _hasLatency
      ? SessionChrome.of(context).latencyColor(_measured)
      : SessionChrome.of(context).muted;

  static const _restFill = Color(0xEB161A1E),
      _tabFill = Color(0xFF20262B),
      _handleEdge = Color(0x2BFFFFFF);

  /// What the folded handle shows now: its content, its size and its edge.
  ({Widget child, Size size, Color fill, Color edge}) _handleLook(
    BuildContext context,
    SessionChrome chrome,
  ) {
    double text(String value) => _textWidth(context, value, _handleText);
    // The tongue's sides curve in: the content keeps clear of them.
    const side = 24.0;
    Size tab(double content) => Size(
      math.max(_restSize.width, (2 * side + content).ceilToDouble()),
      _tabHeight,
    );
    Widget label(String value, Color color) => KeepAllText(
      value,
      maxLines: 1,
      overflow: TextOverflow.clip,
      style: _handleText.copyWith(color: color),
    );
    Widget row(List<Widget> children) => FittedBox(
      fit: BoxFit.scaleDown,
      child: Row(mainAxisSize: MainAxisSize.min, children: children),
    );
    final status = handleStatus;
    if (status == SessionHandleStatus.mouseLock) {
      final title = l10n.mouseLock, tail = l10n.release;
      final keys = SessionKeycaps.mouseLock.fold(
        0.0,
        (sum, key) => sum + text(key) + 12 + 3,
      );
      return (
        child: row([
          Icon(Icons.lock_rounded, size: 13, color: chrome.accent),
          const SizedBox(width: 6),
          label(title, chrome.accent),
          const SizedBox(width: 8),
          SessionKeycaps(SessionKeycaps.mouseLock, color: chrome.ink),
          const SizedBox(width: 6),
          label(tail, chrome.muted),
        ]),
        size: tab(13 + 6 + text(title) + 8 + keys + 6 + text(tail)),
        fill: _restFill,
        edge: chrome.accent.withValues(alpha: .6),
      );
    }
    if (status == SessionHandleStatus.resuming) {
      final message = l10n.reconnecting2;
      return (
        child: row([
          _dot(chrome.fair),
          const SizedBox(width: 6),
          label(message, chrome.ink),
        ]),
        size: tab(6 + 6 + text(message)),
        fill: _restFill,
        edge: _handleEdge,
      );
    }
    if (status == SessionHandleStatus.poor) {
      // While it waits out the hold after getting better, the tab keeps
      // its place but reads the number as it is now, never "20 ms · 느림".
      final poor = _poorNow;
      final message = !_hasLatency
          ? l10n.slow
          : poor
          ? l10n.slow2(_latencyText)
          : _latencyText;
      final color = poor ? chrome.poor : _signal;
      return (
        child: row([
          _SignalBars(
            level: poor ? 1 : SessionChrome.latencyLevel(_measured),
            color: color,
            idle: chrome.faint,
            size: 12,
          ),
          const SizedBox(width: 6),
          label(message, color),
        ]),
        size: tab(12 + 6 + math.max(text(message), text(l10n.n999MsSlow))),
        fill: _restFill,
        edge: poor ? chrome.poor.withValues(alpha: .55) : _handleEdge,
      );
    }
    if (_handleHover && !widget.touch) {
      // Room for "999 ms", so a changing number never moves the tab under
      // the pointer.
      final number = math.max(text(_handleLabel), text(_latencySizer));
      return (
        child: row([
          widget.connected
              ? _SignalBars(
                  level: SessionChrome.latencyLevel(_measured),
                  color: _signal,
                  idle: chrome.faint,
                  size: 12,
                )
              : _dot(_signal),
          const SizedBox(width: 6),
          SizedBox(width: number, child: label(_handleLabel, chrome.ink)),
          const SizedBox(width: 4),
          Icon(
            Icons.keyboard_arrow_down_rounded,
            size: 14,
            color: chrome.accent,
          ),
        ]),
        size: tab(12 + 6 + number + 4 + 14),
        fill: _tabFill,
        edge: _handleEdge,
      );
    }
    return (
      child: Row(
        key: const Key('session-handle-grip'),
        mainAxisSize: MainAxisSize.min,
        children: [
          _dot(_signal),
          const SizedBox(width: 5),
          Icon(
            Icons.keyboard_arrow_down_rounded,
            size: 12,
            color: chrome.muted,
          ),
        ],
      ),
      size: _restSize,
      fill: _restFill,
      edge: _handleEdge,
    );
  }

  // Outlined so it holds on a white title bar.
  static Widget _dot(Color color) => Container(
    width: 6,
    height: 6,
    decoration: BoxDecoration(
      color: color,
      shape: BoxShape.circle,
      boxShadow: const [BoxShadow(color: Color(0x80000000), spreadRadius: .6)],
    ),
  );

  void _report(Rect bounds) {
    if (_reported == bounds) return;
    _reported = bounds;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && _reported == bounds) widget.onBounds?.call(bounds);
    });
  }

  void _hoverHandle(PointerEvent event) {
    // A pointer that arrives with a button held is a drag from elsewhere.
    if (_handleHover || event.buttons != 0 || widget.pointerHeld) return;
    setState(() => _handleHover = true);
  }

  Widget _handle(
    SessionChrome chrome,
    double limit,
    ({Widget child, Size size, Color fill, Color edge}) look,
  ) {
    final tongue = SizedBox.fromSize(
      size: look.size,
      child: CustomPaint(
        key: const Key('session-tab'),
        painter: _TonguePainter(fill: look.fill, edge: look.edge),
        child: Material(
          type: MaterialType.transparency,
          child: InkWell(
            key: const Key('expand-session-toolbar'),
            // By finger the whole (larger) hit area opens the bar.
            onTap: widget.touch ? null : _expand,
            hoverColor: Colors.transparent,
            highlightColor: Colors.transparent,
            splashFactory: NoSplash.splashFactory,
            customBorder: const _TongueBorder(),
            child: Padding(
              padding: const EdgeInsets.only(bottom: 1),
              child: Center(child: look.child),
            ),
          ),
        ),
      ),
    );
    return MouseRegion(
      opaque: true,
      // A click opens the bar; a drag moves the handle. Hover only shows
      // the number.
      cursor: _dragging
          ? SystemMouseCursors.grabbing
          : SystemMouseCursors.click,
      onEnter: _hoverHandle,
      onHover: _hoverHandle,
      onExit: (_) {
        if (mounted && _handleHover) setState(() => _handleHover = false);
      },
      child: Listener(
        behavior: HitTestBehavior.opaque,
        onPointerDown: (_) => widget.onInteract(),
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onHorizontalDragStart: _dragStart,
          onHorizontalDragUpdate: (details) => _dragUpdate(details, limit),
          onHorizontalDragEnd: (_) => _dragEnd(),
          onHorizontalDragCancel: _dragEnd,
          onTap: widget.touch
              ? () {
                  unawaited(HapticFeedback.selectionClick());
                  _expand();
                }
              : null,
          child: Tooltip(
            message: l10n.clickOpenToolbar(_summary),
            verticalOffset: 18,
            preferBelow: true,
            waitDuration: const Duration(milliseconds: 600),
            decoration: chrome.tooltipTheme.decoration,
            textStyle: chrome.tooltipTheme.textStyle,
            padding: chrome.tooltipTheme.padding,
            child: Align(alignment: Alignment.topCenter, child: tongue),
          ),
        ),
      ),
    );
  }

  /// The same handle under the open bar: it folds it.
  Widget _lip(SessionChrome chrome) => SizedBox.fromSize(
    size: _lipSize,
    child: CustomPaint(
      painter: _TonguePainter(fill: chrome.surface, edge: chrome.edge),
      child: Material(
        type: MaterialType.transparency,
        child: Tooltip(
          message: l10n.foldToolbarEsc,
          decoration: chrome.tooltipTheme.decoration,
          textStyle: chrome.tooltipTheme.textStyle,
          padding: chrome.tooltipTheme.padding,
          preferBelow: true,
          verticalOffset: 14,
          waitDuration: const Duration(milliseconds: 450),
          child: InkWell(
            key: const Key('session-toolbar-fold'),
            onTap: () {
              widget.onInteract();
              _collapse();
            },
            hoverColor: chrome.hover,
            highlightColor: chrome.pressed,
            splashFactory: NoSplash.splashFactory,
            customBorder: const _TongueBorder(),
            child: Center(
              child: Icon(
                Icons.keyboard_arrow_up_rounded,
                size: 13,
                color: chrome.muted,
              ),
            ),
          ),
        ),
      ),
    ),
  );

  Widget _expanded(SessionChrome chrome, Widget bar, double limit) =>
      MouseRegion(
        opaque: true,
        // The bar's empty space moves it (buttons keep their hand cursor).
        cursor: _dragging
            ? SystemMouseCursors.grabbing
            : SystemMouseCursors.grab,
        onEnter: _enterBar,
        // A drag that crossed in and ended here counts once the button is up.
        onHover: _enterBar,
        onExit: _exitBar,
        child: Listener(
          behavior: HitTestBehavior.opaque,
          onPointerDown: (_) => widget.onInteract(),
          child: Focus(
            focusNode: _barFocus,
            skipTraversal: true,
            canRequestFocus: !widget.inputCaptured,
            descendantsAreFocusable: !widget.inputCaptured,
            onKeyEvent: (_, event) {
              if (event is KeyDownEvent &&
                  event.logicalKey == LogicalKeyboardKey.escape &&
                  !_pinned) {
                _collapse();
                return KeyEventResult.handled;
              }
              return KeyEventResult.ignored;
            },
            child: FocusTraversalGroup(
              child: TooltipTheme(
                data: chrome.tooltipTheme,
                child: GestureDetector(
                  key: const Key('drag-session-toolbar'),
                  behavior: HitTestBehavior.opaque,
                  onHorizontalDragStart: _dragStart,
                  onHorizontalDragUpdate: (details) =>
                      _dragUpdate(details, limit),
                  onHorizontalDragEnd: (_) => _dragEnd(),
                  onHorizontalDragCancel: _dragEnd,
                  child: Material(
                    key: const Key('session-command-bar'),
                    color: chrome.surface,
                    elevation: 0,
                    clipBehavior: Clip.antiAlias,
                    shape: RoundedRectangleBorder(
                      borderRadius: _barRadius,
                      side: BorderSide(color: chrome.edge),
                    ),
                    // The bar owns its hit path even when capture was true
                    // at pointer-down. Its outer Listener releases remote input
                    // synchronously; absorbing here would discard that first
                    // press before the release can rebuild these controls.
                    // A remote-held gesture is still excluded by IgnorePointer.
                    child: Center(child: bar),
                  ),
                ),
              ),
            ),
          ),
        ),
      );

  // Flat against the top edge it hangs from.
  static const _barRadius = BorderRadius.vertical(bottom: Radius.circular(14));

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    return LayoutBuilder(
      builder: (context, constraints) {
        // A resized window (or another DPI or monitor part) moved what an
        // open menu was anchored to and is about to change the remote
        // resolution its rows show.
        final area = constraints.biggest;
        final resized = _area != null && _area != area;
        _area = area;
        if (resized) _reset(fold: true);
        final maxWidth = constraints.maxWidth;
        final available = math.max(0.0, maxWidth - 16);
        final expanded = _view == _View.bar;
        final built = expanded ? _bar(context, chrome, available) : null;
        final look = expanded ? null : _handleLook(context, chrome);
        // By finger the folded handle sits in a larger invisible hit area.
        final touchHandle = widget.touch && !expanded;
        final handleSize = look == null
            ? _restSize
            : Size(math.min(look.size.width, available), look.size.height);
        final width =
            built?.width ??
            (touchHandle
                ? math.min(
                    math.max(handleSize.width, _touchHit.width),
                    available,
                  )
                : handleSize.width);
        // The limit for moving keeps the bar or handle inside this area
        // (a spanning window passes the primary monitor's part).
        final limit = math.max(0.0, (maxWidth - width) / 2);
        // A moved bar stays where it was within the new area.
        if (resized) _offset = _offset.clamp(-limit, limit);
        final offset = _offset.clamp(-limit, limit);
        final left = (maxWidth - width) / 2 + offset;
        final height = expanded
            ? _barHeight
            : touchHandle
            ? _touchHit.height
            : handleSize.height;
        final bounds = Rect.fromLTWH(
          left,
          0,
          width,
          math.max(0.0, math.min(height, constraints.maxHeight)),
        );
        _report(bounds);
        // Report open/fold after the frame.
        if (expanded != _reportedOpen) {
          _reportedOpen = expanded;
          WidgetsBinding.instance.addPostFrameCallback((_) {
            if (mounted && _reportedOpen == expanded) {
              widget.onOpenChanged?.call(expanded);
            }
          });
        }
        final stack = Stack(
          clipBehavior: Clip.none,
          children: [
            Positioned.fromRect(
              rect: bounds,
              // A press that began on the remote picture owns the pointer
              // until it ends: no hover, no hit testing here.
              child: IgnorePointer(
                key: const Key('session-toolbar-pointer'),
                ignoring: widget.pointerHeld,
                child: expanded
                    ? DecoratedBox(
                        decoration: BoxDecoration(
                          borderRadius: _barRadius,
                          boxShadow: [
                            BoxShadow(
                              color: chrome.shadow.withValues(
                                alpha: _hovered || _menuOpen ? .75 : .55,
                              ),
                              blurRadius: 24,
                              spreadRadius: -8,
                              offset: const Offset(0, 10),
                            ),
                          ],
                        ),
                        child: _expanded(chrome, built!.bar, limit),
                      )
                    : _handle(chrome, limit, (
                        child: look!.child,
                        size: handleSize,
                        fill: look.fill,
                        edge: look.edge,
                      )),
              ),
            ),
            // The handle under the bar, at the bar's centre: one more press
            // at the same place folds what a press there opened.
            // It makes way for a menu or panel dropped from the bar (their
            // barrier takes the pointer anyway).
            if (expanded && !_menuOpen)
              Positioned(
                left: bounds.center.dx - _lipSize.width / 2,
                top: bounds.bottom - 1,
                width: _lipSize.width,
                height: _lipSize.height,
                child: IgnorePointer(
                  ignoring: widget.pointerHeld,
                  child: _control('fold', _lip(chrome)),
                ),
              ),
          ],
        );
        return widget.touch ? SessionTouchScope(child: stack) : stack;
      },
    );
  }
}

/// The handle's shape: a soft tongue hanging from the edge above it.
Path _tonguePath(Size size, {bool closed = true}) {
  final w = size.width, h = size.height;
  final r = math.min(h * .9, math.min(13.0, w / 4));
  final path = Path()
    ..moveTo(0, 0)
    ..cubicTo(r, 0, r, h, 2 * r, h)
    ..lineTo(w - 2 * r, h)
    ..cubicTo(w - r, h, w - r, 0, w, 0);
  if (closed) path.close();
  return path;
}

class _TonguePainter extends CustomPainter {
  const _TonguePainter({required this.fill, required this.edge});
  final Color fill, edge;

  @override
  void paint(Canvas canvas, Size size) {
    final shape = _tonguePath(size);
    // A soft shadow lifts it off the picture; a hairline keeps its edge on
    // a white title bar.
    canvas.drawPath(
      shape.shift(const Offset(0, 2)),
      Paint()
        ..color = const Color(0x52000000)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 4),
    );
    canvas.drawPath(shape, Paint()..color = fill);
    canvas.drawPath(
      _tonguePath(size, closed: false),
      Paint()
        ..color = edge
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1,
    );
  }

  @override
  bool shouldRepaint(_TonguePainter old) =>
      old.fill != fill || old.edge != edge;
}

/// The tongue as an ink shape, so a press lights only the tongue.
class _TongueBorder extends ShapeBorder {
  const _TongueBorder();

  @override
  EdgeInsetsGeometry get dimensions => EdgeInsets.zero;

  @override
  Path getInnerPath(Rect rect, {TextDirection? textDirection}) =>
      getOuterPath(rect, textDirection: textDirection);

  @override
  Path getOuterPath(Rect rect, {TextDirection? textDirection}) =>
      _tonguePath(rect.size).shift(rect.topLeft);

  @override
  void paint(Canvas canvas, Rect rect, {TextDirection? textDirection}) {}

  @override
  ShapeBorder scale(double t) => this;
}

/// Three rising bars; [level] of them lit in [color].
class _SignalBars extends StatelessWidget {
  const _SignalBars({
    required this.level,
    required this.color,
    required this.idle,
    this.size = 13,
  });
  final int level;
  final Color color, idle;
  final double size;
  @override
  Widget build(BuildContext context) => CustomPaint(
    size: Size.square(size),
    painter: _SignalPainter(level, color, idle),
  );
}

class _SignalPainter extends CustomPainter {
  const _SignalPainter(this.level, this.color, this.idle);
  final int level;
  final Color color, idle;
  @override
  void paint(Canvas canvas, Size size) {
    final width = size.width * 3 / 13, gap = size.width * 2 / 13;
    for (var i = 0; i < 3; i++) {
      final height = size.height * (i + 2) / 4;
      final rect = RRect.fromRectAndRadius(
        Rect.fromLTWH(i * (width + gap), size.height - height, width, height),
        const Radius.circular(1),
      );
      canvas.drawRRect(
        rect,
        Paint()..color = i < level ? color : idle.withValues(alpha: .45),
      );
    }
  }

  @override
  bool shouldRepaint(_SignalPainter old) =>
      old.level != level || old.color != color || old.idle != idle;
}

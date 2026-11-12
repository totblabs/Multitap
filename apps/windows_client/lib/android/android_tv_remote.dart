import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../ui/app_dialog.dart';
import '../ui/app_theme.dart';
import '../ui/session_menu.dart';
import 'android_toolbar.dart';
import 'android_touch.dart';
import '../l10n/l10n.dart';

enum AndroidTvPointerMode {
  pointer,
  drag,
  scroll;

  const AndroidTvPointerMode();
  String get label => switch (this) {
    AndroidTvPointerMode.pointer => l10n.pointer,
    AndroidTvPointerMode.drag => l10n.drag,
    AndroidTvPointerMode.scroll => l10n.scroll,
  };

}

/// Bound at button-down to its original connection, including its cancel path.
typedef AndroidTvDragRelease = void Function(
  Offset point, {
  required bool cancelled,
});

/// Owns only TV remote navigation while the remote picture itself has focus.
/// The native activity leaves real QWERTY keyboard input on its existing path.
class AndroidTvRemote extends StatefulWidget {
  const AndroidTvRemote({
    super.key,
    required this.enabled,
    required this.session,
    required this.position,
    required this.source,
    required this.canControl,
    required this.onMove,
    required this.onClick,
    required this.onMenu,
    this.mode = AndroidTvPointerMode.pointer,
    this.speed = AndroidPointerSpeed.standard,
    this.onDragStart,
    this.onScroll,
    required this.child,
  });
  final bool enabled;
  final Object? session;
  final Offset Function() position;
  final Size? Function() source;
  final bool Function() canControl;
  final ValueChanged<Offset> onMove;
  final void Function(Offset point, bool right) onClick;
  final VoidCallback onMenu;
  final AndroidTvPointerMode mode;
  final AndroidPointerSpeed speed;
  final AndroidTvDragRelease Function(Offset point)? onDragStart;
  final void Function(Offset point, int delta)? onScroll;
  final Widget child;

  @override
  AndroidTvRemoteState createState() => AndroidTvRemoteState();
}

class AndroidTvRemoteState extends State<AndroidTvRemote>
    with WidgetsBindingObserver {
  final _focus = FocusNode(debugLabel: 'TV remote picture');
  final _directions = <LogicalKeyboardKey>{};
  Timer? _movement, _hold;
  int _ticks = 0;
  bool _select = false, _longSelect = false, _foreground = true;
  AndroidTvDragRelease? _dragRelease;
  Offset? _dragPoint;

  bool get _owns =>
      mounted &&
      widget.enabled &&
      _foreground &&
      _focus.hasPrimaryFocus &&
      ModalRoute.of(context)?.isCurrent != false &&
      widget.canControl();

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    FocusManager.instance.addListener(_focusChanged);
    final state = WidgetsBinding.instance.lifecycleState;
    _foreground = state == null || state == AppLifecycleState.resumed;
  }

  @override
  void didUpdateWidget(AndroidTvRemote oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!widget.enabled ||
        oldWidget.session != widget.session ||
        oldWidget.mode != widget.mode ||
        oldWidget.speed != widget.speed) {
      cancel();
    }
    if (widget.enabled &&
        (!oldWidget.enabled || oldWidget.session != widget.session)) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted &&
            widget.enabled &&
            ModalRoute.of(context)?.isCurrent != false) {
          _focus.requestFocus();
        }
      });
    }
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    _foreground = state == AppLifecycleState.resumed;
    if (!_foreground) cancel();
  }

  void _focusChanged() {
    // A local descendant leaves hasFocus true; a drag requires primary focus.
    if (!_focus.hasPrimaryFocus) cancel();
  }

  /// Menus, native focus loss, lease changes and disposal retire all repeats.
  void cancel() {
    _movement?.cancel();
    _movement = null;
    _hold?.cancel();
    _hold = null;
    _directions.clear();
    _ticks = 0;
    _select = false;
    _longSelect = false;
    final release = _dragRelease;
    _dragRelease = null;
    final point = _dragPoint;
    _dragPoint = null;
    if (release != null) {
      release(point ?? const Offset(.5, .5), cancelled: true);
      _refreshMode();
    }
  }

  void _refreshMode() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) setState(() {});
    });
    WidgetsBinding.instance.ensureVisualUpdate();
  }

  void _move() {
    if (!_owns) {
      cancel();
      return;
    }
    if (widget.mode == AndroidTvPointerMode.scroll) {
      final dy =
          (_directions.contains(LogicalKeyboardKey.arrowUp) ? 1 : 0) -
          (_directions.contains(LogicalKeyboardKey.arrowDown) ? 1 : 0);
      if (dy != 0) widget.onScroll?.call(widget.position(), dy * 120);
      return;
    }
    final size = widget.source();
    if (size == null ||
        !size.width.isFinite ||
        !size.height.isFinite ||
        size.width <= 0 ||
        size.height <= 0) {
      cancel();
      return;
    }
    final dx =
        (_directions.contains(LogicalKeyboardKey.arrowRight) ? 1 : 0) -
        (_directions.contains(LogicalKeyboardKey.arrowLeft) ? 1 : 0);
    final dy =
        (_directions.contains(LogicalKeyboardKey.arrowDown) ? 1 : 0) -
        (_directions.contains(LogicalKeyboardKey.arrowUp) ? 1 : 0);
    final precise = widget.speed == AndroidPointerSpeed.precise;
    final boost = _ticks < 12
        ? 1.0
        : _ticks < 30
        ? 2.0
        : widget.speed.acceleration
        ? 3.5
        : 2.5;
    // One short precise tap is exactly one remote pixel. Held motion and
    // the other modes use the same saved gain as the touch pointer.
    final step = precise && _ticks == 0
        ? 1.0
        : (precise ? 2.0 : 8.0) * widget.speed.gain * boost;
    _ticks++;
    final old = widget.position();
    final point = Offset(
      (old.dx + dx * step / size.width).clamp(0.0, 1.0),
      (old.dy + dy * step / size.height).clamp(0.0, 1.0),
    );
    if (point != old) {
      if (_dragRelease != null) _dragPoint = point;
      widget.onMove(point);
    }
  }

  static bool _direction(LogicalKeyboardKey key) =>
      key == LogicalKeyboardKey.arrowLeft ||
      key == LogicalKeyboardKey.arrowRight ||
      key == LogicalKeyboardKey.arrowUp ||
      key == LogicalKeyboardKey.arrowDown;

  KeyEventResult _key(FocusNode _, KeyEvent event) {
    if (!_owns) {
      cancel();
      return KeyEventResult.ignored;
    }
    final key = event.logicalKey;
    if (key == LogicalKeyboardKey.contextMenu) {
      if (event is KeyDownEvent) {
        cancel();
        widget.onMenu();
      }
      return KeyEventResult.handled;
    }
    if (_direction(key)) {
      // No horizontal-wheel message exists in this protocol. Do not turn
      // sideways presses into another operation while scroll mode owns input.
      if (widget.mode == AndroidTvPointerMode.scroll &&
          (key == LogicalKeyboardKey.arrowLeft ||
              key == LogicalKeyboardKey.arrowRight)) {
        return KeyEventResult.handled;
      }
      if (event is KeyUpEvent) {
        _directions.remove(key);
        if (_directions.isEmpty) {
          _movement?.cancel();
          _movement = null;
          _ticks = 0;
        }
      } else if (event is KeyDownEvent && _directions.add(key)) {
        _move();
        if (_directions.isNotEmpty) {
          _movement ??= Timer.periodic(
            Duration(
              milliseconds: widget.mode == AndroidTvPointerMode.scroll
                  ? 100
                  : 33,
            ),
            (_) => _move(),
          );
        }
      }
      return KeyEventResult.handled;
    }
    if (key == LogicalKeyboardKey.select ||
        key == LogicalKeyboardKey.enter ||
        key == LogicalKeyboardKey.numpadEnter) {
      if (event is KeyDownEvent && !_select) {
        _select = true;
        _longSelect = false;
        if (widget.mode == AndroidTvPointerMode.drag) {
          final release = _dragRelease;
          _dragRelease = null;
          if (release != null) {
            release(_dragPoint ?? widget.position(), cancelled: false);
            _dragPoint = null;
          } else {
            _dragPoint = widget.position();
            _dragRelease = widget.onDragStart?.call(_dragPoint!);
          }
          _refreshMode();
          return KeyEventResult.handled;
        }
        _hold = Timer(const Duration(milliseconds: 550), () {
          if (!_owns || !_select) {
            cancel();
            return;
          }
          _longSelect = true;
          widget.onClick(widget.position(), true);
        });
      } else if (event is KeyUpEvent && _select) {
        _hold?.cancel();
        _hold = null;
        final click = !_longSelect && widget.mode != AndroidTvPointerMode.drag;
        _select = false;
        _longSelect = false;
        if (click) widget.onClick(widget.position(), false);
      }
      return KeyEventResult.handled;
    }
    return KeyEventResult.ignored;
  }

  @override
  void dispose() {
    cancel();
    WidgetsBinding.instance.removeObserver(this);
    FocusManager.instance.removeListener(_focusChanged);
    _focus.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Focus(
    focusNode: _focus,
    autofocus: widget.enabled,
    canRequestFocus: widget.enabled,
    skipTraversal: true,
    onFocusChange: (focused) {
      if (!focused) cancel();
    },
    onKeyEvent: _key,
    child: Stack(
      fit: StackFit.expand,
      children: [
        widget.child,
        if (widget.enabled && widget.mode != AndroidTvPointerMode.pointer)
          Positioned(
            left: 12,
            right: 12,
            top: MediaQuery.paddingOf(context).top + 12,
            child: Align(
              alignment: Alignment.topLeft,
              child: IgnorePointer(
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: const Color(0xE61A1F24),
                    border: Border.all(color: const Color(0x66F5A623)),
                    borderRadius: BorderRadius.circular(8),
                  ),
                  child: Padding(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 10,
                      vertical: 6,
                    ),
                    child: Text(
                      widget.mode == AndroidTvPointerMode.scroll
                          ? l10n.scrollUpDown
                          : _dragRelease != null
                          ? l10n.draggingOkRelease
                          : l10n.dragOkGrab,
                      key: const Key('android-tv-pointer-mode'),
                      maxLines: 2,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        color: Color(0xFFFFC264),
                        fontSize: 12,
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
      ],
    ),
  );
}

/// Always local; the caller performs the chosen action through existing paths.
Future<String?> showAndroidTvRemoteMenu(
  BuildContext context, {
  required bool keyboardOpen,
  AndroidTvPointerMode mode = AndroidTvPointerMode.pointer,
}) => showDialog<String>(
  context: context,
  builder: (context) {
    final p = AppPalette.of(context);
    return AppDialog(
      title: l10n.remoteScreenMenu,
      icon: Icons.settings_remote_rounded,
      maxWidth: 400,
      body: [
        FilledButton(
          autofocus: true,
          style: AppButtons.filled(p),
          onPressed: () => Navigator.pop(context, 'keyboard'),
          child: Text(keyboardOpen ? l10n.closeKeyboard : l10n.openKeyboard),
        ),
        const SizedBox(height: 8),
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            for (final entry in [('screen', l10n.screenSettings), ('shortcuts', l10n.shortcuts)])
              OutlinedButton(
                key: Key('android-tv-menu-${entry.$1}'),
                style: AppButtons.outlined(p),
                onPressed: () => Navigator.pop(context, entry.$1),
                child: Text(entry.$2),
              ),
          ],
        ),
        const SizedBox(height: 12),
        Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            for (final value in AndroidTvPointerMode.values)
              TextButton(
                key: Key('android-tv-mode-${value.name}'),
                style: value == mode
                    ? AppButtons.tonal(p)
                    : AppButtons.quiet(p),
                onPressed: () => Navigator.pop(context, 'mode:${value.name}'),
                child: Text(value.label),
              ),
          ],
        ),
        const SizedBox(height: 12),
        OutlinedButton(
          style: AppButtons.outlined(p),
          onPressed: () => Navigator.pop(context, 'disconnect'),
          child: Text(l10n.disconnect),
        ),
      ],
      actions: [
        TextButton(
          style: AppButtons.quiet(p),
          onPressed: () => Navigator.pop(context),
          child: Text(l10n.backRemoteScreen),
        ),
      ],
    );
  },
);

/// A local, focusable shell around the existing screen/shortcut content. The
/// existing panel keeps its own scrolling and rebuilds with the parent view.
class AndroidTvLocalPanel extends StatefulWidget {
  const AndroidTvLocalPanel({
    super.key,
    required this.child,
    required this.onClose,
  });
  final Widget child;
  final VoidCallback onClose;
  @override
  State<AndroidTvLocalPanel> createState() => _AndroidTvLocalPanelState();
}

class _AndroidTvLocalPanelState extends State<AndroidTvLocalPanel> {
  final _scope = FocusScopeNode(debugLabel: 'TV local tools');
  bool _queued = false;
  @override
  void initState() {
    super.initState();
    FocusManager.instance.addListener(_reveal);
  }

  void _reveal() {
    if (_queued || !mounted || !_scope.hasFocus) return;
    _queued = true;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _queued = false;
      final context = FocusManager.instance.primaryFocus?.context;
      if (mounted && _scope.hasFocus && context != null) {
        Scrollable.ensureVisible(
          context,
          alignment: .5,
          duration: const Duration(milliseconds: 100),
        );
      }
    });
  }

  @override
  void dispose() {
    FocusManager.instance.removeListener(_reveal);
    _scope.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Positioned.fill(
    child: Stack(
      children: [
        ModalBarrier(
          color: Colors.black54,
          dismissible: true,
          onDismiss: widget.onClose,
        ),
        SafeArea(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Center(
              child: ConstrainedBox(
                constraints: const BoxConstraints(maxWidth: 520),
                child: FocusScope(
                  node: _scope,
                  child: Focus(
                    onKeyEvent: (_, event) {
                      if (event is KeyDownEvent &&
                          (event.logicalKey == LogicalKeyboardKey.escape ||
                              event.logicalKey ==
                                  LogicalKeyboardKey.contextMenu)) {
                        widget.onClose();
                        return KeyEventResult.handled;
                      }
                      return KeyEventResult.ignored;
                    },
                    child: Material(
                      color: SessionChrome.of(context).surface,
                      borderRadius: BorderRadius.circular(16),
                      clipBehavior: Clip.antiAlias,
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          Align(
                            alignment: Alignment.centerRight,
                            child: TextButton.icon(
                              key: const Key('android-tv-tools-close'),
                              autofocus: true,
                              onPressed: widget.onClose,
                              icon: const Icon(Icons.close_rounded),
                              label: Text(l10n.backRemoteScreen),
                            ),
                          ),
                          Flexible(
                            child: FocusTraversalGroup(child: widget.child),
                          ),
                        ],
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      ],
    ),
  );
}

/// The TV's 단축키 panel (its remote has no key row; the phone's session
/// menu has no 키 조합 since 2026-11-03): the entries and callbacks the
/// remote view gives, including disabled commands, protocol limits and the
/// existing secure-attention boundary.
Widget androidTvShortcuts(List<SessionMenuItem> items, VoidCallback close) =>
    AndroidToolbarPanel(
      title: l10n.shortcuts,
      children: [
        for (final (index, item) in items.indexed)
          if (item.heading || item.onSelected == null)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 8),
              child: Text(item.text),
            )
          else
            TextButton.icon(
              key: Key('android-tv-shortcut-$index'),
              onPressed: () {
                close();
                item.onSelected!();
              },
              icon: Icon(item.icon ?? Icons.keyboard_rounded),
              label: Text(item.text),
            ),
      ],
    );

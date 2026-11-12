import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../ui/app_toast.dart';
import 'android_application.dart';
import '../l10n/l10n.dart';

/// Device capability, separate from screen size: a tablet is not a television.
class AndroidTvScope extends InheritedWidget {
  const AndroidTvScope({
    super.key,
    required this.television,
    this.hardwareKeyboard = false,
    required super.child,
  });
  final bool television;
  final bool hardwareKeyboard;
  static bool hasHardwareKeyboard(BuildContext context) =>
      context
          .dependOnInheritedWidgetOfExactType<AndroidTvScope>()
          ?.hardwareKeyboard ??
      false;
  static bool of(BuildContext context) =>
      context
          .dependOnInheritedWidgetOfExactType<AndroidTvScope>()
          ?.television ??
      false;
  @override
  bool updateShouldNotify(AndroidTvScope oldWidget) =>
      television != oldWidget.television ||
      hardwareKeyboard != oldWidget.hardwareKeyboard;
}

/// Paint focus on the control itself. There is no window-sized focus overlay.
class AndroidTvFocus extends StatefulWidget {
  const AndroidTvFocus({super.key, required this.builder});
  final Widget Function(BuildContext context, bool focused) builder;

  static Color surface(BuildContext context) =>
      Theme.of(context).brightness == Brightness.dark
      ? const Color(0xFFF0F3F6)
      : const Color(0xFF243343);
  static Color ink(BuildContext context) =>
      Theme.of(context).brightness == Brightness.dark
      ? const Color(0xFF15212B)
      : Colors.white;

  static ButtonStyle style(BuildContext context, ButtonStyle base) {
    if (!AndroidTvScope.of(context)) return base;
    bool active(Set<WidgetState> states) =>
        states.contains(WidgetState.focused) &&
        !states.contains(WidgetState.disabled);
    WidgetStateProperty<T?> paint<T>(
      WidgetStateProperty<T?>? fallback,
      T value,
    ) => WidgetStateProperty.resolveWith(
      (states) => active(states) ? value : fallback?.resolve(states),
    );
    return base.copyWith(
      backgroundColor: paint(base.backgroundColor, surface(context)),
      foregroundColor: paint(base.foregroundColor, ink(context)),
      iconColor: paint(base.iconColor, ink(context)),
      elevation: paint(base.elevation, 4),
      shadowColor: paint(base.shadowColor, Colors.black.withValues(alpha: .35)),
      side: paint(base.side, BorderSide.none),
      overlayColor: WidgetStateProperty.resolveWith(
        (states) => active(states)
            ? states.contains(WidgetState.pressed)
                  ? ink(context).withValues(alpha: .12)
                  : Colors.transparent
            : base.overlayColor?.resolve(states),
      ),
      animationDuration: const Duration(milliseconds: 120),
    );
  }

  @override
  State<AndroidTvFocus> createState() => _AndroidTvFocusState();
}

class _AndroidTvFocusState extends State<AndroidTvFocus> {
  bool _focused = false;
  int _focusRevision = 0;

  void _focusChanged(bool focused) {
    if (!mounted || focused == _focused) return;
    setState(() => _focused = focused);
    final revision = ++_focusRevision;
    if (!focused) return;
    final node = FocusManager.instance.primaryFocus;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted ||
          !_focused ||
          revision != _focusRevision ||
          FocusManager.instance.primaryFocus != node ||
          !AndroidTvScope.of(context) ||
          ModalRoute.of(context)?.isCurrent == false) {
        return;
      }
      final object = context.findRenderObject();
      if (object == null || !object.attached) return;
      // Buttons do not reveal themselves like EditableText. Keep the whole
      // focused control (including its focus scale) inside short TV views.
      // Immediate movement also leaves no old animation to outlive focus.
      unawaited(Scrollable.ensureVisible(
        context,
        alignment: .5,
        duration: Duration.zero,
      ));
    });
  }

  @override
  Widget build(BuildContext context) {
    if (!AndroidTvScope.of(context)) return widget.builder(context, false);
    return Focus(
      canRequestFocus: false,
      onFocusChange: _focusChanged,
      child: Semantics(
        selected: _focused,
        child: AnimatedScale(
          scale: _focused ? 1.025 : 1,
          duration: const Duration(milliseconds: 120),
          child: Builder(
            builder: (context) => widget.builder(context, _focused),
          ),
        ),
      ),
    );
  }
}

/// An explicit editing lease: a TV IME can be visible without resizing Flutter.
/// Navigation must stay with the editor until that keyboard has closed.
class AndroidTvImeOwner extends StatefulWidget {
  const AndroidTvImeOwner({
    super.key,
    required this.active,
    required this.child,
    this.onDismissed,
    this.ownerToken,
  });
  final bool active;
  final Widget child;
  final VoidCallback? onDismissed;
  final Object? ownerToken;

  static bool ownsFocus(FocusNode? focus) =>
      focus?.context
          ?.getInheritedWidgetOfExactType<_AndroidTvImeScope>()
          ?.active ==
      true;

  @override
  State<AndroidTvImeOwner> createState() => _AndroidTvImeOwnerState();
}

class _AndroidTvImeScope extends InheritedWidget {
  const _AndroidTvImeScope({required this.active, required super.child});
  final bool active;
  @override
  bool updateShouldNotify(_AndroidTvImeScope oldWidget) =>
      active != oldWidget.active;
}

class _AndroidTvImeOwnerState extends State<AndroidTvImeOwner> {
  bool _seenVisible = false;
  bool _seenInsets = false;
  int _revision = 0;
  int? _dismissQueued;
  @override
  void initState() {
    super.initState();
    _seenVisible = widget.active && AndroidApplication.imeVisible.value;
    _seenInsets = widget.active && AndroidApplication.imeInsetVisible.value;
    AndroidApplication.imeVisible.addListener(_visibility);
    AndroidApplication.imeInsetVisible.addListener(_visibility);
  }

  @override
  void didUpdateWidget(AndroidTvImeOwner oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.active != widget.active ||
        oldWidget.ownerToken != widget.ownerToken) {
      _revision++;
      _seenVisible = widget.active && AndroidApplication.imeVisible.value;
      _seenInsets = widget.active && AndroidApplication.imeInsetVisible.value;
    }
  }

  void _visibility() {
    if (!widget.active) {
      _seenVisible = false;
      _seenInsets = false;
      return;
    }
    final native = AndroidApplication.imeVisible.value;
    final inset = AndroidApplication.imeInsetVisible.value;
    if (native) _seenVisible = true;
    if (inset) _seenInsets = true;
    if (native || inset || (!_seenVisible && !_seenInsets)) return;
    // An initial false cannot cancel a pending keyboard show.
    _seenVisible = false;
    _seenInsets = false;
    _dismiss();
  }

  void _dismiss() {
    final revision = _revision;
    if (_dismissQueued == revision) return;
    _dismissQueued = revision;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_dismissQueued == revision) _dismissQueued = null;
      if (mounted &&
          widget.active &&
          revision == _revision &&
          !AndroidApplication.imeVisible.value &&
          !AndroidApplication.imeInsetVisible.value) {
        widget.onDismissed?.call();
      }
    });
    WidgetsBinding.instance.ensureVisualUpdate();
  }

  @override
  Widget build(BuildContext context) =>
      _AndroidTvImeScope(active: widget.active, child: widget.child);
  @override
  void dispose() {
    AndroidApplication.imeVisible.removeListener(_visibility);
    AndroidApplication.imeInsetVisible.removeListener(_visibility);
    super.dispose();
  }
}

/// A TV remote opens a real Android editor. Flutter's input connection can
/// retain app focus while still failing to steer a TV IME's own key grid.
class AndroidTvTextEntry extends StatefulWidget {
  const AndroidTvTextEntry({
    super.key,
    required this.controller,
    required this.label,
    required this.builder,
    this.enabled = true,
    this.obscureText = false,
    this.keyboardType = TextInputType.text,
    this.action = TextInputAction.done,
    this.maxLength = 8192,
    this.onChanged,
    this.onSubmitted,
  });
  final TextEditingController controller;
  final String label;
  final bool enabled, obscureText;
  final TextInputType keyboardType;
  final TextInputAction action;
  final int maxLength;
  final ValueChanged<String>? onChanged, onSubmitted;
  final Widget Function(FocusNode? focusNode, bool readOnly, VoidCallback? edit)
  builder;
  @override
  State<AndroidTvTextEntry> createState() => _AndroidTvTextEntryState();
}

class _AndroidTvTextEntryState extends State<AndroidTvTextEntry> {
  late final _focus = FocusNode(debugLabel: 'tv-text-entry', onKeyEvent: _key);
  bool _pending = false;

  Future<void> _edit() async {
    if (_pending || !widget.enabled || !AndroidTvScope.of(context)) return;
    final controller = widget.controller;
    final original = controller.value;
    final dark = Theme.of(context).brightness == Brightness.dark;
    setState(() => _pending = true);
    try {
      await SystemChannels.textInput.invokeMethod<void>('TextInput.hide');
      if (!mounted ||
          !widget.enabled ||
          widget.controller != controller ||
          controller.value != original ||
          ModalRoute.of(context)?.isCurrent == false) {
        return;
      }
      final value = await AndroidApplication.channel.invokeMethod<String>(
        'tvEditText',
        {
          'label': widget.label,
          'initial': original.text,
          'type': widget.obscureText
              ? 'password'
              : widget.keyboardType == TextInputType.url
              ? 'url'
              : 'text',
          'action': widget.action == TextInputAction.next
              ? 'next'
              : widget.action == TextInputAction.go
              ? 'go'
              : 'done',
          'maxLength': widget.maxLength,
          'dark': dark,
        },
      );
      if (!mounted ||
          !widget.enabled ||
          widget.controller != controller ||
          controller.value != original ||
          ModalRoute.of(context)?.isCurrent == false) {
        return;
      }
      if (value == null ||
          value.length > 8192 ||
          value.characters.length > widget.maxLength) {
        return;
      }
      controller.value = TextEditingValue(
        text: value,
        selection: TextSelection.collapsed(offset: value.length),
      );
      if (value != original.text) {
        widget.onChanged?.call(value);
      }
      if (widget.action == TextInputAction.next) _focus.nextFocus();
      widget.onSubmitted?.call(value);
    } on PlatformException catch (_) {
      if (mounted) {
        showAppToast(
          context,
          l10n.couldntOpenTvInputField,
          kind: AppToastKind.failed,
        );
      }
    } on MissingPluginException catch (_) {
      if (mounted) {
        showAppToast(
          context,
          l10n.updateAppUseTvInput,
          kind: AppToastKind.failed,
        );
      }
    } finally {
      if (mounted) setState(() => _pending = false);
    }
  }

  KeyEventResult _key(FocusNode _, KeyEvent event) {
    if (!AndroidTvScope.of(context) ||
        _pending ||
        (AndroidTvScope.hasHardwareKeyboard(context) &&
            AndroidApplication.hardwareNavigation) ||
        event is KeyUpEvent ||
        (event.logicalKey != LogicalKeyboardKey.select &&
            event.logicalKey != LogicalKeyboardKey.enter &&
            event.logicalKey != LogicalKeyboardKey.numpadEnter)) {
      return KeyEventResult.ignored;
    }
    if (event is KeyRepeatEvent) return KeyEventResult.handled;
    unawaited(_edit());
    return KeyEventResult.handled;
  }

  @override
  Widget build(BuildContext context) {
    final television = AndroidTvScope.of(context);
    return AndroidTvImeOwner(
      active: television && _pending,
      child: AndroidTvFocus(
        builder: (context, focused) {
          final child = widget.builder(
            television ? _focus : null,
            television && !AndroidTvScope.hasHardwareKeyboard(context),
            television && widget.enabled ? () => unawaited(_edit()) : null,
          );
          if (!television) return child;
          final theme = Theme.of(context);
          final dark = theme.brightness == Brightness.dark;
          final decoration = theme.inputDecorationTheme;
          final strongInk = dark ? Colors.white : const Color(0xFF15212B);
          return Theme(
            data: theme.copyWith(
              inputDecorationTheme: decoration.copyWith(
                fillColor: focused
                    ? dark
                          ? const Color(0xFF3B4A5C)
                          : const Color(0xFFD9E8F6)
                    : decoration.fillColor,
                focusedBorder: decoration.focusedBorder?.copyWith(
                  borderSide: BorderSide(color: strongInk, width: 2),
                ),
                prefixIconColor: focused
                    ? strongInk
                    : decoration.prefixIconColor,
                floatingLabelStyle: focused
                    ? decoration.floatingLabelStyle?.copyWith(color: strongInk)
                    : decoration.floatingLabelStyle,
              ),
            ),
            child: child,
          );
        },
      ),
    );
  }

  @override
  void dispose() {
    _focus.dispose();
    super.dispose();
  }
}

/// D-pad navigation after the TV keyboard closes. Controls paint their own
/// focus state; a separate rectangle over the window would duplicate it.
/// Remote-picture handlers and open menus get their keys first. The keyboard
/// retains character/caret navigation while it is visible.
class AndroidTvNavigation extends StatefulWidget {
  const AndroidTvNavigation({
    super.key,
    required this.television,
    this.hardwareKeyboard = false,
    required this.child,
  });
  final bool television;
  final bool hardwareKeyboard;
  final Widget child;
  @override
  State<AndroidTvNavigation> createState() => _AndroidTvNavigationState();
}

class _AndroidTvNavigationState extends State<AndroidTvNavigation>
    with WidgetsBindingObserver {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
  }

  @override
  void didUpdateWidget(AndroidTvNavigation oldWidget) {
    super.didUpdateWidget(oldWidget);
    _readImeInsets();
  }

  @override
  void didChangeMetrics() => _readImeInsets();
  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _readImeInsets();
  }

  void _readImeInsets() {
    AndroidApplication.imeInsetVisible.value =
        widget.television && View.of(context).viewInsets.bottom > 0;
  }

  KeyEventResult _key(FocusNode _, KeyEvent event) {
    final focus = FocusManager.instance.primaryFocus;
    final key = event.logicalKey;
    final imeNavigation =
        key == LogicalKeyboardKey.arrowUp ||
        key == LogicalKeyboardKey.arrowDown ||
        key == LogicalKeyboardKey.arrowLeft ||
        key == LogicalKeyboardKey.arrowRight ||
        key == LogicalKeyboardKey.select ||
        key == LogicalKeyboardKey.enter ||
        key == LogicalKeyboardKey.numpadEnter;
    final nativeIme =
        AndroidApplication.imeVisible.value &&
        focus?.context?.findAncestorStateOfType<EditableTextState>() != null;
    if (widget.television &&
        imeNavigation &&
        (AndroidTvImeOwner.ownsFocus(focus) || nativeIme)) {
      // Skip Flutter traversal/activation, but report unhandled to the embedder
      // so the normal Android text/IME path remains available.
      return KeyEventResult.skipRemainingHandlers;
    }
    if (!widget.television ||
        event is KeyUpEvent ||
        HardwareKeyboard.instance.isAltPressed ||
        HardwareKeyboard.instance.isControlPressed ||
        HardwareKeyboard.instance.isMetaPressed ||
        MediaQuery.viewInsetsOf(context).bottom > 0) {
      return KeyEventResult.ignored;
    }
    final direction = switch (event.logicalKey) {
      LogicalKeyboardKey.arrowUp => TraversalDirection.up,
      LogicalKeyboardKey.arrowDown => TraversalDirection.down,
      LogicalKeyboardKey.arrowLeft => TraversalDirection.left,
      LogicalKeyboardKey.arrowRight => TraversalDirection.right,
      _ => null,
    };
    if (direction == null) return KeyEventResult.ignored;
    if (focus == null) return KeyEventResult.ignored;
    // Real keyboard caret/selection shortcuts keep Flutter's text editing
    // actions. A TV remote still traverses fields, even with a keyboard attached.
    if (AndroidApplication.hardwareNavigation &&
        focus.context?.findAncestorStateOfType<EditableTextState>() != null) {
      return KeyEventResult.ignored;
    }
    if (focus is FocusScopeNode) {
      focus.nextFocus();
    } else {
      focus.focusInDirection(direction);
    }
    return KeyEventResult.handled;
  }

  @override
  Widget build(BuildContext context) {
    if (!widget.television) return widget.child;
    return AndroidTvScope(
      television: true,
      hardwareKeyboard: widget.hardwareKeyboard,
      child: Focus(
        canRequestFocus: false,
        onKeyEvent: _key,
        child: Theme(
          data: Theme.of(context).copyWith(
            focusColor: Theme.of(context).brightness == Brightness.dark
                ? Colors.white.withValues(alpha: .22)
                : Colors.black.withValues(alpha: .12),
          ),
          child: widget.child,
        ),
      ),
    );
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }
}

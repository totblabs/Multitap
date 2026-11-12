// Selection composition follows Flutter's public SelectableText implementation.
// Copyright 2014 The Flutter Authors. BSD-style license, Flutter LICENSE.
// This plain-ASCII MFA widget keeps public gestures/semantics and gates the
// common virtual copySelection method; it does not modify the framework.
import 'package:flutter/cupertino.dart';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

/// A selectable MFA seed/code leaf. [binding] is the exact enrollment/receipt.
/// A binding or payload change retires the old editable lifetime before rebuild.
class MfaSensitiveText extends StatefulWidget {
  const MfaSensitiveText(this.data, {super.key, required this.binding,
    required this.canCopy, this.style});
  final String data;
  final Object binding;
  final bool Function() canCopy;
  final TextStyle? style;
  @override
  State<MfaSensitiveText> createState() => _MfaSensitiveTextState();
}

class _CopyLifetime {
  _CopyLifetime(this.binding, this.data, this.allowed);
  final Object binding;
  final String data;
  bool Function() allowed;
  bool retired = false;
  bool admit() {
    if (retired) { return false; }
    try { return allowed() && !retired; } catch (_) { return false; }
  }
}

class _MfaEditableText extends EditableText {
  _MfaEditableText({super.key, required this.binding, required this.lifetime,
    required super.controller, required super.focusNode, required super.style,
    required super.cursorColor, required super.selectionColor,
    required super.selectionControls, required super.showSelectionHandles,
    required super.onSelectionChanged, required super.onSelectionHandleTapped,
    required super.textAlign, required super.textWidthBasis,
    required super.textHeightBehavior, required super.maxLines,
    required super.cursorOpacityAnimates, required super.paintCursorAboveText,
    super.cursorRadius, super.cursorOffset})
    : super(readOnly: true, showCursor: false, forceLine: false,
        rendererIgnoresPointer: true, strutStyle: const StrutStyle(),
        backgroundCursorColor: CupertinoColors.inactiveGray,
        magnifierConfiguration: TextMagnifier.adaptiveMagnifierConfiguration,
        autofillHints: null, contextMenuBuilder: _menu);
  final _CopyLifetime lifetime;
  final Object binding;
  static Widget _menu(BuildContext context, EditableTextState state) =>
      AdaptiveTextSelectionToolbar.buttonItems(anchors: state.contextMenuAnchors,
        buttonItems: state.contextMenuButtonItems.where((item) =>
            item.type == ContextMenuButtonType.copy ||
            item.type == ContextMenuButtonType.selectAll).toList(growable: false));
  @override
  EditableTextState createState() => _MfaEditableTextState();
}

class _MfaEditableTextState extends EditableTextState {
  @override
  void copySelection(SelectionChangedCause cause) {
    if (!mounted) { return; }
    final own = widget as _MfaEditableText, lifetime = own.lifetime;
    final before = textEditingValue, selection = textEditingValue.selection;
    if (lifetime.retired || !identical(own.binding, lifetime.binding) ||
        before.text != lifetime.data || own.controller.text != lifetime.data ||
        !selection.isValid || selection.isCollapsed || selection.start < 0 ||
        selection.end > lifetime.data.length || !lifetime.admit()) { return; }
    if (!mounted || !identical(widget, own) || !identical(own.lifetime, lifetime) ||
        lifetime.retired || !identical(own.binding, lifetime.binding) ||
        textEditingValue != before || own.controller.text != lifetime.data) { return; }
    // No await or queued callback between exact-owner admission and dispatch.
    // The existing implementation may finish its one admitted platform Future.
    super.copySelection(cause);
  }
}

class _MfaSensitiveTextState extends State<MfaSensitiveText>
    implements TextSelectionGestureDetectorBuilderDelegate {
  late TextEditingController _controller;
  late FocusNode _focus;
  late _CopyLifetime _lifetime;
  late GlobalKey<EditableTextState> _editableKey;
  late final _gestures = TextSelectionGestureDetectorBuilder(delegate: this);
  bool _showHandles = false;
  @override
  bool forcePressEnabled = false;
  @override
  bool get selectionEnabled => true;
  @override
  GlobalKey<EditableTextState> get editableTextKey => _editableKey;
  void _createLifetime() {
    _lifetime = _CopyLifetime(widget.binding, widget.data, widget.canCopy);
    _controller = TextEditingController(text: widget.data)..addListener(_changed);
    _focus = FocusNode(skipTraversal: true)..addListener(_focusChanged);
    _editableKey = GlobalKey<EditableTextState>();
    _showHandles = false;
  }
  void _retireLifetime() {
    _lifetime.retired = true;
    _controller.removeListener(_changed); _focus.removeListener(_focusChanged);
    _controller.dispose(); _focus.dispose();
  }
  @override
  void initState() { super.initState(); _createLifetime(); }
  @override
  void didUpdateWidget(covariant MfaSensitiveText oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!identical(oldWidget.binding, widget.binding) || oldWidget.data != widget.data) {
      _retireLifetime(); _createLifetime();
    } else {
      _lifetime.allowed = widget.canCopy;
    }
  }
  @override
  void dispose() { _retireLifetime(); super.dispose(); }
  void _changed() {
    final show = !_focus.hasFocus || !_controller.selection.isCollapsed;
    if (show != _showHandles && mounted) { setState(() => _showHandles = show); }
  }
  void _focusChanged() {
    if (!_focus.hasFocus && SchedulerBinding.instance.lifecycleState == AppLifecycleState.resumed) {
      _controller.value = TextEditingValue(text: _controller.value.text);
    }
  }
  void _selectionChanged(TextSelection selection, SelectionChangedCause? cause) {
    final show = _gestures.shouldShowSelectionToolbar && !selection.isCollapsed &&
        cause != SelectionChangedCause.keyboard &&
        (cause == SelectionChangedCause.longPress || _controller.text.isNotEmpty);
    if (show != _showHandles) { setState(() => _showHandles = show); }
    final platform = Theme.of(context).platform;
    if ((platform == TargetPlatform.iOS || platform == TargetPlatform.macOS) &&
        cause == SelectionChangedCause.longPress) {
      _editableKey.currentState?.bringIntoView(selection.base);
    }
  }
  void _handleTapped() {
    if (_controller.selection.isCollapsed) { _editableKey.currentState?.toggleToolbar(); }
  }
  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context), defaults = DefaultTextStyle.of(context);
    final selectionStyle = DefaultSelectionStyle.of(context);
    final apple = theme.platform == TargetPlatform.iOS || theme.platform == TargetPlatform.macOS;
    final desktop = theme.platform == TargetPlatform.windows || theme.platform == TargetPlatform.linux;
    forcePressEnabled = theme.platform == TargetPlatform.iOS;
    final primary = apple ? CupertinoTheme.of(context).primaryColor : theme.colorScheme.primary;
    final TextSelectionControls controls = switch (theme.platform) {
      TargetPlatform.iOS => cupertinoTextSelectionHandleControls,
      TargetPlatform.macOS => cupertinoDesktopTextSelectionHandleControls,
      _ => desktop ? desktopTextSelectionHandleControls : materialTextSelectionHandleControls,
    };
    final style = widget.style == null || widget.style!.inherit
        ? defaults.style.merge(widget.style) : widget.style!;
    final child = RepaintBoundary(child: _MfaEditableText(
      key: _editableKey, binding: widget.binding, lifetime: _lifetime, controller: _controller, focusNode: _focus,
      style: style, cursorColor: selectionStyle.cursorColor ?? primary,
      selectionColor: selectionStyle.selectionColor ?? primary.withAlpha(102),
      selectionControls: controls, showSelectionHandles: _showHandles,
      onSelectionChanged: _selectionChanged, onSelectionHandleTapped: _handleTapped,
      textAlign: defaults.textAlign ?? TextAlign.start, textWidthBasis: defaults.textWidthBasis,
      textHeightBehavior: defaults.textHeightBehavior, maxLines: defaults.maxLines,
      cursorOpacityAnimates: apple, paintCursorAboveText: apple,
      cursorRadius: apple ? const Radius.circular(2) : null,
      cursorOffset: apple ? Offset(-2 / MediaQuery.devicePixelRatioOf(context), 0) : null,
    ));
    return Semantics(onLongPress: _focus.requestFocus,
      child: _gestures.buildGestureDetector(behavior: HitTestBehavior.translucent, child: child));
  }
}

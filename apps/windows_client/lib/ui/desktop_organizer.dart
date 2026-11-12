import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../models/desktop_machine.dart';
import '../models/desktop_organize.dart';
import 'app_menu.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// The height of the launcher's search, status filter and sort button: 34
/// (50 for touch), growing with enlarged text so the labels never clip.
double organizerHeight(BuildContext context, {bool touch = false}) {
  final font = MediaQuery.textScalerOf(context).scale(touch ? 15 : 13);
  // By finger: 48 dp targets inside the status pill's 1 dp edge.
  return math.max(touch ? 50.0 : 34.0, font * 1.4 + 10);
}

/// The launcher's search box: ONE pill (a hairline edge, the amber focus ring
/// on that pill only; the text field inside draws no border, fill or outline
/// of its own). A magnifier, the text, then a clear button while there is text
/// (or the "Ctrl F" keycap while the box is empty and idle).
///
/// With a [width] it grows out of the magnifier button ([open] false is just
/// that button, which calls [onOpen]); without one it fills the space it is
/// given. Esc clears the text, a second Esc leaves the box ([onLeave]).
class DesktopSearchField extends StatelessWidget {
  const DesktopSearchField({
    super.key,
    required this.controller,
    required this.focusNode,
    required this.onSubmitted,
    required this.onLeave,
    this.onDown,
    this.onOpen,
    this.open = true,
    this.width,
    this.height,
    this.touch = false,
    this.compactChrome = false,
    this.closable = false,
    String? openTooltip,
  }) : _openTooltip = openTooltip; // ignore: prefer_initializing_formals

  final TextEditingController controller;
  final FocusNode focusNode;
  final ValueChanged<String> onSubmitted;

  /// The second Esc (the text was already empty), or the close button.
  final VoidCallback onLeave;

  /// Arrow down: on to the first result.
  final VoidCallback? onDown;

  /// The collapsed magnifier was pressed.
  final VoidCallback? onOpen;
  final bool open;

  /// The expanded width; null fills the parent (no collapsed form).
  final double? width;

  /// Null: [organizerHeight].
  final double? height;
  final bool touch;
  final bool compactChrome;

  /// An empty box shows a close button too (where the box takes its own row).
  final bool closable;
  final String? _openTooltip;
  String get openTooltip => _openTooltip ?? l10n.searchCtrlF;

  static const _grow = Duration(milliseconds: 200);

  @override
  Widget build(BuildContext context) {
    final expanded = width;
    final size = height ?? organizerHeight(context, touch: touch);
    if (expanded == null) return _field(context, null, size);
    return TweenAnimationBuilder<double>(
      tween: Tween<double>(end: open ? expanded : size),
      duration: _grow,
      curve: Curves.easeOutCubic,
      builder: (context, current, _) => open || current > size + .5
          ? _field(context, current, size)
          : _magnifier(context, size),
    );
  }

  Widget _magnifier(BuildContext context, double height) {
    final p = AppPalette.of(context);
    return IconButton(
      key: const Key('desktop-search-open'),
      tooltip: openTooltip,
      onPressed: onOpen,
      style: IconButton.styleFrom(
        foregroundColor: p.ink,
        backgroundColor: p.pillGlass,
        minimumSize: Size(height, height),
        maximumSize: Size(height, height),
        padding: EdgeInsets.zero,
        tapTargetSize: MaterialTapTargetSize.shrinkWrap,
        shape: compactChrome
            ? RoundedRectangleBorder(borderRadius: BorderRadius.circular(10))
            : CircleBorder(side: BorderSide(color: p.glassEdge)),
      ),
      icon: Icon(Icons.search_rounded, size: touch ? 20 : 18),
    );
  }

  Widget _field(BuildContext context, double? current, double height) {
    final p = AppPalette.of(context);
    final fontSize = touch ? 15.0 : 13.0;
    final content = ListenableBuilder(
      listenable: Listenable.merge([focusNode, controller]),
      builder: (context, _) {
        final focused = focusNode.hasFocus;
        final hasText = controller.text.isNotEmpty;
        Widget trailing;
        if (hasText) {
          trailing = _RoundAction(
            key: const Key('desktop-search-clear'),
            tooltip: l10n.clear,
            size: touch ? 34 : 24,
            color: p.muted,
            hover: p.hover,
            onPressed: () {
              controller.clear();
              focusNode.requestFocus();
            },
          );
        } else if (closable) {
          trailing = _RoundAction(
            key: const Key('desktop-search-close'),
            tooltip: l10n.closeSearch,
            size: touch ? 34 : 24,
            color: p.muted,
            hover: p.hover,
            onPressed: onLeave,
          );
        } else if (!touch && !focused) {
          trailing = Container(
            key: const Key('desktop-search-hint'),
            padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
            decoration: BoxDecoration(
              color: p.hover,
              borderRadius: BorderRadius.circular(5),
            ),
            child: Text(
              'Ctrl F',
              maxLines: 1,
              style: uiText(
                size: 12,
                weight: FontWeight.w600,
                color: p.muted,
              ),
            ),
          );
        } else {
          trailing = const SizedBox.shrink();
        }
        final row = Padding(
          padding: EdgeInsets.only(
            left: touch ? 14 : 11,
            right: hasText || closable || (!touch && !focused) ? 5 : 12,
          ),
          child: Row(
            children: [
              Icon(
                Icons.search_rounded,
                size: touch ? 20 : 17,
                color: focused ? p.amberInk : p.muted,
              ),
              SizedBox(width: touch ? 9 : 7),
              Expanded(
                child: TextField(
                  key: const Key('desktop-search-input'),
                  controller: controller,
                  focusNode: focusNode,
                  textInputAction: TextInputAction.search,
                  onSubmitted: onSubmitted,
                  cursorColor: p.amber,
                  cursorHeight: touch ? 18 : 16,
                  maxLength: 64,
                  maxLengthEnforcement: MaxLengthEnforcement.enforced,
                  style: uiText(size: fontSize, color: p.ink),
                  decoration: InputDecoration(
                    // Everything the app-wide field theme would draw (the
                    // filled well and the outline) is switched off: the pill
                    // around it is the only container.
                    isCollapsed: true,
                    isDense: true,
                    filled: false,
                    contentPadding: EdgeInsets.zero,
                    border: InputBorder.none,
                    enabledBorder: InputBorder.none,
                    focusedBorder: InputBorder.none,
                    disabledBorder: InputBorder.none,
                    errorBorder: InputBorder.none,
                    focusedErrorBorder: InputBorder.none,
                    counterText: '',
                    hintText: touch ? l10n.searchNameNumberGroup : l10n.search,
                    hintStyle: uiText(size: fontSize, color: p.muted),
                  ),
                ),
              ),
              trailing,
            ],
          ),
        );
        final inner = current == null
            ? row
            : OverflowBox(
                alignment: Alignment.centerLeft,
                minWidth: width! - 2,
                maxWidth: width! - 2,
                child: row,
              );
        return MouseRegion(
          cursor: SystemMouseCursors.text,
          child: GestureDetector(
            behavior: HitTestBehavior.translucent,
            onTap: focusNode.requestFocus,
            child: Container(
              key: const Key('desktop-search'),
              width: current,
              height: height,
              decoration: BoxDecoration(
                color: p.pillGlass,
                borderRadius: BorderRadius.circular(compactChrome ? 10 : 999),
                border: Border.all(color: focused ? p.amberLine : p.glassEdge),
                boxShadow: [
                  if (focused) BoxShadow(color: p.amberSoft, spreadRadius: 2.5),
                ],
              ),
              child: ClipRRect(
                borderRadius: BorderRadius.circular(compactChrome ? 10 : 999),
                child: inner,
              ),
            ),
          ),
        );
      },
    );
    return Focus(
      canRequestFocus: false,
      skipTraversal: true,
      onKeyEvent: (node, event) {
        if (onDown != null &&
            event is KeyDownEvent &&
            event.logicalKey == LogicalKeyboardKey.arrowDown) {
          onDown!();
          return KeyEventResult.handled;
        }
        if (event is KeyDownEvent &&
            event.logicalKey == LogicalKeyboardKey.escape) {
          if (controller.text.isNotEmpty) {
            controller.clear();
          } else {
            onLeave();
          }
          return KeyEventResult.handled;
        }
        return KeyEventResult.ignored;
      },
      child: content,
    );
  }
}

/// A small round × button inside the search pill: hand cursor, a hover
/// wash and a tooltip, and no ink (the pill's translucent fill sits over it).
class _RoundAction extends StatefulWidget {
  const _RoundAction({
    super.key,
    required this.tooltip,
    required this.size,
    required this.color,
    required this.hover,
    required this.onPressed,
  });
  final String tooltip;
  final double size;
  final Color color, hover;
  final VoidCallback onPressed;

  @override
  State<_RoundAction> createState() => _RoundActionState();
}

class _RoundActionState extends State<_RoundAction> {
  bool _hovered = false;

  @override
  Widget build(BuildContext context) {
    return Tooltip(
      message: widget.tooltip,
      child: Semantics(
        button: true,
        label: widget.tooltip,
        excludeSemantics: true,
        onTap: widget.onPressed,
        child: MouseRegion(
          cursor: SystemMouseCursors.click,
          onEnter: (_) => setState(() => _hovered = true),
          onExit: (_) => setState(() => _hovered = false),
          child: GestureDetector(
            behavior: HitTestBehavior.opaque,
            onTap: widget.onPressed,
            child: Container(
              width: widget.size,
              height: widget.size,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                color: _hovered ? widget.hover : Colors.transparent,
              ),
              child: Icon(
                Icons.close_rounded,
                size: widget.size * .62,
                color: widget.color,
              ),
            ),
          ),
        ),
      ),
    );
  }
}

/// The 상태 filter: one segmented pill 전체 · 실행 중 · 꺼짐, each with its
/// live count. Single selection (amber segment), left/right arrows move it,
/// and a segment without members stays visible but disabled, saying why.
class DesktopStatusFilter extends StatelessWidget {
  const DesktopStatusFilter({
    super.key,
    required this.selected,
    required this.counts,
    required this.onSelected,
    this.onUnavailable,
    this.height,
    this.touch = false,
    this.compactChrome = false,
  });

  final DesktopStatus selected;
  final DesktopStatusCounts counts;
  final ValueChanged<DesktopStatus> onSelected;

  /// Touch surfaces may show a notice instead of a tooltip bubble when a
  /// segment has no members. It remains unavailable for selection.
  final ValueChanged<DesktopStatus>? onUnavailable;

  /// Null: [organizerHeight].
  final double? height;
  final bool touch;
  final bool compactChrome;

  @override
  Widget build(BuildContext context) => _SegmentedFilter<DesktopStatus>(
    key: const Key('desktop-status'),
    name: 'status',
    semanticLabel: l10n.statusFilter,
    values: DesktopStatus.values,
    selected: selected,
    idOf: (status) => status.name,
    labelOf: (status) => status.label,
    countOf: counts.of,
    actionOf: (status) => status.action,
    emptyReasonOf: (status) => status.emptyReason,
    onSelected: onSelected,
    onUnavailable: onUnavailable,
    height: height,
    touch: touch,
    compactChrome: compactChrome,
  );
}

/// The 구분 filter (2026-11-03): 전체 · VM · 로컬 PC with live counts, in the
/// 상태 filter's segmented pill and behaviour. The launcher shows it only
/// while both kinds are assigned.
class DesktopKindFilter extends StatelessWidget {
  const DesktopKindFilter({
    super.key,
    required this.selected,
    required this.machines,
    required this.onSelected,
    this.onUnavailable,
    this.height,
    this.touch = false,
    this.compactChrome = false,
  });

  final DesktopKind selected;

  /// The assigned list the counts describe.
  final List<DesktopMachine> machines;
  final ValueChanged<DesktopKind> onSelected;
  final ValueChanged<DesktopKind>? onUnavailable;
  final double? height;
  final bool touch;
  final bool compactChrome;

  @override
  Widget build(BuildContext context) => _SegmentedFilter<DesktopKind>(
    key: const Key('desktop-kind'),
    name: 'kind',
    semanticLabel: l10n.typeFilter,
    values: DesktopKind.values,
    selected: selected,
    idOf: (kind) => kind.name,
    labelOf: (kind) => kind.label,
    countOf: (kind) => kind.count(machines),
    actionOf: (kind) => kind.action,
    emptyReasonOf: (kind) => kind.emptyReason,
    onSelected: onSelected,
    onUnavailable: onUnavailable,
    height: height,
    touch: touch,
    compactChrome: compactChrome,
  );
}

/// One segmented single-choice pill; its first value stands for "all" and is
/// always available. Keys: `[name]-[id]` per segment, `[name]-count-[id]` per
/// count.
class _SegmentedFilter<T> extends StatefulWidget {
  const _SegmentedFilter({
    super.key,
    required this.name,
    required this.semanticLabel,
    required this.values,
    required this.selected,
    required this.idOf,
    required this.labelOf,
    required this.countOf,
    required this.actionOf,
    required this.emptyReasonOf,
    required this.onSelected,
    this.onUnavailable,
    this.height,
    this.touch = false,
    this.compactChrome = false,
  });

  final String name, semanticLabel;
  final List<T> values;
  final T selected;
  final String Function(T) idOf, labelOf, actionOf, emptyReasonOf;
  final int Function(T) countOf;
  final ValueChanged<T> onSelected;
  final ValueChanged<T>? onUnavailable;
  final double? height;
  final bool touch;
  final bool compactChrome;

  @override
  State<_SegmentedFilter<T>> createState() => _SegmentedFilterState<T>();
}

class _SegmentedFilterState<T> extends State<_SegmentedFilter<T>> {
  late final Map<T, FocusNode> _nodes = {
    for (final value in widget.values)
      value: FocusNode(
        debugLabel: '${widget.name}-${widget.idOf(value)}',
        onKeyEvent: (_, event) => _key(value, event),
      ),
  };

  @override
  void dispose() {
    for (final node in _nodes.values) {
      node.dispose();
    }
    super.dispose();
  }

  bool _enabled(T value) =>
      value == widget.values.first ||
      value == widget.selected ||
      widget.countOf(value) > 0;

  KeyEventResult _key(T from, KeyEvent event) {
    if (event is! KeyDownEvent && event is! KeyRepeatEvent) {
      return KeyEventResult.ignored;
    }
    final order = widget.values;
    final here = order.indexOf(from);
    final key = event.logicalKey;
    int step;
    if (key == LogicalKeyboardKey.arrowRight) {
      step = 1;
    } else if (key == LogicalKeyboardKey.arrowLeft) {
      step = -1;
    } else if (key == LogicalKeyboardKey.home) {
      step = -order.length;
    } else if (key == LogicalKeyboardKey.end) {
      step = order.length;
    } else {
      return KeyEventResult.ignored;
    }
    // The nearest segment that can be chosen, in that direction (an end key
    // goes to the far end).
    final direction = step.sign;
    var index = step.abs() == 1 ? here : (direction > 0 ? -1 : order.length);
    T? target;
    while (true) {
      index += direction;
      if (index < 0 || index >= order.length) break;
      if (_enabled(order[index])) {
        target = order[index];
        if (step.abs() == 1) break;
      }
    }
    if (target != null && target != from) {
      widget.onSelected(target);
      _nodes[target]!.requestFocus();
    }
    return KeyEventResult.handled;
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final height =
        widget.height ?? organizerHeight(context, touch: widget.touch);
    return Semantics(
      container: true,
      label: widget.semanticLabel,
      child: Container(
        height: height,
        // By finger the segments take the pill's whole height (48 dp).
        padding: EdgeInsets.symmetric(
          horizontal: 2,
          vertical: widget.touch ? 0 : 2,
        ),
        decoration: BoxDecoration(
          color: p.pillGlass,
          borderRadius: BorderRadius.circular(widget.compactChrome ? 10 : 999),
          border: Border.all(color: p.glassEdge),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            for (final value in widget.values) _segment(p, value, height),
          ],
        ),
      ),
    );
  }

  Widget _segment(AppPalette p, T value, double height) {
    final count = widget.countOf(value);
    final enabled = _enabled(value);
    final selected = value == widget.selected;
    final fore = selected
        ? (widget.compactChrome ? p.amberInk : p.onAmber)
        : (enabled ? p.ink : p.faint);
    final node = _nodes[value]!;
    final id = widget.idOf(value), label = widget.labelOf(value);
    final emptyReason = widget.emptyReasonOf(value);
    final tooltip = enabled ? widget.actionOf(value) : emptyReason;
    return Semantics(
      button: true,
      enabled: enabled,
      selected: selected,
      inMutuallyExclusiveGroup: true,
      label: l10n.text16(label, count),
      hint: enabled ? null : emptyReason,
      excludeSemantics: true,
      onTap: enabled ? () => widget.onSelected(value) : null,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: !enabled && widget.onUnavailable != null
            ? () => widget.onUnavailable!(value)
            : null,
        child: Tooltip(
          message: tooltip,
          waitDuration: const Duration(milliseconds: 500),
          // A disabled segment explains itself on a tap too (touch has no hover).
          triggerMode: !enabled && widget.onUnavailable != null
              ? TooltipTriggerMode.manual
              : enabled
              ? TooltipTriggerMode.longPress
              : TooltipTriggerMode.tap,
          child: MouseRegion(
            cursor: enabled
                ? SystemMouseCursors.click
                : SystemMouseCursors.basic,
            child: ListenableBuilder(
              listenable: node,
              builder: (context, child) {
                final ring =
                    node.hasFocus &&
                    FocusManager.instance.highlightMode ==
                        FocusHighlightMode.traditional;
                return DecoratedBox(
                  position: DecorationPosition.foreground,
                  decoration: ShapeDecoration(
                    shape: widget.compactChrome
                      ? RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(10),
                          side: BorderSide(color: ring ? p.ink : Colors.transparent, width: 1.5),
                        )
                      : StadiumBorder(
                      side: BorderSide(
                        color: ring ? p.ink : Colors.transparent,
                        width: 1.5,
                      ),
                    ),
                  ),
                  child: child,
                );
              },
              child: Material(
                key: Key('${widget.name}-$id'),
                color: selected
                    ? (widget.compactChrome ? p.amberSoft : p.amber)
                    : Colors.transparent,
                shape: widget.compactChrome
                    ? RoundedRectangleBorder(borderRadius: BorderRadius.circular(10))
                    : const StadiumBorder(),
                clipBehavior: Clip.antiAlias,
                child: InkWell(
                  focusNode: node,
                  onTap: enabled ? () => widget.onSelected(value) : null,
                  hoverColor: selected
                      ? Colors.black.withValues(alpha: .08)
                      : p.hover,
                  child: SizedBox(
                    height: height - (widget.touch ? 2 : 6),
                    child: Padding(
                      padding: EdgeInsets.symmetric(
                        horizontal: widget.compactChrome ? 10 : (widget.touch ? 13 : 11),
                      ),
                      child: Row(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          KeepAllText(
                            label,
                            maxLines: 1,
                            style: uiText(
                              size: widget.touch ? 14 : 12.5,
                              weight: selected
                                  ? FontWeight.w600
                                  : FontWeight.w600,
                              color: fore,
                            ),
                          ),
                          SizedBox(width: widget.touch ? 6 : 5),
                          KeepAllText(
                            '$count',
                            key: Key('${widget.name}-count-$id'),
                            maxLines: 1,
                            style:
                                uiText(
                                  size: widget.touch ? 13 : 11.5,
                                  color: selected
                                      ? (widget.compactChrome ? p.amberInk : p.onAmber.withValues(alpha: .75))
                                      : (enabled ? p.muted : p.faint),
                                ).copyWith(
                                  fontFeatures: const [
                                    FontFeature.tabularFigures(),
                                  ],
                                ),
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
      ),
    );
  }
}

/// 필터 해제: appears only while a 구분 or 상태 filter or a 분류 chip is
/// active.
class DesktopFilterReset extends StatelessWidget {
  const DesktopFilterReset({
    super.key,
    required this.onPressed,
    this.height,
    this.touch = false,
    this.compactChrome = false,
  });
  final VoidCallback onPressed;
  final double? height;
  final bool touch;
  final bool compactChrome;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return Tooltip(
      message: l10n.clearAllFilters,
      child: TextButton.icon(
        key: const Key('desktop-filter-reset'),
        onPressed: onPressed,
        style: TextButton.styleFrom(
          foregroundColor: p.amberInk,
          minimumSize: Size(
            0,
            height ?? organizerHeight(context, touch: touch),
          ),
          padding: EdgeInsets.symmetric(horizontal: touch ? 12 : 10),
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          textStyle: uiText(size: touch ? 14 : 12.5, weight: FontWeight.w600),
          shape: compactChrome
              ? RoundedRectangleBorder(borderRadius: BorderRadius.circular(10))
              : const StadiumBorder(),
        ),
        icon: Icon(Icons.filter_alt_off_outlined, size: touch ? 18 : 16),
        label: KeepAllText(l10n.clearFilters, maxLines: 1),
      ),
    );
  }
}

/// A button of the launcher's title row (미리보기, 새로고침), drawn like the
/// search, the filters and the sort button around it (2026-11-03: 새로고침
/// was shorter and 미리보기 square): [organizerHeight] high, a pill with the
/// glass fill and hairline edge, a 17 px icon 6 px before a 12.5 px label.
/// Without a [label] it is a round icon button; its [tooltip] names it.
///
/// [selected] makes it a toggle: on is the amber tint and edge, off the plain
/// pill. [busy] puts a small spinner where the icon is and ignores presses
/// without fading, so the button keeps its shape while work runs.
class LauncherHeaderButton extends StatefulWidget {
  const LauncherHeaderButton({
    super.key,
    required this.icon,
    required this.tooltip,
    this.label,
    this.onPressed,
    this.selected,
    this.busy = false,
    this.height,
    this.compactChrome = false,
  });

  final IconData icon;
  final String tooltip;
  final String? label;
  final VoidCallback? onPressed;
  final bool? selected;
  final bool busy;

  /// Null: [organizerHeight].
  final double? height;
  final bool compactChrome;

  @override
  State<LauncherHeaderButton> createState() => _LauncherHeaderButtonState();
}

class _LauncherHeaderButtonState extends State<LauncherHeaderButton> {
  bool _focused = false;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final height = widget.height ?? organizerHeight(context);
    final on = widget.selected == true;
    final enabled = widget.onPressed != null && !widget.busy;
    final ink = widget.onPressed == null && !widget.busy ? p.faint : p.ink;
    final iconColor = on ? p.amberInk : ink;
    final label = widget.label;
    final ring =
        _focused &&
        FocusManager.instance.highlightMode == FocusHighlightMode.traditional;
    final edge = BorderSide(
      color: ring
          ? p.ink
          : on
          ? p.amberLine
          : p.glassEdge,
      width: ring ? 1.5 : 1,
    );
    final glyph = widget.busy
        ? SizedBox.square(
            key: const Key('header-button-busy'),
            dimension: 14,
            child: CircularProgressIndicator(strokeWidth: 2, color: p.amber),
          )
        : Icon(widget.icon, size: 17, color: iconColor);
    final content = label == null
        ? SizedBox.square(
            dimension: height,
            child: Center(child: glyph),
          )
        : SizedBox(
            height: height,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12),
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  SizedBox.square(dimension: 17, child: Center(child: glyph)),
                  const SizedBox(width: 6),
                  KeepAllText(
                    label,
                    maxLines: 1,
                    style: uiText(
                      size: 12.5,
                      weight: FontWeight.w600,
                      color: ink,
                    ),
                  ),
                ],
              ),
            ),
          );
    return Tooltip(
      message: widget.tooltip,
      waitDuration: const Duration(milliseconds: 500),
      child: Semantics(
        button: true,
        enabled: enabled,
        toggled: widget.selected,
        label: label ?? widget.tooltip,
        hint: label == null ? null : widget.tooltip,
        excludeSemantics: true,
        onTap: enabled ? widget.onPressed : null,
        child: MouseRegion(
          cursor: widget.busy
              ? SystemMouseCursors.progress
              : enabled
              ? SystemMouseCursors.click
              : SystemMouseCursors.basic,
          child: Material(
            color: on ? p.amberSoft : p.pillGlass,
            shape: widget.compactChrome
                ? RoundedRectangleBorder(borderRadius: BorderRadius.circular(10), side: edge)
                : label == null
                ? CircleBorder(side: edge)
                : StadiumBorder(side: edge),
            clipBehavior: Clip.antiAlias,
            child: InkWell(
              onTap: enabled ? widget.onPressed : null,
              onFocusChange: (value) => setState(() => _focused = value),
              hoverColor: p.hover,
              focusColor: Colors.transparent,
              child: content,
            ),
          ),
        ),
      ),
    );
  }
}

/// A row of fixed-size controls that scrolls sideways when it does not fit
/// (never wrapping, so a narrow window or enlarged text only scrolls it): the
/// mouse wheel, a drag and touch all move it, and the page's own vertical
/// scroll takes over while everything fits.
class DesktopSideScroll extends StatefulWidget {
  const DesktopSideScroll({
    super.key,
    required this.height,
    required this.child,
  });
  final double height;
  final Widget child;

  @override
  State<DesktopSideScroll> createState() => _DesktopSideScrollState();
}

class _DesktopSideScrollState extends State<DesktopSideScroll> {
  final _scroll = ScrollController();

  @override
  void dispose() {
    _scroll.dispose();
    super.dispose();
  }

  void _wheel(PointerSignalEvent event) {
    if (event is! PointerScrollEvent || !_scroll.hasClients) return;
    final position = _scroll.position;
    if (position.maxScrollExtent <= 0) return;
    GestureBinding.instance.pointerSignalResolver.register(event, (event) {
      final delta = (event as PointerScrollEvent).scrollDelta;
      final move = delta.dx != 0 ? delta.dx : delta.dy;
      _scroll.jumpTo(
        (_scroll.offset + move).clamp(0, position.maxScrollExtent),
      );
    });
  }

  /// More chips hide before / after the visible part: that edge fades, so a
  /// cut chip reads as "scroll for more" (2026-11-03: "꺼짐" was cut flat at
  /// 360 px and looked broken).
  bool _before = false, _after = false;

  bool _metrics(ScrollMetrics metrics) {
    final before = metrics.extentBefore > 1, after = metrics.extentAfter > 1;
    if (mounted && (before != _before || after != _after)) {
      setState(() {
        _before = before;
        _after = after;
      });
    }
    return false;
  }

  @override
  Widget build(BuildContext context) {
    // Always the same tree (a mask that appears would rebuild the scroll
    // view and lose its offset); without hidden chips it masks nothing.
    final row = ShaderMask(
      blendMode: BlendMode.dstIn,
      shaderCallback: (rect) {
        final fade = rect.width <= 0
            ? 0.0
            : (28 / rect.width).clamp(0.0, .3).toDouble();
        return LinearGradient(
          colors: [
            _before ? Colors.transparent : Colors.black,
            Colors.black,
            Colors.black,
            _after ? Colors.transparent : Colors.black,
          ],
          stops: [0, fade, 1 - fade, 1],
        ).createShader(rect);
      },
      child: SingleChildScrollView(
        controller: _scroll,
        scrollDirection: Axis.horizontal,
        child: widget.child,
      ),
    );
    return SizedBox(
      height: widget.height,
      child: Listener(
        onPointerSignal: _wheel,
        child: ScrollConfiguration(
          behavior: ScrollConfiguration.of(context).copyWith(
            scrollbars: false,
            dragDevices: {
              PointerDeviceKind.touch,
              PointerDeviceKind.mouse,
              PointerDeviceKind.stylus,
              PointerDeviceKind.trackpad,
            },
          ),
          child: NotificationListener<ScrollMetricsNotification>(
            onNotification: (n) => _metrics(n.metrics),
            child: NotificationListener<ScrollNotification>(
              onNotification: (n) => _metrics(n.metrics),
              child: row,
            ),
          ),
        ),
      ),
    );
  }
}

/// The 분류 chips (즐겨찾기, pools, tags) in one sideways-scrolling row. A
/// chip is a toggle: pressing the chosen one again lets go of it.
class DesktopChipRow extends StatelessWidget {
  const DesktopChipRow({
    super.key,
    required this.chips,
    required this.selected,
    required this.onSelected,
    this.touch = false,
  });

  final List<(DesktopChip, int)> chips;
  final DesktopChip selected;
  final ValueChanged<DesktopChip> onSelected;
  final bool touch;

  @override
  Widget build(BuildContext context) {
    final height = touch ? 40.0 : 34.0;
    return KeyedSubtree(
      key: const Key('desktop-chips'),
      child: DesktopSideScroll(
        height: height,
        child: Row(
          children: [
            for (final (chip, count) in chips) ...[
              _Chip(
                chip: chip,
                count: count,
                selected: chip == selected,
                height: height,
                // Pressing the chosen chip again lets go of it.
                onTap: () =>
                    onSelected(chip == selected ? DesktopChip.all : chip),
              ),
              const SizedBox(width: 8),
            ],
          ],
        ),
      ),
    );
  }
}

class _Chip extends StatelessWidget {
  const _Chip({
    required this.chip,
    required this.count,
    required this.selected,
    required this.height,
    required this.onTap,
  });
  final DesktopChip chip;
  final int count;
  final bool selected;
  final double height;
  final VoidCallback onTap;

  IconData? get _icon => switch (chip.kind) {
    DesktopChipKind.favorites => Icons.star_rounded,
    DesktopChipKind.pool => Icons.folder_outlined,
    DesktopChipKind.tag => Icons.sell_outlined,
    DesktopChipKind.all => null,
  };

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final icon = _icon;
    final fore = selected ? p.onAmber : p.ink;
    return Semantics(
      button: true,
      selected: selected,
      label: l10n.text17(chip.label, count),
      excludeSemantics: true,
      onTap: onTap,
      child: Tooltip(
        message: selected ? l10n.selectAgainClear : l10n.showOnly(chip.label),
        waitDuration: const Duration(milliseconds: 700),
        child: Material(
          key: Key('chip-${chip.id}'),
          color: selected ? p.amber : p.pillGlass,
          shape: StadiumBorder(
            side: BorderSide(color: selected ? p.amber : p.glassEdge),
          ),
          clipBehavior: Clip.antiAlias,
          child: InkWell(
            onTap: onTap,
            hoverColor: selected
                ? Colors.black.withValues(alpha: .08)
                : p.hover,
            child: ConstrainedBox(
              constraints: BoxConstraints(minHeight: height),
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 13),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    if (icon != null) ...[
                      Icon(
                        icon,
                        size: 15,
                        color: selected
                            ? p.onAmber
                            : switch (chip.kind) {
                                DesktopChipKind.favorites => p.amberInk,
                                DesktopChipKind.pool => p.blue,
                                DesktopChipKind.tag => p.purple,
                                _ => p.muted,
                              },
                      ),
                      const SizedBox(width: 6),
                    ],
                    ConstrainedBox(
                      constraints: const BoxConstraints(maxWidth: 180),
                      child: KeepAllText(
                        chip.label,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: uiText(
                          size: 12.5,
                          weight: selected ? FontWeight.w600 : FontWeight.w600,
                          color: fore,
                        ),
                      ),
                    ),
                    const SizedBox(width: 6),
                    KeepAllText(
                      '$count',
                      style: uiText(
                        size: 12,
                        color: selected
                            ? p.onAmber.withValues(alpha: .75)
                            : p.muted,
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

/// The sort button and its anchored menu (번호순 · 이름순 · 최근 사용순 ·
/// 실행 중 먼저); the current choice carries the green check.
class DesktopSortMenu extends StatefulWidget {
  const DesktopSortMenu({
    super.key,
    required this.sort,
    required this.onSelected,
    this.size,
    this.touch = false,
  });
  final DesktopSort sort;
  final ValueChanged<DesktopSort> onSelected;

  /// Null: [organizerHeight].
  final double? size;
  final bool touch;

  @override
  State<DesktopSortMenu> createState() => _DesktopSortMenuState();
}

class _DesktopSortMenuState extends State<DesktopSortMenu> {
  final _menu = MenuController();

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final size = widget.size ?? organizerHeight(context, touch: widget.touch);
    return MenuAnchor(
      controller: _menu,
      consumeOutsideTap: true,
      onOpen: () => AppMenu.opened(_menu),
      onClose: () => AppMenu.closed(_menu),
      style: AppMenu.menuStyle(p, minWidth: 168),
      menuChildren: [
        for (final sort in DesktopSort.values)
          MenuItemButton(
            key: Key('sort-${sort.name}'),
            style: AppMenu.itemStyle(p),
            onPressed: () => widget.onSelected(sort),
            trailingIcon: sort == widget.sort ? AppMenu.check(p) : null,
            child: KeepAllText(sort.label),
          ),
      ],
      builder: (context, controller, _) => IconButton(
        key: const Key('desktop-sort'),
        tooltip: l10n.sort(widget.sort.label),
        onPressed: () =>
            controller.isOpen ? controller.close() : controller.open(),
        style: IconButton.styleFrom(
          foregroundColor: p.ink,
          backgroundColor: p.pillGlass,
          minimumSize: Size(size, size),
          maximumSize: Size(size, size),
          padding: EdgeInsets.zero,
          tapTargetSize: MaterialTapTargetSize.shrinkWrap,
          shape: CircleBorder(side: BorderSide(color: p.glassEdge)),
        ),
        icon: const Icon(Icons.swap_vert_rounded, size: 18),
      ),
    );
  }
}

/// The launcher's keyboard shortcuts for the search box, shared by Windows
/// and Android (a hardware keyboard): Ctrl+F and / open it, a printable key
/// starts a search with that character, Esc clears a search left standing.
/// Only while this page is the current route, no menu is open and the focus
/// is not in a text field. Returns whether the key was used.
bool handleLauncherSearchKey(
  BuildContext context,
  KeyEvent event, {
  required void Function({String? text}) open,
  required VoidCallback close,
  required bool searching,
  required bool hasDesktops,
}) {
  if (event is! KeyDownEvent && event is! KeyRepeatEvent) return false;
  if (ModalRoute.of(context)?.isCurrent != true) return false;
  if (AppMenu.anyOpen) return false;
  final focus = FocusManager.instance.primaryFocus;
  final editing =
      focus?.context?.findAncestorWidgetOfExactType<EditableText>() != null;
  final keyboard = HardwareKeyboard.instance;
  final ctrl = keyboard.isControlPressed || keyboard.isMetaPressed;
  final key = event.logicalKey;
  if (ctrl && key == LogicalKeyboardKey.keyF && !keyboard.isAltPressed) {
    if (!hasDesktops) return false;
    open();
    return true;
  }
  if (editing) return false;
  if (key == LogicalKeyboardKey.escape && searching) {
    close();
    return true;
  }
  if (ctrl || keyboard.isAltPressed || !hasDesktops) return false;
  final character = event.character;
  if (character == '/') {
    open();
    return true;
  }
  if (character != null && character.isNotEmpty && event is KeyDownEvent) {
    final code = character.runes.first;
    if (code > 0x20 && code != 0x7f && !(code >= 0x80 && code < 0xA0)) {
      open(text: character);
      return true;
    }
  }
  return false;
}

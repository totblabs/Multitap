// Layout audit for the Android screens: wrapped or clipped control labels,
// touch targets under 48 dp and controls outside the screen or under the
// system bars. Used by the layout tests and the design captures.
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

/// Widgets whose text is a control's label: it must stay on one line.
bool _control(Widget widget) {
  if (widget is ButtonStyleButton ||
      widget is RawChip ||
      widget is PopupMenuItem ||
      widget is SegmentedButton ||
      widget is Tab ||
      widget is MenuItemButton) {
    return true;
  }
  final name = widget.runtimeType.toString();
  return name.endsWith('Button') ||
      name.endsWith('MenuRow') ||
      name.contains('Segment') ||
      name.endsWith('Chip');
}

/// A paragraph's number of lines.
int lineCount(RenderParagraph paragraph) {
  final text = paragraph.text.toPlainText();
  if (text.isEmpty) return 0;
  final boxes = paragraph.getBoxesForSelection(
    TextSelection(baseOffset: 0, extentOffset: text.length),
  );
  return boxes.map((b) => b.top.round()).toSet().length;
}

String _clean(String text) =>
    text.replaceAll('⁠', '').replaceAll('\n', '⏎').trim();

/// A problem the audit found, in one line.
class UiFinding {
  const UiFinding(this.kind, this.detail);

  /// wrap, clip, small, offscreen, inset.
  final String kind;
  final String detail;
  @override
  String toString() => '$kind: $detail';
}

/// Every control label that wraps onto more lines than one, or that is cut
/// (ellipsis or max lines), in what is on screen now. [multiline] lists
/// label texts (or parts of them) that may take two lines by design.
List<UiFinding> auditLabels(
  WidgetTester t, {
  Set<String> multiline = const {},
}) {
  final out = <UiFinding>[];
  final view = t.view.physicalSize / t.view.devicePixelRatio;
  final screen = Offset.zero & view;
  for (final element in find.byType(RichText).hitTestable().evaluate()) {
    final paragraph = element.renderObject;
    if (paragraph is! RenderParagraph ||
        !paragraph.attached ||
        !paragraph.hasSize) {
      continue;
    }
    final text = _clean(paragraph.text.toPlainText());
    if (text.isEmpty) continue;
    final rect = paragraph.localToGlobal(Offset.zero) & paragraph.size;
    if (!rect.overlaps(screen)) continue;
    Widget? control;
    element.visitAncestorElements((ancestor) {
      if (_control(ancestor.widget)) {
        control = ancestor.widget;
        return false;
      }
      return true;
    });
    final where = control == null ? '' : ' in ${control.runtimeType}';
    if (paragraph.didExceedMaxLines) {
      out.add(UiFinding('clip', '"$text"$where'));
    }
    if (control != null &&
        !text.contains('⏎') &&
        !multiline.any(text.contains) &&
        lineCount(paragraph) > 1) {
      out.add(UiFinding('wrap', '"$text"$where'));
    }
  }
  return out;
}

/// Tappable semantics nodes under 48×48 dp, partly off screen, or under the
/// system bars / cutout ([t.view.padding]).
List<UiFinding> auditTargets(WidgetTester t, {double minimum = 48}) {
  final out = <UiFinding>[];
  final ratio = t.view.devicePixelRatio;
  final view = t.view.physicalSize / ratio;
  final screen = Offset.zero & view;
  final pad = t.view.padding;
  final insets = <Rect>[
    if (pad.top > 0) Rect.fromLTWH(0, 0, view.width, pad.top / ratio),
    if (pad.bottom > 0)
      Rect.fromLTWH(
        0,
        view.height - pad.bottom / ratio,
        view.width,
        pad.bottom / ratio,
      ),
    if (pad.left > 0) Rect.fromLTWH(0, 0, pad.left / ratio, view.height),
    if (pad.right > 0)
      Rect.fromLTWH(
        view.width - pad.right / ratio,
        0,
        pad.right / ratio,
        view.height,
      ),
  ];
  void visit(SemanticsNode node) {
    node.visitChildren((child) {
      visit(child);
      return true;
    });
    if (node.isMergedIntoParent) return;
    final data = node.getSemanticsData();
    if (!data.hasAction(ui.SemanticsAction.tap) &&
        !data.hasAction(ui.SemanticsAction.longPress)) {
      return;
    }
    if (data.flagsCollection.isHidden) return;
    var rect = node.rect;
    SemanticsNode? current = node;
    var clipped = false;
    while (current != null) {
      final transform = current.transform;
      if (transform != null) rect = MatrixUtils.transformRect(transform, rect);
      // Inside a scrolling list it may be scrolled partly out of view (its
      // rect is then cut at the list's edge).
      if (current.flagsCollection.hasImplicitScrolling) {
        final list = current.getSemanticsData();
        final across =
            list.hasAction(ui.SemanticsAction.scrollLeft) ||
            list.hasAction(ui.SemanticsAction.scrollRight);
        if (_touches(rect, current.rect, across: across)) clipped = true;
      }
      // A sideways row scrolled so that a segment is only partly in view
      // (cut under the row's own inset, not at the list's edge): narrow but
      // full height is the scrolled-away part, not a small target. English
      // labels shift where the filter row rests (2026-11-11).
      final scroll = current.getSemanticsData();
      if ((scroll.hasAction(ui.SemanticsAction.scrollLeft) ||
              scroll.hasAction(ui.SemanticsAction.scrollRight)) &&
          rect.height / ratio >= minimum - .01 &&
          rect.width / ratio < minimum - .01) {
        clipped = true;
      }
      current = current.parent;
    }
    rect = Rect.fromLTRB(
      rect.left / ratio,
      rect.top / ratio,
      rect.right / ratio,
      rect.bottom / ratio,
    );
    final label = _clean(
      [
        data.label,
        data.tooltip,
        data.value,
      ].firstWhere((s) => s.isNotEmpty, orElse: () => '(no label)'),
    );
    final whole =
        rect.width >= view.width - 1 && rect.height >= view.height - 1;
    if (whole) return; // a barrier or the picture
    if (!clipped &&
        (rect.width < minimum - .01 || rect.height < minimum - .01)) {
      out.add(
        UiFinding(
          'small',
          '"$label" ${rect.width.toStringAsFixed(0)}×'
              '${rect.height.toStringAsFixed(0)}'
              ' @${rect.left.toStringAsFixed(0)},${rect.top.toStringAsFixed(0)}',
        ),
      );
    }
    if (!clipped && !_inside(rect, screen.inflate(.5))) {
      out.add(UiFinding('offscreen', '"$label" $rect'));
    }
    for (final inset in insets) {
      final overlap = rect.intersect(inset);
      if (!clipped &&
          overlap.width > 1 &&
          overlap.height > 1 &&
          // Full-width bars that only start under the inset are fine.
          math.min(overlap.width, overlap.height) > 4) {
        out.add(UiFinding('inset', '"$label" $rect'));
        break;
      }
    }
  }

  final owner = t.binding.renderViews.first.owner!.semanticsOwner;
  final root = owner?.rootSemanticsNode;
  if (root != null) visit(root);
  return out;
}

/// [child] reaches an edge of [parent] (or beyond it) along the scroll
/// axis ([across]: sideways).
bool _touches(Rect child, Rect parent, {bool across = false}) => across
    ? child.left - parent.left <= .001 || parent.right - child.right <= .001
    : child.top - parent.top <= .001 || parent.bottom - child.bottom <= .001;

bool _inside(Rect child, Rect parent) =>
    child.left >= parent.left - .5 &&
    child.top >= parent.top - .5 &&
    child.right <= parent.right + .5 &&
    child.bottom <= parent.bottom + .5;

/// Both audits (targets need [WidgetTester.ensureSemantics]).
List<UiFinding> auditScreen(
  WidgetTester t, {
  Set<String> multiline = const {},
}) => [...auditLabels(t, multiline: multiline), ...auditTargets(t)];

import 'dart:math' as math;

import 'package:flutter/foundation.dart' show ValueListenable;
import 'package:flutter/scheduler.dart' show Ticker;
import 'package:flutter/widgets.dart';

import '../ui/session_menu.dart' show SessionChrome;
import 'android_touch.dart';

/// Where a click or touch landed, and a long press filling up to its right
/// click, drawn over the remote picture: this device's own feedback, nothing
/// is sent and no pointer event is taken.
///
/// Cue points are picture points (0..1) placed with the current [view] on
/// every frame, so a mark stays on what was clicked while the picture zooms
/// or moves. Marks last 300 ms. The long-press ring shows only after a
/// third of the hold, so quick taps and swipes never show it.
class AndroidTouchFeedback extends StatefulWidget {
  const AndroidTouchFeedback({
    super.key,
    required this.cues,
    required this.view,
    this.longPress = const Duration(milliseconds: 500),
  });

  final ValueListenable<(AndroidTouchCue, Offset?, int)?> cues;
  final AndroidPictureView? Function() view;

  /// The gesture's long-press time (the ring is full when it right-clicks).
  final Duration longPress;

  @override
  State<AndroidTouchFeedback> createState() => _AndroidTouchFeedbackState();
}

class _Mark {
  const _Mark(this.kind, this.at, this.start);
  final AndroidTouchCue kind;
  final Offset at;
  final Duration start;
}

class _AndroidTouchFeedbackState extends State<AndroidTouchFeedback>
    with SingleTickerProviderStateMixin {
  static const _life = Duration(milliseconds: 300);
  late final Ticker _ticker = createTicker(_tick);
  final _frame = ValueNotifier<int>(0);
  final _marks = <_Mark>[];
  _Mark? _press;
  Duration _now = Duration.zero;

  @override
  void initState() {
    super.initState();
    widget.cues.addListener(_changed);
  }

  void _changed() {
    final cue = widget.cues.value;
    if (cue == null) return;
    final (kind, at, _) = cue;
    // A stopped ticker starts again from zero.
    if (!_ticker.isActive) _now = Duration.zero;
    switch (kind) {
      case AndroidTouchCue.press:
        _press = at == null ? null : _Mark(kind, at, _now);
      case AndroidTouchCue.pressEnd:
        _press = null;
      case AndroidTouchCue.tap ||
          AndroidTouchCue.contact ||
          AndroidTouchCue.rightClick ||
          AndroidTouchCue.hold ||
          AndroidTouchCue.dragStart ||
          AndroidTouchCue.dragEnd:
        if (kind == AndroidTouchCue.rightClick) _press = null;
        if (at != null) _marks.add(_Mark(kind, at, _now));
      default:
        return;
    }
    if (!_ticker.isActive && (_press != null || _marks.isNotEmpty)) {
      _ticker.start();
    }
    _frame.value++;
  }

  void _tick(Duration elapsed) {
    _now = elapsed;
    _marks.removeWhere((mark) => elapsed - mark.start >= _life);
    final press = _press;
    if (press != null && elapsed - press.start > widget.longPress + _life) {
      _press = null;
    }
    if (_marks.isEmpty && _press == null) _ticker.stop();
    _frame.value++;
  }

  @override
  void dispose() {
    widget.cues.removeListener(_changed);
    _ticker.dispose();
    _frame.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => IgnorePointer(
    child: RepaintBoundary(
      child: CustomPaint(
        size: Size.infinite,
        painter: _FeedbackPainter(this, SessionChrome.of(context).accent),
      ),
    ),
  );
}

class _FeedbackPainter extends CustomPainter {
  _FeedbackPainter(this.state, this.accent) : super(repaint: state._frame);
  final _AndroidTouchFeedbackState state;
  final Color accent;

  static const _right = Color(0xFF78B9F7);
  static const _white = Color(0xFFFFFFFF);

  /// A ring that stays visible on light and dark pictures (a dark edge
  /// under the coloured stroke).
  static void _ring(
    Canvas canvas,
    Offset at,
    double radius,
    Color color,
    double alpha, {
    double width = 2.4,
  }) {
    canvas.drawCircle(
      at,
      radius,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = width + 2
        ..color = const Color(0xFF000000).withValues(alpha: .3 * alpha),
    );
    canvas.drawCircle(
      at,
      radius,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = width
        ..color = color.withValues(alpha: .9 * alpha),
    );
  }

  @override
  void paint(Canvas canvas, Size size) {
    final view = state.widget.view();
    if (view == null) return;
    final now = state._now;
    for (final mark in state._marks) {
      final t =
          (now - mark.start).inMicroseconds /
          _AndroidTouchFeedbackState._life.inMicroseconds;
      if (t < 0 || t >= 1) continue;
      final at = view.point(mark.at);
      final fade = 1 - Curves.easeOut.transform(t);
      final grow = Curves.easeOut.transform(t);
      switch (mark.kind) {
        case AndroidTouchCue.rightClick:
          // A filled flash: the right click went out here.
          canvas.drawCircle(
            at,
            22 + 12 * grow,
            Paint()..color = _right.withValues(alpha: .4 * fade),
          );
          _ring(canvas, at, 22 + 12 * grow, _right, fade, width: 2.8);
        case AndroidTouchCue.hold:
          _ring(canvas, at, 26, accent, fade);
        case AndroidTouchCue.dragStart:
          _ring(canvas, at, 10 + 18 * grow, accent, fade);
        default:
          canvas.drawCircle(
            at,
            10 + 18 * grow,
            Paint()..color = _white.withValues(alpha: .16 * fade),
          );
          _ring(canvas, at, 10 + 18 * grow, _white, fade);
      }
    }
    final press = state._press;
    if (press == null) return;
    final p =
        (now - press.start).inMicroseconds /
        state.widget.longPress.inMicroseconds;
    if (p < 1 / 3) return;
    final at = view.point(press.at);
    final shown = ((p - 1 / 3) / .15).clamp(0.0, 1.0);
    _ring(canvas, at, 24, _white, .35 * shown, width: 3);
    canvas.drawArc(
      Rect.fromCircle(center: at, radius: 24),
      -math.pi / 2,
      2 * math.pi * p.clamp(0.0, 1.0),
      false,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 3.4
        ..strokeCap = StrokeCap.round
        ..color = _right.withValues(alpha: shown),
    );
  }

  @override
  bool shouldRepaint(_FeedbackPainter old) =>
      old.state != state || old.accent != accent;
}

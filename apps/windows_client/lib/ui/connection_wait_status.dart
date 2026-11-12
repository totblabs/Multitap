import 'dart:async';

import 'package:flutter/material.dart';

import '../data/connection_feedback.dart';
import 'session_menu.dart' show SessionChrome;
import '../l10n/l10n.dart';

/// The observed stages of a connection as a list, with the elapsed time on
/// the stage it is at now. No progress percentage and no activity callback:
/// it cannot renew authentication, retry, or declare a frame ready.
///
/// The rows are in the order a connection goes through them and the mark
/// only tells where it is: the rows above the stage it is at are checked,
/// the rows below are empty circles. Being at a stage means the ones before
/// it are over (a connection that receives the stream's size is connected
/// and its virtual display is set), also when a stage was passed too fast to
/// be drawn; nothing below the mark is ever checked, so no row is left empty
/// or checked out of order. A wait
/// that is not one of the four stages (다시 연결, a Host update, a desktop
/// restart) is a row above them, checked once the attempt moved on from it.
///
/// [compact] is the one line it was before: the stage and the elapsed time,
/// for a small chip over a still picture.
class ConnectionWaitStatus extends StatefulWidget {
  const ConnectionWaitStatus({
    super.key,
    required this.step,
    this.attempt,
    this.elapsed,
    this.foreground,
    this.secondary,
    this.compact = false,
    this.order = stages,
  });

  final ConnectionWaitStep step;
  final Object? attempt;

  /// An existing attempt clock may include time before this panel appeared.
  final Duration Function()? elapsed;
  final Color? foreground, secondary;
  final bool compact;

  /// The rows, in the order this connection goes through them.
  final List<ConnectionWaitStep> order;

  /// The stages of a connection in the order it goes through them, on Windows
  /// and Android alike: the request, the connection to the server, the Host
  /// setting the virtual display that was asked for, the first picture.
  static const stages = [
    ConnectionWaitStep.request,
    ConnectionWaitStep.relay,
    ConnectionWaitStep.display,
    ConnectionWaitStep.firstFrame,
  ];

  @override
  State<ConnectionWaitStatus> createState() => _ConnectionWaitStatusState();
}

class _ConnectionWaitStatusState extends State<ConnectionWaitStatus> {
  final _clock = Stopwatch();
  Timer? _tick;

  /// The waits outside the four stages this attempt was observed at, in that
  /// order: each is a row above the stages.
  final _seen = <ConnectionWaitStep>{};

  @override
  void initState() {
    super.initState();
    _clock.start();
    _seen.add(widget.step);
    _tick = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void didUpdateWidget(ConnectionWaitStatus oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.attempt != widget.attempt) {
      _clock.reset();
      _seen.clear();
    }
    _seen.add(widget.step);
  }

  @override
  void dispose() {
    _tick?.cancel();
    _clock.stop();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    final elapsed = connectionElapsed(widget.elapsed?.call() ?? _clock.elapsed);
    final ink = widget.foreground ?? chrome.ink;
    final muted = widget.secondary ?? chrome.muted;
    final time = Text(
      l10n.elapsed(elapsed),
      key: const Key('connection-wait-elapsed'),
      style: TextStyle(
        fontFamily: 'Pretendard',
        fontSize: 12,
        fontFeatures: const [FontFeature.tabularFigures()],
        color: muted,
      ),
    );
    Widget line() => Wrap(
      key: const Key('connection-wait-status'),
      alignment: WrapAlignment.center,
      crossAxisAlignment: WrapCrossAlignment.center,
      spacing: 12,
      runSpacing: 4,
      children: [
        Text(
          widget.step.label,
          key: const Key('connection-wait-step'),
          textAlign: TextAlign.center,
          style: TextStyle(
            fontFamily: 'Pretendard',
            fontSize: 14,
            fontWeight: FontWeight.w500,
            color: ink,
          ),
        ),
        time,
      ],
    );
    return Semantics(
      liveRegion: false,
      label: l10n.elapsed2(widget.step.label, elapsed),
      child: ExcludeSemantics(
        child: widget.compact
            ? line()
            : LayoutBuilder(
                // A sliver of a window (a card being measured, a window
                // being dragged shut) has no room for rows: the one line.
                builder: (context, constraints) =>
                    constraints.maxWidth <
                        160 * MediaQuery.textScalerOf(context).scale(1)
                    ? line()
                    : _list(chrome, ink, muted, time),
              ),
      ),
    );
  }

  Widget _list(SessionChrome chrome, Color ink, Color muted, Widget time) {
    final scale = MediaQuery.textScalerOf(context).scale(1);
    // A stage the caller left out of its order still gets its row.
    final order =
        widget.order.contains(widget.step) ||
            !ConnectionWaitStatus.stages.contains(widget.step)
        ? widget.order
        : ConnectionWaitStatus.stages;
    final at = order.indexOf(widget.step);
    final rows = [
      for (final step in _seen)
        if (!ConnectionWaitStatus.stages.contains(step)) step,
      ...order,
    ];
    bool done(ConnectionWaitStep step) {
      if (step == widget.step) return false;
      final index = order.indexOf(step);
      return index < 0 ? _seen.contains(step) : index < at;
    }
    return Container(
      key: const Key('connection-wait-status'),
      padding: const EdgeInsets.symmetric(horizontal: 12),
      decoration: BoxDecoration(
        color: chrome.popupHighlight,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: chrome.popupLine),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          for (final (index, step) in rows.indexed)
            DecoratedBox(
              decoration: BoxDecoration(
                border: index == 0
                    ? null
                    : Border(top: BorderSide(color: chrome.popupLine)),
              ),
              child: Padding(
                padding: const EdgeInsets.symmetric(vertical: 8),
                child: Row(
                  children: [
                    SizedBox.square(
                      dimension: 16 * scale,
                      child: Center(
                        child: step == widget.step
                            ? SizedBox.square(
                                key: const Key('connection-wait-now'),
                                dimension: 13 * scale,
                                child: CircularProgressIndicator(
                                  strokeWidth: 2,
                                  color: chrome.accent,
                                ),
                              )
                            : done(step)
                            ? DecoratedBox(
                                key: ValueKey(
                                  'connection-wait-done-${step.name}',
                                ),
                                decoration: BoxDecoration(
                                  color: chrome.good.withValues(alpha: .16),
                                  shape: BoxShape.circle,
                                ),
                                child: Center(
                                  child: Icon(
                                    Icons.check_rounded,
                                    size: 11 * scale,
                                    color: chrome.good,
                                  ),
                                ),
                              )
                            : DecoratedBox(
                                decoration: BoxDecoration(
                                  shape: BoxShape.circle,
                                  border: Border.all(
                                    color: chrome.faint,
                                    width: 1.5,
                                  ),
                                ),
                                child: SizedBox.square(dimension: 13 * scale),
                              ),
                      ),
                    ),
                    const SizedBox(width: 10),
                    Expanded(
                      // The time at the right of its row; under the label
                      // when large text leaves no room beside it.
                      child: Wrap(
                        alignment: WrapAlignment.spaceBetween,
                        crossAxisAlignment: WrapCrossAlignment.center,
                        spacing: 10,
                        children: [
                          Text(
                            step.label,
                            key: step == widget.step
                                ? const Key('connection-wait-step')
                                : null,
                            style: TextStyle(
                              fontFamily: 'Pretendard',
                              fontSize: 13,
                              height: 1.5,
                              fontWeight: step == widget.step
                                  ? FontWeight.w600
                                  : FontWeight.w400,
                              color: step == widget.step
                                  ? ink
                                  : done(step)
                                  ? ink.withValues(alpha: .82)
                                  : muted,
                            ),
                          ),
                          if (step == widget.step) time,
                        ],
                      ),
                    ),
                  ],
                ),
              ),
            ),
        ],
      ),
    );
  }
}

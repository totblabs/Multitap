import 'dart:async';

import 'package:flutter/material.dart';

import '../data/connection_feedback.dart' show connectionElapsed;
import '../l10n/l10n.dart';
import 'app_dialog.dart' show AppTone, AppToneColors;
import 'app_theme.dart';
import 'korean_text.dart';

/// Shared marks for work that is under way. A wait longer than about two
/// seconds always shows one of them, so no screen looks stopped.
///
/// Progress here is only ever what the code measured: [AppProgressBlock]
/// fills its bar when a real fraction is given (bytes of a known total,
/// Windows Installer's own count) and runs it back and forth otherwise.
/// Nothing in this file invents a percentage or moves a bar on a timer.

/// One row whose work is under way: a small spinner where [AppStatusLine]
/// has its dot or icon, then the sentence. Use it for the in-flight state of
/// a list row (a device being attached, a folder being connected); the
/// settled states keep [AppStatusLine].
class AppBusyLine extends StatelessWidget {
  const AppBusyLine(
    this.text, {
    super.key,
    this.textKey,
    this.size = 13,
    this.maxLines,
    this.color,
    this.spinnerColor,
    this.trailing,
  });
  final String text;

  /// The key of the sentence itself (what tests and semantics find).
  final Key? textKey;
  final double size;
  final int? maxLines;

  /// The sentence's colour; the muted text colour when null.
  final Color? color;

  /// The spinner's colour; the brand amber when null.
  final Color? spinnerColor;

  /// Shown after the sentence, e.g. an [AppElapsedText].
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final scale = MediaQuery.textScalerOf(context).scale(1);
    final mark = 12.0 * scale;
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          // Centred on the first line of the sentence.
          padding: EdgeInsets.only(top: (size * 1.6 * scale - mark) / 2),
          child: SizedBox.square(
            key: const Key('app-busy-line-spinner'),
            dimension: mark,
            child: CircularProgressIndicator(
              strokeWidth: 1.8,
              // The deep amber on a light surface (the plain amber is 2:1 there).
              color: spinnerColor ?? p.amberInk,
            ),
          ),
        ),
        const SizedBox(width: 8),
        Expanded(
          child: KeepAllText(
            text,
            key: textKey,
            maxLines: maxLines,
            overflow: maxLines == null ? null : TextOverflow.ellipsis,
            style: uiText(size: size, height: 1.6, color: color ?? p.muted),
          ),
        ),
        if (trailing != null) ...[const SizedBox(width: 8), trailing!],
      ],
    );
  }
}

/// "경과 00:42": how long a wait has run, redrawn every second. The time is
/// the caller's clock ([elapsed]) when it has one, else the time since this
/// widget appeared ([restartOn] changing starts it again).
class AppElapsedText extends StatefulWidget {
  const AppElapsedText({
    super.key,
    this.elapsed,
    this.restartOn,
    this.after = Duration.zero,
    this.style,
  });

  /// An existing clock (it may include time before this widget appeared).
  final Duration Function()? elapsed;

  /// A new value restarts the widget's own clock.
  final Object? restartOn;

  /// Nothing is drawn before this much time has passed: a wait that ends
  /// quickly never flashes a clock.
  final Duration after;
  final TextStyle? style;

  @override
  State<AppElapsedText> createState() => _AppElapsedTextState();
}

class _AppElapsedTextState extends State<AppElapsedText> {
  final _clock = Stopwatch()..start();
  Timer? _tick;

  @override
  void initState() {
    super.initState();
    _tick = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void didUpdateWidget(AppElapsedText oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.restartOn != widget.restartOn) _clock.reset();
  }

  @override
  void dispose() {
    _tick?.cancel();
    _clock.stop();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final elapsed = widget.elapsed?.call() ?? _clock.elapsed;
    if (elapsed < widget.after) return const SizedBox.shrink();
    final p = AppPalette.of(context);
    return Text(
      l10n.elapsed(connectionElapsed(elapsed)),
      key: const Key('app-elapsed'),
      style:
          widget.style ??
          uiText(size: 12, color: p.muted).copyWith(
            fontFeatures: const [FontFeature.tabularFigures()],
          ),
    );
  }
}

/// The body of a long operation: the bar, the step it is at, and how long it
/// has run.
///
/// [value] is a measured fraction (0..1) or null. With null the bar runs
/// back and forth: the work is going on and its length is not known.
/// [stepIndex] of [stepCount] are real, sequential steps known before the
/// work started (never an estimate), drawn as "2/3단계".
class AppProgressBlock extends StatelessWidget {
  const AppProgressBlock({
    super.key,
    required this.step,
    this.value,
    this.stepIndex,
    this.stepCount,
    this.detail,
    this.elapsed,
    this.restartOn,
    this.showElapsed = true,
    this.tone = AppTone.accent,
    this.barKey,
    this.stepKey,
  }) : assert(value == null || (value >= 0 && value <= 1)),
       assert((stepIndex == null) == (stepCount == null)),
       assert(stepIndex == null || stepIndex >= 1);

  /// What is being done now, one short sentence.
  final String step;
  final double? value;
  final int? stepIndex, stepCount;

  /// A measured amount beside the step ("12.4 / 30.1 MB", "42%"), or null.
  final String? detail;
  final Duration Function()? elapsed;
  final Object? restartOn;
  final bool showElapsed;
  final AppTone tone;
  final Key? barKey, stepKey;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final numbers = uiText(size: 12, color: p.muted).copyWith(
      fontFeatures: const [FontFeature.tabularFigures()],
    );
    final counted = stepIndex != null && stepCount! > 1;
    return Semantics(
      container: true,
      liveRegion: true,
      label: step,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        mainAxisSize: MainAxisSize.min,
        children: [
          LinearProgressIndicator(
            key: barKey ?? const Key('app-progress-bar'),
            value: value,
            minHeight: 6,
            borderRadius: BorderRadius.circular(3),
            color: tone == AppTone.accent ? p.amber : p.tone(tone),
          ),
          const SizedBox(height: 10),
          ExcludeSemantics(
            child: KeepAllText(
              step,
              key: stepKey ?? const Key('app-progress-step'),
              style: uiText(size: 13, height: 1.5, color: p.ink),
            ),
          ),
          if (counted || detail != null || showElapsed) ...[
            const SizedBox(height: 4),
            Wrap(
              spacing: 10,
              runSpacing: 2,
              crossAxisAlignment: WrapCrossAlignment.center,
              children: [
                if (counted)
                  Text(
                    l10n.progressStepOf(stepIndex!, stepCount!),
                    key: const Key('app-progress-count'),
                    style: numbers,
                  ),
                if (detail != null)
                  Text(
                    detail!,
                    key: const Key('app-progress-detail'),
                    style: numbers,
                  ),
                if (showElapsed)
                  AppElapsedText(
                    elapsed: elapsed,
                    restartOn: restartOn,
                    style: numbers,
                  ),
              ],
            ),
          ],
        ],
      ),
    );
  }
}

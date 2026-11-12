import 'package:flutter/material.dart';

import 'app_theme.dart';
import 'korean_text.dart';
import 'session_menu.dart' show SessionChrome;
import 'session_panels.dart';
import 'session_tools.dart';
import '../l10n/l10n.dart';

/// 연결: what the quality chip opens on the session bar (2026-11-10). The
/// measured round trip as one large number with a plain word (좋음, 보통,
/// 느림), the other measured values (never invented: '—' where nothing was
/// measured), 연결 통계 보기, and, while the latency has stayed poor, one
/// thing the user can do about it: 절약으로 다시 연결.
///
/// It follows [listenable] (the session) live; [latency], [connected],
/// [values] and [advice] are read on every change.
class SessionConnectionPanel extends StatelessWidget {
  const SessionConnectionPanel({
    super.key,
    required this.title,
    required this.mode,
    required this.latency,
    required this.connected,
    required this.values,
    required this.onStats,
    this.listenable,
    this.advice,
    this.onSaver,
    this.embedded = false,
    this.actions = const [],
  });

  /// Inside another card that has its own title, padding and scrolling (the
  /// Android menu card): no header of its own.
  final bool embedded;

  /// Rows of the platform's own before 연결 통계 보기 (Android's 화면 크기
  /// 다시 적용 after a failed resize).
  final List<Widget> actions;

  /// The desktop's name and what kind of connection this is (스트리밍).
  final String title, mode;
  final Listenable? listenable;

  /// The measured round trip in ms; null before a measurement.
  final double? Function() latency;
  final bool Function() connected;
  final List<SessionQualityValue> Function() values;
  final VoidCallback onStats;

  /// The latency has stayed poor: say what helps.
  final bool Function()? advice;

  /// Reconnects in 절약 (null: not possible now, the advice has no button).
  final VoidCallback? onSaver;

  static const width = 312.0, minHeight = 320.0;

  @override
  Widget build(BuildContext context) {
    final listenable = this.listenable;
    return listenable == null
        ? _build(context)
        : ListenableBuilder(
            listenable: listenable,
            builder: (context, _) => _build(context),
          );
  }

  Widget _build(BuildContext context) {
    final p = AppPalette.of(context);
    const chrome = SessionChrome.forDark(true);
    final up = connected();
    final sample = latency();
    final ms = up && sample != null && sample.isFinite && sample > 0
        ? sample
        : null;
    final color = ms == null ? p.muted : chrome.latencyColor(ms);
    final word = SessionChrome.latencyWord(ms);
    final measured = {for (final value in values()) value.id: value};
    // '60.0 fps' as a number and its unit; a value without a unit whole.
    Widget tile(String id, String label) {
      final value = measured[id]?.value ?? '—';
      final space = value.lastIndexOf(' ');
      final unit = id != 'resolution' && space > 0
          ? value.substring(space + 1)
          : null;
      final number = unit == null ? value : value.substring(0, space);
      return Expanded(
        flex: id == 'resolution' ? 4 : 3,
        child: Container(
          key: Key('quality-value-$id'),
          padding: const EdgeInsets.fromLTRB(10, 9, 10, 9),
          decoration: BoxDecoration(
            color: p.tileWell,
            borderRadius: BorderRadius.circular(10),
            border: Border.all(color: p.tileLine),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisSize: MainAxisSize.min,
            children: [
              // The whole label, drawn smaller where large text leaves no
              // room: never "Scre", "Rece".
              FittedBox(
                fit: BoxFit.scaleDown,
                alignment: Alignment.centerLeft,
                child: KeepAllText(
                  label,
                  maxLines: 1,
                  style: uiText(size: 12, color: p.muted),
                ),
              ),
              const SizedBox(height: 4),
              FittedBox(
                fit: BoxFit.scaleDown,
                alignment: Alignment.centerLeft,
                child: Text.rich(
                  TextSpan(
                    text: number.replaceAll(' ', ''),
                    children: [
                      if (unit != null)
                        TextSpan(
                          text: ' $unit',
                          style: uiText(
                            size: 12,
                            weight: FontWeight.w500,
                            color: p.muted,
                          ),
                        ),
                    ],
                  ),
                  maxLines: 1,
                  style: uiText(size: 14, weight: FontWeight.w600, color: p.ink)
                      .copyWith(
                        fontFeatures: const [FontFeature.tabularFigures()],
                      ),
                ),
              ),
            ],
          ),
        ),
      );
    }

    Widget line(String id) {
      final value = measured[id];
      if (value == null) return const SizedBox.shrink();
      return Padding(
        key: Key('quality-value-$id'),
        padding: const EdgeInsets.symmetric(vertical: 5),
        child: Row(
          children: [
            Expanded(
              child: KeepAllText(
                value.label,
                style: uiText(size: 12, height: 1.3, color: p.muted),
              ),
            ),
            const SizedBox(width: 12),
            KeepAllText(
              value.value,
              style: uiText(
                size: 12,
                height: 1.3,
                weight: FontWeight.w600,
                color: p.ink,
              ).copyWith(fontFeatures: const [FontFeature.tabularFigures()]),
            ),
          ],
        ),
      );
    }

    final showAdvice = advice?.call() == true;
    final body = Column(
      key: const Key('session-connection-panel'),
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        if (!embedded) ...[
          SessionPanelHeader(title, status: mode),
          const SizedBox(height: 12),
        ],
        Row(
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            Expanded(
              child: FittedBox(
                fit: BoxFit.scaleDown,
                alignment: Alignment.bottomLeft,
                child: Text.rich(
                  key: const Key('quality-latency'),
                  ms == null
                      ? TextSpan(
                          text: up ? l10n.notMeasuredYet : l10n.waiting3,
                          style: uiText(
                            size: 20,
                            weight: FontWeight.w600,
                            color: p.muted,
                          ),
                        )
                      : TextSpan(
                          text: ms < 1 ? '<1' : ms.toStringAsFixed(0),
                          children: [
                            TextSpan(
                              text: ' ms',
                              style: uiText(
                                size: 14,
                                weight: FontWeight.w600,
                                color: color,
                              ),
                            ),
                          ],
                          style:
                              uiText(
                                size: 34,
                                height: 1,
                                weight: FontWeight.w600,
                                color: color,
                              ).copyWith(
                                letterSpacing: -.5,
                                fontFeatures: const [
                                  FontFeature.tabularFigures(),
                                ],
                              ),
                        ),
                  maxLines: 1,
                ),
              ),
            ),
            const SizedBox(width: 10),
            if (word != null)
              Padding(
                padding: const EdgeInsets.only(bottom: 3),
                child: Container(
                  key: const Key('quality-word'),
                  padding: const EdgeInsets.symmetric(
                    horizontal: 10,
                    vertical: 4,
                  ),
                  decoration: BoxDecoration(
                    color: color.withValues(alpha: .13),
                    borderRadius: BorderRadius.circular(12),
                  ),
                  child: KeepAllText(
                    word,
                    style: uiText(
                      size: 12,
                      height: 1.3,
                      weight: FontWeight.w600,
                      color: color,
                    ),
                  ),
                ),
              ),
          ],
        ),
        const SizedBox(height: 6),
        KeepAllText(l10n.roundTripLatencyHostResponse, style: uiText(size: 12, color: p.muted)),
        const SizedBox(height: 14),
        IntrinsicHeight(
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              tile('fps', l10n.screen),
              const SizedBox(width: 8),
              tile('rate', l10n.received),
              const SizedBox(width: 8),
              tile('resolution', l10n.resolution),
            ],
          ),
        ),
        const SizedBox(height: 8),
        line('path'),
        line('encode'),
        line('decode'),
        if (showAdvice)
          Container(
            key: const Key('quality-advice'),
            margin: const EdgeInsets.only(top: 10),
            padding: const EdgeInsets.fromLTRB(12, 11, 12, 11),
            decoration: BoxDecoration(
              color: p.warnSoft,
              borderRadius: BorderRadius.circular(11),
              border: Border.all(color: p.warnLine),
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                KeepAllText(
                  l10n.connectionSlowLowerPictureQuality,
                  style: uiText(size: 12.5, height: 1.5, color: p.ink),
                ),
                if (onSaver != null) ...[
                  const SizedBox(height: 9),
                  FilledButton(
                    key: const Key('quality-saver'),
                    onPressed: onSaver,
                    style: FilledButton.styleFrom(
                      backgroundColor: p.amber,
                      foregroundColor: p.onAmber,
                      minimumSize: const Size(0, 34),
                      padding: const EdgeInsets.symmetric(
                        horizontal: 12,
                        vertical: 8,
                      ),
                      textStyle: uiText(size: 12.5, weight: FontWeight.w600),
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(9),
                      ),
                    ),
                    child: KeepAllText(l10n.reconnectSaver),
                  ),
                ],
              ],
            ),
          ),
        Container(
          height: 1,
          margin: const EdgeInsets.only(top: 12, bottom: 6),
          color: p.tileLine,
        ),
        ...actions,
        SessionPanelRow(
          key: const Key('quality-stats'),
          icon: Icons.insights_rounded,
          title: l10n.showConnectionStats,
          opens: true,
          onTap: onStats,
        ),
      ],
    );
    return embedded
        ? body
        : SingleChildScrollView(
            padding: const EdgeInsets.fromLTRB(14, 14, 14, 8),
            child: body,
          );
  }
}

/// The bar's first-connection hint: three short lines under the open bar,
/// once per account on this PC. 확인 folds the bar to its handle.
class SessionBarCoach extends StatelessWidget {
  const SessionBarCoach({super.key, required this.onConfirm});
  final VoidCallback onConfirm;

  static const width = 320.0;

  @override
  Widget build(BuildContext context) {
    const p = AppPalette(true);
    return Theme(
      data: buildAppTheme(brightness: Brightness.dark),
      child: Container(
        key: const Key('session-bar-coach'),
        width: width,
        padding: const EdgeInsets.fromLTRB(14, 13, 14, 12),
        decoration: BoxDecoration(
          color: const Color(0xFF20262B),
          borderRadius: BorderRadius.circular(13),
          border: Border.all(color: p.amber.withValues(alpha: .45)),
          boxShadow: const [
            BoxShadow(
              color: Color(0x99000000),
              blurRadius: 30,
              spreadRadius: -10,
              offset: Offset(0, 16),
            ),
          ],
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            KeepAllText(
              l10n.toolbarHere,
              style: uiText(
                size: 13,
                height: 1.55,
                weight: FontWeight.w600,
                color: p.amber,
              ),
            ),
            KeepAllText(
              l10n.changeScreenModeScaleSettings,
              style: uiText(size: 13, height: 1.55, color: p.ink),
            ),
            const SizedBox(height: 10),
            Align(
              alignment: Alignment.centerRight,
              child: FilledButton(
                key: const Key('session-bar-coach-ok'),
                onPressed: onConfirm,
                style: FilledButton.styleFrom(
                  backgroundColor: p.amber,
                  foregroundColor: p.onAmber,
                  minimumSize: const Size(0, 32),
                  padding: const EdgeInsets.symmetric(
                    horizontal: 14,
                    vertical: 7,
                  ),
                  textStyle: uiText(size: 12.5, weight: FontWeight.w600),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(9),
                  ),
                ),
                child: KeepAllText(l10n.ok),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

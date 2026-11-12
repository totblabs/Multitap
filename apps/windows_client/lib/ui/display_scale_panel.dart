import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../data/display_scale.dart' show DisplayScaleApplied;
import '../data/remote_session.dart';
import '../data/video_geometry.dart' show maximumGeometrySide;
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

class SessionDisplayChoice {
  const SessionDisplayChoice.scale(int value)
    : scale = value,
      width = null,
      height = null,
      automatic = false;
  const SessionDisplayChoice.resolution(
    this.width,
    this.height, {
    this.automatic = false,
  }) : scale = null;

  final int? scale, width, height;
  final bool automatic;
}

const _resolutionSteps = [75, 100, 125, 150, 200];
const _scaleSteps = [100, 125, 150, 175, 200];

/// Physical viewport aspect with the existing width4/height2 floor alignment.
/// These recommendations do not establish native decoder or Host capacity.
List<SessionDisplayChoice> recommendedDisplayResolutions(Size? viewportPixels) {
  if (viewportPixels == null ||
      !viewportPixels.width.isFinite ||
      !viewportPixels.height.isFinite ||
      viewportPixels.width > maximumGeometrySide ||
      viewportPixels.height > maximumGeometrySide ||
      viewportPixels.width <= 0 ||
      viewportPixels.height <= 0) {
    return const [];
  }
  return [
    for (final step in _resolutionSteps)
      SessionDisplayChoice.resolution(
        math.max(4, (viewportPixels.width * step / 100 / 4).floor() * 4),
        math.max(2, (viewportPixels.height * step / 100 / 2).floor() * 2),
        automatic: step == 100,
      ),
  ];
}

class DisplayScalePanel extends StatelessWidget {
  const DisplayScalePanel({
    super.key,
    required this.session,
    required this.available,
    required this.viewportPixels,
    required this.automaticResolution,
    required this.resolutionAvailable,
    required this.onSelected,
    this.showTitle = true,
    this.requiresScaleReconnect = false,
  });
  final RemoteSession session;
  // Retained call arguments do not expose a manual resolution action.
  final bool available, automaticResolution, resolutionAvailable, showTitle;
  final bool requiresScaleReconnect;
  final Size? viewportPixels;
  final ValueChanged<SessionDisplayChoice> onSelected;

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: session,
    builder: (context, _) {
      final p = AppPalette.of(context);
      final applied = session.displayScaleApplied;
      final wanted = session.displayScaleWanted;
      // Independent outputs: the largest step any monitor allows.
      final maximum = applied?.maximum ?? 0;
      // One value only when every monitor really uses it.
      final currentScale = applied?.commonScale;
      final outputs = applied?.outputs ?? const <DisplayScaleApplied>[];
      String perMonitor(int Function(DisplayScaleApplied) value) => [
        for (final o in outputs)
          l10n.monitor(o.id == '1' ? 2 : 1, value(o) > 0 ? '${value(o)}%' : l10n.unknown3),
      ].join(' · ');
      // Two monitors that differ (or one failed): each actual value.
      final split = outputs.length > 1 && currentScale == null
          ? perMonitor((o) => o.scale)
          : null;
      final automaticScale = wanted == null;
      final failure = applied == null || applied.ok
          ? null
          : switch (applied.code) {
              1 => l10n.couldNotFindRemoteScreen,
              2 => l10n.couldNotCheckWindowsScale,
              3 => l10n.windowsDidNotAllowScale,
              4 => l10n.couldNotSaveOriginalScale,
              5 => l10n.remoteScreenDoesNotSupport,
              _ => l10n.couldNotCheckResultScale,
            };
      final scaleStatus = session.closed
          ? l10n.disconnected2
          : !available
          ? l10n.youCannotChangeScaleConnection
          : automaticScale && currentScale != null
          ? l10n.autoNow(currentScale)
          : automaticScale && applied == null
          ? l10n.autoCurrentScaleNotChecked
          : failure != null
          ? (split != null ? '$failure\n$split' : failure)
          : split != null
          ? '${automaticScale ? l10n.auto : l10n.appliedStatus} · $split'
          : currentScale != null
          ? l10n.applied(currentScale, wanted != null && wanted != currentScale ? l10n.requested4(wanted) : '')
          : wanted != null
          ? l10n.requestedChecking(wanted)
          : l10n.currentScaleNotCheckedYet;
      final sourceWidth = session.sourceWidth,
          sourceHeight = session.sourceHeight;
      final resolutionStatus =
          sourceWidth != null &&
              sourceHeight != null &&
              sourceWidth > 0 &&
              sourceHeight > 0
          ? l10n.resolutionAutoNow(sourceWidth, sourceHeight)
          : l10n.resolutionAutoCurrentSizeNot;
      final rowHeight = math.max(
        36.0,
        math.min(52.0, MediaQuery.textScalerOf(context).scale(13) * 1.3 + 16),
      );
      Widget status(String text, String key) => Padding(
        padding: const EdgeInsets.only(top: 10),
        child: KeepAllText(
          text,
          key: Key(key),
          style: uiText(size: 12, height: 1.45, color: p.muted),
        ),
      );
      return FocusTraversalGroup(
        child: Column(
          key: const Key('display-scale-panel'),
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            if (showTitle) ...[
              KeepAllText(
                l10n.scale,
                style: uiText(size: 15, weight: FontWeight.w600, color: p.ink),
              ),
              const SizedBox(height: 12),
            ],
            // One row of five steps (배율 sits inside 화면, so the steps lie
            // across).
            Container(
              key: const Key('display-scale-steps'),
              padding: const EdgeInsets.all(3),
              decoration: BoxDecoration(
                color: p.tileWell,
                borderRadius: BorderRadius.circular(11),
                border: Border.all(color: p.tileLine),
              ),
              child: Row(
                children: [
                  for (final value in _scaleSteps) ...[
                    if (value != _scaleSteps.first) const SizedBox(width: 2),
                    Expanded(
                      child: SizedBox(
                        key: Key('display-scale-$value-row'),
                        height: rowHeight,
                        child: Semantics(
                          key: Key('display-scale-$value-selected'),
                          selected: currentScale == value,
                          child: TextButton(
                            key: Key('display-scale-$value'),
                            onPressed:
                                available &&
                                    !session.closed &&
                                    (maximum == 0 || value <= maximum)
                                ? () => onSelected(
                                    SessionDisplayChoice.scale(value),
                                  )
                                : null,
                            style: TextButton.styleFrom(
                              minimumSize: const Size(0, 36),
                              padding: const EdgeInsets.symmetric(
                                horizontal: 2,
                              ),
                              tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                              foregroundColor: currentScale == value
                                  ? p.amber
                                  : p.ink,
                              disabledForegroundColor: p.faint,
                              backgroundColor: currentScale == value
                                  ? p.amber.withValues(alpha: .16)
                                  : Colors.transparent,
                              shape: RoundedRectangleBorder(
                                borderRadius: BorderRadius.circular(8),
                                side: currentScale == value
                                    ? BorderSide(
                                        color: p.amber.withValues(alpha: .75),
                                      )
                                    : BorderSide.none,
                              ),
                            ),
                            child: FittedBox(
                              fit: BoxFit.scaleDown,
                              child: Text(
                                '$value%',
                                maxLines: 1,
                                style: uiText(
                                  size: 13,
                                  height: 1.3,
                                  weight: FontWeight.w600,
                                ),
                              ),
                            ),
                          ),
                        ),
                      ),
                    ),
                  ],
                ],
              ),
            ),
            status(
              automaticScale &&
                      applied != null &&
                      !applied.ok &&
                      available &&
                      !session.closed
                  ? l10n.autoCurrentScaleUnknown(scaleStatus)
                  : scaleStatus,
              'display-scale-status',
            ),
            if (outputs.length > 1 &&
                outputs.any((o) => o.maximum > 0 && o.maximum < 200) &&
                outputs.first.maximum != outputs.last.maximum)
              status(
                l10n.max(perMonitor((o) => o.maximum)),
                'display-scale-maximum',
              )
            else if (maximum > 0 && maximum < 200)
              status(l10n.upScreenSize(maximum), 'display-scale-maximum'),
            status(resolutionStatus, 'display-resolution-status'),
          ],
        ),
      );
    },
  );
}

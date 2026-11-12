import 'package:flutter/material.dart';

import '../data/session_activity.dart';
import 'app_theme.dart';
import '../l10n/l10n.dart';

String _countdown(Duration value) {
  // Round display only; the controller still owns the exact expiry instant.
  final seconds = value.isNegative ? 0 : (value.inMilliseconds + 999) ~/ 1000;
  final minutes = seconds ~/ 60;
  return '$minutes:${(seconds % 60).toString().padLeft(2, '0')}';
}

/// Displays the controller's deadline; never creates activity or a timer.
class SessionIdleBadge extends StatelessWidget {
  const SessionIdleBadge({
    super.key,
    required this.activity,
    this.compact = false,
    this.flat = false,
  });

  final SessionActivity activity;
  final bool compact;
  final bool flat;

  @override
  Widget build(BuildContext context) => AnimatedBuilder(
    animation: activity,
    builder: (context, _) {
      if (activity.reason != null) return const SizedBox.shrink();
      final p = AppPalette.of(context);
      final left = activity.remaining;
      final known = activity.idleSupported;
      final off = known && !activity.idleEnabled;
      final warning = left != null && left <= const Duration(minutes: 1);
      final text = !known || (!off && left == null)
          ? l10n.checkingAutoSignOut
          : off
          ? (compact ? l10n.off : l10n.autoSignOutOff)
          : compact
          ? _countdown(left!)
          : l10n.autoSignOut(_countdown(left!));
      final color = warning ? p.amberInk : p.muted;
      return Tooltip(
        message: off
            ? l10n.youStaySignedWithoutInput
            : l10n.signsYouOutMultitapWhen,
        child: Semantics(
          label: off ? l10n.autoSignOutOff : text,
          // A changing second must not interrupt a screen reader every second.
          liveRegion: false,
          child: ExcludeSemantics(
            child: Container(
              key: const Key('session-idle-badge'),
              padding: EdgeInsets.symmetric(
                horizontal: compact ? 4 : 9,
                vertical: compact ? 4 : 6,
              ),
              decoration: BoxDecoration(
                color: warning ? p.amberSoft : flat ? Colors.transparent : p.quiet,
                borderRadius: BorderRadius.circular(9),
                border: flat && !warning
                    ? null
                    : Border.all(color: warning ? p.amberLine : p.tileLine),
              ),
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(
                    off ? Icons.timer_off_outlined : Icons.timer_outlined,
                    size: 16,
                    color: color,
                  ),
                  SizedBox(width: compact ? 4 : 6),
                  Flexible(
                    child: Text(
                      text,
                      style:
                          uiText(
                            size: 12,
                            weight: FontWeight.w600,
                            color: color,
                          ).copyWith(
                            fontFeatures: const [FontFeature.tabularFigures()],
                          ),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ),
      );
    },
  );
}

/// Account-menu control. Only the launcher controller can offer a switch.
class SessionIdleControl extends StatelessWidget {
  const SessionIdleControl({
    super.key,
    required this.activity,
    this.touch = false,
  });

  final SessionActivity activity;
  final bool touch;

  @override
  Widget build(BuildContext context) => AnimatedBuilder(
    animation: activity,
    builder: (context, _) {
      final p = AppPalette.of(context);
      final pending = activity.idleChangePending;
      final canChange = activity.canChangeIdle;
      final error = activity.idleChangeError;
      final left = activity.remaining;
      final status = !activity.idleSupported
          ? l10n.cantCheckServerSetting
          : pending
          ? l10n.changingSetting
          : !activity.idleEnabled
          ? l10n.youStaySignedWithoutInput2
          : left == null
          ? l10n.checkingTimeLeft
          : l10n.withoutInputYoureSignedOut(_countdown(left));
      return Padding(
        key: const Key('session-idle-control'),
        padding: EdgeInsets.symmetric(horizontal: touch ? 12 : 10, vertical: 8),
        // A plain row of the settings window: no icon.
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Expanded(
                        child: Text(
                          l10n.autoSignOut2,
                          style: uiText(
                            size: touch ? 14 : 13,
                            weight: FontWeight.w600,
                            color: p.ink,
                          ),
                        ),
                      ),
                      if (canChange) ...[
                        const SizedBox(width: 6),
                        Semantics(
                          label: l10n.autoSignOut2,
                          child: Switch(
                            key: const Key('session-idle-switch'),
                            value: activity.idleEnabled,
                            onChanged: pending ? null : activity.setIdleEnabled,
                            materialTapTargetSize: touch
                                ? MaterialTapTargetSize.padded
                                : MaterialTapTargetSize.shrinkWrap,
                          ),
                        ),
                      ],
                    ],
                  ),
                  const SizedBox(height: 4),
                  Text(
                    status,
                    style: uiText(size: 12, color: p.muted, height: 1.45),
                  ),
                  if (canChange && !pending && error != null) ...[
                    const SizedBox(height: 4),
                    Text(
                      l10n.couldntChangeSettingTryAgain,
                      key: const Key('session-idle-error'),
                      style: uiText(size: 12, color: p.amberInk, height: 1.45),
                    ),
                  ],
                ],
              ),
            ),
          ],
        ),
      );
    },
  );
}

import 'package:flutter/material.dart';

import '../models/desktop_machine.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// Explains the broker's stream cleanup barrier for one assigned desktop.
///
/// A short wait only informs. A stalled wait states that restoration was not
/// confirmed and offers the action this client can actually take: refresh the
/// broker snapshot. It never offers streaming; the broker alone clears the
/// barrier.
class SessionCleanupDialog extends StatelessWidget {
  const SessionCleanupDialog({super.key, required this.machine, this.onRefresh});

  final DesktopMachine machine;
  final VoidCallback? onRefresh;

  static String _clock(DateTime value) {
    final local = value.toLocal();
    String two(int n) => n.toString().padLeft(2, '0');
    return '${two(local.hour)}:${two(local.minute)}:${two(local.second)}';
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    void close() => Navigator.pop(context);
    if (!machine.cleanupStalled) {
      return AppDialog(
        key: const Key('session-cleanup-dialog'),
        icon: Icons.hourglass_top_rounded,
        tone: AppTone.info,
        title: l10n.restoringScreen,
        subtitle: machine.name,
        body: [AppDialog.message(context, machine.streamReadiness)],
        actions: [
          FilledButton(
            key: const Key('cleanup-close'),
            autofocus: true,
            onPressed: close,
            style: AppButtons.filled(p),
            child: KeepAllText(l10n.ok),
          ),
        ],
      );
    }
    final at = machine.sessionCleanup.checkedAt;
    void refresh() {
      close();
      onRefresh!();
    }

    return AppDialog(
      key: const Key('session-cleanup-dialog'),
      icon: Icons.warning_amber_rounded,
      tone: AppTone.warn,
      title: l10n.couldntConfirmScreenWasRestored,
      subtitle: machine.name,
      body: [
        AppDialog.message(
          context,
          l10n.connectionsBlockedUntilRestoreConfirmed(machine.cleanupCause),
        ),
        const SizedBox(height: 12),
        KeepAllText(
          machine.cleanupNextStep,
          key: const Key('cleanup-next-step'),
          style: uiText(
            size: 13.5,
            height: 1.55,
            weight: FontWeight.w600,
            color: p.ink,
          ),
        ),
        if (at != null) ...[
          const SizedBox(height: 10),
          KeepAllText(
            l10n.statusRecorded(machine.cleanupEvidence, _clock(at)),
            key: const Key('cleanup-evidence'),
            style: uiText(size: 12, height: 1.4, color: p.muted),
          ),
        ],
      ],
      actions: [
        TextButton(
          key: const Key('cleanup-close'),
          onPressed: close,
          style: AppButtons.quiet(p),
          child: KeepAllText(l10n.close),
        ),
        if (onRefresh != null)
          FilledButton.icon(
            key: const Key('cleanup-refresh'),
            onPressed: refresh,
            style: AppButtons.filled(p),
            icon: const Icon(Icons.refresh_rounded, size: 18),
            label: KeepAllText(l10n.refresh),
          ),
      ],
    );
  }
}

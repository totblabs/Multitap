import 'package:flutter/material.dart';

import '../models/desktop_machine.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import 'desktop_readiness.dart';
import 'vm_resource_bars.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// The readiness checklist: one verdict sentence and four evidence rows
/// (전원 / Host 서비스 / 가상 모니터 / 중계 경로). A row the client cannot
/// confirm stays grey ("확인 안 됨") instead of being painted green. Only the
/// failing row carries an action, and only actions this user may take.
///
/// Opening it asks the broker for the desktop's state again ([onCheck]); the
/// rows follow each list refresh and the footer shows when that state was
/// received, with "다시 점검" (2026-10-26: it only showed a stale snapshot).
class ReadinessDialog extends StatelessWidget {
  const ReadinessDialog({
    super.key,
    required this.machine,
    required this.readiness,
    this.hostVersion,
    this.actionFor,
    this.checkedAt,
    this.checking = false,
    this.checkFailed = false,
    this.onCheck,
  });

  final DesktopMachine machine;
  final DesktopReadiness readiness;

  /// A quiet information line under the rows ("Host 1.2.3 · 최신"); the
  /// update itself stays in ⋯, listed only while it can be installed.
  final String? hostVersion;

  /// When the state shown was received from the broker.
  final DateTime? checkedAt;
  final bool checking, checkFailed;

  /// Asks the broker again; null while that is not possible.
  final VoidCallback? onCheck;

  /// The callback that performs an action, or null when it is unavailable.
  final VoidCallback? Function(ReadinessAction action)? actionFor;

  static String actionLabel(ReadinessAction action) => switch (action) {
    ReadinessAction.start => l10n.start,
    ReadinessAction.reboot => l10n.restart,
    ReadinessAction.refresh => l10n.refresh,
    ReadinessAction.console => l10n.openConsole2,
    ReadinessAction.releaseConsole => l10n.disconnectConsoleConnect2,
  };

  static String _clock(DateTime time) {
    final local = time.toLocal();
    String two(int value) => value.toString().padLeft(2, '0');
    return '${two(local.hour)}:${two(local.minute)}:${two(local.second)}';
  }

  static IconData actionIcon(ReadinessAction action) => switch (action) {
    ReadinessAction.start => Icons.power_settings_new_rounded,
    ReadinessAction.reboot => Icons.restart_alt_rounded,
    ReadinessAction.refresh => Icons.refresh_rounded,
    ReadinessAction.console => Icons.settings_remote_outlined,
    ReadinessAction.releaseConsole => Icons.swap_horiz_rounded,
  };

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final width = MediaQuery.sizeOf(context).width;
    final narrow = width / MediaQuery.textScalerOf(context).scale(1) < 520;
    final tone = switch (readiness.phase) {
      TilePhase.ready => CheckState.ok,
      TilePhase.waiting when readiness.progressLabel != null =>
        CheckState.pending,
      TilePhase.attention || TilePhase.off => CheckState.warn,
      _ => CheckState.unknown,
    };
    final toneColor = switch (tone) {
      CheckState.ok => p.green,
      CheckState.pending => p.amber,
      CheckState.warn => p.warn,
      CheckState.unknown => p.muted,
    };
    final resources = machine.resources;
    final hasResources =
        resources.cpuUsagePercent != null ||
        resources.memoryUsedBytes != null ||
        resources.vcpus != null;
    void close() => Navigator.pop(context);

    Widget row(int index, ReadinessCheck check) {
      final action = check.action;
      final perform = action == null ? null : actionFor?.call(action);
      final color = switch (check.state) {
        CheckState.ok => p.green,
        CheckState.pending => p.amber,
        CheckState.warn => p.warn,
        CheckState.unknown => p.faint,
      };
      final icon = check.state == CheckState.pending
          ? SizedBox.square(
              key: Key('readiness-progress-$index'),
              dimension: 18,
              child: CircularProgressIndicator(strokeWidth: 2, color: color),
            )
          : Icon(
              switch (check.state) {
                CheckState.ok => Icons.check_rounded,
                CheckState.warn => Icons.warning_amber_rounded,
                _ => Icons.remove_rounded,
              },
              size: 18,
              color: color,
            );
      final label = KeepAllText(
        check.label,
        style: TextStyle(
          fontSize: 13.5,
          fontWeight: FontWeight.w600,
          color: check.state == CheckState.unknown ? p.muted : p.ink,
        ),
      );
      final detail = KeepAllText(
        check.state == CheckState.unknown && !check.detail.startsWith(l10n.unknown4)
            ? l10n.unknown5(check.detail)
            : check.detail,
        key: Key('readiness-detail-$index'),
        style: TextStyle(
          fontSize: 13,
          height: 1.4,
          color: p.muted,
        ),
      );
      final button = perform == null
          ? null
          : FilledButton.icon(
              key: Key('readiness-action-${action!.name}'),
              onPressed: () {
                close();
                perform();
              },
              style: FilledButton.styleFrom(
                backgroundColor: p.amber,
                foregroundColor: p.onAmber,
                minimumSize: const Size(0, 32),
                padding: const EdgeInsets.symmetric(horizontal: 12),
                // The platform's target: 48 dp by finger, compact by mouse.
                textStyle: uiText(size: 12.5, weight: FontWeight.w600),
                shape: RoundedRectangleBorder(
                  borderRadius: BorderRadius.circular(8),
                ),
              ),
              icon: Icon(actionIcon(action), size: 15),
              label: KeepAllText(actionLabel(action)),
            );
      return Container(
        key: Key('readiness-row-$index'),
        constraints: const BoxConstraints(minHeight: 48),
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
        decoration: BoxDecoration(
          color: check.state == CheckState.warn ? p.warnSoft : null,
          border: index == 0
              ? null
              : Border(top: BorderSide(color: p.tileLine)),
        ),
        child: narrow
            ? Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(children: [icon, const SizedBox(width: 10), label]),
                  const SizedBox(height: 4),
                  Padding(
                    padding: const EdgeInsets.only(left: 28),
                    child: detail,
                  ),
                  if (button != null) ...[
                    const SizedBox(height: 8),
                    Padding(
                      padding: const EdgeInsets.only(left: 28),
                      child: button,
                    ),
                  ],
                ],
              )
            : Row(
                children: [
                  SizedBox(width: 20, child: icon),
                  const SizedBox(width: 10),
                  SizedBox(width: 104, child: label),
                  // The action sits under its detail so a long Korean detail
                  // keeps the full width instead of breaking mid-word.
                  Expanded(
                    child: button == null
                        ? detail
                        : Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              detail,
                              const SizedBox(height: 8),
                              button,
                            ],
                          ),
                  ),
                ],
              ),
      );
    }

    return Dialog(
      key: const Key('readiness-dialog'),
      backgroundColor: p.tile,
      surfaceTintColor: Colors.transparent,
      insetPadding: EdgeInsets.symmetric(
        horizontal: narrow ? 12 : 32,
        vertical: 24,
      ),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(18),
        side: BorderSide(color: p.tileLine),
      ),
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 620),
        child: SingleChildScrollView(
          padding: const EdgeInsets.fromLTRB(22, 18, 22, 18),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              // The AppDialog header (2026-11-03): what this is as the
              // title, the VM as the subtitle, as in every other dialog.
              Row(
                children: [
                  if (!narrow) ...[
                    // The verdict's colour, never green over a warning.
                    AppToneIcon(
                      key: const Key('readiness-header-icon'),
                      icon: Icons.fact_check_outlined,
                      tone: switch (tone) {
                        CheckState.ok => AppTone.success,
                        CheckState.warn => AppTone.warn,
                        CheckState.pending => AppTone.accent,
                        CheckState.unknown => AppTone.neutral,
                      },
                    ),
                    const SizedBox(width: 14),
                  ],
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        KeepAllText(
                          l10n.checkReadiness,
                          key: const Key('readiness-title'),
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: uiText(
                            size: 17,
                            weight: FontWeight.w600,
                            color: p.ink,
                            height: 1.35,
                          ).copyWith(letterSpacing: -.2),
                        ),
                        const SizedBox(height: 3),
                        KeepAllText(
                          machine.name +
                              (machine.specification.isEmpty
                                  ? ''
                                  : ' · ${machine.specification}'),
                          maxLines: 2,
                          overflow: TextOverflow.ellipsis,
                          style: uiText(size: 13, color: p.muted),
                        ),
                      ],
                    ),
                  ),
                  const SizedBox(width: 8),
                  IconButton(
                    key: const Key('close-readiness'),
                    tooltip: l10n.close,
                    onPressed: close,
                    style: IconButton.styleFrom(
                      foregroundColor: p.muted,
                      minimumSize: const Size.square(34),
                      maximumSize: const Size.square(34),
                      padding: EdgeInsets.zero,
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(10),
                      ),
                    ),
                    icon: const Icon(Icons.close_rounded, size: 20),
                  ),
                ],
              ),
              const SizedBox(height: 14),
              Container(
                key: const Key('readiness-verdict'),
                padding: const EdgeInsets.fromLTRB(14, 12, 16, 12),
                decoration: BoxDecoration(
                  color: switch (tone) {
                    CheckState.ok => p.okSoft,
                    CheckState.warn => p.warnSoft,
                    CheckState.pending || CheckState.unknown => p.tileWell,
                  },
                  borderRadius: BorderRadius.circular(14),
                  border: Border.all(
                    color: switch (tone) {
                      CheckState.ok => p.okLine,
                      CheckState.warn => p.warnLine,
                      CheckState.pending || CheckState.unknown => p.tileLine,
                    },
                  ),
                ),
                child: Row(
                  children: [
                    Container(
                      width: 36,
                      height: 36,
                      decoration: BoxDecoration(
                        color: toneColor,
                        shape: BoxShape.circle,
                      ),
                      child: tone == CheckState.pending
                          ? Padding(
                              padding: const EdgeInsets.all(9),
                              child: CircularProgressIndicator(
                                key: const Key('readiness-verdict-progress'),
                                strokeWidth: 2,
                                color: p.tile,
                              ),
                            )
                          : Icon(
                              switch (tone) {
                                CheckState.ok => Icons.check_rounded,
                                CheckState.warn => Icons.priority_high_rounded,
                                _ => Icons.more_horiz_rounded,
                              },
                              size: 20,
                              color: p.tile,
                            ),
                    ),
                    const SizedBox(width: 14),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          KeepAllText(
                            readiness.headline,
                            style: TextStyle(
                              color: p.ink,
                              fontSize: 16.5,
                              fontWeight: FontWeight.w600,
                            ),
                          ),
                          const SizedBox(height: 2),
                          KeepAllText(
                            readiness.detail,
                            style: TextStyle(
                              color: p.muted,
                              fontSize: 12.5,
                              height: 1.45,
                            ),
                          ),
                        ],
                      ),
                    ),
                  ],
                ),
              ),
              const SizedBox(height: 12),
              ClipRRect(
                borderRadius: BorderRadius.circular(14),
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    border: Border.all(color: p.tileLine),
                    borderRadius: BorderRadius.circular(14),
                  ),
                  child: Column(
                    children: [
                      for (final (index, check) in readiness.checks.indexed)
                        row(index, check),
                    ],
                  ),
                ),
              ),
              if (hostVersion != null) ...[
                const SizedBox(height: 10),
                Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 4),
                  child: Row(
                    children: [
                      Icon(
                        machine.hostUpdate.offered
                            ? Icons.system_update_alt_rounded
                            : Icons.info_outline_rounded,
                        size: 15,
                        color: machine.hostUpdate.offered ? p.amber : p.faint,
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: KeepAllText(
                          hostVersion!,
                          key: const Key('readiness-host-version'),
                          style: TextStyle(color: p.muted, fontSize: 12.5),
                        ),
                      ),
                    ],
                  ),
                ),
              ],
              if (hasResources) ...[
                const SizedBox(height: 14),
                KeepAllText(
                  // A VM's Host can measure inside Windows too (measuredByHost).
                  machine.kind == MachineKind.pc || resources.measuredByHost
                      ? l10n.usageReportedByHost
                      : l10n.usageReportedByProxmox,
                  style: TextStyle(
                    color: p.muted,
                    fontSize: 12,
                    fontWeight: FontWeight.w600,
                    letterSpacing: .3,
                  ),
                ),
                const SizedBox(height: 8),
                VmResourceBars(
                  sample: resources,
                  pc: machine.kind == MachineKind.pc,
                ),
              ],
              const SizedBox(height: 12),
              Row(
                children: [
                  Expanded(
                    child: KeepAllText(
                      checking
                          ? l10n.checking4
                          : [
                              if (checkFailed) l10n.couldNotCheck,
                              if (checkedAt != null)
                                '${checkFailed ? l10n.lastResult : l10n.checked3} ${_clock(checkedAt!)}',
                              if (!checkFailed) l10n.statusServerGotFromHost,
                            ].join(' · '),
                      key: const Key('readiness-checked'),
                      style: TextStyle(
                        color: checkFailed ? p.warn : p.muted,
                        fontSize: 12,
                      ),
                    ),
                  ),
                  TextButton.icon(
                    key: const Key('readiness-check'),
                    onPressed: checking ? null : onCheck,
                    style: TextButton.styleFrom(
                      foregroundColor: p.ink,
                      minimumSize: const Size(0, 30),
                      padding: const EdgeInsets.symmetric(horizontal: 10),
                      // The platform's target: 48 dp by finger.
                      textStyle: uiText(size: 12.5, weight: FontWeight.w600),
                    ),
                    icon: checking
                        ? const SizedBox.square(
                            dimension: 14,
                            child: CircularProgressIndicator(strokeWidth: 2),
                          )
                        : const Icon(Icons.refresh_rounded, size: 16),
                    label: KeepAllText(l10n.checkAgain3),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}

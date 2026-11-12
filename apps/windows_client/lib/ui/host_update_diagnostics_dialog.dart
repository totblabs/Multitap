import 'package:flutter/material.dart';

import '../models/desktop_machine.dart';
import '../models/host_update.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

Future<void> showHostUpdateDiagnostics(
  BuildContext context,
  DesktopMachine machine,
) => showDialog<void>(
  context: context,
  builder: (_) => HostUpdateDiagnosticsDialog(machine: machine),
);

class HostUpdateDiagnosticsDialog extends StatelessWidget {
  const HostUpdateDiagnosticsDialog({super.key, required this.machine});
  final DesktopMachine machine;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final update = machine.hostUpdate, operation = update.operation;
    final code = operation?.code ?? '';
    final shownCode = RegExp(r'^[a-z_]{1,48}$').hasMatch(code)
        ? code
        : 'unknown';
    final result = switch (operation?.status) {
      'failed' || 'rejected' => l10n.failed,
      'unknown' => l10n.checkResult2,
      'expired' => l10n.waitExpired,
      'completed' => l10n.done,
      _ => l10n.progress2,
    };
    final reason = code == 'install_failed'
        ? l10n.installerCouldNotFinishUpdate
        : hostUpdateReason(code);
    final stacked = MediaQuery.textScalerOf(context).scale(13) >= 19.5;
    Widget row(String name, String value) => Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: stacked
          ? Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                KeepAllText(name, style: uiText(size: 12.5, color: p.muted)),
                const SizedBox(height: 2),
                KeepAllText(value, style: uiText(size: 13, color: p.ink)),
              ],
            )
          : Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                SizedBox(
                  width: 82,
                  child: KeepAllText(
                    name,
                    style: uiText(size: 12.5, color: p.muted),
                  ),
                ),
                Expanded(
                  child: KeepAllText(
                    value,
                    style: uiText(size: 13, color: p.ink),
                  ),
                ),
              ],
            ),
    );
    return AppDialog(
      key: const Key('host-update-diagnostics-dialog'),
      icon: Icons.troubleshoot_rounded,
      title: l10n.hostUpdateDiagnostics,
      subtitle: machine.name,
      maxWidth: 480,
      onClose: () => Navigator.pop(context),
      body: [
        AppDialog.message(context, reason),
        const SizedBox(height: 10),
        row(l10n.currentVersion, update.version ?? l10n.unknown4),
        row(l10n.requestedVersion, operation?.targetVersion ?? l10n.unknown4),
        row(l10n.result, result),
        row(l10n.errorCode2, shownCode),
        if (operation?.checkedAt != null)
          row(
            l10n.checked2,
            operation!.checkedAt!.toLocal().toString().split('.').first,
          ),
        const SizedBox(height: 12),
        AppDialog.message(
          context,
          l10n.checkInstallStatusHostSettings(machine.kind == MachineKind.pc ? l10n.localPc3 : l10n.openDesktopConsole),
        ),
      ],
      // A lone acknowledgement is the main button, as in every notice.
      actions: [
        FilledButton(
          key: const Key('close-host-update-diagnostics'),
          autofocus: true,
          onPressed: () => Navigator.pop(context),
          style: AppButtons.filled(p),
          child: KeepAllText(l10n.close),
        ),
      ],
    );
  }
}

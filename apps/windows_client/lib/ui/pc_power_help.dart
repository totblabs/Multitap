import 'package:flutter/material.dart';

import '../models/pc_power.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// "PC는 원격으로 켤 수 없어요" with one line of reason; what
/// Wake-on-LAN needs stays behind 자세히 (concise reading).
Future<void> showPcWakeHelp(BuildContext context, {required String reason}) =>
    showDialog<void>(
      context: context,
      builder: (_) => _PcWakeHelp(reason: reason),
    );

class _PcWakeHelp extends StatefulWidget {
  const _PcWakeHelp({required this.reason});
  final String reason;
  @override
  State<_PcWakeHelp> createState() => _PcWakeHelpState();
}

class _PcWakeHelpState extends State<_PcWakeHelp> {
  bool _details = false;
  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return AppDialog(
      key: const Key('pc-wake-help'),
      icon: Icons.power_settings_new_rounded,
      tone: AppTone.warn,
      title: l10n.couldNotStartLocalPc,
      body: [
        AppDialog.message(
          context,
          widget.reason,
          key: const Key('pc-wake-reason'),
        ),
        AppDisclosure(
          headerKey: const Key('pc-wake-details'),
          label: _details ? l10n.collapse : l10n.whatNeedsStart,
          open: _details,
          onChanged: (open) => setState(() => _details = open),
        ),
        if (_details)
          for (final line in pcWakeRequirements)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: KeepAllText(
                '· $line',
                style: uiText(size: 12.5, color: p.muted),
              ),
            ),
      ],
      actions: [
        // Acknowledging does nothing: the grey button.
        TextButton(
          key: const Key('pc-wake-help-close'),
          autofocus: true,
          onPressed: () => Navigator.pop(context),
          style: AppButtons.quiet(p),
          child: KeepAllText(l10n.ok),
        ),
      ],
    );
  }
}

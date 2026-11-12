import 'package:flutter/material.dart';

import '../data/broker_api.dart';
import '../models/desktop_machine.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// Configures real VM displays from the session toolbar (the launcher menu
/// has no count: connecting prepares it). Selecting which
/// existing display to view is separate. The confirmation is a step of this
/// one dialog, not a second dialog stacked on top of it.
class VirtualDisplayDialog extends StatefulWidget {
  const VirtualDisplayDialog({
    super.key,
    required this.machine,
    required this.onApply,
  });
  final DesktopMachine machine;
  final Future<DesktopMachine> Function(int count) onApply;
  @override
  State<VirtualDisplayDialog> createState() => _VirtualDisplayDialogState();
}

class _VirtualDisplayDialogState extends State<VirtualDisplayDialog> {
  late int _count = widget.machine.display.activeCount == 2 ? 2 : 1;
  bool _confirming = false, _busy = false, _attempted = false;
  String? _error;

  bool get _canChange =>
      !_busy && !_attempted && widget.machine.canConfigureDisplays;

  void _askToApply() {
    if (!_canChange) return;
    setState(() => _confirming = true);
  }

  void _cancelConfirmation() => setState(() => _confirming = false);

  Future<void> _apply() async {
    if (!_canChange) return;
    setState(() {
      _confirming = false;
      _busy = true;
      _attempted = true;
      _error = null;
    });
    try {
      final machine = await widget.onApply(_count);
      if (!mounted) return;
      if (machine.display.activeCount != _count ||
          machine.display.stale ||
          machine.display.busy) {
        throw const BrokerException('display_operation_unknown');
      }
      Navigator.pop(context, machine);
    } catch (error) {
      if (mounted) {
        setState(
          () => _error = error is BrokerException
              ? error.message
              : l10n.couldntConfirmNumberMonitorsChanged,
        );
      }
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Widget _current(AppPalette p) => KeepAllText(
    widget.machine.display.label,
    key: const Key('actual-virtual-display-count'),
    style: uiText(size: 13, weight: FontWeight.w600, color: p.ink),
  );

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final failed = _error != null;
    final List<Widget> body;
    final List<Widget> actions;
    if (_confirming) {
      body = [
        _current(p),
        const SizedBox(height: 8),
        AppDialog.message(context, l10n.connectionClosesReconnectAfterNumber),
      ];
      actions = [
        TextButton(
          onPressed: _cancelConfirmation,
          style: AppButtons.quiet(p),
          child: KeepAllText(l10n.cancel),
        ),
        FilledButton(
          key: const Key('confirm-virtual-displays'),
          autofocus: true,
          onPressed: _apply,
          style: AppButtons.filled(p),
          child: KeepAllText(l10n.disconnectApply),
        ),
      ];
    } else {
      body = [
        _current(p),
        if (!_attempted) ...[
          const SizedBox(height: 14),
          SegmentedButton<int>(
            key: const Key('virtual-display-count'),
            expandedInsets: EdgeInsets.zero,
            showSelectedIcon: false,
            segments: [
              ButtonSegment(
                value: 1,
                icon: Icon(Icons.crop_landscape_rounded, size: 18),
                label: KeepAllText(l10n.n1Monitor),
              ),
              ButtonSegment(
                value: 2,
                icon: Icon(Icons.view_column_rounded, size: 18),
                label: KeepAllText(l10n.n2Monitors),
              ),
            ],
            selected: {_count},
            onSelectionChanged: _canChange
                ? (values) => setState(() => _count = values.single)
                : null,
          ),
          const SizedBox(height: 12),
          AppDialog.message(context, l10n.numberMonitorsKeptDesktopWhich),
        ],
        if (_busy)
          Padding(
            padding: const EdgeInsets.only(top: 12),
            child: AppDialog.message(context, l10n.changingNumberMonitors2),
          ),
        if (failed)
          Padding(
            padding: const EdgeInsets.only(top: 12),
            child: KeepAllText(
              _error!,
              style: uiText(size: 13, height: 1.6, color: p.red),
            ),
          ),
      ];
      actions = [
        TextButton(
          onPressed: _busy ? null : () => Navigator.pop(context),
          style: AppButtons.quiet(p),
          child: KeepAllText(l10n.close),
        ),
        if (!_attempted)
          FilledButton(
            key: const Key('apply-virtual-displays'),
            onPressed:
                _canChange && _count != widget.machine.display.activeCount
                ? _askToApply
                : null,
            style: AppButtons.filled(p),
            child: KeepAllText(l10n.apply),
          ),
      ];
    }
    return PopScope(
      canPop: !_busy,
      child: AppDialog(
        icon: failed ? Icons.error_outline_rounded : Icons.monitor_rounded,
        tone: failed ? AppTone.danger : AppTone.accent,
        busy: _busy,
        title: _confirming ? l10n.switchLabel2(_count) : l10n.monitors,
        subtitle: widget.machine.name,
        body: body,
        actions: actions,
      ),
    );
  }
}

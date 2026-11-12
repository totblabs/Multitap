import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/broker_api.dart';
import '../data/console_resume.dart';
import '../models/desktop_machine.dart';
import 'app_dialog.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

class ConsoleResumeDialog extends StatefulWidget {
  const ConsoleResumeDialog({
    super.key,
    required this.machine,
    required this.resume,
    this.onCancelled,
  });
  final DesktopMachine machine;
  final Future<DesktopMachine> Function() resume;
  final VoidCallback? onCancelled;
  @override
  State<ConsoleResumeDialog> createState() => _ConsoleResumeDialogState();
}

// No second question: the tile already says the Windows screen is taken
// over, so opening this starts at once and only shows progress or the
// failure. Cancelling retires this connection intent, never reverses a Windows
// transfer that the Host may have started already.
class _ConsoleResumeDialogState extends State<ConsoleResumeDialog> {
  bool _busy = false, _attempted = false;
  bool _retired = false;
  final _cancellation = ConsoleResumeCancellation();
  String? _error;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _resume();
    });
  }

  Future<void> _resume() async {
    if (_retired || _busy || _attempted) return;
    setState(() {
      _busy = true;
      _attempted = true;
    });
    try {
      final machine = await _cancellation.bind(widget.resume);
      if (mounted && !_retired) {
        _retired = true;
        Navigator.pop(context, machine);
      }
    } catch (error) {
      if (mounted && !_retired) {
        setState(() {
          _busy = false;
          _error = error is BrokerException
              ? error.message
              : l10n.couldntConfirmSessionSwitchResult;
        });
      }
    }
  }

  void _retire() {
    if (_retired) return;
    _retired = true;
    _cancellation.cancel();
    if (_error == null) widget.onCancelled?.call();
  }

  void _dismiss() {
    if (_retired) return;
    _retire();
    Navigator.pop(context);
  }

  @override
  void dispose() {
    _retire();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final failed = _error != null;
    return PopScope(
      onPopInvokedWithResult: (didPop, _) {
        if (didPop) _retire();
      },
      child: CallbackShortcuts(
        bindings: {const SingleActivator(LogicalKeyboardKey.escape): _dismiss},
        child: Focus(
          autofocus: true,
          child: AppDialog(
            icon: failed
                ? Icons.error_outline_rounded
                : Icons.swap_horiz_rounded,
            tone: failed ? AppTone.danger : AppTone.accent,
            busy: _busy || !_attempted,
            title: failed ? l10n.couldntTakeOverWindowsScreen : l10n.takingOverWindowsScreen,
            subtitle: widget.machine.name,
            body: [
              // The red is the icon above; the sentence reads as text.
              if (failed)
                AppDialog.message(context, _error!)
              else
                AppDialog.message(
                  context,
                  l10n.bringingOverWindowsScreenYou,
                ),
            ],
            actions: [
              // A lone acknowledgement is the grey button, as in every
              // notice: it carries nothing out.
              if (failed)
                TextButton(
                  key: const Key('console-resume-close'),
                  onPressed: _dismiss,
                  style: AppButtons.quiet(p),
                  child: KeepAllText(l10n.ok),
                )
              else
                TextButton(
                  key: const Key('console-resume-cancel'),
                  onPressed: _dismiss,
                  style: AppButtons.quiet(p),
                  child: KeepAllText(l10n.cancel),
                ),
            ],
          ),
        ),
      ),
    );
  }
}

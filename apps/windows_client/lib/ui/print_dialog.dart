import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/native_print.dart';
import '../data/print_session.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import 'print_preview.dart';
import '../l10n/l10n.dart';

/// A document the VM printed to "Multitap 프린터 (PC)": print it on a
/// printer of this PC (the last one used is preselected), save it as a PDF,
/// or cancel. Nothing prints until the user presses 인쇄. Resolves with what
/// happened (sent to the Host as a fixed outcome only).
Future<PrintOutcome> showPrintDialog(
  BuildContext context, {
  required PrintDocument document,
  required String machine,
  LocalPrintBridge bridge = const NativeLocalPrint(),
  PrintMemory? memory,
  int waiting = 0,
}) async {
  final outcome = await showDialog<PrintOutcome>(
    context: context,
    barrierDismissible: false,
    builder: (_) => PrintDialog(
      document: document,
      machine: machine,
      bridge: bridge,
      memory: memory,
      waiting: waiting,
    ),
  );
  return outcome ?? PrintOutcome.cancelled;
}

class PrintDialog extends StatefulWidget {
  const PrintDialog({
    super.key,
    required this.document,
    required this.machine,
    required this.bridge,
    this.memory,
    this.waiting = 0,
  });
  final PrintDocument document;
  final String machine;
  final LocalPrintBridge bridge;
  final PrintMemory? memory;

  /// Documents already received and waiting behind this one (each gets its
  /// own dialog in turn).
  final int waiting;
  @override
  State<PrintDialog> createState() => _PrintDialogState();
}

class _PrintDialogState extends State<PrintDialog> {
  List<LocalPrinter>? _printers;
  String? _printer;
  int? _pages;
  int _copies = 1;
  bool _busy = false;
  String? _error;

  /// What the spooler says is wrong with the chosen printer ('' when nothing
  /// is known to be): shown, never blocking (the job waits in its queue).
  String _status = '';

  /// Page progress of the running print, polled from the native worker. The
  /// bar waits ~1.2 s so a short job never flashes one.
  static const progressDelay = Duration(milliseconds: 1200);
  Timer? _poll, _slow;
  LocalPrintProgress? _progress;
  bool _showProgress = false, _printing = false;

  @override
  void dispose() {
    _poll?.cancel();
    _slow?.cancel();
    super.dispose();
  }

  void _startProgress() {
    _progress = null;
    _showProgress = false;
    _printing = true;
    _slow?.cancel();
    _slow = Timer(progressDelay, () {
      if (mounted && _busy) setState(() => _showProgress = true);
    });
    _poll?.cancel();
    var asking = false;
    _poll = Timer.periodic(const Duration(milliseconds: 250), (_) async {
      if (asking) return;
      asking = true;
      final value = await widget.bridge.progress();
      asking = false;
      if (!mounted || !_busy || value == null) return;
      final old = _progress;
      if (old == null || old.done != value.done || old.total != value.total) {
        setState(() => _progress = value);
      }
    });
  }

  void _stopProgress() {
    _poll?.cancel();
    _slow?.cancel();
    _progress = null;
    _showProgress = false;
    _printing = false;
  }

  /// What the dialog says while it works: the save dialog's wait, or a print
  /// that shows its page count once it takes a moment.
  Widget _busyPanel(BuildContext context) {
    final progress = _progress;
    if (!_printing || !_showProgress) {
      return Padding(
        padding: const EdgeInsets.only(top: 10),
        child: AppDialog.message(
          context,
          _printing ? l10n.sendingPrinter : l10n.saving,
        ),
      );
    }
    final p = AppPalette.of(context);
    // All pages are with the print control: the spooler takes the job over.
    final sending = progress != null && progress.done >= progress.total;
    final known = progress != null && progress.known && !sending;
    final label = sending
        ? l10n.sendingPrinter
        : known
        ? l10n.printingPage(progress.done, progress.total)
        : l10n.preparingPrint;
    return Padding(
      padding: const EdgeInsets.only(top: 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          KeepAllText(
            label,
            key: const Key('print-progress-label'),
            style: uiText(size: 13, color: p.ink),
          ),
          const SizedBox(height: 8),
          LinearProgressIndicator(
            key: const Key('print-progress-bar'),
            value: known ? progress.done / progress.total : null,
          ),
        ],
      ),
    );
  }

  @override
  void initState() {
    super.initState();
    unawaited(_load());
  }

  Future<void> _load() async {
    final results = await Future.wait<Object?>([
      widget.bridge.printers(),
      widget.bridge.pages(widget.document.file.path),
      widget.memory?.load() ?? Future<String?>.value(),
    ]);
    if (!mounted) return;
    final printers = results[0] as List<LocalPrinter>;
    final remembered = results[2] as String?;
    setState(() {
      _printers = printers;
      _pages = results[1] as int?;
      _printer = printers.any((value) => value.name == remembered)
          ? remembered
          : printers
                    .where((value) => value.isDefault)
                    .map((value) => value.name)
                    .firstOrNull ??
                printers.firstOrNull?.name;
    });
    unawaited(_checkStatus());
  }

  Future<void> _checkStatus() async {
    final printer = _printer;
    if (printer == null) return;
    final status = await widget.bridge.status(printer);
    if (mounted && _printer == printer && status != _status) {
      setState(() => _status = status);
    }
  }

  Future<void> _print() async {
    final printer = _printer;
    if (_busy || printer == null) return;
    setState(() {
      _busy = true;
      _error = null;
    });
    _startProgress();
    final result = await widget.bridge.print(
      widget.document.file.path,
      printer: printer,
      copies: _copies,
      title: widget.document.title.isEmpty
          ? 'Multitap ${widget.machine}'
          : widget.document.title,
    );
    _stopProgress();
    if (!mounted) return;
    if (result.ok) {
      unawaited(widget.memory?.save(printer));
      Navigator.pop(context, PrintOutcome.printed);
      return;
    }
    setState(() {
      _busy = false;
      // Closing the printer's own dialog (Print to PDF asks where to save)
      // keeps this one open for another choice, like the save dialog.
      _error = result.cancelled ? null : printCodeMessage(result.code);
    });
    unawaited(_checkStatus());
  }

  Future<void> _save() async {
    if (_busy) return;
    setState(() {
      _busy = true;
      _error = null;
    });
    final result = await widget.bridge.save(
      widget.document.file.path,
      suggestedName: widget.document.suggestedName,
    );
    if (!mounted) return;
    if (result.ok) {
      Navigator.pop(context, PrintOutcome.saved);
      return;
    }
    setState(() {
      _busy = false;
      // Closing the save dialog keeps this one open for another choice.
      _error = result.cancelled ? null : printCodeMessage(result.code);
    });
  }

  static String _size(int bytes) => bytes < 1024 * 1024
      ? '${(bytes / 1024).ceil()} KB'
      : '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MB';

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final printers = _printers;
    final loading = printers == null;
    final none = printers != null && printers.isEmpty;
    final details = [
      if (_pages != null) l10n.text27(_pages!),
      _size(widget.document.size),
      'PDF',
    ].join(' · ');
    return PopScope(
      canPop: !_busy,
      child: CallbackShortcuts(
        bindings: {
          const SingleActivator(LogicalKeyboardKey.escape): () {
            if (!_busy) Navigator.pop(context, PrintOutcome.cancelled);
          },
        },
        child: AppDialog(
          key: const Key('print-dialog'),
          icon: Icons.print_rounded,
          busy: _busy,
          title: widget.document.title.isEmpty
              ? l10n.documentSentFromDesktop
              : widget.document.title,
          titleKey: const Key('print-document-title'),
          subtitle:
              '${widget.machine} · $details${widget.waiting > 0 ? l10n.moreWaiting(widget.waiting) : ''}',
          body: [
            AppDialog.message(
              context,
              // The direction is said once, under the preview.
              l10n.paperColorTwoSidedPrinting,
            ),
            if (_pages != null && widget.bridge is LocalPrintPreviewBridge) ...[
              const SizedBox(height: 8),
              PrintPreview(
                path: widget.document.file.path,
                pages: _pages!,
                bridge: widget.bridge as LocalPrintPreviewBridge,
                enabled: !_busy,
              ),
            ],
            const SizedBox(height: 14),
            if (loading)
              const LinearProgressIndicator(minHeight: 2)
            else if (none)
              AppStatusLine(
                l10n.noPrinterInstalledPcYou,
                textKey: Key('print-no-printers'),
                tone: AppTone.warn,
              )
            else
              AppSelect<String>(
                fieldKey: const Key('print-printer'),
                label: l10n.printer,
                value: _printer,
                items: [
                  for (final printer in printers)
                    DropdownMenuItem(
                      value: printer.name,
                      child: Text(
                        printer.isDefault
                            ? l10n.defaultLabel2(printer.name)
                            : printer.name,
                        overflow: TextOverflow.ellipsis,
                      ),
                    ),
                ],
                onChanged: _busy
                    ? null
                    : (value) {
                        setState(() {
                          _printer = value;
                          _status = '';
                        });
                        unawaited(_checkStatus());
                      },
              ),
            if (_status.isNotEmpty && !loading && !none)
              Padding(
                padding: const EdgeInsets.only(top: 8),
                child: AppStatusLine(
                  printCodeMessage(_status),
                  textKey: const Key('print-printer-status'),
                  tone: AppTone.warn,
                ),
              ),
            if (!loading && !none) ...[
              const SizedBox(height: 12),
              Row(
                children: [
                  Expanded(
                    child: KeepAllText(
                      l10n.copies,
                      style: uiText(
                        size: 13.5,
                        weight: FontWeight.w600,
                        color: p.ink,
                      ),
                    ),
                  ),
                  IconButton(
                    key: const Key('print-copies-less'),
                    tooltip: _copies > 1 ? l10n.oneFewerCopy : l10n.n1CopyMinimum,
                    onPressed: _busy || _copies <= 1
                        ? null
                        : () => setState(() => _copies--),
                    icon: const Icon(Icons.remove_rounded, size: 18),
                  ),
                  SizedBox(
                    width: 36,
                    child: Text(
                      '$_copies',
                      key: const Key('print-copies'),
                      textAlign: TextAlign.center,
                      style: uiText(
                        size: 15,
                        weight: FontWeight.w600,
                        color: p.ink,
                      ),
                    ),
                  ),
                  IconButton(
                    key: const Key('print-copies-more'),
                    tooltip: _copies < NativeLocalPrint.maximumCopies
                        ? l10n.oneMoreCopy
                        : l10n.youCanPrintUpCopies(NativeLocalPrint.maximumCopies),
                    onPressed:
                        _busy || _copies >= NativeLocalPrint.maximumCopies
                        ? null
                        : () => setState(() => _copies++),
                    icon: const Icon(Icons.add_rounded, size: 18),
                  ),
                ],
              ),
            ],
            if (_busy) _busyPanel(context),
            if (_error != null)
              Padding(
                padding: const EdgeInsets.only(top: 10),
                // One red icon; the sentence reads as text.
                child: AppStatusLine(
                  _error!,
                  textKey: const Key('print-error'),
                ),
              ),
          ],
          actions: [
            TextButton(
              key: const Key('print-cancel'),
              onPressed: _busy
                  ? null
                  : () => Navigator.pop(context, PrintOutcome.cancelled),
              style: AppButtons.quiet(p),
              child: KeepAllText(l10n.cancel),
            ),
            OutlinedButton(
              key: const Key('print-save'),
              onPressed: _busy ? null : _save,
              style: AppButtons.outlined(p),
              child: KeepAllText(l10n.saveAsPdf),
            ),
            Tooltip(
              message: _printer == null && !loading ? l10n.noPrinterPrint : '',
              child: FilledButton(
                key: const Key('print-confirm'),
                autofocus: true,
                onPressed: _busy || _printer == null ? null : _print,
                style: AppButtons.filled(p),
                child: KeepAllText(l10n.print),
              ),
            ),
          ],
          maxWidth: widget.bridge is LocalPrintPreviewBridge ? 560 : 440,
        ),
      ),
    );
  }
}

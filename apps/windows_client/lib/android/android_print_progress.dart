import 'dart:async';

import 'package:flutter/material.dart';

import '../data/print_session.dart';
import '../ui/clipboard_transfer_notice.dart' show clipboardBytes;
import '../l10n/l10n.dart';

/// Owns only the current print receive notice. It does not share or clear the
/// snack-bar queue, so finishing a download cannot dismiss another message.
class AndroidPrintProgress {
  AndroidPrintProgress({required this.context});

  final BuildContext? Function() context;
  static const delay = Duration(milliseconds: 1200);
  PrintSession? _printer;
  Timer? _slow;
  OverlayEntry? _entry;
  bool _receiving = false, _disposed = false;

  void watch(PrintSession? printer) {
    if (_disposed || identical(printer, _printer)) return;
    _printer?.removeListener(_changed);
    _hide();
    _printer = printer?..addListener(_changed);
    _changed();
  }

  void _changed() {
    final printer = _printer;
    if (printer == null || printer.closed || !printer.receiving) {
      _hide();
      return;
    }
    if (_receiving) return;
    _receiving = true;
    _slow = Timer(delay, () {
      _slow = null;
      if (_disposed ||
          !identical(printer, _printer) ||
          printer.closed ||
          !printer.receiving) {
        return;
      }
      final target = context();
      if (target == null || !target.mounted) return;
      final overlay = Overlay.maybeOf(target);
      if (overlay == null) return;
      final entry = OverlayEntry(
        builder: (context) => Positioned(
          left: 12,
          right: 12,
          bottom: 12 + MediaQuery.viewInsetsOf(context).bottom,
          child: SafeArea(
            top: false,
            child: IgnorePointer(
              child: Align(
                alignment: Alignment.bottomCenter,
                child: ConstrainedBox(
                  constraints: const BoxConstraints(maxWidth: 320),
                  child: Material(
                    key: const Key('android-print-receive'),
                    color:
                        Theme.of(context).snackBarTheme.backgroundColor ??
                        const Color(0xff25313a),
                    elevation: 3,
                    borderRadius: BorderRadius.circular(8),
                    child: DefaultTextStyle(
                      style:
                          Theme.of(context).snackBarTheme.contentTextStyle ??
                          const TextStyle(
                            color: Color(0xffedf1f5),
                            fontSize: 14,
                          ),
                      child: Padding(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 10,
                          vertical: 8,
                        ),
                        child: _ReceiveSummary(printer: printer),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      );
      _entry = entry;
      overlay.insert(entry);
    });
  }

  void _hide() {
    _slow?.cancel();
    _slow = null;
    _receiving = false;
    final entry = _entry;
    _entry = null;
    entry?.remove();
    entry?.dispose();
  }

  void dispose() {
    _disposed = true;
    _printer?.removeListener(_changed);
    _printer = null;
    _hide();
  }
}

/// One line of actual received bytes and a thin progress bar. The entire
/// containing overlay remains pointer-passive while the transfer continues.
class _ReceiveSummary extends StatefulWidget {
  const _ReceiveSummary({required this.printer});
  final PrintSession printer;

  @override
  State<_ReceiveSummary> createState() => _ReceiveSummaryState();
}

class _ReceiveSummaryState extends State<_ReceiveSummary> {
  Timer? _sample;
  int _received = 0, _total = 0;

  @override
  void initState() {
    super.initState();
    _read();
    _sample = Timer.periodic(const Duration(milliseconds: 250), (_) {
      if (mounted && _read()) setState(() {});
    });
  }

  bool _read() {
    if (!widget.printer.receiving) return false;
    final received = widget.printer.receivedBytes;
    final total = widget.printer.receivingSize;
    final changed = received != _received || total != _total;
    _received = received;
    _total = total;
    return changed;
  }

  @override
  void dispose() {
    _sample?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final known = _total > 0;
    final percent = known ? (_received * 100 ~/ _total).clamp(0, 100) : 0;
    final label = known
        ? '${clipboardBytes(_received)} / ${clipboardBytes(_total)} · $percent%'
        : l10n.receivingPrintDocument;
    return Semantics(
      // The size needs its subject; the sentence already names it.
      label: known ? l10n.receivingPrintDocument2(label) : label,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              const Icon(
                Icons.print_outlined,
                size: 14,
                color: Color(0xffb9c2c9),
              ),
              const SizedBox(width: 6),
              Expanded(
                child: Text(
                  label,
                  key: const Key('print-receive-size'),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    fontSize: 12,
                    color: Color(0xffedf1f5),
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 4),
          LinearProgressIndicator(
            key: const Key('print-receive-bar'),
            minHeight: 2,
            value: known ? _received / _total : null,
          ),
        ],
      ),
    );
  }
}

import 'dart:async';

import 'package:flutter/material.dart';

import '../data/print_session.dart';
import 'clipboard_transfer_notice.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// The body of the snack bar shown while a document the VM printed is still
/// arriving: "VM에서 문서를 받는 중", a determinate amber bar and
/// "N MB / M MB · 35%" (the size is announced up front by print.job), the same
/// look as the clipboard transfer notice. The time left appears only when the
/// measured rate is steady. It samples the receive a few times a second.
class PrintReceiveNotice extends StatefulWidget {
  const PrintReceiveNotice({
    super.key,
    required this.printer,
    this.interval = const Duration(milliseconds: 250),
    this.clock,
  });
  final PrintSession printer;
  final Duration interval;

  /// Time since the notice appeared; a stopwatch unless a test supplies one.
  final Duration Function()? clock;

  @override
  State<PrintReceiveNotice> createState() => _ReceiveNoticeState();
}

class _ReceiveNoticeState extends State<PrintReceiveNotice> {
  late final Stopwatch _watch = Stopwatch()..start();
  final _estimate = TransferEstimator();
  Timer? _timer;
  int _received = 0, _total = 0;
  Duration? _left;

  Duration _now() => widget.clock?.call() ?? _watch.elapsed;

  @override
  void initState() {
    super.initState();
    _sample();
    _timer = Timer.periodic(widget.interval, (_) {
      if (_sample() && mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  /// True when what is shown changed. Keeps the last numbers once the receive
  /// has ended (the bar is closed right after).
  bool _sample() {
    final printer = widget.printer;
    if (!printer.receiving) return false;
    final received = printer.receivedBytes, total = printer.receivingSize;
    final now = _now();
    _estimate.add(now, received);
    final left = total > 0 ? _estimate.remaining(now, received, total) : null;
    final changed =
        received != _received ||
        total != _total ||
        clipboardRemaining(left) != clipboardRemaining(_left);
    _received = received;
    _total = total;
    _left = left;
    return changed;
  }

  @override
  Widget build(BuildContext context) {
    final known = _total > 0;
    final percent = known ? (_received * 100 ~/ _total).clamp(0, 100) : 0;
    final left = clipboardRemaining(_left);
    final small = Theme.of(context).snackBarTheme.contentTextStyle
        ?.copyWith(fontSize: 12, color: const Color(0xFFB9C2C9));
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        KeepAllText(l10n.receivingPrintDocument),
        const SizedBox(height: 8),
        LinearProgressIndicator(
          key: const Key('print-receive-bar'),
          value: known ? _received / _total : null,
        ),
        if (known) ...[
          const SizedBox(height: 6),
          Row(
            children: [
              Flexible(
                child: Text(
                  '${clipboardBytes(_received)} / ${clipboardBytes(_total)}'
                  ' · $percent%',
                  key: const Key('print-receive-size'),
                  style: small,
                ),
              ),
              if (left != null) ...[
                const Spacer(),
                Text(left, key: const Key('print-receive-eta'), style: small),
              ],
            ],
          ),
        ],
      ],
    );
  }
}

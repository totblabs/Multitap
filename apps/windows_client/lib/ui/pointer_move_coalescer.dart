import 'dart:async';

/// Limits move traffic while always delivering the most recent position.
/// Discard at every input-scope boundary; delayed work must never survive a
/// focus reset, capture lease replacement, or a new video epoch.
class PointerMoveCoalescer {
  PointerMoveCoalescer({
    required this._send,
    Duration interval = const Duration(milliseconds: 16),
    Duration Function()? elapsed,
  }) : _interval = interval {
    if (interval <= Duration.zero) {
      throw ArgumentError.value(interval, 'interval', 'Must be positive.');
    }
    final clock = Stopwatch()..start();
    _elapsed = elapsed ?? (() => clock.elapsed);
  }

  final void Function(double x, double y) _send;
  final Duration _interval;
  late final Duration Function() _elapsed;
  Duration? _lastSent;
  (double, double)? _pending;
  Timer? _timer;
  int _generation = 0;
  bool _disposed = false;

  void move(double x, double y, {bool immediate = false}) {
    if (_disposed) return;
    _pending = (x, y);
    final lastSent = _lastSent;
    final remaining = lastSent == null
        ? Duration.zero
        : _interval - (_elapsed() - lastSent);
    if (immediate || remaining <= Duration.zero) {
      flush();
    } else if (_timer == null) {
      final generation = _generation;
      _timer = Timer(remaining, () {
        if (!_disposed && generation == _generation) flush();
      });
    }
  }

  /// Input transitions call this before dispatch. Only moves are coalesced.
  void flush() {
    _timer?.cancel();
    _timer = null;
    final pending = _pending;
    _pending = null;
    if (_disposed || pending == null) return;
    _lastSent = _elapsed();
    _send(pending.$1, pending.$2);
  }

  void discard() {
    _generation++;
    _timer?.cancel();
    _timer = null;
    _pending = null;
    _lastSent = null;
  }

  void dispose() {
    discard();
    _disposed = true;
  }
}

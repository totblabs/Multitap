import 'dart:async';

/// Latest motion only, at most about 120 Hz. Button/wheel edges are never paced.
/// The view owns the lifetime and discards motion on focus/source changes.
class ConsolePointerPacer {
  ConsolePointerPacer(this.send);
  final void Function(int x, int y, int mask) send;
  static const interval = Duration(microseconds: 8334);
  Timer? _timer;
  (int, int, int)? _pending;
  int _generation = 0;
  bool _disposed = false;

  void motion(int x, int y, int mask) {
    if (_disposed) return;
    _pending = (x, y, mask);
    final generation = _generation;
    _timer ??= Timer(interval, () {
      if (_disposed || generation != _generation) return;
      _timer = null;
      _flush();
    });
  }

  void _flush() {
    _timer?.cancel();
    _timer = null;
    final value = _pending;
    _pending = null;
    if (!_disposed && value != null) send(value.$1, value.$2, value.$3);
  }

  void edge(int x, int y, int mask) {
    if (_disposed) return;
    final generation = _generation;
    _flush();
    if (!_disposed && generation == _generation) send(x, y, mask);
  }

  void reset() {
    ++_generation;
    _timer?.cancel();
    _timer = null;
    _pending = null;
  }

  void dispose() {
    _disposed = true;
    reset();
  }
}

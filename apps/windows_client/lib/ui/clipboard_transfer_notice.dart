import '../l10n/l10n.dart';

/// A count with thousands separators: 1702 is "1,702".
String groupDigits(int value) {
  final digits = value.abs().toString();
  final out = StringBuffer(value < 0 ? '-' : '');
  for (var i = 0; i < digits.length; i++) {
    if (i > 0 && (digits.length - i) % 3 == 0) out.write(',');
    out.write(digits[i]);
  }
  return out.toString();
}

/// The time left in plain words, rounded so it does not tick: "곧 끝나요",
/// "약 45초 남음", "약 4분 남음", "약 1시간 20분 남음"; null for no estimate.
String? transferEta(Duration? left) {
  if (left == null || left.isNegative) return null;
  final seconds = left.inMilliseconds / 1000;
  if (seconds < 10) return l10n.almostDone;
  if (seconds < 55) return l10n.aboutSecLeft(((seconds / 5).ceil() * 5));
  final minutes = (seconds / 60).round();
  if (minutes < 60) return l10n.aboutMinLeft(minutes);
  if (minutes >= 24 * 60) return null;
  final rest = (minutes % 60) ~/ 5 * 5;
  return rest == 0
      ? l10n.aboutLeft(minutes ~/ 60)
      : l10n.aboutHMinLeft(minutes ~/ 60, rest);
}

/// A failure text as its cause and the one thing to do next: the first
/// sentence, and the rest ('' when it is one sentence).
(String, String) splitReason(String text) {
  final trimmed = text.trim();
  final end = trimmed.indexOf('. ');
  if (end < 0) return (trimmed, '');
  return (trimmed.substring(0, end + 1), trimmed.substring(end + 2).trim());
}

/// Bytes in readable units: "812 KB", "1.2 GB".
String clipboardBytes(int bytes) {
  if (bytes < 1024) return '$bytes B';
  if (bytes < 1024 * 1024) return '${(bytes / 1024).round()} KB';
  if (bytes < 1024 * 1024 * 1024) {
    return '${(bytes / 1048576).toStringAsFixed(1)} MB';
  }
  return '${(bytes / 1073741824).toStringAsFixed(1)} GB';
}

/// "약 40초 남음", "약 3분 남음", "약 1시간 20분 남음"; null for no estimate.
String? clipboardRemaining(Duration? left) {
  if (left == null || left.isNegative) return null;
  final seconds = left.inMilliseconds / 1000;
  if (seconds < 60) return l10n.aboutSecLeft(seconds.ceil().clamp(1, 59));
  final minutes = (seconds / 60).round();
  if (minutes < 60) return l10n.aboutMinLeft(minutes);
  if (minutes >= 24 * 60) return null;
  final rest = minutes % 60;
  return rest == 0
      ? l10n.aboutLeft(minutes ~/ 60)
      : l10n.aboutHMinLeft(minutes ~/ 60, rest);
}

/// Time left from the measured rate. The rate is the bytes moved over the last
/// second, smoothed; nothing is estimated before [warmUp] of data, while no
/// bytes have moved, or while the smoothed rate is still swinging.
class TransferEstimator {
  TransferEstimator({
    this.warmUp = const Duration(seconds: 3),
    this.maxSwing = 1.5,
  });
  final Duration warmUp;

  /// The recent smoothed rates may differ by at most this factor.
  final double maxSwing;
  static const _window = Duration(seconds: 1);
  static const _smoothing = 0.2;
  static const _history = 8;
  final _samples = <(Duration, int)>[];
  final _rates = <double>[];
  Duration? _first;
  double? _rate;

  /// The smoothed rate in bytes per second; null before two samples.
  double? get rate => _rate;

  void add(Duration at, int bytes) {
    _first ??= at;
    _samples.add((at, bytes));
    while (_samples.length > 2 && at - _samples[1].$1 >= _window) {
      _samples.removeAt(0);
    }
    final oldest = _samples.first;
    final span = (at - oldest.$1).inMicroseconds / 1e6;
    if (span <= 0) return;
    final instant = (bytes - oldest.$2) / span;
    final rate = _rate == null
        ? instant
        : _rate! + (instant - _rate!) * _smoothing;
    _rate = rate;
    _rates.add(rate);
    if (_rates.length > _history) _rates.removeAt(0);
  }

  Duration? remaining(Duration at, int bytes, int total) {
    final first = _first, rate = _rate;
    if (first == null || rate == null || rate <= 0 || total <= bytes) {
      return null;
    }
    if (at - first < warmUp || _rates.length < _history) return null;
    final low = _rates.reduce((a, b) => a < b ? a : b);
    final high = _rates.reduce((a, b) => a > b ? a : b);
    if (low <= 0 || high / low > maxSwing) return null;
    return Duration(milliseconds: ((total - bytes) / rate * 1000).round());
  }
}

/// [name] shortened in the middle to at most [max] characters, keeping the end (the extension):
/// "아주아주긴이름.mp4" becomes "아주아…름.mp4". Shorter names are returned as they are.
String middleEllipsize(String name, int max) {
  if (max < 5 || name.length <= max) return name;
  final tail = (max - 1) ~/ 2;
  final head = max - 1 - tail;
  return '${name.substring(0, head)}…${name.substring(name.length - tail)}';
}

/// Opt-in Android lifecycle observation, separate from measured sample times.
/// Socket endings and transaction deadlines are deliberately outside this policy.
class AndroidWatchdogObservation {
  static const timeoutUs = 15000000;
  bool _enabled = false, _foreground = true;
  int _epoch = 0;
  int? _resumedUs;

  int get epoch => _epoch;

  void setForeground(bool foreground, int nowUs) {
    if (_enabled && _foreground == foreground) return;
    final returning = _enabled && !_foreground && foreground;
    _enabled = true;
    _foreground = foreground;
    ++_epoch;
    if (returning) _resumedUs = nowUs;
  }

  bool current(int epoch) => epoch == _epoch;

  bool stalled(int nowUs, int observedUs, {required bool eligible}) {
    if (_enabled && eligible) {
      if (!_foreground) return false;
      final resumed = _resumedUs;
      if (resumed != null && observedUs < resumed) observedUs = resumed;
    }
    return nowUs - observedUs > timeoutUs;
  }

  bool deferStatsFailure(int nowUs, {required bool eligible}) =>
      _enabled &&
      eligible &&
      (!_foreground ||
          (_resumedUs != null && nowUs - _resumedUs! <= timeoutUs));
}

import 'dart:async';

import 'broker_api.dart';

/// Why a session could not be kept alive by renewal alone.
enum RenewalLapse {
  /// The broker has no renewal endpoint (released broker).
  unsupported,

  /// The session reached the broker's absolute maximum lifetime.
  exhausted,

  /// The broker refused: the session is already gone or revoked (401).
  rejected,

  /// The exact owner was replaced; saved-password recovery must not run.
  replaced,

  /// Idle expiry is terminal even while a remote session is connected.
  idle,

  /// The broker stayed unreachable until the session's expiry.
  unreachable,
}

/// Keeps one signed-in session alive: [lead] before the expiry it asks the
/// broker to renew the same token ([renew]); every renewal moves the next
/// attempt. Whatever renewal cannot fix is reported once through [onLapse]
/// (the owner then signs in again silently or shows the sign-in screen).
class SessionRenewer {
  SessionRenewer({
    required this.renew,
    required this.onRenewed,
    required this.onLapse,
    this.lead = const Duration(minutes: 5),
    this.retry = const Duration(seconds: 30),
    this.minimumGain = const Duration(minutes: 1),
    DateTime Function()? clock,
  }) : _clock = clock ?? DateTime.now;

  final Future<DateTime> Function() renew;
  final void Function(DateTime expiresAt) onRenewed;
  final void Function(RenewalLapse reason) onLapse;
  final Duration lead, retry, minimumGain;
  final DateTime Function() _clock;
  Timer? _timer;
  DateTime? _expiresAt;
  int _generation = 0;
  String? terminalCode;

  DateTime? get expiresAt => _expiresAt;

  /// Begin (or restart for another token) with the session's current expiry.
  void start(DateTime expiresAt) {
    terminalCode = null;
    _generation++;
    _expiresAt = expiresAt;
    _schedule(_clock());
  }

  void stop() {
    _generation++;
    _timer?.cancel();
    _timer = null;
    _expiresAt = null;
  }

  void _schedule(DateTime now, [Duration? after]) {
    _timer?.cancel();
    final expiry = _expiresAt;
    if (expiry == null) return;
    final delay = after ?? expiry.subtract(lead).difference(now);
    final generation = _generation;
    _timer = Timer(delay.isNegative ? Duration.zero : delay, () {
      if (generation == _generation) unawaited(_attempt(generation));
    });
  }

  /// Renew now (also called after a 401 that arrives before the timer).
  Future<bool> renewNow() async {
    final generation = _generation;
    return _attempt(generation);
  }

  Future<bool> _attempt(int generation) async {
    final before = _expiresAt;
    if (before == null) return false;
    try {
      final next = await renew();
      if (generation != _generation) return false;
      if (next.difference(_clock()) <= lead + minimumGain) {
        // No room left to renew again before the expiry: the broker's
        // absolute maximum is reached. Keep the valid time the session still
        // has and report it once.
        _expiresAt = next.isAfter(before) ? next : before;
        _timer?.cancel();
        _timer = null;
        onLapse(RenewalLapse.exhausted);
        return false;
      }
      _expiresAt = next;
      onRenewed(next);
      _schedule(_clock());
      return true;
    } on BrokerException catch (error) {
      if (generation != _generation) return false;
      if (pcLoginTerminalCode(error.code, error.status) != null) {
        // Also fence a concurrent renewal that could succeed after retirement.
        stop();
        terminalCode = error.code;
        onLapse(
          error.code == 'session_idle_timeout'
              ? RenewalLapse.idle
              : RenewalLapse.replaced,
        );
      } else if (error.status == 404 || error.status == 405) {
        _timer?.cancel();
        _timer = null;
        onLapse(RenewalLapse.unsupported);
      } else if (error.status == 401) {
        stop();
        terminalCode = error.code;
        onLapse(RenewalLapse.rejected);
      } else {
        _transient();
      }
      return false;
    } catch (_) {
      if (generation == _generation) _transient();
      return false;
    }
  }

  void _transient() {
    final expiry = _expiresAt;
    final now = _clock();
    if (expiry == null) return;
    if (!expiry.isAfter(now)) {
      _timer?.cancel();
      _timer = null;
      onLapse(RenewalLapse.unreachable);
      return;
    }
    _schedule(now, retry);
  }
}

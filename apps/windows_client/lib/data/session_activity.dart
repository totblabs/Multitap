import 'dart:async';

import 'package:flutter/foundation.dart';

import 'broker_api.dart';
import '../l10n/l10n.dart';

/// Fixed local cleanup stages only; no account, window or clipboard identity.
enum SessionRetirementStage {
  routes,
  nativeHide,
  consoleHide,
  nativeChildren,
  consoleChildren,
}

enum SessionRetirementResult { verified, failed, timedOut }

/// One immutable observation. A later completion never rewrites a timeout.
class SessionRetirementOutcome {
  SessionRetirementOutcome(
    Map<SessionRetirementStage, SessionRetirementResult> stages,
  ) : stages = Map.unmodifiable(stages);

  final Map<SessionRetirementStage, SessionRetirementResult> stages;
  bool get verified =>
      stages.isNotEmpty &&
      stages.values.every((value) => value == SessionRetirementResult.verified);
}

/// Start independent cleanup before awaiting any route/frame/driver observer.
/// Timeouts retain the failed outcome; they never cancel a mutation or replay it.
Future<bool> retireSessionResources({
  required Future<void> Function() routes,
  required Future<void> Function() native,
  required Future<bool> Function() consoles,
  Future<void> Function()? nativeHide,
  Future<void> Function()? consoleHide,
  Duration routeTimeout = const Duration(seconds: 3),
  Duration nativeTimeout = const Duration(seconds: 15),
  Duration consoleTimeout = const Duration(seconds: 12),
}) async => (await observeSessionRetirement(
  routes: routes,
  native: native,
  consoles: consoles,
  nativeHide: nativeHide,
  consoleHide: consoleHide,
  routeTimeout: routeTimeout,
  nativeTimeout: nativeTimeout,
  consoleTimeout: consoleTimeout,
)).verified;

/// Windows recovery can re-observe exact retained work without replaying it.
/// The legacy boolean wrapper above keeps its original defaults and contract.
Future<SessionRetirementOutcome> observeSessionRetirement({
  required Future<void> Function() routes,
  required Future<void> Function() native,
  required Future<bool> Function() consoles,
  Future<void> Function()? nativeHide,
  Future<void> Function()? consoleHide,
  Duration routeTimeout = const Duration(seconds: 3),
  Duration nativeTimeout = const Duration(seconds: 15),
  Duration consoleTimeout = const Duration(seconds: 12),
}) async {
  Future<MapEntry<SessionRetirementStage, SessionRetirementResult>> observe(
    SessionRetirementStage stage,
    Future<bool> Function() action,
    Duration limit,
  ) async {
    try {
      final verified = await Future<bool>.sync(action).timeout(limit);
      return MapEntry(stage, verified
          ? SessionRetirementResult.verified
          : SessionRetirementResult.failed);
    } on TimeoutException {
      return MapEntry(stage, SessionRetirementResult.timedOut);
    } catch (_) {
      return MapEntry(stage, SessionRetirementResult.failed);
    }
  }

  final all = <Future<MapEntry<SessionRetirementStage, SessionRetirementResult>>>[
    if (nativeHide != null)
      observe(SessionRetirementStage.nativeHide, () async {
        await nativeHide();
        return true;
      }, nativeTimeout),
    if (consoleHide != null)
      observe(SessionRetirementStage.consoleHide, () async {
        await consoleHide();
        return true;
      }, consoleTimeout),
    observe(SessionRetirementStage.routes, () async {
      await routes();
      return true;
    }, routeTimeout),
    observe(SessionRetirementStage.nativeChildren, () async {
      await native();
      return true;
    }, nativeTimeout),
    observe(SessionRetirementStage.consoleChildren, consoles, consoleTimeout),
  ];
  return SessionRetirementOutcome(Map.fromEntries(await Future.wait(all)));
}

/// Native relative mouse packets own activity while cursor warping is active.
/// A server cursor echo must not become a Flutter hover keepalive.
abstract final class SessionHumanSource {
  static Object? _inputOwner;
  static bool Function()? _inputAllowed;
  static bool get inputAllowed => _inputAllowed?.call() ?? true;
  static void bindInput(Object owner, bool Function() allowed) {
    _inputOwner = owner;
    _inputAllowed = allowed;
  }

  static void unbindInput(Object owner) {
    if (identical(_inputOwner, owner)) {
      _inputOwner = null;
      _inputAllowed = null;
    }
  }

  static Object? _relativeOwner;
  static bool get nativeRelativePointer => _relativeOwner != null;
  static void relative(Object owner, bool active) {
    if (active) {
      _relativeOwner = owner;
    } else if (identical(_relativeOwner, owner)) {
      _relativeOwner = null;
    }
  }
}

/// One local authentication owner. Media, renewal and polling never call human().
/// Server receipt time owns extension; failed requests cannot invent a deadline.
class SessionActivity extends ChangeNotifier {
  SessionActivity({DateTime Function()? clock})
    : _clock = clock ?? DateTime.now;
  final DateTime Function() _clock;
  SessionAccess? _policy;
  Future<SessionAccess?> Function()? _record;
  Future<SessionAccess> Function(bool)? _setIdle;
  Future<void> Function(SessionAccess)? _onPolicy;
  void Function(String)? _onTerminal;
  Timer? _warningTimer, _expiryTimer, _activityTimer;
  Timer? _displayTimer;
  final _elapsed = Stopwatch();
  Duration _lifetime = Duration.zero;
  DateTime? _localDeadline;
  DateTime? _lastSubmission;
  int _generation = 0;
  int _policyRevision = 0;
  bool _expiryAuthority = true;
  bool _idleChangePending = false;
  String? _idleChangeError;
  bool _pending = false, _dirty = false, _warning = false, _disposed = false;
  String? _reason;
  String? owner;
  Object? _routeOwner;
  Future<void> Function()? _closeRoutes;
  void bindRoutes(Object owner, Future<void> Function() close) {
    _routeOwner = owner;
    _closeRoutes = close;
  }

  void unbindRoutes(Object owner) {
    if (identical(owner, _routeOwner)) {
      _routeOwner = null;
      _closeRoutes = null;
    }
  }

  Future<void> closeRoutes() => _closeRoutes?.call() ?? Future<void>.value();
  bool _retirementFinished = false, _retirementFailed = false;
  Object? _retirementRecoveryOwner;
  Future<void> Function()? _retirementRecovery;
  Future<void>? _retirementRecoveryPending;
  final _retirementAttempts = <SessionRetirementOutcome>[];
  String? get reason => _reason;
  bool get warning => _warning && _reason == null;
  bool get retirementFinished => _retirementFinished;
  bool get retirementFailed => _retirementFailed;
  bool get retirementRecovering => _retirementRecoveryPending != null;
  bool get retirementRecoveryAvailable =>
      !_disposed && _reason != null && _retirementFailed &&
      !_retirementFinished && _retirementRecovery != null;
  List<SessionRetirementOutcome> get retirementAttempts =>
      List.unmodifiable(_retirementAttempts);

  /// Opt-in only. Android callers retain the existing boolean/ACK behavior.
  void bindRetirementRecovery(Object owner, Future<void> Function() recover) {
    if (_disposed || _reason == null) return;
    if (!identical(owner, _retirementRecoveryOwner)) {
      _retirementRecoveryPending = null;
    }
    _retirementRecoveryOwner = owner;
    _retirementRecovery = recover;
    notifyListeners();
  }

  Future<void> retryRetirement() {
    final pending = _retirementRecoveryPending;
    if (pending != null) return pending;
    final recover = _retirementRecovery;
    if (!retirementRecoveryAvailable || recover == null) {
      return Future<void>.value();
    }
    final owner = _retirementRecoveryOwner, completed = Completer<void>();
    _retirementRecoveryPending = completed.future;
    notifyListeners();
    unawaited(() async {
      try {
        await recover();
      } catch (_) {
        // Unverified work remains retired; raw exception text is never shown.
      } finally {
        if (!_disposed && identical(owner, _retirementRecoveryOwner) &&
            identical(_retirementRecoveryPending, completed.future)) {
          _retirementRecoveryPending = null;
          notifyListeners();
        }
        completed.complete();
      }
    }());
    return completed.future;
  }
  bool get active => _policy != null && _reason == null;
  SessionAccess? get policy => _policy;
  bool get idleEnabled => _policy?.idleEnabled == true;
  bool get idleSupported => _policy != null;
  bool get canChangeIdle => _setIdle != null && _expiryAuthority && active;
  bool get idleChangePending => _idleChangePending;
  String? get idleChangeError => _idleChangeError;
  Duration? get remaining {
    if (!active || !idleEnabled) return null;
    final value = _lifetime - _elapsed.elapsed;
    final wall = _localDeadline!.difference(_clock());
    final left = value < wall ? value : wall;
    return left < Duration.zero ? Duration.zero : left;
  }

  void start(
    SessionAccess? policy, {
    String? owner,
    required Future<SessionAccess?> Function() record,
    required void Function(String) onTerminal,
    Future<SessionAccess> Function(bool)? setIdle,
    Future<void> Function(SessionAccess)? onPolicy,
    bool expiryAuthority = true,
  }) {
    stop();
    this.owner = owner;
    _reason = null;
    _retirementFinished = _retirementFailed = false;
    _retirementAttempts.clear();
    _record = record;
    _setIdle = setIdle;
    _onPolicy = onPolicy;
    _expiryAuthority = expiryAuthority;
    _idleChangePending = false;
    _idleChangeError = null;
    _onTerminal = onTerminal;
    if (policy != null) _apply(policy);
    notifyListeners();
  }

  void stop() {
    ++_generation;
    _retirementRecoveryOwner = null;
    _retirementRecovery = null;
    _retirementRecoveryPending = null;
    _warningTimer?.cancel();
    _expiryTimer?.cancel();
    _activityTimer?.cancel();
    _displayTimer?.cancel();
    _elapsed.stop();
    _policy = null;
    _record = null;
    _setIdle = null;
    _onPolicy = null;
    _onTerminal = null;
    owner = null;
    _pending = _dirty = _warning = false;
    _lastSubmission = null;
  }

  void _apply(SessionAccess value) {
    _policy = value;
    _warningTimer?.cancel();
    _expiryTimer?.cancel();
    _displayTimer?.cancel();
    _elapsed.stop();
    _warning = false;
    if (!value.idleEnabled) {
      _dirty = false;
      _activityTimer?.cancel();
      _activityTimer = null;
      return;
    }
    final left = value.remaining!;
    _lifetime = left < Duration.zero
        ? Duration.zero
        : left > value.idleTimeout
        ? value.idleTimeout
        : left;
    _elapsed
      ..reset()
      ..start();
    _localDeadline = _clock().add(_lifetime);
    _warningTimer?.cancel();
    _expiryTimer?.cancel();
    _warning = _lifetime <= value.warning;
    if (_lifetime > value.warning) {
      _warningTimer = Timer(_lifetime - value.warning, () {
        if (!check()) return;
        _warning = true;
        notifyListeners();
      });
    }
    if (_expiryAuthority) _expiryTimer = Timer(_lifetime, _expire);
    _displayTimer = Timer.periodic(const Duration(seconds: 1), (_) {
      if (!_disposed && active && idleEnabled) notifyListeners();
    });
  }

  /// Called before resume/source events. An expired owner cannot be revived.
  bool check() {
    final value = _policy;
    if (value == null || _reason != null) return _reason == null;
    if (!value.idleEnabled) return true;
    if (!_expiryAuthority) return true;
    if (!_localDeadline!.isAfter(_clock()) || _elapsed.elapsed >= _lifetime) {
      _expire();
      return false;
    }
    return true;
  }

  void _expire() {
    if (!active) return;
    terminate('session_idle_timeout');
  }

  void terminate(String code) {
    if (_reason != null) return;
    final callback = _onTerminal;
    retire(code);
    callback?.call(code);
  }

  void retire(String code) {
    if (_reason != null) return;
    stop();
    _reason = code;
    _retirementFinished = _retirementFailed = false;
    notifyListeners();
  }

  void finishRetirement({
    required bool verified,
    SessionRetirementOutcome? outcome,
    Object? recoveryOwner,
  }) {
    if (_reason == null || _disposed ||
        (recoveryOwner != null &&
            !identical(recoveryOwner, _retirementRecoveryOwner))) {
      return;
    }
    if (outcome != null) _retirementAttempts.add(outcome);
    _retirementFinished = verified && (outcome?.verified ?? true);
    _retirementFailed = !_retirementFinished;
    notifyListeners();
  }

  void acknowledge() {
    if (!_retirementFinished || retirementRecovering) return;
    _retirementRecoveryOwner = null;
    _retirementRecovery = null;
    _reason = null;
    _retirementFinished = false;
    notifyListeners();
  }

  void human() {
    if (!check() || !active || !idleEnabled || _idleChangePending) return;
    _dirty = true;
    _flush();
  }

  void _flush() {
    if (_pending ||
        !_dirty ||
        !check() ||
        !active ||
        !idleEnabled ||
        _idleChangePending) {
      return;
    }
    final last = _lastSubmission;
    final wait = last == null
        ? Duration.zero
        : const Duration(seconds: 5) - _clock().difference(last);
    if (wait > Duration.zero) {
      _activityTimer ??= Timer(wait, () {
        _activityTimer = null;
        _flush();
      });
      return;
    }
    _activityTimer?.cancel();
    _activityTimer = null;
    _dirty = false;
    _pending = true;
    _lastSubmission = _clock();
    final generation = _generation,
        revision = _policyRevision,
        record = _record!;
    unawaited(() async {
      try {
        final observed = await record();
        if (_disposed ||
            generation != _generation ||
            revision != _policyRevision ||
            !check() ||
            !active) {
          return;
        }
        // Accept only a bounded forward extension from this authenticated reply.
        if (observed != null &&
            observed.idleEnabled &&
            observed.remaining! <= observed.idleTimeout &&
            !observed.idleDeadline!.isBefore(_policy!.idleDeadline!)) {
          await _onPolicy?.call(observed);
          if (_disposed ||
              generation != _generation ||
              revision != _policyRevision ||
              !check()) {
            return;
          }
          _apply(observed);
          notifyListeners();
        }
      } on BrokerException catch (error) {
        if (!_disposed &&
            generation == _generation &&
            pcLoginTerminalCode(error.code, error.status) != null) {
          final callback = _onTerminal;
          retire(error.code);
          callback?.call(error.code);
        }
      } catch (_) {
        // Connectivity failure does not extend the observed deadline.
      } finally {
        if (!_disposed && generation == _generation) {
          _pending = false;
          if (_dirty) _flush();
        }
      }
    }());
  }

  /// Exact-owner deadline broadcast to an already opened child.
  void acceptPolicy(SessionAccess value) {
    if (!check() || !active) {
      return;
    }
    _apply(value);
    notifyListeners();
  }

  Future<void> setIdleEnabled(bool enabled) async {
    final submit = _setIdle;
    if (!check() ||
        !active ||
        _idleChangePending ||
        submit == null ||
        enabled == idleEnabled) {
      return;
    }
    final generation = _generation, revision = ++_policyRevision;
    _idleChangePending = true;
    _idleChangeError = null;
    notifyListeners();
    try {
      final next = await submit(enabled);
      if (_disposed ||
          generation != _generation ||
          revision != _policyRevision ||
          !check()) {
        return;
      }
      if (next.idleEnabled != enabled) {
        throw const BrokerException('invalid_response');
      }
      await _onPolicy?.call(next);
      if (_disposed ||
          generation != _generation ||
          revision != _policyRevision ||
          !check()) {
        return;
      }
      _apply(next);
    } on BrokerException catch (error) {
      if (!_disposed &&
          generation == _generation &&
          revision == _policyRevision) {
        if (pcLoginTerminalCode(error.code, error.status) != null) {
          terminate(error.code);
        } else {
          _idleChangeError = l10n.couldntChangeAutoSignOut;
        }
      }
    } catch (_) {
      if (!_disposed &&
          generation == _generation &&
          revision == _policyRevision) {
        _idleChangeError = l10n.couldntConfirmAutoSignOut;
      }
    } finally {
      if (!_disposed &&
          generation == _generation &&
          revision == _policyRevision) {
        _idleChangePending = false;
        notifyListeners();
      }
    }
  }

  @override
  void dispose() {
    _disposed = true;
    stop();
    super.dispose();
  }
}

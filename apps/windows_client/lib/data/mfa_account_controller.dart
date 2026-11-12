import 'dart:async';

import 'package:flutter/foundation.dart';

import 'broker_api.dart';
import '../l10n/l10n.dart';

/// A LOCAL presentation owner, not a server auth/revision claim. It contains
/// no bearer. Parent retires it on explicit logout, origin/account switch or
/// successor login, but not merely an authorizing-session401 while POST waits.
class MfaCompletionOwner {
  MfaCompletionOwner({required this.origin, required this.accountId,
    required this.username, required this.generation, required this._current}) {
    if (origin.scheme != 'https' || !RegExp(r'^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$').hasMatch(accountId) ||
        username.isEmpty || generation < 0) { throw ArgumentError('Invalid local MFA presentation owner.'); }
  }
  final Uri origin;
  final String accountId, username;
  final int generation;
  final bool Function() _current;
  bool _retired = false;
  bool get current { try { return !_retired && _current(); } catch (_) { return false; } }
  void retire() => _retired = true;
  @override
  String toString() => 'Local MFA presentation owner';
}

class MfaMutationTicket {
  MfaMutationTicket._(this.owner);
  final MfaCompletionOwner owner;
}
class MfaCompletedOperation {
  MfaCompletedOperation._(this.owner, this.action, this.enabled, this._codes);
  final MfaCompletionOwner owner;
  final MfaAccountAction action;
  final bool enabled;
  List<String> _codes;
  List<String> get codes => List.unmodifiable(_codes);
  bool get requiresLogin => true;
  void forget() => _codes = [];
  @override
  String toString() => 'Completed MFA operation';
}

/// Root-owned OUTSIDE the authenticated Navigator. It stores only a validated
/// completed operation, never a fake authenticated session or pending secret.
class MfaCompletionInbox extends ChangeNotifier {
  MfaMutationTicket? _pending;
  MfaCompletedOperation? _completed;
  bool _disposed = false;
  bool pendingFor(MfaCompletionOwner owner) => !_disposed && owner.current && identical(_pending?.owner, owner);
  MfaCompletedOperation? get completed {
    if (_completed?.owner.current == false) { _completed?.forget(); _completed = null; }
    return _completed;
  }
  MfaMutationTicket reserve(MfaCompletionOwner owner) {
    if (_disposed || !owner.current || _pending != null || completed != null) { throw StateError('MFA operation already owned.'); }
    return _pending = MfaMutationTicket._(owner);
  }
  bool accept(MfaMutationTicket ticket, MfaAccountChange change) {
    if (_disposed || !identical(_pending, ticket) || !ticket.owner.current) { change.forget(); return false; }
    _pending = null;
    _completed = MfaCompletedOperation._(ticket.owner, change.action, change.enabled, change.takeCodes());
    notifyListeners(); return true;
  }
  void reject(MfaMutationTicket ticket) { if (identical(_pending, ticket)) { _pending = null; } }
  void retire(MfaCompletionOwner owner) {
    owner.retire();
    if (identical(_pending?.owner, owner)) { _pending = null; }
    if (identical(_completed?.owner, owner)) { _completed?.forget(); _completed = null; }
    if (!_disposed) { notifyListeners(); }
  }
  bool acknowledgeSaved({required bool saved}) {
    final value = completed;
    if (value == null || (value.codes.isNotEmpty && !saved)) { return false; }
    value.forget(); _completed = null; if (!_disposed) { notifyListeners(); } return true;
  }
  void discard() { _completed?.forget(); _completed = null; if (!_disposed) { notifyListeners(); } }
  @override
  void dispose() { _disposed = true; _pending = null; _completed?.forget(); _completed = null; super.dispose(); }
}

enum MfaAccountPhase { unavailable, summary, proof, enrollment, pendingResult, uncertain, completed, closed }

class MfaAccountController extends ChangeNotifier {
  MfaAccountController({required this.api, required this.owner, required String token,
    required this._currentAuthentication, required this.completed, this.onCommitted, this.onAuthenticationRejected})
      : _token = token {
    if (api.origin != owner.origin || token.isEmpty) { throw ArgumentError('MFA owner does not match API.'); }
    completed.addListener(_completionChanged);
  }
  final MfaAccountBrokerApi api;
  final MfaCompletionOwner owner;
  final MfaCompletionInbox completed;
  final bool Function() _currentAuthentication;
  final void Function(MfaAccountAction action)? onCommitted;
  final ValueChanged<String>? onAuthenticationRejected;
  String? _token;
  MfaAccountStatus? _status;
  MfaEnrollment? _enrollment;
  MfaAccountAction? _action;
  MfaAccountPhase _phase = MfaAccountPhase.unavailable;
  String? _error;
  bool _busy = false, _reading = false, _mutating = false, _disposed = false;
  int _revision = 0;
  Timer? _expiry;
  MfaAccountStatus? get status => _status;
  MfaEnrollment? get enrollment => _enrollment;
  MfaAccountAction? get action => _action;
  String? get error => _error;
  bool get busy => _busy;
  bool get reading => _reading;
  bool get mutating => _mutating;
  MfaAccountPhase get phase => identical(completed.completed?.owner, owner) ? MfaAccountPhase.completed : _phase;
  bool get _current {
    if (_disposed || _token == null || !owner.current) { return false; }
    try { return _currentAuthentication(); } catch (_) { return false; }
  }
  void _notify() { if (!_disposed) { notifyListeners(); } }
  void _completionChanged() => _notify();
  void _clearEnrollment() { _expiry?.cancel(); _expiry = null; _enrollment?.forget(); _enrollment = null; }
  void _expireEnrollment(MfaEnrollment value) {
    if (_disposed || !identical(_enrollment, value)) { return; }
    // Local expiry retires the candidate, never the exact sent mutation.
    // Its current root ticket can still receive a real committed reply.
    _clearEnrollment();
    _phase = _mutating ? MfaAccountPhase.pendingResult : MfaAccountPhase.proof;
    _error = _mutating ? null : l10n.setupTimeRanOutStart;
    _notify();
  }
  /// Pure display/paint observation. Do not invoke the parent's authentication
  /// callback here: it can retire/notify during a build. Actual retirement
  /// synchronously clears this local token/setup; owner.current is presentation.
  bool canDisplayEnrollment(MfaEnrollment value) => !_disposed && _token != null &&
      owner.current && identical(_enrollment, value) &&
      _phase == MfaAccountPhase.enrollment && value.remaining > Duration.zero &&
      value.secret.isNotEmpty && value.uri != null;
  /// Dispatch-time only: never called by a copy-enabled/build getter.
  bool canCopyEnrollment(MfaEnrollment value) {
    if (!_current || !identical(_enrollment, value) ||
        _phase != MfaAccountPhase.enrollment || value.secret.isEmpty) { return false; }
    if (value.remaining == Duration.zero) { _expireEnrollment(value); return false; }
    return true;
  }
  void _authenticationRejected(BrokerException error) {
    final current = _current;
    _token = null; _clearEnrollment(); _status = null; _phase = MfaAccountPhase.closed;
    if (current) { onAuthenticationRejected?.call(pcLoginTerminalCode(error.code, error.status) ?? 'session_expired'); }
    _notify();
  }
  Future<void> refresh() async {
    if (!_current || _busy || _reading || _mutating || completed.completed != null) { return; }
    final request = ++_revision, token = _token!; _reading = true; _error = null; _notify();
    try {
      final value = await api.readMfaAccount(token);
      if (!_current || request != _revision) { return; }
      _status = value; _phase = MfaAccountPhase.summary;
    } catch (error) {
      if (!_current || request != _revision) { return; }
      if (error is BrokerException && error.status == 401) { _authenticationRejected(error); return; }
      _status = null; _phase = MfaAccountPhase.unavailable;
      _error = l10n.checkAgainMoment;
    } finally { if (request == _revision) { _reading = false; _notify(); } }
  }
  void choose(MfaAccountAction action) {
    if (!_current || _busy || _status == null || completed.completed != null || _phase == MfaAccountPhase.uncertain) { return; }
    if ((action == MfaAccountAction.enable) == _status!.enabled) { return; }
    ++_revision; _reading = false; _clearEnrollment(); _action = action;
    _error = null; _phase = MfaAccountPhase.proof; _notify();
  }
  void cancelDraft() {
    if (_busy || _mutating) { return; }
    ++_revision; _clearEnrollment(); _action = null; _error = null;
    _phase = _status == null ? MfaAccountPhase.unavailable : MfaAccountPhase.summary; _notify();
  }
  Future<void> submitProof(String currentPassword, {String? code, String? recoveryCode}) async {
    if (!_current || _busy || _action == null || _phase != MfaAccountPhase.proof) { return; }
    final request = ++_revision, token = _token!, action = _action!;
    _busy = true; _reading = false; _error = null; _notify();
    try {
      if (action == MfaAccountAction.enable) {
        final value = await api.setupMfaAccount(token, currentPassword);
        if (!_current || request != _revision) { value.forget(); return; }
        _enrollment = value; _phase = MfaAccountPhase.enrollment;
        _expiry = Timer(value.remaining, () => _expireEnrollment(value));
      } else {
        await api.reauthenticateMfaAccount(token, currentPassword: currentPassword, code: code, recoveryCode: recoveryCode);
        if (!_current || request != _revision) { return; }
        await _change(action, () => action == MfaAccountAction.disable
            ? api.disableMfaAccount(token, currentPassword)
            : api.regenerateMfaRecovery(token, currentPassword));
      }
    } catch (error) {
      if (request == _revision && _current && error is BrokerException && error.status == 401) {
        _authenticationRejected(error); return;
      }
      if (!_disposed && request == _revision && !_mutating) { _error = error is BrokerException ? error.message : l10n.couldntFinishIdentityVerificationTry; }
    } finally { _busy = false; _notify(); }
  }
  Future<void> enable(String code) async {
    final value = _enrollment;
    if (!_current || _busy || value == null || _phase != MfaAccountPhase.enrollment) { return; }
    if (value.remaining == Duration.zero) { _clearEnrollment(); _phase = MfaAccountPhase.proof; _error = l10n.setupTimeRanOutStart; _notify(); return; }
    final token = _token!; ++_revision; _busy = true; _error = null; _notify();
    try { await _change(MfaAccountAction.enable, () => api.enableMfaAccount(token, value, code)); }
    finally { _busy = false; _notify(); }
  }
  bool _definitive(Object error) => error is BrokerException &&
      {400, 401, 403, 410, 429}.contains(error.status);
  Future<void> _change(MfaAccountAction action, Future<MfaAccountChange> Function() dispatch) async {
    if (!_current) { return; }
    final MfaMutationTicket ticket;
    try { ticket = completed.reserve(owner); }
    on StateError { _error = l10n.checkResultOtherSettingsChange; _notify(); return; }
    _mutating = true; _notify();
    try {
      final value = await dispatch();
      if (value.action != action) { value.forget(); throw const FormatException('MFA action does not match.'); }
      // The server revoked the authorizing session BEFORE this response.
      // Auth current may now be false; ONLY this exact pending ticket and
      // separate current presentation owner may receive the acknowledged codes.
      final accepted = completed.accept(ticket, value);
      _token = null; _clearEnrollment(); _status = null;
      _phase = accepted ? MfaAccountPhase.completed : MfaAccountPhase.closed;
      if (accepted) {
        try { onCommitted?.call(action); }
        catch (_) { _error = l10n.settingChangedSignOutThen; }
      }
    } catch (error) {
      completed.reject(ticket);
      if (error is BrokerException && error.status == 401) { _authenticationRejected(error); return; }
      if (_definitive(error) && _current) {
        _error = (error as BrokerException).message;
        final enrollment = _enrollment;
        if (action == MfaAccountAction.enable && enrollment != null && enrollment.remaining > Duration.zero) {
          _phase = MfaAccountPhase.enrollment;
        } else {
          _clearEnrollment(); _phase = MfaAccountPhase.proof;
        }
      } else {
        _token = null; _status = null; _clearEnrollment(); _phase = MfaAccountPhase.uncertain;
        _error = l10n.couldntConfirmChange;
      }
    } finally { _mutating = false; _notify(); }
  }
  /// Auth revocation is not presentation identity change. No new requests may
  /// run, but an already pending exact ticket can still receive its one reply.
  void authenticationRetired() {
    _token = null; _clearEnrollment(); _status = null;
    if (completed.completed == null) { _phase = MfaAccountPhase.closed; }
    _notify();
  }
  bool closeByUser() {
    if (_mutating) { return false; }
    // Closing the old auth-scoped dialog must not discard its ROOT receipt.
    // Only the completed view's explicit saved/discard choice does that.
    if (identical(completed.completed?.owner, owner)) { return true; }
    completed.retire(owner); _token = null; ++_revision; _clearEnrollment(); _phase = MfaAccountPhase.closed; _notify(); return true;
  }
  @override
  void dispose() {
    _disposed = true; _token = null; _clearEnrollment(); completed.removeListener(_completionChanged);
    // Do not cancel a sent mutation merely because auth-route teardown disposed
    // this controller. The root inbox/owner independently fences its late reply.
    super.dispose();
  }
}

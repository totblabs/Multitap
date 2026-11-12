import 'dart:async';
import 'dart:convert';

import 'package:crypto/crypto.dart';
import 'package:flutter/foundation.dart';

import 'mfa_account.dart';
import 'mfa_account_controller.dart';

/// Root-owned presentation coordinator. The actor comes from the parent's actual
/// current native authentication owner; this object grants no authentication.
class MfaAccountHost extends ChangeNotifier {
  MfaAccountHost({DateTime Function()? clock}) : _clock = clock ?? DateTime.now {
    inbox.addListener(_completionChanged);
  }
  static const completionIdle = Duration(minutes: 30);
  final MfaCompletionInbox inbox = MfaCompletionInbox();
  final DateTime Function() _clock;
  MfaCompletionOwner? _owner;
  MfaAccountController? _controller;
  String? _actor;
  String? _sessionHash;
  int _generation = 0;
  bool _disposed = false;
  Timer? _idle;
  final _age = Stopwatch();
  DateTime? _lastHuman;
  MfaCompletedOperation? _observed;
  bool get blocked => _controller != null || inbox.completed != null || (_owner != null && inbox.pendingFor(_owner!));

  MfaAccountController? begin({required MfaAccountBrokerApi api, required String token,
    required String accountId, required String username, required String actor,
    required bool Function() currentPresentation, required bool Function() currentAuthentication,
    required void Function(MfaAccountAction action) onCommitted, required ValueChanged<String> onAuthenticationRejected}) {
    if (_disposed || blocked || !currentPresentation() || !currentAuthentication() ||
        !RegExp(r'^[a-f0-9]{32}$').hasMatch(actor)) { return null; }
    retire();
    final generation = ++_generation;
    late final MfaCompletionOwner owner;
    owner = MfaCompletionOwner(origin: api.origin, accountId: accountId, username: username,
      generation: generation, current: () => !_disposed && _generation == generation && identical(_owner, owner) && currentPresentation());
    _owner = owner; _actor = actor; _sessionHash = sha256.convert(utf8.encode(token)).toString();
    return _controller = MfaAccountController(api: api, owner: owner, token: token,
      currentAuthentication: currentAuthentication, completed: inbox,
      onCommitted: onCommitted, onAuthenticationRejected: onAuthenticationRejected);
  }
  void finished(MfaAccountController controller) {
    if (identical(_controller, controller)) { _controller = null; }
    controller.dispose();
  }
  /// The parent calls this before its existing authentication retirement. Only
  /// an exact pending/acknowledged own mutation can survive a generic rejection.
  /// Takeover/idle/unknown reasons always discard, regardless of any ticket.
  void authenticationRetired(String? actor, String reason) {
    if (_disposed || actor == null || _actor != actor) { return; }
    final owner = _owner;
    final own = owner != null && (inbox.pendingFor(owner) || identical(inbox.completed?.owner, owner));
    if (own && const {'session_expired', 'authentication_required', 'session_reauthentication_required'}.contains(reason)) {
      _controller?.authenticationRetired();
      return;
    }
    retire();
  }
  /// Correlate even a terminal callback arriving after generic401 cleared the
  /// parent's session. Retain only a digest, not the authorizing bearer.
  void sessionRetired(String token, String reason) {
    if (_disposed || _sessionHash == null || sha256.convert(utf8.encode(token)).toString() != _sessionHash) { return; }
    authenticationRetired(_actor, reason);
  }
  /// Explicit logout, server/account change, successor intent and root disposal
  /// call this synchronously BEFORE publishing any replacement identity.
  void retire() {
    if (_disposed) { return; }
    ++_generation; _actor = null; _sessionHash = null;
    final owner = _owner; _owner = null;
    if (owner != null) { inbox.retire(owner); }
    _controller?.authenticationRetired();
    _clearIdle();
  }
  void _clearIdle() { _idle?.cancel(); _idle = null; _lastHuman = null; _observed = null; _age.stop(); _age.reset(); }
  void _completionChanged() {
    if (_disposed) { return; }
    final completed = inbox.completed;
    if (completed == null) { _clearIdle(); }
    else if (!identical(_observed, completed)) {
      _observed = completed; _lastHuman = _clock(); _age.reset(); _age.start(); _schedule();
    }
    notifyListeners();
  }
  void _schedule() { _idle?.cancel(); _idle = Timer(completionIdle, checkPrivacyIdle); }
  /// Called ONLY by the completed overlay's actual local pointer/key handlers.
  /// Frames, status GETs, callbacks, focus/resume and network events never call it.
  void human() {
    if (!checkPrivacyIdle() || inbox.completed == null) { return; }
    _lastHuman = _clock(); _age.reset(); _age.start(); _schedule();
  }
  /// Copy is allowed for this exact completed presentation even after its own
  /// authorizing session retired. It does not count as human activity or renew.
  bool canCopyCompleted(MfaCompletedOperation expected) {
    if (_disposed || !identical(inbox.completed, expected) || !expected.owner.current) { return false; }
    if (!checkPrivacyIdle()) { return false; }
    return identical(inbox.completed, expected) && expected.owner.current && expected.codes.isNotEmpty;
  }
  /// Lifecycle observation may expire/discard; it never refreshes a deadline.
  bool checkPrivacyIdle() {
    if (_disposed) { return false; }
    final before = _lastHuman;
    if (before == null || inbox.completed == null) { return false; }
    final elapsed = _clock().difference(before);
    if (elapsed.isNegative || elapsed >= completionIdle || _age.elapsed >= completionIdle) {
      retire(); return false;
    }
    return true;
  }
  @override
  void dispose() {
    retire(); _disposed = true;
    inbox.removeListener(_completionChanged); inbox.dispose(); super.dispose();
  }
}

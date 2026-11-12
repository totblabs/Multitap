import 'dart:async';

import 'package:flutter/services.dart';

import 'clipboard_policy.dart';
import 'native_clipboard.dart';

/// A transfer that never started (another one was running, the channel was not
/// ready or closed): nothing was read or sent, so trying again is harmless.
abstract interface class ClipboardNotStarted {}

/// Why a change was seen but did not become a transfer (logged as clipboard
/// action 9, field `stage`; docs in ClipboardSession).
abstract final class ClipboardIgnored {
  static const inactive = 1,
      denied = 2,
      duplicate = 3,
      notStarted = 4,
      readAfterLeaving = 5,
      gaveUp = 6,
      superseded = 7;
}

/// Only sequence metadata is observed. Content capture remains serialized by
/// ClipboardSession and requires both an active view and the broker's policy.
class ClipboardSync {
  ClipboardSync({
    required this.owner,
    required this.bridge,
    required this.policy,
    required this.sendLocal,
    required this.receiveRemote,
    required this.watchRemote,
    required this.waitIdle,
    this.waitRemoteIdle,
    required this.onFailure,
    this.receiveSettledRemote,
    this.receiveObservedRemote,
    this.armRemote,
    this.onLocalCopy,
    this.onLocalChange,
    this.remoteQuiet = const Duration(milliseconds: 300),
    this.remoteDebounced = false,
    this.onIgnored,
    this.onRemoteDelay,
    this.cancelOlder,
    this.grace = const Duration(seconds: 2),
    this.observeInitialLocalOnly = false,
    this.initializationBackoffs = const [],
    this.retryRemoteInitialization,
    this.onInitializationRecovered,
    this.onInitializationReady,
    int? receivedRemoteSequence,
  }) {
    if (receivedRemoteSequence != null && _sequence(receivedRemoteSequence)) {
      _receivedRemote = receivedRemoteSequence;
    }
  }

  /// After an uncertain commit, the current native watch snapshot is a
  /// baseline, not a new copy. Do not replay it under a fresh request ID.
  final bool observeInitialLocalOnly;
  /// Opt-in metadata initialization only. Legacy/fake/Android callers default off.
  final List<Duration> initializationBackoffs;
  final Future<bool> Function()? retryRemoteInitialization;
  final void Function(Object failureToken)? onInitializationRecovered;
  final void Function()? onInitializationReady;
  Timer? _initializationTimer;
  Future<void>? _initializationRetry;
  bool _localWatchReady = false, _remoteArmReady = false;
  bool _initializationFailed = false, _initializationObserving = false;
  int _initializationRetries = 0, _initializationStage = 0;
  Object? _initializationFailureToken;
  int get initializationFailureStage => _initializationStage;
  /// Why the last initialization step failed, as a fixed number for the
  /// diagnostics (see [initializationFailureCause]); 0 when unknown.
  int get initializationFailureCause => _initializationCause;
  int _initializationCause = 0;

  /// 1 clipboard_changed, 2 clipboard_busy, 3 clipboard_transport_closed,
  /// 4 clipboard_not_active, 5 clipboard_transfer_failed, 6 other platform
  /// error, 7 missing plugin, 8 state error, 9 timeout, 10 anything else;
  /// busy with a named reason: 11 Windows sequence unavailable, 12 owner table
  /// full, 13 cancellation acknowledgement pending, 14 a zero sequence returned.
  static int failureCause(Object error) {
    if (error is PlatformException) {
      return switch (error.code) {
        'clipboard_changed' => 1,
        // The busy reason the native side names; a busy without one is 2.
        'clipboard_busy' => switch (error.message) {
          'clipboard_sequence_unavailable' => 11,
          'clipboard_owners_full' => 12,
          'clipboard_cancel_pending' => 13,
          'clipboard_sequence_zero' => 14,
          _ => 2,
        },
        'clipboard_transport_closed' => 3,
        'clipboard_not_active' => 4,
        'clipboard_transfer_failed' => 5,
        _ => 6,
      };
    }
    if (error is MissingPluginException) return 7;
    if (error is StateError) return 8;
    if (error is TimeoutException) return 9;
    return 10;
  }
  Object? get initializationFailureToken => _initializationFailureToken;
  int get initializationGeneration => _generation;
  bool get initializationReady => active &&
      (!policy.toRemote || _localWatchReady) &&
      (!policy.toLocal || _remoteArmReady);
  bool get _initializationRecovery => initializationBackoffs.isNotEmpty;

  void _initializationFailure(int stage, [Object? error]) {
    _initializationCause = error == null ? 0 : failureCause(error);
    if (_initializationRecovery) {
      _initializationFailed = true;
      _initializationStage = stage;
      _initializationFailureToken = Object();
      _scheduleInitializationRetry();
    }
    onFailure();
  }

  void _scheduleInitializationRetry() {
    if (!_initializationRecovery || !active || !_initializationFailed ||
        _initializationTimer != null || _initializationRetry != null ||
        _initializationRetries >= initializationBackoffs.length) {
      return;
    }
    final generation = _generation;
    _initializationTimer = Timer(initializationBackoffs[_initializationRetries], () {
      _initializationTimer = null;
      if (active && generation == _generation) unawaited(retryInitialization());
    });
  }

  /// Explicit accepted recapture consumes the SAME per-generation retry budget.
  /// Ordinary setActive(true) remains idempotent, including failure notifications.
  Future<void> retryInitialization() {
    if (!_initializationRecovery || !active || !_initializationFailed ||
        _initializationRetry != null ||
        _initializationRetries >= initializationBackoffs.length) {
      return _initializationRetry ?? Future<void>.value();
    }
    _initializationTimer?.cancel(); _initializationTimer = null;
    final generation = _generation;
    _initializationRetries++;
    final work = _watchTail.catchError((Object _) {}).then((_) async {
      if (!active || generation != _generation || !_initializationFailed) return;
      var stage = 1;
      try {
        if (!_localWatchReady && policy.toRemote) {
          _initializationObserving = true;
          _localDuringBaseline = null;
          try {
            final found = await _watchOwned();
            if (!active || generation != _generation) return;
            if (!_sequence(found.sequence)) throw StateError('clipboard_watch_sequence');
            // Recovery observes a baseline; it NEVER replays current contents.
            _localBaseline = _sentLocal = found.sequence;
            _localBaselineReady = true;
            _local = null; _localDuringBaseline = null;
            if (found.own) appliedLocal(found.sequence);
            _localWatchReady = true;
          } finally {
            // A new copy AFTER this baseline belongs to the existing live
            // change path, even while remote-arm/drain completion is pending.
            _initializationObserving = false;
          }
        }
        if (!active || generation != _generation) return;
        stage = 2;
        if (!_remoteArmReady && policy.toLocal) {
          final retry = retryRemoteInitialization;
          // A real pending arm needs its exact unadmitted initialization proof.
          if (retry == null || !await retry()) return;
          if (!active || generation != _generation) return;
          _remoteArmReady = true;
          watchRemote(true);
        }
        if (!active || generation != _generation) return;
        if ((!policy.toRemote || _localWatchReady) &&
            (!policy.toLocal || _remoteArmReady)) {
          _initializationFailed = false;
          final token = _initializationFailureToken;
          _initializationFailureToken = null;
          if (token != null) {
            try { onInitializationRecovered?.call(token); } catch (_) {}
          }
        }
      } catch (error) {
        if (active && generation == _generation) _initializationFailure(stage, error);
      } finally { _initializationObserving = false; }
    });
    final tracked = work.whenComplete(() {
      _initializationRetry = null;
      _scheduleInitializationRetry();
    });
    _initializationRetry = tracked;
    _watchTail = tracked;
    return tracked;
  }
  bool _localBaselineReady = false;
  int? _localBaseline, _localDuringBaseline;
  int? get receivedRemoteSequence => _receivedRemote;

  /// A stale notification can arrive after the watch result. Check the actual
  /// native capture sequence too, before the session sends any manifest/data.
  bool allowsAutomaticLocal(int? sequence) =>
      !observeInitialLocalOnly ||
      (_localBaselineReady &&
          sequence != null &&
          _sequence(sequence) &&
          sequence != _localBaseline);

  /// A newer copy (either side) replaces whatever copy is still crossing: the
  /// session cancels its running automatic transfer so the newer one starts
  /// at once instead of waiting behind (for a 64 MiB text or a big folder,
  /// minutes). Not called for an equal or already handled sequence.
  final void Function()? cancelOlder;

  /// Leaving the remote window does not cut off a copy made just before: for
  /// this long the remote watch stays on and a VM copy announced meanwhile is
  /// still read (the user copied while here, then moved to the PC to paste).
  /// Anything copied after it is not pulled: the focus gate is otherwise intact.
  final Duration grace;
  Timer? _grace;
  bool get _receiving => active || _grace != null;

  /// A change that was announced and deliberately not turned into a transfer
  /// (a [ClipboardIgnored] reason; `remote` is the direction): diagnostics only.
  final void Function(int reason, bool remote)? onIgnored;

  /// Local time from an accepted remote announcement until its read starts.
  /// Includes debounce/idle waiting, never contents or remote clock values.
  final void Function(int elapsedMs)? onRemoteDelay;
  Stopwatch? _remotePending;
  static const _retryLimit = 3;
  int _remoteRetries = 0;
  bool _closing = false;

  /// A copy in the VM can announce several changes in a row (the application
  /// publishing in steps, delayed rendering, other listeners). Read the remote
  /// clipboard once, after it has been quiet this long, so an intermediate
  /// state never becomes this PC's clipboard. Released Hosts announce every
  /// step; newer Hosts already wait for quiet themselves.
  final Duration remoteQuiet;

  /// The peer announces a change only after its clipboard has been quiet for
  /// a while (Hosts of the settled protocol: 250 ms, every kind of copy), so a
  /// fresh announcement is read at once. A retry still waits [remoteQuiet].
  final bool remoteDebounced;
  bool _remoteFast = false;
  final String owner;
  final ClipboardObservationBridge bridge;
  final ClipboardPolicy policy;
  final Future<void> Function() sendLocal, receiveRemote, waitIdle;
  final Future<void> Function(ClipboardRemoteCandidate)? waitRemoteIdle;
  final Future<void> Function(int expectedSequence)? receiveSettledRemote;
  final Future<void> Function(int expectedSequence, bool settled)?
  receiveObservedRemote;
  final Future<void> Function()? armRemote;
  final void Function()? onLocalCopy;

  /// Before [onLocalCopy], with how the change was seen (see _changedLocal):
  /// diagnostics only.
  final void Function(int via)? onLocalChange;
  final void Function(bool) watchRemote;
  final void Function() onFailure;
  StreamSubscription<ClipboardChange>? _subscription;
  Future<void> _watchTail = Future.value();
  Future<void> _subscriptionTail = Future<void>.value();
  Future<void>? _closeFuture;
  Future<void>? _draining;
  Timer? _debounce;
  bool _active = false, _closed = false;
  bool _remoteSettled = false;
  // Readiness belongs to the latest observation, including time spent behind
  // another transfer. A same-intent refusal starts a fresh bounded retry wait.
  bool _quietReady = false;
  int _generation = 0;
  int _intent = 0;
  int? _local,
      _remote,
      _sentLocal,
      _confirmedLocal,
      _receivedRemote,
      _appliedLocal;
  bool get active => _active && !_closed;

  Future<void> setActive(bool value) {
    if (_closed || _active == value) return _watchTail;
    _active = value;
    final generation = ++_generation;
    _debounce?.cancel();
    // Back within the grace: the remote watch never went off.
    final resumed = value && _grace != null;
    final remoteWasReady = _remoteArmReady;
    if (_initializationRecovery) {
      _initializationTimer?.cancel(); _initializationTimer = null;
      _initializationFailed = false; _initializationFailureToken = null;
      _localWatchReady = !policy.toRemote;
      if (value) {
        _initializationRetries = 0;
        if (!resumed) _remoteArmReady = !policy.toLocal;
      }
    }
    if (value) {
      _grace?.cancel();
      _grace = null;
    } else {
      _local = null;
      _localDuringBaseline = null;
      _cancelSubscription();
      if (policy.toLocal && !_closing) {
        // Keep the remote watch on for the grace; _endGrace turns it off.
        _grace?.cancel();
        _grace = Timer(grace, _endGrace);
        if (_remote != null) _schedule();
      } else {
        _remote = null;
        _remoteSettled = _remoteFast = false;
        watchRemote(false);
      }
    }
    return _watchTail = _watchTail.catchError((Object _) {}).then((_) async {
      if (_closed && value) return;
      var initializationStage = 1;
      try {
        if (value && generation != _generation) return;
        if (value) {
          _subscription ??= bridge.changes.listen((event) {
            if (active &&
                generation == _generation &&
                event.owner == owner &&
                policy.toRemote) {
              if (_initializationObserving ||
                  (observeInitialLocalOnly && !_localBaselineReady)) {
                if (_sequence(event.sequence)) {
                  _localDuringBaseline = event.sequence;
                }
              } else {
                _changedLocal(event.sequence, via: 1);
              }
            }
          });
        }
        final watching = value && policy.toRemote;
        final found = watching
            ? await _watchOwned()
            : (sequence: await bridge.watch(owner, false), own: false);
        final sequence = found.sequence;
        if (!active || generation != _generation) return;
        _localWatchReady = true;
        var copiedHere = false;
        if (policy.toRemote) {
          if (observeInitialLocalOnly && !_localBaselineReady) {
            if (!_sequence(sequence)) {
              _initializationFailure(1);
              return;
            }
            _localBaseline = _sentLocal = sequence;
            _localBaselineReady = true;
            final changed = _localDuringBaseline;
            _localDuringBaseline = null;
            if (changed != null && changed != sequence) {
              _changedLocal(changed, via: 1);
            }
            copiedHere = true;
          } else if (found.own) {
            // Our own publication (the VM's copy, or the placeholder that is
            // waiting for it): treating it as a PC copy would cancel the VM's
            // copy still arriving and read the placeholder back.
            appliedLocal(sequence);
          } else {
            copiedHere = _changedLocal(sequence, via: 2);
          }
        }
        // A new watch epoch cancels the VM copy still arriving. Back within
        // the grace with nothing copied on this PC meanwhile (known only when
        // this PC's clipboard is observed), the running watch and its anchor
        // are still right: keep them.
        if (policy.toLocal &&
            !(resumed && policy.toRemote && !copiedHere &&
                (!_initializationRecovery || remoteWasReady))) {
          initializationStage = 2;
          await armRemote?.call();
          if (!active || generation != _generation) return;
          _remoteArmReady = true;
          watchRemote(true);
        }
        if (_initializationRecovery && active &&
            generation == _generation && initializationReady) {
          try { onInitializationReady?.call(); } catch (_) {}
        }
      } catch (error) {
        if (active && generation == _generation) {
          _initializationFailure(initializationStage, error);
        }
      }
    });
  }

  void _endGrace() {
    _grace = null;
    if (active || _closed) return;
    _debounce?.cancel();
    _remote = null;
    _remoteArmReady = false;
    _remoteSettled = _remoteFast = false;
    watchRemote(false);
  }

  Future<({int sequence, bool own})> _watchOwned() async {
    final owned = bridge;
    if (owned is ClipboardWatchOwnership) {
      return (owned as ClipboardWatchOwnership).watchOwned(owner);
    }
    return (sequence: await bridge.watch(owner, true), own: false);
  }

  /// [via]: 1 a change notification, 2 the clipboard as found when the window
  /// became active again, 3 the same before a paste into the VM. Returns
  /// whether it was taken as a copy made here.
  bool _changedLocal(int sequence, {required int via}) {
    if (!_sequence(sequence) ||
        sequence == _appliedLocal ||
        sequence == _sentLocal) {
      return false;
    }
    // The latest valid observation replaces a pending copy in the opposite
    // direction. Independent Windows sequence counters are never compared.
    _intent++;
    onLocalChange?.call(via);
    onLocalCopy?.call();
    _remote = null;
    _remoteSettled = _remoteFast = false;
    _remotePending = null;
    _remoteRetries = 0;
    _local = sequence;
    cancelOlder?.call();
    _schedule();
    return true;
  }

  void remoteChanged(int sequence, {bool settled = false}) {
    if (!_sequence(sequence) || _closed) return;
    if (!policy.toLocal) {
      onIgnored?.call(ClipboardIgnored.denied, true);
      return;
    }
    if (!_receiving) {
      onIgnored?.call(ClipboardIgnored.inactive, true);
      return;
    }
    if (sequence == _receivedRemote) {
      onIgnored?.call(ClipboardIgnored.duplicate, true);
      return;
    }
    // Announced within the grace after leaving the window: accepted.
    if (!active) onIgnored?.call(ClipboardIgnored.readAfterLeaving, true);
    _intent++;
    _local = null;
    _localDuringBaseline = null;
    if (_remote != sequence) _remoteRetries = 0;
    _remote = sequence;
    _remoteSettled =
        settled &&
        (receiveSettledRemote != null || receiveObservedRemote != null);
    _remoteFast = remoteDebounced && _remoteRetries == 0;
    _remotePending = Stopwatch()..start();
    cancelOlder?.call();
    _schedule();
  }

  /// A negotiated copy intent invalidates older automatic work without reading
  /// source data or starting a transfer before its settled notification.
  void remoteIntent(int sequence) {
    if (!_sequence(sequence) || _closed || !policy.toLocal || !_receiving) {
      return;
    }
    _intent++;
    _remote = null;
    _remoteSettled = _remoteFast = false;
    _remotePending = null;
    _debounce?.cancel();
    _quietReady = false;
    if (_receivedRemote == sequence) _receivedRemote = null;
    cancelOlder?.call();
  }

  void capturedLocal(int? sequence) {
    if (sequence != null && _sequence(sequence)) _sentLocal = sequence;
  }

  void confirmedLocal(int? sequence) {
    if (sequence != null && _sequence(sequence)) _confirmedLocal = sequence;
  }

  void appliedLocal(int? sequence) {
    if (sequence == null || !_sequence(sequence)) return;
    _appliedLocal = sequence;
    if (_local == sequence) _local = null;
  }

  static bool _sequence(int value) => value >= 0 && value <= 0xffffffff;

  void _schedule({bool alreadyQuiet = false}) {
    if (!_receiving) return;
    _debounce?.cancel();
    _quietReady = false;
    // Every remote announcement restarts the wait: read once it is quiet.
    _debounce = Timer(
      alreadyQuiet
          ? Duration.zero
          : _remote != null
          ? (_remoteSettled || _remoteFast ? Duration.zero : remoteQuiet)
          : const Duration(milliseconds: 80),
      () {
        _quietReady = true;
        unawaited(_drain());
      },
    );
  }

  Future<void> _drain() {
    if (!_receiving) return Future.value();
    return _draining ??= _run().whenComplete(() {
      _draining = null;
      if (_receiving &&
          (_local != null || _remote != null) &&
          !(_debounce?.isActive ?? false)) {
        _schedule(alreadyQuiet: _quietReady);
      }
    });
  }

  Future<void> _run() async {
    final generation = _generation;
    try {
      final candidate = _local == null && _remote != null
          ? ClipboardRemoteCandidate(_remote!, _intent, generation)
          : null;
      if (candidate != null && waitRemoteIdle != null) {
        await waitRemoteIdle!(candidate);
        if (!acceptsRemoteCandidate(candidate)) return;
      } else {
        await waitIdle();
      }
      // A newer observation can replace this drain while it waits for idle.
      // Its timer, rather than the old drain, decides when it is safe to read.
      // preparePaste deliberately cancels that timer before its explicit drain.
      if (_debounce?.isActive ?? false) return;
      final here = active && generation == _generation;
      if (!here && _grace == null) return;
      _quietReady = false;
      final local = here ? _local : null;
      if (local != null) {
        _local = null;
        if (local == _appliedLocal || local == _sentLocal) return;
        _sentLocal = local;
        await sendLocal();
      } else if (_remote != null) {
        final remote = _remote!;
        final settled = _remoteSettled;
        _remoteSettled = _remoteFast = false;
        final intent = _intent;
        _remote = null;
        if (remote == _receivedRemote) return;
        final pending = _remotePending;
        _remotePending = null;
        if (pending != null) {
          try {
            onRemoteDelay?.call(pending.elapsedMilliseconds.clamp(0, 86400000));
          } catch (_) {
            // Observational callbacks never discard an approved copy.
          }
        }
        if (_closed || intent != _intent) return;
        _receivedRemote = remote;
        try {
          if (receiveObservedRemote != null) {
            await receiveObservedRemote!(remote, settled);
          } else if (settled && receiveSettledRemote != null) {
            await receiveSettledRemote!(remote);
          } else {
            await receiveRemote();
          }
          if (intent == _intent) _remoteRetries = 0;
        } on ClipboardNotStarted {
          // A late result from a superseded copy cannot replay that copy or
          // consume the retry budget of a newer observation.
          if (_closed || intent != _intent) return;
          // Nothing was read: the same announcement is tried again shortly
          // (a few times), instead of being forgotten without a trace.
          _receivedRemote = null;
          if (_receiving && _remoteRetries < _retryLimit) {
            _remoteRetries++;
            _remote ??= remote;
            // A retry never spins on a stale fast hint.
            _remoteSettled = _remoteFast = false;
            _quietReady = false;
          } else {
            _remoteRetries = 0;
            onIgnored?.call(ClipboardIgnored.gaveUp, true);
          }
          rethrow;
        }
      }
    } catch (_) {
      // Transfer status retains the actionable error. No unhandled background
      // exception may terminate a video session or retry content indefinitely.
    }
  }

  bool acceptsRemoteCandidate(ClipboardRemoteCandidate candidate) =>
      !_closed &&
      _receiving &&
      candidate.intent == _intent &&
      _local == null &&
      _remote == candidate.sequence &&
      ((active && candidate.generation == _generation) ||
          (!active && _grace != null));

  /// Paste must follow the approved copy's completion, including file commit.
  /// Callers bound the wait and discard input when focus/session ownership ends.
  Future<bool> preparePaste() async {
    if (!active || !policy.toRemote) return true;
    final generation = _generation;
    for (var n = 0; n < 4; n++) {
      await waitIdle();
      if (!active || generation != _generation) return false;
      final found = await _watchOwned();
      final sequence = found.sequence;
      if (!active || generation != _generation) return false;
      if (sequence == _appliedLocal) return true;
      if (sequence == _confirmedLocal) return true;
      // Our own publication of the VM's copy (a paste in the VM right after
      // copying there): nothing on this PC to send first.
      if (found.own) {
        appliedLocal(sequence);
        return true;
      }
      if (sequence == _sentLocal && _draining == null) return false;
      _changedLocal(sequence, via: 3);
      _debounce?.cancel();
      await _drain();
      if (!active || generation != _generation) return false;
    }
    return false;
  }

  void _cancelSubscription() {
    final subscription = _subscription;
    _subscription = null;
    if (subscription == null) return;
    _subscriptionTail = Future.wait<void>([
      _subscriptionTail,
      Future<void>.sync(subscription.cancel),
    ]).then<void>((_) {});
    // Close still observes the failure; background focus changes must not
    // create an unhandled asynchronous cancellation error.
    _subscriptionTail.ignore();
  }

  Future<void> close() => _closeFuture ??= _close();
  Future<void> _close() async {
    if (_closed) return;
    _closing = true;
    _grace?.cancel();
    _grace = null;
    final stopped = setActive(false);
    _closed = true;
    _generation++;
    _debounce?.cancel();
    try {
      await stopped;
    } finally {
      _cancelSubscription();
      await _subscriptionTail;
    }
  }
}

/// One remote candidate, before any request or publication is started.
class ClipboardRemoteCandidate {
  const ClipboardRemoteCandidate(this.sequence, this.intent, this.generation);
  final int sequence, intent, generation;
}

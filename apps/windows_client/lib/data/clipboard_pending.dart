import 'native_clipboard.dart';

/// Local ownership metadata only; never sent over the relay or logged.
class ClipboardPendingAdmission {
  const ClipboardPendingAdmission({
    required this.owner,
    required this.sourceSequence,
    required this.epoch,
    required this.revision,
    required this.lease,
    Object? bindingHead,
    // Preserve the public argument name while keeping the identity token private.
    // ignore: prefer_initializing_formals
  }) : _bindingHead = bindingHead;
  final String owner;
  final int sourceSequence, epoch, revision;
  final ClipboardPendingLease? lease;
  final Object? _bindingHead;
}

class ClipboardPendingBinding {
  const ClipboardPendingBinding._(
    this.admission,
    this.requestId,
    this.lease,
    this._head,
  );
  final ClipboardPendingAdmission admission;
  final String requestId;
  final ClipboardPendingLease lease;
  final Object _head;
}

/// Receiver metadata only. The anchor predates the remote watch; arrival of a
/// delayed remote hint never adopts the current local clipboard as its anchor.
/// Opaque proof of a failed, unadmitted initialization. Never sent or logged.
class ClipboardPendingInitialization {
  ClipboardPendingInitialization._(this.epoch, this.revision);
  final int epoch, revision;
}

class ClipboardPending {
  ClipboardPending(this.owner, this.bridge);
  final String owner;
  final ClipboardPendingBridge bridge;
  int _epoch = 0, _revision = 0;
  int? _anchor, _source, _refused;
  bool _active = false, _closed = false;
  ClipboardPendingLease? _lease;
  Object? _bindingHead;
  ClipboardPendingAdmission? _bindingAdmission;
  Future<void> _sourceTail = Future<void>.value();
  final _activities = <Future<void>>{};
  Future<void>? _closing;
  ClipboardPendingInitialization? _failedInitialization;
  ClipboardPendingInitialization? _recoveredInitialization;
  ClipboardPendingInitialization? _virginInitialization;
  /// Distinct proof: local watch failed before this owner ever attempted arm.
  ClipboardPendingInitialization? get virginInitialization {
    if (_closed || _epoch != 0 || _activities.isNotEmpty || _anchor != null ||
        _source != null || _lease != null || _bindingHead != null ||
        _bindingAdmission != null) {
      return null;
    }
    // Focus/grace end can advance revision without ever attempting native arm.
    final previous = _virginInitialization;
    if (previous == null || previous.revision != _revision) {
      _virginInitialization = ClipboardPendingInitialization._(0, _revision);
    }
    return _virginInitialization;
  }
  ClipboardPendingInitialization? get failedInitialization {
    final token = _failedInitialization;
    return token != null && initializationCurrent(token) ? token : null;
  }
  bool initializationCurrent(ClipboardPendingInitialization token) =>
      !_closed &&
      (identical(token, _failedInitialization) ||
          (identical(token, _virginInitialization) && _epoch == 0)) &&
      token.epoch == _epoch && token.revision == _revision &&
      _anchor == null && _source == null && _lease == null &&
      _bindingHead == null && _bindingAdmission == null;
  bool initializationRecoveredFrom(ClipboardPendingInitialization token) =>
      identical(token, _recoveredInitialization) && active &&
      _epoch == token.epoch + 1 && _revision == token.revision + 1 &&
      _source == null && _lease == null && _bindingHead == null &&
      _bindingAdmission == null;

  /// Join only work that predates this retry, then recheck the exact proof.
  /// A failed owner cancellation never produces such a proof.
  Future<bool> retryInitialization(ClipboardPendingInitialization token) {
    final activities = _activities.toList(), source = _sourceTail;
    return _activity(() async {
      if (!initializationCurrent(token)) return false;
      await Future.wait([source, ...activities]);
      if (!initializationCurrent(token)) return false;
      final epoch = _epoch + 1, revision = _revision + 1;
      await _arm();
      if (!active || _epoch != epoch || _revision != revision) return false;
      _recoveredInitialization = token;
      return initializationRecoveredFrom(token);
    }());
  }
  int get epoch => _epoch;
  bool get active => _active && !_closed && _anchor != null;
  int? get publicationSequence => _lease?.sequence;
  bool accepts(int value) => active && value == _epoch;

  /// A VM copy was announced and is not refused: its data may still arrive.
  bool get arriving => active && _source != null && _refused != _source;
  bool refused(int sequence) => _refused == sequence;
  static bool _sequence(int n) => n >= 0 && n <= 0xffffffff;

  Future<T> _activity<T>(Future<T> work) {
    final settled = work.then<void>(
      (_) {},
      onError: (Object _, StackTrace _) {},
    );
    _activities.add(settled);
    settled.whenComplete(() => _activities.remove(settled));
    return work;
  }

  static bool _sameLease(ClipboardPendingLease a, ClipboardPendingLease b) =>
      a.token == b.token &&
      a.sequence == b.sequence &&
      a.sourceSequence == b.sourceSequence &&
      a.kind == b.kind &&
      a.epoch == b.epoch;

  ClipboardPendingAdmission? admissionFor(int sequence) {
    if (!active || _source != sequence || _refused == sequence) return null;
    return ClipboardPendingAdmission(
      owner: owner,
      sourceSequence: sequence,
      epoch: _epoch,
      revision: _revision,
      lease: _lease,
      bindingHead: _bindingHead,
    );
  }

  bool acceptsAdmission(
    ClipboardPendingAdmission admission, {
    bool requireLease = false,
  }) =>
      active &&
      admission.owner == owner &&
      admission.epoch == _epoch &&
      admission.revision == _revision &&
      admission.sourceSequence == _source &&
      _refused != admission.sourceSequence &&
      (!requireLease ||
          (admission.lease != null &&
              _lease != null &&
              _sameLease(admission.lease!, _lease!)));

  Future<ClipboardPendingAdmission?> settledAdmission(int sequence) =>
      _activity(_settledAdmission(sequence));

  Future<ClipboardPendingAdmission?> _settledAdmission(int sequence) async {
    final admission = admissionFor(sequence);
    if (admission == null) return null;
    await _sourceTail;
    return acceptsAdmission(admission) ? admissionFor(sequence) : null;
  }

  bool acceptsBinding(ClipboardPendingBinding binding) =>
      acceptsAdmission(binding.admission) &&
      identical(binding._head, _bindingHead) &&
      _lease != null &&
      _sameLease(binding.lease, _lease!);

  Future<void> arm() => _activity(_arm());
  Future<void> _arm() async {
    if (_closed || _epoch == 0xffffffff) {
      throw StateError('clipboard_pending_closed');
    }
    final old = _epoch;
    final epoch = ++_epoch, revision = ++_revision;
    _failedInitialization = null;
    _active = true;
    _anchor = null;
    _source = null;
    _refused = null;
    _lease = null;
    _bindingHead = null;
    _bindingAdmission = null;
    var oldCancellationAcknowledged = old == 0;
    try {
      if (old > 0) await bridge.cancelPendingOwner(owner, old);
      oldCancellationAcknowledged = true;
      if (_closed || !_active || epoch != _epoch || revision != _revision) return;
      final sequence = await bridge.observePendingSequence(owner, epoch: epoch);
      if (_closed || !_active || epoch != _epoch || revision != _revision) {
        await bridge.cancelPendingOwner(owner, epoch);
        return;
      }
      if (!_sequence(sequence)) throw StateError('clipboard_pending_sequence');
      _anchor = sequence;
    } catch (_) {
      if (oldCancellationAcknowledged && !_closed && _active &&
          epoch == _epoch && revision == _revision && _anchor == null &&
          _source == null && _lease == null && _bindingHead == null &&
          _bindingAdmission == null) {
        _failedInitialization = ClipboardPendingInitialization._(epoch, revision);
      }
      rethrow;
    }
  }

  Future<void> source(int sequence, String kind, int epoch) {
    if (!accepts(epoch) || !_sequence(sequence)) return Future<void>.value();
    final revision = ++_revision;
    _bindingHead = null;
    _bindingAdmission = null;
    _source = sequence;
    _refused = null;
    final work = _sourceTail.catchError((Object _) {}).then((_) async {
      if (!accepts(epoch) || revision != _revision) return;
      final old = _lease;
      _lease = null;
      if (old != null) await bridge.cancelPending(owner, old);
      if (kind == 'other') return;
      if (!{'text', 'richtext', 'image'}.contains(kind)) {
        throw StateError('clipboard_pending_kind');
      }
      final anchor = _anchor;
      if (anchor == null || !accepts(epoch) || revision != _revision) return;
      final lease = await bridge.beginPending(
        owner,
        expectedLocalSequence: anchor,
        sourceSequence: sequence,
        kind: kind,
        epoch: epoch,
      );
      if (lease == null) {
        if (revision == _revision) _refused = sequence;
        return;
      }
      final valid =
          lease.sourceSequence == sequence &&
          lease.kind == kind &&
          lease.epoch == epoch &&
          _sequence(lease.sequence);
      // This is a known own publication, not a fresh sample of arbitrary PC
      // contents. Preserve its anchor chain even when B superseded this A.
      if (valid && accepts(epoch)) _anchor = lease.sequence;
      if (!valid ||
          !accepts(epoch) ||
          revision != _revision ||
          _source != sequence) {
        await bridge.cancelPending(owner, lease);
        if (revision == _revision) _refused = sequence;
        return;
      }
      _lease = lease;
    });
    _sourceTail = work.catchError((Object _) {});
    return _activity(work);
  }

  /// false refuses publication, rather than falling through to a publisher
  /// which would sample and overwrite a newer local copy made during transit.
  Future<bool> bind(int sequence, String requestId, String format) async {
    await _sourceTail;
    final admission = admissionFor(sequence);
    if (admission == null) return false;
    return await bindAdmission(admission, requestId, format) != null;
  }

  Future<ClipboardPendingBinding?> bindAdmission(
    ClipboardPendingAdmission admission,
    String requestId,
    String format,
  ) => _activity(_bindAdmission(admission, requestId, format));

  Future<ClipboardPendingBinding?> _bindAdmission(
    ClipboardPendingAdmission admission,
    String requestId,
    String format,
  ) async {
    await _sourceTail;
    if (!acceptsAdmission(admission)) return null;
    // Native leases bind once. A rejected second request must not supersede
    // the first request's in-flight or acknowledged binding.
    if (_bindingHead != null) return null;
    final head = Object();
    _bindingHead = head;
    _bindingAdmission = admission;
    var retained = false;
    try {
      final sequence = admission.sourceSequence;
      var lease = _lease;
      final revision = _revision, epoch = _epoch;
      if (lease == null &&
          {'text', 'richtext', 'image'}.contains(format) &&
          _anchor != null) {
        lease = await bridge.beginPending(
          owner,
          expectedLocalSequence: _anchor!,
          sourceSequence: sequence,
          kind: format,
          epoch: epoch,
        );
        if (lease == null) {
          if (acceptsAdmission(admission)) _refused = sequence;
          return null;
        }
        if (!active ||
            !identical(_bindingHead, head) ||
            revision != _revision ||
            lease.epoch != epoch ||
            lease.sourceSequence != sequence ||
            lease.kind != format ||
            !_sequence(lease.sequence)) {
          await bridge.cancelPending(owner, lease);
          return null;
        }
        _lease = lease;
        _anchor = lease.sequence;
      }
      if (lease == null ||
          lease.kind != format ||
          !identical(_bindingHead, head)) {
        return null;
      }
      final accepted = await bridge.bindPending(owner, lease, requestId);
      final binding = ClipboardPendingBinding._(
        admission,
        requestId,
        lease,
        head,
      );
      if (!accepted || !acceptsBinding(binding)) {
        if (accepted &&
            (identical(_bindingHead, head) ||
                _lease == null ||
                !_sameLease(lease, _lease!))) {
          await bridge.cancelPending(owner, lease);
        }
        return null;
      }
      retained = true;
      return binding;
    } finally {
      if (!retained && identical(_bindingHead, head)) {
        _bindingHead = null;
        _bindingAdmission = null;
      }
    }
  }

  /// A genuine local observation never advances the remote publication anchor.
  Future<void> localCopy() => _activity(_localCopy());
  Future<void> _localCopy() async {
    ++_revision;
    _bindingHead = null;
    _bindingAdmission = null;
    _refused = _source;
    final lease = _lease;
    _lease = null;
    if (lease != null) await bridge.cancelPending(owner, lease);
  }

  void published(int? sequence) {
    if (active && sequence != null && _sequence(sequence)) _anchor = sequence;
  }

  void publishedBinding(ClipboardPendingBinding binding, int? sequence) {
    if (acceptsBinding(binding)) published(sequence);
  }

  Future<void> failedBinding(ClipboardPendingBinding binding) =>
      _activity(_failedBinding(binding));

  Future<void> _failedBinding(ClipboardPendingBinding binding) async {
    if (!acceptsBinding(binding)) return;
    ++_revision;
    _refused = binding.admission.sourceSequence;
    _lease = null;
    _bindingHead = null;
    _bindingAdmission = null;
    await bridge.cancelPending(owner, binding.lease);
  }

  Future<void> failedAdmission(ClipboardPendingAdmission admission) =>
      _activity(_failedAdmission(admission, admission.lease));

  Future<void> _failedAdmission(
    ClipboardPendingAdmission admission,
    ClipboardPendingLease? exactLease,
  ) async {
    if (!identical(admission._bindingHead, _bindingHead) &&
        !identical(admission, _bindingAdmission)) {
      return;
    }
    var lease = exactLease;
    if (acceptsAdmission(admission)) {
      lease ??= _lease;
      ++_revision;
      _refused = admission.sourceSequence;
      _lease = null;
      _bindingHead = null;
      _bindingAdmission = null;
    }
    if (lease != null) await bridge.cancelPending(owner, lease);
  }

  Future<void> failed(int? sourceSequence) =>
      _activity(_failed(sourceSequence));
  Future<void> _failed(int? sourceSequence) async {
    if (sourceSequence == null || sourceSequence != _source) return;
    ++_revision;
    _bindingHead = null;
    _bindingAdmission = null;
    _refused = sourceSequence;
    final lease = _lease;
    _lease = null;
    if (lease != null) await bridge.cancelPending(owner, lease);
  }

  // Session closes pending before Sync stops its watch. A late watch-off must
  // join that same close, not enqueue another native barrier after it ended.
  Future<void> end() =>
      _closed ? (_closing ?? Future<void>.value()) : _activity(_end());
  Future<void> _end() async {
    _active = false;
    _bindingHead = null;
    _bindingAdmission = null;
    ++_revision;
    _anchor = null;
    _source = null;
    _refused = null;
    _lease = null;
    final epoch = _epoch;
    if (epoch > 0) await bridge.cancelPendingOwner(owner, epoch);
  }

  Future<void> close() => _closing ??= _close();
  Future<void> _close() async {
    _closed = true;
    // Invalidate first: Render must be released before joining old STA work.
    Object? failure;
    StackTrace? trace;
    try {
      await _end();
    } catch (error, stack) {
      failure = error;
      trace = stack;
    }
    await _sourceTail;
    while (_activities.isNotEmpty) {
      await Future.wait(_activities.toList());
    }
    if (failure != null) Error.throwWithStackTrace(failure, trace!);
  }
}

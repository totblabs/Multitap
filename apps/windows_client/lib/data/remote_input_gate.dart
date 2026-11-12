import 'dart:async';

/// Preserve the physical key order while a permitted clipboard copy commits.
/// No OS input is generated here; all events stay inside the authorized relay.
class RemoteInputGate {
  RemoteInputGate({
    required this.send,
    required this.preparePaste,
    required this.pasteEnabled,
    required this.onPasteBlocked,
    this.onPasteSettled,
    this.onBlockedEscape,
    this.pasteTimeout = const Duration(seconds: 30),
    this.observationEnabled,
    this.nowUs,
    this.onWaitObserved,
  });
  final void Function(Map<String, dynamic>) send;
  final Future<bool> Function() preparePaste;
  final bool Function() pasteEnabled;
  final void Function() onPasteBlocked;
  final void Function()? onPasteSettled, onBlockedEscape;
  final Duration pasteTimeout;
  final bool Function()? observationEnabled;
  final int Function()? nowUs;
  final void Function(int waitUs, int queuedPackets)? onWaitObserved;
  bool get _observing {
    try {
      return observationEnabled?.call() == true;
    } catch (_) {
      return false;
    }
  }

  int? _waitBeganUs;
  int get queuedPackets => _queue.length;
  int? get observedWaitUs {
    final began = _waitBeganUs;
    if (began == null || !_observing) return null;
    try {
      final now = nowUs?.call();
      return now != null && now >= began ? now - began : null;
    } catch (_) {
      return null;
    }
  }

  void _finishObservation() {
    final elapsed = observedWaitUs;
    _waitBeganUs = null;
    if (elapsed == null) return;
    try {
      onWaitObserved?.call(elapsed, _queue.length);
    } catch (_) {
      // An observer must never change paste ordering, reset or cancellation.
    }
  }

  final _modifiers = <int, Map<String, dynamic>>{};
  final _queue = <Map<String, dynamic>>[];
  int _generation = 0;
  bool _waiting = false;
  bool _clipboardBlocked = false;
  final _pasteChord = <Map<String, dynamic>>[];
  bool get preparingPaste => _waiting;

  /// Block new input without revoking the one paste already being prepared.
  /// A real ownership reset still always revokes that paste.
  void setClipboardBlocked(bool blocked) {
    if (_clipboardBlocked == blocked) return;
    _clipboardBlocked = blocked;
    if (blocked && _waiting) {
      _queue
        ..clear()
        ..addAll(_pasteChord.map((event) => Map<String, dynamic>.from(event)))
        ..add({'type': 'input.reset'});
    }
  }

  static const _modifierKeys = {
    0x10,
    0x11,
    0x12,
    0xa0,
    0xa1,
    0xa2,
    0xa3,
    0xa4,
    0xa5,
    0x5b,
    0x5c,
  };

  void reset() {
    _finishObservation();
    _generation++;
    _waiting = false;
    _queue.clear();
    _pasteChord.clear();
    _modifiers.clear();
  }

  void add(Map<String, dynamic> event) {
    if (event['type'] == 'input.reset') {
      reset();
      send(event);
      return;
    }
    if (_clipboardBlocked) {
      if (event['type'] == 'input' &&
          event['event'] == 'key' &&
          event['vk'] == 0x1b &&
          event['down'] == true) {
        reset();
        send({'type': 'input.reset'});
        onBlockedEscape?.call();
      }
      return;
    }
    final key = event['type'] == 'input' && event['event'] == 'key';
    final vk = event['vk'];
    if (key && vk is int && _modifierKeys.contains(vk)) {
      if (event['down'] == true) {
        _modifiers[vk] = Map.from(event);
      } else {
        _modifiers.remove(vk);
      }
    }
    if (_waiting) {
      if (_queue.length >= 128) {
        reset();
        send({'type': 'input.reset'});
        onPasteBlocked();
      } else {
        _queue.add(Map.from(event));
      }
      return;
    }
    final ctrl = _modifiers.keys.any(
      (vk) => const {0x11, 0xa2, 0xa3}.contains(vk),
    );
    final other = _modifiers.keys.any(
      (vk) => const {0x12, 0xa4, 0xa5, 0x5b, 0x5c}.contains(vk),
    );
    final shift = _modifiers.keys.any(
      (vk) => const {0x10, 0xa0, 0xa1}.contains(vk),
    );
    final pasteKey = (vk == 0x56 && ctrl) || (vk == 0x2d && shift && !ctrl);
    if (key && pasteKey && event['down'] == true && !other && pasteEnabled()) {
      _waiting = true;
      if (_observing) {
        try {
          _waitBeganUs = nowUs?.call();
        } catch (_) {}
      }
      final generation = ++_generation;
      // Release modifiers already sent before Ctrl+V or Shift+Insert. Restore
      // their physical events only after publication, then replay the sequence.
      send({'type': 'input.reset'});
      _queue.addAll(
        _modifiers.values.map((event) => Map<String, dynamic>.from(event)),
      );
      _queue.add(Map.from(event));
      _pasteChord
        ..clear()
        ..addAll(_queue.map((event) => Map<String, dynamic>.from(event)));
      unawaited(_finishPaste(generation));
    } else {
      send(event);
    }
  }

  Future<void> _finishPaste(int generation) async {
    try {
      await _prepareAndReplay(generation);
    } finally {
      onPasteSettled?.call();
    }
  }

  Future<void> _prepareAndReplay(int generation) async {
    var accepted = false;
    try {
      accepted = await preparePaste().timeout(pasteTimeout);
    } catch (_) {}
    if (generation != _generation || !_waiting) return;
    _finishObservation();
    _waiting = false;
    if (!accepted || !pasteEnabled()) {
      reset();
      send({'type': 'input.reset'});
      onPasteBlocked();
      return;
    }
    final pending = List<Map<String, dynamic>>.from(_queue);
    _queue.clear();
    _pasteChord.clear();
    for (final event in pending) {
      if (generation != _generation) return;
      send(event);
    }
    if (_clipboardBlocked) _modifiers.clear();
  }
}

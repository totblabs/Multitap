import 'dart:async';

// Released diagnostic collectors do not know these two pull-wire reasons.
// Keep the transport/UI reason intact and use an existing diagnostic code,
// with `error` telling them apart. Every other pull-wire reason is a
// diagnostic code already.
String? clipboardPullDiagnosticCode(String? code) => switch (code) {
  'clipboard_offer_expired' => 'clipboard_file_unavailable',
  'clipboard_selected_file_changed' => 'clipboard_changed',
  _ => code,
};
int clipboardPullDiagnosticDetail(String? code) => switch (code) {
  'clipboard_offer_expired' => 1,
  'clipboard_selected_file_changed' => 2,
  _ => 0,
};

/// Counts transport ranges only. These events never establish that Explorer
/// saved a file, and carry no content, names, paths, offer IDs or range IDs.
class ClipboardPullObservation {
  const ClipboardPullObservation({
    required this.sending,
    required this.stage,
    required this.requests,
    required this.failedRequests,
    required this.elapsedMs,
    this.code,
  });

  final bool sending;
  final int stage, requests, failedRequests, elapsedMs;
  final String? code;

  String? get diagnosticCode => clipboardPullDiagnosticCode(code);
  int get diagnosticDetail => clipboardPullDiagnosticDetail(code);
}

class _Episode {
  _Episode(this.generation, this.started) : lastGeneration = generation;
  final int generation;
  int lastGeneration;
  final DateTime started;
  final pending = <int>{};
  int requests = 0, completed = 0, failed = 0, windows = 0;
  bool firstData = false, terminal = false, cancelled = false;
  bool failureReported = false;
}

/// Observational grouping: adjacent ranges within two seconds belong to one
/// episode. Each has one start/first-data/terminal and at most twelve five-second
/// summaries. A hard budget also bounds many tiny, separated pastes per owner.
class ClipboardPullObserver {
  ClipboardPullObserver({
    required this.sending,
    required this.emit,
    DateTime Function()? now,
  }) : _now = now ?? DateTime.now;

  final bool sending;
  final void Function(ClipboardPullObservation) emit;
  final DateTime Function() _now;
  _Episode? _episode;
  Timer? _quiet, _periodic;
  int _generation = 0, _remaining = 64;
  bool _closed = false, _waiting = false;

  int request() {
    if (_closed) return 0;
    _quiet?.cancel();
    final generation = ++_generation;
    final fresh = _episode == null;
    final episode = _episode ??= _Episode(generation, _now());
    episode.lastGeneration = generation;
    episode.requests++;
    episode.pending.add(generation);
    if (fresh) {
      _record(episode, 1);
      _periodic = Timer.periodic(const Duration(seconds: 5), (_) {
        if (_closed || !identical(_episode, episode) || episode.terminal) {
          _periodic?.cancel();
          return;
        }
        if (episode.windows++ < 12) _record(episode, 3);
        if (episode.windows >= 12) _periodic?.cancel();
      });
    }
    return generation;
  }

  _Episode? _current(int generation) {
    final episode = _episode;
    return !_closed &&
            episode != null &&
            generation >= episode.generation &&
            generation <= episode.lastGeneration
        ? episode
        : null;
  }

  void data(int generation) {
    final episode = _current(generation);
    if (episode == null ||
        !episode.pending.contains(generation) ||
        episode.terminal ||
        episode.firstData) {
      return;
    }
    episode.firstData = true;
    _record(episode, 2);
  }

  void rangeEnded(int generation) {
    final episode = _current(generation);
    if (episode == null) return;
    if (episode.pending.remove(generation)) episode.completed++;
    _settled();
  }

  void failed(int generation, String code) {
    if (code == 'clipboard_cancelled') {
      cancelRange(generation);
      return;
    }
    final episode = _current(generation);
    if (episode == null) return;
    episode.pending.remove(generation);
    episode.failed++;
    _terminal(episode, 6, code);
    _settled();
  }

  /// Whether a failure of [generation] is the first of its episode to be
  /// reported as a failed transfer (clipboard action 8). The ranges of one
  /// episode fail together when their source goes away (the Host's queued
  /// pulls of one paste all meet the same expired offer): one transfer
  /// failed, not one per range. A range outside any live episode reports.
  bool claimFailureReport(int generation) {
    final episode = _current(generation);
    if (episode == null) return true;
    if (episode.failureReported) return false;
    episode.failureReported = true;
    return true;
  }

  void cancel() {
    final episode = _episode;
    if (_closed || episode == null) return;
    episode.pending.clear();
    episode.cancelled = true;
    _terminal(episode, 5, 'clipboard_cancelled');
    _settled();
  }

  void cancelRange(int generation) {
    final episode = _current(generation);
    if (episode == null) return;
    episode.pending.remove(generation);
    episode.cancelled = true;
    _settled();
  }

  void waiting(bool value) {
    if (_closed) return;
    // A new waiting operation can be a quick retry after failure. It gets a
    // fresh observational epoch without resuming any old transport ranges.
    if (value && !_waiting && _episode?.terminal == true) {
      _quiet?.cancel();
      _periodic?.cancel();
      _episode = null;
    }
    _waiting = value;
    if (value) {
      _quiet?.cancel();
    } else {
      _settled();
    }
  }

  void _settled() {
    final episode = _episode;
    if (_closed || episode == null || episode.pending.isNotEmpty || _waiting) {
      return;
    }
    _quiet?.cancel();
    _quiet = Timer(const Duration(seconds: 2), () {
      if (_closed || !identical(_episode, episode)) return;
      if (!episode.terminal) {
        if (episode.cancelled) {
          _terminal(episode, 5, 'clipboard_cancelled');
        } else if (episode.completed > 0) {
          _terminal(episode, 4);
        }
      }
      _periodic?.cancel();
      _episode = null;
    });
  }

  void _terminal(_Episode episode, int stage, [String? code]) {
    if (episode.terminal) return;
    _record(episode, stage, code);
    episode.terminal = true;
    _periodic?.cancel();
  }

  void _record(_Episode episode, int stage, [String? code]) {
    // Reserve the last slot for a terminal record of the current episode.
    if (_closed || _remaining == 0 || (_remaining == 1 && stage < 4)) return;
    _remaining--;
    final elapsed = _now().difference(episode.started).inMilliseconds;
    try {
      emit(
        ClipboardPullObservation(
          sending: sending,
          stage: stage,
          requests: episode.requests.clamp(0, 0xffffffff),
          failedRequests: episode.failed.clamp(0, 0xffffffff),
          elapsedMs: elapsed.clamp(0, 0xffffffff),
          code: code,
        ),
      );
    } catch (_) {
      // Diagnostics cannot alter transfer, permission or transport behavior.
    }
  }

  void close(String reason) {
    if (_closed) return;
    final episode = _episode;
    if (episode != null) {
      if (episode.cancelled) {
        _terminal(episode, 5, 'clipboard_cancelled');
      } else if (episode.pending.isEmpty && episode.completed > 0) {
        _terminal(episode, 4);
      } else {
        _terminal(episode, 6, reason);
      }
    }
    _closed = true;
    _quiet?.cancel();
    _periodic?.cancel();
    _episode = null;
  }
}

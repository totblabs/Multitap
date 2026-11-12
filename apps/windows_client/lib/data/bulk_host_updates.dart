import 'dart:async';

import 'package:flutter/foundation.dart';

import '../models/desktop_machine.dart';
import 'broker_api.dart';
import 'product_updates.dart';
import '../l10n/l10n.dart';

enum BulkHostPhase {
  available,
  unavailable,
  queued,
  requesting,
  rateLimited,
  acknowledged,
  waiting,
  downloading,
  verifying,
  installing,

  /// Installing while the Host is away: its service restarts to finish.
  reconnecting,
  completed,
  rebootRequired,
  needsCheck,
  failed,
  uncertain,
  stopped,
}

enum _Submission {
  none,
  queued,
  requesting,
  rateLimited,
  acknowledged,
  failed,
  uncertain,
  stopped,
}

class BulkHostUpdateRow {
  BulkHostUpdateRow(this.machine, this.targetVersion)
    : observed = machine.hostUpdate;
  DesktopMachine machine;
  final String targetVersion;
  HostUpdateState observed;
  bool assigned = true, selected = false, attempted = false;
  _Submission _submission = _Submission.none;
  String? _localCode;
  HostUpdateOperation? _baseline;
  int _posts = 0;

  /// The broker refused to end another account's connection (an ordinary
  /// account ends only its own): the request was sent again to wait for it.
  bool otherUser = false;

  String get id => machine.id;
  HostUpdateOperation? get operation =>
      observed.operation?.targetVersion == targetVersion
      ? observed.operation
      : null;
  bool get eligible => !attempted && _canRequest;

  /// VMs and approved local PCs alike; the broker authorizes
  /// each request by assignment.
  bool get _canRequest =>
      assigned &&
      machine.hostUpdate.offered &&
      machine.hostUpdate.latestVersion == targetVersion &&
      _newer(targetVersion, machine.hostUpdate.version);

  static bool _newer(String target, String? current) {
    try {
      return current != null && compareProductVersions(target, current) > 0;
    } catch (_) {
      return false;
    }
  }

  BulkHostPhase get phase {
    if (!assigned) {
      return attempted ? BulkHostPhase.uncertain : BulkHostPhase.unavailable;
    }
    if (_submission == _Submission.queued) return BulkHostPhase.queued;
    if (_submission == _Submission.rateLimited) {
      return BulkHostPhase.rateLimited;
    }
    if (_submission == _Submission.stopped) return BulkHostPhase.stopped;
    final op = operation;
    final fresh =
        op != null &&
        (attempted
            ? _submission == _Submission.acknowledged ||
                  !_sameOperation(op, _baseline)
            : op.active);
    if (fresh) {
      return switch (op.status) {
        'waiting' => BulkHostPhase.waiting,
        'dispatched' || 'accepted' => BulkHostPhase.acknowledged,
        'downloading' => BulkHostPhase.downloading,
        'verifying' => BulkHostPhase.verifying,
        'installing' when !observed.connected => BulkHostPhase.reconnecting,
        'installing' => BulkHostPhase.installing,
        'completed'
            when op.code == 'result_unreported' ||
                op.code == 'install_result_missing' ||
                op.code == 'display_attention' =>
          BulkHostPhase.needsCheck,
        'completed' when op.rebootRequired || op.code == 'restart_required' =>
          BulkHostPhase.rebootRequired,
        'completed' => BulkHostPhase.completed,
        'failed' || 'rejected' || 'expired' => BulkHostPhase.failed,
        _ => BulkHostPhase.uncertain,
      };
    }
    return switch (_submission) {
      _Submission.requesting => BulkHostPhase.requesting,
      _Submission.acknowledged => BulkHostPhase.acknowledged,
      _Submission.failed => BulkHostPhase.failed,
      _Submission.uncertain => BulkHostPhase.uncertain,
      _ => eligible ? BulkHostPhase.available : BulkHostPhase.unavailable,
    };
  }

  /// The row's short state; [message] is its reason.
  String get stage => switch (phase) {
    BulkHostPhase.queued ||
    BulkHostPhase.requesting ||
    BulkHostPhase.rateLimited ||
    BulkHostPhase.acknowledged ||
    BulkHostPhase.waiting => l10n.waiting,
    BulkHostPhase.downloading => l10n.download,
    BulkHostPhase.verifying || BulkHostPhase.installing => l10n.install,
    BulkHostPhase.reconnecting => l10n.reconnect,
    BulkHostPhase.completed || BulkHostPhase.rebootRequired => l10n.done,
    BulkHostPhase.needsCheck when operation?.code == 'display_attention' =>
      l10n.done,
    BulkHostPhase.needsCheck || BulkHostPhase.uncertain => l10n.needsCheck,
    BulkHostPhase.failed => l10n.failed,
    BulkHostPhase.stopped => l10n.stopped,
    BulkHostPhase.available => l10n.updateAvailable,
    BulkHostPhase.unavailable => l10n.notEligible,
  };

  static bool _sameOperation(HostUpdateOperation? a, HostUpdateOperation? b) =>
      a?.status == b?.status &&
      a?.targetVersion == b?.targetVersion &&
      a?.checkedAt == b?.checkedAt &&
      a?.code == b?.code;

  String get message {
    if (!assigned) {
      return attempted
          ? l10n.assignmentWasRemovedSoResult
          : l10n.desktopIsntAssignedYouNow;
    }
    final op = operation;
    return switch (phase) {
      BulkHostPhase.available =>
        machine.hostUpdate.sessionActive
            ? l10n.useRequestingDisconnects
            : l10n.notUseUpdatesRightAway,
      BulkHostPhase.queued => l10n.notRequestedYet,
      BulkHostPhase.requesting => l10n.requestingFromServer,
      BulkHostPhase.rateLimited => l10n.notAcceptedYetRequestingAgain,
      BulkHostPhase.acknowledged => l10n.acceptedByServerWaitingInstall,
      BulkHostPhase.waiting =>
        otherUser ? l10n.updatesWhenOtherUsersConnection : l10n.useUpdatesWhenConnectionEnds,
      BulkHostPhase.downloading =>
        l10n.downloadingInstaller(op?.progress == null ? '' : ' ${op!.progress}%'),
      BulkHostPhase.verifying => l10n.checkingInstaller,
      BulkHostPhase.installing => l10n.installingHost,
      BulkHostPhase.reconnecting => l10n.hostServiceRestartedWaitingConnection,
      BulkHostPhase.completed => l10n.serverConfirmedInstallFinished,
      BulkHostPhase.rebootRequired => l10n.installedWindowsRestartNeeded,
      BulkHostPhase.needsCheck =>
        op?.code == 'display_attention'
            ? l10n.installedOpenHostSettingsDesktop
            : l10n.runningNewVersionInstallResult,
      BulkHostPhase.failed =>
        _localCode != null
            ? _requestReason(_localCode!)
            : hostUpdateReason(op?.code ?? ''),
      BulkHostPhase.uncertain => l10n.checkRequestInstallResultWont,
      BulkHostPhase.stopped => l10n.stoppedRemainingRequestsWerentAccepted,
      BulkHostPhase.unavailable => _unavailableReason,
    };
  }

  String get _unavailableReason {
    final state = machine.hostUpdate;
    if (!state.present) return l10n.serverCantCheckUpdatePermission;
    if (!state.connected) return l10n.hostOffline;
    if (!state.supported) {
      return l10n.remoteUpdateNotSupportedInstall;
    }
    if (state.latestVersion != targetVersion) {
      return l10n.latestReleaseChangedCloseWindow;
    }
    if (!_newer(targetVersion, state.version)) {
      return state.version == null ? l10n.checkCurrentHostVersion : l10n.noNewVersionInstall;
    }
    if (state.busy) return l10n.anotherHostUpdateProgress;
    return l10n.serverDoesntAllowUpdatesRight;
  }

  static String _requestReason(String code) => switch (code) {
    'host_offline' => l10n.hostOfflineSoRequestWasnt,
    'host_update_in_progress' => l10n.anotherUpdateAlreadyProgress,
    'host_release_unavailable' => l10n.latestReleaseChangedCloseWindow,
    'host_update_current' => l10n.serverConfirmedItsAlreadyUp,
    'host_update_unsupported' => l10n.hostDoesntSupportRemoteUpdates2,
    'permission_denied' => l10n.noUpdatePermissionSignExpired,
    'too_many_host_update_attempts' => l10n.tooManyRequestsSoWasnt,
    _ => l10n.serverCouldntAcceptRequestClose,
  };

  void _observe(HostUpdateState state) {
    final before = operation, next = state.operation;
    if (attempted) {
      if (next?.targetVersion != targetVersion) return;
      final newObservation = !_sameOperation(before, _baseline);
      if (newObservation &&
          before?.checkedAt != null &&
          next?.checkedAt != null &&
          next!.checkedAt!.isBefore(before!.checkedAt!)) {
        return;
      }
      if (newObservation &&
          before?.status == 'completed' &&
          next?.status != 'completed') {
        return;
      }
      if (_sameOperation(next, _baseline) && _submission != _Submission.none) {
        return;
      }
    }
    observed = state;
  }
}

/// One request at a time; only a known pre-accept limiter rejection permits a
/// bounded repeat. Accepted/uncertain VMs are never automatically requested twice. Server
/// observations drive installation results; a POST acknowledgement is not one.
class BulkHostUpdateController extends ChangeNotifier {
  BulkHostUpdateController({
    required this.api,
    required this._token,
    required this.release,
    required List<DesktopMachine> assignedMachines,
    this.requestTimeout = const Duration(seconds: 35),
    this.minimumRequestInterval = const Duration(milliseconds: 3100),
    this.rateLimitBackoff = const Duration(seconds: 60),
    this.maxRateLimitRetries = 2,
    this._elapsed,
    Future<void> Function(Duration)? pause,
  }) : _pauseOverride = pause {
    if (release.product != 'host') throw ArgumentError('Host release required');
    compareProductVersions(release.version, release.version);
    synchronize(assignedMachines);
  }
  final HostUpdateBrokerApi api;
  final ProductRelease release;
  final Duration requestTimeout, minimumRequestInterval, rateLimitBackoff;
  final int maxRateLimitRetries;
  final Duration Function()? _elapsed;
  final Future<void> Function(Duration)? _pauseOverride;
  final Stopwatch _paceClock = Stopwatch()..start();
  Duration? _lastPostAt;
  Completer<void>? _pauseCancelled;
  bool pacing = false;
  String _token;
  final _rows = <String, BulkHostUpdateRow>{};
  bool submitting = false,
      stopped = false,
      authorizationStopped = false,
      catalogChanged = false,
      _disposed = false;
  bool _postPending = false;
  List<BulkHostUpdateRow> get rows => List.unmodifiable(_rows.values);
  int get selectedCount => rows.where((r) => r.selected && r.eligible).length;
  int get requestedCount => rows.where((r) => r._posts > 0).length;
  int get pendingCount => rows
      .where(
        (r) =>
            r._submission == _Submission.queued ||
            r._submission == _Submission.rateLimited,
      )
      .length;
  int get completedCount => rows
      .where(
        (r) =>
            r.phase == BulkHostPhase.completed ||
            r.phase == BulkHostPhase.rebootRequired ||
            (r.phase == BulkHostPhase.needsCheck &&
                r.operation?.code == 'display_attention'),
      )
      .length;
  int get rebootCount => rows
      .where(
        (r) =>
            r.operation?.rebootRequired == true ||
            r.phase == BulkHostPhase.rebootRequired,
      )
      .length;
  int get checkingCount => rows
      .where(
        (r) =>
            r.phase == BulkHostPhase.needsCheck ||
            r.phase == BulkHostPhase.uncertain,
      )
      .length;
  static Set<String> get _resultStages => {l10n.done, l10n.failed, l10n.needsCheck, l10n.stopped, l10n.notEligible};
  Iterable<BulkHostUpdateRow> get _requested => rows.where((r) => r._posts > 0);
  int _stageCount(String stage) =>
      _requested.where((r) => r.stage == stage).length;

  /// Every Host this window requested has a result; nothing runs any more.
  bool get finished =>
      !submitting &&
      _requested.isNotEmpty &&
      _requested.every((r) => _resultStages.contains(r.stage));

  /// The line under the rows: how many run and how many ended which way,
  /// then the final result. Null before any request.
  String? get summary {
    if (_requested.isEmpty) return null;
    String count(String stage) => l10n.text5(stage, _stageCount(stage));
    if (!finished) {
      final running = _requested
          .where((r) => !_resultStages.contains(r.stage))
          .length;
      return l10n.progress(running, count(l10n.done), count(l10n.failed));
    }
    return [
      l10n.allFinished,
      count(l10n.done),
      count(l10n.failed),
      for (final stage in [l10n.needsCheck, l10n.stopped, l10n.notEligible])
        if (_stageCount(stage) > 0) count(stage),
    ].join(' · ');
  }

  bool get hasServerWork => rows.any(
    (r) =>
        r.attempted &&
        {
          BulkHostPhase.acknowledged,
          BulkHostPhase.waiting,
          BulkHostPhase.downloading,
          BulkHostPhase.verifying,
          BulkHostPhase.installing,
          BulkHostPhase.reconnecting,
          BulkHostPhase.uncertain,
        }.contains(r.phase),
  );
  bool get authenticated => _token.isNotEmpty;
  bool get canSubmit =>
      authenticated &&
      !submitting &&
      !_postPending &&
      !authorizationStopped &&
      !catalogChanged &&
      !_disposed;

  void synchronize(List<DesktopMachine> assignedMachines) {
    if (_disposed) return;
    final ids = <String>{};
    for (final machine in assignedMachines) {
      if (!ids.add(machine.id)) continue;
      final previousLatest =
          _rows[machine.id]?.machine.hostUpdate.latestVersion;
      final row = _rows.putIfAbsent(
        machine.id,
        () => BulkHostUpdateRow(machine, release.version),
      );
      row.machine = machine;
      row.assigned = true;
      row._observe(machine.hostUpdate);
      final latest = machine.hostUpdate.latestVersion;
      if (latest != null &&
          latest != release.version &&
          (previousLatest == release.version ||
              BulkHostUpdateRow._newer(latest, release.version))) {
        catalogChanged = true;
      }
    }
    for (final row in rows) {
      if (!ids.contains(row.id)) {
        row.assigned = false;
        row.selected = false;
      }
    }
    if (catalogChanged && submitting) stopPending();
    _notify();
  }

  void select(String desktopId, bool selected) {
    if (_disposed || submitting || authorizationStopped || catalogChanged) {
      return;
    }
    final row = _rows[desktopId];
    if (row != null && row.eligible) {
      row.selected = selected;
      _notify();
    }
  }

  void selectAll(bool selected) {
    if (_disposed || submitting || authorizationStopped || catalogChanged) {
      return;
    }
    for (final row in rows) {
      if (row.eligible) row.selected = selected;
    }
    _notify();
  }

  Future<void> submitSelected() async {
    if (!canSubmit) return;
    final queue = rows.where((r) => r.selected && r.eligible).toList();
    if (queue.isEmpty) return;
    submitting = true;
    stopped = false;
    for (final row in queue) {
      row._submission = _Submission.queued;
    }
    _notify();
    try {
      for (final row in queue) {
        if (_disposed || stopped) break;
        if (!row.eligible) {
          row._submission = _Submission.none;
          continue;
        }
        await _submitRow(row);
        _notify();
      }
    } finally {
      submitting = false;
      for (final row in queue) {
        if (row._submission == _Submission.queued) {
          row._submission = _Submission.stopped;
        }
      }
      _notify();
    }
  }

  Duration get _now => _elapsed?.call() ?? _paceClock.elapsed;
  Future<bool> _pause(Duration duration) async {
    if (duration <= Duration.zero) return !stopped && !_disposed;
    final cancelled = Completer<void>(), done = Completer<void>();
    _pauseCancelled = cancelled;
    Timer? timer;
    if (_pauseOverride == null) {
      timer = Timer(duration, done.complete);
    } else {
      unawaited(
        Future<void>.sync(() => _pauseOverride(duration)).then(
          (_) {
            if (!done.isCompleted) done.complete();
          },
          onError: (Object error, StackTrace stack) {
            if (!done.isCompleted) done.completeError(error, stack);
          },
        ),
      );
    }
    pacing = true;
    _notify();
    try {
      await Future.any([done.future, cancelled.future]);
    } finally {
      timer?.cancel();
      if (identical(_pauseCancelled, cancelled)) _pauseCancelled = null;
      pacing = false;
      _notify();
    }
    return !stopped && !_disposed;
  }

  /// Each request ends the desktop's connections now (a bulk update does
  /// not wait for them). Where the broker refuses to end
  /// another account's connection for an ordinary account, the same request
  /// is sent once more without it and waits for that connection to end.
  Future<void> _submitRow(BulkHostUpdateRow row) async {
    row._baseline = row.observed.operation;
    var disconnect = true;
    for (var retry = 0; ;) {
      if (stopped || _disposed) return;
      final last = _lastPostAt;
      if (last != null &&
          !await _pause(minimumRequestInterval - (_now - last))) {
        return;
      }
      if (stopped || _disposed) return;
      if (!row._canRequest) {
        row._submission = _Submission.none;
        return;
      }
      row.attempted = true;
      row._posts++;
      row._submission = _Submission.requesting;
      row._localCode = null;
      _lastPostAt = _now;
      _notify();
      try {
        final pending = api.requestHostUpdate(
          _token,
          row.id,
          release.version,
          disconnectSessions: disconnect,
        );
        _postPending = true;
        unawaited(
          pending.then<void>(
            (_) {
              _postPending = false;
              _notify();
            },
            onError: (Object _, StackTrace _) {
              _postPending = false;
              _notify();
            },
          ),
        );
        final state = await pending.timeout(requestTimeout);
        if (!state.present ||
            state.operation?.targetVersion != release.version) {
          throw const BrokerException('invalid_response');
        }
        row._submission = _Submission.acknowledged;
        row._observe(state);
        if (state.latestVersion != null &&
            state.latestVersion != release.version) {
          catalogChanged = true;
          stopPending();
        }
      } on TimeoutException {
        row._submission = _Submission.uncertain;
        stopPending(); // Underlying POST may still exist: no overlap or replay.
      } on BrokerException catch (error) {
        if (error.status == 429 &&
            error.code == 'too_many_host_update_attempts') {
          // This exact broker limiter rejects BEFORE HostUpdates.request.
          // It is not an accepted/uncertain request, so bounded retry is safe.
          row.attempted = false;
          if (retry++ < maxRateLimitRetries) {
            row._submission = _Submission.rateLimited;
            _notify();
            if (!await _pause(rateLimitBackoff)) return;
            continue;
          }
          row._submission = _Submission.failed;
          row._localCode = error.code;
        } else if (disconnect &&
            error.status == 409 &&
            error.code == 'desktop_busy' &&
            error.reason == 'other_user') {
          // Refused before any operation was stored: nothing to duplicate.
          disconnect = false;
          row.otherUser = true;
          continue;
        } else if (error.status == 401 || error.status == 403) {
          row._submission = _Submission.failed;
          row._localCode = 'permission_denied';
          authorizationStopped = true;
          stopPending();
        } else if (error.code == 'host_release_unavailable') {
          row._submission = _Submission.failed;
          row._localCode = error.code;
          catalogChanged = true;
          stopPending();
        } else if (error.status >= 400 &&
            error.status < 500 &&
            error.status != 408) {
          row._submission = _Submission.failed;
          row._localCode = error.code;
        } else {
          row._submission = _Submission.uncertain;
        }
      } catch (_) {
        row._submission = _Submission.uncertain;
      }
      return;
    }
  }

  /// Stops only rows whose POST has not begun. Accepted server operations and
  /// the currently awaiting POST continue, including after this UI closes.
  void stopPending() {
    stopped = true;
    if (_pauseCancelled?.isCompleted == false) _pauseCancelled!.complete();
    for (final row in rows) {
      if (row._submission == _Submission.queued ||
          row._submission == _Submission.rateLimited) {
        row._submission = _Submission.stopped;
      }
    }
    _notify();
  }

  void _notify() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    stopped = true;
    if (_pauseCancelled?.isCompleted == false) _pauseCancelled!.complete();
    _disposed = true;
    _token = '';
    super.dispose();
  }
}

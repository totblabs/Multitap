import 'dart:async';

import 'broker_api.dart';
import '../models/desktop_machine.dart';
import '../models/stream_preparation.dart';

/// How often a broker that is restarting is asked again (gateway answers, a
/// refused or reset port) while a stream recovers.
const restartPollInterval = Duration(milliseconds: 250);

/// How often the list is read while a Host update still runs on an online
/// Host (downloading, installing): progress needs no 500 ms reads.
const hostUpdatePollInterval = Duration(seconds: 2);

/// Display states a Host passes through while it restores its outputs after
/// its connection to the relay was lost (stream stop, console restore, fresh
/// probe). States that need the user (locked, login, console, administrator,
/// configuration, verification, restart) are never waited for.
bool streamPreparationTransient(StreamPreparation? value) =>
    value == null ||
    value == StreamPreparation.maintenance ||
    value == StreamPreparation.checking ||
    value == StreamPreparation.display ||
    value == StreamPreparation.recovery;

/// Used after retiring this view's stream or observing another window's pending
/// cleanup. Local closure is not proof that Host restored its display profile.
/// Observe fresh capabilities; never retry a count/connection mutation here.
///
/// [recovering]: the previous stream ended because the relay lost the Host.
/// Its service reconnects within seconds and restores the display before it
/// reports count control again; the transient states in between are waited
/// for too (up to 30 s) instead of failing: a reconnect 3 s after such a drop
/// can fail as display_not_supported where the same retry 0.4 s later
/// succeeds. [countControl] false (a view that prepares no virtual
/// monitor count) waits for the Host to stream again instead of for count
/// control.
///
/// [initialPreparation] also waits through an explicit maintenance/checking/
/// recovery observation even if count control is already advertised. It still
/// permits count activation when that is needed to make an inactive output
/// ready. A new HTTP response can contain an old Host observation; this helper
/// never treats HTTP freshness as proof of a newer Host readiness generation.
/// [refreshFirst] includes the first inventory read in this wait. Only explicit
/// post-transport recovery opts into [retryGatewayErrors]; it retries read-only
/// gateway failures within this same deadline, never a connection mutation.
///
/// [restarting]: the user restarted the VM from this window (the sound
/// card). Its power task, a stopped moment, the Host's absence and the
/// time before Windows signs in are waited through too, within [maxWait].
///
/// Restart recovery: a broker update ends every stream as
/// server_stopping. While the broker itself is away, a refused or reset
/// connection is retried like a gateway answer, every [restartPollInterval]
/// (inside this wait, not in a 1-2-4-8 s backoff). While its Host
/// is away, [waitHost] (host-ready-wait-v1) is answered by the broker the
/// moment the Host reported, instead of reading the whole list every 500 ms;
/// it returns null where the broker does not offer it, and the list is read
/// every 500 ms as before.
///
/// [hostUpdating]: a Host update ended the stream (host_update).
/// While the broker reports that update under way, the list is read every
/// [hostUpdatePollInterval]; the Host's own restart is then waited through
/// like any Host return. A Host that stays away after the update
/// (hostMissingAfterUpdate) ends the wait at once, and a wait that runs out
/// while the update still runs ends as `host_update` (409).
///
/// [hostAwayWait]: Windows restarted from inside the VM (Start → 다시 시작,
/// an update, a crash) takes longer than the 30 s a Host restart gets
/// (within those the window would give up while Windows is still
/// starting). While recovering, the first read that finds the Host away with
/// the VM still running extends this wait to [hostAwayWait] from then and
/// waits like [restarting] (power task, Windows sign-in); [onHostAway] is
/// told once. A VM that powers off ends the wait as before.
Future<DesktopMachine> waitForStreamDisplayControl(
  DesktopMachine machine, {
  required Future<DesktopMachine> Function() refresh,
  required bool Function() current,
  bool recovering = false,
  bool countControl = true,
  bool initialPreparation = false,
  bool independentTracks = false,
  bool refreshFirst = false,
  bool retryGatewayErrors = false,
  bool restarting = false,
  bool hostUpdating = false,
  Duration? maxWait,
  Future<bool?> Function()? waitHost,
  Duration? hostAwayWait,
  void Function()? onHostAway,
}) async {
  var retired = false;
  // Set once [hostAwayWait] took over: the wait is then a restart's.
  var hostAway = false;
  void check() {
    if (retired || !current()) {
      throw const BrokerException('operation_cancelled');
    }
  }

  // A running VM whose Host service is restarting (a Host update, a worker or
  // capture failure, its own recovery) usually returns within seconds.
  // Production diagnostics: reconnecting in that gap failed at once as
  // display_not_supported, and the same retry seconds later succeeded.
  bool hostReturning(DesktopMachine value) =>
      value.hostOnline == false &&
      !value.sessionCleanup.pending &&
      value.powerState == 'running' &&
      !value.powerStale &&
      !value.powerBusy;

  bool settled(DesktopMachine value) =>
      (!independentTracks || value.independentDisplayStreaming) &&
          !(initialPreparation &&
              (value.streamPreparation == StreamPreparation.maintenance ||
                  value.streamPreparation == StreamPreparation.checking ||
                  value.streamPreparation == StreamPreparation.recovery)) &&
          (countControl
              ? value.canConfigureDisplays
              : value.status == MachineStatus.ready);

  bool restartPending(DesktopMachine value) =>
      !value.sessionCleanup.pending &&
      (restarting
          ? (value.hostOnline != true ||
                value.powerBusy ||
                value.powerState != 'running' ||
                value.interactiveSession == false)
          // Windows restarting by itself: the Host away while the VM runs
          // or a power task runs, then Windows signing in. A VM that was
          // shut down is not waited for.
          : hostAway &&
                (hostReturning(value) ||
                    (value.hostOnline != true && value.powerBusy) ||
                    (value.hostOnline == true &&
                        value.interactiveSession == false)));

  bool updatePending(DesktopMachine value) =>
      hostUpdating && value.hostUpdate.busy;

  bool waiting(DesktopMachine value) =>
      updatePending(value) ||
      restartPending(value) ||
      hostReturning(value) ||
      value.hostOnline == true &&
          (value.sessionCleanup.pending
              ? !value.canRecoverConsole
              : !settled(value) &&
                    value.interactiveSession != false &&
                    value.status != MachineStatus.poweredOff &&
                    !value.powerBusy &&
                    (recovering
                        ? streamPreparationTransient(value.streamPreparation)
                        : !value.display.busy &&
                              (value.streamPreparation == null ||
                                  value.streamPreparation ==
                                      StreamPreparation.maintenance ||
                                  value.streamPreparation ==
                                      StreamPreparation.checking)));

  var latest = machine;
  BrokerException? gatewayFailure;
  // Restarts the deadline below from now ([hostAwayWait]).
  void Function(Duration wait)? extendWait;
  Future<DesktopMachine> read() async {
    while (true) {
      check();
      try {
        final fresh = await refresh().timeout(const Duration(seconds: 8));
        check();
        if (fresh.id != machine.id) {
          throw const BrokerException('invalid_response');
        }
        latest = fresh;
        gatewayFailure = null;
        if (hostAwayWait != null &&
            !hostAway &&
            recovering &&
            !initialPreparation &&
            hostReturning(fresh)) {
          hostAway = true;
          extendWait?.call(hostAwayWait);
          onHostAway?.call();
        }
        return fresh;
      } on BrokerException catch (error) {
        check();
        // Only an existing transport recovery may outwait a gateway response
        // or a restarting broker's refused/reset port. Parsed authorization/
        // policy/protocol decisions remain authoritative.
        final gateway =
            const {502, 503, 504}.contains(error.status) &&
            const {
              'server_restarting',
              'invalid_response',
              'request_failed',
            }.contains(error.code);
        if (!retryGatewayErrors ||
            !recovering ||
            initialPreparation ||
            !(gateway || error.serverRestarting || error.networkUnavailable)) {
          rethrow;
        }
        gatewayFailure = error;
        await Future<void>.delayed(restartPollInterval);
        check();
      } on TimeoutException {
        // No answer within 8 s: the same lost network path, while recovering.
        check();
        if (!retryGatewayErrors || !recovering || initialPreparation) rethrow;
        gatewayFailure = const BrokerException('network_error', 0, 'timeout');
        await Future<void>.delayed(restartPollInterval);
        check();
      }
    }
  }

  Future<DesktopMachine> observe() async {
    var fresh = machine;
    if (refreshFirst) fresh = await read();
    // The broker's last host-ready answer was "reported": if the next read
    // still misses the Host, read the list again instead of asking at once.
    var answered = false;
    while (true) {
      check();
      if (hostUpdating && fresh.hostMissingAfterUpdate != null) {
        throw const BrokerException('host_update', 409);
      }
      if (!waiting(fresh)) return fresh;
      // A Host that just reported is read again at the usual pace.
      Duration? pause =
          updatePending(fresh) && fresh.hostOnline == true && !answered
          ? hostUpdatePollInterval
          : const Duration(milliseconds: 500);
      if (waitHost != null && fresh.hostOnline != true && !answered) {
        try {
          final reported = await waitHost().timeout(
            const Duration(seconds: 10),
          );
          if (reported != null) {
            pause = null;
            answered = reported;
          }
        } catch (_) {
          // Its own failure (a gateway answer, a refused port): the list
          // read below decides, soon.
          pause = restartPollInterval;
        }
        check();
      } else {
        answered = false;
      }
      if (pause != null) await Future<void>.delayed(pause);
      check();
      fresh = await read();
    }
  }

  BrokerException timedOut() =>
      gatewayFailure ??
      BrokerException(
        machine.sessionCleanup.pending
            ? 'session_cleanup_pending'
            : updatePending(latest)
            ? 'host_update'
            // The restarted VM is up but nobody signed in to Windows.
            : (restarting || hostAway) &&
                  latest.hostOnline == true &&
                  latest.interactiveSession == false
            ? 'no_interactive_session'
            : hostReturning(latest) || restartPending(latest)
            ? 'host_offline'
            : independentTracks &&
                  latest.canConfigureDisplays &&
                  !latest.independentDisplayStreaming
            ? 'track_streaming_unavailable'
            : 'host_maintenance',
        409,
      );

  // A deadline that [extendWait] can restart (Future.timeout's cannot).
  final result = Completer<DesktopMachine>();
  Timer? deadline;
  void arm(Duration wait) {
    deadline?.cancel();
    deadline = Timer(wait, () {
      if (!result.isCompleted) result.completeError(timedOut());
    });
  }

  extendWait = arm;
  arm(
    maxWait ??
        Duration(
          seconds: machine.sessionCleanup.pending
              ? 45
              : recovering
              ? 30
              : 15,
        ),
  );
  observe().then(
    (value) {
      if (!result.isCompleted) result.complete(value);
    },
    onError: (Object error, StackTrace stack) {
      if (!result.isCompleted) result.completeError(error, stack);
    },
  );
  try {
    return await result.future;
  } finally {
    deadline?.cancel();
    // A deadline cannot cancel an outstanding HTTP read. Its late result
    // must not schedule further reads or reach the caller's count mutation.
    retired = true;
  }
}

/// Checks fresh owned outputs, rather than accepting a count-command ACK.
bool singleStreamDisplayReady(DesktopMachine machine) =>
    streamDisplayCountReady(machine, 1);

bool streamDisplayCountReady(DesktopMachine machine, int count) =>
    !machine.sessionCleanup.pending &&
    (count == 1 || count == 2) &&
    machine.hostOnline == true &&
    machine.virtualMonitorControl &&
    machine.display.supported &&
    !machine.display.stale &&
    !machine.display.busy &&
    !machine.powerBusy &&
    machine.display.activeCount == count &&
    machine.monitors.length == count &&
    machine.monitors.map((m) => m.id).toSet().containsAll([
      '0',
      if (count == 2) '1',
    ]);

Future<DesktopMachine> prepareSingleStreamDisplay(
  DesktopMachine machine,
  Future<DesktopMachine> Function(int count)? configure,
) async {
  return prepareStreamDisplayCount(machine, 1, configure);
}

Future<DesktopMachine> prepareStreamDisplayCount(
  DesktopMachine machine,
  int count,
  Future<DesktopMachine> Function(int count)? configure,
) async {
  if (count != 1 && count != 2) {
    throw const BrokerException('invalid_display_count');
  }
  if (machine.sessionCleanup.pending) {
    throw const BrokerException('session_cleanup_pending', 409);
  }
  if (streamDisplayCountReady(machine, count)) return machine;
  // An offline Host says nothing about display support; report it as such.
  if (machine.hostOnline == false) {
    throw const BrokerException('host_offline', 409);
  }
  if (!machine.virtualMonitorControl ||
      !machine.display.supported ||
      configure == null) {
    throw const BrokerException('display_not_supported');
  }
  final fresh = await configure(count).timeout(
    const Duration(seconds: 32),
    onTimeout: () => throw const BrokerException('display_operation_timeout'),
  );
  if (fresh.id != machine.id || !streamDisplayCountReady(fresh, count)) {
    throw const BrokerException('display_operation_unknown');
  }
  return fresh;
}

import 'dart:async';

import 'broker_api.dart';

/// Stops this client's wait, not the Windows operation already sent to Host.
class ConsoleResumeCancelled implements Exception {
  const ConsoleResumeCancelled();
}

class ConsoleResumeCancellation {
  static final _scope = Object();
  final _cancelled = Completer<void>();
  bool get isCancelled => _cancelled.isCompleted;
  void cancel() {
    if (!isCancelled) _cancelled.complete();
  }

  /// Carry a per-dialog lifetime through the existing zero-argument callback
  /// contract shared by released Windows and Android launchers. Direct callers
  /// can instead pass [ConsoleResume.run]'s explicit cancellation argument.
  Future<T> bind<T>(Future<T> Function() action) async {
    if (isCancelled) throw const ConsoleResumeCancelled();
    return runZoned(action, zoneValues: {_scope: this});
  }

  static ConsoleResumeCancellation? get _current =>
      Zone.current[_scope] as ConsoleResumeCancellation?;

  Future<T> _wait<T>(Future<T> action) => Future.any([
    action,
    _cancelled.future.then<T>((_) => throw const ConsoleResumeCancelled()),
  ]);
}

/// One explicit transfer request, then read-only observation of its exact
/// operation and the actual stream/display capabilities. Never retries POST.
class ConsoleResume {
  ConsoleResume({
    required this.api,
    required this.loadDesktops,
    required this.authorized,
    this.timeout = const Duration(seconds: 45),
    this.interval = const Duration(seconds: 1),
    Future<void> Function(Duration)? delay,
  }) : delay = delay ?? Future<void>.delayed;
  final ConsoleRecoveryApi api;
  final Future<List<BrokerDesktop>> Function() loadDesktops;
  final bool Function() authorized;
  final Duration timeout, interval;
  final Future<void> Function(Duration) delay;

  Future<BrokerDesktop> run(
    String token,
    String desktopId, {
    ConsoleResumeCancellation? cancellation,
  }) async {
    final cancel = cancellation ?? ConsoleResumeCancellation._current;
    final clock = Stopwatch()..start();
    var timeoutCode = 'console_operation_unknown';
    void current() {
      if (cancel?.isCancelled == true) throw const ConsoleResumeCancelled();
      if (!authorized()) throw const BrokerException('session_expired', 401);
    }

    Duration remaining() {
      current();
      final remaining = timeout - clock.elapsed;
      if (remaining <= Duration.zero) throw BrokerException(timeoutCode);
      return remaining;
    }

    Future<T> bounded<T>(Future<T> Function() action, String code) {
      final duration = remaining();
      final request = action();
      return (cancel?._wait(request) ?? request).timeout(
        duration,
        onTimeout: () => throw BrokerException(code),
      );
    }

    current();
    var state = await bounded(
      () => api.prepareConsole(token, desktopId),
      'console_operation_unknown',
    );
    current();
    final id = state.operation?.id;
    if (id == null) throw const BrokerException('invalid_response');
    while (true) {
      current();
      remaining();
      final operation = state.operation;
      if (operation?.id != id) throw const BrokerException('invalid_response');
      if (operation!.status == 'failed') {
        throw BrokerException(
          operation.error ?? 'console_transfer_failed',
          409,
        );
      }
      if (operation.status == 'unknown') {
        throw const BrokerException('console_operation_unknown', 409);
      }
      if (operation.status == 'succeeded') break;
      await (cancel?._wait(delay(interval)) ?? delay(interval));
      current();
      state = await bounded(
        () => api.consolePreparation(token, desktopId, operationId: id),
        'console_operation_unknown',
      );
    }
    timeoutCode = 'console_display_not_ready';
    while (true) {
      current();
      final desktops = await bounded(loadDesktops, 'console_display_not_ready');
      current();
      final matches = desktops.where((desktop) => desktop.id == desktopId);
      if (matches.length != 1) {
        throw const BrokerException('desktop_not_found', 404);
      }
      final desktop = matches.single;
      if (desktop.consolePreparation.operation?.id != id ||
          desktop.consolePreparation.operation?.status != 'succeeded') {
        throw const BrokerException('console_binding_changed', 409);
      }
      if (desktop.hostConnected == false) {
        throw const BrokerException('host_offline', 409);
      }
      if (desktop.consolePreparation.reason == 'windows_session_locked') {
        throw const BrokerException('windows_session_locked', 409);
      }
      if (!desktop.display.busy &&
          !desktop.sessionCleanup.pending &&
          !desktop.powerBusy &&
          desktop.hostConnected == true &&
          desktop.interactiveSession == true &&
          desktop.virtualDisplayReady == true &&
          desktop.canStream) {
        return desktop;
      }
      remaining();
      await (cancel?._wait(delay(interval)) ?? delay(interval));
    }
  }
}

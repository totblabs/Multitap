/// The Android remote view's quiet reconnect rules (android_remote.dart):
/// how long to wait before the next try, which failures end the tries and
/// the wait through a broker restart.
library;

import 'dart:async';
import 'dart:io';

import '../data/broker_api.dart';
import '../data/desktop_mapping.dart';
import '../data/stream_display_entry.dart';
import '../models/desktop_machine.dart';

/// The wait before reconnect try [attempt] (0-based) of [seconds] (the last
/// step repeats), shortened by up to a fifth by [jitter] (0..1): phones that
/// lost the same Host come back spread out, never sooner than 80 % of a step.
Duration androidReconnectDelay(List<int> seconds, int attempt, double jitter) {
  final step = seconds[attempt.clamp(0, seconds.length - 1)];
  final spread = jitter.isFinite ? jitter.clamp(0.0, 1.0) : 0.0;
  return Duration(milliseconds: (step * 1000 * (1 - 0.2 * spread)).round());
}

/// The broker was not reached at all (no network, DNS, TLS, timeout). Only
/// then can a newly available network help; an answer from the broker (a
/// Host not registered yet, a restart) proves the network works.
bool androidReconnectOffline(Object error) =>
    error is TimeoutException ||
    error is SocketException ||
    error is HandshakeException ||
    error is HttpException ||
    error is WebSocketException ||
    (error is BrokerException &&
        error.status == 0 &&
        error.code == 'network_error');

/// A refusal that trying again cannot change: the broker rejected the
/// request itself or this account's access to the VM.
bool androidReconnectRefusalFinal(Object error) =>
    error is BrokerException &&
    const {400, 401, 403, 404, 410, 422}.contains(error.status);

/// Restart recovery (host-ready-wait-v1, 2026-11-03): the Windows remote
/// window's own wait (waitForStreamDisplayControl), so the two cannot drift.
/// A restarting broker (502-504, a refused or reset port) is asked again
/// every 250 ms; while [machine]'s Host is away, a broker whose list offers
/// host-ready answers the moment the Host reported (else the list is read
/// every 500 ms). Returns once the Host streams again; throws after [limit]
/// or on any other failure, for the caller's back-off to decide.
/// [hostUpdating]: a Host update ended the stream; its run is waited
/// through too (waitForStreamDisplayControl), `host_update` when it ends
/// without its Host.
Future<void> androidAwaitBrokerRestart(
  BrokerApi api,
  String token,
  DesktopMachine machine, {
  required bool Function() current,
  required Duration limit,
  bool hostUpdating = false,
  Duration? hostAwayWait,
  void Function()? onHostAway,
}) async {
  final hostReady = api is HostReadyBrokerApi
      ? api as HostReadyBrokerApi
      : null;
  await waitForStreamDisplayControl(
    machine,
    refresh: () async {
      final listed = (await api.desktops(token))
          .where((desktop) => desktop.id == machine.id)
          .toList();
      if (listed.length != 1) {
        throw const BrokerException('desktop_not_found', 404);
      }
      return machineFromBroker(listed.single);
    },
    current: current,
    recovering: true,
    // Android's own preparation settles the output; this waits for the Host.
    countControl: false,
    refreshFirst: true,
    retryGatewayErrors: true,
    hostUpdating: hostUpdating,
    maxWait: limit,
    waitHost: hostReady == null
        ? null
        : () => hostReady.waitHostReady(token, machine.id),
    hostAwayWait: hostAwayWait,
    onHostAway: onHostAway,
  );
}

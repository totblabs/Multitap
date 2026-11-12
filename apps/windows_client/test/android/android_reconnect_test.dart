import 'dart:async';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_reconnect.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';

BrokerDesktop _desktop({String id = 'pve-149', required bool host}) =>
    BrokerDesktop(
      id: id,
      name: 'TESTER',
      kind: 'vm',
      description: '',
      os: 'Windows',
      status: host ? 'ready' : 'host_offline',
      canConnect: host,
      hostConnected: host,
      power: BrokerPowerSnapshot(
        state: 'running',
        stale: false,
        checkedAt: DateTime.utc(2026, 11, 3),
      ),
    );

/// The list answers [list]; host-ready answers [ready] when it is set.
class _Broker implements BrokerApi, HostReadyBrokerApi {
  List<BrokerDesktop> Function() list = () => [_desktop(host: false)];
  Completer<bool?>? ready;
  final calls = <String>[];

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    calls.add('list');
    return list();
  }

  @override
  Future<bool?> waitHostReady(String token, String desktopId) {
    calls.add('host-ready:$desktopId');
    return ready?.future ?? Future.value(null);
  }

  @override
  dynamic noSuchMethod(Invocation invocation) =>
      throw UnimplementedError(invocation.memberName.toString());
}

void main() {
  testWidgets('the restart wait asks host-ready for this desktop and reads '
      'the list once after its answer', (tester) async {
    final broker = _Broker()..ready = Completer<bool?>();
    var done = false;
    unawaited(
      androidAwaitBrokerRestart(
        broker,
        'token',
        machineFromBroker(_desktop(host: true)),
        current: () => true,
        limit: const Duration(seconds: 30),
      ).then((_) => done = true),
    );
    await tester.pump(const Duration(seconds: 3));
    expect(broker.calls, ['list', 'host-ready:pve-149']);
    broker.list = () => [_desktop(host: true)];
    broker.ready!.complete(true);
    await tester.pump();
    expect(broker.calls, ['list', 'host-ready:pve-149', 'list']);
    expect(done, isTrue);
  });

  testWidgets('a desktop missing from the restarted broker is a final '
      'refusal', (tester) async {
    final broker = _Broker()..list = () => [_desktop(id: 'pve-150', host: true)];
    Object? failure;
    unawaited(
      androidAwaitBrokerRestart(
        broker,
        'token',
        machineFromBroker(_desktop(host: true)),
        current: () => true,
        limit: const Duration(seconds: 30),
      ).catchError((Object error) => failure = error),
    );
    await tester.pump();
    expect(failure, isA<BrokerException>());
    expect(androidReconnectRefusalFinal(failure!), isTrue);
  });

  test('reconnect waits back off 1, 2, 4, 8 s, capped, at most a fifth '
      'shorter by jitter', () {
    const steps = [1, 2, 4, 8];
    Duration wait(int attempt, double jitter) =>
        androidReconnectDelay(steps, attempt, jitter);
    expect([
      for (var i = 0; i < 6; i++) wait(i, 0).inMilliseconds,
    ], [1000, 2000, 4000, 8000, 8000, 8000]);
    expect(wait(0, 1).inMilliseconds, 800);
    expect(wait(2, 0.5).inMilliseconds, 3600);
    expect(wait(-1, 0).inMilliseconds, 1000);
    expect(wait(1, double.nan).inMilliseconds, 2000);
    expect(wait(1, 7).inMilliseconds, 1600, reason: 'jitter is clamped');
  });

  test('only an unreachable broker is a wait for the network', () {
    expect(androidReconnectOffline(const BrokerException('network_error')), isTrue);
    expect(
      androidReconnectOffline(const BrokerException('network_error', 0, 'dns')),
      isTrue,
    );
    expect(androidReconnectOffline(TimeoutException('x')), isTrue);
    expect(androidReconnectOffline(const SocketException('x')), isTrue);
    for (final answered in const [
      BrokerException('host_not_registered', 409),
      BrokerException('stream_unavailable', 409),
      BrokerException('server_restarting', 503),
      BrokerException('host_maintenance'),
    ]) {
      expect(androidReconnectOffline(answered), isFalse, reason: answered.code);
    }
  });

  test('refusals of the request or of access end the tries; a Host or '
      'broker state does not', () {
    for (final status in const [400, 401, 403, 404, 410, 422]) {
      expect(
        androidReconnectRefusalFinal(BrokerException('refused', status)),
        isTrue,
        reason: '$status',
      );
    }
    for (final status in const [0, 409, 429, 500, 502, 503]) {
      expect(
        androidReconnectRefusalFinal(BrokerException('refused', status)),
        isFalse,
        reason: '$status',
      );
    }
    expect(androidReconnectRefusalFinal(TimeoutException('x')), isFalse);
  });
}

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_settings_api.dart';

class _RpcProcess extends Fake implements Process {
  final incoming = StreamController<List<int>>(),
      outgoing = StreamController<List<int>>();
  final ended = Completer<int>();
  final writes = <int>[];
  int kills = 0;
  late final IOSink input = IOSink(incoming.sink);
  _RpcProcess() {
    incoming.stream.listen(writes.addAll);
  }
  @override
  IOSink get stdin => input;
  @override
  Stream<List<int>> get stdout => outgoing.stream;
  @override
  Stream<List<int>> get stderr => const Stream.empty();
  @override
  Future<int> get exitCode => ended.future;
  @override
  bool kill([ProcessSignal signal = ProcessSignal.sigterm]) {
    kills++;
    finish({'ok': false}, 1);
    return true;
  }

  void finish(Map<String, dynamic> response, [int code = 0]) {
    if (ended.isCompleted) return;
    outgoing.add(utf8.encode(jsonEncode(response)));
    unawaited(outgoing.close());
    ended.complete(code);
  }
}

void main() {
  for (final action in [
    'restartServiceV1',
    'cancelConnectionV1',
    'clearWindowsLoginV1',
  ]) {
    testWidgets(
      '$action observer timeout keeps the owned mutation process alive',
      (t) async {
        final process = _RpcProcess();
        var starts = 0;
        Object? failure;
        final api = NativeHostSettingsApi(
          executable: 'synthetic-only',
          processStarter: (_) async {
            starts++;
            return process;
          },
        );
        unawaited(
          api
              .request(action, {
                'protocol': switch (action) {
                  'restartServiceV1' => 'service-restart-v1',
                  'clearWindowsLoginV1' => SavedWindowsLoginStatus.protocol,
                  _ => 'cancel-request-v1',
                },
                if (action == 'restartServiceV1') 'confirmed': true,
                if (action != 'restartServiceV1') 'revision': 'a' * 64,
              })
              .then<void>(
                (_) {},
                onError: (Object e) {
                  failure = e;
                },
              ),
        );
        await t.pump();
        await t.pump(
          hostSettingsRequestTimeout(action) + const Duration(seconds: 1),
        );
        expect(failure, isA<HostSettingsException>());
        expect(process.kills, 0);
        expect(starts, 1);
        expect(process.ended.isCompleted, isFalse);
        process.finish({'ok': true});
        await t.pump();
        expect(starts, 1);
        expect(process.kills, 0);
        final request = jsonDecode(utf8.decode(process.writes)) as Map;
        expect(request['action'], action);
      },
    );
  }
  testWidgets('Short read timeout still retires only its own helper', (
    t,
  ) async {
    final process = _RpcProcess();
    Object? failure;
    final api = NativeHostSettingsApi(processStarter: (_) async => process);
    unawaited(
      api
          .request('status')
          .then<void>(
            (_) {},
            onError: (Object e) {
              failure = e;
            },
          ),
    );
    await t.pump();
    await t.pump(const Duration(seconds: 36));
    await t.pump();
    expect(process.kills, 1);
    expect(failure, isA<HostSettingsException>());
  });
  testWidgets(
    'Native incomplete restart keeps its fixed diagnostic code and short next action',
    (t) async {
      final process = _RpcProcess();
      Object? failure;
      final api = NativeHostSettingsApi(processStarter: (_) async => process);
      unawaited(
        api
            .request('restartServiceV1')
            .then<void>(
              (_) {},
              onError: (Object e) {
                failure = e;
              },
            ),
      );
      await t.pump();
      process.finish({
        'ok': false,
        'error': 'host_service_restart_unverified',
        'restart': {'completed': false},
      }, 1);
      await t.pump();
      await t.pump();
      expect(
        (failure as HostSettingsException).code,
        'host_service_restart_unverified',
      );
      expect('$failure', contains('상태를 새로고침'));
      expect(process.kills, 0);
    },
  );
}

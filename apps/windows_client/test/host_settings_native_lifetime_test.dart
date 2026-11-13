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
  // settings-progress-v1: the helper's step lines reach the window while the
  // request runs, in order; the reply is the last line and is read as before.
  test('A progress line is read strictly; anything else belongs to the reply', () {
    HostSettingsProgress? read(String line) =>
        HostSettingsProgress.parseLine('installWinfsp', line);
    final step = read('{"progress":{"step":"winfsp_package"}}')!;
    expect((step.action, step.step, step.done, step.total, step.fraction), (
      'installWinfsp',
      'winfsp_package',
      null,
      null,
      null,
    ));
    final measured = read(
      '{"progress":{"step":"winfsp_install","done":50,"total":200}}',
    )!;
    expect((measured.done, measured.total, measured.fraction), (50, 200, .25));
    // A measure that is not one is dropped; the step stays.
    for (final bad in [
      '{"progress":{"step":"winfsp_install","done":201,"total":200}}',
      '{"progress":{"step":"winfsp_install","done":-1,"total":200}}',
      '{"progress":{"step":"winfsp_install","done":1,"total":0}}',
      '{"progress":{"step":"winfsp_install","done":"1","total":2}}',
      '{"progress":{"step":"winfsp_install","done":1}}',
    ]) {
      final value = read(bad)!;
      expect(value.fraction, isNull, reason: bad);
      expect(value.step, 'winfsp_install');
    }
    // Not a progress line: the reply, another shape, free text as a step.
    for (final other in [
      '{"ok":true}',
      '{"ok":true,"progress":{"step":"winfsp_install"}}',
      '{"progress":{"step":"winfsp_install"},"ok":true}',
      '{"progress":"winfsp_install"}',
      '{"progress":{"step":"C:\\Users\\name"}}',
      '{"progress":{"step":"Winfsp Install"}}',
      '{"progress":{"step":""}}',
      '{"progress":{"step":42}}',
      '{"progress":{"step":"winfsp_install"}',
      ' {"progress":{"step":"winfsp_install"}}',
      '',
    ]) {
      expect(read(other), isNull, reason: other);
    }
  });
  testWidgets('Step lines arrive while the request runs; the reply is the last line', (
    t,
  ) async {
    final process = _RpcProcess();
    final api = NativeHostSettingsApi(processStarter: (_) async => process);
    final told = <String>[];
    api.progress.listen(
      (p) => told.add('${p.action}:${p.step}:${p.done}/${p.total}'),
    );
    Map<String, dynamic>? reply;
    unawaited(api.request('installWinfsp', {'confirmed': true}).then((r) => reply = r));
    await t.pump();
    void line(String text) => process.outgoing.add(utf8.encode('$text\n'));
    line('{"progress":{"step":"winfsp_package"}}');
    await t.pump();
    // Shown at once, long before the reply.
    expect(told, ['installWinfsp:winfsp_package:null/null']);
    expect(reply, isNull);
    // A line split across two reads is still one line.
    process.outgoing.add(utf8.encode('{"progress":{"step":"winfsp_ins'));
    await t.pump();
    expect(told, hasLength(1));
    process.outgoing.add(utf8.encode('tall","done":1,"total":4}}\r\n'));
    await t.pump();
    expect(told.last, 'installWinfsp:winfsp_install:1/4');
    line('{"progress":{"step":"winfsp_check"}}');
    process.finish({
      'ok': true,
      'winfsp': {'action': 'installed', 'restart': false, 'error': ''},
    });
    await t.pump();
    await t.pump();
    expect(told, [
      'installWinfsp:winfsp_package:null/null',
      'installWinfsp:winfsp_install:1/4',
      'installWinfsp:winfsp_check:null/null',
    ]);
    expect(reply, {
      'ok': true,
      'winfsp': {'action': 'installed', 'restart': false, 'error': ''},
    });
    expect(process.kills, 0);
  });
  testWidgets('A helper that tells nothing is read as before, and step lines never hide a failure', (
    t,
  ) async {
    final silent = _RpcProcess();
    final api = NativeHostSettingsApi(processStarter: (_) async => silent);
    final told = <HostSettingsProgress>[];
    api.progress.listen(told.add);
    Map<String, dynamic>? reply;
    unawaited(api.request('status').then((r) => reply = r));
    await t.pump();
    silent.finish({'ok': true, 'status': <String, dynamic>{}});
    await t.pump();
    await t.pump();
    expect(reply, {'ok': true, 'status': <String, dynamic>{}});
    expect(told, isEmpty);

    final failing = _RpcProcess();
    final second = NativeHostSettingsApi(processStarter: (_) async => failing);
    Object? failure;
    unawaited(
      second
          .request('installWinfsp', {'confirmed': true})
          .then<void>((_) {}, onError: (Object e) => failure = e),
    );
    await t.pump();
    failing.outgoing.add(utf8.encode('{"progress":{"step":"winfsp_install"}}\n'));
    failing.finish({'ok': false, 'error': '관리자 권한이 필요해요.'}, 1);
    await t.pump();
    await t.pump();
    expect('$failure', '관리자 권한이 필요해요.');

    // Only step lines and no reply at all is not a reply.
    final cut = _RpcProcess();
    final third = NativeHostSettingsApi(processStarter: (_) async => cut);
    Object? unread;
    unawaited(
      third
          .request('installWinfsp', {'confirmed': true})
          .then<void>((_) {}, onError: (Object e) => unread = e),
    );
    await t.pump();
    cut.outgoing.add(utf8.encode('{"progress":{"step":"winfsp_install"}}\n'));
    unawaited(cut.outgoing.close());
    cut.ended.complete(1);
    await t.pump();
    await t.pump();
    expect(unread, isA<HostSettingsException>());
  });
}

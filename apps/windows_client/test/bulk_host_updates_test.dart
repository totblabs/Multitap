import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/bulk_host_updates.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/models/desktop_machine.dart';

const bulkTarget = '0.2.81';
final bulkRelease = ProductRelease(
  product: 'host',
  version: bulkTarget,
  filename: 'Multitap-Host-$bulkTarget-windows-x64.exe',
  size: 10,
  sha256: 'a' * 64,
  url: Uri.parse('https://fixture.invalid/downloads/host.exe'),
  changelog: 'USB와 클립보드 안정성을 개선했어요.\n설치 후 실제 상태를 확인하세요.',
);

HostUpdateState bulkState({
  String? version = '0.2.80',
  String? latest = bulkTarget,
  bool present = true,
  bool connected = true,
  bool supported = true,
  bool newer = true,
  bool available = true,
  bool sessionActive = false,
  HostUpdateOperation? operation,
}) => HostUpdateState(
  present: present,
  connected: connected,
  supported: supported,
  version: version,
  latestVersion: latest,
  newer: newer,
  available: available,
  sessionActive: sessionActive,
  operation: operation,
);
HostUpdateOperation bulkOperation(
  String status, {
  String code = '',
  bool reboot = false,
  String target = bulkTarget,
  int tick = 1,
}) => HostUpdateOperation(
  status: status,
  targetVersion: target,
  code: code,
  rebootRequired: reboot,
  checkedAt: DateTime.utc(2026, 10, 27, 10, 0, tick),
);
DesktopMachine bulkMachine(
  String id, {
  HostUpdateState? state,
  String? name,
  MachineKind kind = MachineKind.vm,
}) => DesktopMachine(
  id: id,
  name: name ?? 'VM ${id.replaceFirst('pve-', '')}',
  description: '',
  kind: kind,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.amber,
  hostUpdate: state ?? bulkState(),
);

class BulkCall {
  BulkCall(this.id, this.version, this.disconnect);
  final String id, version;
  final bool disconnect;
}

class FakeBulkApi implements HostUpdateBrokerApi {
  final calls = <BulkCall>[];
  int active = 0, maxActive = 0;
  bool tokensMatched = true;
  Future<HostUpdateState> Function(BulkCall call, int index)? handler;
  @override
  Future<HostUpdateState> requestHostUpdate(
    String token,
    String desktopId,
    String version, {
    bool disconnectSessions = false,
  }) async {
    tokensMatched &= token == 'in-memory-token';
    final call = BulkCall(desktopId, version, disconnectSessions);
    calls.add(call);
    active++;
    if (active > maxActive) maxActive = active;
    try {
      return handler == null
          ? bulkState(operation: bulkOperation('accepted'))
          : await handler!(call, calls.length - 1);
    } finally {
      active--;
    }
  }
}

BulkHostUpdateController bulkController(
  FakeBulkApi api,
  List<DesktopMachine> machines, {
  Duration timeout = const Duration(seconds: 35),
}) => BulkHostUpdateController(
  api: api,
  token: 'in-memory-token',
  release: bulkRelease,
  assignedMachines: machines,
  requestTimeout: timeout,
  minimumRequestInterval: Duration.zero,
);
Future<void> bulkTick() => Future<void>.delayed(Duration.zero);

void main() {
  test('Default deny, exact latest and explicit selection keep requests within assigned VMs', () async {
    for (final state in [
      HostUpdateState.absent,
      bulkState(connected: false),
      bulkState(supported: false),
      bulkState(available: false),
      bulkState(version: null),
      bulkState(version: bulkTarget, newer: false),
      bulkState(latest: '0.2.80'),
      bulkState(operation: bulkOperation('waiting')),
    ]) {
      final api = FakeBulkApi();
      final c = bulkController(api, [bulkMachine('pve-149', state: state)]);
      c.selectAll(true);
      await c.submitSelected();
      expect(api.calls, isEmpty);
      c.dispose();
    }
    final api = FakeBulkApi();
    final c = bulkController(api, [
      bulkMachine('pve-149'),
      bulkMachine('pve-149'),
      bulkMachine('pve-170'),
    ]);
    c.select('unassigned', true);
    expect(c.selectedCount, 0);
    c.selectAll(true);
    await c.submitSelected();
    await c.submitSelected();
    c.selectAll(true);
    await c.submitSelected();
    expect(api.calls.map((r) => r.id), ['pve-149', 'pve-170']);
    // A bulk update ends the desktops' connections now (2026-11-03).
    expect(
      api.calls.every((r) => r.version == bulkTarget && r.disconnect),
      isTrue,
    );
    expect(api.tokensMatched, isTrue);
    expect(api.maxActive, 1);
    c.dispose();
  });

  test(
    'Local PC Hosts are candidates like VMs and say what they are',
    () async {
      final api = FakeBulkApi();
      final c = bulkController(api, [
        bulkMachine('pve-149'),
        bulkMachine('pc-gaming', name: 'TOWER-PC', kind: MachineKind.pc),
        bulkMachine(
          'pc-office',
          kind: MachineKind.pc,
          state: bulkState(supported: false),
        ),
      ]);
      expect(c.rows.map((r) => r.eligible), [true, true, false]);
      expect(c.rows.last.message, '원격 업데이트 미지원 · 데스크톱에서 직접 설치해야 해요');
      c.selectAll(true);
      expect(c.selectedCount, 2);
      await c.submitSelected();
      expect(api.calls.map((r) => r.id), ['pve-149', 'pc-gaming']);
      expect(api.calls.every((r) => r.disconnect), isTrue);
      c.dispose();
    },
  );

  test('Another account\'s connection: the row says so and the update waits for it', () async {
    final api = FakeBulkApi()
      ..handler = (call, index) async {
        if (call.id == 'pve-149' && call.disconnect) {
          throw const BrokerException('desktop_busy', 409, 'other_user', false);
        }
        return bulkState(
          sessionActive: call.id == 'pve-149',
          operation: bulkOperation(
            call.id == 'pve-149' ? 'waiting' : 'accepted',
            code: call.id == 'pve-149' ? 'session_active' : '',
          ),
        );
      };
    final c = bulkController(api, [
      bulkMachine('pve-149', state: bulkState(sessionActive: true)),
      bulkMachine('pve-170'),
    ]);
    expect(c.rows.first.message, '사용 중 · 요청하면 연결을 끊어요');
    c.selectAll(true);
    await c.submitSelected();
    expect(
      [for (final call in api.calls) (call.id, call.disconnect)],
      [('pve-149', true), ('pve-149', false), ('pve-170', true)],
    );
    expect(c.rows.first.phase, BulkHostPhase.waiting);
    expect(c.rows.first.otherUser, isTrue);
    expect(c.rows.first.message, '다른 사용자의 연결이 끝나면 업데이트해요');
    expect(c.rows.last.phase, BulkHostPhase.acknowledged);
    expect(api.maxActive, 1);
    c.dispose();

    // Any other refusal is that row's result; nothing is sent again.
    final busy = FakeBulkApi()
      ..handler = (_, _) async =>
          throw const BrokerException('desktop_busy', 409);
    final other = bulkController(busy, [bulkMachine('pve-149')]);
    other.selectAll(true);
    await other.submitSelected();
    expect(busy.calls.length, 1);
    expect(other.rows.single.phase, BulkHostPhase.failed);
    other.dispose();
  });

  test('Each requested Host shows a short stage and the batch ends with a result summary', () async {
    final api = FakeBulkApi();
    final c = bulkController(api, [
      bulkMachine('pve-149'),
      bulkMachine('pve-170'),
      bulkMachine('pve-247', state: bulkState(connected: false)),
    ]);
    expect(c.summary, isNull);
    expect(c.rows.map((r) => r.stage), ['업데이트 가능', '업데이트 가능', '대상 아님']);
    expect(c.rows.last.message, 'Host 오프라인');
    c.selectAll(true);
    await c.submitSelected();
    expect(c.rows.take(2).map((r) => r.stage), ['대기', '대기']);
    expect(c.summary, '진행 중 2대 · 완료 0대 · 실패 0대');
    c.synchronize([
      bulkMachine(
        'pve-149',
        state: bulkState(
          operation: HostUpdateOperation(
            status: 'downloading',
            targetVersion: bulkTarget,
            progress: 40,
            checkedAt: DateTime.utc(2026, 10, 27, 10, 0, 2),
          ),
        ),
      ),
      // The Host service restarts to finish installing: away, not failed.
      bulkMachine(
        'pve-170',
        state: bulkState(
          connected: false,
          operation: bulkOperation('installing', tick: 2),
        ),
      ),
      bulkMachine('pve-247', state: bulkState(connected: false)),
    ]);
    expect(c.rows.take(2).map((r) => r.stage), ['다운로드', '다시 연결']);
    expect(c.rows[0].message, '설치 파일 다운로드 40%');
    expect(c.rows[1].phase, BulkHostPhase.reconnecting);
    expect(c.hasServerWork, isTrue, reason: 'Reconnecting keeps polling.');
    expect(c.finished, isFalse);
    c.synchronize([
      bulkMachine(
        'pve-149',
        state: bulkState(
          operation: bulkOperation('failed', code: 'hash_mismatch', tick: 3),
        ),
      ),
      bulkMachine(
        'pve-170',
        state: bulkState(
          version: bulkTarget,
          newer: false,
          operation: bulkOperation('completed', tick: 3),
        ),
      ),
      bulkMachine('pve-247', state: bulkState(connected: false)),
    ]);
    expect(c.rows.take(2).map((r) => r.stage), ['실패', '완료']);
    expect(c.rows[0].message, '파일의 SHA-256이 배포 정보와 달라요');
    expect(c.finished, isTrue);
    expect(c.summary, '모두 끝났어요 · 완료 1대 · 실패 1대');
    expect(api.calls.length, 2);
    c.dispose();
  });

  test('A server waiting operation does not hold up another idle VM or imply installation', () async {
    final api = FakeBulkApi()
      ..handler = (call, index) async => bulkState(
        sessionActive: index == 0,
        operation: bulkOperation(index == 0 ? 'waiting' : 'accepted'),
      );
    final c = bulkController(api, [
      bulkMachine('pve-149', state: bulkState(sessionActive: true)),
      bulkMachine('pve-170'),
    ]);
    c.selectAll(true);
    await c.submitSelected();
    expect(api.calls.length, 2);
    expect(api.maxActive, 1);
    expect(c.rows.map((r) => r.phase), [
      BulkHostPhase.waiting,
      BulkHostPhase.acknowledged,
    ]);
    expect(c.completedCount, 0);
    expect(c.submitting, isFalse);
    expect(c.rows.first.message, contains('연결이 끝나면 업데이트해요'));
    c.dispose();
  });

  test('Observed version alone is not success; server terminal result, restart and checks stay distinct', () async {
    final cases = <(String, String, bool, BulkHostPhase)>[
      ('accepted', '', false, BulkHostPhase.acknowledged),
      ('completed', '', false, BulkHostPhase.completed),
      ('completed', '', true, BulkHostPhase.rebootRequired),
      ('completed', 'display_attention', false, BulkHostPhase.needsCheck),
      ('completed', 'result_unreported', false, BulkHostPhase.needsCheck),
      ('failed', 'install_result_missing', false, BulkHostPhase.failed),
      ('unknown', 'host_not_returned', false, BulkHostPhase.uncertain),
    ];
    for (final (status, code, reboot, phase) in cases) {
      final api = FakeBulkApi();
      final controller = bulkController(api, [bulkMachine('pve-149')]);
      controller.selectAll(true);
      await controller.submitSelected();
      controller.synchronize([
        bulkMachine(
          'pve-149',
          state: bulkState(
            version: bulkTarget,
            operation: bulkOperation(
              status,
              code: code,
              reboot: reboot,
              tick: 2,
            ),
          ),
        ),
      ]);
      expect(
        controller.rows.single.phase,
        phase,
        reason: '$status/$code/$reboot',
      );
      expect(api.calls.length, 1);
      controller.dispose();
    }
  });

  test('A definitive per-VM failure continues; authorization rejection stops unsent requests', () async {
    for (final status in [409, 403, 401]) {
      final api = FakeBulkApi()
        ..handler = (call, index) async {
          if (index == 0) {
            throw BrokerException(
              status == 409 ? 'host_offline' : 'forbidden',
              status,
            );
          }
          return bulkState(operation: bulkOperation('accepted'));
        };
      final c = bulkController(api, [
        bulkMachine('pve-149'),
        bulkMachine('pve-170'),
      ]);
      c.selectAll(true);
      await c.submitSelected();
      expect(api.calls.length, status == 409 ? 2 : 1);
      expect(c.rows.first.phase, BulkHostPhase.failed);
      expect(c.authorizationStopped, status != 409);
      if (status != 409) expect(c.rows.last.phase, BulkHostPhase.stopped);
      c.dispose();
    }
  });

  test('Network uncertainty is not retried and a fresh server observation can reconcile it', () async {
    final api = FakeBulkApi()
      ..handler = (_, _) async =>
          throw const SocketException('PRIVATE_SENTINEL');
    final c = bulkController(api, [bulkMachine('pve-149')]);
    c.selectAll(true);
    await c.submitSelected();
    expect(c.rows.single.phase, BulkHostPhase.uncertain);
    expect(c.rows.single.message, isNot(contains('PRIVATE_SENTINEL')));
    c.selectAll(true);
    await c.submitSelected();
    expect(api.calls.length, 1);
    c.synchronize([
      bulkMachine(
        'pve-149',
        state: bulkState(operation: bulkOperation('waiting', tick: 2)),
      ),
    ]);
    expect(c.rows.single.phase, BulkHostPhase.waiting);
    c.dispose();
  });

  test('A POST timeout pauses unsent work without overlapping or replaying its late request', () async {
    final gate = Completer<HostUpdateState>();
    final api = FakeBulkApi()..handler = (_, _) => gate.future;
    final c = bulkController(api, [
      bulkMachine('pve-149'),
      bulkMachine('pve-170'),
    ], timeout: const Duration(milliseconds: 10));
    c.selectAll(true);
    await c.submitSelected();
    expect(c.rows.first.phase, BulkHostPhase.uncertain);
    expect(c.rows.last.phase, BulkHostPhase.stopped);
    expect(api.calls.length, 1);
    expect(api.maxActive, 1);
    await c.submitSelected();
    expect(
      api.calls.length,
      1,
      reason:
          'an unresolved underlying POST must not overlap an explicit resume',
    );
    gate.complete(bulkState(operation: bulkOperation('accepted')));
    await bulkTick();
    expect(c.rows.first.phase, BulkHostPhase.uncertain);
    c.dispose();
  });

  test('Stop and dispose affect only unsent rows, keeping an in-flight accepted operation', () async {
    for (final dispose in [false, true]) {
      final gate = Completer<HostUpdateState>();
      final api = FakeBulkApi()..handler = (_, _) => gate.future;
      final c = bulkController(api, [
        bulkMachine('pve-149'),
        bulkMachine('pve-170'),
      ]);
      c.selectAll(true);
      final pending = c.submitSelected();
      await bulkTick();
      if (dispose) {
        c.dispose();
      } else {
        c.stopPending();
      }
      gate.complete(bulkState(operation: bulkOperation('waiting')));
      await pending;
      expect(api.calls.length, 1);
      expect(c.rows.first.phase, BulkHostPhase.waiting);
      expect(c.rows.last.phase, BulkHostPhase.stopped);
      if (!dispose) c.dispose();
    }
  });

  test('Assignment revocation and a changed latest version stop unsubmitted rows without retargeting', () async {
    for (final newer in [false, true]) {
      final gate = Completer<HostUpdateState>();
      final api = FakeBulkApi()..handler = (_, _) => gate.future;
      final c = bulkController(api, [
        bulkMachine('pve-149'),
        bulkMachine('pve-170'),
      ]);
      c.selectAll(true);
      final pending = c.submitSelected();
      await bulkTick();
      c.synchronize([
        bulkMachine(
          'pve-149',
          state: bulkState(latest: newer ? '0.2.82' : bulkTarget),
        ),
      ]);
      gate.complete(bulkState(operation: bulkOperation('accepted')));
      await pending;
      expect(api.calls.map((call) => call.id), ['pve-149']);
      expect(api.calls.single.version, bulkTarget);
      expect(c.catalogChanged, newer);
      expect(c.rows.last.assigned, isFalse);
      c.dispose();
    }
  });

  test('A newer server terminal observation cannot regress when an older POST acknowledgement arrives', () async {
    final gate = Completer<HostUpdateState>();
    final api = FakeBulkApi()..handler = (_, _) => gate.future;
    final c = bulkController(api, [bulkMachine('pve-149')]);
    c.selectAll(true);
    final pending = c.submitSelected();
    await bulkTick();
    c.synchronize([
      bulkMachine(
        'pve-149',
        state: bulkState(
          version: bulkTarget,
          operation: bulkOperation('completed', tick: 4),
        ),
      ),
    ]);
    expect(c.rows.single.phase, BulkHostPhase.completed);
    gate.complete(bulkState(operation: bulkOperation('accepted', tick: 2)));
    await pending;
    expect(c.rows.single.phase, BulkHostPhase.completed);
    c.dispose();
  });

  test(
    'An old completed operation cannot stand in for the newly submitted update',
    () async {
      final api = FakeBulkApi();
      final c = bulkController(api, [
        bulkMachine(
          'pve-149',
          state: bulkState(operation: bulkOperation('completed', tick: 1)),
        ),
      ]);
      c.selectAll(true);
      await c.submitSelected();
      expect(c.rows.single.phase, BulkHostPhase.acknowledged);
      expect(c.completedCount, 0);
      c.dispose();
    },
  );

  test('Any catalog mismatch in an acknowledgement stops remaining work without retargeting', () async {
    for (final latest in ['0.2.82', '0.2.80']) {
      final api = FakeBulkApi()
        ..handler = (_, _) async =>
            bulkState(latest: latest, operation: bulkOperation('accepted'));
      final c = bulkController(api, [
        bulkMachine('pve-149'),
        bulkMachine('pve-170'),
      ]);
      c.selectAll(true);
      await c.submitSelected();
      expect(api.calls.length, 1);
      expect(c.catalogChanged, isTrue);
      expect(api.calls.single.version, bulkTarget);
      expect(c.rows.first.phase, BulkHostPhase.acknowledged);
      c.dispose();
    }
  });

  test('A catalog rollback observation stops the frozen batch too', () async {
    final api = FakeBulkApi();
    final c = bulkController(api, [bulkMachine('pve-149')]);
    c.selectAll(true);
    c.synchronize([bulkMachine('pve-149', state: bulkState(latest: '0.2.80'))]);
    await c.submitSelected();
    expect(c.catalogChanged, isTrue);
    expect(api.calls, isEmpty);
    c.dispose();
  });

  test('Two hundred VMs respect the existing twenty-per-minute limiter with a monotonic fake clock', () async {
    var elapsed = Duration.zero;
    final starts = <Duration>[];
    final api = FakeBulkApi()
      ..handler = (_, _) async {
        starts.add(elapsed);
        return bulkState(operation: bulkOperation('waiting'));
      };
    final c = BulkHostUpdateController(
      api: api,
      token: 'in-memory-token',
      release: bulkRelease,
      assignedMachines: [
        for (var i = 0; i < 200; i++) bulkMachine('pve-${1000 + i}'),
      ],
      elapsed: () => elapsed,
      pause: (duration) async {
        elapsed += duration;
      },
    );
    c.selectAll(true);
    await c.submitSelected();
    expect(api.calls.length, 200);
    expect(api.maxActive, 1);
    for (var i = 1; i < starts.length; i++) {
      expect(
        starts[i] - starts[i - 1],
        greaterThanOrEqualTo(const Duration(milliseconds: 3100)),
      );
    }
    for (final begin in starts) {
      expect(
        starts
            .where(
              (at) => at >= begin && at < begin + const Duration(minutes: 1),
            )
            .length,
        lessThanOrEqualTo(20),
      );
    }
    expect(c.rows.every((r) => r.phase == BulkHostPhase.waiting), isTrue);
    expect(c.completedCount, 0);
    c.dispose();
  });

  test(
    'Only the exact pre-accept limiter response gets bounded delayed retry',
    () async {
      var elapsed = Duration.zero;
      final pauses = <Duration>[];
      final api = FakeBulkApi()
        ..handler = (_, index) async {
          if (index == 0) {
            throw const BrokerException('too_many_host_update_attempts', 429);
          }
          return bulkState(operation: bulkOperation('accepted'));
        };
      final c = BulkHostUpdateController(
        api: api,
        token: 'in-memory-token',
        release: bulkRelease,
        assignedMachines: [bulkMachine('pve-149'), bulkMachine('pve-170')],
        elapsed: () => elapsed,
        pause: (duration) async {
          pauses.add(duration);
          elapsed += duration;
        },
      );
      c.selectAll(true);
      await c.submitSelected();
      expect(api.calls.map((r) => r.id), ['pve-149', 'pve-149', 'pve-170']);
      expect(pauses.first, const Duration(seconds: 60));
      expect(api.maxActive, 1);
      expect(c.requestedCount, 2);
      expect(c.rows.first.phase, BulkHostPhase.acknowledged);
      c.dispose();

      final unknown = FakeBulkApi()
        ..handler = (_, _) async =>
            throw const BrokerException('proxy_rate_limited', 429);
      final other = bulkController(unknown, [bulkMachine('pve-149')]);
      other.selectAll(true);
      await other.submitSelected();
      expect(unknown.calls.length, 1);
      expect(other.rows.single.phase, BulkHostPhase.failed);
      other.dispose();
    },
  );

  test('Rate-limit retry is bounded and the known-unaccepted VM is not permanently deduplicated', () async {
    var elapsed = Duration.zero;
    final api = FakeBulkApi()
      ..handler = (_, _) async =>
          throw const BrokerException('too_many_host_update_attempts', 429);
    final c = BulkHostUpdateController(
      api: api,
      token: 'in-memory-token',
      release: bulkRelease,
      assignedMachines: [bulkMachine('pve-149')],
      elapsed: () => elapsed,
      pause: (duration) async {
        elapsed += duration;
      },
    );
    c.selectAll(true);
    await c.submitSelected();
    expect(api.calls.length, 3);
    expect(c.rows.single.attempted, isFalse);
    expect(c.rows.single.eligible, isTrue);
    expect(c.rows.single.phase, BulkHostPhase.failed);
    c.dispose();
  });

  test('Stop interrupts both ordinary pacing and an unaccepted rate-limit wait without a further POST', () async {
    for (final rateLimited in [false, true]) {
      final waiting = Completer<void>();
      var elapsed = Duration.zero;
      final api = FakeBulkApi()
        ..handler = (_, _) async {
          if (rateLimited) {
            throw const BrokerException('too_many_host_update_attempts', 429);
          }
          return bulkState(operation: bulkOperation('waiting'));
        };
      final c = BulkHostUpdateController(
        api: api,
        token: 'in-memory-token',
        release: bulkRelease,
        assignedMachines: [bulkMachine('pve-149'), bulkMachine('pve-170')],
        elapsed: () => elapsed,
        pause: (_) => waiting.future,
      );
      c.selectAll(true);
      final pending = c.submitSelected();
      for (var i = 0; i < 10 && !c.pacing; i++) {
        await bulkTick();
      }
      expect(c.pacing, isTrue);
      expect(api.calls.length, 1);
      if (rateLimited) expect(c.rows.first.phase, BulkHostPhase.rateLimited);
      c.stopPending();
      await pending;
      expect(api.calls.length, 1);
      expect(c.rows.last.phase, BulkHostPhase.stopped);
      waiting.complete();
      elapsed += const Duration(minutes: 1);
      await bulkTick();
      c.dispose();
    }
  });

  test('Server latest-only rejection halts remaining requests and never silently uses a different version', () async {
    final api = FakeBulkApi()
      ..handler = (_, _) async =>
          throw const BrokerException('host_release_unavailable', 409);
    final c = bulkController(api, [
      bulkMachine('pve-149'),
      bulkMachine('pve-170'),
    ]);
    c.selectAll(true);
    await c.submitSelected();
    expect(c.catalogChanged, isTrue);
    expect(api.calls.length, 1);
    expect(c.rows.last.phase, BulkHostPhase.stopped);
    c.dispose();
  });
}

// A Host update restarts the Host service on purpose (user screenshot,
// 2026-11-02: "Host 연결이 끊겼어요" + 새로고침 during a bulk update read as
// a fault). Readiness only; no broker, Host or VM.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/host_update.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';

void main() {
  final reported = DateTime.utc(2026, 11, 2, 6, 0);
  DesktopMachine lost(String status, {int? progress}) => DesktopMachine(
    id: 'pve-247',
    name: 'WorkPC',
    description: '',
    kind: MachineKind.vm,
    status: MachineStatus.awaitingHost,
    specification: 'Windows',
    accent: Colors.blue,
    powerState: 'running',
    powerStale: false,
    hostOnline: false,
    vmState: 'host_lost',
    hostUpdate: HostUpdateState(
      present: true,
      operation: HostUpdateOperation(
        status: status,
        targetVersion: '0.2.138',
        progress: progress,
        checkedAt: reported,
      ),
    ),
  );

  test(
    'While the update runs, a lost Host is progress, not a warning; past the bound it is a warning again',
    () {
      final during = DesktopReadiness.of(
        lost('installing'),
        now: reported.add(const Duration(minutes: 2)),
      );
      expect(during.phase, TilePhase.waiting);
      expect(during.headline, 'Host 업데이트 중');
      expect(during.detail, '설치 중이에요. 끝나면 다시 연결할 수 있어요.');
      expect(during.primary, isNull, reason: 'no 새로고침');
      expect(during.hostUpdating, isTrue);

      final download = DesktopReadiness.of(
        lost('downloading', progress: 42),
        now: reported,
      );
      expect(download.detail, '다운로드 중이에요 (42%). 끝나면 다시 연결할 수 있어요.');

      final late = DesktopReadiness.of(
        lost('installing'),
        now: reported.add(const Duration(minutes: 6)),
      );
      expect(late.phase, TilePhase.attention);
      expect(late.headline, 'Host 연결이 끊겼어요');
      expect(late.primary, ReadinessAction.refresh);
      expect(late.hostUpdating, isFalse);
    },
  );

  test('The update line reads as a sentence, not "중… 설치"', () {
    expect(lost('installing').hostUpdate.tileStatus(reported), 'Host 업데이트 중 · 설치');
  });
}

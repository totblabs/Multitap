import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';

void main() {
  test('physical PC launch rejects unapproved PC and malformed IDs', () {
    final base = SessionWindowLaunch(
      origin: Uri.parse('https://console.invalid'),
      token: 'synthetic',
      expiresAt: DateTime.utc(2035),
      desktopId: 'pve-149',
      name: '시험 VM',
      mode: SessionDisplayMode.windowed,
      options: StreamOptions.recommended,
    ).toMap();
    expect(SessionWindowLaunch.fromMap(base).desktopId, 'pve-149');
    const pc = 'pc-01234567-89ab-cdef-8123-0123456789ab';
    expect(
      SessionWindowLaunch.fromMap({
        ...base,
        'desktopId': pc,
        'localPcApproved': true,
      }).desktopId,
      pc,
    );
    expect(
      () => SessionWindowLaunch.fromMap({...base, 'desktopId': pc}),
      throwsFormatException,
    );
    expect(
      () => SessionWindowLaunch.fromMap({...base, 'desktopId': 'pve-149/other'}),
      throwsFormatException,
    );
  });

  test('PC preserves approved marker through refresh helpers but never offers Proxmox power', () {
    const pc = DesktopMachine(
      id: 'pc-01234567-89ab-cdef-8123-0123456789ab',
      name: '시험 PC',
      description: '',
      kind: MachineKind.pc,
      status: MachineStatus.ready,
      specification: 'Windows',
      accent: Colors.amber,
      localPcApproved: true,
      powerStale: false,
      powerState: 'running',
      powerCanForceStop: true,
    );
    expect(pc.canShutdown, isFalse);
    expect(pc.canStop, isFalse);
    final updated = pc.withPower(
      state: 'stopped',
      checkedAt: DateTime.utc(2030),
      stale: false,
      busy: false,
    );
    expect(updated.canStart, isFalse);
    expect(updated.localPcApproved, isTrue);
    expect(updated.withResources(pc.resources).localPcApproved, isTrue);
    expect(pc.withDisplay(pc.display).localPcApproved, isTrue);
    expect(pc.withOrganize(pc.organize).localPcApproved, isTrue);
    expect(pc.withHostUpdate(pc.hostUpdate).localPcApproved, isTrue);
  });

  test('PC stream requires approved marker', () {
    final pc = <String, dynamic>{
      'id': 'pc-01234567-89ab-cdef-8123-0123456789ab',
      'name': 'PC',
      'kind': 'pc',
      'description': '',
      'os': 'Windows',
      'status': 'ready',
      'canConnect': true,
    };
    expect(machineFromBroker(BrokerDesktop.fromJson(pc)).isReady, isFalse);
    final approved = machineFromBroker(
      BrokerDesktop.fromJson({...pc, 'localPcApproved': true}),
    );
    expect(approved.isReady, isTrue);
    final unknownPower = machineFromBroker(
      BrokerDesktop.fromJson({
        ...pc,
        'localPcApproved': true,
        'power': {'state': 'unknown', 'stale': true},
      }),
    );
    expect(unknownPower.isReady, isTrue);
    expect(unknownPower.canShutdown, isFalse);
  });
}

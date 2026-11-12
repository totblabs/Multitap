import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/emergency_console_session.dart';
import 'package:multitap_client/data/console_keyboard.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/emergency_console_view.dart';

void main() {
  test('console/physical PC launch preserves safe mode and rejects unapproved PC and malformed IDs', () {
    final base = SessionWindowLaunch(
      origin: Uri.parse('https://console.invalid'),
      token: 'synthetic',
      expiresAt: DateTime.utc(2035),
      desktopId: 'pve-149',
      name: '시험 VM',
      mode: SessionDisplayMode.windowed,
      options: StreamOptions.recommended,
      console: true,
    ).toMap();
    expect(SessionWindowLaunch.fromMap(base).console, isTrue);
    const pc = 'pc-01234567-89ab-cdef-8123-0123456789ab';
    expect(
      () => SessionWindowLaunch.fromMap({...base, 'desktopId': pc}),
      throwsFormatException,
    );
    expect(
      SessionWindowLaunch.fromMap({
        ...base,
        'desktopId': pc,
        'console': false,
        'localPcApproved': true,
      }).desktopId,
      pc,
    );
    expect(
      () => SessionWindowLaunch.fromMap({
        ...base,
        'desktopId': pc,
        'console': false,
      }),
      throwsFormatException,
    );
  });

  test('PC preserves approved marker through refresh helpers but never offers Proxmox power/console', () {
    const pc = DesktopMachine(
      id: 'pc-01234567-89ab-cdef-8123-0123456789ab',
      name: '시험 PC',
      description: '',
      kind: MachineKind.pc,
      status: MachineStatus.ready,
      specification: 'Windows',
      accent: Colors.amber,
      localPcApproved: true,
      consoleRelayProtocol: 'rfb-none-v1',
      powerStale: false,
      powerState: 'running',
      powerCanForceStop: true,
    );
    expect(pc.emergencyConsoleSupported, isFalse);
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

  test('only broker console capability exposes VM mode; PC stream requires approved marker', () {
    final vm = <String, dynamic>{
      'id': 'pve-149',
      'name': 'VM',
      'kind': 'vm',
      'description': '',
      'os': 'Windows',
      'status': 'ready',
      'canConnect': true,
    };
    expect(
      machineFromBroker(BrokerDesktop.fromJson(vm)).emergencyConsoleSupported,
      isFalse,
    );
    expect(
      machineFromBroker(
        BrokerDesktop.fromJson({...vm, 'consoleRelayProtocol': 'rfb-none-v1'}),
      ).emergencyConsoleSupported,
      isTrue,
    );
    final pc = {
      ...vm,
      'id': 'pc-01234567-89ab-cdef-8123-0123456789ab',
      'kind': 'pc',
      'consoleRelayProtocol': 'rfb-none-v1',
    };
    expect(machineFromBroker(BrokerDesktop.fromJson(pc)).isReady, isFalse);
    final approved = machineFromBroker(
      BrokerDesktop.fromJson({...pc, 'localPcApproved': true}),
    );
    expect(approved.isReady, isTrue);
    expect(approved.emergencyConsoleSupported, isFalse);
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

  for (final size in [
    const Size(1920, 1080),
    const Size(1366, 768),
    const Size(960, 640),
  ]) {
    for (final brightness in Brightness.values) {
      testWidgets(
        'console loading/close layout $size $brightness enlarged text',
        (tester) async {
          tester.view.physicalSize = size;
          tester.view.devicePixelRatio = 1;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);
          final pending = Completer<BrokerConsoleGrant>();
          var closed = 0;
          final actions = SessionWindowActions(
            closeWindow: () async {
              closed++;
            },
          );
          await tester.pumpWidget(
            MaterialApp(
              theme: buildAppTheme(brightness: brightness),
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(context).copyWith(
                  textScaler: TextScaler.linear(size.width < 1500 ? 1.5 : 1),
                ),
                child: child!,
              ),
              home: EmergencyConsoleView(
                name: '가상 예시 VM',
                windowActions: actions,
                keyboard: NoConsoleKeyboardCapture(),
                create: () => EmergencyConsoleSession(
                  origin: Uri.parse('https://console.invalid'),
                  token: 'synthetic',
                  acquire: () => pending.future,
                  release: (_) async {},
                ),
              ),
            ),
          );
          await tester.pump();
          expect(tester.takeException(), isNull);
          expect(find.textContaining('Console 화면'), findsOneWidget);
          await tester.tap(find.byKey(const Key('leave-remote-connection')));
          await tester.pump();
          expect(closed, 1);
          await tester.pumpWidget(const SizedBox());
        },
      );
    }
  }
}

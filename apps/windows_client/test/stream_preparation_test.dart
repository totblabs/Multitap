import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';
import 'package:multitap_client/ui/workspace_shell.dart';
import 'helpers/pump_work.dart';

DesktopMachine machine(StreamPreparation? preparation, {bool ready = false}) =>
    DesktopMachine(
      id: 'pve-149',
      name: 'TESTER',
      description: '',
      kind: MachineKind.vm,
      status: ready ? MachineStatus.ready : MachineStatus.awaitingHost,
      specification: 'Windows',
      accent: Colors.blue,
      hostOnline: true,
      interactiveSession: true,
      virtualDisplayReady: ready,
      streamPreparation: preparation,
      powerState: 'running',
      powerStale: false,
    );

Map<String, dynamic> desktop(Object? reason) => {
  'id': 'pve-149',
  'name': 'TESTER',
  'kind': 'vm',
  'description': '',
  'os': 'Windows',
  'status': 'ready',
  'canConnect': true,
  'connectionModes': {'stream': false},
  'host': {
    'connected': true,
    'capabilities': {
      'streaming': false,
      'virtualDisplayReady': false,
      'monitors': [],
      'viewportFailureReason': reason,
    },
  },
};

void main() {
  test(
    'Broker retains fixed native reasons without making streaming available',
    () {
      for (final entry in {
        'reboot_required': StreamPreparation.restart,
        'interactive_console_required': StreamPreparation.console,
        'windows_login_required': StreamPreparation.login,
        'windows_identity_required': StreamPreparation.identity,
        'native_display_not_ready': StreamPreparation.display,
        'native_display_provider_unavailable': StreamPreparation.verification,
        'native_display_snapshot_invalid': StreamPreparation.recovery,
        'native_display_configuration_untrusted':
            StreamPreparation.verification,
        'native_display_topology_mismatch': StreamPreparation.recovery,
        'native_display_change_pending': StreamPreparation.display,
        'native_topology_not_ready': StreamPreparation.display,
        'native_topology_recovery_required': StreamPreparation.recovery,
      }.entries) {
        final parsed = BrokerDesktop.fromJson(desktop(entry.key));
        expect(parsed.streamPreparation, entry.value);
        expect(parsed.canStream, isFalse);
        expect(parsed.monitors, isEmpty);
      }
      for (final bad in [
        null,
        '',
        'PRIVATE_TOKEN=secret',
        'reboot_required\n',
        {'code': 'reboot_required'},
        ['reboot_required'],
        1,
        true,
      ]) {
        expect(BrokerDesktop.fromJson(desktop(bad)).streamPreparation, isNull);
      }
    },
  );

  test('A missing saved Windows identity stays actionable and never claims no login', () {
    final readiness = DesktopReadiness.of(machine(StreamPreparation.identity));
    expect(readiness.phase, TilePhase.attention);
    expect(readiness.headline, 'Windows 계정 확인이 필요해요');
    expect(readiness.detail, contains('Host 설정 → Windows 로그인'));
    expect(readiness.detail, isNot(contains('로그인 세션이 없어요')));
    expect(readiness.primary, ReadinessAction.refresh);
    expect(readiness.checks[2].action, isNull);
    expect(readiness.progressLabel, isNull);
    expect(readiness.canStream, isFalse);
  });

  test(
    'Power, resource and display events retain diagnosis until fresh inventory',
    () {
      final original = machine(StreamPreparation.restart);
      final changed = original
          .withResources(const VmResources())
          .withDisplay(const VirtualDisplayState())
          .withPower(
            state: 'running',
            checkedAt: DateTime.utc(2026),
            stale: false,
            busy: false,
          );
      expect(changed.streamReadinessBadge, '다시 시작 필요');
      expect(changed.streamReadiness, StreamPreparation.restart.detail);
      expect(changed.isReady, isFalse);
      final stopped = changed.withPower(
        state: 'stopped',
        checkedAt: DateTime.utc(2026),
        stale: false,
        busy: false,
      );
      expect(stopped.streamReadinessBadge, '전원 꺼짐');
      expect(stopped.streamReadiness, '데스크톱이 꺼져 있어요.');
      final ready = machine(StreamPreparation.checking, ready: true);
      expect(ready.streamReadinessBadge, isNull);
      expect(ready.streamReadiness, '지금은 연결을 시작할 수 없어요. 잠시 후 다시 시도하세요.');
    },
  );

  testWidgets(
    'Unready tiles show the fixed cause, keep one main button and do not grow',
    (t) async {
      t.view.physicalSize = const Size(640, 900);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      for (final brightness in Brightness.values) {
        Size? previous;
        for (final reason in [null, ...StreamPreparation.values]) {
          await t.pumpWidget(
            MaterialApp(
              theme: buildAppTheme(brightness: brightness),
              home: WorkspaceShell(
                key: UniqueKey(),
                onSignOut: () {},
                onRefresh: () {},
                displayName: 'tester',
                serverOrigin: 'https://cloud.example.com',
                machines: [machine(reason)],
                onConnect: (_, {options = const StreamOptions()}) async =>
                    throw StateError('fixture only'),
              ),
            ),
          );
          await pumpWork(t);
          final current = t.getSize(find.byKey(const Key('card-pve-149')));
          if (previous != null) expect(current, previous);
          previous = current;
          final readiness = DesktopReadiness.of(machine(reason));
          expect(readiness.canStream, isFalse);
          expect(find.text(readiness.headline), findsWidgets);
          if (readiness.phase == TilePhase.attention) {
            expect(find.byKey(const Key('open-pve-149')), findsNothing);
            // One main button: '다시 시작…' only for a restart-required
            // state, otherwise 새로고침; never 다시 시작.
            expect(
              t
                  .widget<FilledButton>(find.byKey(const Key('attention-pve-149')))
                  .onPressed,
              // This shell has no power or console handler, so a restart and
              // Console are greyed.
              readiness.primary == ReadinessAction.reboot ||
                      readiness.primary == ReadinessAction.console
                  ? isNull
                  : isNotNull,
            );
            expect(find.text('다시 시작'), findsNothing);
          } else {
            expect(
              t
                  .widget<FilledButton>(find.byKey(const Key('open-pve-149')))
                  .onPressed,
              isNull,
            );
          }
          expect(t.takeException(), isNull);
        }
      }
    },
  );
}

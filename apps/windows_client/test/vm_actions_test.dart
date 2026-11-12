import 'package:flutter/material.dart';
import 'package:flutter/semantics.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/vm_audio.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_menu.dart';
import 'package:multitap_client/ui/desktop_tile.dart' show DesktopTile;
import 'package:multitap_client/ui/workspace_shell.dart';

const machine = DesktopMachine(
  id: 'pve-149',
  name: 'VM',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.awaitingHost,
  specification: 'Windows',
  accent: Colors.blue,
  powerState: 'running',
  powerStale: false,
  hostOnline: false,
  vmAudio: true,
);

Future<void> showSurface(
  WidgetTester t,
  Widget child, {
  Size size = const Size(1200, 900),
  double scale = 1,
}) async {
  t.view.physicalSize = size;
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: MediaQuery(
        data: MediaQueryData(size: size, textScaler: TextScaler.linear(scale)),
        child: Scaffold(body: child),
      ),
    ),
  );
  await t.pump();
  await t.pump(const Duration(milliseconds: 300));
}

/// Opens the card's ⋯, checks it lists no sound-card row, and closes it.
Future<void> expectNoSoundCardRow(
  WidgetTester t, {
  String id = 'pve-149',
}) async {
  await t.tap(find.byKey(Key('desktop-more-$id')));
  await t.pumpAndSettle();
  expect(find.byKey(Key('details-$id')), findsOneWidget);
  expect(find.byKey(Key('vm-sound-card-$id')), findsNothing);
  expect(find.text('가상 사운드카드…'), findsNothing);
  await t.sendKeyEvent(LogicalKeyboardKey.escape);
  await t.pumpAndSettle();
  expect(find.byKey(Key('details-$id')), findsNothing);
}

void main() {
  testWidgets(
    'Waiting VM exposes emergency console under streaming arrow without implicit connection',
    (t) async {
      var opened = 0;
      await showSurface(
        t,
        WorkspaceShell(
          onSignOut: () {},
          onRefresh: () {},
          machines: const [machine],
          displayName: 'User',
          serverOrigin: 'https://fixture.invalid',
          onOpenConsole: (_) async {
            opened++;
            return const SessionWindowOpenResult(opened: true);
          },
        ),
      );
      expect(opened, 0);
      await t.tap(find.byKey(const Key('stream-modes-pve-149')));
      await t.pumpAndSettle();
      expect(
        find.byKey(const Key('emergency-console-pve-149')),
        findsOneWidget,
      );
      await t.tap(find.byKey(const Key('emergency-console-pve-149')));
      await t.pumpAndSettle();
      expect(opened, 0);
      expect(find.text('Console로 열까요?'), findsOneWidget);
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(opened, 0);
      await t.tap(find.byKey(const Key('stream-modes-pve-149')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('emergency-console-pve-149')));
      await t.pumpAndSettle();
      await t.tap(find.text('Console로 열기'));
      await t.pumpAndSettle();
      expect(opened, 1);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'The manage menu lists no sound-card row (the audio device is added in Proxmox)',
    (t) async {
      final controller = MenuController();
      await showSurface(
        t,
        DesktopMenu(
          machine: machine,
          actions: DesktopActions(details: () {}),
          mode: SessionDisplayMode.windowed,
          kind: DesktopMenuKind.manage,
          controller: controller,
          child: TextButton(
            onPressed: controller.open,
            child: const Text('메뉴'),
          ),
        ),
      );
      await t.tap(find.text('메뉴'));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('details-pve-149')), findsOneWidget);
      expect(find.byKey(const Key('vm-sound-card-pve-149')), findsNothing);
      expect(find.text('가상 사운드카드…'), findsNothing);
      expect(find.textContaining('사운드카드'), findsNothing);
      expect(find.byKey(const Key('emergency-console-pve-149')), findsNothing);
    },
  );

  test('The desktop DTO keeps the vm-audio-v1 capability and has no add permission to carry', () {
    final json = <String, Object?>{
      'id': 'pve-149',
      'name': 'VM',
      'kind': 'vm',
      'description': '',
      'os': '',
      'status': 'host_offline',
      'canConnect': false,
      'vmAudioProtocol': vmAudioProtocol,
      // Still sent by a broker that offered adding; the client ignores it.
      'soundCardAddAllowed': true,
    };
    final mapped = machineFromBroker(BrokerDesktop.fromJson(json));
    expect(mapped.vmAudio, true);
    expect(mapped.withResources(mapped.resources).vmAudio, true);
    expect(
      BrokerDesktop.fromJson({...json, 'vmAudioProtocol': 'unknown'}).vmAudio,
      false,
    );
    expect(BrokerDesktop.fromJson({...json, 'kind': 'pc'}).vmAudio, false);
  });

  test('The list item audio state is read leniently: anything but a known state is unknown', () {
    final json = <String, Object?>{
      'id': 'pve-149',
      'name': 'VM',
      'kind': 'vm',
      'description': '',
      'os': '',
      'status': 'online',
      'canConnect': true,
    };
    String? read(Object? audio) =>
        machineFromBroker(BrokerDesktop.fromJson({...json, 'audio': audio}))
            .audioState;
    for (final state in ['absent', 'present', 'pending']) {
      expect(read({'state': state, 'canAdd': state == 'absent'}), state);
    }
    expect(
      read({'state': 'pending', 'canAdd': false, 'reason': null}),
      'pending',
    );
    for (final invalid in <Object?>[
      null,
      'absent',
      {'state': 'gone'},
      {'state': 3},
      {'canAdd': true},
      ['absent'],
    ]) {
      expect(read(invalid), isNull, reason: '$invalid');
    }
    expect(
      machineFromBroker(
        BrokerDesktop.fromJson({
          ...json,
          'kind': 'pc',
          'audio': {'state': 'absent'},
        }),
      ).audioState,
      isNull,
    );
    // Carried through the inventory updates.
    final machine = machineFromBroker(
      BrokerDesktop.fromJson({
        ...json,
        'audio': {'state': 'absent'},
      }),
    );
    expect(machine.withResources(machine.resources).audioState, 'absent');
  });

  group('The card badge (an indicator only, 2026-11-10)', () {
    DesktopMachine card(String id, String? audio) => DesktopMachine(
      id: id,
      name: id,
      description: '',
      kind: MachineKind.vm,
      status: MachineStatus.ready,
      specification: 'Windows',
      accent: Colors.blue,
      powerState: 'running',
      powerStale: false,
      hostOnline: true,
      vmAudio: true,
      audioState: audio,
    );

    testWidgets(
      '소리 없음 and 다시 시작 필요 on the picture, none when present or unknown; the cards keep one height; plain statements, and a tap opens nothing',
      (t) async {
        final semantics = t.ensureSemantics();
        await showSurface(
          t,
          WorkspaceShell(
            onSignOut: () {},
            onRefresh: () {},
            machines: [
              card('pve-101', 'absent'),
              card('pve-102', 'pending'),
              card('pve-103', 'present'),
              card('pve-104', null),
            ],
            displayName: 'User',
            serverOrigin: 'https://fixture.invalid',
          ),
          size: const Size(1600, 1100),
        );
        expect(find.byKey(const Key('sound-badge-pve-103')), findsNothing);
        expect(find.byKey(const Key('sound-badge-pve-104')), findsNothing);
        for (final (id, label, statement) in [
          (
            'pve-101',
            '소리 없음',
            '이 데스크톱에는 소리 장치가 없어요. 관리자가 Proxmox에서 오디오 장치를 추가해야 해요.',
          ),
          ('pve-102', '다시 시작 필요', '소리 장치를 쓰려면 데스크톱을 다시 시작해야 해요.'),
        ]) {
          final badge = find.byKey(Key('sound-badge-$id'));
          expect(
            find.descendant(of: badge, matching: find.text(label)),
            findsOneWidget,
          );
          final tooltip = t.widget<Tooltip>(
            find.descendant(of: badge, matching: find.byType(Tooltip)),
          );
          expect(tooltip.message, statement, reason: id);
          expect(tooltip.message, isNot(contains('눌러')), reason: id);
          expect(tooltip.message, isNot(contains('사운드카드')), reason: id);
          // Said to a screen reader as text with that statement, not as a
          // button: nothing to press or to focus.
          final node = t.getSemantics(
            find.descendant(of: badge, matching: find.bySemanticsLabel(label)),
          );
          expect(node.label, label, reason: id);
          expect(node.tooltip, statement, reason: id);
          final data = node.getSemanticsData();
          expect(data.flagsCollection.isButton, isFalse, reason: id);
          expect(data.hasAction(SemanticsAction.tap), isFalse, reason: id);
          expect(data.hasAction(SemanticsAction.focus), isFalse, reason: id);
          expect(
            find.descendant(of: badge, matching: find.byType(InkWell)),
            findsNothing,
            reason: id,
          );
        }
        expect(audioDeviceMissingText, isNot(contains('눌러')));
        expect(audioDevicePendingText, isNot(contains('사운드카드')));
        final heights = {
          for (final id in ['pve-101', 'pve-102', 'pve-103', 'pve-104'])
            t
                .getSize(
                  find
                      .ancestor(
                        of: find.byKey(Key('desktop-more-$id')),
                        matching: find.byType(DesktopTile),
                      )
                      .first,
                )
                .height,
        };
        expect(heights, hasLength(1), reason: 'equal card heights');
        for (final id in ['pve-101', 'pve-102']) {
          await t.tap(find.byKey(Key('sound-badge-$id')), warnIfMissed: false);
          await t.pumpAndSettle();
          expect(find.byType(Dialog), findsNothing, reason: id);
          expect(find.textContaining('사운드카드'), findsNothing, reason: id);
        }
        await expectNoSoundCardRow(t, id: 'pve-101');
        expect(t.takeException(), isNull);
        semantics.dispose();
      },
    );
  });
}

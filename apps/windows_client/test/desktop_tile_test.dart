// Launcher tiles, their connection menu, power-on-then-connect and the
// readiness checklist. Widget/model checks only: no broker, Host or real
// session is contacted.
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/host_update.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/models/virtual_display.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';
import 'package:multitap_client/ui/readiness_dialog.dart';
import 'package:multitap_client/ui/workspace_shell.dart';
import 'helpers/stream_connect_consent.dart';
import 'helpers/pump_work.dart';

DesktopMachine vm(
  String id, {
  String state = 'running',
  bool? hostOnline = true,
  StreamPreparation? preparation,
  bool? virtualDisplay = true,
  bool stale = false,
  bool busy = false,
  String name = 'TESTER',
  String? vmState,
  String? vmStateReason,
  bool monitorControl = true,
  VirtualDisplayState? display,
  List<RemoteMonitor>? monitors,
  bool consoleInUse = false,
  bool qemuAgentOff = false,
}) => DesktopMachine(
  id: id,
  consoleInUse: consoleInUse,
  qemuAgentOff: qemuAgentOff,
  name: name,
  description: '',
  kind: MachineKind.vm,
  status: state == 'stopped' && !stale
      ? MachineStatus.poweredOff
      : hostOnline == true &&
            preparation == null &&
            virtualDisplay == true &&
            state == 'running'
      ? MachineStatus.ready
      : MachineStatus.awaitingHost,
  specification: 'Windows 11 Pro',
  accent: Colors.blue,
  vmState: vmState,
  vmStateReason: vmStateReason,
  hostOnline: state == 'stopped' ? null : hostOnline,
  interactiveSession: true,
  virtualDisplayReady: virtualDisplay,
  streamPreparation: preparation,
  powerState: state,
  powerStale: stale,
  powerBusy: busy,
  virtualMonitorControl: monitorControl,
  display:
      display ??
      VirtualDisplayState(
        supported: true,
        activeCount: 2,
        checkedAt: DateTime.utc(2026, 10, 31),
        stale: false,
      ),
  monitors:
      monitors ??
      const [
        RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
        RemoteMonitor(id: '1', width: 1920, height: 1080),
      ],
);

class _Opened {
  _Opened(this.id, this.mode);
  final String id;
  final SessionDisplayMode mode;
  @override
  String toString() => '$id:${mode.name}';
}

class _Fixture {
  final opened = <_Opened>[];
  final power = <String>[];
  var refreshes = 0;
  late List<DesktopMachine> machines;
  final memory = SessionLauncherMemory();

  Widget shell({
    Size size = const Size(1440, 900),
    double scale = 1,
    Brightness brightness = Brightness.dark,
  }) => RepaintBoundary(
    key: const Key('count-render-boundary'),
    child: MaterialApp(
      theme: buildAppTheme(brightness: brightness),
      home: MediaQuery(
        data: MediaQueryData(size: size, textScaler: TextScaler.linear(scale)),
        child: WorkspaceShell(
          onSignOut: () {},
          onRefresh: () => refreshes++,
          displayName: '백종헌',
          serverOrigin: 'https://vdi.example.com',
          localMonitors: 1,
          machines: machines,
          launcherMemory: memory,
          onConnect: (_, {options = const StreamOptions()}) async =>
              throw StateError('detached windows only'),
          onPower: (id, action) async => power.add('$id:$action'),
          onConfigureDisplays: (_, _) async => throw StateError('fixture only'),
          onOpenWindow: (machine, mode, options) async {
            opened.add(_Opened(machine.id, mode));
            return const SessionWindowOpenResult(opened: true);
          },
        ),
      ),
    ),
  );
}

Future<void> _saveCountRender(
  WidgetTester tester,
  String name,
  Size viewport,
  double scale,
  Brightness brightness,
) async {
  const directory = String.fromEnvironment('MULTITAP_COUNT_RENDER_DIR');
  if (directory.isEmpty) return;
  final boundary = tester.renderObject<RenderRepaintBoundary>(
    find.byKey(const Key('count-render-boundary')),
  );
  await tester.runAsync(() async {
    final image = await boundary.toImage(pixelRatio: 1);
    try {
      final png = await image.toByteData(format: ui.ImageByteFormat.png);
      if (png == null) throw StateError('Fixture PNG unavailable');
      await Directory(directory).create(recursive: true);
      await File('$directory/$name.png').writeAsBytes(
        png.buffer.asUint8List(png.offsetInBytes, png.lengthInBytes),
        flush: true,
      );
      await File('$directory/$name.json').writeAsString(
        jsonEncode({
          'fixtureOnly': true,
          'desktopCaptured': false,
          'viewport': {'width': viewport.width, 'height': viewport.height},
          'textScale': scale,
          'brightness': brightness.name,
          'pixelWidth': image.width,
          'pixelHeight': image.height,
          'sourceFingerprint': const String.fromEnvironment(
            'MULTITAP_COUNT_SOURCE',
          ),
        }),
        flush: true,
      );
    } finally {
      image.dispose();
    }
  });
}

Future<_Fixture> _show(
  WidgetTester t,
  List<DesktopMachine> machines, {
  Size size = const Size(1440, 900),
  double scale = 1,
  Brightness brightness = Brightness.dark,
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = size;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  final fixture = _Fixture()..machines = machines;
  await t.pumpWidget(
    fixture.shell(size: size, scale: scale, brightness: brightness),
  );
  await t.pump();
  await t.pump(const Duration(milliseconds: 300));
  return fixture;
}

void main() {
  for (final layout in [
    (size: const Size(1920, 1080), scale: 1.0, brightness: Brightness.dark),
    (size: const Size(960, 640), scale: 1.5, brightness: Brightness.light),
  ]) {
    testWidgets(
      'owned count is read-only and hides pending/stale values ${layout.brightness}',
      (t) async {
        VirtualDisplayState known(int count, {bool stale = false}) =>
            VirtualDisplayState(
              supported: false,
              activeCount: count,
              checkedAt: DateTime.utc(2026, 10, 31),
              stale: stale,
            );
        final initial = vm('pve-149', monitorControl: false, display: known(2));
        final fixture = await _show(
          t,
          [initial],
          size: layout.size,
          scale: layout.scale,
          brightness: layout.brightness,
        );
        final badge = find.byKey(const Key('owned-display-count-pve-149'));
        expect(badge, findsOneWidget);
        expect(
          find.descendant(of: badge, matching: find.text('2')),
          findsOneWidget,
        );
        expect(initial.canConfigureDisplays, isFalse);
        await _saveCountRender(
          t,
          layout.brightness == Brightness.dark
              ? 'wide-dark'
              : 'compact-light150',
          layout.size,
          layout.scale,
          layout.brightness,
        );
        for (final state in [
          known(2, stale: true),
          initial.display.pending(),
        ]) {
          fixture.machines = [initial.withDisplay(state)];
          await t.pumpWidget(
            fixture.shell(
              size: layout.size,
              scale: layout.scale,
              brightness: layout.brightness,
            ),
          );
          await pumpWork(t);
          expect(badge, findsNothing);
          expect(t.takeException(), isNull);
        }
        fixture.machines = [
          vm(
            'pve-149',
            monitorControl: false,
            display: known(1),
            monitors: const [
              RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
            ],
          ),
        ];
        await t.pumpWidget(
          fixture.shell(
            size: layout.size,
            scale: layout.scale,
            brightness: layout.brightness,
          ),
        );
        await t.pumpAndSettle();
        expect(badge, findsOneWidget);
        expect(
          find.descendant(of: badge, matching: find.text('1')),
          findsOneWidget,
        );
        expect(t.takeException(), isNull);
      },
    );
  }
  group('readiness', () {
    test('ready desktops confirm all four rows from server fields only', () {
      final r = DesktopReadiness.of(vm('a'), server: 'vdi.example.com');
      expect(r.phase, TilePhase.ready);
      expect(r.headline, '연결 준비됨');
      expect(r.checks.map((c) => c.label), [
        '전원',
        'Host 서비스',
        '가상 화면',
        '연결 경로',
      ]);
      expect(r.checks.every((c) => c.state == CheckState.ok), isTrue);
      expect(r.checks[2].detail, contains('2개 준비됨'));
      expect(r.checks[3].detail, contains('vdi.example.com'));
    });

    test('an off VM never shows unconfirmed rows as confirmed', () {
      final r = DesktopReadiness.of(vm('a', state: 'stopped'));
      expect(r.phase, TilePhase.off);
      expect(r.checks[0].state, CheckState.warn);
      expect(r.checks[0].action, ReadinessAction.start);
      for (final check in r.checks.skip(1)) {
        expect(check.state, CheckState.unknown, reason: check.label);
      }
    });

    test('a failing host row offers restart and later rows stay unknown', () {
      final r = DesktopReadiness.of(vm('a', hostOnline: false));
      expect(r.phase, TilePhase.attention);
      expect(r.headline, 'Host가 응답하지 않아요');
      expect(r.checks[1].state, CheckState.warn);
      expect(r.checks[1].action, ReadinessAction.reboot);
      expect(r.checks[2].state, CheckState.unknown);
      expect(r.checks[3].state, CheckState.unknown);
      // A reboot is not the generic fix: the main button refreshes.
      expect(r.primary, ReadinessAction.refresh);
    });

    // The verdict says "Host 업데이트 중" / "Windows 시작 중": the Host row
    // must not warn "응답하지 않아요" and offer 다시 시작 meanwhile.
    test('while Windows starts or the Host updates, the Host row waits without 다시 시작', () {
      for (final (state, detail) in [
        ('windows_starting', 'Windows를 시작하고 있어요'),
        ('host_wait', '연결을 준비하고 있어요'),
      ]) {
        final r = DesktopReadiness.of(
          vm('a', hostOnline: false, vmState: state),
        );
        expect(r.phase, TilePhase.waiting, reason: state);
        expect(r.checks[1].state, CheckState.pending, reason: state);
        expect(r.checks[1].detail, detail, reason: state);
        expect(r.checks[1].action, isNull, reason: state);
      }
      final now = DateTime.utc(2026, 10, 25, 0, 1);
      final updating = DesktopReadiness.of(
        vm('a', hostOnline: false).withHostUpdate(
          HostUpdateState.fromJson({
            'protocol': 'host-update-v1',
            'connected': false,
            'supported': true,
            'version': '0.2.58',
            'latestVersion': '0.2.59',
            'newer': true,
            'available': false,
            'operation': {
              'status': 'installing',
              'targetVersion': '0.2.59',
              'checkedAt': '2026-10-25T00:00:00.000Z',
            },
          }),
        ),
        now: now,
      );
      expect(updating.hostUpdating, isTrue);
      expect(updating.headline, 'Host 업데이트 중');
      expect(updating.checks[1].state, CheckState.pending);
      expect(updating.checks[1].detail, 'Host 업데이트 중');
      expect(updating.checks[1].action, isNull);
      // A Host that dropped from a running desktop is still a warning.
      final lost = DesktopReadiness.of(
        vm('a', hostOnline: false, vmState: 'host_lost'),
      );
      expect(lost.checks[1].state, CheckState.warn);
      expect(lost.checks[1].action, ReadinessAction.reboot);
    });

    test('a console holding the VM offers Console 끊고 연결, not a display wait', () {
      // Not '가상 모니터를 구성하고 있어요' with no way in.
      final r = DesktopReadiness.of(
        vm(
          'a',
          consoleInUse: true,
          display: VirtualDisplayState(
            supported: true,
            activeCount: 1,
            checkedAt: DateTime.utc(2026, 11, 9),
            stale: true,
            busy: true,
          ),
        ),
      );
      expect(r.phase, TilePhase.attention);
      expect(r.headline, 'Console에서 사용 중이에요');
      expect(r.primary, ReadinessAction.releaseConsole);
    });
    test('the main button is a reboot only for a restart-required state', () {
      for (final prep in StreamPreparation.values) {
        final r = DesktopReadiness.of(vm('a', preparation: prep));
        if (r.phase != TilePhase.attention) continue;
        expect(
          r.primary,
          prep == StreamPreparation.restart
              ? ReadinessAction.reboot
              // Signed out or locked: the Proxmox console.
              : prep == StreamPreparation.login ||
                    prep == StreamPreparation.locked
              ? ReadinessAction.console
              : ReadinessAction.refresh,
          reason: '$prep',
        );
      }
    });

    test('a display preparation code becomes the failing row', () {
      final r = DesktopReadiness.of(
        vm('a', preparation: StreamPreparation.display),
      );
      expect(r.phase, TilePhase.attention);
      expect(r.checks[0].state, CheckState.ok);
      expect(r.checks[1].state, CheckState.ok);
      expect(r.checks[2].state, CheckState.warn);
      expect(r.checks[2].action, isNull);
      expect(r.checks[3].state, CheckState.unknown);
      final restart = DesktopReadiness.of(
        vm('a', preparation: StreamPreparation.restart),
      );
      expect(restart.checks[2].action, ReadinessAction.reboot);
    });

    test('unknown power and host stay unconfirmed instead of failing', () {
      final r = DesktopReadiness.of(
        vm('a', state: 'unknown', stale: true, hostOnline: null),
      );
      expect(r.phase, TilePhase.waiting);
      expect(r.checks.every((c) => c.state == CheckState.unknown), isTrue);
    });
  });

  testWidgets(
    'Only the main button and ▶ connect a ready tile, no key does; its main action is always emphasised',
    (t) async {
      final f = await _show(t, [vm('pve-149'), vm('pve-150', name: '개발용 VM')]);
      FilledButton button(String id) =>
          t.widget<FilledButton>(find.byKey(Key('open-$id')));
      final amber = AppPalette(true).amber;
      expect(button('pve-149').style!.backgroundColor!.resolve({}), amber);
      expect(
        button('pve-150').style!.backgroundColor!.resolve({}),
        amber,
        reason: 'A grey resting main action read as disabled.',
      );
      // Enter (or Space) on the focused tile connects nothing and asks
      // nothing.
      for (final key in [
        LogicalKeyboardKey.enter,
        LogicalKeyboardKey.numpadEnter,
        LogicalKeyboardKey.space,
      ]) {
        await t.sendKeyEvent(key);
        await t.pumpAndSettle();
        expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      }
      expect(f.opened, isEmpty);
      await t.tap(find.byKey(const Key('open-pve-149')));
      await acceptStreamConnect(t, 'pve-149');
      await t.pumpAndSettle();
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(f.opened.map((o) => '$o'), ['pve-149:windowed']);
      // A click elsewhere on a tile (picture, name, status) does nothing.
      for (final spot in [
        find.byKey(const Key('tile-pve-150')),
        find.byKey(const Key('desktop-name-pve-150')),
        find.byKey(const Key('status-pve-150')),
      ]) {
        await t.tap(spot, warnIfMissed: false);
        await t.pumpAndSettle();
        expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      }
      expect(f.opened.length, 1);
      // ▶ on the hovered picture is the same explicit ordinary action.
      final mouse = await t.createGesture(kind: PointerDeviceKind.mouse);
      await mouse.addPointer(
        location: t.getCenter(find.byKey(const Key('card-pve-150'))),
      );
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('play-pve-150')));
      await acceptStreamConnect(t, 'pve-150');
      await t.pumpAndSettle();
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(f.opened.length, 2);
      expect(f.opened.last.toString(), 'pve-150:windowed');
      await mouse.removePointer();
      expect(find.text('연결 가능'), findsNothing);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    '▾ chooses how to connect, ⏻ holds power, ⋯ manages the VM, a right click shows connect and manage',
    (t) async {
      final f = await _show(t, [vm('pve-149')]);
      MenuItemButton item(String key) =>
          t.widget<MenuItemButton>(find.byKey(Key(key)));
      // ▾: streaming modes and quality; no VM management.
      await t.tap(find.byKey(const Key('stream-modes-pve-149')));
      await t.pumpAndSettle();
      for (final key in [
        'stream-mode-windowed',
        'stream-mode-fullscreen',
        'stream-mode-allMonitors',
        'stream-settings-pve-149',
      ]) {
        expect(find.byKey(Key(key)), findsOneWidget, reason: key);
      }
      for (final key in ['power-reboot-pve-149', 'details-pve-149']) {
        expect(find.byKey(Key(key)), findsNothing, reason: '$key is elsewhere');
      }
      expect(item('stream-mode-allMonitors').onPressed, isNull);
      // A mode choice applies only to that attempt; the next default is always
      // windowed and is named in words rather than ticked like a setting.
      expect(
        find.byKey(const Key('stream-mode-windowed-note')),
        findsOneWidget,
      );
      expect(find.text('기본'), findsOneWidget);
      expect(find.text('스트리밍 설정…'), findsOneWidget);
      await t.sendKeyEvent(LogicalKeyboardKey.escape);
      await t.pumpAndSettle();
      // ⏻: 종료, 다시 시작 and 정지, outside ⋯.
      await t.tap(find.byKey(const Key('desktop-power-pve-149')));
      await t.pumpAndSettle();
      for (final key in [
        'power-shutdown-pve-149',
        'power-reboot-pve-149',
        'power-stop-pve-149',
      ]) {
        expect(find.byKey(Key(key)), findsOneWidget, reason: key);
      }
      for (final key in ['details-pve-149', 'stream-mode-windowed']) {
        expect(find.byKey(Key(key)), findsNothing, reason: key);
      }
      await t.sendKeyEvent(LogicalKeyboardKey.escape);
      await t.pumpAndSettle();
      // ⋯: the checklist; no power. No virtual-monitor count either:
      // connecting prepares it, and the session toolbar keeps its own
      // control.
      await t.tap(find.byKey(const Key('desktop-more-pve-149')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('details-pve-149')), findsOneWidget);
      expect(item('details-pve-149').onPressed, isNotNull);
      expect(find.byKey(const Key('display-menu-pve-149')), findsNothing);
      expect(find.byKey(const Key('display-count-pve-149-1')), findsNothing);
      expect(find.byKey(const Key('power-reboot-pve-149')), findsNothing);
      expect(find.byKey(const Key('stream-mode-windowed')), findsNothing);
      expect(find.byKey(const Key('power-start-pve-149')), findsNothing);
      expect(find.text('강제 중지'), findsNothing);
      await t.sendKeyEvent(LogicalKeyboardKey.escape);
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('stream-modes-pve-149')));
      await t.pumpAndSettle();
      // Choosing a normal connection mode asks before opening and does not change
      // the launcher's saved default for its ordinary main action.
      await t.tap(find.byKey(const Key('stream-mode-fullscreen')));
      await acceptStreamConnect(t, 'pve-149');
      await t.pumpAndSettle();
      expect(f.opened.single.toString(), 'pve-149:fullscreen');
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(
        (await f.memory.load('https://vdi.example.com')).modeFor('pve-149'),
        SessionDisplayMode.windowed,
      );
      expect(f.opened.single.toString(), 'pve-149:fullscreen');
      expect(find.text('스트리밍 연결'), findsOneWidget);
      expect(
        (await f.memory.load('https://vdi.example.com')).modeFor('pve-149'),
        SessionDisplayMode.windowed,
      );
      await t.tap(find.byKey(const Key('open-pve-149')));
      await acceptStreamConnect(t, 'pve-149');
      await t.pumpAndSettle();
      expect(find.text('스트리밍 · 전체 화면'), findsNothing);
      expect(f.opened.last.mode, SessionDisplayMode.windowed);
      // A right click shows both parts.
      await t.tapAt(
        t.getCenter(find.byKey(const Key('card-pve-149'))),
        buttons: kSecondaryButton,
      );
      await t.pumpAndSettle();
      expect(find.byKey(const Key('stream-mode-windowed')), findsOneWidget);
      expect(find.byKey(const Key('details-pve-149')), findsOneWidget);
      expect(find.byKey(const Key('display-menu-pve-149')), findsNothing);
      // ...and the power actions too (they are not in ⋯).
      expect(find.byKey(const Key('power-reboot-pve-149')), findsOneWidget);
      await t.tap(find.byKey(const Key('stream-mode-fullscreen')));
      await acceptStreamConnect(t, 'pve-149');
      await t.pumpAndSettle();
      expect(f.opened, hasLength(3));
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(f.opened.last.toString(), 'pve-149:fullscreen');
    },
  );

  testWidgets(
    'A stopped VM offers 켜기 only (no connect), asks once from the main button and from the power menu, and never connects by itself',
    (t) async {
      final f = await _show(t, [vm('pve-151', state: 'stopped')]);
      expect(find.text('켜기'), findsOneWidget);
      expect(find.text('켜고 연결'), findsNothing);
      expect(find.byKey(const Key('stream-modes-pve-151')), findsNothing);
      expect(find.byKey(const Key('open-pve-151')), findsNothing);
      // The main button asks "VM을 켤까요?" with the VM name; 취소 does nothing.
      await t.tap(find.byKey(const Key('start-pve-151')));
      await t.pumpAndSettle();
      expect(find.text('데스크톱을 켤까요?'), findsOneWidget);
      expect(f.power, isEmpty, reason: 'The main action asks once first.');
      await t.tap(find.byKey(const Key('cancel-power-start-pve-151')));
      await t.pumpAndSettle();
      expect(f.power, isEmpty);
      await t.tap(find.byKey(const Key('start-pve-151')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('confirm-power-start-pve-151')));
      await t.pumpAndSettle();
      expect(f.power, ['pve-151:start']);
      expect(f.opened, isEmpty, reason: 'Powering on never opens a session.');
      expect(find.byKey(const Key('start-progress-pve-151')), findsNothing);
      // The ⏻ menu asks the same single question.
      await t.tap(find.byKey(const Key('desktop-power-pve-151')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('power-start-pve-151')));
      await t.pumpAndSettle();
      expect(find.text('데스크톱을 켤까요?'), findsOneWidget);
      await t.tap(find.byKey(const Key('confirm-power-start-pve-151')));
      await t.pumpAndSettle();
      expect(f.power, ['pve-151:start', 'pve-151:start']);
      // Once it runs the main button is the normal stream connection.
      f.machines = [vm('pve-151')];
      await t.pumpWidget(f.shell());
      await t.pump();
      expect(find.byKey(const Key('start-pve-151')), findsNothing);
      expect(find.byKey(const Key('open-pve-151')), findsOneWidget);
      expect(find.text('스트리밍 연결'), findsOneWidget);
      expect(f.opened, isEmpty);
    },
  );

  testWidgets(
    'The power sign on a stopped tile lights up under the pointer and asks the same single question',
    (t) async {
      final f = await _show(t, [vm('pve-151', state: 'stopped')]);
      final sign = find.byKey(const Key('power-on-pve-151'));
      expect(sign, findsOneWidget);
      BoxDecoration circle() =>
          t
                  .widget<AnimatedContainer>(
                    find.descendant(
                      of: sign,
                      matching: find.byType(AnimatedContainer),
                    ),
                  )
                  .decoration!
              as BoxDecoration;
      final amber = AppPalette(true).amber;
      expect(circle().color!.a, 0, reason: 'At rest it is an outline only.');
      final mouse = await t.createGesture(kind: PointerDeviceKind.mouse);
      await mouse.addPointer(location: t.getCenter(sign));
      await t.pumpAndSettle();
      expect(circle().color, amber, reason: 'It answers the pointer.');
      // The same question as the main button; nothing starts before the answer.
      await t.tap(sign);
      await t.pumpAndSettle();
      expect(find.text('데스크톱을 켤까요?'), findsOneWidget);
      expect(f.power, isEmpty);
      await t.tap(find.byKey(const Key('confirm-power-start-pve-151')));
      await t.pumpAndSettle();
      expect(f.power, ['pve-151:start']);
      expect(f.opened, isEmpty, reason: 'Powering on never opens a session.');
      await mouse.removePointer();
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('The connect tip names the action and the mode, not a key', (
    t,
  ) async {
    await _show(t, [vm('pve-149')]);
    final tip = t.widget<Tooltip>(
      find.ancestor(
        of: find.byKey(const Key('open-pve-149')),
        matching: find.byType(Tooltip),
      ).first,
    );
    expect(tip.message, '스트리밍 연결 · 창 모드');
  });

  testWidgets(
    'The broker state machine renders its states: Windows 시작 중, 연결 준비 중 and Host 연결 끊김 without Host',
    (t) async {
      for (final (state, headline) in [
        ('windows_starting', 'Windows를 시작하고 있어요'),
        ('host_wait', '연결을 준비하고 있어요'),
        ('host_lost', 'Host 연결이 끊겼어요'),
      ]) {
        final f = await _show(t, [
          vm('pve-153', hostOnline: false, vmState: state),
        ]);
        expect(find.text(headline), findsWidgets, reason: state);
        expect(
          find.byKey(const Key('start-pve-153')),
          findsNothing,
          reason: state,
        );
        f.machines = [vm('pve-153')];
        await t.pumpWidget(f.shell());
        await t.pump();
      }
    },
  );
  testWidgets(
    'Boot steps after the Host connected are progress, not warnings: a spinner label on the disabled main button, no warning texts',
    (t) async {
      // windows_login offers Console at once.
      for (final (reason, prep, label) in [
        ('display_preparing', StreamPreparation.display, '화면 준비 중'),
        ('session_preparing', StreamPreparation.session, '연결 준비 중'),
      ]) {
        final machine = vm(
          'pve-153',
          preparation: prep,
          vmState: 'preparing',
          vmStateReason: reason,
        );
        final f = await _show(t, [machine]);
        expect(find.text(label), findsWidgets, reason: reason);
        expect(find.text(prep.badge), findsNothing, reason: reason);
        expect(find.text('Windows에 로그인돼 있지 않아요'), findsNothing);
        expect(find.text('가상 화면이 준비되지 않았어요'), findsNothing);
        expect(find.text('Windows 화면을 확인하지 못했어요'), findsNothing);
        final button = t.widget<FilledButton>(
          find.byKey(const Key('open-pve-153')),
        );
        expect(button.onPressed, isNull, reason: 'No connect while preparing');
        await t.tap(find.byKey(const Key('open-pve-153')), warnIfMissed: false);
        await t.pump();
        expect(f.opened, isEmpty);
        final r = DesktopReadiness.of(machine);
        expect(r.phase, TilePhase.waiting);
        expect(r.progressLabel, label);
        expect(r.canStream, isFalse);
        expect(r.detail, isNot(contains('자동으로 로그인')));
        expect(r.checks[2].state, CheckState.pending);
        expect(r.checks[2].action, isNull);
        await t.tap(find.byKey(const Key('stream-state-pve-153')));
        await t.pump(const Duration(milliseconds: 300));
        expect(find.byType(ReadinessDialog), findsOneWidget);
        expect(
          find.byKey(const Key('readiness-verdict-progress')),
          findsOneWidget,
        );
        expect(find.byKey(const Key('readiness-progress-2')), findsOneWidget);
        expect(
          t.widget<Text>(find.byKey(const Key('readiness-detail-2'))).data,
          r.detail,
          reason: 'A known boot step is progress, not an unconfirmed failure.',
        );
        expect(find.byKey(const Key('readiness-action-reboot')), findsNothing);
        if (reason == 'windows_login') {
          // The same open checklist follows the broker once its grace window
          // expires: no login is an actionable state, never invented readiness.
          f.machines = [
            vm(
              'pve-153',
              preparation: StreamPreparation.login,
              vmState: 'host_wait',
              vmStateReason: 'host_preparing',
            ),
          ];
          await t.pumpWidget(f.shell());
          await t.pump(const Duration(milliseconds: 300));
          expect(find.text('Windows에 로그인돼 있지 않아요'), findsWidgets);
          expect(
            find.byKey(const Key('readiness-verdict-progress')),
            findsNothing,
          );
          expect(find.byKey(const Key('readiness-progress-2')), findsNothing);
          expect(f.opened, isEmpty);
        }
        await t.tap(find.byKey(const Key('close-readiness')));
        await t.pump(const Duration(milliseconds: 300));
        await t.pumpWidget(const SizedBox.shrink());
      }
    },
  );

  test('Compatibility: other vmState values keep today\'s warning texts, and a released client ignores preparing', () {
    // After the grace window / for a definite failure the broker sends
    // host_wait (or anything this build does not know): attention texts.
    for (final state in [
      'host_wait',
      'host_preparing',
      'brand_new_state',
      null,
    ]) {
      final r = DesktopReadiness.of(
        vm(
          'pve-1',
          preparation: StreamPreparation.login,
          vmState: state,
          vmStateReason: 'host_preparing',
        ),
      );
      expect(r.phase, TilePhase.attention, reason: '$state');
      expect(r.headline, 'Windows에 로그인돼 있지 않아요', reason: '$state');
      expect(r.primary, ReadinessAction.console, reason: '$state');
      expect(r.progressLabel, isNull, reason: '$state');
    }
    // The broker only sends preparing while the Host is connected; a
    // stale preparing without a Host never becomes progress here.
    final noHost = DesktopReadiness.of(
      vm(
        'pve-1',
        hostOnline: false,
        vmState: 'preparing',
        vmStateReason: 'windows_login',
      ),
    );
    expect(noHost.progressLabel, isNull);
    // Ready always wins over a late preparing.
    final ready = DesktopReadiness.of(
      vm('pve-1', vmState: 'preparing', vmStateReason: 'windows_login'),
    );
    expect(ready.phase, TilePhase.ready);
  });
  testWidgets(
    'An attention tile has one main button: 새로고침 here, 다시 시작 only for a restart; details show the four-row checklist',
    (t) async {
      final f = await _show(t, [
        vm('pve-152', preparation: StreamPreparation.display),
      ]);
      expect(find.text('가상 화면이 준비되지 않았어요'), findsWidgets);
      expect(find.byKey(const Key('open-pve-152')), findsNothing);
      expect(find.byKey(const Key('stream-modes-pve-152')), findsNothing);
      expect(find.text('다시 시작'), findsNothing);
      expect(
        find.descendant(
          of: find.byKey(const Key('attention-pve-152')),
          matching: find.text('새로고침'),
        ),
        findsOneWidget,
      );
      final refreshes = f.refreshes;
      await t.tap(find.byKey(const Key('attention-pve-152')));
      await t.pumpAndSettle();
      expect(f.refreshes, refreshes + 1);
      expect(f.opened, isEmpty);
      await t.tap(find.byKey(const Key('stream-state-pve-152')));
      await t.pumpAndSettle();
      expect(find.byType(ReadinessDialog), findsOneWidget);
      expect(
        t.widget<Text>(find.byKey(const Key('readiness-detail-3'))).data,
        startsWith('확인 안 됨'),
      );
      expect(f.opened, isEmpty);
    },
  );

  testWidgets(
    'One footer for every state: a single main button whose wording follows the state, ▾ only where the console is reachable, 다시 시작 only for a restart',
    (t) async {
      // state -> (key prefix, label, ▾ shown)
      final cases = <(String, DesktopMachine, String, String, bool)>[
        ('ready', vm('s1'), 'open-', '스트리밍 연결', true),
        ('off', vm('s2', state: 'stopped'), 'start-', '켜기', false),
        ('power', vm('s3', busy: true), 'open-', '처리 중…', false),
        (
          'starting',
          vm('s4', hostOnline: false, vmState: 'windows_starting'),
          'open-',
          '준비 중…',
          false,
        ),
        (
          'preparing',
          vm(
            's5',
            preparation: StreamPreparation.login,
            vmState: 'preparing',
            vmStateReason: 'windows_login',
          ),
          'attention-',
          'Console로 열기',
          false,
        ),
        (
          'login',
          vm('s6', preparation: StreamPreparation.login),
          'attention-',
          'Console로 열기',
          false,
        ),
        (
          'session',
          const DesktopMachine(
            id: 's7',
            name: 'TESTER',
            description: '',
            kind: MachineKind.vm,
            status: MachineStatus.awaitingHost,
            specification: 'Windows 11 Pro',
            accent: Colors.blue,
            hostOnline: true,
            interactiveSession: false,
            virtualDisplayReady: true,
            powerState: 'running',
          ),
          'attention-',
          '새로고침',
          false,
        ),
        (
          'restart required',
          vm('s8', preparation: StreamPreparation.restart),
          'attention-',
          '다시 시작…',
          false,
        ),
        (
          'host lost',
          vm('s10', hostOnline: false, vmState: 'host_lost'),
          'attention-',
          '새로고침',
          false,
        ),
      ];
      Size? size;
      for (final (name, machine, prefix, label, more) in cases) {
        for (final width in [1440.0, 400.0]) {
          await _show(t, [machine], size: Size(width, 1000), scale: 1.75);
          final id = machine.id;
          final mains = [
            for (final p in ['open-', 'start-', 'attention-'])
              if (find.byKey(Key('$p$id')).evaluate().isNotEmpty) p,
          ];
          expect(mains, [prefix], reason: '$name: one main button');
          expect(
            find.descendant(
              of: find.byKey(Key('$prefix$id')),
              matching: find.text(label),
            ),
            findsOneWidget,
            reason: name,
          );
          expect(
            find.byKey(Key('stream-modes-$id')),
            more ? findsOneWidget : findsNothing,
            reason: '$name: ▾',
          );
          expect(
            find.text('다시 시작'),
            findsNothing,
            reason: '$name: 다시 시작 is in ⏻ only',
          );
          final card = t.getSize(find.byKey(Key('card-$id')));
          if (width == 1440 && name == 'ready') size = card;
          if (width == 1440) expect(card, size, reason: '$name: card size');
          expect(t.takeException(), isNull, reason: name);
        }
      }
    },
  );

  testWidgets(
    'A restart-required tile asks before restarting from its main button; an off tile asks before powering on',
    (t) async {
      final f = await _show(t, [
        vm('pve-160', preparation: StreamPreparation.restart),
      ]);
      await t.tap(find.byKey(const Key('attention-pve-160')));
      await t.pumpAndSettle();
      expect(find.text('데스크톱을 다시 시작할까요?'), findsOneWidget);
      expect(f.power, isEmpty);
      await t.tap(find.byKey(const Key('confirm-power-reboot-pve-160')));
      await t.pumpAndSettle();
      expect(f.power, ['pve-160:reboot']);
    },
  );

  testWidgets(
    'Tiles keep one size across states, rows and text scales without overflow',
    (t) async {
      final machines = [
        vm('vm-0'),
        vm('vm-1', state: 'stopped'),
        vm('vm-2', hostOnline: false),
        vm(
          'vm-3',
          state: 'unknown',
          stale: true,
          hostOnline: null,
          name: '이름이 긴 Windows 데스크톱 작업 공간',
        ),
        vm('vm-4', busy: true),
      ];
      for (final width in [1440.0, 1000.0, 800.0, 400.0]) {
        for (final scale in [1.0, 1.75, 2.0]) {
          await _show(t, machines, size: Size(width, 1000), scale: scale);
          t
              .widget<CustomScrollView>(
                find.byKey(const Key('workspace-scroll')),
              )
              .controller!
              .jumpTo(0);
          await t.pump(const Duration(milliseconds: 250));
          final sizes = <Size>[];
          for (final m in machines) {
            final card = find.byKey(Key('card-${m.id}'));
            await t.scrollUntilVisible(
              card,
              400,
              scrollable: find
                  .descendant(
                    of: find.byKey(const Key('workspace-scroll')),
                    matching: find.byType(Scrollable),
                  )
                  .first,
            );
            sizes.add(t.getSize(card));
          }
          for (final size in sizes) {
            expect(size, sizes.first, reason: 'width=$width scale=$scale');
          }
          expect(
            t.takeException(),
            isNull,
            reason: 'width=$width scale=$scale',
          );
        }
      }
    },
  );

  testWidgets(
    'Cards stay the only layout past eight desktops and preview remains optional',
    (t) async {
      final f = await _show(t, [
        for (var i = 0; i < 9; i++) vm('vm-$i', name: 'VM $i'),
      ]);
      final cardHeight = t.getSize(find.byKey(const Key('card-vm-0'))).height;
      expect(cardHeight, greaterThan(300));
      expect(find.byKey(const Key('desktop-view')), findsNothing);
      await t.tap(find.byKey(const Key('desktop-previews')));
      await t.pumpAndSettle();
      final withoutPreview = t
          .getSize(find.byKey(const Key('card-vm-0')))
          .height;
      expect(withoutPreview, lessThan(cardHeight));
      await t.tap(find.byKey(const Key('open-vm-0')));
      await acceptStreamConnect(t, 'vm-0');
      await t.pumpAndSettle();
      expect(f.opened.single.toString(), 'vm-0:windowed');
      await t.tap(find.byKey(const Key('desktop-previews')));
      await t.pumpAndSettle();
      expect(
        t.getSize(find.byKey(const Key('card-vm-0'))).height,
        greaterThan(300),
      );
      expect(t.takeException(), isNull);
    },
  );
  // A VM whose Proxmox QEMU guest agent option is off says so in
  // the picture's bottom-right corner; other VMs carry no such badge.
  testWidgets('QEMU Agent off badge sits bottom-right on that VM only', (t) async {
    await _show(t, [vm('pve-1', qemuAgentOff: true), vm('pve-2', name: 'OTHER')]);
    final badge = find.byKey(const Key('qemu-agent-off-pve-1'));
    expect(badge, findsOneWidget);
    expect(find.descendant(of: badge, matching: find.text('QEMU Agent 꺼짐')), findsOneWidget);
    expect(find.byKey(const Key('qemu-agent-off-pve-2')), findsNothing);
    final box = t.getRect(find.byKey(const Key('tile-pve-1')).first), chip = t.getRect(badge);
    expect(chip.center.dx, greaterThan(box.center.dx));
    expect(t.takeException(), isNull);
  });
}
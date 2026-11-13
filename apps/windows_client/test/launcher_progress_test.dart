// A wait on the desktop list always moves and says what it waits for: the
// tile of a desktop at work (a power task, Windows starting, the Host
// connecting or updating), a Console or remote window this client is opening,
// a Host update being asked for, signing out and the first list read.
// Widget checks only: no broker, Host or real window.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart' show BrokerException;
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/product_updates.dart' show ProductRelease;
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/host_update.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_progress.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_menu.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';
import 'package:multitap_client/ui/desktop_tile.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'helpers/stream_connect_consent.dart';

const _origin = 'https://vdi.example.com';

DesktopMachine _vm(
  String id, {
  String state = 'running',
  bool? hostOnline = true,
  bool busy = false,
  String? action,
  String? vmState,
  String? vmStateReason,
  HostUpdateState hostUpdate = HostUpdateState.absent,
  String? consoleProtocol,
}) => DesktopMachine(
  id: id,
  name: id.toUpperCase(),
  description: '',
  kind: MachineKind.vm,
  status: state == 'stopped'
      ? MachineStatus.poweredOff
      : hostOnline == true && vmState == null && !busy
      ? MachineStatus.ready
      : MachineStatus.awaitingHost,
  specification: 'Windows 11 Pro',
  accent: Colors.blue,
  vmState: vmState,
  vmStateReason: vmStateReason,
  hostOnline: state == 'stopped' ? null : hostOnline,
  interactiveSession: true,
  virtualDisplayReady: true,
  powerState: state,
  powerStale: false,
  powerBusy: busy,
  powerAction: action,
  hostUpdate: hostUpdate,
  consoleBrowserProtocol: consoleProtocol,
);

HostUpdateState _updating(String status, {int? progress}) => HostUpdateState(
  present: true,
  connected: true,
  supported: true,
  version: '1.1.1',
  latestVersion: '1.1.2',
  operation: HostUpdateOperation(
    status: status,
    targetVersion: '1.1.2',
    progress: progress,
    checkedAt: DateTime.now(),
  ),
);

const _offered = HostUpdateState(
  present: true,
  connected: true,
  supported: true,
  version: '1.1.1',
  latestVersion: '1.1.2',
  newer: true,
  available: true,
);

class _Shell {
  _Shell(this.machines);
  List<DesktopMachine> machines;
  bool busy = false, signingOut = false;
  String? error;
  Map<String, DateTime> powerStarted = const {};
  LauncherMemory memory = SessionLauncherMemory();
  Future<SessionWindowOpenResult?> Function(DesktopMachine)? onOpenConsole;
  Future<SessionWindowOpenResult?> Function(
    DesktopMachine,
    SessionDisplayMode,
    StreamOptions,
  )?
  onOpenWindow;
  Future<void> Function(String, String, {required bool disconnectSessions})?
  onHostUpdate;

  Widget build() => MaterialApp(
    theme: buildAppTheme(),
    home: WorkspaceShell(
      onSignOut: () {},
      onRefresh: () {},
      displayName: 'alice',
      serverOrigin: _origin,
      localMonitors: 1,
      machines: machines,
      busy: busy,
      signingOut: signingOut,
      error: error,
      powerStarted: powerStarted,
      launcherMemory: memory,
      onConnect: (_, {options = const StreamOptions()}) async =>
          throw StateError('detached windows only'),
      onPower: (_, _) async {},
      onOpenConsole: onOpenConsole,
      onOpenWindow:
          onOpenWindow ??
          (_, _, _) async => const SessionWindowOpenResult(opened: true),
      onHostUpdate: onHostUpdate,
      loadHostRelease: (current) async => ProductRelease(
        product: 'host',
        version: '1.1.2',
        filename: 'Multitap-Host-1.1.2-windows-x64.msi',
        size: 17000000,
        sha256: 'a' * 64,
        url: Uri.parse('$_origin/downloads/Multitap-Host-1.1.2-windows-x64.msi'),
        changelog: '안정성 개선',
      ),
    ),
  );
}

Future<void> _show(WidgetTester tester, _Shell shell) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = const Size(1440, 900);
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(shell.build());
  // A tile at work never settles (its mark turns): bounded pumps only.
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
}

Future<void> _again(WidgetTester tester, _Shell shell) async {
  await tester.pumpWidget(shell.build());
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
}

Finder _mark(String slot, String id) => find.byKey(Key('$slot-busy-$id'));

/// The spinner of the [AppBusyLine] whose sentence has [textKey].
Finder _lineMark(Key textKey) => find.descendant(
  of: find.ancestor(
    of: find.byKey(textKey),
    matching: find.byType(AppBusyLine),
  ),
  matching: find.byKey(const Key('app-busy-line-spinner')),
);

void main() {
  testWidgets(
    'A desktop that is starting moves on its tile; a ready, off or attention tile stands still',
    (tester) async {
      final shell = _Shell([
        _vm('boot', hostOnline: false, vmState: 'windows_starting'),
        _vm('wait', hostOnline: false, vmState: 'host_wait'),
        _vm('ready'),
        _vm('off', state: 'stopped'),
        _vm('lost', hostOnline: false, vmState: 'host_lost'),
      ]);
      await _show(tester, shell);
      for (final id in ['boot', 'wait']) {
        // The sign on the picture, the sign of the status card and the
        // main button's icon: each a turning mark in the old sign's place.
        expect(_mark('thumb', id), findsOneWidget, reason: id);
        expect(_mark('status', id), findsOneWidget, reason: id);
        expect(_mark('action', id), findsOneWidget, reason: id);
        expect(
          find.descendant(
            of: _mark('status', id),
            matching: find.byType(CircularProgressIndicator),
          ),
          findsOneWidget,
        );
      }
      expect(find.text('Windows를 시작하고 있어요'), findsOneWidget);
      for (final id in ['ready', 'off', 'lost']) {
        expect(_mark('thumb', id), findsNothing, reason: id);
        expect(_mark('status', id), findsNothing, reason: id);
        expect(_mark('action', id), findsNothing, reason: id);
      }
      // No still hourglass stands for work under way any more.
      expect(find.byIcon(Icons.hourglass_top_rounded), findsNothing);
      // The mark sits in the old sign's slot: every tile keeps one size.
      final sizes = {
        for (final id in ['boot', 'wait', 'ready', 'off', 'lost'])
          tester.getSize(find.byKey(Key('card-$id'))),
      };
      expect(sizes, hasLength(1));

      // Windows came up: the marks are gone, the tile is ready.
      shell.machines = [_vm('boot'), ...shell.machines.skip(1)];
      await _again(tester, shell);
      expect(_mark('thumb', 'boot'), findsNothing);
      expect(_mark('status', 'boot'), findsNothing);
      expect(_mark('action', 'boot'), findsNothing);
      expect(find.byKey(const Key('open-boot')), findsOneWidget);

      // It did not: the warning takes the mark's place, never beside it.
      shell.machines = [
        _vm('boot', hostOnline: false, vmState: 'host_lost'),
        ...shell.machines.skip(1),
      ];
      await _again(tester, shell);
      expect(_mark('thumb', 'boot'), findsNothing);
      expect(_mark('status', 'boot'), findsNothing);
      expect(_mark('action', 'boot'), findsNothing);
      expect(find.byKey(const Key('attention-boot')), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );

  for (final previews in [true, false]) {
    testWidgets(
      'A power task moves on the card${previews ? '' : ' with previews off'} and counts the wait from this client\'s own request',
      (tester) async {
        final memory = SessionLauncherMemory();
        await memory.save(
          _origin,
          LauncherPreferences(showPreviews: previews),
        );
        final shell = _Shell([
          _vm('p1', state: 'stopped', busy: true, action: 'start'),
          _vm('p2', busy: true, action: 'reboot'),
        ])..memory = memory;
        await _show(tester, shell);
        await tester.pump(const Duration(milliseconds: 100));
        for (final id in ['p1', 'p2']) {
          expect(_mark('status', id), findsOneWidget, reason: id);
          expect(_mark('action', id), findsOneWidget, reason: id);
          // No time is invented for a task this client did not start.
          expect(find.byKey(Key('power-elapsed-$id')), findsNothing);
        }
        expect(find.text('처리 중…'), findsNWidgets(2));
        expect(find.byIcon(Icons.hourglass_top_rounded), findsNothing);

        // This client's request, 75 seconds ago by its own clock.
        shell.powerStarted = {
          'p1': DateTime.now().subtract(const Duration(seconds: 75)),
          // Too fresh to show: a quick task never flashes a clock.
          'p2': DateTime.now(),
        };
        await _again(tester, shell);
        // In the main button, beside its words: the one slot on every card.
        final elapsed = find.descendant(
          of: find.byKey(const Key('open-p1')),
          matching: find.byKey(const Key('power-elapsed-p1')),
        );
        expect(
          find.descendant(of: elapsed, matching: find.textContaining('경과 01:1')),
          findsOneWidget,
        );
        expect(
          find.descendant(
            of: find.byKey(const Key('power-elapsed-p2')),
            matching: find.byType(Text),
          ),
          findsNothing,
        );

        // The task ended: no mark and no clock, the tile is ready.
        shell.machines = [_vm('p1'), _vm('p2')];
        shell.powerStarted = const {};
        await _again(tester, shell);
        for (final id in ['p1', 'p2']) {
          expect(_mark('status', id), findsNothing);
          expect(_mark('action', id), findsNothing);
          expect(find.byKey(Key('power-elapsed-$id')), findsNothing);
        }
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'A Host update shows the share the Host reports while it downloads, runs without one in the other steps, and gives way to the result',
    (tester) async {
      final shell = _Shell([
        _vm('u1', hostUpdate: _updating('downloading', progress: 42)),
      ]);
      await _show(tester, shell);
      LinearProgressIndicator bar() => tester.widget<LinearProgressIndicator>(
        find.byKey(const Key('host-update-progress-u1')),
      );
      expect(bar().value, closeTo(.42, 1e-9));
      expect(find.text('Host 업데이트 중 · 다운로드 42%'), findsOneWidget);
      final height = tester.getSize(find.byKey(const Key('card-u1'))).height;

      // Downloading without a number, checking, installing, sent: the bar
      // runs back and forth; nothing fills it.
      for (final (status, progress) in [
        ('downloading', null),
        ('verifying', null),
        ('installing', null),
        ('accepted', null),
        ('dispatched', null),
      ]) {
        shell.machines = [
          _vm('u1', hostUpdate: _updating(status, progress: progress)),
        ];
        await _again(tester, shell);
        expect(bar().value, isNull, reason: status);
        expect(
          tester.getSize(find.byKey(const Key('card-u1'))).height,
          height,
          reason: 'the bar lies on the card\'s edge',
        );
      }

      // Waiting for a connection to end is not work: no bar.
      shell.machines = [_vm('u1', hostUpdate: _updating('waiting'))];
      await _again(tester, shell);
      expect(find.byKey(const Key('host-update-progress-u1')), findsNothing);
      expect(find.byKey(const Key('host-update-status-u1')), findsOneWidget);

      // The Host went away to install: the tile waits and moves.
      shell.machines = [
        _vm(
          'u1',
          hostOnline: false,
          vmState: 'host_lost',
          hostUpdate: _updating('installing'),
        ),
      ];
      await _again(tester, shell);
      expect(bar().value, isNull);
      expect(_mark('status', 'u1'), findsOneWidget);
      expect(_mark('thumb', 'u1'), findsOneWidget);
      expect(find.text('Host 업데이트 중'), findsWidgets);

      // The result replaces the bar: finished, or failed with its reason.
      for (final status in ['completed', 'failed']) {
        shell.machines = [_vm('u1', hostUpdate: _updating(status))];
        await _again(tester, shell);
        await tester.pumpAndSettle();
        expect(
          find.byKey(const Key('host-update-progress-u1')),
          findsNothing,
          reason: status,
        );
        expect(_mark('status', 'u1'), findsNothing, reason: status);
        expect(find.byKey(const Key('host-update-status-u1')), findsOneWidget);
      }
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Asking for a Host update shows on the tile until the server answers; a refusal takes its place',
    (tester) async {
      var answer = Completer<void>();
      final shell = _Shell([_vm('h1', hostUpdate: _offered)])
        ..onHostUpdate = (_, _, {required disconnectSessions}) => answer.future;
      await _show(tester, shell);
      Future<void> ask() async {
        await tester.tap(find.byKey(const Key('desktop-more-h1')));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('host-update-h1')));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('host-update-confirm')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 400));
      }

      await ask();
      final working = find.byKey(const Key('tile-working-h1'));
      expect(tester.widget<Text>(working).data, 'Host 업데이트를 요청하는 중이에요');
      expect(_mark('status', 'h1'), findsOneWidget);

      answer.complete();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      expect(working, findsNothing);
      expect(_mark('status', 'h1'), findsNothing);
      expect(find.textContaining('업데이트를 요청했어요'), findsOneWidget);
      // The answer's notice leaves before the next request.
      await tester.pump(const Duration(seconds: 5));
      await tester.pumpAndSettle();

      answer = Completer<void>();
      await ask();
      expect(working, findsOneWidget);
      answer.completeError(const BrokerException('host_update_unavailable'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      expect(working, findsNothing);
      expect(_mark('status', 'h1'), findsNothing);
      expect(find.byType(SnackBar), findsOneWidget);
      await tester.pump(const Duration(seconds: 5));
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Opening the Console shows on the tile from the question to the window; a failure takes its place',
    (tester) async {
      var opening = Completer<SessionWindowOpenResult?>();
      final asked = <String>[];
      final shell = _Shell([_vm('c1', consoleProtocol: 'console-webview-v1')])
        ..onOpenConsole = (machine) {
          asked.add(machine.id);
          return opening.future;
        };
      await _show(tester, shell);
      Future<void> open() async {
        await tester.tap(find.byKey(const Key('stream-modes-c1')));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('emergency-console-c1')));
        await tester.pumpAndSettle();
        // Nothing is opened and nothing moves while the question is open.
        expect(find.byKey(const Key('tile-working-c1')), findsNothing);
        await tester.tap(find.text('Console로 열기').last);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 400));
      }

      await open();
      expect(asked, ['c1']);
      final working = find.byKey(const Key('tile-working-c1'));
      expect(tester.widget<Text>(working).data, 'Console을 여는 중이에요');
      expect(_mark('status', 'c1'), findsOneWidget);
      // The streaming button is not what was pressed: it stays as it is.
      expect(_mark('action', 'c1'), findsNothing);

      // The window opened: the mark and the sentence are gone.
      opening.complete(null);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      expect(working, findsNothing);
      expect(_mark('status', 'c1'), findsNothing);
      expect(find.byKey(const Key('status-detail-c1')), findsOneWidget);
      await tester.pumpAndSettle();

      // It could not open: the reason is said, and nothing keeps turning.
      opening = Completer<SessionWindowOpenResult?>();
      await open();
      expect(working, findsOneWidget);
      opening.completeError(const BrokerException('console_webview_unavailable'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      expect(working, findsNothing);
      expect(_mark('status', 'c1'), findsNothing);
      expect(find.byType(SnackBar), findsOneWidget);
      await tester.pump(const Duration(seconds: 5));
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Opening a remote window marks the tile and holds its button until the window is there; a failure takes its place',
    (tester) async {
      var opening = Completer<SessionWindowOpenResult?>();
      var calls = 0;
      final shell = _Shell([_vm('w1')])
        ..onOpenWindow = (_, _, _) {
          calls++;
          return opening.future;
        };
      await _show(tester, shell);
      final height = tester.getSize(find.byKey(const Key('card-w1'))).height;
      FilledButton button() =>
          tester.widget<FilledButton>(find.byKey(const Key('open-w1')));
      expect(button().onPressed, isNotNull);

      await tester.tap(find.byKey(const Key('open-w1')));
      await acceptStreamConnect(tester, 'w1');
      expect(calls, 1);
      final working = find.byKey(const Key('tile-working-w1'));
      expect(tester.widget<Text>(working).data, '원격 창을 여는 중이에요');
      expect(_mark('status', 'w1'), findsOneWidget);
      expect(_mark('action', 'w1'), findsOneWidget);
      expect(button().onPressed, isNull, reason: 'one window, one request');
      expect(tester.getSize(find.byKey(const Key('card-w1'))).height, height);

      opening.complete(const SessionWindowOpenResult(opened: true));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      expect(working, findsNothing);
      expect(_mark('status', 'w1'), findsNothing);
      expect(_mark('action', 'w1'), findsNothing);
      expect(button().onPressed, isNotNull);
      await tester.pumpAndSettle();

      opening = Completer<SessionWindowOpenResult?>();
      await tester.tap(find.byKey(const Key('open-w1')));
      await acceptStreamConnect(tester, 'w1');
      expect(working, findsOneWidget);
      opening.completeError(StateError('no window'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      expect(working, findsNothing);
      expect(_mark('action', 'w1'), findsNothing);
      expect(find.text('원격 창을 열지 못했어요. 다시 시도하세요.'), findsOneWidget);
      expect(button().onPressed, isNotNull);
      await tester.pump(const Duration(seconds: 5));
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Signing out says so in words; a list refresh does not borrow the sentence',
    (tester) async {
      final shell = _Shell([_vm('s1')])
        ..busy = true
        ..signingOut = true;
      await _show(tester, shell);
      const line = Key('workspace-signing-out');
      expect(tester.widget<Text>(find.byKey(line)).data, '로그아웃하는 중이에요');
      expect(_lineMark(line), findsOneWidget);
      expect(find.byKey(const Key('header-button-busy')), findsOneWidget);
      expect(find.byTooltip('로그아웃하는 중이에요'), findsWidgets);
      expect(find.byTooltip('목록을 새로고침하는 중이에요'), findsNothing);

      // An ordinary refresh: the bar and the button's spinner only.
      shell.signingOut = false;
      await _again(tester, shell);
      expect(find.byKey(line), findsNothing);
      expect(find.byTooltip('목록을 새로고침하는 중이에요'), findsOneWidget);

      // The sign-out failed: its error line, and nothing keeps turning.
      shell.busy = false;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: WorkspaceShell(
            onSignOut: () {},
            onRefresh: () {},
            displayName: 'alice',
            serverOrigin: _origin,
            machines: shell.machines,
            signOutError: '로그아웃하지 못했어요. 네트워크를 확인하세요.',
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byKey(line), findsNothing);
      expect(find.byKey(const Key('workspace-sign-out-error')), findsOneWidget);
      expect(find.byKey(const Key('header-button-busy')), findsNothing);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'The first list read says what the empty page waits for; the list, the empty note or the error takes its place',
    (tester) async {
      final shell = _Shell([])..busy = true;
      await _show(tester, shell);
      const text = Key('workspace-loading-text');
      expect(tester.widget<Text>(find.byKey(text)).data, '데스크톱 목록을 불러오는 중이에요');
      expect(_lineMark(text), findsOneWidget);
      expect(find.byKey(const Key('empty-desktops')), findsNothing);

      // Nothing is assigned: the empty note, no mark.
      shell.busy = false;
      await _again(tester, shell);
      await tester.pumpAndSettle();
      expect(find.byKey(text), findsNothing);
      expect(find.byKey(const Key('empty-desktops')), findsOneWidget);

      // The read failed: the error card, no mark beside it.
      shell.error = '데스크톱 목록을 불러오지 못했어요.';
      await _again(tester, shell);
      await tester.pumpAndSettle();
      expect(find.byKey(text), findsNothing);
      expect(find.byKey(const Key('workspace-error-card')), findsOneWidget);

      // A refresh over a list that is there keeps the list: no sentence.
      shell
        ..error = null
        ..busy = true
        ..machines = [_vm('l1')];
      await _again(tester, shell);
      expect(find.byKey(text), findsNothing);
      expect(find.byKey(const Key('card-l1')), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'A list row moves in the dot\'s place while its desktop is at work and counts this client\'s own power request in its button',
    (tester) async {
      Widget row(
        DesktopMachine machine, {
        bool dense = false,
        DesktopActions actions = const DesktopActions(),
      }) => MaterialApp(
        theme: buildAppTheme(),
        home: Scaffold(
          body: Center(
            child: SizedBox(
              width: 1100,
              child: DesktopRow(
                machine: machine,
                readiness: DesktopReadiness.of(machine),
                actions: actions,
                mode: SessionDisplayMode.windowed,
                dense: dense,
              ),
            ),
          ),
        ),
      );
      for (final dense in [false, true]) {
        // Settled: a dot, nothing turns.
        await tester.pumpWidget(row(_vm('r1'), dense: dense));
        await tester.pumpAndSettle();
        expect(_mark('status', 'r1'), findsNothing);
        final left = tester.getTopLeft(find.byKey(const Key('status-r1'))).dx;
        final height = tester.getSize(find.byKey(const Key('card-r1'))).height;

        for (final machine in [
          _vm('r1', state: 'stopped', busy: true, action: 'start'),
          _vm('r1', hostOnline: false, vmState: 'windows_starting'),
          _vm('r1', hostUpdate: _updating('installing')),
        ]) {
          await tester.pumpWidget(row(machine, dense: dense));
          await tester.pump(const Duration(milliseconds: 100));
          expect(_mark('status', 'r1'), findsOneWidget);
          // The words keep their place and the row its height.
          expect(
            tester.getTopLeft(find.byKey(const Key('status-r1'))).dx,
            left,
          );
          if (machine.hostUpdate.operation == null) {
            expect(
              tester.getSize(find.byKey(const Key('card-r1'))).height,
              height,
            );
          }
        }

        // This client's power request: the wait is counted in the row's
        // button (a mini row's button is too short for it).
        await tester.pumpWidget(
          row(
            _vm('r1', busy: true, action: 'shutdown'),
            dense: dense,
            actions: DesktopActions(
              powerSince: DateTime.now().subtract(const Duration(seconds: 130)),
            ),
          ),
        );
        await tester.pump(const Duration(milliseconds: 100));
        expect(
          find.descendant(
            of: find.byKey(const Key('power-elapsed-r1')),
            matching: find.textContaining('경과 02:1'),
          ),
          dense ? findsNothing : findsOneWidget,
        );

        // What this client is doing for the desktop leads the row's words.
        await tester.pumpWidget(
          row(
            _vm('r1'),
            dense: dense,
            actions: const DesktopActions(working: 'Console을 여는 중이에요'),
          ),
        );
        await tester.pump(const Duration(milliseconds: 100));
        expect(_mark('status', 'r1'), findsOneWidget);
        expect(
          tester.widget<Text>(find.byKey(const Key('status-r1'))).data,
          startsWith('Console을 여는 중이에요'),
        );
        expect(tester.takeException(), isNull);
      }
    },
  );
}

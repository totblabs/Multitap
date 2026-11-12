import 'dart:async';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/desktop_thumbnails.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/host_update.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_tile.dart';
import 'package:multitap_client/ui/vm_resource_bars.dart';
import 'package:multitap_client/ui/workspace_shell.dart';
import 'helpers/stream_connect_consent.dart';

const viewOrigin = 'https://fixture.example';

DesktopMachine viewMachine(
  int number, {
  bool stopped = false,
  StreamPreparation? preparation,
  double cpu = 23,
}) => DesktopMachine(
  id: 'pve-$number',
  name: '업무 PC $number · 긴 이름 비교',
  description: '',
  kind: MachineKind.vm,
  status: stopped
      ? MachineStatus.poweredOff
      : preparation == null
      ? MachineStatus.ready
      : MachineStatus.awaitingHost,
  specification: 'Windows 11',
  accent: Colors.blue,
  hostOnline: !stopped,
  interactiveSession: !stopped,
  virtualDisplayReady: !stopped && preparation == null,
  streamPreparation: preparation,
  powerState: stopped ? 'stopped' : 'running',
  powerStale: false,
  powerCanForceStop: !stopped,
  resources: VmResources(
    cpuUsagePercent: cpu,
    vcpus: 4,
    memoryUsedBytes: 3 * 1024 * 1024 * 1024,
    memoryTotalBytes: 8 * 1024 * 1024 * 1024,
    stale: false,
    checkedAt: DateTime.now().toUtc(),
  ),
  organize: DesktopOrganize(
    supported: true,
    favorite: number == 101,
    tags: [number.isEven ? '개발' : '업무'],
  ),
  hostUpdate: number == 101
      ? const HostUpdateState(
          present: true,
          connected: true,
          supported: true,
          newer: true,
          available: true,
          version: '0.2.79',
          latestVersion: '0.2.81',
        )
      : const HostUpdateState(),
);

class LauncherViewFixture {
  LauncherViewFixture(
    List<DesktopMachine> initial, {
    LauncherMemory? memory,
    this.thumbnails,
  }) : machines = ValueNotifier(initial),
       memory = memory ?? SessionLauncherMemory();
  final ValueNotifier<List<DesktopMachine>> machines;
  final LauncherMemory memory;
  final ThumbnailSource? thumbnails;
  final opens = <(String, SessionDisplayMode)>[];
  final powers = <(String, String)>[];
  final favorites = <(String, bool)>[];
  final captureKey = GlobalKey();

  Widget widget({
    Size size = const Size(1366, 900),
    double scale = 1,
    Brightness brightness = Brightness.dark,
  }) => RepaintBoundary(
    key: captureKey,
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildAppTheme(brightness: brightness),
      builder: (_, child) => MediaQuery(
        data: MediaQueryData(size: size, textScaler: TextScaler.linear(scale)),
        child: child!,
      ),
      home: ValueListenableBuilder<List<DesktopMachine>>(
        valueListenable: machines,
        builder: (_, items, _) => WorkspaceShell(
          onSignOut: () {},
          onRefresh: () {},
          displayName: 'tester',
          serverOrigin: viewOrigin,
          machines: items,
          launcherMemory: memory,
          thumbnails: thumbnails,
          localMonitors: 1,
          onConnect: (_, {options = const StreamOptions()}) async =>
              throw StateError('No real connection'),
          onOpenWindow: (machine, mode, _) async {
            opens.add((machine.id, mode));
            return const SessionWindowOpenResult(opened: true);
          },
          onPower: (id, action) async {
            powers.add((id, action));
          },
          onHostUpdate: (_, _, {required disconnectSessions}) async =>
              throw StateError('No real update'),
          onFavorite: (id, value) async {
            favorites.add((id, value));
            machines.value = [
              for (final machine in machines.value)
                if (machine.id == id)
                  machine.withOrganize(machine.organize.withFavorite(value))
                else
                  machine,
            ];
          },
        ),
      ),
    ),
  );
}

Future<LauncherViewFixture> showLauncherView(
  WidgetTester t, {
  LauncherView view = LauncherView.cards,
  List<DesktopMachine>? machines,
  LauncherMemory? memory,
  ThumbnailSource? thumbnails,
  Size size = const Size(1366, 900),
  double scale = 1,
  Brightness brightness = Brightness.dark,
}) async {
  t.view.physicalSize = size;
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  final stored = memory ?? SessionLauncherMemory();
  if (memory == null) {
    await stored.save(viewOrigin, LauncherPreferences(view: view));
  }
  final fixture = LauncherViewFixture(
    machines ?? [viewMachine(101), viewMachine(102, stopped: true)],
    memory: stored,
    thumbnails: thumbnails,
  );
  addTearDown(fixture.machines.dispose);
  await t.pumpWidget(
    fixture.widget(size: size, scale: scale, brightness: brightness),
  );
  await t.pumpAndSettle();
  return fixture;
}

class _DelayedMemory extends SessionLauncherMemory {
  final ready = Completer<LauncherPreferences>();
  @override
  Future<LauncherPreferences> load(String server) => ready.future;
  LauncherPreferences? lastSaved;
  @override
  Future<void> save(String server, LauncherPreferences value) async {
    lastSaved = value;
  }
}

class _SlowWrites extends SessionLauncherMemory {
  final gate = Completer<void>();
  final started = <LauncherPreferences>[];
  @override
  Future<void> save(String server, LauncherPreferences value) async {
    started.add(value);
    if (started.length == 1) {
      await gate.future;
    }
    await super.save(server, value);
  }
}

class _Thumbnails implements ThumbnailSource {
  int reads = 0;
  final changes = StreamController<String>.broadcast();
  @override
  Stream<String> get updates => changes.stream;
  @override
  Future<DesktopThumbnail?> read(
    String desktopId, {
    bool refresh = false,
  }) async {
    reads++;
    return null;
  }
}

void main() {
  testWidgets(
    'Every legacy view uses cards and hidden previews do not read thumbnails',
    (t) async {
      for (final view in LauncherView.values) {
        final source = _Thumbnails(), memory = SessionLauncherMemory();
        await memory.save(
          viewOrigin,
          LauncherPreferences(view: view, showPreviews: false),
        );
        await showLauncherView(t, memory: memory, thumbnails: source);
        source.changes.add('pve-101');
        await t.pumpAndSettle();
        expect(source.reads, 0, reason: '$view hidden previews');
        expect(find.byType(DesktopTile), findsWidgets);
        expect(find.byType(DesktopRow), findsNothing);
        expect(find.byKey(const Key('desktop-view')), findsNothing);
        await t.pumpWidget(const SizedBox());
        await source.changes.close();
      }
    },
  );
  testWidgets('Rapid preview toggles serialize writes and keep cards', (
    t,
  ) async {
    final memory = _SlowWrites();
    await showLauncherView(t, memory: memory, machines: [viewMachine(101)]);
    await t.tap(find.byKey(const Key('desktop-previews')));
    await t.pumpAndSettle();
    await t.tap(find.byKey(const Key('desktop-previews')));
    await t.pumpAndSettle();
    expect(memory.started, hasLength(1));
    memory.gate.complete();
    await t.pumpAndSettle();
    expect(memory.started, hasLength(2));
    expect((await memory.load(viewOrigin)).view, LauncherView.cards);
    expect((await memory.load(viewOrigin)).showPreviews, isTrue);
  });

  testWidgets(
    'A saved mini layout reveals the remembered VM in the card grid without connecting',
    (t) async {
      final memory = SessionLauncherMemory();
      await memory.save(
        viewOrigin,
        const LauncherPreferences(
          view: LauncherView.mini,
          lastDesktop: 'pve-200',
        ),
      );
      final fixture = await showLauncherView(
        t,
        memory: memory,
        machines: [for (var i = 101; i < 201; i++) viewMachine(i)],
      );
      expect(find.byKey(const Key('card-pve-200')), findsOneWidget);
      expect(FocusManager.instance.primaryFocus?.debugLabel, 'desktop-pve-200');
      expect(fixture.opens, isEmpty);
      expect(t.takeException(), isNull);
    },
  );
  testWidgets('Cards are the only layout with 30 VMs or one VM', (t) async {
    await showLauncherView(
      t,
      machines: [for (var i = 101; i < 131; i++) viewMachine(i)],
    );
    expect(find.byType(DesktopTile), findsWidgets);
    expect(find.byType(DesktopRow), findsNothing);
    expect(find.byKey(const Key('desktop-view')), findsNothing);
    await t.pumpWidget(const SizedBox());
    await showLauncherView(t, machines: [viewMachine(101)]);
    expect(find.byType(DesktopTile), findsOneWidget);
    expect(find.byType(DesktopRow), findsNothing);
    expect(t.takeException(), isNull);
  });

  testWidgets('Card preview setting persists and survives opening a desktop', (
    t,
  ) async {
    final memory = SessionLauncherMemory();
    final f = await showLauncherView(
      t,
      memory: memory,
      machines: [viewMachine(101)],
    );
    final before = t.getSize(find.byKey(const Key('card-pve-101'))).height;
    await t.tap(find.byKey(const Key('desktop-previews')));
    await t.pumpAndSettle();
    expect(
      t.getSize(find.byKey(const Key('card-pve-101'))).height,
      lessThan(before),
    );
    expect(find.byKey(const Key('favorite-star-pve-101')), findsOneWidget);
    await t.tap(find.byKey(const Key('open-pve-101')));
      await acceptStreamConnect(t, 'pve-101');
    await t.pumpAndSettle();
    expect(f.opens, [('pve-101', SessionDisplayMode.windowed)]);
    expect((await memory.load(viewOrigin)).view, LauncherView.cards);
    expect((await memory.load(viewOrigin)).showPreviews, isFalse);
    await t.pumpWidget(const SizedBox());
    await showLauncherView(t, memory: memory, machines: [viewMachine(101)]);
    expect(find.byType(DesktopTile), findsOneWidget);
    expect(find.byType(DesktopRow), findsNothing);
  });

  testWidgets(
    'Preview choice before a slow read preserves cards and ignores saved fullscreen',
    (t) async {
      final memory = _DelayedMemory();
      await showLauncherView(t, memory: memory, machines: [viewMachine(101)]);
      await t.tap(find.byKey(const Key('desktop-previews')));
      await t.pumpAndSettle();
      memory.ready.complete(
        const LauncherPreferences(
          lastDesktop: 'pve-101',
          modes: {'pve-101': SessionDisplayMode.fullscreen},
        ),
      );
      await t.pumpAndSettle();
      expect(find.byType(DesktopTile), findsOneWidget);
      expect(find.byType(DesktopRow), findsNothing);
      expect(memory.lastSaved?.view, LauncherView.cards);
      expect(memory.lastSaved?.showPreviews, isFalse);
      expect(memory.lastSaved?.modeFor('pve-101'), SessionDisplayMode.windowed);
    },
  );

  for (final savedMode in [
    SessionDisplayMode.fullscreen,
    SessionDisplayMode.allMonitors,
  ]) {
    testWidgets(
      'Saved $savedMode cannot change button, picture, Enter or reopened defaults',
      (t) async {
        final memory = SessionLauncherMemory();
        await memory.save(
          viewOrigin,
          LauncherPreferences(
            lastDesktop: 'pve-101',
            modes: {'pve-101': savedMode},
          ),
        );
        final f = await showLauncherView(
          t,
          memory: memory,
          machines: [viewMachine(101)],
        );
        expect(
          t.widget<DesktopTile>(find.byType(DesktopTile)).mode,
          SessionDisplayMode.windowed,
        );
        expect(find.text('스트리밍 연결'), findsOneWidget);
        Future<void> expectWindowedOpen() async {
          await t.pumpAndSettle();
          expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
          expect(f.opens.last, ('pve-101', SessionDisplayMode.windowed));
        }

        await t.tap(find.byKey(const Key('open-pve-101')));
      await acceptStreamConnect(t, 'pve-101');
        await expectWindowedOpen();
        final mouse = await t.createGesture(kind: PointerDeviceKind.mouse);
        await mouse.addPointer(
          location: t.getCenter(find.byKey(const Key('card-pve-101'))),
        );
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('play-pve-101')));
      await acceptStreamConnect(t, 'pve-101');
        await expectWindowedOpen();
        await mouse.removePointer();
        t
            .widget<DesktopTile>(find.byType(DesktopTile))
            .focusNode!
            .requestFocus();
        await t.pump();
        // No key on the focused tile connects.
        for (final key in [
          LogicalKeyboardKey.enter,
          LogicalKeyboardKey.numpadEnter,
          LogicalKeyboardKey.space,
        ]) {
          await t.sendKeyEvent(key);
          await t.pumpAndSettle();
          expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
        }
        expect(f.opens.length, 2);
        expect((await memory.load(viewOrigin)).modes, isEmpty);

        await t.pumpWidget(const SizedBox());
        final reopened = await showLauncherView(
          t,
          memory: memory,
          machines: [viewMachine(101)],
        );
        await t.tap(find.byKey(const Key('open-pve-101')));
      await acceptStreamConnect(t, 'pve-101');
        await t.pumpAndSettle();
        expect(reopened.opens, [('pve-101', SessionDisplayMode.windowed)]);
        expect(t.takeException(), isNull);
      },
    );
  }

  for (final view in LauncherView.values) {
    testWidgets(
      '$view preserves explicit connect, power, favorites and Host-update status',
      (t) async {
        final f = await showLauncherView(
          t,
          view: view,
          machines: [viewMachine(101)],
        );
        final card = find.byKey(const Key('card-pve-101'));
        await t.tap(find.byKey(const Key('desktop-name-pve-101')));
        await t.tapAt(t.getTopLeft(card) + const Offset(4, 4));
        await t.pumpAndSettle();
        expect(f.opens, isEmpty);
        expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
        final badge = t.widget<Badge>(
          find.byKey(const Key('host-update-badge-pve-101')),
        );
        expect(badge.isLabelVisible, isTrue);
        await t.tap(find.byKey(const Key('desktop-more-pve-101')));
        await t.pumpAndSettle();
        expect(find.byKey(const Key('host-update-pve-101')), findsOneWidget);
        await t.tap(find.byKey(const Key('favorite-pve-101')));
        await t.pumpAndSettle();
        expect(f.favorites, [('pve-101', false)]);
        await t.tap(find.byKey(const Key('desktop-power-pve-101')));
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('power-reboot-pve-101')));
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('cancel-power-reboot-pve-101')));
        await t.pumpAndSettle();
        expect(f.powers, isEmpty);
        await t.tap(find.byKey(const Key('stream-modes-pve-101')));
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('stream-mode-fullscreen')));
      await acceptStreamConnect(t, 'pve-101');
        await t.pumpAndSettle();
        expect(f.opens, [('pve-101', SessionDisplayMode.fullscreen)]);
        final focus = t.widget<Focus>(
          find
              .descendant(
                of: find.byKey(const Key('tile-pve-101')),
                matching: find.byType(Focus),
              )
              .first,
        );
        focus.focusNode!.requestFocus();
        await t.pump();
        await t.sendKeyEvent(LogicalKeyboardKey.enter);
        await t.pumpAndSettle();
        expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
        expect(
          f.opens,
          hasLength(1),
          reason: 'Enter on the focused desktop connects nothing (2026-11-10).',
        );
        expect(t.takeException(), isNull);
      },
    );

    testWidgets(
      '$view keeps search/status/sort and live CPU/RAM observations',
      (t) async {
        final f = await showLauncherView(
          t,
          view: view,
          machines: [
            viewMachine(101),
            viewMachine(102, stopped: true),
            viewMachine(103),
          ],
        );
        await t.tap(find.byKey(const Key('status-stopped')));
        await t.pumpAndSettle();
        expect(find.byKey(const Key('card-pve-102')), findsOneWidget);
        expect(find.byKey(const Key('card-pve-101')), findsNothing);
        await t.tap(find.byKey(const Key('status-all')));
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('desktop-sort')));
        await t.pumpAndSettle();
        await t.tap(find.byKey(const Key('sort-name')));
        await t.pumpAndSettle();
        await t.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
        await t.sendKeyEvent(LogicalKeyboardKey.keyF);
        await t.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
        await t.pumpAndSettle();
        await t.enterText(find.byKey(const Key('desktop-search-input')), '103');
        await t.pumpAndSettle();
        expect(find.byKey(const Key('card-pve-103')), findsOneWidget);
        expect(find.byKey(const Key('card-pve-101')), findsNothing);
        f.machines.value = [
          viewMachine(101),
          viewMachine(102, stopped: true),
          viewMachine(103, cpu: 71),
        ];
        await t.pumpAndSettle();
        final resource = t.widget<VmResourceStrip>(
          find.byKey(const Key('resources-pve-103')),
        );
        expect(resource.sample.cpuUsagePercent, 71);
        expect(find.text('71%'), findsOneWidget);
        expect(f.opens, isEmpty);
        expect(t.takeException(), isNull);
      },
    );

    testWidgets(
      '$view builds a bounded visible set of 100 VMs and scrolls to the last VM',
      (t) async {
        final f = await showLauncherView(
          t,
          view: view,
          machines: [for (var i = 101; i < 201; i++) viewMachine(i)],
        );
        final itemType = DesktopTile;
        expect(find.byType(itemType).evaluate().length, lessThan(25));
        final scroll = t.widget<CustomScrollView>(
          find.byKey(const Key('workspace-scroll')),
        );
        scroll.controller!.jumpTo(scroll.controller!.position.maxScrollExtent);
        await t.pumpAndSettle();
        expect(find.byKey(const Key('card-pve-200')), findsOneWidget);
        expect(find.byType(itemType).evaluate().length, lessThan(25));
        expect(f.opens, isEmpty);
        // The header can be unmounted by lazy scrolling. Its keyboard
        // shortcut must still bring back a usable search field.
        await t.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
        await t.sendKeyEvent(LogicalKeyboardKey.keyF);
        await t.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
        await t.pumpAndSettle();
        await t.enterText(find.byKey(const Key('desktop-search-input')), '200');
        await t.pumpAndSettle();
        expect(find.byKey(const Key('card-pve-200')), findsOneWidget);
        expect(find.byKey(const Key('card-pve-101')), findsNothing);
        expect(t.takeException(), isNull);
      },
    );
  }
}

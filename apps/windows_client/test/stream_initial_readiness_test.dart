// Production RemoteView entry with injected broker/native-window boundaries.
// No authenticated VM, Windows display transition or actual media is used.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/console_preparation.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'stream_display_entry_test.dart' show entryMachine;

DesktopMachine pending({
  String id = 'pve-149',
  StreamPreparation reason = StreamPreparation.checking,
}) => DesktopMachine(
  id: id,
  name: 'TESTER',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: true,
  interactiveSession: true,
  virtualDisplayReady: true,
  streamPreparation: reason,
  display: const VirtualDisplayState(activeCount: 1, stale: false),
  monitors: const [RemoteMonitor(id: '0', width: 1264, height: 760)],
);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  testWidgets(
    'ready stream alone does not skip a temporarily held display-control wait',
    (t) async {
      const held = DesktopMachine(
        id: 'pve-149',
        name: 'TESTER',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.ready,
        specification: 'Windows',
        accent: Colors.blue,
        hostOnline: true,
        interactiveSession: true,
        virtualDisplayReady: true,
        display: VirtualDisplayState(activeCount: 1, stale: false),
        monitors: [
          RemoteMonitor(id: '0', width: 1468, height: 1080, primary: true),
        ],
      );
      final restored = entryMachine(1);
      var reads = 0, completed = false;
      final result = waitForStreamDisplayControl(
        held,
        refresh: () async {
          reads++;
          return restored;
        },
        current: () => true,
      );
      unawaited(result.then((_) => completed = true));
      await t.pump();
      expect(held.streamPreparation, isNull);
      expect(held.canConfigureDisplays, isFalse);
      expect(completed, isFalse);
      expect(reads, 0);
      await t.pump(const Duration(milliseconds: 501));
      expect(await result, same(restored));
      expect(reads, 1);
    },
  );
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const window = MethodChannel('multitap/window');
  const events = MethodChannel('multitap/window/events');
  final presentationCalls = <MethodCall>[];
  setUp(() {
    presentationCalls.clear();
    messenger.setMockMethodCallHandler(events, (_) async => null);
    messenger.setMockMethodCallHandler(window, (call) async {
      presentationCalls.add(call);
      if (call.method == 'monitorLayout') {
        return {
          'x': 0,
          'y': 0,
          'width': 1280,
          'height': 360,
          'monitors': [
            for (var i = 0; i < 2; i++)
              {
                'id': '$i',
                'x': i * 640,
                'y': 0,
                'width': 640,
                'height': 360,
                'primary': i == 0,
                'dpi': 96,
              },
          ],
        };
      }
      return call.method == 'fullscreen' || call.method == 'allMonitors';
    });
  });
  tearDown(() {
    messenger.setMockMethodCallHandler(window, null);
    messenger.setMockMethodCallHandler(events, null);
  });

  for (final mode in SessionDisplayMode.values) {
    testWidgets('first $mode entry waits for fresh display capabilities', (
      t,
    ) async {
      var reads = 0, mutations = 0, connects = 0;
      final count = mode == SessionDisplayMode.allMonitors ? 2 : 1;
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: pending(),
            prepareDisplaysOnEntry: true,
            initialDisplayMode: mode,
            onRefreshDesktop: () async =>
                ++reads == 1 ? pending() : entryMachine(count),
            onConfigureDisplays: (_) async {
              mutations++;
              return entryMachine(count);
            },
            onReconnect: (_) async {
              connects++;
              throw const BrokerException('network_error');
            },
            onConnectTracks: (_, _) async {
              connects++;
              throw const BrokerException('network_error');
            },
            load: () async {
              connects++;
              throw const BrokerException('network_error');
            },
          ),
        ),
      );
      await t.pump();
      await t.pump(const Duration(milliseconds: 100));
      expect(connects, 0);
      expect(mutations, 0);
      expect(find.text('가상 모니터 구성을 지원하지 않습니다.'), findsNothing);
      await t.pump(const Duration(milliseconds: 501));
      for (var i = 0; i < 5; i++) {
        await t.pump();
      }
      expect(reads, 2);
      expect(connects, 1);
      expect(
        mutations,
        0,
        reason: 'An already verified count must not flap devices.',
      );
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    });
  }

  testWidgets('retiring initial wait cannot issue a late count or connection', (
    t,
  ) async {
    var reads = 0, mutations = 0, connects = 0;
    await t.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: pending(),
          prepareDisplaysOnEntry: true,
          onRefreshDesktop: () async {
            reads++;
            return pending();
          },
          onConfigureDisplays: (_) async {
            mutations++;
            return entryMachine(1);
          },
          load: () async {
            connects++;
            throw const BrokerException('network_error');
          },
        ),
      ),
    );
    await t.pump();
    expect(reads, 1);
    await t.pumpWidget(const SizedBox());
    await t.pump(const Duration(seconds: 2));
    expect(reads, 1);
    expect(mutations, 0);
    expect(connects, 0);
    expect(t.takeException(), isNull);
  });

  testWidgets(
    'explicitly confirmed console return waits for post-activation display flags',
    (t) async {
      const disconnected = DesktopMachine(
        id: 'pve-149',
        name: 'TESTER',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.awaitingHost,
        specification: 'Windows',
        accent: Colors.blue,
        hostOnline: true,
        interactiveSession: false,
        consolePreparation: ConsolePreparationState(
          supported: true,
          available: true,
          reason: 'rdp_session_disconnected',
        ),
      );
      var resumed = false,
          reads = 0,
          transfers = 0,
          connects = 0,
          mutations = 0;
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: disconnected,
            prepareDisplaysOnEntry: true,
            onRefreshDesktop: () async {
              reads++;
              return resumed ? entryMachine(1) : disconnected;
            },
            onPrepareConsole: () async {
              transfers++;
              resumed = true;
              return pending();
            },
            onConfigureDisplays: (_) async {
              mutations++;
              return entryMachine(1);
            },
            onReconnect: (_) async {
              connects++;
              throw const BrokerException('network_error');
            },
            load: () async {
              connects++;
              throw const BrokerException('network_error');
            },
          ),
        ),
      );
      await t.pumpAndSettle();
      expect(transfers, 0);
      expect(connects, 0);
      await t.tap(find.byKey(const Key('rdp-handover-accept')));
      // Consent opens the progress dialog, which starts exactly one request.
      for (var i = 0; i < 20 && transfers == 0; i++) {
        await t.pump(const Duration(milliseconds: 10));
      }
      expect(transfers, 1);
      expect(connects, 0);
      await t.pump();
      await t.pump(const Duration(milliseconds: 100));
      expect(transfers, 1);
      expect(connects, 0);
      expect(mutations, 0);
      await t.pump(const Duration(milliseconds: 501));
      for (var i = 0; i < 5; i++) {
        await t.pump();
      }
      expect(reads, 2);
      expect(connects, 1);
      expect(
        transfers,
        1,
        reason: 'Read-only readiness checks cannot repeat Windows transfer.',
      );
      expect(mutations, 0);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );

  testWidgets('initial readiness timeout does not retry a display mutation', (
    t,
  ) async {
    var reads = 0, mutations = 0;
    await t.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: pending(),
          prepareDisplaysOnEntry: true,
          onRefreshDesktop: () async {
            reads++;
            return pending();
          },
          onConfigureDisplays: (_) async {
            mutations++;
            return entryMachine(1);
          },
          load: () async => throw const BrokerException('network_error'),
        ),
      ),
    );
    await t.pump();
    for (var i = 0; i < 31; i++) {
      await t.pump(const Duration(milliseconds: 501));
    }
    expect(reads, greaterThan(1));
    expect(mutations, 0);
    expect(find.text('다시 연결'), findsOneWidget);
    final stoppedAt = reads;
    await t.pump(const Duration(seconds: 2));
    expect(reads, stoppedAt);
    expect(t.takeException(), isNull);
    await t.pumpWidget(const SizedBox());
  });

  testWidgets('a different VM in the initial recheck cannot reach capture', (
    t,
  ) async {
    var reads = 0, connects = 0;
    await t.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: pending(),
          prepareDisplaysOnEntry: true,
          onRefreshDesktop: () async =>
              ++reads == 1 ? pending() : entryMachine(1, id: 'pve-170'),
          load: () async {
            connects++;
            throw const BrokerException('network_error');
          },
        ),
      ),
    );
    await t.pump();
    await t.pump(const Duration(milliseconds: 501));
    await t.pump();
    expect(reads, 2);
    expect(connects, 0);
    expect(find.text('다시 연결'), findsOneWidget);
    expect(t.takeException(), isNull);
    await t.pumpWidget(const SizedBox());
  });

  testWidgets('terminal display configuration failure remains immediate', (
    t,
  ) async {
    var reads = 0, mutations = 0;
    final machine = pending(reason: StreamPreparation.configuration);
    await t.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: machine,
          prepareDisplaysOnEntry: true,
          onRefreshDesktop: () async {
            reads++;
            return machine;
          },
          onConfigureDisplays: (_) async {
            mutations++;
            return entryMachine(1);
          },
          load: () async => throw const BrokerException('network_error'),
        ),
      ),
    );
    for (var i = 0; i < 5; i++) {
      await t.pump();
    }
    expect(find.text('다시 연결'), findsOneWidget);
    await t.pump(const Duration(seconds: 2));
    expect(reads, 1);
    expect(mutations, 0);
    expect(t.takeException(), isNull);
    await t.pumpWidget(const SizedBox());
  });
}

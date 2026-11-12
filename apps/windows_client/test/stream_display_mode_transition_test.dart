// Flutter menu/session orchestration with injected Windows geometry, media and
// broker boundaries. This does not prove physical monitors or real VM capture.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;
import 'stream_display_entry_test.dart' show entryMachine;

Map<String, Object?> layout(int count) => {
  'x': 0,
  'y': 0,
  'width': 640 * count,
  'height': 360,
  'monitors': [
    for (var i = 0; i < count; i++)
      {
        'id': '$i',
        'x': 640 * i,
        'y': 0,
        'width': 640,
        'height': 360,
        'primary': i == 0,
        'dpi': 96,
      },
  ],
};

/// The session toolbar rests as a handle; a click opens it. 화면 then drops
/// its panel, where the screen modes are tiles (2026-11-10).
Future<void> openScreenMenu(WidgetTester t) async {
  if (find.byKey(const Key('session-command-bar')).evaluate().isEmpty) {
    await t.tap(find.byKey(const Key('expand-session-toolbar')));
    await t.pump();
  }
  await t.tap(find.byKey(const Key('stream-menu-화면')));
}

/// A screen-mode tile of the 화면 panel (its InkWell carries the key).
InkWell modeTile(WidgetTester t, String key) =>
    t.widget<InkWell>(find.byKey(Key(key)));

/// Whether that tile is the current mode: its Semantics `selected`.
bool? modeSelected(WidgetTester t, String key) => t
    .widget<Semantics>(
      find
          .ancestor(of: find.byKey(Key(key)), matching: find.byType(Semantics))
          .first,
    )
    .properties
    .selected;

class ModeFixture {
  ModeFixture({this.refresh});
  final Future<DesktopMachine> Function()? refresh;
  final socket = SessionSocket(), media = SessionMedia();
  final calls = <String>[];
  late final session = RemoteSession(
    connection: BrokerConnection.fromJson(
      {
        'sessionId': 'mode-qa',
        'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/mode-qa',
      },
      Uri.parse('https://fixture.invalid'),
      requestedOptions: const StreamOptions(
        maxWidth: 1280,
        maxHeight: 720,
        audio: false,
      ),
    ),
    token: 'fixture-token',
    media: media,
    dialer: (_, _) async => socket,
  );
  Future<void> mount(WidgetTester t) async {
    await t.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: entryMachine(1),
          onRefreshDesktop: refresh,
          initialDisplayMode: SessionDisplayMode.windowed,
          load: () async => session,
          onConfigureDisplays: (count) async {
            calls.add('count:$count');
            return entryMachine(count);
          },
          onConnectTracks: (_, _) async {
            calls.add('tracks');
            throw const BrokerException('network_error');
          },
          onReconnect: (_) async {
            calls.add('single');
            throw const BrokerException('network_error');
          },
        ),
      ),
    );
    await t.pump();
    await t.pump();
    socket.incoming.add(jsonEncode(info));
    socket.incoming.add(packet());
    await t.pump();
    await t.pump(const Duration(seconds: 1));
    await t.pump();
    expect(session.frameReady, isTrue, reason: session.error);
    expect(socket.wasClosed, isFalse);
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const window = MethodChannel('multitap/window'),
      events = MethodChannel('multitap/window/events');
  setUp(() => messenger.setMockMethodCallHandler(events, (_) async => null));
  tearDown(() {
    messenger.setMockMethodCallHandler(window, null);
    messenger.setMockMethodCallHandler(events, null);
  });

  testWidgets(
    'one local monitor hides all-monitor mode and preserves the current stream',
    (t) async {
      final f = ModeFixture();
      messenger.setMockMethodCallHandler(window, (call) async {
        f.calls.add(call.method);
        return call.method == 'monitorLayout' ? layout(1) : false;
      });
      await f.mount(t);
      // This fixture has no session-managed count, so 화면 still offers the
      // virtual-monitor count, but never a mode this PC cannot show.
      await openScreenMenu(t);
      await t.pumpAndSettle();
      expect(find.byKey(const Key('session-screen-panel')), findsOneWidget);
      expect(find.byKey(const Key('stream-windowed')), findsOneWidget);
      expect(find.byKey(const Key('stream-fullscreen')), findsOneWidget);
      expect(find.byKey(const Key('stream-all-monitors')), findsNothing);
      expect(modeSelected(t, 'stream-windowed'), isTrue);
      expect(modeSelected(t, 'stream-fullscreen'), isFalse);
      expect(find.byKey(const Key('remote-virtual-displays')), findsWidgets);
      expect(f.socket.wasClosed, isFalse);
      expect(f.media.stopped, isFalse);
      expect(
        f.calls.where((v) => v.startsWith('count:') || v == 'tracks'),
        isEmpty,
      );
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );

  testWidgets(
    'a monitor unplugged after the menu opens cannot close an active stream',
    (t) async {
      final f = ModeFixture();
      var count = 2;
      messenger.setMockMethodCallHandler(window, (call) async {
        f.calls.add(call.method);
        return call.method == 'monitorLayout' ? layout(count) : false;
      });
      await f.mount(t);
      await openScreenMenu(t);
      await t.pumpAndSettle();
      expect(modeTile(t, 'stream-all-monitors').onTap, isNotNull);
      expect(modeSelected(t, 'stream-all-monitors'), isFalse);
      count = 1;
      f.calls.clear();
      await t.tap(find.byKey(const Key('stream-all-monitors')));
      await t.pump();
      await t.pump();
      expect(f.socket.wasClosed, isFalse);
      expect(f.media.stopped, isFalse);
      expect(f.calls, contains('monitorLayout'));
      expect(
        f.calls.where(
          (v) => v == 'allMonitors' || v.startsWith('count:') || v == 'tracks',
        ),
        isEmpty,
      );
      expect(f.session.frameReady, isTrue);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );

  testWidgets(
    'late all-monitor validation after closing the remote view cannot mutate the window or VM',
    (t) async {
      final f = ModeFixture();
      Completer<Map<String, Object?>>? pending;
      messenger.setMockMethodCallHandler(window, (call) async {
        f.calls.add(call.method);
        if (call.method == 'monitorLayout') return pending?.future ?? layout(2);
        return false;
      });
      await f.mount(t);
      await openScreenMenu(t);
      await t.pumpAndSettle();
      pending = Completer<Map<String, Object?>>();
      f.calls.clear();
      await t.tap(find.byKey(const Key('stream-all-monitors')));
      await t.pump();
      await t.pump();
      await t.pumpWidget(const SizedBox());
      await t.pump();
      pending.complete(layout(2));
      await t.pump();
      await t.pump();
      expect(
        f.calls.where(
          (v) => v == 'allMonitors' || v.startsWith('count:') || v == 'tracks',
        ),
        isEmpty,
      );
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'valid dual mode validates before teardown, then prepares two outputs before connection',
    (t) async {
      final f = ModeFixture(), closedAtRead = <bool>[];
      t.view.devicePixelRatio = 1;
      t.view.physicalSize = const Size(1280, 720);
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      messenger.setMockMethodCallHandler(window, (call) async {
        f.calls.add(call.method);
        if (call.method == 'monitorLayout') {
          closedAtRead.add(f.socket.wasClosed);
          return {
            ...layout(2),
            'height': 720,
            'monitors': [
              for (final monitor in layout(2)['monitors'] as List)
                {...monitor as Map, 'height': 720},
            ],
          };
        }
        return call.method == 'allMonitors';
      });
      await f.mount(t);
      await openScreenMenu(t);
      await t.pumpAndSettle();
      f.calls.clear();
      closedAtRead.clear();
      await t.tap(find.byKey(const Key('stream-all-monitors')));
      await t.pump();
      await t.pump();
      await t.pump();
      await t.pump();
      expect(closedAtRead, [false, false, true]);
      expect(f.calls, [
        'monitorLayout',
        'allMonitors',
        'monitorLayout',
        'count:2',
        'monitorLayout',
        'tracks',
      ]);
      expect(f.socket.wasClosed, isTrue);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );

  testWidgets(
    'reconnect waits for previous host profile release before preparing the new count',
    (t) async {
      late ModeFixture f;
      var released = false, reads = 0;
      f = ModeFixture(
        refresh: () async {
          reads++;
          if (!f.socket.wasClosed || released) return entryMachine(1);
          return const DesktopMachine(
            id: 'pve-149',
            name: 'TESTER',
            description: '',
            kind: MachineKind.vm,
            status: MachineStatus.ready,
            specification: 'Windows',
            accent: Colors.blue,
            hostOnline: true,
            interactiveSession: true,
            independentDisplayStreaming: true,
            // Active profile/stop cleanup temporarily withdraws count control.
            display: VirtualDisplayState(activeCount: 1, stale: false),
          );
        },
      );
      messenger.setMockMethodCallHandler(window, (call) async {
        f.calls.add(call.method);
        return call.method == 'monitorLayout'
            ? layout(2)
            : call.method == 'allMonitors';
      });
      await f.mount(t);
      await openScreenMenu(t);
      await t.pumpAndSettle();
      f.calls.clear();
      await t.tap(find.byKey(const Key('stream-all-monitors')));
      await t.pump();
      await t.pump();
      expect(f.socket.wasClosed, isTrue);
      expect(
        f.calls.where((v) => v.startsWith('count:') || v == 'tracks'),
        isEmpty,
      );
      released = true;
      await t.pump(const Duration(milliseconds: 600));
      await t.pump();
      await t.pump();
      expect(reads, greaterThanOrEqualTo(3));
      expect(f.calls.where((v) => v.startsWith('count:') || v == 'tracks'), [
        'count:2',
        'tracks',
      ]);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );

  testWidgets(
    'closing during host profile release never applies a late count or native mode',
    (t) async {
      late ModeFixture f;
      var reads = 0;
      f = ModeFixture(
        refresh: () async {
          reads++;
          if (!f.socket.wasClosed) return entryMachine(1);
          return const DesktopMachine(
            id: 'pve-149',
            name: 'TESTER',
            description: '',
            kind: MachineKind.vm,
            status: MachineStatus.ready,
            specification: 'Windows',
            accent: Colors.blue,
            hostOnline: true,
            interactiveSession: true,
            independentDisplayStreaming: true,
            display: VirtualDisplayState(activeCount: 1, stale: false),
          );
        },
      );
      messenger.setMockMethodCallHandler(window, (call) async {
        f.calls.add(call.method);
        return call.method == 'monitorLayout'
            ? layout(2)
            : call.method == 'allMonitors';
      });
      await f.mount(t);
      await openScreenMenu(t);
      await t.pumpAndSettle();
      f.calls.clear();
      await t.tap(find.byKey(const Key('stream-all-monitors')));
      await t.pump();
      await t.pump();
      final beforeClose = reads;
      expect(f.calls.where((v) => v == 'allMonitors'), hasLength(1));
      await t.pumpWidget(const SizedBox());
      await t.pump(const Duration(seconds: 16));
      expect(reads, beforeClose);
      expect(
        f.calls.where((v) => v == 'allMonitors'),
        hasLength(1),
        reason: 'only the prevalidated native mode may precede cancellation',
      );
      expect(
        f.calls.where((v) => v.startsWith('count:') || v == 'tracks'),
        isEmpty,
      );
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'local validation timeout preserves the stream and leaves a retry available',
    (t) async {
      final f = ModeFixture();
      Completer<Map<String, Object?>>? pending;
      messenger.setMockMethodCallHandler(window, (call) async {
        f.calls.add(call.method);
        if (call.method == 'monitorLayout') return pending?.future ?? layout(2);
        return false;
      });
      await f.mount(t);
      await openScreenMenu(t);
      await t.pumpAndSettle();
      pending = Completer<Map<String, Object?>>();
      f.calls.clear();
      await t.tap(find.byKey(const Key('stream-all-monitors')));
      await t.pump();
      await t.pump(const Duration(seconds: 4));
      await t.pump();
      expect(f.socket.wasClosed, isFalse);
      expect(f.media.stopped, isFalse);
      pending.complete(layout(2));
      await t.pump();
      expect(
        f.calls.where(
          (v) => v == 'allMonitors' || v.startsWith('count:') || v == 'tracks',
        ),
        isEmpty,
      );
      if (find
          .byKey(const Key('expand-session-toolbar'))
          .evaluate()
          .isNotEmpty) {
        await t.tap(find.byKey(const Key('expand-session-toolbar')));
        await t.pump();
      }
      await openScreenMenu(t);
      await t.pumpAndSettle();
      expect(modeTile(t, 'stream-all-monitors').onTap, isNotNull);
      expect(modeSelected(t, 'stream-all-monitors'), isFalse);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );
  testWidgets(
    'native dual mode refusal preserves the live window stream and never changes Host count',
    (t) async {
      t.view.devicePixelRatio = 1;
      t.view.physicalSize = const Size(1280, 720);
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      final f = ModeFixture();
      messenger.setMockMethodCallHandler(window, (call) async {
        f.calls.add(call.method);
        if (call.method == 'monitorLayout') return layout(2);
        if (call.method == 'allMonitors') {
          throw PlatformException(code: 'presentation_failed');
        }
        return false;
      });
      await f.mount(t);
      await openScreenMenu(t);
      await t.pumpAndSettle();
      f.calls.clear();
      await t.tap(find.byKey(const Key('stream-all-monitors')));
      await t.pump();
      await t.pump();
      await t.pump();
      expect(
        f.calls,
        containsAllInOrder(['monitorLayout', 'allMonitors', 'fullscreen']),
      );
      expect(
        f.calls.where((v) => v.startsWith('count:') || v == 'tracks'),
        isEmpty,
      );
      expect(f.socket.wasClosed, isFalse);
      expect(f.media.stopped, isFalse);
      expect(f.session.frameReady, isTrue);
      expect(find.text('화면 모드를 적용하지 못했어요. 현재 연결은 유지돼요.'), findsOneWidget);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );

  testWidgets(
    'dual reconnect waits for tracks advertisement even when count control has already returned',
    (t) async {
      t.view.devicePixelRatio = 1;
      t.view.physicalSize = const Size(1280, 720);
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      late ModeFixture f;
      var tracksReturned = false, reads = 0;
      f = ModeFixture(
        refresh: () async {
          reads++;
          if (!f.socket.wasClosed || tracksReturned) return entryMachine(1);
          final source = entryMachine(1);
          return DesktopMachine(
            id: source.id,
            name: source.name,
            description: source.description,
            kind: source.kind,
            status: source.status,
            specification: source.specification,
            accent: source.accent,
            hostOnline: true,
            interactiveSession: true,
            virtualMonitorControl: true,
            display: source.display,
            monitors: source.monitors,
            independentDisplayStreaming: false,
          );
        },
      );
      messenger.setMockMethodCallHandler(window, (call) async {
        f.calls.add(call.method);
        if (call.method == 'monitorLayout') {
          return {
            ...layout(2),
            'height': 720,
            'monitors': [
              for (final monitor in layout(2)['monitors'] as List)
                {...monitor as Map, 'height': 720},
            ],
          };
        }
        return call.method == 'allMonitors';
      });
      await f.mount(t);
      await openScreenMenu(t);
      await t.pumpAndSettle();
      f.calls.clear();
      await t.tap(find.byKey(const Key('stream-all-monitors')));
      for (var i = 0; i < 6; i++) {
        await t.pump();
      }
      expect(f.socket.wasClosed, isTrue);
      expect(
        f.calls.where((v) => v.startsWith('count:') || v == 'tracks'),
        isEmpty,
      );
      expect(
        find.text('모든 모니터로 연결하지 못했어요. 목록에서 창 모드나 전체 화면으로 연결하세요.'),
        findsNothing,
      );
      final before = reads;
      tracksReturned = true;
      await t.pump(const Duration(milliseconds: 500));
      for (var i = 0; i < 6; i++) {
        await t.pump();
      }
      expect(reads, greaterThan(before));
      expect(f.calls.where((v) => v.startsWith('count:') || v == 'tracks'), [
        'count:2',
        'tracks',
      ]);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );

  testWidgets(
    'missing dual advertisement expires without late observations or readiness promotion',
    (t) async {
      final source = entryMachine(1);
      final countOnly = DesktopMachine(
        id: source.id,
        name: source.name,
        description: source.description,
        kind: source.kind,
        status: source.status,
        specification: source.specification,
        accent: source.accent,
        hostOnline: true,
        interactiveSession: true,
        virtualMonitorControl: true,
        display: source.display,
        monitors: source.monitors,
        independentDisplayStreaming: false,
      );
      var reads = 0;
      final pending = waitForStreamDisplayControl(
        countOnly,
        refresh: () async {
          reads++;
          return countOnly;
        },
        current: () => true,
        independentTracks: true,
        maxWait: const Duration(milliseconds: 700),
      );
      final checked = expectLater(
        pending,
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'track_streaming_unavailable',
          ),
        ),
      );
      await t.pump(const Duration(milliseconds: 700));
      await checked;
      final atTimeout = reads;
      await t.pump(const Duration(seconds: 1));
      expect(reads, atTimeout);
      expect(countOnly.independentDisplayStreaming, isFalse);
    },
  );
}

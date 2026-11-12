import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/track_stream.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_window.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'session_recovery_test.dart' show mockPresentation;
import 'track_stream_test.dart' as track_fixture;
import 'helpers/stream_connect_consent.dart';

const operationId = '11111111-2222-4333-8444-555555555555';
VirtualDisplayState display({
  int count = 2,
  String status = 'succeeded',
  String id = operationId,
}) => VirtualDisplayState(
  supported: true,
  activeCount: count,
  checkedAt: DateTime.now(),
  stale: false,
  operation: VirtualDisplayOperation(
    id: id,
    status: status,
    requestedCount: count,
  ),
);
BrokerDesktop desktop({
  int count = 1,
  VirtualDisplayState? state,
  String id = 'pve-149',
  String kind = 'vm',
  bool profileTracks = false,
  bool independentDisplayStreaming = false,
}) => BrokerDesktop(
  id: id,
  name: 'TESTER',
  kind: kind,
  localPcApproved: kind == 'pc',
  profileTracks: profileTracks,
  independentDisplayStreaming: independentDisplayStreaming,
  description: '',
  os: 'Windows',
  status: 'ready',
  canConnect: true,
  hostConnected: true,
  virtualMonitorControl: true,
  display:
      state ??
      VirtualDisplayState(
        supported: true,
        activeCount: count,
        checkedAt: DateTime.now(),
        stale: false,
      ),
  monitors: [
    for (var i = 0; i < count; ++i)
      RemoteMonitor(id: '$i', width: 1920, height: 1080),
  ],
);
SessionWindowLaunch launch({
  bool expired = false,
  String id = 'pve-149',
  bool localPcApproved = false,
}) => SessionWindowLaunch(
  origin: Uri.parse('https://fixture.example'),
  token: 'fixture-token',
  expiresAt: DateTime.now().add(Duration(hours: expired ? -1 : 1)),
  desktopId: id,
  localPcApproved: localPcApproved,
  name: 'TESTER',
  mode: SessionDisplayMode.windowed,
  options: const StreamOptions(),
);

class _Guard implements WindowCloseGuard {
  Future<void> Function()? request;
  int closes = 0;
  bool accepted = true;
  @override
  void attach(Future<void> Function() value) => request = value;
  @override
  Future<bool> update({required bool busy, required bool dirty}) async => true;
  @override
  Future<bool> close() async {
    closes++;
    return accepted;
  }

  @override
  void dispose() {}
}

final _defaultConfiguredState = display();

class _Api extends Fake implements BrokerApi {
  int reads = 0, closes = 0, mutations = 0, polls = 0, connects = 0;
  BrokerDesktop current = desktop();
  VirtualDisplayState configureResult = _defaultConfiguredState;
  Completer<List<BrokerDesktop>>? pendingInventory;
  Completer<BrokerConnection>? pendingConnect;
  final nextState = Completer<VirtualDisplayState>();
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    reads++;
    return pendingInventory == null ? [current] : pendingInventory!.future;
  }

  @override
  Future<BrokerConnection> connect(
    String token,
    String id, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    connects++;
    if (pendingConnect != null) return pendingConnect!.future;
    throw const BrokerException('host_unavailable');
  }

  @override
  Future<VirtualDisplayState> configureDisplays(
    String token,
    String id,
    int count,
  ) async {
    mutations++;
    return configureResult;
  }

  @override
  Future<VirtualDisplayState> display(
    String token,
    String id, {
    String? operationId,
  }) async {
    polls++;
    expectSync(operationId, equals('11111111-2222-4333-8444-555555555555'));
    return nextState.future;
  }

  @override
  void close() {
    closes++;
  }
}

class _TrackApi extends _Api implements TrackBrokerApi, ProfileTrackBrokerApi {
  int ordinaryTracks = 0, profileTrackCalls = 0;
  @override
  Future<BrokerConnection> connectTracks(
    String token,
    String id,
    TrackPlan tracks,
    StreamOptions options, {
    int? displayScale,
  }) async {
    ordinaryTracks++;
    throw const BrokerException('fixture_ordinary_selected');
  }

  @override
  Future<BrokerConnection> connectProfileTracks(
    String token,
    String id,
    TrackPlan tracks,
    StreamOptions options, {
    int? displayScale,
  }) async {
    profileTrackCalls++;
    throw const BrokerException('fixture_profile_selected');
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  setUp(() {
    mockPresentation();
    messenger.setMockMethodCallHandler(
      SessionWindows.channel,
      (_) async => null,
    );
  });
  tearDown(
    () => messenger.setMockMethodCallHandler(SessionWindows.channel, null),
  );
  Future<RemoteView> open(
    WidgetTester t,
    _Api api,
    _Guard guard, {
    SessionWindowLaunch? bootstrap,
  }) async {
    await t.pumpWidget(
      MaterialApp(
        home: SessionWindow(
          launch: bootstrap ?? launch(),
          api: api,
          guard: guard,
        ),
      ),
    );
    await t.pump();
    await t.pump();
    await t.pump(const Duration(milliseconds: 20));
    return t.widget<RemoteView>(find.byType(RemoteView));
  }

  Future<void> close(WidgetTester t, _Guard guard) async {
    unawaited(guard.request!());
    await t.pump();
    await t.pump(const Duration(milliseconds: 300));
    // Only a live connection asks first; a loading one just closes.
    final confirm = find.widgetWithText(FilledButton, '연결 끊기');
    if (confirm.evaluate().isNotEmpty) {
      await t.tap(confirm);
      await t.pump();
      await t.pump(const Duration(milliseconds: 300));
    }
  }

  testWidgets(
    'a streaming window of a VM asks the launcher for the Console',
    (t) async {
      final api = _Api();
      final bootstrap = SessionWindowLaunch.fromMap(launch().toMap());
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: bootstrap, api: api, guard: _Guard()),
        ),
      );
      await t.pump();
      await t.pump();
      // The VM's Console is opened by the launcher (WebView), on request.
      expect(
        t.widget<RemoteView>(find.byType(RemoteView)).onOpenConsole,
        isNotNull,
      );
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );

  testWidgets(
    'PC raw group capability selects ordinary tracks on first request while VM keeps profile negotiation',
    (t) async {
      const pcId = 'pc-11111111-1111-4111-8111-111111111111';
      for (final kind in ['pc', 'vm']) {
        final id = kind == 'pc' ? pcId : 'pve-149';
        final api = _TrackApi()
          ..current = desktop(
            id: id,
            kind: kind,
            profileTracks: true,
            independentDisplayStreaming: true,
          );
        final guard = _Guard();
        final machine = machineFromBroker(api.current);
        expect(
          machine.profileTracks,
          isTrue,
          reason:
              'retain the Host observation rather than falsifying capabilities',
        );
        expect(machine.requiresProfileTracks, kind == 'vm');
        final view = await open(
          t,
          api,
          guard,
          bootstrap: launch(id: id, localPcApproved: kind == 'pc'),
        );
        await expectLater(
          view.onConnectTracks!(track_fixture.plan(), track_fixture.options),
          throwsA(
            isA<BrokerException>().having(
              (e) => e.code,
              'selected route',
              kind == 'pc'
                  ? 'fixture_ordinary_selected'
                  : 'fixture_profile_selected',
            ),
          ),
        );
        expect(api.ordinaryTracks, kind == 'pc' ? 1 : 0);
        expect(api.profileTrackCalls, kind == 'vm' ? 1 : 0);
        expect(
          api.mutations,
          0,
          reason: 'selection does not bypass or mutate count readiness',
        );
        await t.pumpWidget(const SizedBox());
        await t.pump();
      }
    },
  );

  testWidgets(
    'Initially expired bootstrap closes without inventory or connect',
    (t) async {
      final api = _Api(), guard = _Guard();
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(
            launch: launch(expired: true),
            api: api,
            guard: guard,
          ),
        ),
      );
      await t.pump();
      await t.pump(const Duration(milliseconds: 1));
      expect(api.reads, 0);
      expect(api.connects, 0);
      expect(guard.closes, 1);
      await t.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'Display ACK waits for correlated fresh actual output inventory',
    (t) async {
      final api = _Api(), guard = _Guard();
      final view = await open(t, api, guard);
      var completed = false;
      final configured = view.onConfigureDisplays!(2).then((value) {
        completed = true;
        return value;
      });
      await t.pump();
      expect(completed, isFalse); // ACK says2; inventory still1.
      await t.pump(const Duration(milliseconds: 710));
      expect(api.polls, 1);
      api.current = desktop(count: 2, state: display());
      api.nextState.complete(display());
      await t.pump();
      final actual = await configured;
      expect(actual.monitors.map((m) => m.id), ['0', '1']);
      expect(api.mutations, 1);
      await t.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'Display count mismatch fails and concurrent duplicate is rejected',
    (t) async {
      final api = _Api(), guard = _Guard();
      final view = await open(t, api, guard);
      api.configureResult = display(status: 'pending');
      final first = view.onConfigureDisplays!(2);
      await expectLater(
        view.onConfigureDisplays!(2),
        throwsA(
          isA<BrokerException>().having((e) => e.code, 'code', 'display_busy'),
        ),
      );
      await t.pump(const Duration(milliseconds: 710));
      final failure = expectLater(
        first,
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'invalid_response',
          ),
        ),
      );
      api.nextState.complete(display(count: 1));
      await t.pump();
      await failure;
      expect(api.mutations, 1);
      await t.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'Closing while count polling rejects late completion and further mutation',
    (t) async {
      final api = _Api(), guard = _Guard();
      final view = await open(t, api, guard);
      api.configureResult = display(status: 'pending');
      final pending = view.onConfigureDisplays!(2);
      await t.pump(const Duration(milliseconds: 710));
      await close(t, guard);
      expect(guard.closes, 1);
      final failure = expectLater(
        pending,
        throwsA(
          isA<BrokerException>().having(
            (e) => e.code,
            'code',
            'session_expired',
          ),
        ),
      );
      api.nextState.complete(display());
      await t.pump();
      await failure;
      await expectLater(
        view.onConfigureDisplays!(1),
        throwsA(isA<BrokerException>()),
      );
      expect(api.mutations, 1);
      await t.pumpWidget(const SizedBox());
    },
  );

  // The window's first card has the stage list, so the connection card
  // that replaces it is no taller (the card does not jump).
  testWidgets(
    'The first card already shows the stage list at its first row',
    (t) async {
      final api = _Api()..pendingInventory = Completer<List<BrokerDesktop>>();
      final guard = _Guard();
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: launch(), api: api, guard: guard),
        ),
      );
      await t.pump();
      expect(find.byType(RemoteView), findsNothing);
      expect(find.text('데스크톱 연결 정보를 확인하고 있어요.'), findsOneWidget);
      expect(find.byKey(const Key('connection-wait-status')), findsOneWidget);
      expect(
        t.widget<Text>(find.byKey(const Key('connection-wait-step'))).data,
        '연결 요청',
      );
      for (final stage in ['연결 요청', '서버 연결', '가상 화면 준비', '첫 화면 수신 대기']) {
        expect(find.text(stage), findsOneWidget, reason: stage);
      }
      await close(t, guard);
      api.pendingInventory!.complete([desktop()]);
      await t.pump();
      await t.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'Rejected native close stays retired and ignores late inventory',
    (t) async {
      final api = _Api()..pendingInventory = Completer<List<BrokerDesktop>>();
      final guard = _Guard()..accepted = false;
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: launch(), api: api, guard: guard),
        ),
      );
      await close(t, guard);
      api.pendingInventory!.complete([desktop()]);
      await t.pump();
      expect(find.byType(RemoteView), findsNothing);
      expect(api.connects, 0);
      expect(find.text('다시 연결'), findsNothing);
      expect(find.text('연결을 끊었어요. ‘원격 창 닫기’를 다시 누르세요.'), findsOneWidget);
      await t.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'Refocusing an open stream window opens no session UI and no notice',
    (t) async {
      var opens = 0;
      await t.pumpWidget(
        MaterialApp(
          home: WorkspaceShell(
            onSignOut: () {},
            onRefresh: () {},
            machines: [machineFromBroker(desktop())],
            displayName: 'fixture',
            serverOrigin: 'https://fixture.example',
            onConnect: (_, {options = const StreamOptions()}) async =>
                throw StateError('No dashboard media'),
            onOpenWindow: (_, mode, options) async {
              opens++;
              return const SessionWindowOpenResult(opened: false);
            },
          ),
        ),
      );
      await t.tap(find.byKey(const Key('stream-modes-pve-149')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('stream-mode-windowed')));
      await acceptStreamConnect(t, 'pve-149');
      await t.pumpAndSettle();
      expect(opens, 1);
      expect(find.byType(SnackBar), findsNothing);
      expect(find.byType(RemoteView), findsNothing);
      expect(find.byType(AlertDialog), findsNothing);
      await t.pumpWidget(const SizedBox());
    },
  );
}

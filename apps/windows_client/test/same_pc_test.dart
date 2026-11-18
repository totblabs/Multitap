// same-pc-v1 (lib/data/same_pc.dart): the Windows app never connects to the
// desktop whose Host runs on the PC the app runs on. The broker lists a tag
// per desktop (server/test/same-pc.test.mjs pins the same vectors); the app
// derives it from the PC's own SMBIOS UUID. In-memory broker, HTTP and native
// channel boundaries only.
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/data/initial_viewport.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/same_pc.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/track_stream.dart';
import 'package:multitap_client/l10n/l10n.dart';
import 'package:multitap_client/models/console_preparation.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_window.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestLoginMemory, fixtureOrigin;
import 'helpers/stream_connect_consent.dart' show acceptStreamConnect;
import 'pc_login_replacement_lifetime_test.dart'
    show NativePolicyBroker, QuietWatch;
import 'power_api_test.dart' show ApiClient;
import 'session_recovery_test.dart' show mockPresentation;
import 'session_renewal_test.dart' show VirtualClock;
import 'session_window_lifecycle_test.dart' show launch;

// The vectors of server/test/same-pc.test.mjs.
const _uuid = '12345678-1234-4234-8234-000000000710';
const _tag710 =
    'ffd750749f61c8f55352ae92c1d64bcf32e10de9b13a8103ca148fcd7868ce9e';
const _pcUuid = 'a1234567-89ab-cdef-8123-0123456789ab';
const _pcId = 'pc-0f8fad5b-d9cb-469f-a165-70867728950e';
const _pcTag =
    '432f17dfe8914d7b9aea1a77dac64b677262ead32b58d7e2bf5e5717e6d19c03';
const _otherUuid = '99999999-1234-4234-8234-000000000001';

const _headline = '지금 쓰고 있는 PC예요';
const _detail = '다른 기기에서 연결하세요.';
const _message = '지금 Multitap 앱을 쓰고 있는 PC라 연결할 수 없어요. 다른 기기에서 연결하세요.';

Map<String, dynamic> _json(String id, {Object? tag, String kind = 'vm'}) => {
  'id': id,
  'name': 'Fixture $id',
  'kind': kind,
  'description': '',
  'os': 'Windows',
  'status': 'ready',
  'canConnect': true,
  if (kind == 'pc') 'localPcApproved': true,
  'hostMachineTag': ?tag,
  'powerState': 'running',
  'powerStale': false,
  'host': {
    'connected': true,
    'capabilities': {'service': true, 'streaming': true},
  },
};

BrokerDesktop _desktop(
  String id, {
  String? tag,
  bool ready = true,
  ConsolePreparationState preparation = const ConsolePreparationState(),
}) => BrokerDesktop(
  id: id,
  name: 'TESTER',
  kind: 'vm',
  description: '',
  os: 'Windows',
  status: 'ready',
  canConnect: true,
  canStream: ready,
  hostMachineTag: tag,
  hostConnected: true,
  interactiveSession: ready,
  virtualDisplayReady: ready,
  virtualMonitorControl: ready,
  display: VirtualDisplayState(
    supported: ready,
    activeCount: ready ? 1 : 0,
    checkedAt: DateTime.now(),
    stale: false,
  ),
  monitors: ready
      ? const [RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true)]
      : const [],
  power: const BrokerPowerSnapshot(state: 'running', stale: false),
  consolePreparation: preparation,
);

class _Shell {
  _Shell(this.machines, {this.detached = true});
  final List<DesktopMachine> machines;
  final bool detached;
  int opens = 0, connects = 0;
  Future<RemoteSession> connect(
    String id, {
    StreamOptions options = const StreamOptions(),
  }) async {
    connects++;
    throw const BrokerException('host_unavailable');
  }

  Future<SessionWindowOpenResult?> open(
    DesktopMachine machine,
    SessionDisplayMode mode,
    StreamOptions options,
  ) async {
    opens++;
    return null;
  }

  Future<void> mount(WidgetTester t) async {
    t.view.physicalSize = const Size(1440, 1000);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    mockPresentation();
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: WorkspaceShell(
          displayName: 'tester',
          guideAccountKey: 'account-a',
          serverOrigin: 'https://fixture.invalid',
          onSignOut: () {},
          onRefresh: () {},
          machines: machines,
          localMonitors: 1,
          onConnect: connect,
          onOpenWindow: detached ? open : null,
        ),
      ),
    );
    await t.pumpAndSettle();
  }
}

class _LauncherBroker extends NativePolicyBroker {
  _LauncherBroker(super.clock);
  List<BrokerDesktop> assigned = [];
  int mediaRequests = 0;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async => assigned;
  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
    InitialViewport? initialViewport,
    int? displayScale,
  }) async {
    mediaRequests++;
    throw StateError('The launcher must open the native child, not own media');
  }
}

class _WindowApi extends Fake
    implements BrokerApi, StreamTakeoverPreflightBrokerApi, TrackBrokerApi {
  _WindowApi(this.current);
  BrokerDesktop current;
  int reads = 0, preflights = 0, connects = 0, mutations = 0;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    reads++;
    return [current];
  }

  @override
  Future<void> checkStreamTakeover(String token, String desktopId) async {
    preflights++;
  }

  @override
  Future<BrokerConnection> connect(
    String token,
    String id, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    connects++;
    throw const BrokerException('host_unavailable');
  }

  @override
  Future<BrokerConnection> connectTracks(
    String token,
    String id,
    TrackPlan tracks,
    StreamOptions options, {
    int? displayScale,
  }) async {
    connects++;
    throw const BrokerException('host_unavailable');
  }

  @override
  Future<VirtualDisplayState> configureDisplays(
    String token,
    String id,
    int count,
  ) async {
    mutations++;
    throw const BrokerException('display_busy');
  }

  @override
  void close() {}
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  setUp(() => SamePc.debugSetMachineUuid(null));
  tearDown(() => SamePc.debugSetMachineUuid(null));

  group('tag', () {
    test('is the value the broker lists for the same desktop and UUID', () {
      expect(SamePc.tag('pve-710', _uuid), _tag710);
      expect(SamePc.tag(_pcId, _pcUuid), _pcTag);
      expect(SamePc.tag('pve-711', _uuid), isNot(_tag710));
    });

    test('a desktop matches only with this PC\'s UUID and its own tag', () {
      expect(SamePc.known, isFalse);
      expect(
        SamePc.matches('pve-710', _tag710),
        isFalse,
        reason: 'An unknown UUID never blocks.',
      );
      SamePc.debugSetMachineUuid(_uuid);
      expect(SamePc.known, isTrue);
      expect(SamePc.matches('pve-710', _tag710), isTrue);
      expect(SamePc.matches('pve-710', null), isFalse);
      expect(SamePc.matches('pve-711', _tag710), isFalse);
      expect(SamePc.matches(_pcId, _pcTag), isFalse);
      SamePc.debugSetMachineUuid(_pcUuid);
      expect(SamePc.matches('pve-710', _tag710), isFalse);
      expect(SamePc.matches(_pcId, _pcTag), isTrue);
    });

    test('only a set, well-formed UUID and a 64-digit tag are accepted', () {
      expect(SamePc.parseUuid(_uuid), _uuid);
      for (final value in <Object?>[
        null,
        5,
        '',
        _pcUuid.toUpperCase(),
        '{$_uuid}',
        _uuid.replaceAll('-', ''),
        '$_uuid ',
        '00000000-0000-0000-0000-000000000000',
        'ffffffff-ffff-ffff-ffff-ffffffffffff',
      ]) {
        expect(SamePc.parseUuid(value), isNull, reason: '$value');
      }
      expect(
        SamePc.parseUuid('00000000-0000-0000-0000-000000000001'),
        isNotNull,
      );
      expect(SamePc.parseTag(_tag710), _tag710);
      for (final value in <Object?>[
        null,
        5,
        '',
        _tag710.toUpperCase(),
        _tag710.substring(1),
        '${_tag710}0',
        _uuid,
      ]) {
        expect(SamePc.parseTag(value), isNull, reason: '$value');
      }
    });

    test('the runner\'s answer is read once; no answer is unknown', () async {
      const channel = MethodChannel('multitap/window');
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final calls = <String>[];
      Object? answer = _uuid;
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call.method);
        final value = answer;
        if (value is Exception) throw value;
        return value;
      });
      await SamePc.load();
      expect(calls, ['machineUuid']);
      expect(SamePc.matches('pve-710', _tag710), isTrue);
      for (final value in <Object?>[
        null,
        '',
        'unknown',
        _pcUuid.toUpperCase(),
        '00000000-0000-0000-0000-000000000000',
        PlatformException(code: 'unavailable'),
      ]) {
        answer = value;
        await SamePc.load();
        expect(SamePc.known, isFalse, reason: '$value');
        expect(SamePc.matches('pve-710', _tag710), isFalse, reason: '$value');
      }
      // A runner without the method (an older build): unknown as well.
      SamePc.debugSetMachineUuid(_uuid);
      messenger.setMockMethodCallHandler(channel, null);
      await SamePc.load();
      expect(SamePc.known, isFalse);
    });
  });

  group('desktop list', () {
    test('the listed tag marks the desktop of this PC and no other', () {
      SamePc.debugSetMachineUuid(_uuid);
      final same = BrokerDesktop.fromJson(_json('pve-710', tag: _tag710));
      final other = BrokerDesktop.fromJson(
        _json('pve-711', tag: SamePc.tag('pve-711', _otherUuid)),
      );
      final untagged = BrokerDesktop.fromJson(_json('pve-712'));
      final malformed = BrokerDesktop.fromJson(_json('pve-713', tag: 7));
      expect(same.hostMachineTag, _tag710);
      expect(same.samePc, isTrue);
      expect(other.samePc, isFalse);
      expect((untagged.hostMachineTag, untagged.samePc), (null, false));
      expect((malformed.hostMachineTag, malformed.samePc), (null, false));

      final machine = machineFromBroker(same);
      expect(machine.samePc, isTrue);
      expect(machine.status, MachineStatus.ready);
      expect(machine.isReady, isFalse);
      expect(machine.streamReadiness, _message);
      expect(machineFromBroker(other).samePc, isFalse);
      expect(machineFromBroker(other).isReady, isTrue);
      // Copies made for power, display, resources and organising keep it.
      for (final copy in [
        machine.withDisplay(machine.display),
        machine.withResources(machine.resources),
        machine.withOrganize(machine.organize),
        machine.withHostUpdate(machine.hostUpdate),
        machine.withPower(
          state: 'running',
          checkedAt: null,
          stale: false,
          busy: false,
        ),
      ]) {
        expect(copy.samePc, isTrue);
      }
    });

    test('an approved PC is matched the same way', () {
      SamePc.debugSetMachineUuid(_pcUuid);
      final pc = BrokerDesktop.fromJson(_json(_pcId, tag: _pcTag, kind: 'pc'));
      expect(pc.localPcApproved, isTrue);
      expect(machineFromBroker(pc).samePc, isTrue);
      expect(machineFromBroker(pc).isReady, isFalse);
    });

    test('without this PC\'s UUID nothing is blocked', () {
      final listed = BrokerDesktop.fromJson(_json('pve-710', tag: _tag710));
      expect(listed.samePc, isFalse);
      final machine = machineFromBroker(listed);
      expect((machine.samePc, machine.isReady), (false, true));
      expect(DesktopReadiness.of(machine).canStream, isTrue);
    });

    test('the card state says so and offers no connection or fix', () {
      SamePc.debugSetMachineUuid(_uuid);
      final machine = machineFromBroker(_desktop('pve-710', tag: _tag710));
      final readiness = DesktopReadiness.of(machine);
      expect(readiness.samePc, isTrue);
      expect(readiness.phase, TilePhase.attention);
      expect(readiness.canStream, isFalse);
      expect(readiness.primary, isNull);
      expect((readiness.headline, readiness.detail), (_headline, _detail));
      expect(readiness.checks.last.state, CheckState.warn);
      expect(readiness.checks.last.detail, _headline);

      final other = DesktopReadiness.of(
        machineFromBroker(_desktop('pve-711', tag: _tag710)),
      );
      expect((other.samePc, other.canStream), (false, true));
    });

    test('a Windows screen held by RDP is not offered for takeover either', () {
      const preparation = ConsolePreparationState(
        supported: true,
        available: true,
        reason: 'rdp_session_disconnected',
      );
      final held = _desktop('pve-710', ready: false, preparation: preparation);
      final offered = machineFromBroker(held);
      expect(offered.canRecoverConsole, isTrue);
      expect(
        DesktopReadiness.of(offered, consoleRecovery: true).phase,
        TilePhase.ready,
      );
      SamePc.debugSetMachineUuid(_uuid);
      final same = machineFromBroker(
        _desktop(
          'pve-710',
          tag: _tag710,
          ready: false,
          preparation: preparation,
        ),
      );
      expect(same.canRecoverConsole, isFalse);
      final readiness = DesktopReadiness.of(same, consoleRecovery: true);
      expect((readiness.samePc, readiness.console), (true, false));
      expect(readiness.canStream, isFalse);
    });

    test('the wording in English', () {
      AppLanguageController.debugSystemLanguage = 'en';
      AppLanguageController.instance.debugReset();
      expect(l10n.samePcHeadline, "This desktop is the PC you're using");
      expect(
        l10n.samePcDetail,
        "Connect from another device.",
      );
      expect(
        const BrokerException('same_pc').message,
        "This desktop is the PC you're using Multitap on, so you can't "
        'connect to it. Connect from another device.',
      );
    });

    test('the Korean wording never says 이 PC, 내 PC or 이 컴퓨터', () {
      for (final text in [
        l10n.samePcHeadline,
        l10n.samePcDetail,
        l10n.samePcMessage,
      ]) {
        expect(text, contains(RegExp('PC|기기')));
        for (final word in ['이 PC', '내 PC', '이 컴퓨터', '내 컴퓨터']) {
          expect(text, isNot(contains(word)));
        }
      }
      expect(
        (l10n.samePcHeadline, l10n.samePcDetail, l10n.samePcMessage),
        (_headline, _detail, _message),
      );
      expect(const BrokerException('same_pc').message, _message);
    });
  });

  group('broker requests', () {
    Map<String, dynamic> connected() => {
      'sessionId': 'same-pc-session',
      'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/same-pc-session',
      'mode': 'stream',
      'options': const StreamOptions().toJson(),
    };

    test('no connection request is sent for the desktop of this PC', () async {
      SamePc.debugSetMachineUuid(_uuid);
      final client = ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [
              _json('pve-710', tag: _tag710),
              _json('pve-711', tag: SamePc.tag('pve-711', _otherUuid)),
            ],
          },
        ));
      final api = HttpBrokerApi(
        origin: Uri.parse('https://fixture.invalid'),
        client: client,
        requestStreamTakeoverContext: true,
      );
      addTearDown(api.close);
      final listed = await api.desktops('synthetic-token');
      expect(listed.map((d) => d.samePc), [true, false]);
      final refused = throwsA(
        isA<BrokerException>()
            .having((e) => e.code, 'code', 'same_pc')
            .having((e) => e.message, 'message', _message),
      );
      final plan = TrackPlan(const [
        TrackViewport('0', 1920, 1080, 0, 0),
        TrackViewport('1', 1920, 1080, 1920, 0),
      ]);
      await expectLater(api.connect('synthetic-token', 'pve-710'), refused);
      await expectLater(
        api.connectScaled(
          'synthetic-token',
          'pve-710',
          options: const StreamOptions(),
          displayScale: 100,
        ),
        refused,
      );
      await expectLater(
        api.connectTracks(
          'synthetic-token',
          'pve-710',
          plan,
          const StreamOptions(),
        ),
        refused,
      );
      await expectLater(
        api.connectProfileTracks(
          'synthetic-token',
          'pve-710',
          plan,
          const StreamOptions(),
        ),
        refused,
      );
      await expectLater(
        api.checkStreamTakeover('synthetic-token', 'pve-710'),
        refused,
      );
      expect(
        client.requests.map((request) => request.uri.path),
        ['/api/v1/desktops'],
        reason: 'Only the list was read.',
      );

      // Another desktop of the same list connects as before.
      client.responses.add((200, connected()));
      final connection = await api.connect('synthetic-token', 'pve-711');
      expect(connection.sessionId, 'same-pc-session');
      expect(client.requests.last.uri.path, '/api/v1/desktops/pve-711/connect');
      expect(client.requests.last.method, 'POST');
    });

    test('without this PC\'s UUID the same list connects', () async {
      final client = ApiClient()
        ..responses.addAll([
          (
            200,
            {
              'desktops': [_json('pve-710', tag: _tag710)],
            },
          ),
          (200, connected()),
        ]);
      final api = HttpBrokerApi(
        origin: Uri.parse('https://fixture.invalid'),
        client: client,
      );
      addTearDown(api.close);
      await api.desktops('synthetic-token');
      await api.connect('synthetic-token', 'pve-710');
      expect(client.requests.last.uri.path, '/api/v1/desktops/pve-710/connect');
      expect(
        jsonDecode(client.requests.last.body.toString()),
        containsPair('mode', 'stream'),
      );
    });
  });

  group('launcher', () {
    testWidgets('the card says why and its button opens nothing', (t) async {
      SamePc.debugSetMachineUuid(_uuid);
      for (final detached in [true, false]) {
        final shell = _Shell([
          machineFromBroker(_desktop('pve-710', tag: _tag710)),
          machineFromBroker(_desktop('pve-711')),
        ], detached: detached);
        await shell.mount(t);
        expect(find.text(_headline), findsWidgets);
        expect(find.byKey(const Key('open-pve-710')), findsNothing);
        expect(find.byKey(const Key('stream-modes-pve-710')), findsNothing);
        expect(find.byKey(const Key('open-pve-711')), findsOneWidget);
        final button = find.byKey(const Key('same-pc-pve-710'));
        expect(button, findsOneWidget);
        await t.tap(button);
        await t.pumpAndSettle();
        expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
        final notice = find.byKey(const Key('same-pc-notice'));
        expect(notice, findsOneWidget);
        expect(
          find.descendant(of: notice, matching: find.text(_headline)),
          findsOneWidget,
        );
        expect(
          find.descendant(of: notice, matching: find.text(_detail)),
          findsOneWidget,
        );
        await t.tap(find.byKey(const Key('same-pc-notice-close')));
        await t.pumpAndSettle();
        expect(notice, findsNothing);
        expect((shell.opens, shell.connects), (0, 0));

        // The other desktop of the same list connects as before.
        await t.tap(find.byKey(const Key('open-pve-711')));
        await acceptStreamConnect(t, 'pve-711');
        await t.pump(const Duration(milliseconds: 350));
        expect((shell.opens, shell.connects), detached ? (1, 0) : (0, 1));
        await t.pumpWidget(const SizedBox.shrink());
        await t.pumpAndSettle();
      }
    });

    testWidgets('signed in, the desktop of this PC opens no remote window', (
      t,
    ) async {
      debugDefaultTargetPlatformOverride = TargetPlatform.windows;
      var opens = 0;
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        if (call.method == 'open') {
          opens++;
          return {'opened': true};
        }
        return null;
      });
      addTearDown(
        () => messenger.setMockMethodCallHandler(SessionWindows.channel, null),
      );
      t.view.physicalSize = const Size(1440, 900);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      Future<_LauncherBroker> signIn() async {
        final clock = VirtualClock();
        final api = _LauncherBroker(clock)
          ..assigned = [_desktop('qa-vm', tag: SamePc.tag('qa-vm', _uuid))];
        final memory = SessionLauncherMemory();
        await memory.save(
          fixtureOrigin.toString(),
          const LauncherPreferences(showPreviews: false),
        );
        await t.pumpWidget(
          MultitapApp(
            key: UniqueKey(),
            api: api,
            serverOrigin: fixtureOrigin,
            clock: clock.call,
            loginMemory: TestLoginMemory(),
            launcherMemory: memory,
            desktopWatchFactory: (_, _) => QuietWatch(),
          ),
        );
        await t.pumpAndSettle();
        await t.enterText(find.byKey(const Key('sign-in-id')), 'fixture-user');
        await t.enterText(
          find.byKey(const Key('sign-in-password')),
          'fixture-password',
        );
        await t.tap(find.byKey(const Key('login-submit')));
        await t.pumpAndSettle();
        return api;
      }

      // The PC of this desktop: the message, and nothing opens.
      SamePc.debugSetMachineUuid(_uuid);
      var api = await signIn();
      expect(find.text(_headline), findsWidgets);
      expect(find.byKey(const Key('open-qa-vm')), findsNothing);
      await t.tap(find.byKey(const Key('same-pc-qa-vm')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('same-pc-notice')), findsOneWidget);
      await t.tap(find.byKey(const Key('same-pc-notice-close')));
      await t.pumpAndSettle();
      expect((opens, api.mediaRequests), (0, 0));
      await t.pumpWidget(const SizedBox.shrink());
      await t.pumpAndSettle();

      // Another PC, same account and list: the connection opens as before.
      SamePc.debugSetMachineUuid(_otherUuid);
      api = await signIn();
      expect(find.text(_headline), findsNothing);
      await t.ensureVisible(find.byKey(const Key('open-qa-vm')));
      await t.tap(find.byKey(const Key('open-qa-vm')));
      await t.pump();
      await acceptStreamConnect(t, 'qa-vm');
      for (var n = 0; n < 10 && opens == 0; n++) {
        await t.pump(const Duration(milliseconds: 20));
      }
      expect((opens, api.mediaRequests), (1, 0));
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
      await t.pumpAndSettle();
      debugDefaultTargetPlatformOverride = null;
    });
  });

  group('remote window', () {
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

    testWidgets('a window for the desktop of this PC shows why and requests '
        'nothing', (t) async {
      SamePc.debugSetMachineUuid(_uuid);
      final api = _WindowApi(_desktop('pve-710', tag: _tag710));
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: launch(id: 'pve-710'), api: api),
        ),
      );
      await t.pump();
      await t.pump();
      await t.pump(const Duration(milliseconds: 20));
      expect(find.byType(RemoteView), findsNothing);
      expect(find.text(_message), findsOneWidget);
      expect(api.reads, 1);
      expect((api.preflights, api.connects, api.mutations), (0, 0, 0));
      await t.pumpWidget(const SizedBox());
      await t.pump();
    });

    testWidgets('the same window on another PC goes on to connect', (t) async {
      SamePc.debugSetMachineUuid(_otherUuid);
      final api = _WindowApi(_desktop('pve-710', tag: _tag710));
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: launch(id: 'pve-710'), api: api),
        ),
      );
      await t.pump();
      await t.pump();
      await t.pump(const Duration(milliseconds: 20));
      expect(find.byType(RemoteView), findsOneWidget);
      expect(find.text(_message), findsNothing);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    });
  });
}

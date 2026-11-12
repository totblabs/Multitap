import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_settings_api.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_overview_panels.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

// The status reads of the Host tool (settings_rpc.hpp): nothing is changed.
bool isReadOnlyHostRequest(Map<String, dynamic> request) =>
    request.length == 1 &&
    const {
      'status',
      'encoderSettings',
      // host-prerequisites-v1: what a first install still needs.
      'prerequisiteStatus',
    }.contains(request['action']);

class FakeCloseGuard implements WindowCloseGuard {
  Future<void> Function()? onRequest;
  bool available = true, busy = false, dirty = false;
  int closes = 0;
  @override
  void attach(Future<void> Function() callback) {
    onRequest = callback;
  }

  @override
  Future<bool> update({required bool busy, required bool dirty}) async {
    this.busy = busy;
    this.dirty = dirty;
    return available;
  }

  @override
  Future<bool> close() async {
    if (!available || busy) return false;
    closes++;
    return true;
  }

  @override
  void dispose() {
    onRequest = null;
  }
}

class FakeHostSettings implements HostSettingsApi {
  FakeHostSettings({this.registered = false, this.administrator = true});
  bool registered, administrator;
  bool rejectTest = false,
      recoveryRequired = false,
      rebootRequired = false,
      running = true,
      failStatus = false,
      cancelPending = false,
      credentialsConfigured = false,
      rejectCredentials = false;
  String credentialUsername = '';
  String credentialRevision = 'a' * 64;
  int displays = 0;
  Completer<void>? pending;
  Completer<Map<String, dynamic>>? nextStatus;
  Completer<void>? credentialPending;
  Completer<void>? repairPending;
  Map<String, dynamic> repairPlan = {
    'planId': List.filled(64, 'a').join(),
    'canRepair': true,
    'repairNeeded': true,
    'reason': '',
    'driverStoreRetained': true,
    'configurationFilesRetained': true,
    'deviceCount': 0,
    'configurationCount': 1,
    'items': [
      {
        'id': 'fixture-config',
        'kind': 'configuration',
        'name': '이전 가상 화면 설정',
        'required': true,
        'registryPath': r'HKLM\SOFTWARE\Multitap\Display',
        'registryValue': 'SettingsPath',
        'impact': '남아 있는 설정 연결만 정리합니다. 기존 설정 파일은 유지합니다.',
      },
    ],
  };
  Map<String, dynamic> repairResult = {
    'repairInstalled': true,
    'cleanupCompleted': true,
    'removedDeviceCount': 0,
    'configurationRestored': false,
    'partialRemoval': false,
    'rebootRequired': true,
    'configurationPending': true,
    'reason': '',
  };
  int repairExitCode = 0;
  bool repairTimedOut = false;
  Map<String, dynamic> resetResult = {
    'ok': true,
    'moved': 4,
    'steps': [
      {'step': 'sessions', 'ok': true, 'code': 'idle'},
      {'step': 'sdk', 'ok': true, 'code': 'absent'},
      {'step': 'console', 'ok': true, 'code': 'console_active'},
      {'step': 'archive', 'ok': true, 'code': 'moved'},
      {'step': 'reinitialize', 'ok': true, 'code': 'reinitialized'},
    ],
  };
  bool consoleCompleted = true, rejectConsole = false;
  Completer<void>? consolePending;
  Map<String, dynamic> connectionRequest = {};
  int connectionRequests = 0;
  final requests = <Map<String, dynamic>>[];
  Map<String, dynamic> statusOverrides = {};
  // devices-v1 as a ready VM reports it (apps/windows_host/src/device_status.hpp).
  Map<String, dynamic> devices = {
    'schema': 1,
    'drives': {'winfsp': true, 'webClient': 'usable'},
    'usb': {'supported': true, 'package': true, 'controller': 'started'},
    'printers': {'reason': ''},
    'audio': {'output': 'present'},
  };
  // installWinfsp / startPrintSpooler replies (settings_rpc.hpp); a success
  // changes the observed devices the way the real Host would.
  Map<String, dynamic> winfspResult = {
    'action': 'installed',
    'restart': false,
    'error': '',
  };
  Map<String, dynamic> spoolerResult = {'state': 'running', 'error': ''};
  Completer<void>? devicePending;
  // display-adapters-v1 as a real Host reports it: one unproven, one recorded leftover.
  Map<String, dynamic> displayAdapters = {
    'read': true,
    'current': 1,
    'legacy': 0,
    'duplicate': 1,
    'unproven': 1,
    'foreign': 2,
    'currentStarted': true,
    'blocker': '',
    'cleanup': 'none',
    'extras': [
      {
        'id': r'ROOT\DISPLAY\0001',
        'kind': 'unproven',
        'present': true,
        'started': true,
      },
      {
        'id': r'ROOT\DISPLAY\0002',
        'kind': 'duplicate',
        'present': true,
        'started': false,
      },
    ],
  };
  @override
  Future<Map<String, dynamic>> request(
    String action, [
    Map<String, dynamic> fields = const {},
  ]) async {
    requests.add({'action': action, ...fields});
    if (action == 'consoleTransfer') {
      await consolePending?.future;
      if (rejectConsole) {
        throw const HostSettingsException('console_transfer_session_changed');
      }
      return {
        'ok': true,
        'transfer': {'requested': true, 'completed': consoleCompleted},
      };
    }
    if (action == 'cleanDisplayAdapters') {
      final removed = (displayAdapters['extras'] as List).length;
      displayAdapters = {
        ...displayAdapters,
        'legacy': 0,
        'duplicate': 0,
        'unproven': 0,
        'extras': [],
      };
      return {
        'ok': true,
        'adapters': {
          'error': '',
          'removed': removed,
          'pending': 0,
          'failed': 0,
          'refused': 0,
        },
      };
    }
    if (action == 'replaceUsbController') {
      devices = {
        ...devices,
        'usb': {...devices['usb'] as Map, 'controller': 'started'},
      };
      return {
        'ok': true,
        'usb': {'error': '', 'restart': false, 'removed': 1},
        'capabilitiesRefreshRequested': true,
      };
    }
    if (action == 'installWinfsp' || action == 'startPrintSpooler') {
      await devicePending?.future;
      final winfsp = action == 'installWinfsp';
      final result = winfsp ? winfspResult : spoolerResult;
      if (result['error'] == '') {
        devices = {
          ...devices,
          if (winfsp)
            'drives': {...devices['drives'] as Map, 'winfsp': true}
          else
            'printers': {'reason': '', 'spooler': 'running'},
        };
      }
      return {
        'ok': true,
        winfsp ? 'winfsp' : 'spooler': result,
        'capabilitiesRefreshRequested': result['error'] == '',
      };
    }
    if (action == 'displayRepairPlan') return {'ok': true, 'plan': repairPlan};
    if (action == 'resetNativeRecovery') {
      return {'ok': true, 'reset': resetResult};
    }
    if (action == 'repairDisplay') {
      await repairPending?.future;
      rebootRequired = repairResult['rebootRequired'] == true;
      return {
        'ok': true,
        'result': repairResult,
        'helperExitCode': repairExitCode,
        'timedOut': repairTimedOut,
      };
    }
    if (action == 'status' && nextStatus != null) {
      final delayed = nextStatus!;
      nextStatus = null;
      return delayed.future;
    }
    if (action == 'testBroker' && rejectTest) {
      throw const HostSettingsException('서버 연결 실패');
    }
    if (action == 'status' && failStatus) {
      throw const HostSettingsException('상태 조회 실패');
    }
    if (action == 'service') {
      await pending?.future;
      running = fields['command'] == 'start';
    }
    if (action == 'restartServiceV1') {
      await pending?.future;
      running = true;
    }
    if (action == 'requestConnection') {
      connectionRequests++;
      connectionRequest = {
        'status': 'pending',
        'origin': fields['origin'],
        'requestId': 'fixture-$connectionRequests',
        'expiresAt': DateTime.now()
            .add(const Duration(minutes: 10))
            .toIso8601String(),
      };
    }
    if (action == 'cancelConnectionV1') {
      connectionRequest = {
        ...connectionRequest,
        'status': 'cancelled',
        'cancelAcknowledged': !cancelPending,
        'error': cancelPending ? 'cancel_pending' : '',
      };
    }
    if (action == 'saveRdpCredentials') {
      await credentialPending?.future;
      if (rejectCredentials) {
        throw const HostSettingsException('Windows 계정을 확인하지 못했습니다.');
      }
      credentialsConfigured = true;
      credentialUsername = fields['username'] as String;
    }
    if (action == 'clearRdpCredentials') {
      throw const HostSettingsException(
        'Host 업데이트가 필요해요.',
        code: 'windows_login_clear_update_required',
      );
    }
    if (action == 'clearWindowsLoginV1') {
      if (fields['protocol'] != SavedWindowsLoginStatus.protocol ||
          fields['revision'] != credentialRevision) {
        throw const HostSettingsException(
          '저장 정보가 바뀌었어요.',
          code: 'windows_login_record_changed',
        );
      }
      credentialsConfigured = false;
      credentialUsername = '';
      return {
        'ok': true,
        'clear': {
          'protocol': SavedWindowsLoginStatus.protocol,
          'outcome': 'removed',
        },
      };
    }
    if (action == 'recoverDisplay') {
      recoveryRequired = false;
      displays = 1;
    }
    return {
      'ok': true,
      'status': {
        'version': '0.2.6',
        'administrator': administrator,
        'registered': registered,
        'desktopId': registered ? 'pve-totbvm-149' : '',
        'serverOrigin': registered ? 'https://cloud.example.com' : '',
        'connectionRequest': {
          ...connectionRequest,
          if (connectionRequest.isNotEmpty)
            'cancelProtocol': 'cancel-request-v1',
          if (connectionRequest.isNotEmpty)
            'cancelRevision': connectionRequest['cancelRevision'] ?? 'a' * 64,
        },
        'service': {
          'installed': true,
          'running': running,
          'restartProtocol': 'service-restart-v1',
        },
        'interactiveSession': true,
        'display': {
          'installed': true,
          'deviceReady': true,
          'owned': true,
          'activeDisplay': displays > 0,
          'ready': displays > 0 && !recoveryRequired,
          'recoveryRequired': recoveryRequired,
          'rebootRequired': rebootRequired,
          'foreignDeviceCount': 0,
          'remoteSession': true,
          'monitors': [
            for (var i = 0; i < displays; i++)
              {'id': '$i', 'width': 1920, 'height': 1080},
          ],
        },
        'windowsAccount': {
          'configured': credentialsConfigured,
          'username': credentialUsername,
          'error': '',
          'clearProtocol': SavedWindowsLoginStatus.protocol,
          if (credentialsConfigured) 'clearRevision': credentialRevision,
        },
        'devices': devices,
        'displayAdapters': displayAdapters,
        ...statusOverrides,
      },
    };
  }
}

void main() {
  Future<void> open(
    WidgetTester t,
    FakeHostSettings api, {
    Size size = const Size(1440, 1000),
    FakeCloseGuard? closeGuard,
    double textScale = 1,
    Brightness brightness = Brightness.light,
  }) async {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = size;
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    final app = MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildAppTheme(brightness: brightness),
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(context)
            .copyWith(textScaler: TextScaler.linear(textScale)),
        child: child!,
      ),
      home: HostSettingsScreen(
        api: api,
        closeGuard: closeGuard ?? FakeCloseGuard(),
      ),
    );
    await t.pumpWidget(app);
    await t.pumpAndSettle();
  }

  Future<void> nav(WidgetTester t, int page) async {
    final f = find.byKey(Key('host-nav-$page'));
    await t.ensureVisible(f);
    await t.tap(f);
    await t.pumpAndSettle();
  }

  Future<void> tap(WidgetTester t, String key) async {
    await t.pump();
    final f = find.byKey(Key(key));
    // To the middle of the view: at its top edge a control sits under the
    // page's pinned header and the tap would hit the header instead.
    await Scrollable.ensureVisible(t.element(f), alignment: .5);
    await t.pumpAndSettle();
    await t.tap(f);
    await t.pumpAndSettle();
  }

  // The overview's ONE status beside the desktop's name.
  String pill(WidgetTester t) => t
      .widget<HostReadiness>(find.byKey(const Key('host-overview-state')))
      .label;

  // The overview's server line. With a fresh acknowledgement it reads
  // "<server> · 방금 확인" (or "· N초 전 확인"); otherwise the connection state.
  String serverLine(WidgetTester t) =>
      t.widget<Text>(find.byKey(const Key('host-broker-state'))).data!;
  final confirmed = matches(r'^cloud\.example\.com · (방금|[0-9]+초 전) 확인$');

  Map<String, dynamic> nativeState({
    bool restart = false,
    String reason = '',
    bool eligible = true,
  }) => {
    'version': '0.2.21',
    'broker': {'state': 'online', 'online': true, 'heartbeatAgeSeconds': 0},

    'display': {
      'provider': 'native',
      'installed': true,
      'deviceReady': true,
      'owned': true,
      'ready': false,
      'activeDisplay': false,
      'remoteSession': true,
      'configurationPending': true,
      'recoveryRequired': false,
      'rebootRequired': restart,
      'error': 'interactive_console_required',
      'monitors': [],
    },
    'consoleTransfer': {
      'supported': true,
      'eligible': eligible && !restart,
      'reason': restart ? 'restart_required' : reason,
      'sourceSessionId': 2,
      'consoleSessionId': 1,
    },
  };

  testWidgets(
    'Native display offers a confirmed own-login transfer and never invokes the legacy install or cleanup path',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..statusOverrides = nativeState();
      await open(t, api);
      await nav(t, 2);
      expect(find.byKey(const Key('host-display-setup')), findsNothing);
      expect(find.byKey(const Key('host-display-repair-scan')), findsNothing);
      expect(
        find.byKey(const Key('host-native-display-refresh')),
        findsOneWidget,
      );
      expect(
        api.requests.where((r) => r['action'] == 'consoleTransfer'),
        isEmpty,
      );
      await tap(t, 'host-console-transfer');
      expect(find.text('스트리밍 화면으로 전환할까요?'), findsOneWidget);
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(
        api.requests.where((r) => r['action'] == 'consoleTransfer'),
        isEmpty,
      );
      await tap(t, 'host-console-transfer');
      api.consolePending = Completer<void>();
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pump();
      await t.pump();
      final calls = api.requests
          .where((r) => r['action'] == 'consoleTransfer')
          .toList();
      expect(calls, [
        {
          'action': 'consoleTransfer',
          'sourceSessionId': 2,
          'consoleSessionId': 1,
        },
      ]);
      expect(
        t
            .widget<FilledButton>(
              find.byKey(const Key('host-console-transfer')),
            )
            .onPressed,
        isNull,
      );
      api.consolePending!.complete();
      await t.pumpAndSettle();
      expect(find.textContaining('Windows 로그인 전환을 확인했어요.'), findsWidgets);
      expect(
        api.requests.any(
          (r) => [
            'display',
            'recoverDisplay',
            'displayRepairPlan',
            'repairDisplay',
          ].contains(r['action']),
        ),
        isFalse,
      );
    },
  );

  testWidgets(
    'Native recovery-record reset requires confirmation, reports each service step and refuses from RDP',
    (t) async {
      Map<String, dynamic> blocked({bool remote = false}) {
        final state = nativeState(eligible: false);
        state['display'] = {
          ...(state['display'] as Map<String, dynamic>),
          'recoveryRequired': true,
          'remoteSession': remote,
          'error': 'native_topology_recovery_required',
        };
        return state;
      }

      final api = FakeHostSettings(registered: true)
        ..statusOverrides = blocked();
      await open(t, api);
      await nav(t, 2);
      await tap(t, 'host-native-recovery-reset');
      expect(find.text('가상 화면 복구 기록을 초기화할까요?'), findsOneWidget);
      expect(find.textContaining('기록은 삭제하지 않고 백업해요'), findsOneWidget);
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(
        api.requests.where((r) => r['action'] == 'resetNativeRecovery'),
        isEmpty,
      );
      await tap(t, 'host-native-recovery-reset');
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pumpAndSettle();
      expect(
        api.requests
            .where((r) => r['action'] == 'resetNativeRecovery')
            .toList(),
        [
          {'action': 'resetNativeRecovery', 'confirmed': true},
        ],
      );
      expect(find.textContaining('4개를 백업'), findsWidgets);
      expect(find.textContaining('✓ 이전 가상 모니터: 남아 있지 않음'), findsWidgets);
      await t.tap(find.byKey(const Key('host-result-close')));
      await t.pumpAndSettle();

      api.resetResult = {
        'ok': false,
        'code': 'stream_active',
        'steps': [
          {'step': 'sessions', 'ok': false, 'code': 'stream_active'},
        ],
      };
      await tap(t, 'host-native-recovery-reset');
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pumpAndSettle();
      expect(find.textContaining('초기화하지 않았어요'), findsWidgets);
      expect(find.textContaining('✗ 사용 중인 연결: 스트리밍 연결 중'), findsWidgets);
      await t.tap(find.byKey(const Key('host-result-close')));
      await t.pumpAndSettle();
      await t.pumpWidget(const SizedBox.shrink());

      final remote = FakeHostSettings(registered: true)
        ..statusOverrides = blocked(remote: true);
      await open(t, remote);
      await nav(t, 2);
      expect(
        t
            .widget<OutlinedButton>(
              find.byKey(const Key('host-native-recovery-reset')),
            )
            .onPressed,
        isNull,
      );
      await t.pumpWidget(const SizedBox.shrink());

      final healthy = FakeHostSettings(registered: true)
        ..statusOverrides = nativeState();
      await open(t, healthy);
      await nav(t, 2);
      expect(find.byKey(const Key('host-native-recovery-reset')), findsNothing);
    },
  );

  testWidgets(
    'Native restart and occupied-console states keep the transfer disabled with a specific next action',
    (t) async {
      for (final restart in [true, false]) {
        final api = FakeHostSettings(registered: true)
          ..statusOverrides = nativeState(
            restart: restart,
            eligible: false,
            reason: 'console_in_use',
          );
        await open(t, api);
        await nav(t, 2);
        expect(
          t
              .widget<FilledButton>(
                find.byKey(const Key('host-console-transfer')),
              )
              .onPressed,
          isNull,
        );
        expect(
          find.textContaining(restart ? 'Windows를 다시 시작' : '콘솔에 다른 사용자'),
          findsWidgets,
        );
        await tap(t, 'host-native-display-refresh');
        expect(
          api.requests.any(
            (r) => [
              'consoleTransfer',
              'display',
              'displayRepairPlan',
            ].contains(r['action']),
          ),
          isFalse,
        );
        await t.pumpWidget(const SizedBox.shrink());
      }
    },
  );

  testWidgets(
    'An accepted but unverified console transfer remains uncertain and is not automatically retried',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..statusOverrides = nativeState()
        ..consoleCompleted = false;
      await open(t, api);
      await nav(t, 2);
      await tap(t, 'host-console-transfer');
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pumpAndSettle();
      expect(find.textContaining('전환 요청은 받았지만 완료를 확인하지 못했어요.'), findsWidgets);
      await t.pump(const Duration(seconds: 5));
      await t.pumpAndSettle();
      expect(
        api.requests.where((r) => r['action'] == 'consoleTransfer').length,
        1,
      );
    },
  );

  testWidgets(
    'A settings window notices service enrollment completed outside its own request form',
    (t) async {
      final api = FakeHostSettings();
      await open(t, api);
      final before = api.requests.where((r) => r['action'] == 'status').length;
      api.registered = true;
      await t.pump(const Duration(seconds: 3));
      await t.pumpAndSettle();
      expect(
        api.requests.where((r) => r['action'] == 'status').length,
        greaterThan(before),
      );
      expect(find.text('pve-totbvm-149'), findsWidgets);
      expect(
        api.requests.any((r) => r['action'] == 'requestConnection'),
        isFalse,
      );
    },
  );

  Map<String, dynamic> approvalRequest({
    String origin = 'https://cloud.example.com',
    String status = 'pending',
    Duration validity = const Duration(minutes: 10),
  }) => {
    'status': status,
    'origin': origin,
    'requestId': 'fixture-pending',
    'expiresAt': DateTime.now().add(validity).toIso8601String(),
  };

  Future<void> approvalTick(WidgetTester t) async {
    await t.pump(const Duration(milliseconds: 3010));
    await t.pumpAndSettle();
  }

  Map<String, dynamic> brokerEvidence({
    String state = 'online',
    bool online = true,
    num? age = 1,
    String reason = '',
  }) => {
    'state': state,
    'online': online,
    'heartbeatAgeSeconds': age,
    'lastReadyAt': 1792700400000,
    'lastHeartbeatAt': 1792700400000,
    'failureReason': reason,
  };

  testWidgets(
    'Registration and service alone never imply broker connectivity',
    (t) async {
      final api = FakeHostSettings(registered: true);
      await open(t, api, size: const Size(886, 673));
      expect(pill(t), '서버 연결 안 됨');
      expect(serverLine(t), '서버 연결 확인 필요');
      expect(serverLine(t), isNot(confirmed));
      expect(find.textContaining('서버 연결을 확인하지 못했어요'), findsOneWidget);
      expect(find.textContaining('온라인'), findsNothing);
      await tap(t, 'host-broker-diagnostics');
      // The running service is said on its own line and in the summary.
      expect(find.text('서비스 실행 중'), findsNWidgets(2));
      expect(find.text('확인된 응답 없음'), findsNWidgets(3));
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Broker connectivity requires a current ACK and a running service',
    (t) async {
      for (final testCase in [
        (
          brokerEvidence(state: 'awaiting_ack', online: false, age: null),
          true,
          false,
        ),
        (brokerEvidence(online: false), true, false),
        (brokerEvidence(age: null), true, false),
        (brokerEvidence(age: -1), true, false),
        (brokerEvidence(age: double.nan), true, false),
        (brokerEvidence(age: 31), true, false),
        (brokerEvidence(), false, false),
        (brokerEvidence(), true, true),
      ]) {
        final api = FakeHostSettings(registered: true)
          ..running = testCase.$2
          ..displays = 1
          ..statusOverrides = {'broker': testCase.$1};
        await open(t, api);
        expect(serverLine(t), testCase.$3 ? confirmed : isNot(confirmed));
        expect(
          pill(t),
          testCase.$3 ? '스트리밍 가능' : isNot(contains('스트리밍')),
        );
        await t.pumpWidget(const SizedBox());
        await t.pumpAndSettle();
      }
    },
  );

  testWidgets(
    'Fresh broker ACKs update quietly and failed reads revoke certainty',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..statusOverrides = {
          'broker': brokerEvidence(
            state: 'awaiting_ack',
            online: false,
            age: null,
          ),
        };
      await open(t, api);
      expect(find.text('서버 응답 대기'), findsOneWidget);
      api.statusOverrides = {'broker': brokerEvidence()};
      await approvalTick(t);
      expect(serverLine(t), confirmed);
      api.failStatus = true;
      await approvalTick(t);
      expect(find.text('연결 상태 조회 지연'), findsOneWidget);
      expect(serverLine(t), isNot(confirmed));
      api.failStatus = false;
      api.statusOverrides = {
        'broker': brokerEvidence(
          state: 'reconnecting',
          online: false,
          reason: 'network_unavailable',
        ),
      };
      await approvalTick(t);
      expect(find.text('서버 다시 연결 중'), findsOneWidget);
      expect(find.textContaining('Windows의 네트워크'), findsOneWidget);
      expect(find.byKey(const Key('host-result-dialog')), findsNothing);
    },
  );

  testWidgets('An old ACK ages out while a slow status process stays single', (
    t,
  ) async {
    final api = FakeHostSettings(registered: true)
      ..statusOverrides = {'broker': brokerEvidence(age: 29.9)};
    await open(t, api);
    expect(serverLine(t), confirmed);
    final delayed = Completer<Map<String, dynamic>>();
    api.nextStatus = delayed;
    await t.pump(const Duration(seconds: 3));
    await t.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 150)),
    );
    await t.pump(const Duration(seconds: 3));
    await t.pump();
    expect(serverLine(t), '서버 응답 확인 필요');
    expect(api.requests.where((r) => r['action'] == 'status'), hasLength(2));
    await t.pumpWidget(const SizedBox());
    delayed.complete({
      'ok': true,
      'status': {'registered': true},
    });
    await t.pumpAndSettle();
    await t.pump(const Duration(seconds: 20));
    expect(api.requests.where((r) => r['action'] == 'status'), hasLength(2));
    expect(t.takeException(), isNull);
  });

  testWidgets(
    'Broker errors use safe actions and never render unknown raw reasons',
    (t) async {
      for (final testCase in [
        ('authorization_failed', '연결을 다시 요청'),
        ('tls_failed', 'HTTPS 주소와 인증서'),
        ('protocol_rejected', 'Host와 서버 버전'),
        ('raw-secret-token', '서버 연결을 확인하지 못했어요'),
      ]) {
        final api = FakeHostSettings(registered: true)
          ..statusOverrides = {
            'broker': brokerEvidence(
              state: 'unknown',
              online: false,
              reason: testCase.$1,
            ),
          };
        await open(t, api);
        expect(find.textContaining(testCase.$2), findsOneWidget);
        expect(find.textContaining('raw-secret-token'), findsNothing);
        await t.pumpWidget(const SizedBox());
        await t.pumpAndSettle();
      }
    },
  );

  testWidgets('Registered status polling respects foreground and disposal', (
    t,
  ) async {
    final api = FakeHostSettings(registered: true)
      ..statusOverrides = {'broker': brokerEvidence()};
    await open(t, api);
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
    await t.pump(const Duration(seconds: 20));
    expect(api.requests.where((r) => r['action'] == 'status'), hasLength(1));
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await approvalTick(t);
    expect(api.requests.where((r) => r['action'] == 'status'), hasLength(2));
    await t.pumpWidget(const SizedBox());
    await t.pump(const Duration(seconds: 20));
    expect(api.requests.where((r) => r['action'] == 'status'), hasLength(2));
  });

  testWidgets(
    'First connection request needs no registration code and survives closing settings',
    (t) async {
      final api = FakeHostSettings(), guard = FakeCloseGuard();
      await open(t, api, closeGuard: guard);
      await tap(t, 'host-open-server');
      await t.enterText(
        find.byKey(const Key('host-server-origin')),
        'https://cloud.example.com',
      );
      await tap(t, 'host-test-server');
      await tap(t, 'host-request-connection');
      expect(find.byKey(const Key('host-confirm-dialog')), findsNothing);
      expect(
        api.requests.where((r) => r['action'] == 'requestConnection').single,
        {'action': 'requestConnection', 'origin': 'https://cloud.example.com'},
      );
      await tap(t, 'host-result-close');
      expect(find.text('관리자 승인 대기'), findsOneWidget);
      expect(find.text('창을 닫아도 Windows 서비스가 연결 요청을 계속 확인해요.'), findsOneWidget);
      expect(find.textContaining('등록 코드'), findsNothing);
      expect(find.textContaining('온라인'), findsNothing);
      expect(find.byType(LinearProgressIndicator), findsNothing);
      expect(guard.dirty, isFalse);
      await guard.onRequest!();
      await t.pump();
      expect(guard.closes, 1);
      expect(
        api.requests.where((r) => r['action'] == 'cancelConnectionV1'),
        isEmpty,
      );
      await t.pumpWidget(const SizedBox());
      final count = api.requests.length;
      await t.pump(const Duration(seconds: 12));
      expect(api.requests.length, count);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'A pending request shows the verification code to compare with the console, and only a well-formed one',
    (t) async {
      final api = FakeHostSettings()
        ..connectionRequest = {
          ...approvalRequest(),
          'verificationCode': '49J2-HQP1',
        };
      await open(t, api);
      // The overview says it in one line, with the code whole on its own.
      expect(pill(t), '관리자 승인 필요');
      expect(find.text('49J2-HQP1'), findsOneWidget);
      expect(
        find.descendant(
          of: find.byType(HostProblemBanner),
          matching: find.byKey(const Key('host-overview-code')),
        ),
        findsOneWidget,
      );
      await tap(t, 'host-open-request');
      expect(find.text('관리자 승인 대기'), findsOneWidget);
      expect(find.byKey(const Key('host-verification-code')), findsOneWidget);
      expect(find.text('49J2-HQP1'), findsOneWidget);
      expect(find.textContaining('관리자 웹에도 이 코드가 보여야 해요'), findsOneWidget);
      expect(find.textContaining('확인 코드가 같은지 확인한 뒤 승인해 주세요'), findsOneWidget);
      // Released Hosts and malformed values show no code (no invented text).
      for (final value in [
        null,
        '',
        'x',
        '49j2-hqp1',
        '49J2HQP1',
        'OIL0-0000',
        42,
      ]) {
        api.connectionRequest = {
          ...approvalRequest(),
          'verificationCode': ?value,
        };
        await approvalTick(t);
        expect(
          find.byKey(const Key('host-verification-code')),
          findsNothing,
          reason: '$value',
        );
        expect(
          find.textContaining('데스크톱의 승인 대기에서 이 연결 요청을 승인해 주세요'),
          findsOneWidget,
        );
      }
      // Once approved (registration being confirmed) the code is no longer asked for.
      api.connectionRequest = {
        ...approvalRequest(status: 'approved'),
        'verificationCode': '49J2-HQP1',
      };
      await approvalTick(t);
      expect(find.byKey(const Key('host-verification-code')), findsNothing);
      await nav(t, 0);
      expect(find.byKey(const Key('host-overview-code')), findsNothing);
      expect(find.textContaining('49J2'), findsNothing);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Approval polling becomes connection monitoring after real registration',
    (t) async {
      final api = FakeHostSettings()..connectionRequest = approvalRequest();
      await open(t, api);
      expect(api.requests, hasLength(1));
      await t.pump(const Duration(seconds: 2));
      expect(api.requests, hasLength(1));
      await t.pump(const Duration(milliseconds: 1010));
      await t.pumpAndSettle();
      expect(api.requests, hasLength(2));
      expect(pill(t), '관리자 승인 필요');
      await nav(t, 1);
      expect(find.text('관리자 승인 대기'), findsOneWidget);
      api.registered = true;
      api.connectionRequest = {...api.connectionRequest, 'status': 'approved'};
      await approvalTick(t);
      // Registered: back on the overview, which names the desktop.
      expect(find.byKey(const Key('host-approval-heading')), findsNothing);
      expect(
        t.widget<Text>(find.byKey(const Key('host-connection-heading'))).data,
        'pve-totbvm-149',
      );
      expect(find.textContaining('관리자 승인으로 서버 등록이 완료됐어요.'), findsOneWidget);
      expect(find.textContaining('온라인'), findsNothing);
      final count = api.requests.length;
      await t.pump(const Duration(seconds: 15));
      expect(api.requests.length, greaterThan(count));
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      expect(find.byKey(const Key('host-approval-heading')), findsNothing);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Existing registration does not falsely complete a replacement approval',
    (t) async {
      final api = FakeHostSettings(
        registered: true,
      )..connectionRequest = approvalRequest(origin: 'https://new.example.com');
      await open(t, api);
      // Still registered with the old server while the new one is asked.
      expect(pill(t), '관리자 승인 필요');
      expect(
        find.descendant(
          of: find.byKey(const Key('host-open-server')),
          matching: find.text('new.example.com · 승인 대기'),
        ),
        findsOneWidget,
      );
      await nav(t, 1);
      expect(find.text('관리자 승인 대기'), findsOneWidget);
      expect(find.text('https://new.example.com'), findsOneWidget);
      expect(find.text('pve-totbvm-149'), findsNothing);
      api.connectionRequest = {
        ...api.connectionRequest,
        'status': 'approved',
        'error': 'already_approved',
      };
      await approvalTick(t);
      expect(find.text('서버 등록 확인 중'), findsOneWidget);
      expect(find.text('이미 승인된 요청이에요. 서버 등록 결과를 확인하고 있어요.'), findsOneWidget);
      expect(find.textContaining('등록이 완료됐어요'), findsNothing);
      api.statusOverrides = {'serverOrigin': 'https://new.example.com'};
      await approvalTick(t);
      expect(find.byKey(const Key('host-approval-heading')), findsNothing);
      expect(
        t.widget<Text>(find.byKey(const Key('host-connection-heading'))).data,
        'pve-totbvm-149',
      );
      expect(serverLine(t), startsWith('서버 '));
      await nav(t, 1);
      expect(find.text('https://new.example.com'), findsOneWidget);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Cancel requires confirmation and ignores a pending status response from before cancellation',
    (t) async {
      final api = FakeHostSettings()..connectionRequest = approvalRequest();
      await open(t, api);
      await nav(t, 1);
      final oldResponse = await api.request('status');
      final delayed = Completer<Map<String, dynamic>>();
      api.nextStatus = delayed;
      await t.pump(const Duration(milliseconds: 3010));
      await t.pump();
      await tap(t, 'host-cancel-connection');
      expect(
        api.requests.where((r) => r['action'] == 'cancelConnectionV1'),
        isEmpty,
      );
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      await tap(t, 'host-cancel-connection');
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pump();
      expect(
        api.requests.where((r) => r['action'] == 'cancelConnectionV1'),
        hasLength(1),
      );
      delayed.complete(oldResponse);
      await t.pumpAndSettle();
      await tap(t, 'host-result-close');
      expect(find.byKey(const Key('host-approval-heading')), findsNothing);
      expect(find.text('연결 요청을 취소했어요.'), findsOneWidget);
      final count = api.requests.length;
      await t.pump(const Duration(seconds: 12));
      expect(api.requests.length, count);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Editing a pending server waits for cancellation acknowledgement before a new request',
    (t) async {
      final api = FakeHostSettings()
        ..connectionRequest = approvalRequest()
        ..cancelPending = true;
      await open(t, api);
      await nav(t, 1);
      await tap(t, 'host-edit-pending-server');
      expect(find.text('연결할 서버를 수정할까요?'), findsOneWidget);
      expect(
        api.requests.where((r) => r['action'] == 'cancelConnectionV1'),
        isEmpty,
      );
      await tap(t, 'host-confirm');
      await tap(t, 'host-result-close');
      expect(find.text('연결 요청 취소 중'), findsOneWidget);
      expect(find.byKey(const Key('host-server-origin')), findsNothing);
      expect(find.byKey(const Key('host-request-connection')), findsNothing);
      expect(
        t
            .widget<OutlinedButton>(
              find.byKey(const Key('host-cancel-connection')),
            )
            .onPressed,
        isNull,
      );
      final count = api.requests.length;
      api.connectionRequest = {
        ...api.connectionRequest,
        'error': '',
        'cancelAcknowledged': true,
      };
      await approvalTick(t);
      expect(api.requests.length, count + 1);
      expect(find.byKey(const Key('host-server-origin')), findsOneWidget);
      expect(
        t
            .widget<FilledButton>(
              find.byKey(const Key('host-request-connection')),
            )
            .onPressed,
        isNull,
      );
      expect(find.byKey(const Key('host-approval-heading')), findsNothing);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'A slow approval status has a UI deadline without overlapping reads or applying late data',
    (t) async {
      final api = FakeHostSettings()..connectionRequest = approvalRequest();
      await open(t, api);
      await nav(t, 1);
      final lateResponse = await api.request('status');
      (lateResponse['status'] as Map)['registered'] = true;
      (lateResponse['status'] as Map)['desktopId'] = 'late-untrusted-result';
      (lateResponse['status'] as Map)['serverOrigin'] =
          'https://cloud.example.com';
      (lateResponse['status'] as Map)['connectionRequest'] = {
        ...api.connectionRequest,
        'status': 'approved',
      };
      final delayed = Completer<Map<String, dynamic>>();
      api.nextStatus = delayed;
      await t.pump(const Duration(milliseconds: 3010));
      await t.pump();
      final count = api.requests.length;
      await t.pump(const Duration(milliseconds: 10010));
      await t.pump();
      expect(
        find.byKey(const Key('host-approval-status-error')),
        findsOneWidget,
      );
      expect(find.byType(LinearProgressIndicator), findsNothing);
      await t.pump(const Duration(seconds: 9));
      expect(api.requests.length, count);
      delayed.complete(lateResponse);
      await t.pump();
      expect(find.text('late-untrusted-result'), findsNothing);
      expect(find.text('관리자 승인 대기'), findsOneWidget);
      await approvalTick(t);
      expect(find.byKey(const Key('host-approval-status-error')), findsNothing);
      await t.pumpWidget(const SizedBox());
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Approval checks pause in background and discard the prior foreground response',
    (t) async {
      final api = FakeHostSettings()..connectionRequest = approvalRequest();
      await open(t, api);
      final oldResponse = await api.request('status');
      final delayed = Completer<Map<String, dynamic>>();
      api.nextStatus = delayed;
      await t.pump(const Duration(milliseconds: 3010));
      await t.pump();
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
      void resume() {
        if (t.binding.lifecycleState == AppLifecycleState.paused) {
          t.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
        }
        if (t.binding.lifecycleState == AppLifecycleState.hidden) {
          t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
        }
        if (t.binding.lifecycleState != AppLifecycleState.resumed) {
          t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
        }
      }

      addTearDown(resume);
      await t.pump(const Duration(seconds: 20));
      final count = api.requests.length;
      delayed.complete(oldResponse);
      await t.pump();
      await t.pump(const Duration(seconds: 10));
      expect(api.requests.length, count);
      resume();
      await approvalTick(t);
      expect(api.requests.length, count + 1);
      await t.pumpWidget(const SizedBox());
      expect(t.takeException(), isNull);
    },
  );

  // host-machine-v1: a full clone of a registered VM. The Host kept the copied
  // registration aside; the window offers the normal request with one note.
  testWidgets('A copied registration offers a new connection request', (
    t,
  ) async {
    final api = FakeHostSettings()
      ..statusOverrides = {
        'copiedRegistration': true,
        'serverOrigin': 'https://vdi.example.com',
      };
    await open(t, api);
    await nav(t, 1);
    expect(
      t.widget<Text>(find.byKey(const Key('host-request-result'))).data,
      '이 Windows는 등록된 다른 데스크톱을 복사한 것 같아요. 이 데스크톱의 연결을 새로 요청하세요.',
    );
    expect(find.byKey(const Key('host-server-origin')), findsOneWidget);
    expect(find.byKey(const Key('host-request-connection')), findsOneWidget);
    await nav(t, 0);
    expect(
      t.widget<Text>(find.byKey(const Key('host-connection-result'))).data,
      startsWith('이 Windows는 등록된 다른 데스크톱을 복사한 것 같아요.'),
    );
  });

  testWidgets('A registered Host never shows the copied note', (t) async {
    final registered = FakeHostSettings(registered: true)
      ..statusOverrides = {'copiedRegistration': true};
    await open(t, registered);
    expect(find.textContaining('복사한 것 같아요'), findsNothing);
    await nav(t, 1);
    expect(find.textContaining('복사한 것 같아요'), findsNothing);
  });

  testWidgets(
    'Approval expiry stops automatic queries and terminal rejection exposes no raw error',
    (t) async {
      final api = FakeHostSettings()
        ..connectionRequest = approvalRequest(
          validity: const Duration(seconds: 1),
        );
      await open(t, api);
      await nav(t, 1);
      await t.pump(const Duration(seconds: 2));
      expect(find.text('승인 대기 시간 확인 필요'), findsOneWidget);
      final count = api.requests.length;
      await t.pump(const Duration(seconds: 15));
      expect(api.requests.length, count);
      api.connectionRequest = {
        ...api.connectionRequest,
        'status': 'rejected',
        'error': 'secret-arbitrary-native-error',
      };
      await tap(t, 'host-refresh');
      // The server page says why its form is back; so does the overview.
      expect(find.textContaining('관리자가 요청을 거절했어요.'), findsOneWidget);
      expect(find.byKey(const Key('host-server-origin')), findsOneWidget);
      expect(
        find.textContaining('secret-arbitrary-native-error'),
        findsNothing,
      );
      expect(find.byKey(const Key('host-approval-heading')), findsNothing);
      await nav(t, 0);
      expect(
        t.widget<Text>(find.byKey(const Key('host-connection-result'))).data,
        startsWith('관리자가 요청을 거절했어요.'),
      );
      expect(
        find.textContaining('secret-arbitrary-native-error'),
        findsNothing,
      );
      final afterRefresh = api.requests.length;
      await t.pump(const Duration(seconds: 10));
      expect(api.requests.length, afterRefresh);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Restart and healthy RDP-deferred displays cannot repeat installation',
    (t) async {
      final api = FakeHostSettings(registered: true)..rebootRequired = true;
      await open(t, api);
      await nav(t, 2);
      final button = find.byKey(const Key('host-display-setup'));
      expect(t.widget<OutlinedButton>(button).onPressed, isNull);
      expect(find.text('Windows 다시 시작 후 확인'), findsOneWidget);
      expect(find.textContaining('자동으로 다시 시작하지 않아요.'), findsOneWidget);
      api.rebootRequired = false;
      api.statusOverrides = {
        'display': {
          'installed': true,
          'deviceReady': true,
          'owned': true,
          'remoteSession': true,
          'configurationPending': true,
          'monitors': [],
        },
      };
      await tap(t, 'host-refresh');
      expect(t.widget<OutlinedButton>(button).onPressed, isNull);
      expect(find.text('Proxmox 콘솔에서 가상 화면 설정'), findsOneWidget);
      expect(
        find.textContaining('Proxmox 콘솔(로컬 PC는 본체 화면)에서 Windows에 로그인한 뒤'),
        findsOneWidget,
      );
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Display error reasons are allowlisted and do not render arbitrary helper text',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..statusOverrides = {
          'display': {'error': 'driver_signature_untrusted', 'monitors': []},
        };
      await open(t, api);
      await nav(t, 2);
      expect(find.textContaining('드라이버 파일을 확인하지 못했어요.'), findsOneWidget);
      api.statusOverrides = {
        'display': {'error': 'raw-private-device-or-token', 'monitors': []},
      };
      await tap(t, 'host-refresh');
      expect(find.textContaining('진단 정보를 복사해 관리자에게 알려 주세요.'), findsOneWidget);
      expect(find.textContaining('raw-private-device-or-token'), findsNothing);
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'The Windows account requires matching protected fields and confirmation and preserves password bytes',
    (t) async {
      final api = FakeHostSettings(registered: true), guard = FakeCloseGuard();
      await open(t, api, closeGuard: guard);
      await nav(t, 5);
      final user = find.byKey(const Key('host-account-username')),
          password = find.byKey(const Key('host-account-password')),
          confirmation = find.byKey(
            const Key('host-account-password-confirmation'),
          );
      await t.enterText(user, r'  TESTER\테스터  ');
      const secret = '  Fixture 암호!  ';
      await t.enterText(password, secret);
      await t.enterText(confirmation, 'different');
      expect(t.widget<TextField>(password).obscureText, isTrue);
      expect(t.widget<TextField>(confirmation).obscureText, isTrue);
      expect(t.widget<TextField>(password).enableSuggestions, isFalse);
      expect(t.widget<TextField>(confirmation).enableSuggestions, isFalse);
      expect(find.byIcon(Icons.visibility_outlined), findsNothing);
      await tap(t, 'host-save-account');
      expect(find.text('비밀번호가 일치하지 않아요.'), findsOneWidget);
      expect(
        api.requests.where((r) => r['action'] == 'saveRdpCredentials'),
        isEmpty,
      );
      await t.enterText(confirmation, secret);
      await tap(t, 'host-save-account');
      final dialog = find.byKey(const Key('host-confirm-dialog'));
      expect(dialog, findsOneWidget);
      expect(
        find.descendant(of: dialog, matching: find.textContaining(secret)),
        findsNothing,
      );
      expect(
        api.requests.where((r) => r['action'] == 'saveRdpCredentials'),
        isEmpty,
      );
      api.credentialPending = Completer<void>();
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pump();
      expect(t.widget<TextField>(password).controller!.text, isEmpty);
      expect(t.widget<TextField>(confirmation).controller!.text, isEmpty);
      expect(
        api.requests.where((r) => r['action'] == 'saveRdpCredentials').single,
        {
          'action': 'saveRdpCredentials',
          'username': r'TESTER\테스터',
          'password': secret,
          'passwordConfirmation': secret,
        },
      );
      expect(
        t
            .widget<FilledButton>(find.byKey(const Key('host-save-account')))
            .onPressed,
        isNull,
      );
      api.credentialPending!.complete();
      await t.pumpAndSettle();
      await tap(t, 'host-result-close');
      expect(find.text(r'TESTER\테스터'), findsOneWidget);
      expect(find.byKey(const Key('host-account-password')), findsNothing);
      expect(guard.dirty, isFalse);
      await tap(t, 'host-edit-account');
      expect(t.widget<TextField>(password).controller!.text, isEmpty);
      expect(t.widget<TextField>(confirmation).controller!.text, isEmpty);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'A rejected Windows account save clears entered passwords and deletion is confirmed',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..credentialsConfigured = true
        ..credentialUsername = r'TESTER\existing'
        ..rejectCredentials = true;
      await open(t, api);
      await nav(t, 5);
      await tap(t, 'host-edit-account');
      final password = find.byKey(const Key('host-account-password')),
          confirmation = find.byKey(
            const Key('host-account-password-confirmation'),
          );
      await t.enterText(password, 'fixture-wrong-password');
      await t.enterText(confirmation, 'fixture-wrong-password');
      await tap(t, 'host-save-account');
      await tap(t, 'host-confirm');
      await tap(t, 'host-result-close');
      expect(t.widget<TextField>(password).controller!.text, isEmpty);
      expect(t.widget<TextField>(confirmation).controller!.text, isEmpty);
      expect(api.credentialsConfigured, isTrue);
      await tap(t, 'host-cancel-account');
      await tap(t, 'host-clear-account');
      expect(
        api.requests.where((r) => r['action'] == 'clearWindowsLoginV1'),
        isEmpty,
      );
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(api.credentialsConfigured, isTrue);
      await tap(t, 'host-clear-account');
      await tap(t, 'host-confirm');
      await tap(t, 'host-result-close');
      expect(
        api.requests.where((r) => r['action'] == 'clearWindowsLoginV1'),
        hasLength(1),
      );
      expect(api.credentialsConfigured, isFalse);
      expect(
        t
            .widget<TextField>(find.byKey(const Key('host-account-username')))
            .controller!
            .text,
        isEmpty,
      );
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Approval polling preserves an in-progress Windows credential draft and close protection',
    (t) async {
      final api = FakeHostSettings()..connectionRequest = approvalRequest();
      final guard = FakeCloseGuard();
      await open(
        t,
        api,
        closeGuard: guard,
        size: const Size(640, 680),
        textScale: 1.25,
      );
      await nav(t, 5);
      final user = find.byKey(const Key('host-account-username')),
          password = find.byKey(const Key('host-account-password'));
      await t.enterText(user, '테스터');
      await t.enterText(password, 'fixture-draft');
      expect(guard.dirty, isTrue);
      await approvalTick(t);
      expect(t.widget<TextField>(user).controller!.text, '테스터');
      expect(t.widget<TextField>(password).controller!.text, 'fixture-draft');
      await t.ensureVisible(find.byKey(const Key('host-save-account')));
      expect(
        find.byKey(const Key('host-save-account')).hitTestable(),
        findsOneWidget,
      );
      final closing = guard.onRequest!();
      await t.pumpAndSettle();
      expect(find.text('저장하지 않은 설정을 버릴까요?'), findsOneWidget);
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      await closing;
      expect(guard.closes, 0);
      expect(
        api.requests.where((r) => r['action'] == 'saveRdpCredentials'),
        isEmpty,
      );
      await t.pumpWidget(const SizedBox());
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Existing display configuration conflict is explicit and is not masked as a restart',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..statusOverrides = {
          'display': {
            'error': 'existing_virtual_display_configuration',
            'installed': false,
            'owned': false,
            'foreignDeviceCount': 0,
            'rebootRequired': false,
            'monitors': [],
          },
        };
      await open(t, api);
      await nav(t, 2);
      expect(find.textContaining('기존 가상 화면 설정 확인이 필요해요.'), findsOneWidget);
      expect(find.text('Windows 다시 시작 후 확인'), findsNothing);
      expect(find.byKey(const Key('host-display-attention')), findsNothing);
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
    },
  );

  testWidgets(
    'Display repair lists configuration remnants separately and changes nothing before explicit confirmation',
    (t) async {
      final api = FakeHostSettings(registered: true);
      await open(t, api);
      await nav(t, 2);
      await tap(t, 'host-display-repair-scan');
      expect(find.textContaining('장치 0개 · 남아 있는 설정 1개'), findsOneWidget);
      expect(find.text('남아 있는 설정 · 필수'), findsOneWidget);
      expect(find.text('가상 화면 장치 · 필수'), findsNothing);
      expect(find.text('이전 가상 화면 설정'), findsOneWidget);
      expect(
        find.textContaining(r'HKLM\SOFTWARE\Multitap\Display'),
        findsOneWidget,
      );
      expect(find.textContaining('아직 삭제하지 않았어요.'), findsOneWidget);
      expect(find.text(api.repairPlan['planId'] as String), findsNothing);
      expect(
        api.requests.where((r) => r['action'] == 'repairDisplay'),
        isEmpty,
      );
      await tap(t, 'host-display-repair-apply');
      final dialog = find.byKey(const Key('host-confirm-dialog'));
      expect(
        find.descendant(
          of: dialog,
          matching: find.textContaining('이전 가상 화면 설정'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: dialog,
          matching: find.textContaining('드라이버 보관함·기존 설정 파일은 유지'),
        ),
        findsOneWidget,
      );
      expect(
        api.requests.where((r) => r['action'] == 'repairDisplay'),
        isEmpty,
      );
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(
        api.requests.where((r) => r['action'] == 'repairDisplay'),
        isEmpty,
      );
      expect(
        find.byKey(const Key('host-display-repair-apply')),
        findsOneWidget,
      );
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Confirmed display repair uses the reviewed plan once with real indeterminate progress and restart result',
    (t) async {
      final api = FakeHostSettings(registered: true), guard = FakeCloseGuard();
      (api.repairPlan['items'] as List).insert(0, {
        'id': 'fixture-device',
        'kind': 'device',
        'name': '기존 가상 화면 장치',
        'required': true,
        'hardwareId': r'Root\MttVDD',
        'instanceId': r'ROOT\DISPLAY\0001',
        'impact': '이 가상 화면이 연결 해제됩니다. 콘솔용 화면은 유지합니다.',
      });
      api.repairPlan['deviceCount'] = 1;
      api.repairResult['removedDeviceCount'] = 1;
      api.repairPending = Completer<void>();
      await open(t, api, closeGuard: guard);
      await nav(t, 2);
      await tap(t, 'host-display-repair-scan');
      expect(find.text('가상 화면 장치 · 필수'), findsOneWidget);
      expect(find.textContaining(r'ROOT\DISPLAY\0001'), findsOneWidget);
      await tap(t, 'host-display-repair-apply');
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pump();
      expect(api.requests.where((r) => r['action'] == 'repairDisplay').single, {
        'action': 'repairDisplay',
        'planId': api.repairPlan['planId'],
      });
      expect(guard.busy, isTrue);
      expect(
        t
            .widget<LinearProgressIndicator>(
              find.byKey(const Key('host-operation-progress')),
            )
            .value,
        isNull,
      );
      expect(find.byKey(const Key('host-display-repair-apply')), findsNothing);
      expect(
        t
            .widget<OutlinedButton>(
              find.byKey(const Key('host-display-repair-scan')),
            )
            .onPressed,
        isNull,
      );
      api.repairPending!.complete();
      await t.pumpAndSettle();
      expect(find.text('작업 완료'), findsOneWidget);
      expect(
        find.descendant(
          of: find.byKey(const Key('host-result-dialog')),
          matching: find.textContaining('Windows를 다시 시작하세요. 자동으로 다시 시작하지 않아요.'),
        ),
        findsOneWidget,
      );
      await tap(t, 'host-result-close');
      expect(find.text('직전 정리·설치 결과'), findsOneWidget);
      expect(find.text('Windows 다시 시작 후 확인'), findsOneWidget);
      expect(
        t
            .widget<OutlinedButton>(find.byKey(const Key('host-display-setup')))
            .onPressed,
        isNull,
      );
      expect(find.byKey(const Key('host-display-repair-apply')), findsNothing);
      expect(find.text('가상 화면 준비됨'), findsNothing);
      expect(
        api.requests.where((r) => r['action'] == 'repairDisplay'),
        hasLength(1),
      );
      expect(guard.busy, isFalse);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Display repair preserves structured partial failure and requires a fresh review for another attempt',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..repairExitCode = 1
        ..repairResult = {
          'repairInstalled': false,
          'cleanupCompleted': false,
          'partialRemoval': true,
          'removedDeviceCount': 1,
          'configurationRestored': true,
          'configurationRestoreAttempted': true,
          'rebootRequired': false,
          'configurationPending': false,
          'reason': 'arbitrary_private_helper_message',
          'backupId': 'must-not-render-backup-id',
          'state': {'secret': 'must-not-render-state'},
        };
      await open(t, api);
      await nav(t, 2);
      await tap(t, 'host-display-repair-scan');
      await tap(t, 'host-display-repair-apply');
      await tap(t, 'host-confirm');
      expect(find.text('결과 확인이 필요해요'), findsOneWidget);
      expect(
        find.descendant(
          of: find.byKey(const Key('host-result-dialog')),
          matching: find.textContaining('일부 항목만 정리됐어요.'),
        ),
        findsOneWidget,
      );
      expect(find.text('작업 완료'), findsNothing);
      await tap(t, 'host-result-close');
      expect(find.text('이전 설정을 복원했어요.'), findsOneWidget);
      expect(find.text('완료하지 못함'), findsOneWidget);
      expect(find.textContaining('일부 항목이 남아 있어요.'), findsOneWidget);
      expect(find.textContaining('must-not-render'), findsNothing);
      expect(
        find.textContaining('arbitrary_private_helper_message'),
        findsNothing,
      );
      expect(find.byKey(const Key('host-display-repair-apply')), findsNothing);
      expect(
        api.requests.where((r) => r['action'] == 'repairDisplay'),
        hasLength(1),
      );
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Display repair timeout and changed plan do not claim installation success',
    (t) async {
      for (final timedOut in [true, false]) {
        final api = FakeHostSettings(registered: true)
          ..repairExitCode = timedOut ? 259 : 1
          ..repairTimedOut = timedOut
          ..repairResult = {
            'repairInstalled': false,
            'cleanupCompleted': false,
            'removedDeviceCount': 0,
            'reason': timedOut ? 'repair_timeout' : 'repair_plan_changed',
          };
        await open(t, api);
        await nav(t, 2);
        await tap(t, 'host-display-repair-scan');
        await tap(t, 'host-display-repair-apply');
        await tap(t, 'host-confirm');
        expect(find.text('결과 확인이 필요해요'), findsOneWidget);
        expect(
          find.descendant(
            of: find.byKey(const Key('host-result-dialog')),
            matching: find.textContaining(
              timedOut ? '작업 응답 시간이 지났어요.' : '장치 또는 설정이 변경되어 정리를 시작하지 않았어요.',
            ),
          ),
          findsOneWidget,
        );
        await tap(t, 'host-result-close');
        final result = find.byKey(const Key('host-display-repair-result'));
        expect(
          find.descendant(of: result, matching: find.text('완료')),
          findsNothing,
        );
        if (timedOut) {
          expect(
            find.descendant(of: result, matching: find.text('확인 필요')),
            findsNWidgets(2),
          );
        }
        expect(
          find.byKey(const Key('host-display-repair-apply')),
          findsNothing,
        );
        expect(t.takeException(), isNull);
        await t.pumpWidget(const SizedBox());
      }
    },
  );

  testWidgets(
    'Display repair rejects a plan containing a protected system display and honors console-only review',
    (t) async {
      final api = FakeHostSettings(registered: true);
      (api.repairPlan['items'] as List).add({
        'id': 'protected',
        'kind': 'device',
        'name': 'Microsoft basic display',
        'required': true,
        'hardwareId': r'PCI\VEN_1234',
        'instanceId': r'PCI\PROTECTED',
        'impact': 'Must not be removed',
      });
      await open(t, api);
      await nav(t, 2);
      await tap(t, 'host-display-repair-scan');
      expect(find.textContaining('충돌 항목을 확인하지 못했어요.'), findsOneWidget);
      expect(find.byKey(const Key('host-display-repair-apply')), findsNothing);
      expect(find.text('Microsoft basic display'), findsNothing);
      expect(
        api.requests.where((r) => r['action'] == 'repairDisplay'),
        isEmpty,
      );
      (api.repairPlan['items'] as List).removeLast();
      api.repairPlan['canRepair'] = false;
      api.repairPlan['reason'] = 'repair_console_required';
      await tap(t, 'host-display-repair-scan');
      expect(
        find.byKey(const Key('host-display-repair-blocked')),
        findsOneWidget,
      );
      expect(
        t
            .widget<FilledButton>(
              find.byKey(const Key('host-display-repair-apply')),
            )
            .onPressed,
        isNull,
      );
      expect(
        api.requests.where((r) => r['action'] == 'repairDisplay'),
        isEmpty,
      );
      api.repairPlan['items'] = [];
      api.repairPlan['reason'] = 'driver_signature_untrusted';
      await tap(t, 'host-display-repair-scan');
      expect(find.textContaining('드라이버 파일을 확인하지 못했어요.'), findsOneWidget);
      expect(find.text('정리할 충돌 항목이 없어요.'), findsNothing);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Acknowledged device removal is not displayed as completed removal or overall repair success',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..repairResult = {
          'repairInstalled': true,
          'cleanupCompleted': true,
          'removedDeviceCount': 0,
          'removeRequestedCount': 1,
          'removalUncertain': true,
          'partialRemoval': false,
          'reason': 'repair_device_remove_pending',
        };
      await open(t, api);
      await nav(t, 2);
      await tap(t, 'host-display-repair-scan');
      await tap(t, 'host-display-repair-apply');
      await tap(t, 'host-confirm');
      expect(find.text('결과 확인이 필요해요'), findsOneWidget);
      expect(find.text('작업 완료'), findsNothing);
      expect(
        find.descendant(
          of: find.byKey(const Key('host-result-dialog')),
          matching: find.textContaining('실제 제거를 확인하지 못했어요.'),
        ),
        findsOneWidget,
      );
      await tap(t, 'host-result-close');
      expect(find.text('제거 요청한 장치'), findsOneWidget);
      expect(find.text('제거 확인된 장치'), findsOneWidget);
      expect(find.text('1개'), findsOneWidget);
      expect(find.text('0개'), findsOneWidget);
      expect(find.text('장치 제거 여부를 아직 확인하지 못했어요.'), findsOneWidget);
      final cleanupRow = find.byKey(const Key('host-repair-cleanup-state'));
      expect(
        find.descendant(of: cleanupRow, matching: find.text('확인 필요')),
        findsOneWidget,
      );
      expect(
        find.descendant(of: cleanupRow, matching: find.text('완료')),
        findsNothing,
      );
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Unregistered host presents one connection action without claiming online status',
    (t) async {
      final api = FakeHostSettings();
      await open(t, api, size: const Size(886, 673));
      final heading = find.byKey(const Key('host-connection-heading')),
          action = find.byKey(const Key('host-connect')),
          row = find.byKey(const Key('host-open-server'));
      // No name and no kind are claimed for a desktop the server never saw.
      expect(t.widget<Text>(heading).data, '이 데스크톱');
      expect(
        t.widget<Text>(find.byKey(const Key('host-machine-line'))).data,
        '아직 서버에 등록하지 않았어요',
      );
      expect(pill(t), '서버 연결 필요');
      // ONE banner with ONE action.
      expect(find.byType(HostProblemBanner), findsOneWidget);
      expect(find.textContaining('관리자에게 받은 서버 주소로 연결을 요청하세요.'), findsOneWidget);
      expect(find.byType(FilledButton), findsOneWidget);
      expect(t.widget<FilledButton>(action).onPressed, isNotNull);
      expect(find.widgetWithText(FilledButton, '연결하기'), findsOneWidget);
      expect(action.hitTestable(), findsOneWidget);
      expect(t.getRect(action).bottom, lessThan(673));
      expect(row.hitTestable(), findsOneWidget);
      expect(t.getRect(row).bottom, lessThanOrEqualTo(673));
      expect(find.textContaining('온라인'), findsNothing);
      expect(find.textContaining('연결됨'), findsNothing);
      expect(find.textContaining('VM'), findsNothing);
      // Devices join once the desktop is registered.
      expect(find.byKey(const Key('host-open-devices')), findsNothing);
      await tap(t, 'host-connect');
      expect(find.byKey(const Key('host-server-origin')), findsOneWidget);
      expect(find.byKey(const Key('host-enroll-code')), findsNothing);
      expect(find.textContaining('등록 코드'), findsNothing);
      expect(find.text('먼저 서버 연결 테스트를 완료하세요.'), findsOneWidget);
      expect(api.requests.map((request) => request['action']), ['status']);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Registered host shows VM and server with a secondary settings action',
    (t) async {
      final api = FakeHostSettings(registered: true)..displays = 2;
      await open(t, api, size: const Size(640, 680), textScale: 1.25);
      expect(
        t.widget<Text>(find.byKey(const Key('host-connection-heading'))).data,
        'pve-totbvm-149',
      );
      // A Proxmox VM by the id the server gave; the id is the title here
      // because this Host was told no name.
      expect(
        t.widget<Text>(find.byKey(const Key('host-machine-line'))).data,
        'Proxmox VM',
      );
      // The server row opens its page; the one amber action belongs to the
      // one problem (no fresh server acknowledgement in this fixture).
      expect(
        t.widget(find.byKey(const Key('host-open-server'))),
        isA<HostAreaRow>(),
      );
      expect(find.byType(HostProblemBanner), findsOneWidget);
      expect(find.byType(FilledButton), findsOneWidget);
      expect(find.widgetWithText(FilledButton, '연결 진단'), findsOneWidget);
      expect(find.textContaining('온라인'), findsNothing);
      await tap(t, 'host-open-server');
      expect(find.text('서버 연결 정보'), findsOneWidget);
      expect(find.text('등록 정보는 업데이트 후에도 유지돼요.'), findsOneWidget);
      expect(find.text('https://cloud.example.com'), findsOneWidget);
      expect(find.text('pve-totbvm-149'), findsOneWidget);
      expect(find.byKey(const Key('host-server-origin')), findsNothing);
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Connection request guidance follows the current tested address without a code',
    (t) async {
      final api = FakeHostSettings();
      await open(t, api, size: const Size(640, 680), textScale: 1.25);
      await tap(t, 'host-open-server');
      final origin = find.byKey(const Key('host-server-origin')),
          enroll = find.byKey(const Key('host-request-connection'));
      expect(find.text('먼저 서버 연결 테스트를 완료하세요.'), findsOneWidget);
      expect(t.widget<FilledButton>(enroll).onPressed, isNull);
      await t.enterText(origin, 'https://cloud.example.com');
      await tap(t, 'host-test-server');
      expect(find.text('이제 연결 요청을 보내세요.'), findsOneWidget);
      expect(t.widget<FilledButton>(enroll).onPressed, isNotNull);
      expect(find.byKey(const Key('host-enroll-code')), findsNothing);
      await t.enterText(origin, 'https://different.example.com');
      await t.pump();
      expect(find.text('먼저 서버 연결 테스트를 완료하세요.'), findsOneWidget);
      expect(t.widget<FilledButton>(enroll).onPressed, isNull);
      await t.ensureVisible(enroll);
      expect(enroll.hitTestable(), findsOneWidget);
      expect(
        api.requests.where(
          (request) => request['action'] == 'requestConnection',
        ),
        isEmpty,
      );
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Restart guidance is visible before server controls in a compact host window',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..rebootRequired = true
        ..statusOverrides = {'version': '0.2.10'};
      await open(t, api, size: const Size(886, 673));
      final banner = find.byKey(const Key('host-display-attention'));
      expect(banner.hitTestable(), findsOneWidget);
      // The restart outranks the server (no fresh acknowledgement here).
      expect(pill(t), 'Windows 다시 시작 필요');
      expect(find.byType(HostProblemBanner), findsOneWidget);
      expect(find.textContaining('자동으로 다시 시작하지 않아요.'), findsOneWidget);
      expect(t.getRect(banner).bottom, lessThan(673));
      expect(
        t.getRect(banner).bottom,
        lessThan(t.getTopLeft(find.byKey(const Key('host-open-server'))).dy),
      );
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      await tap(t, 'host-refresh');
      expect(
        banner.hitTestable(),
        findsOneWidget,
        reason: 'Refresh does not dismiss a restart still required by native status.',
      );
      api.rebootRequired = false;
      await tap(t, 'host-refresh');
      expect(banner, findsNothing);
      // The next problem takes the one banner.
      expect(pill(t), '서버 연결 안 됨');
      expect(find.byType(HostProblemBanner), findsOneWidget);
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Recovery guidance precedes registration and opens details without changing displays',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..recoveryRequired = true
        ..rebootRequired = true
        ..displays = 2;
      await open(t, api, size: const Size(640, 480), textScale: 1.25);
      final banner = find.byKey(const Key('host-display-attention'));
      // In the smallest window at large text the status is at the top and
      // the banner begins in view (it follows the status).
      expect(
        find.byKey(const Key('host-overview-state')).hitTestable(),
        findsOneWidget,
      );
      expect(pill(t), 'Windows 다시 시작 필요');
      expect(banner.hitTestable(), findsOneWidget);
      expect(t.getRect(banner).top, lessThan(480));
      expect(find.byType(HostProblemBanner), findsOneWidget);
      expect(find.textContaining('복구를 진행하세요.'), findsOneWidget);
      expect(find.textContaining('Windows를 다시 시작하세요.'), findsOneWidget);
      expect(
        find.descendant(
          of: find.byKey(const Key('host-open-display')),
          matching: find.textContaining('준비됨'),
        ),
        findsNothing,
        reason: 'Pending recovery/restart takes precedence over stale ready output.',
      );
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      await tap(t, 'host-display-attention-open');
      expect(
        find.widgetWithText(OutlinedButton, 'Windows 다시 시작 후 확인'),
        findsOneWidget,
      );
      expect(find.textContaining('이전 화면 변경이 중단됐어요.'), findsOneWidget);
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Recovery requires confirmation and never silently installs a display',
    (t) async {
      final api = FakeHostSettings(registered: true)..recoveryRequired = true;
      await open(t, api);
      await nav(t, 2);
      expect(find.textContaining('이전 화면 변경이 중단'), findsOneWidget);
      await tap(t, 'host-display-setup');
      expect(
        api.requests.where((r) => r['action'] == 'recoverDisplay'),
        isEmpty,
      );
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      await tap(t, 'host-display-setup');
      await tap(t, 'host-confirm');
      expect(find.byKey(const Key('host-result-dialog')), findsOneWidget);
      await tap(t, 'host-result-close');
      expect(
        api.requests.where((r) => r['action'] == 'recoverDisplay'),
        hasLength(1),
      );
      expect(api.requests.where((r) => r['action'] == 'display'), isEmpty);
      expect(find.text('가상 모니터 1대 켜짐'), findsOneWidget);
    },
  );
  testWidgets(
    'Replacing a registered server requires a current tested address and explicit confirmation',
    (t) async {
      final api = FakeHostSettings(registered: true);
      await open(t, api);
      await nav(t, 1);
      await tap(t, 'host-change-server');
      final origin = find.byKey(const Key('host-server-origin'));
      await t.enterText(origin, 'https://one.example.com');
      await t.pump();
      expect(
        t
            .widget<FilledButton>(
              find.byKey(const Key('host-request-connection')),
            )
            .onPressed,
        isNull,
      );
      await tap(t, 'host-test-server');
      await t.enterText(origin, 'https://two.example.com');
      await t.pump();
      expect(
        t
            .widget<FilledButton>(
              find.byKey(const Key('host-request-connection')),
            )
            .onPressed,
        isNull,
      );
      api.rejectTest = true;
      await tap(t, 'host-test-server');
      expect(
        t
            .widget<FilledButton>(
              find.byKey(const Key('host-request-connection')),
            )
            .onPressed,
        isNull,
      );
      api.rejectTest = false;
      await tap(t, 'host-test-server');
      await tap(t, 'host-request-connection');
      expect(
        api.requests.where((r) => r['action'] == 'requestConnection'),
        isEmpty,
      );
      expect(find.text('서버 연결을 변경할까요?'), findsOneWidget);
      await tap(t, 'host-confirm');
      await tap(t, 'host-result-close');
      expect(
        api.requests.where((r) => r['action'] == 'requestConnection').single,
        {'action': 'requestConnection', 'origin': 'https://two.example.com'},
      );
      expect(find.byKey(const Key('host-enroll-code')), findsNothing);
      expect(find.text('관리자 승인 대기'), findsOneWidget);
      expect(
        api.requests
            .where((r) => r['action'] == 'requestConnection')
            .single['origin'],
        'https://two.example.com',
      );
      await t.pumpWidget(const SizedBox());
    },
  );
  testWidgets('Non-admin narrow layout distinguishes service from stream', (
    t,
  ) async {
    final api = FakeHostSettings(registered: true, administrator: false);
    await open(t, api, size: const Size(720, 800));
    // The screen answers for itself, the sign-in for the session.
    expect(find.text('가상 화면을 확인해야 해요'), findsOneWidget);
    expect(find.text('로그인돼 있지 않거나 잠겨 있어요'), findsNothing);
    await nav(t, 4);
    expect(find.text('서비스 실행 중'), findsNWidgets(2));
    await nav(t, 2);
    expect(find.text('가상 화면 확인 필요'), findsOneWidget);
    expect(api.requests.every(isReadOnlyHostRequest), isTrue);
    expect(t.takeException(), isNull);
  });
  testWidgets(
    'Pending service operation uses indeterminate progress and prevents duplicate changes',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..pending = Completer<void>();
      await open(t, api);
      await nav(t, 4);
      await tap(t, 'host-service');
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 350));
      expect(
        t
            .widget<LinearProgressIndicator>(
              find.byKey(const Key('host-operation-progress')),
            )
            .value,
        isNull,
      );
      expect(
        t
            .widget<OutlinedButton>(find.byKey(const Key('host-service')))
            .onPressed,
        isNull,
      );
      expect(
        t.widget<IconButton>(find.byKey(const Key('host-refresh'))).onPressed,
        isNull,
      );
      expect(
        api.requests.where((r) => r['action'] == 'restartServiceV1'),
        hasLength(1),
      );
      api.pending!.complete();
      await t.pumpAndSettle();
      await tap(t, 'host-result-close');
      expect(find.text('서비스 다시 시작'), findsOneWidget);
    },
  );
  testWidgets(
    'Accepted operation with failed status is reported as uncertain instead of false success',
    (t) async {
      final api = FakeHostSettings(registered: true);
      await open(t, api);
      await nav(t, 4);
      await tap(t, 'host-service');
      api.failStatus = true;
      await tap(t, 'host-confirm');
      expect(find.text('결과 확인이 필요해요'), findsOneWidget);
      expect(
        find.descendant(
          of: find.byKey(const Key('host-result-dialog')),
          matching: find.textContaining('작업 응답은 받았지만'),
        ),
        findsOneWidget,
      );
      await tap(t, 'host-result-close');
      api.failStatus = false;
      await t.pumpAndSettle(const Duration(seconds: 1));
      if (find.byKey(const Key('host-result-close')).evaluate().isNotEmpty) {
        await tap(t, 'host-result-close');
      }
      await tap(t, 'host-refresh');
      expect(find.text('서비스 다시 시작'), findsOneWidget);
      // The native transaction owns stop+start even when its observer's
      // following status read fails. The UI does not replay either command.
      expect(
        api.requests.where((r) => r['action'] == 'restartServiceV1'),
        hasLength(1),
      );
      expect(api.requests.where((r) => r['action'] == 'service'), isEmpty);
    },
  );
  testWidgets(
    'Window close is blocked during a real pending change and restored after it finishes',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..pending = Completer<void>();
      final guard = FakeCloseGuard();
      await open(t, api, closeGuard: guard);
      await nav(t, 4);
      await tap(t, 'host-service');
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 350));
      expect(guard.busy, isTrue);
      await guard.onRequest!();
      await t.pump();
      expect(guard.closes, 0);
      expect(find.text('설정 작업이 끝난 뒤 창을 닫아 주세요.'), findsOneWidget);
      api.pending!.complete();
      await t.pumpAndSettle();
      await tap(t, 'host-result-close');
      expect(guard.busy, isFalse);
    },
  );
  testWidgets(
    'A Windows account draft needs a discard confirmation before native close',
    (t) async {
      final guard = FakeCloseGuard();
      await open(t, FakeHostSettings(registered: true), closeGuard: guard);
      await nav(t, 5);
      await t.enterText(
        find.byKey(const Key('host-account-username')),
        'draft',
      );
      await t.pump();
      expect(guard.dirty, isTrue);
      final cancelled = guard.onRequest!();
      await t.pumpAndSettle();
      expect(find.text('저장하지 않은 설정을 버릴까요?'), findsOneWidget);
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      await cancelled;
      expect(guard.closes, 0);
      expect(
        t
            .widget<TextField>(find.byKey(const Key('host-account-username')))
            .controller!
            .text,
        'draft',
      );
      final confirmed = guard.onRequest!();
      await t.pumpAndSettle();
      await tap(t, 'host-confirm');
      await confirmed;
      expect(guard.closes, 1);
    },
  );
  testWidgets('Unprotected native window prevents a setting mutation', (
    t,
  ) async {
    final api = FakeHostSettings(registered: true),
        guard = FakeCloseGuard()..available = false;
    await open(t, api, closeGuard: guard);
    await nav(t, 4);
    await tap(t, 'host-service');
    await tap(t, 'host-confirm');
    expect(api.requests.where((r) => r['action'] == 'service'), isEmpty);
    expect(
      find.descendant(
        of: find.byKey(const Key('host-result-dialog')),
        matching: find.textContaining('작업을 시작하지 못했어요'),
      ),
      findsOneWidget,
    );
  });
  testWidgets(
    'Invalid server addresses are inline and never reach the native helper',
    (t) async {
      final api = FakeHostSettings();
      await open(t, api);
      await nav(t, 1);
      final field = find.byKey(const Key('host-server-origin'));
      await t.enterText(field, 'ftp://insecure.example.com');
      await tap(t, 'host-test-server');
      expect(
        t.widget<TextField>(field).decoration!.errorText,
        contains('HTTP 또는 HTTPS'),
      );
      expect(t.widget<TextField>(field).focusNode!.hasFocus, isTrue);
      expect(api.requests.where((r) => r['action'] == 'testBroker'), isEmpty);
      await t.enterText(field, 'cloud.example.com/');
      await tap(t, 'host-test-server');
      expect(
        t.widget<TextField>(field).controller!.text,
        'https://cloud.example.com',
      );
      expect(
        api.requests.where((r) => r['action'] == 'testBroker').single['origin'],
        'https://cloud.example.com',
      );
    },
  );

  testWidgets(
    'Service lookup failure remains unknown and cannot be mistaken for a missing service',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..statusOverrides = {
          'service': {
            'installed': null,
            'running': null,
            'state': 'unknown',
            'queryError': 5,
          },
        };
      await open(t, api);
      // Unknown is said as unknown: no "stopped", no start button.
      expect(pill(t), 'Host 서비스 확인 필요');
      // Said once, by the status beside the name (no route line repeats it).
      expect(find.text('서비스 조회 필요'), findsNothing);
      expect(find.text('서비스 미설치'), findsNothing);
      expect(find.byKey(const Key('host-service-start')), findsNothing);
      await nav(t, 4);
      expect(find.text('서비스 조회 필요'), findsNWidgets(2));
      expect(find.text('서비스 미설치'), findsNothing);
      expect(
        t
            .widget<OutlinedButton>(find.byKey(const Key('host-service')))
            .onPressed,
        isNull,
      );
      expect(find.text('Windows 오류 5'), findsOneWidget);
    },
  );
  testWidgets(
    'Diagnostics are copied only on explicit click and exclude arbitrary secret status fields',
    (t) async {
      final copied = <String>[];
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(SystemChannels.platform, (call) async {
        if (call.method == 'Clipboard.setData') {
          copied.add((call.arguments as Map)['text'] as String);
        }
        return null;
      });
      addTearDown(
        () => messenger.setMockMethodCallHandler(SystemChannels.platform, null),
      );
      final api = FakeHostSettings(registered: true)
        ..statusOverrides = {
          'token': 'must-not-copy',
          'password': 'must-not-copy-password',
        };
      await open(t, api);
      await nav(t, 4);
      expect(copied, isEmpty);
      await tap(t, 'host-copy-diagnostics');
      expect(copied, hasLength(1));
      expect(copied.single, contains('Multitap Host 진단'));
      expect(copied.single, isNot(contains('must-not-copy')));
      expect(find.text('진단 정보를 복사했어요.'), findsOneWidget);
    },
  );
}

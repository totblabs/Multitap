import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/login_memory.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_preview.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'helpers/stream_connect_consent.dart';

final fixtureOrigin = Uri.parse('https://cloud.example.com');

/// The tile's status sentence (its first line).
String statusOf(WidgetTester tester, String id) =>
    tester.widget<Text>(find.byKey(Key('status-$id'))).data!;
String badgeDetail(WidgetTester tester, String key) => tester
    .widget<Tooltip>(
      find
          .ancestor(of: find.byKey(Key(key)), matching: find.byType(Tooltip))
          .first,
    )
    .message!;

class TestLoginMemory implements LoginMemory {
  final entries = <String, RememberedLogin>{};
  Future<RememberedLogin> Function(Uri)? onLoad;
  int passwordSaves = 0;
  int loads = 0;
  bool failWrites = false;
  bool failPasswordSave = false;
  int passwordSaveAttempts = 0;

  @override
  Future<RememberedLogin> load(Uri origin) async {
    loads++;
    if (onLoad != null) return onLoad!(origin);
    return entries[origin.origin] ?? const RememberedLogin();
  }

  @override
  Future<void> saveUsername(Uri origin, String username) async {
    if (failWrites) throw StateError('Storage unavailable');
    final previous = entries[origin.origin];
    entries[origin.origin] = RememberedLogin(
      username: username,
      password: previous?.username == username ? previous?.password : null,
    );
  }

  @override
  Future<void> savePassword(
    Uri origin,
    String username,
    String password,
  ) async {
    passwordSaveAttempts++;
    if (failWrites || failPasswordSave) {
      throw StateError('Credential vault unavailable');
    }
    passwordSaves++;
    entries[origin.origin] = RememberedLogin(
      username: username,
      password: password,
    );
  }

  @override
  Future<void> forgetPassword(Uri origin) async {
    if (failWrites) throw StateError('Credential vault unavailable');
    entries[origin.origin] = RememberedLogin(
      username: entries[origin.origin]?.username ?? '',
    );
  }
}

/// Only the native delete reply is substituted; the UI still awaits the real
/// WindowsCredentialVault MethodChannel future and its own ordered write tail.
class HeldNativeLoginMemory extends TestLoginMemory {
  final firstDelete = Completer<void>();
  final calls = <String>[];
  int nativeDeletes = 0;

  @override
  Future<void> saveUsername(Uri origin, String username) async {
    calls.add('username:$username');
    await super.saveUsername(origin, username);
  }

  @override
  Future<void> forgetPassword(Uri origin) async {
    calls.add('forget:start');
    await const WindowsCredentialVault().delete(origin.origin);
    await super.forgetPassword(origin);
    calls.add('forget:end');
  }

  @override
  Future<void> savePassword(
    Uri origin,
    String username,
    String password,
  ) async {
    calls.add('password:$username');
    await super.savePassword(origin, username, password);
  }
}

class TestBroker implements BrokerApi {
  @override
  Future<VirtualDisplayState> configureDisplays(
    String token,
    String desktopId,
    int count,
  ) async => throw const BrokerException('display_not_supported');
  @override
  Future<VirtualDisplayState> display(
    String token,
    String desktopId, {
    String? operationId,
  }) async => const VirtualDisplayState();
  @override
  Future<BrokerPowerResult> requestPower(
    String token,
    String desktopId,
    String action,
  ) async => throw const BrokerException('invalid_power_state');
  @override
  Future<BrokerPowerResult> power(
    String token,
    String desktopId, {
    String? taskId,
  }) async => throw const BrokerException('power_status_unavailable');
  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    connects++;
    lastConnectToken = token;
    lastConnectDesktopId = desktopId;
    lastStreamOptions = options;
    lastConnectMode = mode;
    // Stop before a real transport in this widget test. The real route must
    // still call the broker, rather than silently opening the demo preview.
    throw const BrokerException('network_error');
  }

  int connects = 0;
  String? lastConnectToken, lastConnectDesktopId;
  String? lastConnectMode;
  StreamOptions? lastStreamOptions;
  bool rejectLogin = false,
      rejectLogout = false,
      networkFailure = false,
      expired = false;
  int logouts = 0;
  int logins = 0;
  int passwordChanges = 0;
  String? lastPassword, lastNewPassword;
  @override
  Future<void> changePassword(
    String token,
    String currentPassword,
    String newPassword,
    String confirmation,
  ) async {
    passwordChanges++;
    lastNewPassword = newPassword;
  }

  List<BrokerDesktop> assigned = [];
  @override
  Future<BrokerSession> login(String username, String password) async {
    logins++;
    lastPassword = password;
    if (rejectLogin) throw const BrokerException('invalid_credentials', 401);
    return BrokerSession(
      token: 'test-token',
      expiresAt: DateTime.now().add(const Duration(hours: 1)),
      username: username,
    );
  }

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    if (expired) throw const BrokerException('session_expired', 401);
    if (networkFailure) throw const BrokerException('network_error');
    return assigned;
  }

  @override
  Future<void> logout(String token) async {
    logouts++;
    if (rejectLogout) throw const BrokerException('network_error');
  }

  @override
  void close() {}
}

const assignedVm = BrokerDesktop(
  id: 'assigned-vm',
  name: 'My Server VM',
  kind: 'vm',
  description: 'Assigned by admin',
  os: 'Windows 11',
  status: 'host_not_registered',
  canConnect: false,
);
void main() {
  signInMemoryObservationTests();

  void mockStreamPresentation() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    for (final name in ['multitap/window', 'multitap/window/events']) {
      final channel = MethodChannel(name);
      messenger.setMockMethodCallHandler(
        channel,
        (call) async => call.method == 'fullscreen' ? call.arguments : null,
      );
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    }
  }

  Future<void> confirmWorkspaceSignOut(WidgetTester tester) async {
    await tester.tap(find.byKey(const Key('workspace-account')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('sign-out')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('confirm-sign-out')));
    await tester.pumpAndSettle();
  }

  test('Session JSON requires the ID without a separate display name', () {
    final session = BrokerSession.fromJson({
      'sessionToken': 'test-token',
      'expiresAt': '2026-10-22T09:00:00Z',
      'user': {'username': '한글아이디'},
    });
    expect(session.username, '한글아이디');
  });

  Future<void> start(
    WidgetTester tester,
    TestBroker api, {
    LoginMemory memory = const NoopLoginMemory(),
  }) async {
    tester.view.physicalSize = const Size(1440, 900);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MultitapApp(api: api, serverOrigin: fixtureOrigin, loginMemory: memory),
    );
    await tester.pumpAndSettle();
  }

  Future<void> login(WidgetTester tester, {String username = 'alice'}) async {
    await tester.enterText(find.byKey(const Key('sign-in-id')), username);
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'test-password',
    );
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
  }

  testWidgets(
    'Password focus scopes native IME mode and preserves pasted Unicode passwords',
    (tester) async {
      const channel = MethodChannel('multitap/input');
      final modes = <bool>[];
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (call) async {
            expect(call.method, 'passwordMode');
            modes.add((call.arguments as Map)['enabled'] as bool);
            return null;
          });
      addTearDown(
        () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
            .setMockMethodCallHandler(channel, null),
      );
      final api = TestBroker();
      await start(tester, api);
      await tester.enterText(find.byKey(const Key('sign-in-id')), '한글아이디');
      await tester.tap(find.byKey(const Key('sign-in-password')));
      await tester.pump();
      expect(modes.last, isTrue);
      await tester.tap(find.byKey(const Key('sign-in-id')));
      await tester.pump();
      expect(modes.last, isFalse);
      const pasted = '붙여넣은비밀번호123';
      await tester.enterText(find.byKey(const Key('sign-in-password')), pasted);
      await tester.tap(find.byKey(const Key('login-submit')));
      await tester.pumpAndSettle();
      expect(api.lastPassword, pasted);
    },
  );

  testWidgets(
    'Self password change validates confirmation, then clears saved password and requires login',
    (tester) async {
      final memory = TestLoginMemory();
      final api = TestBroker();
      await start(tester, api, memory: memory);
      await tester.tap(find.byKey(const Key('remember-password')));
      await login(tester);
      expect(memory.entries[fixtureOrigin.origin]?.password, isNotNull);
      await tester.tap(find.byKey(const Key('open-app-settings')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('account-change-password')));
      await tester.pumpAndSettle();
      for (final key in [
        'current-password',
        'new-password',
        'confirm-new-password',
      ]) {
        expect(
          tester.widget<TextField>(find.byKey(Key(key))).obscureText,
          isTrue,
        );
      }
      await tester.enterText(
        find.byKey(const Key('current-password')),
        'test-password',
      );
      await tester.enterText(
        find.byKey(const Key('new-password')),
        '새비밀번호-test123',
      );
      await tester.enterText(
        find.byKey(const Key('confirm-new-password')),
        'mismatched-password',
      );
      await tester.tap(find.byKey(const Key('save-password-change')));
      await tester.pumpAndSettle();
      expect(api.passwordChanges, 0);
      expect(find.byKey(const Key('password-change-error')), findsOneWidget);
      await tester.enterText(
        find.byKey(const Key('confirm-new-password')),
        '새비밀번호-test123',
      );
      await tester.tap(find.byKey(const Key('save-password-change')));
      await tester.pumpAndSettle();
      expect(api.passwordChanges, 1);
      expect(api.lastNewPassword, '새비밀번호-test123');
      expect(find.byKey(const Key('login-submit')), findsOneWidget);
      expect(memory.entries[fixtureOrigin.origin]?.password, isNull);
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        'alice',
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        isEmpty,
      );
      expect(
        tester.widget<Switch>(find.byKey(const Key('remember-password'))).value,
        isFalse,
      );
    },
  );

  for (final rememberPassword in [false, true]) {
    testWidgets(
      'Unavailable login memory never blocks manual authentication (remember=$rememberPassword)',
      (tester) async {
        final memory = TestLoginMemory()..failWrites = true;
        final api = TestBroker();
        await start(tester, api, memory: memory);
        if (rememberPassword) {
          await tester.tap(find.byKey(const Key('remember-password')));
        }
        await login(tester);
        expect(api.logins, 1);
        expect(find.byKey(const Key('empty-desktops')), findsOneWidget);
        expect(find.byType(SnackBar), findsOneWidget);
        expect(find.text('로그인 정보 기억 설정을 저장하지 못했어요.'), findsOneWidget);
        expect(memory.entries, isEmpty);
        expect(memory.passwordSaves, 0);
      },
    );
  }

  // The server's error line must not stay while the ID or the password is
  // being edited (as the local one clears).
  testWidgets('The error of the last attempt goes with the next edit', (
    tester,
  ) async {
    final api = TestBroker()..rejectLogin = true;
    await start(tester, api);
    await login(tester);
    expect(find.byKey(const Key('login-error')), findsOneWidget);
    await tester.enterText(find.byKey(const Key('sign-in-password')), 'x');
    await tester.pump();
    expect(find.byKey(const Key('login-error')), findsNothing);
    await login(tester);
    expect(find.byKey(const Key('login-error')), findsOneWidget);
    await tester.enterText(find.byKey(const Key('sign-in-id')), 'alice2');
    await tester.pump();
    expect(find.byKey(const Key('login-error')), findsNothing);
    expect(api.logins, 2);
  });

  testWidgets('Memory failures preserve the authentication error separately', (
    tester,
  ) async {
    final memory = TestLoginMemory()..failWrites = true;
    final api = TestBroker()..rejectLogin = true;
    await start(tester, api, memory: memory);
    await login(tester);
    expect(api.logins, 1);
    expect(find.byKey(const Key('login-error')), findsOneWidget);
    expect(find.text('아이디 또는 비밀번호를 확인해 주세요.'), findsOneWidget);
    expect(find.byKey(const Key('login-memory-warning')), findsOneWidget);
    expect(
      tester
          .widget<TextField>(find.byKey(const Key('sign-in-password')))
          .controller!
          .text,
      isEmpty,
    );
  });

  testWidgets(
    'A failed optional password save authenticates but does not claim a remembered password',
    (tester) async {
      final memory = TestLoginMemory()..failPasswordSave = true;
      final api = TestBroker();
      await start(tester, api, memory: memory);
      await tester.tap(find.byKey(const Key('remember-password')));
      await login(tester);
      expect(api.logins, 1);
      expect(memory.passwordSaveAttempts, 1);
      expect(memory.entries[fixtureOrigin.origin]?.username, 'alice');
      expect(memory.entries[fixtureOrigin.origin]?.password, isNull);
      expect(find.byType(SnackBar), findsOneWidget);
      await confirmWorkspaceSignOut(tester);
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        'alice',
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        isEmpty,
      );
      expect(
        tester.widget<Switch>(find.byKey(const Key('remember-password'))).value,
        isFalse,
      );
    },
  );

  testWidgets(
    'Production login cannot enter a demo and rejected credentials stay at login',
    (tester) async {
      final api = TestBroker()..rejectLogin = true;
      await start(tester, api);
      expect(find.byKey(const Key('open-preview')), findsNothing);
      await login(tester, username: '한글아이디');
      expect(find.byKey(const Key('login-error')), findsOneWidget);
      expect(find.text('내 데스크톱'), findsNothing);
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        isEmpty,
      );
    },
  );
  testWidgets(
    'No assignments means a real empty workspace without sample desktops',
    (tester) async {
      await start(tester, TestBroker());
      await login(tester, username: '한글아이디');
      expect(find.text('한글아이디'), findsOneWidget);
      expect(find.byKey(const Key('empty-desktops')), findsOneWidget);
      expect(find.byKey(const Key('card-sample-office')), findsNothing);
      expect(find.byKey(const Key('open-preview')), findsNothing);
    },
  );
  testWidgets(
    'Ready server assignment enables a real connection and opens the broker RemoteView route',
    (tester) async {
      mockStreamPresentation();
      final api = TestBroker()
        ..assigned = [
          const BrokerDesktop(
            id: 'ready-vm',
            name: 'Ready Server VM',
            kind: 'vm',
            description: 'Assigned by admin',
            os: 'Windows 11',
            status: 'ready',
            canConnect: true,
            hostConnected: true,
            virtualMonitorControl: true,
            display: VirtualDisplayState(
              supported: true,
              activeCount: 1,
              stale: false,
            ),
            monitors: [
              RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
            ],
          ),
        ];
      await start(tester, api);
      await login(tester);
      // A host that streams is running; the tile says so without a badge.
      expect(
        find.descendant(
          of: find.byKey(const Key('power-state-ready-vm')),
          matching: find.text('실행 중'),
        ),
        findsOneWidget,
      );
      expect(
        tester.widget<Text>(find.byKey(const Key('status-ready-vm'))).data,
        '연결 준비됨',
      );
      expect(find.byKey(const Key('stream-state-ready-vm')), findsNothing);
      expect(find.text('연결 가능'), findsNothing);
      expect(find.text('Host 등록 준비 중'), findsNothing);
      expect(find.text('원격 연결 준비 중'), findsNothing);
      final button = find.byKey(const Key('open-ready-vm'));
      expect(tester.widget<FilledButton>(button).onPressed, isNotNull);
      expect(
        find.descendant(of: button, matching: find.text('스트리밍 연결')),
        findsOneWidget,
      );
      expect(api.connects, 0);
      await tester.tap(find.byKey(const Key('stream-modes-ready-vm')));
      await tester.pumpAndSettle();
      expect(api.connects, 0);
      await tester.tap(find.byKey(const Key('stream-mode-fullscreen')));
      await tester.pumpAndSettle();
      await acceptStreamConnect(tester, 'ready-vm');
      expect(api.connects, 1);
      expect(api.lastConnectDesktopId, 'ready-vm');
      expect(api.lastConnectToken, 'test-token');
      expect(find.byType(RemoteView), findsOneWidget);
      expect(find.byType(SessionPreview), findsNothing);
      // The session toolbar rests as a handle until clicked.
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await tester.pump();
      await tester.tap(find.byKey(const Key('session-disconnect')));
      await tester.pumpAndSettle();
      // Only a live connection asks first.
      if (find.text('연결을 끊을까요?').evaluate().isNotEmpty) {
        await tester.tap(find.widgetWithText(FilledButton, '연결 끊기'));
        await tester.pumpAndSettle();
      }
      expect(find.byKey(const Key('card-ready-vm')), findsOneWidget);
    },
  );
  testWidgets(
    'Offline and powered-off assignments show distinct status and cannot start a connection',
    (tester) async {
      final api = TestBroker()
        ..assigned = [
          const BrokerDesktop(
            id: 'offline-vm',
            name: 'Offline VM',
            kind: 'vm',
            description: '',
            os: 'Windows 11',
            status: 'host_offline',
            canConnect: false,
          ),
          const BrokerDesktop(
            id: 'off-vm',
            name: 'Powered Off VM',
            kind: 'vm',
            description: '',
            os: 'Windows 11',
            status: 'powered_off',
            canConnect: false,
          ),
        ];
      await start(tester, api);
      await login(tester);
      // No host report and no power snapshot: still being checked.
      expect(statusOf(tester, 'offline-vm'), '상태를 확인하고 있어요');
      expect(find.byKey(const Key('stream-state-offline-vm')), findsOneWidget);
      // The broker reports this one off; without a Proxmox power snapshot
      // the client cannot start it from here.
      expect(statusOf(tester, 'off-vm'), '꺼져 있어요');
      expect(
        find.descendant(
          of: find.byKey(const Key('power-state-off-vm')),
          matching: find.text('꺼짐'),
        ),
        findsOneWidget,
      );
      expect(find.text('Host 등록 준비 중'), findsNothing);
      for (final key in ['open-offline-vm', 'start-off-vm']) {
        final button = find.byKey(Key(key));
        expect(tester.widget<FilledButton>(button).onPressed, isNull);
        await tester.tap(button);
      }
      await tester.pumpAndSettle();
      expect(api.connects, 0);
      expect(find.byType(RemoteView), findsNothing);
    },
  );
  testWidgets(
    'Online service with unprepared display shows its exact reason rather than waiting for an agent',
    (tester) async {
      final api = TestBroker()
        ..assigned = [
          BrokerDesktop.fromJson({
            'id': 'vm149',
            'name': '온라인 VM',
            'kind': 'vm',
            'description': '',
            'os': 'Windows 11',
            'status': 'ready',
            'canConnect': false,
            'connectionModes': {'stream': false},
            'host': {
              'connected': true,
              'version': '0.2.0',
              'capabilities': {
                'service': true,
                'interactiveSession': true,
                'streaming': false,
                'virtualDisplayReady': false,
                'monitors': [],
                'displayCount': 0,
              },
            },
          }),
        ];
      await start(tester, api);
      await login(tester);
      expect(statusOf(tester, 'vm149'), '가상 화면이 준비되지 않았어요');
      // The one main button is the refresh; its tooltip keeps the reason.
      expect(badgeDetail(tester, 'attention-vm149'), '가상 화면이 준비되지 않았어요');
      expect(find.text('Host 연결을 기다리고 있어요.'), findsNothing);
      expect(find.byKey(const Key('open-vm149')), findsNothing);
      await tester.tap(find.byKey(const Key('stream-state-vm149')));
      await tester.pumpAndSettle();
      expect(find.textContaining('준비 안 됨 · Host 설정에서 확인해야 해요'), findsOneWidget);
      await tester.tap(find.byKey(const Key('close-readiness')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('stream-modes-vm149')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('emergency-console-vm149')), findsOneWidget);
      expect(
        tester
            .widget<MenuItemButton>(
              find.byKey(const Key('stream-mode-windowed')),
            )
            .onPressed,
        isNull,
      );
      expect(api.connects, 0);
    },
  );
  testWidgets(
    "모든 모니터 follows this PC's monitor count from the window channel",
    (tester) async {
      var monitors = 2;
      final reads = <Object?>[];
      const window = MethodChannel('multitap/window');
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(window, (call) async {
        if (call.method != 'localMonitorCount') return null;
        reads.add(call.arguments);
        return monitors;
      });
      addTearDown(() => messenger.setMockMethodCallHandler(window, null));
      final api = TestBroker()
        ..assigned = [
          const BrokerDesktop(
            id: 'dual',
            name: 'VM',
            kind: 'vm',
            description: '',
            os: 'Windows 11',
            status: 'ready',
            canConnect: true,
            canStream: true,
            independentDisplayStreaming: true,
          ),
        ];
      await start(tester, api);
      await login(tester);
      expect(reads, isNotEmpty);
      expect(reads, everyElement(isNull), reason: 'No arguments.');
      Future<VoidCallback?> allMonitors() async {
        await tester.tap(find.byKey(const Key('stream-modes-dual')));
        await tester.pumpAndSettle();
        final item = tester.widget<MenuItemButton>(
          find.byKey(const Key('stream-mode-allMonitors')),
        );
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        return item.onPressed;
      }

      expect(await allMonitors(), isNotNull);
      // A monitor unplugged later: read again after the display change.
      monitors = 1;
      final before = reads.length;
      tester.view.physicalSize = tester.view.physicalSize + const Offset(8, 0);
      addTearDown(tester.view.resetPhysicalSize);
      await tester.pump(const Duration(milliseconds: 600));
      await tester.pumpAndSettle();
      expect(reads.length, greaterThan(before));
      expect(await allMonitors(), isNull);
      expect(api.connects, 0);
    },
  );
  testWidgets(
    'Streaming settings forward actual selected options and do not advertise unreported dual displays',
    (tester) async {
      mockStreamPresentation();
      final api = TestBroker()
        ..assigned = [
          const BrokerDesktop(
            id: 'settings-vm',
            name: 'Settings VM',
            kind: 'vm',
            description: '',
            os: 'Windows 11',
            status: 'ready',
            canConnect: true,
            hostConnected: true,
            virtualMonitorControl: true,
            display: VirtualDisplayState(
              supported: true,
              activeCount: 1,
              stale: false,
            ),
            monitors: [
              RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
            ],
          ),
        ];
      await start(tester, api);
      await login(tester);
      await tester.tap(find.byKey(const Key('stream-modes-settings-vm')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('stream-settings-settings-vm')));
      await tester.pumpAndSettle();
      expect(
        find.byKey(const Key('stream-dynamic-resolution')),
        findsOneWidget,
      );
      expect(find.byKey(const Key('stream-resolution')), findsNothing);
      expect(find.text('창 크기에 맞춰 원격 해상도가 바뀌어요.'), findsOneWidget);
      expect(find.text('가상 화면 두 개 함께'), findsNothing);
      await tester.tap(find.byKey(const Key('stream-advanced')));
      await tester.pumpAndSettle();
      await tester.tap(find.text('30 fps'));
      // Sound is always on: there is no switch to turn it off.
      expect(find.byKey(const Key('stream-audio')), findsNothing);
      await tester.drag(
        find.byKey(const Key('stream-bitrate')),
        const Offset(-500, 0),
      );
      await tester.tap(find.byKey(const Key('apply-stream-settings')));
      await tester.pumpAndSettle();
      await acceptStreamConnect(tester, 'settings-vm');
      expect(api.connects, 1);
      expect(api.lastStreamOptions!.toJson(), {
        'maxWidth': 3840,
        'maxHeight': 2160,
        'fps': 30,
        'bitrate': 4000000,
        'audio': true,
        'monitor': '0',
      });
      expect(find.byType(RemoteView), findsOneWidget);
    },
  );
  testWidgets(
    'Streaming settings offer no virtual-monitor choice; the screen mode decides the outputs',
    (tester) async {
      mockStreamPresentation();
      final api = TestBroker()
        ..assigned = [
          const BrokerDesktop(
            id: 'dual-vm',
            name: 'Dual VM',
            kind: 'vm',
            description: '',
            os: 'Windows 11',
            status: 'ready',
            canConnect: true,
            monitors: [
              RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
              RemoteMonitor(id: '1', width: 1920, height: 1080),
            ],
          ),
        ];
      await start(tester, api);
      await login(tester);
      await tester.tap(find.byKey(const Key('stream-modes-dual-vm')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('stream-settings-dual-vm')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('stream-resolution')), findsNothing);
      // No 화면 selector even with two remote outputs — a window
      // or full screen shows one output, 모든 모니터 사용 shows both.
      expect(find.byKey(const Key('stream-monitor-0')), findsNothing);
      expect(find.textContaining('가상 화면'), findsNothing);
      expect(find.byKey(const Key('apply-stream-settings')), findsOneWidget);
      await tester.tap(find.text('취소'));
      await tester.pumpAndSettle();
      expect(api.connects, 0);
    },
  );
  testWidgets(
    'Only server assignments appear and unavailable hosts cannot connect',
    (tester) async {
      final api = TestBroker()..assigned = [assignedVm];
      await start(tester, api);
      await login(tester);
      expect(find.text('My Server VM'), findsOneWidget);
      expect(
        tester
            .widget<FilledButton>(find.byKey(const Key('open-assigned-vm')))
            .onPressed,
        isNull,
      );
      for (final label in ['추가', '편집', '삭제']) {
        expect(find.text(label), findsNothing);
      }
      api.assigned = [];
      await tester.tap(find.byKey(const Key('refresh-desktops')));
      await tester.pumpAndSettle();
      expect(find.text('My Server VM'), findsNothing);
      expect(find.byKey(const Key('empty-desktops')), findsOneWidget);
    },
  );
  testWidgets(
    'Refresh failure hides stale assignments and a revoked session returns to login',
    (tester) async {
      final api = TestBroker()..assigned = [assignedVm];
      await start(tester, api);
      await login(tester);
      api.networkFailure = true;
      await tester.tap(find.byKey(const Key('refresh-desktops')));
      await tester.pumpAndSettle();
      expect(find.text('My Server VM'), findsNothing);
      expect(find.byKey(const Key('workspace-error')), findsOneWidget);
      api.expired = true;
      await tester.tap(find.byKey(const Key('refresh-desktops')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('session-end-ack')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('login-submit')), findsOneWidget);
      expect(find.byKey(const Key('login-error')), findsOneWidget);
    },
  );
  testWidgets(
    'Logout revokes the server session and clears credentials, failure stays visible',
    (tester) async {
      final api = TestBroker()..rejectLogout = true;
      await start(tester, api);
      await login(tester);
      await confirmWorkspaceSignOut(tester);
      // Its own line, not the list's error (다시 시도 there would reload the
      // list): it says what failed and signs out again.
      expect(find.byKey(const Key('workspace-error')), findsNothing);
      expect(find.byKey(const Key('workspace-error-card')), findsNothing);
      expect(
        tester
            .widget<Text>(find.byKey(const Key('workspace-sign-out-error')))
            .data,
        '로그아웃하지 못했어요. 네트워크를 확인하고 다시 시도하세요.',
      );
      expect(api.logouts, 1);
      api.rejectLogout = false;
      await tester.tap(find.byKey(const Key('workspace-sign-out-retry')));
      await tester.pumpAndSettle();
      expect(api.logouts, 2);
      expect(find.byKey(const Key('login-submit')), findsOneWidget);
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        isEmpty,
      );
    },
  );

  testWidgets('Fresh installs require an explicitly tested server address', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1440, 900);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final api = TestBroker();
    final memory = TestLoginMemory();
    await tester.pumpWidget(MultitapApp(api: api, loginMemory: memory));
    await tester.pumpAndSettle();
    expect(memory.loads, 0);
    // The address form is open from the start: it is the first thing to fill in.
    expect(find.byKey(const Key('login-server-form')), findsOneWidget);
    expect(find.byKey(const Key('sign-in-id')), findsNothing);
    expect(
      tester
          .widget<TextField>(find.byKey(const Key('server-origin')))
          .controller!
          .text,
      isEmpty,
    );
    expect(
      tester
          .widget<FilledButton>(find.byKey(const Key('save-server')))
          .onPressed,
      isNull,
    );
    // Cancelled, the card shows the sign-in fields disabled and the way back.
    await tester.tap(find.byKey(const Key('cancel-server-settings')));
    await tester.pumpAndSettle();
    expect(
      tester.widget<TextField>(find.byKey(const Key('sign-in-id'))).enabled,
      isFalse,
    );
    expect(find.text('서버 설정하고 시작'), findsOneWidget);
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('login-server-form')), findsOneWidget);
    expect(api.logins, 0);
  });

  testWidgets(
    'IDs are remembered automatically while passwords default off and always stay obscured',
    (tester) async {
      final memory = TestLoginMemory();
      final api = TestBroker();
      await start(tester, api, memory: memory);
      expect(
        tester.widget<Switch>(find.byKey(const Key('remember-password'))).value,
        isFalse,
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .obscureText,
        isTrue,
      );
      expect(find.byIcon(Icons.visibility_outlined), findsNothing);
      expect(find.byIcon(Icons.visibility_off_outlined), findsNothing);
      await login(tester, username: '한글사용자');
      expect(memory.entries[fixtureOrigin.origin]?.username, '한글사용자');
      expect(memory.entries[fixtureOrigin.origin]?.password, isNull);
      expect(memory.passwordSaves, 0);
      await confirmWorkspaceSignOut(tester);
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        '한글사용자',
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        isEmpty,
      );
      expect(api.logins, 1);
      await login(tester, username: '한글사용자');
      api.expired = true;
      await tester.tap(find.byKey(const Key('refresh-desktops')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('session-end-ack')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('login-error')), findsOneWidget);
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        '한글사용자',
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        isEmpty,
      );
    },
  );

  testWidgets(
    'Password opt-in survives logout and relaunch; opt-out deletes it before another login',
    (tester) async {
      final memory = TestLoginMemory();
      final api = TestBroker();
      await start(tester, api, memory: memory);
      await tester.tap(find.byKey(const Key('remember-password')));
      await login(tester);
      expect(memory.entries[fixtureOrigin.origin]?.password, 'test-password');
      expect(memory.passwordSaves, 1);
      await confirmWorkspaceSignOut(tester);
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        'test-password',
      );
      expect(
        tester.widget<Switch>(find.byKey(const Key('remember-password'))).value,
        isTrue,
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await start(tester, api, memory: memory);
      expect(
        api.logins,
        1,
        reason: 'Loading remembered credentials must not auto-login',
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        'alice',
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        'test-password',
      );
      await tester.tap(find.byKey(const Key('remember-password')));
      await tester.pumpAndSettle();
      expect(memory.entries[fixtureOrigin.origin]?.password, isNull);
      expect(api.logins, 1);
      await tester.pumpWidget(const SizedBox.shrink());
      await start(tester, api, memory: memory);
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        'alice',
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        isEmpty,
      );
      expect(
        tester.widget<Switch>(find.byKey(const Key('remember-password'))).value,
        isFalse,
      );
    },
  );

  testWidgets(
    'Changing accounts clears the autofilled password and removes the previous credential',
    (tester) async {
      final memory = TestLoginMemory()
        ..entries[fixtureOrigin.origin] = const RememberedLogin(
          username: 'alice',
          password: 'old-password',
        );
      final api = TestBroker();
      await start(tester, api, memory: memory);
      await tester.enterText(find.byKey(const Key('sign-in-id')), '다른사용자');
      await tester.pumpAndSettle();
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        isEmpty,
      );
      expect(memory.entries[fixtureOrigin.origin]?.username, '다른사용자');
      expect(memory.entries[fixtureOrigin.origin]?.password, isNull);
      expect(memory.passwordSaves, 0);
      expect(api.logins, 0);
    },
  );

  testWidgets(
    'Rejected remembered passwords are cleared and never saved again',
    (tester) async {
      final memory = TestLoginMemory()
        ..entries[fixtureOrigin.origin] = const RememberedLogin(
          username: 'alice',
          password: 'bad-password',
        );
      final api = TestBroker()..rejectLogin = true;
      await start(tester, api, memory: memory);
      await tester.tap(find.byKey(const Key('login-submit')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('login-error')), findsOneWidget);
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        isEmpty,
      );
      expect(memory.entries[fixtureOrigin.origin]?.password, isNull);
      expect(memory.passwordSaves, 0);
      expect(
        tester.widget<Switch>(find.byKey(const Key('remember-password'))).value,
        isTrue,
      );
    },
  );

  testWidgets(
    'Server changes load only that server credentials and late loads cannot overwrite edits',
    (tester) async {
      final otherOrigin = Uri.parse('https://other.example.com');
      final memory = TestLoginMemory()
        ..entries[fixtureOrigin.origin] = const RememberedLogin(
          username: 'alice',
          password: 'first-password',
        )
        ..entries[otherOrigin.origin] = const RememberedLogin(
          username: '둘째사용자',
          password: 'second-password',
        );
      await start(tester, TestBroker(), memory: memory);
      Widget screen(Uri origin) => MaterialApp(
        home: SignInScreen(
          serverOrigin: origin,
          loginMemory: memory,
          onServerChanged: (_) async {},
          onLogin: (_, _) async => SignInResult.failed,
        ),
      );
      await tester.pumpWidget(screen(fixtureOrigin));
      await tester.pumpAndSettle();
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        'alice',
      );
      await tester.pumpWidget(screen(otherOrigin));
      await tester.pumpAndSettle();
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        '둘째사용자',
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        'second-password',
      );
      final late = Completer<RememberedLogin>();
      memory.onLoad = (_) => late.future;
      await tester.pumpWidget(screen(fixtureOrigin));
      await tester.pump();
      await tester.enterText(find.byKey(const Key('sign-in-id')), '새로입력');
      await tester.enterText(
        find.byKey(const Key('sign-in-password')),
        'manual-password',
      );
      late.complete(
        const RememberedLogin(
          username: 'stale-user',
          password: 'stale-password',
        ),
      );
      await tester.pumpAndSettle();
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-id')))
            .controller!
            .text,
        '새로입력',
      );
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('sign-in-password')))
            .controller!
            .text,
        'manual-password',
      );
      expect(
        tester.widget<Switch>(find.byKey(const Key('remember-password'))).value,
        isFalse,
      );
    },
  );
}

void signInMemoryObservationTests() {
  void prepare(WidgetTester tester, HeldNativeLoginMemory memory) {
    tester.view.physicalSize = const Size(1440, 900);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    const channel = MethodChannel('multitap/credentials');
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'delete');
      if (++memory.nativeDeletes == 1) await memory.firstDelete.future;
      return null;
    });
    addTearDown(() {
      if (!memory.firstDelete.isCompleted) memory.firstDelete.complete();
      messenger.setMockMethodCallHandler(channel, null);
    });
  }

  HeldNativeLoginMemory memoryForAlice() => HeldNativeLoginMemory()
    ..entries[fixtureOrigin.origin] = const RememberedLogin(username: 'alice');

  Future<void> submitPassword(WidgetTester tester, String password) async {
    await tester.enterText(find.byKey(const Key('sign-in-password')), password);
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pump();
    await tester.pump();
  }

  String passwordOf(WidgetTester tester) => tester
      .widget<TextField>(find.byKey(const Key('sign-in-password')))
      .controller!
      .text;

  testWidgets(
    'Held native login memory releases rejected login controls after observation timeout',
    (tester) async {
      final memory = memoryForAlice();
      prepare(tester, memory);
      final api = TestBroker()..rejectLogin = true;
      await tester.pumpWidget(
        MultitapApp(api: api, serverOrigin: fixtureOrigin, loginMemory: memory),
      );
      await tester.pumpAndSettle();
      await submitPassword(tester, 'rejected-password');
      expect(api.logins, 1);
      expect(memory.nativeDeletes, 1);
      expect(
        tester.widget<TextField>(find.byKey(const Key('sign-in-id'))).enabled,
        isFalse,
      );
      await tester.pump(const Duration(milliseconds: 2999));
      expect(
        tester
            .widget<FilledButton>(find.byKey(const Key('login-submit')))
            .onPressed,
        isNull,
      );
      await tester.pump(const Duration(milliseconds: 1));
      await tester.pump();
      expect(find.text('아이디 또는 비밀번호를 확인해 주세요.'), findsOneWidget);
      expect(find.text('로그인 정보 기억 설정의 저장 결과를 확인하지 못했어요.'), findsOneWidget);
      expect(find.text('로그인 정보 기억 설정을 저장하지 못했어요.'), findsNothing);
      expect(
        tester.widget<TextField>(find.byKey(const Key('sign-in-id'))).enabled,
        isTrue,
      );
      expect(passwordOf(tester), isEmpty);
      expect(memory.firstDelete.isCompleted, isFalse);
      memory.firstDelete.complete();
      await tester.pumpAndSettle();
      expect(passwordOf(tester), isEmpty);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Held native login memory preserves ordered late completion and a newer identity',
    (tester) async {
      final memory = memoryForAlice();
      prepare(tester, memory);
      var logins = 0;
      await tester.pumpWidget(
        MaterialApp(
          home: SignInScreen(
            serverOrigin: fixtureOrigin,
            loginMemory: memory,
            onServerChanged: (_) async {},
            onLogin: (_, _) async =>
                ++logins == 1 ? SignInResult.rejected : SignInResult.success,
          ),
        ),
      );
      await tester.pumpAndSettle();
      await submitPassword(tester, 'old-password');
      await tester.pump(const Duration(seconds: 3));
      await tester.pump();
      expect(memory.calls, ['username:alice', 'forget:start']);
      await tester.enterText(find.byKey(const Key('sign-in-id')), 'bob');
      await tester.tap(find.byKey(const Key('remember-password')));
      await submitPassword(tester, 'new-password');
      expect(
        logins,
        2,
        reason: 'Authentication does not wait for the old vault reply',
      );
      expect(memory.calls, ['username:alice', 'forget:start']);
      await tester.pump(const Duration(seconds: 3));
      await tester.pump();
      expect(passwordOf(tester), 'new-password');
      expect(
        tester.widget<TextField>(find.byKey(const Key('sign-in-id'))).enabled,
        isTrue,
      );
      memory.firstDelete.complete();
      await tester.pumpAndSettle();
      expect(memory.calls, [
        'username:alice',
        'forget:start',
        'forget:end',
        'forget:start',
        'forget:end',
        'username:bob',
        'username:bob',
        'password:bob',
      ]);
      expect(memory.entries[fixtureOrigin.origin]?.username, 'bob');
      expect(memory.entries[fixtureOrigin.origin]?.password, 'new-password');
      expect(passwordOf(tester), 'new-password');
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Held native login memory cannot clear fields or warn on a different server after late authentication',
    (tester) async {
      final memory = memoryForAlice();
      prepare(tester, memory);
      final other = Uri.parse('https://other.example.com');
      memory.entries[other.origin] = const RememberedLogin(
        username: 'other-user',
        password: 'other-password',
      );
      final authentication = Completer<SignInResult>();
      Widget screen(Uri origin) => MaterialApp(
        home: SignInScreen(
          serverOrigin: origin,
          loginMemory: memory,
          onServerChanged: (_) async {},
          onLogin: (_, _) => authentication.future,
        ),
      );
      await tester.pumpWidget(screen(fixtureOrigin));
      await tester.pumpAndSettle();
      await submitPassword(tester, 'old-password');
      await tester.pumpWidget(screen(other));
      await tester.pump();
      await tester.pump();
      expect(passwordOf(tester), 'other-password');
      authentication.complete(SignInResult.rejected);
      await tester.pump();
      await tester.pump();
      expect(memory.nativeDeletes, 1);
      await tester.pump(const Duration(seconds: 3));
      await tester.pump();
      expect(passwordOf(tester), 'other-password');
      expect(find.byKey(const Key('login-memory-warning')), findsNothing);
      expect(find.byType(SnackBar), findsNothing);
      expect(
        tester.widget<TextField>(find.byKey(const Key('sign-in-id'))).enabled,
        isTrue,
      );
      memory.firstDelete.complete();
      await tester.pumpAndSettle();
      expect(passwordOf(tester), 'other-password');
      expect(memory.entries[other.origin]?.password, 'other-password');
      expect(tester.takeException(), isNull);
    },
  );
}

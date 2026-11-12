import '../helpers/stream_connect_consent.dart' show acceptStreamConnect;

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';

const _optionalPenChannel = MethodChannel('multitap/pen-input');

/// A tile's main button, by the name the Windows tile gives it in each
/// state (`open-`, `start-` or `attention-<id>`; Android renders that tile).
Finder mainButton(String id) => find.byWidgetPredicate(
  (widget) =>
      widget.key is ValueKey<String> &&
      RegExp('^(open|start|attention)-${RegExp.escape(id)}\$')
          .hasMatch((widget.key! as ValueKey<String>).value),
  description: 'the main button of $id',
);

const _task = '12345678-1234-1234-1234-123456789012';

BrokerDesktop vm({
  String id = 'pve-149',
  String name = 'TESTER',
  String power = 'running',
  bool stream = true,
  bool? canStop,
  VmResources resources = const VmResources(),
  DesktopUse use = DesktopUse.idle,
  bool? virtualDisplayReady,
  StreamPreparation? preparation,
  bool ownedOutput = false,
}) => BrokerDesktop(
  id: id,
  name: name,
  kind: 'vm',
  description: '',
  os: 'Windows 11 · 4 vCPU',
  status: power == 'stopped' ? 'powered_off' : 'online',
  canConnect: stream,
  canStream: stream,
  hostConnected: power == 'running',
  resources: resources,
  use: use,
  virtualDisplayReady: virtualDisplayReady,
  streamPreparation: preparation,
  display: ownedOutput
      ? VirtualDisplayState(
          supported: true,
          activeCount: 1,
          stale: false,
          checkedAt: DateTime.utc(2026, 11, 6),
        )
      : const VirtualDisplayState(),
  monitors: ownedOutput
      ? const [RemoteMonitor(id: '0', width: 1920, height: 1080)]
      : const [],
  power: BrokerPowerSnapshot(
    state: power,
    stale: false,
    checkedAt: DateTime.utc(2026, 10, 26),
  ),
  // A current broker allows stop on a running idle VM.
  powerCanForceStop: canStop ?? power == 'running',
);

class FakeBroker implements BrokerApi {
  FakeBroker(this.inventory);
  List<BrokerDesktop> inventory;
  final calls = <String>[];

  @override
  Future<BrokerSession> login(String username, String password) async {
    calls.add('login:$username');
    return BrokerSession(
      token: 'not-persisted',
      expiresAt: DateTime.now().add(const Duration(hours: 1)),
      username: username,
    );
  }

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    calls.add('desktops');
    return inventory;
  }

  @override
  Future<BrokerPowerResult> requestPower(
    String token,
    String desktopId,
    String action,
  ) async {
    calls.add('power:$action');
    return BrokerPowerResult(
      power: const BrokerPowerSnapshot(state: 'running', stale: false),
      task: BrokerPowerTask(
        id: _task,
        desktopId: desktopId,
        action: action,
        status: 'stopped',
        exitStatus: 'OK',
      ),
    );
  }

  @override
  Future<void> logout(String token) async => calls.add('logout');

  @override
  Future<BrokerConnection> connect(
    String token,
    String desktopId, {
    String mode = 'stream',
    StreamOptions? options,
  }) async {
    calls.add('connect');
    throw const BrokerException('network_error');
  }

  @override
  void close() {}

  @override
  dynamic noSuchMethod(Invocation invocation) =>
      throw UnimplementedError(invocation.memberName.toString());
}

/// The Android store channel backed by memory, recording every call.
class MemoryStore {
  MemoryStore({this.origin, this.username = '', Map<String, String>? prefs})
    : prefs = prefs ?? {};
  String? origin;
  String username;
  final Map<String, String> prefs;
  final calls = <MethodCall>[];
  Completer<void>? qualityWrite;
  bool failQualityWrite = false;
  bool failViewWrite = false;

  void install() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, (call) async {
          calls.add(call);
          final args = (call.arguments as Map?) ?? const {};
          switch (call.method) {
            case 'origin':
              return origin;
            case 'saveOrigin':
              origin = args['origin'] as String;
              return null;
            case 'login':
              return {'username': username, 'password': null};
            case 'saveLogin':
              username = args['username'] as String;
              return null;
            case 'preferences':
              return Map<String, String>.of(prefs);
            case 'savePreference':
              if (args['key'] == 'launcher.view' && failViewWrite) {
                throw PlatformException(code: 'storage_failed');
              }
              if (args['key'] == 'stream.quality' && qualityWrite != null) {
                await qualityWrite!.future;
                if (failQualityWrite) {
                  throw PlatformException(code: 'storage_failed');
                }
              }
              prefs[args['key'] as String] = args['value'] as String;
              return null;
            case 'appVersion':
              return '0.1.0-preview.4';
          }
          return null;
        });
  }

  List<String> get methods => [for (final call in calls) call.method];
}

Future<FakeBroker> signedIn(
  WidgetTester tester,
  List<BrokerDesktop> desktops, {
  MemoryStore? store,
}) async {
  tester.view.physicalSize = const Size(1080, 2400);
  tester.view.devicePixelRatio = 3;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  final fixture =
      store ?? MemoryStore(origin: 'https://vdi.example', username: 'tester');
  // These legacy cases exercise the card arrangement; default-view coverage is separate.
  fixture.prefs.putIfAbsent('launcher.view', () => 'cards');
  fixture.install();
  final broker = FakeBroker(desktops);
  await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
  await tester.pumpAndSettle();
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'secret-pass',
  );
  await _tapVisible(tester, find.byKey(const Key('login-submit')));
  await tester.pumpAndSettle();
  return broker;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUp(() {
    // ACK only: this fixture does not advertise native pen readiness.
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_optionalPenChannel, (_) async => null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          AndroidSessionKeepalive.channel,
          (call) async => call.method == 'start' ? true : null,
        );
  });
  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(_optionalPenChannel, null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });

  testWidgets('remembered server and ID fill sign-in; no password is stored', (
    tester,
  ) async {
    final store = MemoryStore(origin: 'https://vdi.example', username: '한글아이디');
    final broker = await signedIn(tester, [vm()], store: store);
    expect(broker.calls.first, 'login:한글아이디');
    final save = store.calls.singleWhere((call) => call.method == 'saveLogin');
    expect((save.arguments as Map)['password'], isNull);
    expect((save.arguments as Map)['remember'], false);
    expect(find.text('내 데스크톱'), findsOneWidget);
    expect(find.text('TESTER'), findsOneWidget);
    // The Windows tile's status card says it is ready.
    expect(find.byKey(const Key('status-pve-149')), findsOneWidget);
    expect(mainButton('pve-149'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  testWidgets('sign-in footer shows the running APK version', (tester) async {
    MemoryStore(origin: 'https://vdi.example', username: 'tester').install();
    await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => FakeBroker([])));
    await tester.pumpAndSettle();
    expect(find.text('vdi.example'), findsOneWidget);
    expect(
      tester
          .widget<TextField>(find.byKey(const Key('sign-in-id')))
          .controller
          ?.text,
      'tester',
    );
    expect(find.text('0.1.0-preview.4'), findsOneWidget);
    expect(find.text('Android · 스트리밍'), findsOneWidget);
    // No trademark notice.
    expect(find.textContaining('등록 상표'), findsNothing);
  });

  testWidgets('a saved server stays selected when its login-memory read fails '
      'and the old server credentials are not reused', (tester) async {
    var savedOrigin = 'https://vdi.example';
    final calls = <MethodCall>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, (call) async {
          calls.add(call);
          final args = (call.arguments as Map?) ?? const {};
          switch (call.method) {
            case 'origin':
              return savedOrigin;
            case 'appVersion':
              return '0.1.0-preview.4';
            case 'preferences':
              return <String, String>{};
            case 'saveOrigin':
              savedOrigin = args['origin'] as String;
              return null;
            case 'login':
              if (args['origin'] == 'https://other.example') {
                throw PlatformException(code: 'storage_failed');
              }
              return {'username': 'old-user', 'password': 'old-secret'};
          }
          return null;
        });
    tester.view.physicalSize = const Size(1080, 2400);
    tester.view.devicePixelRatio = 3;
    addTearDown(tester.view.reset);
    final brokers = <String, FakeBroker>{};
    await tester.pumpWidget(
      AndroidVdiApp(
        apiFactory: (origin) =>
            brokers.putIfAbsent(origin.origin, () => FakeBroker([])),
      ),
    );
    await tester.pumpAndSettle();
    TextField field(String key) =>
        tester.widget<TextField>(find.byKey(Key(key)));
    expect(field('sign-in-id').controller!.text, 'old-user');
    expect(field('sign-in-password').controller!.text, 'old-secret');
    await _tapVisible(tester, find.byKey(const Key('open-server-settings')));
    await tester.pumpAndSettle();
    final dialog = find.byType(AndroidServerDialog);
    final selected = Uri.parse('https://other.example');
    // Fake the already-tested dialog's successful save/result handoff.
    // This does not exercise HTTPS probing or contact a server.
    await tester.widget<AndroidServerDialog>(dialog).save(selected);
    Navigator.of(tester.element(dialog)).pop(selected);
    await tester.pumpAndSettle();
    expect(savedOrigin, selected.origin);
    expect(find.text('other.example'), findsWidgets);
    expect(field('sign-in-id').controller!.text, isEmpty);
    expect(field('sign-in-password').controller!.text, isEmpty);
    expect(find.textContaining('저장된 로그인 정보를 불러오지 못했어요'), findsOneWidget);
    expect(calls.where((c) => c.method == 'forgetPassword'), isEmpty);
    await tester.enterText(find.byKey(const Key('sign-in-id')), 'new-user');
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'new-secret',
    );
    await _tapVisible(tester, find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
    expect(brokers['https://other.example']!.calls, contains('login:new-user'));
    expect(
      brokers['https://vdi.example']!.calls.where(
        (c) => c.startsWith('login:'),
      ),
      isEmpty,
    );
  });

  testWidgets(
    'only explicit connect controls ask before opening; the card body stays passive',
    (tester) async {
      final broker = await signedIn(tester, [vm(ownedOutput: true)]);
      await _tapVisible(tester, find.text('TESTER'));
      expect(find.byType(AndroidRemoteView), findsNothing);
      expect(broker.calls.contains('connect'), isFalse);
      await _tapVisible(tester, mainButton('pve-149'));
      await acceptStreamConnect(tester, 'pve-149');
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(find.byType(AndroidRemoteView), findsOneWidget);
      expect(
        tester.widget<AndroidRemoteView>(find.byType(AndroidRemoteView)).name,
        'TESTER',
      );
      expect(broker.calls.where((c) => c == 'connect'), hasLength(1));
      await tester.binding.handlePopRoute();
      await tester.pumpAndSettle();
      expect(find.byType(AndroidRemoteView), findsNothing);
      await _tapVisible(tester, find.byKey(const Key('play-pve-149')));
      await acceptStreamConnect(tester, 'pve-149');
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(find.byType(AndroidRemoteView), findsOneWidget);
      expect(broker.calls.where((c) => c == 'connect'), hasLength(2));
      expect(broker.calls.where((c) => c.startsWith('power:')), isEmpty);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );

  testWidgets('power offers 종료, 다시 시작, then 정지 apart; reboot is confirmed', (
    tester,
  ) async {
    final broker = await signedIn(tester, [vm()]);
    await _tapVisible(tester, find.byKey(const Key('desktop-power-pve-149')));
    await tester.pumpAndSettle();
    expect(find.text('종료'), findsOneWidget);
    expect(find.text('다시 시작'), findsOneWidget);
    expect(find.text('정지'), findsOneWidget);
    expect(find.text('정상 종료'), findsNothing);
    expect(find.text('켜기'), findsNothing);
    expect(find.textContaining('강제'), findsNothing);
    double top(String key) => tester.getTopLeft(find.byKey(Key(key))).dy;
    expect(
      top('power-shutdown-pve-149'),
      lessThan(top('power-reboot-pve-149')),
    );
    expect(top('power-reboot-pve-149'), lessThan(top('power-stop-pve-149')));
    // 정지 stands apart, under a hairline.
    expect(
      top('power-stop-pve-149') -
          tester
              .getBottomLeft(find.byKey(const Key('power-reboot-pve-149')))
              .dy,
      greaterThan(4),
    );
    for (final key in ['shutdown', 'reboot', 'stop']) {
      expect(_enabled(tester, 'power-$key-pve-149'), true, reason: key);
    }
    await _tapVisible(tester, find.text('다시 시작'));
    await tester.pumpAndSettle();
    expect(find.text('데스크톱을 다시 시작할까요?'), findsOneWidget);
    expect(broker.calls.where((c) => c.startsWith('power:')), isEmpty);
    await _tapVisible(
      tester,
      find.byKey(const Key('confirm-power-reboot-pve-149')),
    );
    await tester.pumpAndSettle();
    expect(broker.calls.where((c) => c.startsWith('power:')), ['power:reboot']);
    expect(find.byType(SnackBar), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('a stopped VM offers 켜기 only, asking once before powering on', (
    tester,
  ) async {
    final broker = await signedIn(tester, [
      vm(power: 'stopped', stream: false),
    ]);
    expect(find.text('켜고 연결'), findsNothing);
    expect(find.text('켜기'), findsOneWidget);
    expect(find.text('꺼져 있어요'), findsOneWidget);
    await _tapVisible(tester, mainButton('pve-149'));
    await tester.pumpAndSettle();
    expect(find.text('데스크톱을 켤까요?'), findsOneWidget);
    expect(broker.calls.where((c) => c.startsWith('power:')), isEmpty);
    await _tapVisible(
      tester,
      find.byKey(const Key('cancel-power-start-pve-149')),
    );
    await tester.pumpAndSettle();
    expect(broker.calls.where((c) => c.startsWith('power:')), isEmpty);
    await _tapVisible(tester, mainButton('pve-149'));
    await tester.pumpAndSettle();
    await _tapVisible(
      tester,
      find.byKey(const Key('confirm-power-start-pve-149')),
    );
    await tester.pumpAndSettle();
    expect(broker.calls.where((c) => c.startsWith('power:')), ['power:start']);
    expect(find.byType(SnackBar), findsNothing);
    expect(find.byType(AndroidRemoteView), findsNothing);
    await _tapVisible(tester, find.byKey(const Key('desktop-power-pve-149')));
    await tester.pumpAndSettle();
    expect(find.text('켜기'), findsNWidgets(2));
    // A stopped VM cannot be restarted or shut down; the reason is shown.
    expect(find.byKey(const Key('reboot-reason-pve-149')), findsNothing);
    for (final key in ['reboot', 'shutdown', 'stop']) {
      expect(_enabled(tester, 'power-$key-pve-149'), false, reason: key);
    }
    expect(tester.takeException(), isNull);
  });

  testWidgets('정지 asks in the danger tone and sends stop', (tester) async {
    final broker = await signedIn(tester, [vm()]);
    await _tapVisible(tester, find.byKey(const Key('desktop-power-pve-149')));
    await tester.pumpAndSettle();
    await _tapVisible(tester, find.text('정지'));
    await tester.pumpAndSettle();
    expect(find.text('데스크톱을 정지할까요?'), findsOneWidget);
    expect(
      find.text('전원을 바로 끊어요. 저장하지 않은 작업이 사라질 수 있어요. Windows가 응답하지 않을 때만 쓰세요.'),
      findsOneWidget,
    );
    await _tapVisible(
      tester,
      find.byKey(const Key('cancel-power-stop-pve-149')),
    );
    await tester.pumpAndSettle();
    expect(broker.calls.where((c) => c.startsWith('power:')), isEmpty);
    await _tapVisible(tester, find.byKey(const Key('desktop-power-pve-149')));
    await tester.pumpAndSettle();
    await _tapVisible(tester, find.text('정지'));
    await tester.pumpAndSettle();
    await _tapVisible(
      tester,
      find.byKey(const Key('confirm-power-stop-pve-149')),
    );
    await tester.pumpAndSettle();
    expect(broker.calls.where((c) => c.startsWith('power:')), ['power:stop']);
    expect(find.byType(SnackBar), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('종료 asks with Proxmox wording and sends shutdown', (
    tester,
  ) async {
    final broker = await signedIn(tester, [vm()]);
    await _tapVisible(tester, find.byKey(const Key('desktop-power-pve-149')));
    await tester.pumpAndSettle();
    await _tapVisible(tester, find.text('종료'));
    await tester.pumpAndSettle();
    expect(find.text('데스크톱을 종료할까요?'), findsOneWidget);
    expect(find.textContaining('Windows를 정상적으로 끄도록 요청해요.'), findsOneWidget);
    await _tapVisible(
      tester,
      find.byKey(const Key('confirm-power-shutdown-pve-149')),
    );
    await tester.pumpAndSettle();
    expect(broker.calls.where((c) => c.startsWith('power:')), [
      'power:shutdown',
    ]);
    expect(find.byType(SnackBar), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'successful consecutive power operations never cover the launcher with notices',
    (tester) async {
      final broker = await signedIn(tester, [vm()]);
      for (final (label, key) in [('종료', 'shutdown'), ('다시 시작', 'reboot')]) {
        await _tapVisible(
          tester,
          find.byKey(const Key('desktop-power-pve-149')),
        );
        await tester.pumpAndSettle();
        await _tapVisible(tester, find.text(label));
        await tester.pumpAndSettle();
        await _tapVisible(
          tester,
          find.byKey(Key('confirm-power-$key-pve-149')),
        );
        await tester.pumpAndSettle();
      }
      expect(broker.calls.where((c) => c.startsWith('power:')), [
        'power:shutdown',
        'power:reboot',
      ]);
      expect(find.byType(SnackBar), findsNothing);
    },
  );

  testWidgets(
    'without the broker stop permission 정지 is greyed with the reason',
    (tester) async {
      final broker = await signedIn(tester, [vm(canStop: false)]);
      await _tapVisible(tester, find.byKey(const Key('desktop-power-pve-149')));
      await tester.pumpAndSettle();
      expect(_enabled(tester, 'power-stop-pve-149'), false);
      // By finger the reason is read under the label.
      expect(
        find.byKey(const Key('power-stop-pve-149-reason')),
        findsOneWidget,
      );
      expect(find.textContaining('정지를 쓸 수 없어요'), findsOneWidget);
      expect(broker.calls.where((c) => c.startsWith('power:')), isEmpty);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('⋯ and ▾ hold the parts and words of the Windows tile menu', (
    tester,
  ) async {
    await signedIn(tester, [vm()]);
    await _tapVisible(tester, find.byKey(const Key('desktop-more-pve-149')));
    await tester.pumpAndSettle();
    expect(find.text('스트리밍 설정…'), findsNothing);
    expect(find.text('연결 준비 점검'), findsOneWidget);
    expect(find.textContaining('하고 연결'), findsNothing);
    await tester.tapAt(Offset.zero);
    await tester.pumpAndSettle();
    // ▾ beside the main button: 화질 옵션, as on Windows.
    await _tapVisible(tester, find.byKey(const Key('stream-modes-pve-149')));
    await tester.pumpAndSettle();
    expect(find.text('스트리밍 설정…'), findsOneWidget);
    // Touch-sized rows.
    expect(
      tester.getSize(find.byKey(const Key('stream-settings-pve-149'))).height,
      greaterThanOrEqualTo(48),
    );
  });

  for (final changeAccount in [false, true]) {
    testWidgets(
      'late quality-save failure cannot open the old VM after '
      '${changeAccount ? 'account replacement' : 'another VM was opened and closed'}',
      (tester) async {
        final store = MemoryStore(
          origin: 'https://vdi.example',
          username: 'tester',
        );
        final broker = await signedIn(tester, [
          vm(),
          vm(id: 'pve-150', name: 'OTHER'),
        ], store: store);
        store.qualityWrite = Completer<void>();
        store.failQualityWrite = true;
        await _tapVisible(
          tester,
          find.byKey(const Key('stream-modes-pve-149')),
        );
        await tester.pumpAndSettle();
        await _tapVisible(
          tester,
          find.byKey(const Key('stream-settings-pve-149')),
        );
        await tester.pumpAndSettle();
        await _tapVisible(
          tester,
          find.descendant(
            of: find.byType(AndroidStreamDialog),
            matching: find.widgetWithText(FilledButton, '연결'),
          ),
        );
        await tester.pumpAndSettle();
        expect(find.byType(AndroidRemoteView), findsNothing);
        if (changeAccount) {
          await _tapVisible(tester, find.byKey(const Key('account-menu')));
          await tester.pumpAndSettle();
          await _tapVisible(tester, find.byKey(const Key('sign-out')));
          await tester.pumpAndSettle();
          await _tapVisible(tester, find.byKey(const Key('confirm-sign-out')));
          await tester.pumpAndSettle();
          await tester.enterText(
            find.byKey(const Key('sign-in-id')),
            'other-user',
          );
          await tester.enterText(
            find.byKey(const Key('sign-in-password')),
            'other-secret',
          );
          await _tapVisible(tester, find.byKey(const Key('login-submit')));
          await tester.pumpAndSettle();
        } else {
          await tester.ensureVisible(find.byKey(const Key('play-pve-150')));
          await _tapVisible(tester, find.byKey(const Key('play-pve-150')));
          await acceptStreamConnect(tester, 'pve-150');
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
          expect(find.byType(AndroidRemoteView), findsOneWidget);
          await tester.binding.handlePopRoute();
          await tester.pumpAndSettle();
          expect(find.byType(AndroidRemoteView), findsNothing);
        }
        final attempts = broker.calls.where((c) => c == 'connect').length;
        store.qualityWrite!.complete();
        await tester.pumpAndSettle();
        expect(find.byType(AndroidRemoteView), findsNothing);
        expect(broker.calls.where((c) => c == 'connect'), hasLength(attempts));
        expect(find.text('스트리밍 설정을 저장하지 못했어요. 이번 연결에는 적용해요.'), findsNothing);
      },
    );
  }

  test('the login hint asks for a Windows sign-in', () {
    final machine = machineFromBroker(
      BrokerDesktop(
        id: 'pve-152',
        name: 'LOGIN',
        kind: 'vm',
        description: '',
        os: 'Windows 11',
        status: 'online',
        canConnect: false,
        hostConnected: true,
        streamPreparation: StreamPreparation.login,
        power: BrokerPowerSnapshot(
          state: 'running',
          stale: false,
          checkedAt: DateTime.utc(2026, 10, 26),
        ),
      ),
    );
    final readiness = DesktopReadiness.of(machine);
    // Signed out: Console or a restart, never an in-stream sign-in.
    expect(readiness.headline, 'Windows에 로그인돼 있지 않아요');
    expect(readiness.detail, contains('Console'));
    expect(readiness.primary, ReadinessAction.console);
    expect(readiness.detail, isNot(contains('RDP')));
  });

  testWidgets('the account menu names the server with its own port', (
    tester,
  ) async {
    // A server on 8443 must not read like the one on 443.
    await signedIn(
      tester,
      [vm()],
      store: MemoryStore(
        origin: 'https://vdi.example:8443',
        username: 'tester',
      ),
    );
    await _tapVisible(tester, find.byKey(const Key('account-menu')));
    await tester.pumpAndSettle();
    expect(
      tester.widget<Text>(find.byKey(const Key('account-server'))).data,
      startsWith('vdi.example:8443'),
    );
  });

  testWidgets('cards keep one height across states', (tester) async {
    await signedIn(tester, [
      vm(id: 'a', name: 'ON'),
      vm(id: 'b', name: 'OFF', power: 'stopped', stream: false),
    ]);
    final on = tester.getSize(find.byKey(const Key('card-a')));
    final off = tester.getSize(find.byKey(const Key('card-b')));
    expect(on.height, off.height);
    expect(on.width, off.width);
  });

  testWidgets(
    'a Host away during its own update shows progress on the card, not a warning or 새로고침',
    (tester) async {
      final updating = BrokerDesktop(
        id: 'pve-247',
        name: 'WorkPC',
        kind: 'vm',
        description: '',
        os: 'Windows 11',
        status: 'online',
        canConnect: false,
        hostConnected: false,
        vmState: 'host_lost',
        power: BrokerPowerSnapshot(
          state: 'running',
          stale: false,
          checkedAt: DateTime.utc(2026, 11, 2),
        ),
        hostUpdate: HostUpdateState(
          present: true,
          operation: HostUpdateOperation(
            status: 'installing',
            targetVersion: '0.2.140',
            checkedAt: DateTime.now().toUtc(),
          ),
        ),
      );
      await signedIn(tester, [updating]);
      expect(
        tester.widget<Text>(find.byKey(const Key('status-pve-247'))).data,
        'Host 업데이트 중',
      );
      expect(find.text('설치 중이에요. 끝나면 다시 연결할 수 있어요.'), findsOneWidget);
      expect(find.text('Host 연결이 끊겼어요'), findsNothing);
      expect(find.byIcon(Icons.warning_amber_rounded), findsNothing);
      final primary = tester.widget<ButtonStyleButton>(mainButton('pve-247'));
      expect(primary.onPressed, isNull, reason: 'no 새로고침 while it installs');
    },
  );

  testWidgets('light uses the day scene and the choice is remembered', (
    tester,
  ) async {
    final store = MemoryStore(prefs: {'appearance.theme': 'light'})..install();
    await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => FakeBroker([])));
    await tester.pumpAndSettle();
    String scene() =>
        (tester
                    .widget<Image>(
                      find.byKey(const Key('scene-background-image')),
                    )
                    .image
                as AssetImage)
            .assetName;
    expect(scene(), 'assets/backgrounds/alpine-dusk-day.jpg');
    await _tapVisible(tester, find.byKey(const Key('appearance-menu')));
    await tester.pumpAndSettle();
    await _tapVisible(tester, find.byKey(const Key('theme-dark')));
    await tester.pumpAndSettle();
    expect(scene(), 'assets/backgrounds/alpine-dusk.png');
    expect(store.prefs['appearance.theme'], 'dark');
    // Appearance never touches server or sign-in memory.
    expect(store.methods.contains('saveOrigin'), false);
    expect(store.methods.contains('saveLogin'), false);
  });
}

/// A tile menu row (the Windows tile's [MenuItemButton]) can be chosen.
bool _enabled(WidgetTester tester, String key) =>
    tester.widget<MenuItemButton>(find.byKey(Key(key))).onPressed != null;

Future<void> _tapVisible(WidgetTester tester, Finder target) async {
  await tester.ensureVisible(target);
  await tester.pumpAndSettle();
  await tester.tap(target);
}

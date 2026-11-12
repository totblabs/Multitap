// 'Console 끊고 연결' on Android: one confirmation that names both acts, the
// console connections end, and the stream opens as soon as the tile is ready,
// as on Windows. It does not only end the console and leave the person on the
// list.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'android_launcher_test.dart' show FakeBroker, MemoryStore, mainButton;

const _pen = MethodChannel('multitap/pen-input');
const _id = 'pve-149';

/// The VM as the broker lists it while a console holds it (the display is
/// reserved for the console) and after the console ended.
BrokerDesktop _vm({required bool console}) => BrokerDesktop(
  id: _id,
  name: 'TESTER',
  kind: 'vm',
  description: '',
  os: 'Windows 11 · 4 vCPU',
  status: 'online',
  canConnect: !console,
  canStream: !console,
  hostConnected: true,
  consoleInUse: console,
  display: VirtualDisplayState(
    supported: true,
    activeCount: 1,
    stale: console,
    busy: console,
    checkedAt: DateTime.utc(2026, 11, 9),
  ),
  monitors: const [RemoteMonitor(id: '0', width: 1920, height: 1080)],
  power: BrokerPowerSnapshot(
    state: 'running',
    stale: false,
    checkedAt: DateTime.utc(2026, 11, 9),
  ),
  powerCanForceStop: true,
);

class _Broker extends FakeBroker implements ConsoleTakeoverBrokerApi {
  _Broker() : super([_vm(console: true)]);

  /// What ending the consoles answers; null ends them.
  Object? failure;

  @override
  Future<int> endConsoles(String token, String desktopId) async {
    calls.add('endConsoles:$desktopId');
    final error = failure;
    if (error != null) throw error;
    inventory = [_vm(console: false)];
    return 1;
  }
}

Future<_Broker> _signedIn(WidgetTester tester) async {
  tester.view.physicalSize = const Size(1080, 2400);
  tester.view.devicePixelRatio = 3;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  MemoryStore(
    origin: 'https://vdi.example',
    username: 'tester',
    prefs: {'launcher.view': 'cards'},
  ).install();
  final broker = _Broker();
  await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
  await tester.pumpAndSettle();
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'secret-pass',
  );
  await _tap(tester, find.byKey(const Key('login-submit')));
  return broker;
}

Future<void> _tap(WidgetTester tester, Finder target) async {
  await tester.ensureVisible(target);
  await tester.pumpAndSettle();
  await tester.tap(target);
  await tester.pumpAndSettle();
}

Future<void> _end(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pumpAndSettle();
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  setUp(() {
    messenger.setMockMethodCallHandler(_pen, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? true : null,
    );
  });
  tearDown(() {
    messenger.setMockMethodCallHandler(_pen, null);
    messenger.setMockMethodCallHandler(AndroidStore.channel, null);
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, null);
  });

  testWidgets(
    'the tile offers it; one confirmation ends the console and then opens the stream',
    (tester) async {
      final broker = await _signedIn(tester);
      expect(find.text('Console에서 사용 중이에요'), findsOneWidget);
      expect(find.text('Console 끊고 연결'), findsOneWidget);
      await _tap(tester, mainButton(_id));
      // One question that names both acts, in the Windows launcher's words.
      expect(find.text('Console을 끊고 연결할까요?'), findsOneWidget);
      expect(find.text('끊고 연결'), findsOneWidget);
      expect(broker.calls.where((c) => c.startsWith('endConsoles')), isEmpty);
      expect(broker.calls.contains('connect'), isFalse);
      await _tap(tester, find.byKey(const Key('confirm-end-console-$_id')));
      // The list was read again, the tile was ready, and the stream opened
      // without a second question.
      for (var i = 0; i < 5 && !broker.calls.contains('connect'); i++) {
        await tester.pump(const Duration(seconds: 1));
        await tester.pumpAndSettle();
      }
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(broker.calls.where((c) => c == 'endConsoles:$_id'), hasLength(1));
      expect(broker.calls.where((c) => c == 'connect'), hasLength(1));
      expect(
        broker.calls.indexOf('endConsoles:$_id'),
        lessThan(broker.calls.indexOf('connect')),
      );
      expect(find.byType(AndroidRemoteView), findsOneWidget);
      expect(
        tester.widget<AndroidRemoteView>(find.byType(AndroidRemoteView)).name,
        'TESTER',
      );
      expect(tester.takeException(), isNull);
      await _end(tester);
    },
  );

  testWidgets('cancel sends nothing and connects nothing', (tester) async {
    final broker = await _signedIn(tester);
    await _tap(tester, mainButton(_id));
    expect(find.text('Console을 끊고 연결할까요?'), findsOneWidget);
    await _tap(tester, find.text('취소'));
    await tester.pump(const Duration(seconds: 4));
    await tester.pumpAndSettle();
    expect(find.text('Console을 끊고 연결할까요?'), findsNothing);
    expect(broker.calls.where((c) => c.startsWith('endConsoles')), isEmpty);
    expect(broker.calls.contains('connect'), isFalse);
    expect(find.byType(AndroidRemoteView), findsNothing);
    expect(find.text('Console 끊고 연결'), findsOneWidget);
    await _end(tester);
  });

  testWidgets(
    'a console that could not be ended says so and connects nothing',
    (tester) async {
      final broker = await _signedIn(tester);
      broker.failure = StateError('fixture');
      await _tap(tester, mainButton(_id));
      await _tap(tester, find.byKey(const Key('confirm-end-console-$_id')));
      expect(find.text('Console 연결을 끊지 못했어요.'), findsOneWidget);
      await tester.pump(const Duration(seconds: 6));
      await tester.pumpAndSettle();
      expect(
        broker.calls.where((c) => c.startsWith('endConsoles')),
        hasLength(1),
      );
      expect(broker.calls.contains('connect'), isFalse);
      expect(find.byType(AndroidRemoteView), findsNothing);
      expect(find.text('Console 끊고 연결'), findsOneWidget);
      expect(tester.takeException(), isNull);
      await _end(tester);
    },
  );
}

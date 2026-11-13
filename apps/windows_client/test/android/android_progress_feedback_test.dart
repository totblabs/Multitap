// Waits on Android that used to look stopped: each one shows a turning mark
// and what it is doing while its request is pending, and the result or the
// reason takes its place when it ends. Fakes only: no server, installer or
// device.
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_drive.dart';
import 'package:multitap_client/android/android_folder_chooser.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/product_updates.dart' show UpdateFailure;
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';

import 'android_console_launcher_test.dart' as console;
import 'android_launcher_parity_test.dart' show ConsoleFake, afterRdp, pumpApp;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore, mainButton;
import 'android_updates_test.dart' as updates;

const _activity = Key('android-launcher-activity');
const _activityText = Key('android-launcher-activity-text');
const _application = MethodChannel('multitap/android/application');
const _pen = MethodChannel('multitap/pen-input');

String? _activitySentence(WidgetTester t) =>
    find.byKey(_activityText).evaluate().isEmpty
    ? null
    : t.widget<Text>(find.byKey(_activityText)).data;

/// A few frames without waiting for the turning marks to stop (they do not).
Future<void> _frames(WidgetTester t, [int count = 6]) async {
  for (var i = 0; i < count; i++) {
    await t.pump(const Duration(milliseconds: 40));
  }
}

Future<void> _end(WidgetTester t) async {
  await t.pumpWidget(const SizedBox.shrink());
  await t.pump(const Duration(seconds: 1));
  expect(t.takeException(), isNull);
}

/// The VM while a Console holds it, and after the Console ended.
BrokerDesktop _consoleVm({required bool console}) => BrokerDesktop(
  id: 'pve-149',
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

/// Ending the Consoles is answered when [gate] opens.
class _ReleaseBroker extends FakeBroker implements ConsoleTakeoverBrokerApi {
  _ReleaseBroker() : super([_consoleVm(console: true)]);
  final gate = Completer<void>();
  Object? failure;

  @override
  Future<int> endConsoles(String token, String desktopId) async {
    calls.add('endConsoles:$desktopId');
    await gate.future;
    final error = failure;
    if (error != null) throw error;
    inventory = [_consoleVm(console: false)];
    return 1;
  }
}

/// A VM left in RDP whose "is somebody connected" check and whose list read
/// are answered when the test says so.
class _SlowHandOver extends ConsoleFake
    implements StreamTakeoverPreflightBrokerApi {
  _SlowHandOver() : super([afterRdp()]);
  Completer<void>? preflight;
  Completer<List<BrokerDesktop>>? listGate;

  @override
  Future<void> checkStreamTakeover(String token, String desktopId) async {
    calls.add('preflight');
    await preflight?.future;
  }

  @override
  Future<List<BrokerDesktop>> desktops(String token) {
    final gate = listGate;
    if (gate == null) return super.desktops(token);
    listGate = null;
    calls.add('desktops');
    return gate.future;
  }
}

/// The hand-off returns when [handOff] opens and reports the bytes it copied.
class _CopyPlatform extends updates.FakePlatform
    implements AndroidUpdateInstallProgress {
  final copy = StreamController<({int copied, int total})>.broadcast();
  final handOff = Completer<void>();
  @override
  Stream<({int copied, int total})> get installProgress => copy.stream;
  @override
  Future<void> install(File file, AndroidRelease release) async {
    calls.add('install:${file.uri.pathSegments.last}');
    await handOff.future;
    if (installError != null) throw UpdateFailure(installError!);
  }
}

/// The hand-off returns when [handOff] opens; nothing is measured.
class _PlainPlatform extends updates.FakePlatform {
  final handOff = Completer<void>();
  @override
  Future<void> install(File file, AndroidRelease release) async {
    calls.add('install:${file.uri.pathSegments.last}');
    await handOff.future;
  }
}

class _SlowFolders extends AndroidDriveFileSystem {
  var next = Completer<AndroidFolderList>();
  final asked = <String?>[];
  @override
  Future<AndroidFolderList> storageFolders(String? path) {
    asked.add(path);
    return next.future;
  }
}

class _PasswordBroker extends FakeBroker {
  _PasswordBroker() : super([]);
  final gate = Completer<void>();
  @override
  Future<void> changePassword(
    String token,
    String currentPassword,
    String newPassword,
    String confirmation,
  ) => gate.future;
}

LinearProgressIndicator _bar(WidgetTester t) => t.widget<LinearProgressIndicator>(
  find.byKey(const Key('app-progress-bar')),
);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final browserCalls = <Map>[];
  Completer<void>? closeGate;
  setUp(() {
    browserCalls.clear();
    closeGate = null;
    messenger.setMockMethodCallHandler(_application, (call) async {
      if (call.method == 'closeConsoleWebViews') {
        await closeGate?.future;
        return null;
      }
      if (call.method != 'openConsoleWebView') return null;
      browserCalls.add(call.arguments as Map);
      return true;
    });
    messenger.setMockMethodCallHandler(_pen, (_) async => null);
    messenger.setMockMethodCallHandler(
      AndroidSessionKeepalive.channel,
      (call) async => call.method == 'start' ? true : null,
    );
  });
  tearDown(() {
    for (final channel in [
      _application,
      _pen,
      AndroidSessionKeepalive.channel,
      AndroidStore.channel,
    ]) {
      messenger.setMockMethodCallHandler(channel, null);
    }
  });

  group('Console 열기', () {
    testWidgets('says "Console을 여는 중이에요" on the list from the '
        'confirmation until the Console is open', (t) async {
      final broker = console.ConsoleBroker([console.desktop()]);
      await console.boot(t, broker);
      expect(find.byKey(_activity), findsNothing);
      await console.request(t);
      expect(find.byKey(_activity), findsNothing, reason: 'nothing is sent');
      final gate = Completer<List<BrokerDesktop>>();
      broker.nextRead = gate;
      await console.tap(t, 'confirm-console-pve-149');
      expect(browserCalls, isEmpty);
      expect(_activitySentence(t), 'Fixture pve-149 · Console을 여는 중이에요');
      expect(
        find.descendant(
          of: find.byKey(_activity),
          matching: find.byType(CircularProgressIndicator),
        ),
        findsOneWidget,
      );
      // No clock on a wait that has only just begun (it comes after 3 s of
      // real time: AppElapsedText's own Stopwatch, which a test cannot move).
      expect(find.byKey(const Key('app-elapsed')), findsNothing);
      gate.complete([console.desktop()]);
      await _frames(t);
      expect(browserCalls, hasLength(1));
      expect(find.byKey(_activity), findsNothing);
      await console.finish(t);
    });

    testWidgets('a refusal takes the line\'s place', (t) async {
      final broker = console.ConsoleBroker([console.desktop()]);
      await console.boot(t, broker);
      await console.request(t);
      final gate = Completer<List<BrokerDesktop>>();
      broker.nextRead = gate;
      await console.tap(t, 'confirm-console-pve-149');
      expect(find.byKey(_activity), findsOneWidget);
      const refusal = BrokerException('console_permission_denied', 403);
      gate.completeError(refusal);
      await _frames(t);
      expect(find.byKey(_activity), findsNothing);
      expect(find.text(refusal.message), findsOneWidget);
      expect(browserCalls, isEmpty);
      await console.finish(t);
    });
  });

  group('로그아웃', () {
    testWidgets('while the open Console closes the list is covered and says '
        '"로그아웃하는 중이에요"; then the sign-in screen', (t) async {
      final broker = console.ConsoleBroker([console.desktop()]);
      await console.boot(t, broker);
      await console.request(t);
      await console.tap(t, 'confirm-console-pve-149');
      expect(browserCalls, hasLength(1));
      // The native side has not confirmed the Console's window closed.
      closeGate = Completer<void>();
      await console.tap(t, 'account-menu');
      await console.tap(t, 'sign-out');
      await t.tap(find.byKey(const Key('confirm-sign-out')));
      await _frames(t, 10);
      final cover = find.byKey(const Key('android-signing-out'));
      expect(cover, findsOneWidget);
      expect(
        find.descendant(of: cover, matching: find.text('로그아웃하는 중이에요')),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: cover,
          matching: find.byType(CircularProgressIndicator),
        ),
        findsOneWidget,
      );
      // Still the list underneath, and it takes no touch.
      expect(find.byKey(const Key('sign-in-id')), findsNothing);
      expect(find.text('내 데스크톱'), findsOneWidget);
      expect(find.byKey(const Key('refresh-desktops')).hitTestable(), findsNothing);
      expect(broker.calls.contains('logout'), isFalse);
      closeGate!.complete();
      await _frames(t, 10);
      expect(cover, findsNothing);
      expect(find.byKey(const Key('sign-in-id')), findsOneWidget);
      expect(broker.calls, contains('logout'));
      await console.finish(t);
    });
  });

  group('Console 끊고 연결', () {
    Future<_ReleaseBroker> signedIn(WidgetTester t) async {
      t.view.physicalSize = const Size(1080, 2400);
      t.view.devicePixelRatio = 3;
      addTearDown(t.view.reset);
      MemoryStore(
        origin: 'https://vdi.example',
        username: 'tester',
        prefs: {'launcher.view': 'cards'},
      ).install();
      final broker = _ReleaseBroker();
      await t.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
      await t.pumpAndSettle();
      await t.enterText(
        find.byKey(const Key('sign-in-password')),
        'secret-pass',
      );
      await t.ensureVisible(find.byKey(const Key('login-submit')));
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      await t.ensureVisible(mainButton('pve-149'));
      await t.pumpAndSettle();
      await t.tap(mainButton('pve-149'));
      await t.pumpAndSettle();
      expect(find.byKey(_activity), findsNothing);
      await t.tap(find.byKey(const Key('confirm-end-console-pve-149')));
      await _frames(t, 10);
      return broker;
    }

    testWidgets('says "Console 연결을 끊는 중이에요" while the request is sent',
        (t) async {
      final broker = await signedIn(t);
      expect(broker.calls, contains('endConsoles:pve-149'));
      expect(_activitySentence(t), 'TESTER · Console 연결을 끊는 중이에요');
      expect(find.byType(AndroidRemoteView), findsNothing);
      broker.gate.complete();
      await _frames(t, 3);
      // The tile's own step line (and 취소) takes over from here.
      expect(find.byKey(_activity), findsNothing);
      expect(t.takeException(), isNull);
      await _end(t);
    });

    testWidgets('a failure takes the line\'s place', (t) async {
      final broker = await signedIn(t);
      expect(find.byKey(_activity), findsOneWidget);
      broker.failure = StateError('fixture');
      broker.gate.complete();
      await _frames(t, 3);
      expect(find.byKey(_activity), findsNothing);
      expect(find.text('Console 연결을 끊지 못했어요.'), findsOneWidget);
      expect(broker.calls.contains('connect'), isFalse);
      await t.pump(const Duration(seconds: 6));
      await _end(t);
    });
  });

  group('before the stream screen', () {
    Future<_SlowHandOver> asked(WidgetTester t, _SlowHandOver broker) async {
      await pumpApp(t, broker);
      await t.ensureVisible(mainButton('pve-149'));
      await t.pumpAndSettle();
      await t.tap(mainButton('pve-149'));
      await t.pumpAndSettle();
      expect(find.byKey(_activity), findsNothing);
      await t.tap(find.byKey(const Key('confirm-console-handover-pve-149')));
      await _frames(t, 10);
      return broker;
    }

    testWidgets('the check for another connection is said on the list, and '
        'its failure takes the line\'s place', (t) async {
      final broker = await asked(
        t,
        _SlowHandOver()..preflight = Completer<void>(),
      );
      expect(broker.calls, contains('preflight'));
      expect(_activitySentence(t), 'TESTER · 다른 연결이 있는지 확인하는 중이에요');
      expect(find.byType(AndroidRemoteView), findsNothing);
      const failure = BrokerException('display_operation_timeout');
      broker.preflight!.completeError(failure);
      await _frames(t);
      expect(find.byKey(_activity), findsNothing);
      expect(find.text(failure.message), findsOneWidget);
      expect(find.byType(AndroidRemoteView), findsNothing);
      expect(broker.transfers, 0);
      expect(t.takeException(), isNull);
      await _end(t);
    });

    testWidgets('then the fresh read of the desktop is said, and its failure '
        'takes the line\'s place', (t) async {
      final preflight = Completer<void>();
      final broker = await asked(t, _SlowHandOver()..preflight = preflight);
      final list = Completer<List<BrokerDesktop>>();
      broker.listGate = list;
      preflight.complete();
      await _frames(t);
      expect(_activitySentence(t), 'TESTER · 데스크톱 상태를 다시 확인하는 중이에요');
      const failure = BrokerException('network_error');
      list.completeError(failure);
      await _frames(t);
      expect(find.byKey(_activity), findsNothing);
      expect(find.text(failure.message), findsOneWidget);
      expect(find.byType(AndroidRemoteView), findsNothing);
      expect(broker.transfers, 0);
      await _end(t);
    });
  });

  group('APK update', () {
    Future<void> start(WidgetTester t, updates.FakePlatform platform) async {
      await updates.openDialog(t, updates.InstantUpdates(updates.origin), platform);
      await t.tap(find.byKey(const Key('android-update-install')));
      await _frames(t);
    }

    String step(WidgetTester t, String key) =>
        t.widget<Text>(find.byKey(Key(key))).data!;

    testWidgets('the hand-off fills its bar only from the bytes copied', (
      t,
    ) async {
      final platform = _CopyPlatform();
      await start(t, platform);
      // Checking the package: no measure, the bar runs back and forth.
      expect(step(t, 'android-update-installing'), '설치 파일의 서명을 확인하는 중이에요');
      expect(_bar(t).value, isNull);
      expect(find.byKey(const Key('app-progress-detail')), findsNothing);
      platform.copy.add((copied: 5 * 1024 * 1024, total: 10 * 1024 * 1024));
      await _frames(t, 2);
      expect(step(t, 'android-update-installing'), '설치를 준비하는 중이에요');
      expect(_bar(t).value, .5);
      expect(
        t.widget<Text>(find.byKey(const Key('app-progress-detail'))).data,
        '5.0 / 10.0 MB',
      );
      // Copied whole: committing has no measure again.
      platform.copy.add((copied: 10 * 1024 * 1024, total: 10 * 1024 * 1024));
      await _frames(t, 2);
      expect(step(t, 'android-update-installing'), 'Android 설치 화면을 여는 중이에요.');
      expect(_bar(t).value, isNull);
      expect(find.byKey(const Key('app-progress-detail')), findsNothing);
      // Not to be left while the native side works.
      expect(
        t
            .widget<TextButton>(find.byKey(const Key('android-update-cancel')))
            .onPressed,
        isNull,
      );
      platform.handOff.complete();
      await _frames(t, 2);
      expect(find.byKey(const Key('android-update-installing')), findsNothing);
      expect(
        find.byKey(const Key('android-update-opening-installer')),
        findsOneWidget,
      );
      await t.tap(find.byKey(const Key('android-update-close')));
      await t.pumpAndSettle();
      platform.dir?.deleteSync(recursive: true);
    });

    testWidgets('a platform that measures nothing shows no filled bar', (
      t,
    ) async {
      final platform = _PlainPlatform();
      await start(t, platform);
      expect(step(t, 'android-update-installing'), '설치를 준비하는 중이에요');
      expect(_bar(t).value, isNull);
      expect(find.byKey(const Key('app-progress-detail')), findsNothing);
      platform.handOff.complete();
      await _frames(t, 2);
      await t.tap(find.byKey(const Key('android-update-close')));
      await t.pumpAndSettle();
      platform.dir?.deleteSync(recursive: true);
    });

    testWidgets('after the hand-off the dialog follows the installer: its '
        'confirmation opens, the person comes back, and a cancel replaces '
        'the instruction', (t) async {
      final platform = _CopyPlatform();
      await start(t, platform);
      platform.handOff.complete();
      await _frames(t, 2);
      expect(
        step(t, 'android-update-opening-installer'),
        'Android 설치 화면을 여는 중이에요.',
      );
      expect(find.byKey(const Key('android-update-handed-off')), findsNothing);
      // Android's confirmation is on screen: waiting for the person.
      platform.status.add(androidUpdateConfirmOpened);
      await _frames(t, 2);
      expect(
        find.byKey(const Key('android-update-opening-installer')),
        findsNothing,
      );
      expect(
        step(t, 'android-update-waiting-confirm'),
        'Android 설치 화면의 확인을 기다리고 있어요',
      );
      expect(find.byKey(const Key('android-update-handed-off')), findsOneWidget);
      expect(find.byKey(const Key('android-update-failure')), findsNothing);
      // The confirmation covered the app and is gone again: Android installs.
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      await t.pump();
      expect(find.byKey(const Key('android-update-finishing')), findsNothing);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await _frames(t, 2);
      expect(
        step(t, 'android-update-finishing'),
        startsWith('Android가 업데이트를 설치하고 있어요.'),
      );
      expect(_bar(t).value, isNull);
      expect(find.byKey(const Key('android-update-handed-off')), findsNothing);
      // The person had cancelled there: the reason replaces every mark.
      platform.status.add('aborted');
      await _frames(t, 2);
      expect(find.byKey(const Key('android-update-finishing')), findsNothing);
      expect(
        step(t, 'android-update-failure'),
        androidUpdateMessage('aborted'),
      );
      final dialog = find.byKey(const Key('android-update-dialog'));
      expect(
        find.descendant(
          of: dialog,
          matching: find.byType(LinearProgressIndicator),
        ),
        findsNothing,
      );
      expect(
        find.descendant(
          of: dialog,
          matching: find.byType(CircularProgressIndicator),
        ),
        findsNothing,
      );
      expect(find.text('다시 시도'), findsOneWidget);
      await t.tap(find.byKey(const Key('android-update-later')));
      await t.pumpAndSettle();
      platform.dir?.deleteSync(recursive: true);
    });

    testWidgets('a failure reported while waiting for the confirmation '
        'replaces the instruction', (t) async {
      final platform = _CopyPlatform();
      await start(t, platform);
      platform.handOff.complete();
      await _frames(t, 2);
      platform.status.add(androidUpdateConfirmOpened);
      await _frames(t, 2);
      expect(find.byKey(const Key('android-update-handed-off')), findsOneWidget);
      platform.status.add('blocked');
      await _frames(t, 2);
      expect(find.byKey(const Key('android-update-handed-off')), findsNothing);
      expect(
        find.byKey(const Key('android-update-waiting-confirm')),
        findsNothing,
      );
      expect(
        step(t, 'android-update-failure'),
        androidUpdateMessage('blocked'),
      );
      await t.tap(find.byKey(const Key('android-update-later')));
      await t.pumpAndSettle();
      platform.dir?.deleteSync(recursive: true);
    });

    testWidgets('a result that arrived before the hand-off call returned is '
        'not lost', (t) async {
      final platform = _CopyPlatform();
      await start(t, platform);
      platform.status.add('installer_start_failed');
      await _frames(t, 2);
      expect(find.byKey(const Key('android-update-installing')), findsOneWidget);
      platform.handOff.complete();
      await _frames(t, 2);
      expect(
        step(t, 'android-update-failure'),
        androidUpdateMessage('installer_start_failed'),
      );
      expect(
        find.byKey(const Key('android-update-opening-installer')),
        findsNothing,
      );
      await t.tap(find.byKey(const Key('android-update-later')));
      await t.pumpAndSettle();
      platform.dir?.deleteSync(recursive: true);
    });

    test('the native channel: copied bytes of a known size, and the '
        'confirmation step', () async {
      final platform = NativeAndroidUpdatePlatform();
      addTearDown(
        () => NativeAndroidUpdatePlatform.channel.setMethodCallHandler(null),
      );
      final copies = <({int copied, int total})>[];
      final statuses = <String>[];
      platform.installProgress.listen(copies.add);
      platform.statuses.listen(statuses.add);
      Future<void> send(String method, Object? arguments) =>
          messenger.handlePlatformMessage(
            NativeAndroidUpdatePlatform.channel.name,
            const StandardMethodCodec().encodeMethodCall(
              MethodCall(method, arguments),
            ),
            (_) {},
          );
      await send('installProgress', {'copied': 1048576, 'size': 4194304});
      await send('installProgress', {'copied': 9, 'size': 0});
      await send('installProgress', {'copied': '1', 'size': 4});
      await send('installProgress', {'copied': -1, 'size': 4});
      await send('installProgress', {'copied': 9, 'size': 4});
      await send('installStatus', {'status': 'confirm_opened'});
      await Future<void>.delayed(Duration.zero);
      expect(copies, [
        (copied: 1048576, total: 4194304),
        (copied: 4, total: 4),
      ]);
      expect(statuses, [androidUpdateConfirmOpened]);
    });
  });

  group('folder chooser', () {
    AndroidFolderList volumes() => const AndroidFolderList(
      folders: [
        (path: '/storage/emulated/0', name: '내부 저장공간'),
        (path: '/storage/1234-5678', name: 'SD 카드'),
      ],
    );

    Future<_SlowFolders> open(WidgetTester t) async {
      final files = _SlowFolders();
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(body: AndroidFolderChooser(files: files)),
        ),
      );
      await t.pump();
      return files;
    }

    const reading = Key('android-folder-reading');

    testWidgets('the first read says "폴더를 읽는 중이에요" where the folders '
        'will be', (t) async {
      final files = await open(t);
      expect(t.widget<Text>(find.byKey(reading)).data, '폴더를 읽는 중이에요');
      expect(find.byKey(const Key('app-busy-line-spinner')), findsOneWidget);
      files.next.complete(volumes());
      await t.pumpAndSettle();
      expect(find.byKey(reading), findsNothing);
      expect(find.text('SD 카드'), findsOneWidget);
    });

    testWidgets('a folder that opens at once keeps the list; one that takes '
        'a while is said in its place, and a failure replaces the line', (
      t,
    ) async {
      final files = await open(t);
      files.next.complete(volumes());
      await t.pumpAndSettle();
      files.next = Completer<AndroidFolderList>();
      await t.tap(find.text('SD 카드'));
      await t.pump(const Duration(milliseconds: 100));
      expect(files.asked.last, '/storage/1234-5678');
      // Not yet: a quick folder never flashes the line.
      expect(find.byKey(reading), findsNothing);
      expect(find.text('SD 카드'), findsOneWidget);
      await t.pump(const Duration(milliseconds: 300));
      expect(find.byKey(reading), findsOneWidget);
      expect(find.text('SD 카드'), findsNothing);
      expect(find.byKey(const Key('android-folder-error')), findsNothing);
      files.next.completeError(const DriveFileException('drive_denied'));
      await t.pumpAndSettle();
      expect(find.byKey(reading), findsNothing);
      expect(find.byKey(const Key('android-folder-error')), findsOneWidget);
      // The list it came from is there again.
      expect(find.text('SD 카드'), findsOneWidget);
    });
  });

  group('dialogs with a spinner and no words', () {
    testWidgets('비밀번호 변경 says what it is doing; the reason replaces it', (
      t,
    ) async {
      final broker = _PasswordBroker();
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(
            body: AndroidPasswordDialog(api: broker, token: 'not-persisted'),
          ),
        ),
      );
      const saving = Key('android-password-saving');
      for (final (index, value) in [
        'old-password',
        'new-password-1',
        'new-password-1',
      ].indexed) {
        await t.enterText(find.byType(TextField).at(index), value);
      }
      expect(find.byKey(saving), findsNothing);
      await t.tap(find.byType(FilledButton));
      await t.pump();
      expect(t.widget<Text>(find.byKey(saving)).data, '비밀번호를 바꾸는 중이에요');
      const refusal = BrokerException('invalid_credentials', 401);
      broker.gate.completeError(refusal);
      await t.pumpAndSettle();
      expect(find.byKey(saving), findsNothing);
      expect(find.text(refusal.message), findsOneWidget);
    });

    testWidgets('연결 테스트 says what it is doing; the result replaces it', (
      t,
    ) async {
      final pending = Completer<BrokerProbeResult>();
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(
            body: AndroidServerDialog(
              initial: null,
              save: (_) async {},
              probe: (_) => pending.future,
            ),
          ),
        ),
      );
      const running = Key('server-test-running');
      await t.enterText(find.byType(TextField), 'first.example');
      expect(find.byKey(running), findsNothing);
      await t.tap(find.text('연결 테스트'));
      await t.pump();
      expect(t.widget<Text>(find.byKey(running)).data, '서버 연결을 테스트하는 중이에요');
      expect(find.byKey(const Key('server-test-result')), findsNothing);
      pending.completeError(const BrokerProbeException('timeout'));
      await t.pumpAndSettle();
      expect(find.byKey(running), findsNothing);
      expect(find.byKey(const Key('server-test-result')), findsOneWidget);
    });
  });
}

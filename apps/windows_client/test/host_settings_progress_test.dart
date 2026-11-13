// What Host settings shows while a long request runs (settings-progress-v1,
// apps/windows_host/src/settings_progress.hpp): the step the helper names,
// a bar that is filled only from a real measure, the step count of a first
// install's finish, and a mark that moves while the window waits for approval.
// Widget tests on the in-process fixture only: no Host tool, driver,
// installer or Windows setting is contacted.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_settings_api.dart';
import 'package:multitap_client/l10n/l10n.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

import 'helpers/pump_work.dart';
import 'host_prerequisites_test.dart' show FreshHost;
import 'host_settings_test.dart' show FakeCloseGuard, FakeHostSettings;

/// A Host whose helper tells what its request is doing, and whose
/// `prerequisiteStatus` read can be held open.
class TellingHost extends FreshHost implements HostSettingsProgressSource {
  TellingHost({super.registered, super.administrator, super.winfsp});
  final _told = StreamController<HostSettingsProgress>.broadcast(sync: true);
  @override
  Stream<HostSettingsProgress> get progress => _told.stream;
  void tell(String action, String step, {int? done, int? total}) =>
      _told.add(HostSettingsProgress(action, step, done: done, total: total));
  Completer<void>? prerequisitePending;

  /// `prerequisiteStatus` reads started (one that is held is counted too).
  int prerequisiteReads = 0;

  @override
  Future<Map<String, dynamic>> request(
    String action, [
    Map<String, dynamic> fields = const {},
  ]) async {
    if (action == 'prerequisiteStatus') {
      prerequisiteReads++;
      await prerequisitePending?.future;
    }
    return super.request(action, fields);
  }
}

bool _hangul(String text) => RegExp(r'[ᄀ-ᇿ㄰-㆏가-힣]').hasMatch(text);

void main() {
  Future<void> open(WidgetTester t, FakeHostSettings api) async {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = const Size(1440, 1000);
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    await t.pumpWidget(
      MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: buildAppTheme(brightness: Brightness.light),
        home: HostSettingsScreen(api: api, closeGuard: FakeCloseGuard()),
      ),
    );
    await pumpWork(t);
  }

  Future<void> tap(WidgetTester t, String key) async {
    final f = find.byKey(Key(key));
    await Scrollable.ensureVisible(t.element(f), alignment: .5);
    await pumpWork(t);
    await t.tap(f);
    await pumpWork(t);
  }

  final bar = find.byKey(const Key('host-operation-progress'));
  final count = find.byKey(const Key('app-progress-count'));
  final detail = find.byKey(const Key('app-progress-detail'));
  final result = find.byKey(const Key('host-result-dialog'));
  double? filled(WidgetTester t) =>
      t.widget<LinearProgressIndicator>(bar).value;
  String step(WidgetTester t) =>
      t.widget<Text>(find.byKey(const Key('host-operation-step'))).data!;
  String title(WidgetTester t) =>
      t.widget<Text>(find.byKey(const Key('host-operation-title'))).data!;

  // A Host with its virtual display but without WinFsp: the 장치 page offers it.
  TellingHost withoutWinfsp() =>
      TellingHost(registered: true)..displayInstalled = true;

  Future<void> startWinfsp(WidgetTester t, TellingHost api) async {
    api.devicePending = Completer<void>();
    await open(t, api);
    await t.tap(find.byKey(const Key('host-nav-6')));
    await pumpWork(t);
    await tap(t, 'host-device-drives-action');
    await t.tap(find.byKey(const Key('host-confirm')));
    await t.pump();
    await t.pump();
  }

  testWidgets(
    'WinFsp: the step the helper names, a bar filled only by its measure, gone when it ends',
    (t) async {
      final api = withoutWinfsp();
      await startWinfsp(t, api);
      // Before the helper has said anything: the request's own sentence, a
      // bar that only runs, and how long it has taken.
      expect(bar, findsOneWidget);
      expect(title(t), 'WinFsp 설치 중');
      expect(step(t), 'WinFsp를 설치하고 있어요. 창을 닫지 마세요.');
      expect(filled(t), isNull);
      expect(detail, findsNothing);
      expect(count, findsNothing); // not part of a row of steps
      expect(find.byKey(const Key('app-elapsed')), findsOneWidget);
      expect(find.textContaining('1~2분'), findsNothing);

      api.tell('installWinfsp', 'winfsp_package');
      await t.pump();
      expect(step(t), 'WinFsp 설치 파일을 확인하는 중이에요');
      expect(filled(t), isNull);

      // Windows Installer is still writing its script: no number yet.
      api.tell('installWinfsp', 'winfsp_install');
      await t.pump();
      expect(step(t), 'WinFsp를 설치하는 중이에요');
      expect(filled(t), isNull);
      expect(detail, findsNothing);

      // Its own count: the bar is that count and nothing else.
      api.tell('installWinfsp', 'winfsp_install', done: 50, total: 200);
      await t.pump();
      expect(filled(t), .25);
      expect(t.widget<Text>(detail).data, '25%');
      api.tell('installWinfsp', 'winfsp_install', done: 199, total: 200);
      await t.pump();
      expect(filled(t), closeTo(.995, 1e-9));
      expect(t.widget<Text>(detail).data, '99%'); // never rounded up to done

      // Another request's line is not this panel's.
      api.tell('installNativeDisplay', 'display_device');
      await t.pump();
      expect(step(t), 'WinFsp를 설치하는 중이에요');
      expect(filled(t), closeTo(.995, 1e-9));

      // A rollback counts down: said in words, never drawn as progress.
      api.tell('installWinfsp', 'winfsp_rollback');
      await t.pump();
      expect(step(t), 'WinFsp 설치를 되돌리는 중이에요');
      expect(filled(t), isNull);
      expect(detail, findsNothing);

      // A step this window has no sentence for: the request's own sentence,
      // and no measure is borrowed from it.
      api.tell('installWinfsp', 'some_later_step', done: 1, total: 2);
      await t.pump();
      expect(step(t), 'WinFsp를 설치하고 있어요. 창을 닫지 마세요.');
      expect(filled(t), isNull);

      // The last step has no measure: the earlier one is not kept.
      api.tell('installWinfsp', 'winfsp_install', done: 200, total: 200);
      api.tell('installWinfsp', 'winfsp_check');
      await t.pump();
      expect(step(t), '설치된 WinFsp를 확인하는 중이에요');
      expect(filled(t), isNull);

      api.devicePending!.complete();
      await pumpWork(t);
      expect(bar, findsNothing);
      expect(result, findsOneWidget);
      expect(find.textContaining('WinFsp를 설치했어요'), findsWidgets);
      // A line that arrives after the end shows nothing.
      api.tell('installWinfsp', 'winfsp_install', done: 1, total: 2);
      await t.pump();
      expect(bar, findsNothing);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('WinFsp: a failure replaces the progress', (t) async {
    final api = withoutWinfsp()
      ..winfspResult = {
        'action': 'skipped',
        'restart': false,
        'error': 'winfsp_install_failed',
      };
    await startWinfsp(t, api);
    api.tell('installWinfsp', 'winfsp_install', done: 10, total: 200);
    await t.pump();
    expect(filled(t), .05);
    api.devicePending!.complete();
    await pumpWork(t);
    expect(bar, findsNothing);
    expect(find.byKey(const Key('host-operation-step')), findsNothing);
    expect(result, findsOneWidget);
    expect(
      find.descendant(of: result, matching: find.text('결과 확인이 필요해요')),
      findsOneWidget,
    );
    expect(find.textContaining('WinFsp를 설치했어요'), findsNothing);
    expect(t.takeException(), isNull);
  });

  testWidgets(
    'The finish of a first install counts its steps, and no part of it runs without the panel',
    (t) async {
      // The service needs a moment after both installations.
      final api = TellingHost(registered: true)..settleReads = 4;
      await open(t, api);
      api.nativePending = Completer<void>();
      api.devicePending = Completer<void>();
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pump();
      await t.pump();
      // 1/3: the driver.
      expect(bar, findsOneWidget);
      expect(title(t), '가상 화면 드라이버 설치 중');
      expect(t.widget<Text>(count).data, '1/3단계');
      expect(filled(t), isNull);
      for (final (code, sentence) in [
        ('display_package', '드라이버 파일이 원본과 같은지 확인하는 중이에요'),
        ('display_trust', '드라이버 서명을 확인하는 중이에요'),
        ('display_device', '가상 화면 드라이버를 설치하는 중이에요. 화면이 잠시 깜빡일 수 있어요.'),
        ('display_service', 'Host 서비스가 가상 화면을 여는 것을 기다리는 중이에요'),
        ('display_restart', 'Host 서비스를 다시 시작하는 중이에요'),
      ]) {
        api.tell('installNativeDisplay', code);
        await t.pump();
        expect(step(t), sentence);
        expect(filled(t), isNull, reason: code); // none of them has a measure
      }

      // 2/3: WinFsp. The driver's last step is not carried over.
      api.nativePending!.complete();
      await t.pump();
      await t.pump();
      await t.pump();
      expect(bar, findsOneWidget);
      expect(title(t), 'WinFsp 설치 중');
      expect(t.widget<Text>(count).data, '2/3단계');
      expect(step(t), 'WinFsp를 설치하고 있어요. 창을 닫지 마세요.');
      api.tell('installWinfsp', 'winfsp_install', done: 3, total: 4);
      await t.pump();
      expect(filled(t), .75);

      // 3/3: the check afterwards, while the service takes the new device.
      api.devicePending!.complete();
      await t.pump();
      await t.pump();
      await t.pump(const Duration(seconds: 1));
      expect(bar, findsOneWidget);
      expect(title(t), 'Host 상태 확인 중');
      expect(step(t), 'Host 서비스가 가상 화면과 서버 연결을 준비하고 있어요.');
      expect(t.widget<Text>(count).data, '3/3단계');
      expect(filled(t), isNull); // WinFsp's measure is over with WinFsp
      expect(result, findsNothing);

      await pumpWork(t, limit: const Duration(seconds: 30));
      expect(bar, findsNothing);
      expect(count, findsNothing);
      expect(result, findsOneWidget);
      expect(find.text('설치를 마쳤어요'), findsOneWidget);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'A driver that did not install: what is left is read under the panel, never with nothing on screen',
    (t) async {
      final api = TellingHost(registered: true)
        ..nativeResult = {
          'action': 'skipped',
          'error': 'pnp_install_failed',
          'restart': false,
          'deviceReady': false,
          'certificate': 'removed',
          'service': 'unchanged',
          'rolledBack': true,
        };
      await open(t, api);
      expect(api.prerequisiteReads, 1);
      api.devicePending = Completer<void>();
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pump();
      await t.pump();
      await t.pump();
      // The driver failed at once; WinFsp is still installed (2/3).
      expect(title(t), 'WinFsp 설치 중');
      expect(t.widget<Text>(count).data, '2/3단계');
      // 3/3: the Host still has no virtual display, so what a first install
      // needs is read again (up to 15 seconds). This read used to run after
      // the panel had gone.
      api.prerequisitePending = Completer<void>();
      api.devicePending!.complete();
      await t.pump();
      await t.pump();
      await t.pump();
      expect(api.prerequisiteReads, 2); // asked, and held
      expect(bar, findsOneWidget);
      expect(title(t), '설치 결과 확인 중');
      expect(step(t), '설치한 항목을 Windows에서 다시 확인하고 있어요.');
      expect(t.widget<Text>(count).data, '3/3단계');
      expect(filled(t), isNull);
      expect(result, findsNothing);
      await t.pump(const Duration(seconds: 5));
      expect(bar, findsOneWidget); // still held, still shown
      expect(result, findsNothing);

      api.prerequisitePending!.complete();
      await pumpWork(t, limit: const Duration(seconds: 30));
      // The failure replaces the progress.
      expect(bar, findsNothing);
      expect(count, findsNothing);
      expect(result, findsOneWidget);
      expect(find.text('설치 마무리를 끝내지 못했어요'), findsOneWidget);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('With WinFsp already there the finish has two steps', (t) async {
    final api = TellingHost(registered: true, winfsp: true)..settleReads = 2;
    await open(t, api);
    api.nativePending = Completer<void>();
    await t.tap(find.byKey(const Key('host-confirm')));
    await t.pump();
    await t.pump();
    expect(t.widget<Text>(count).data, '1/2단계');
    api.nativePending!.complete();
    await t.pump();
    await t.pump();
    await t.pump(const Duration(seconds: 1));
    // The service needs a moment: the wait is the last step and says so.
    expect(bar, findsOneWidget);
    expect(title(t), 'Host 상태 확인 중');
    expect(step(t), 'Host 서비스가 가상 화면과 서버 연결을 준비하고 있어요.');
    expect(t.widget<Text>(count).data, '2/2단계');
    expect(api.of('installWinfsp'), isEmpty);
    await pumpWork(t, limit: const Duration(seconds: 30));
    expect(bar, findsNothing);
    expect(result, findsOneWidget);
    expect(t.takeException(), isNull);
  });

  testWidgets('A request that goes to the server says it waits for the server', (
    t,
  ) async {
    final api = FakeHostSettings(registered: true)..pending = Completer<void>();
    await open(t, api);
    await t.tap(find.byKey(const Key('host-nav-4')));
    await pumpWork(t);
    await tap(t, 'host-service');
    await t.tap(find.byKey(const Key('host-confirm')));
    await t.pump();
    await t.pump(const Duration(milliseconds: 350));
    expect(bar, findsOneWidget);
    expect(title(t), '서비스 다시 시작 중');
    expect(step(t), 'Host 서비스를 멈췄다가 다시 시작하고 있어요. 창을 닫지 마세요.');
    expect(filled(t), isNull);
    api.pending!.complete();
    await pumpWork(t);
    expect(bar, findsNothing);
    expect(t.takeException(), isNull);
  });

  testWidgets(
    'Waiting for approval shows a mark that moves, only while the window is really asking',
    (t) async {
      final waiting = find.byKey(const Key('host-approval-waiting'));
      Map<String, dynamic> request(String status) => {
        'status': status,
        'origin': 'https://cloud.example.com',
        'requestId': 'fixture-pending',
        'expiresAt': DateTime.now()
            .add(const Duration(minutes: 10))
            .toIso8601String(),
      };
      final api = FakeHostSettings()..connectionRequest = request('pending');
      await open(t, api);
      await tap(t, 'host-open-request');
      expect(find.text('관리자 승인 대기'), findsOneWidget);
      expect(waiting, findsOneWidget);
      expect(
        find.descendant(
          of: waiting,
          matching: find.text('승인을 기다리는 중이에요. 승인되면 바로 다음으로 넘어가요.'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: waiting,
          matching: find.byType(CircularProgressIndicator),
        ),
        findsOneWidget,
      );
      // A status read that fails: the error stands in its place.
      api.failStatus = true;
      await t.pump(const Duration(milliseconds: 3010));
      await pumpWork(t);
      expect(find.byKey(const Key('host-approval-status-error')), findsOneWidget);
      expect(waiting, findsNothing);
      // Reads work again: it is waiting again.
      api.failStatus = false;
      await t.pump(const Duration(milliseconds: 3010));
      await pumpWork(t);
      expect(waiting, findsOneWidget);
      // Approved: the wait for a person is over.
      api.connectionRequest = request('approved');
      await t.pump(const Duration(milliseconds: 3010));
      await pumpWork(t);
      expect(waiting, findsNothing);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('English: every step has its sentence and none keeps Korean', (
    t,
  ) async {
    AppLanguageController.debugSystemLanguage = 'en';
    addTearDown(() => AppLanguageController.debugSystemLanguage = 'ko');
    final api = withoutWinfsp();
    await startWinfsp(t, api);
    expect(title(t), 'Installing WinFsp');
    expect(_hangul(step(t)), isFalse);
    final seen = <String>{step(t)};
    for (final code in [
      'display_package',
      'display_trust',
      'display_device',
      'display_service',
      'display_restart',
      'winfsp_package',
      'winfsp_install',
      'winfsp_rollback',
      'winfsp_waiting',
      'winfsp_check',
      'usb_remove',
      'usb_install',
    ]) {
      api.tell('installWinfsp', code);
      await t.pump();
      expect(_hangul(step(t)), isFalse, reason: code);
      expect(seen.add(step(t)), isTrue, reason: '$code has its own sentence');
    }
    api.tell('installWinfsp', 'winfsp_install', done: 1, total: 4);
    await t.pump();
    expect(t.widget<Text>(detail).data, '25%');
    expect(
      _hangul(t.widget<Text>(find.byKey(const Key('app-elapsed'))).data!),
      isFalse,
    );
    api.devicePending!.complete();
    await pumpWork(t);
    expect(bar, findsNothing);
    expect(t.takeException(), isNull);
  });
}

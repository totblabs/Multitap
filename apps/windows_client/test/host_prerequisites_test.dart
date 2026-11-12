// host-prerequisites-v1: Host settings finishes a first install (virtual
// display driver after one confirmation, then WinFsp). The Windows Installer
// package alone installs neither one: on a clean Windows it leaves a running,
// registered Host and no virtual display.
// Widget tests on the in-process fixture only: no Host tool, driver,
// certificate store or Windows setting is contacted.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_prerequisites.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

import 'host_settings_test.dart'
    show FakeCloseGuard, FakeHostSettings, isReadOnlyHostRequest;

Map<String, dynamic> prerequisites({
  String state = 'install',
  String reason = '',
  bool trusted = false,
  bool remote = false,
}) => {
  'schema': 1,
  'administrator': true,
  'remoteSession': remote,
  'display': {
    'state': state,
    'reason': reason,
    'package': 'intact',
    'certificate': trusted ? 'trusted' : 'untrusted',
    'selection': 'none',
    'device': 'none',
  },
  'winfsp': {'installed': false, 'package': true},
  'restartRequired': state == 'restart',
};

/// A Host as the installer alone leaves a clean Windows: service running, no
/// virtual display of either kind, no WinFsp. A successful
/// `installNativeDisplay` changes what the status reports, as the real Host.
class FreshHost extends FakeHostSettings {
  FreshHost({super.registered, super.administrator, bool winfsp = false}) {
    devices = {
      'schema': 1,
      'drives': {'winfsp': winfsp, 'package': true, 'webClient': 'usable'},
      'usb': {'supported': true, 'package': true, 'controller': 'none'},
      'printers': {'reason': '', 'spooler': 'running'},
      'audio': {'output': 'present'},
    };
  }
  bool displayInstalled = false;
  Map<String, dynamic> observed = prerequisites();
  // As a real Host answers: the running service took the device, no restart.
  Map<String, dynamic> nativeResult = {
    'action': 'installed',
    'error': '',
    'restart': false,
    'deviceReady': true,
    'certificate': 'added',
    'service': 'adopted',
    'rolledBack': false,
  };
  Completer<void>? nativePending;

  /// How many status reads after the install still show the service busy
  /// (display not ready yet, server connection coming back).
  int settleReads = 0;
  int readsAfterInstall = 0;

  @override
  Future<Map<String, dynamic>> request(
    String action, [
    Map<String, dynamic> fields = const {},
  ]) async {
    if (action == 'prerequisiteStatus') {
      requests.add({'action': action, ...fields});
      return {'ok': true, 'prerequisites': observed};
    }
    if (action == 'installNativeDisplay') {
      requests.add({'action': action, ...fields});
      await nativePending?.future;
      if (nativeResult['error'] == '') displayInstalled = true;
      return {
        'ok': true,
        'nativeDisplay': nativeResult,
        'capabilitiesRefreshRequested': nativeResult['error'] == '',
      };
    }
    final reply = await super.request(action, fields);
    final status = reply['status'];
    if (status is Map<String, dynamic>) {
      final busy = displayInstalled && ++readsAfterInstall <= settleReads;
      if (registered) {
        status['broker'] = busy
            ? {'state': 'reconnecting', 'online': false}
            : {'state': 'online', 'online': true, 'heartbeatAgeSeconds': 0};
      }
      status['display'] = displayInstalled
          ? {
              'provider': 'native',
              'installed': true,
              'deviceReady': true,
              'owned': true,
              'ready': !busy,
              'activeDisplay': !busy,
              'remoteSession': false,
              'configurationPending': busy,
              'recoveryRequired': false,
              'rebootRequired': nativeResult['restart'] == true,
              'foreignDeviceCount': 0,
              'error': busy ? 'native_display_activation_pending' : '',
              'monitors': [
                if (!busy) ...[
                  {'id': '0', 'width': 1920, 'height': 1080},
                  {'id': '1', 'width': 1920, 'height': 1080},
                ],
              ],
            }
          : {
              'provider': 'legacy',
              'installed': false,
              'deviceReady': false,
              'owned': false,
              'ready': false,
              'activeDisplay': false,
              'remoteSession': false,
              'configurationPending': false,
              'recoveryRequired': false,
              'rebootRequired': false,
              'foreignDeviceCount': 0,
              'error': '',
              'monitors': [],
            };
    }
    return reply;
  }

  Iterable<Map<String, dynamic>> of(String action) =>
      requests.where((r) => r['action'] == action);
}

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
    await t.pumpAndSettle();
  }

  final dialog = find.byKey(const Key('host-confirm-dialog'));
  final card = find.byKey(const Key('host-finish-card'));
  final result = find.byKey(const Key('host-result-dialog'));

  test('The status reply is read strictly; anything else offers nothing', () {
    final ready = HostPrerequisites.parse(prerequisites(trusted: true))!;
    expect(ready.displayInstallable, isTrue);
    expect(ready.certificateTrusted, isTrue);
    expect(ready.attention, isNull);
    for (final bad in [
      null,
      <String, dynamic>{},
      {'schema': 2, 'display': {'state': 'install'}},
      {'schema': 1},
      {'schema': 1, 'display': {'state': 'unknown'}},
      {'schema': 1, 'display': 'install'},
    ]) {
      expect(HostPrerequisites.parse(bad), isNull);
    }
    final changed = HostPrerequisites.parse(
      prerequisites(
        state: 'attention',
        reason: 'native_display_package_mismatch',
      ),
    )!;
    expect(changed.displayInstallable, isFalse);
    expect(changed.attention, contains('Host 설치 파일을 다시 받아 실행하세요'));
    expect(
      HostPrerequisites.parse(
        prerequisites(state: 'attention', reason: r'C:\Users\name'),
      )!.reason,
      isEmpty,
    );
  });

  test('The one confirmation names the certificate only when it adds trust', () {
    final trust = hostFinishConfirmation(trust: true, winfsp: true);
    expect(trust.title, '가상 화면 드라이버의 서명을 신뢰할까요?');
    expect(trust.accept, '신뢰하고 설치');
    expect(trust.message, contains('Sunshine이 서명한 원본 드라이버'));
    expect(trust.message, contains('Root·TrustedPublisher'));
    expect(trust.message, contains('WinFsp'));
    expect('\n'.allMatches(trust.message).length, 2);
    final plain = hostFinishConfirmation(trust: false, winfsp: false);
    expect(plain.accept, '설치');
    expect(plain.message, isNot(contains('신뢰 저장소')));
    expect(plain.message, isNot(contains('WinFsp')));
  });

  test('Every install result is two or three plain lines with one next step', () {
    Map<String, dynamic> reply(Map<String, dynamic> value) => {
      'ok': true,
      'nativeDisplay': {
        'action': 'installed',
        'error': '',
        'restart': false,
        'service': 'restarted',
        ...value,
      },
    };
    final done = hostNativeDisplayInstallResult(reply({}));
    expect(done.ok, isTrue);
    expect(done.message, '가상 화면 드라이버를 설치했어요.');
    // The running service took the device by itself: nothing about a restart.
    for (final service in ['adopted', 'restarted', 'unchanged']) {
      expect(
        hostNativeDisplayInstallResult(reply({'service': service})).message,
        '가상 화면 드라이버를 설치했어요.',
        reason: service,
      );
    }
    final restart = hostNativeDisplayInstallResult(reply({'restart': true}));
    expect(restart.ok, isTrue);
    expect(restart.message, contains('자동으로 다시 시작하지 않아요'));
    final rdp = hostNativeDisplayInstallResult(reply({}), remoteSession: true);
    expect(rdp.message, contains('Proxmox 콘솔(로컬 PC는 본체 화면)에서 확인하세요'));
    expect(
      hostNativeDisplayInstallResult(reply({'action': 'kept'})).message,
      '가상 화면 드라이버가 이미 설치돼 있어요.',
    );
    expect(
      hostNativeDisplayInstallResult(
        reply({'service': 'unverified'}),
      ).message,
      contains('서비스를 다시 시작하세요'),
    );
    for (final code in [
      'certificate_trust_required',
      'native_display_package_mismatch',
      'certificate_store_failed',
      'native_display_catalog_untrusted',
      'native_selection_invalid',
      'native_display_install_busy',
      'native_selection_write_failed',
      'pnp_install_failed',
      'native_display_binding_mismatch',
    ]) {
      final failed = hostNativeDisplayInstallResult(reply({'error': code}));
      expect(failed.ok, isFalse, reason: code);
      expect(failed.message, contains('(오류 코드: $code)'), reason: code);
      expect('\n'.allMatches(failed.message).length, 1, reason: code);
    }
    // Free text never reaches the screen; a malformed reply is not a success.
    final leaked = hostNativeDisplayInstallResult(
      reply({'error': r'C:\Users\name failed'}),
    );
    expect(leaked.ok, isFalse);
    expect(leaked.message, isNot(contains('Users')));
    expect(hostNativeDisplayInstallResult({'ok': true}).ok, isFalse);
    // The summary keeps the driver's lines and one line for a WinFsp success.
    final both = hostFinishSummary(
      display: done,
      winfsp: (ok: true, message: 'WinFsp를 설치했어요.\n다음 연결부터 큰 파일도 바로 열 수 있어요.'),
    );
    expect(both.ok, isTrue);
    expect(both.message, '가상 화면 드라이버를 설치했어요.\nWinFsp를 설치했어요.');
    final partial = hostFinishSummary(
      display: done,
      winfsp: (ok: false, message: '설치에 실패했어요.\n다시 시도하세요.'),
    );
    expect(partial.ok, isFalse);
    expect(partial.message, contains('설치에 실패했어요.\n다시 시도하세요.'));
    // WinFsp was there before the run: said, so the result covers both parts.
    expect(
      hostFinishSummary(display: done, winfspPresent: true).message,
      '가상 화면 드라이버를 설치했어요.\nWinFsp는 이미 설치돼 있었어요.',
    );
    expect(
      hostFinishSummary(display: done).message,
      '가상 화면 드라이버를 설치했어요.',
    );
  });

  testWidgets(
    'A first install is finished after one confirmation: driver, then WinFsp',
    (t) async {
      final api = FreshHost();
      await open(t, api);
      // Asked at once, before anything is changed.
      expect(dialog, findsOneWidget);
      expect(find.text('가상 화면 드라이버의 서명을 신뢰할까요?'), findsOneWidget);
      expect(find.text('신뢰하고 설치'), findsOneWidget);
      expect(api.of('prerequisiteStatus'), hasLength(1));
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);

      api.nativePending = Completer<void>();
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pump();
      await t.pump();
      expect(find.byKey(const Key('host-operation-progress')), findsOneWidget);
      expect(find.text('가상 화면 드라이버 설치 중'), findsOneWidget);
      expect(api.of('installNativeDisplay').single, {
        'action': 'installNativeDisplay',
        'confirmed': true,
        'trustConfirmed': true,
      });
      expect(api.of('installWinfsp'), isEmpty);

      api.nativePending!.complete();
      await t.pumpAndSettle();
      expect(api.of('installWinfsp').single, {
        'action': 'installWinfsp',
        'confirmed': true,
      });
      final order = [
        for (final r in api.requests)
          if (r['action'] != 'status' && r['action'] != 'prerequisiteStatus')
            r['action'],
      ];
      expect(order, ['installNativeDisplay', 'installWinfsp']);
      expect(result, findsOneWidget);
      expect(find.text('설치를 마쳤어요'), findsOneWidget);
      expect(
        find.descendant(
          of: result,
          matching: find.text('가상 화면 드라이버를 설치했어요.\nWinFsp를 설치했어요.'),
        ),
        findsOneWidget,
      );
      await t.tap(find.byKey(const Key('host-result-close')));
      await t.pumpAndSettle();
      // Nothing is left to finish, and nothing asks again.
      expect(card, findsNothing);
      expect(dialog, findsNothing);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Declining changes nothing and leaves one card with one button',
    (t) async {
      final api = FreshHost();
      await open(t, api);
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(dialog, findsNothing);
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      expect(card, findsOneWidget);
      expect(find.text('설치 마무리 필요'), findsOneWidget);
      expect(
        find.text(
          '가상 화면 드라이버와 공유 폴더 구성 요소(WinFsp)가 아직 없어요.\n설치해야 이 데스크톱에 연결할 수 있어요.',
        ),
        findsOneWidget,
      );
      // A refresh does not ask again by itself; the card's button does.
      await t.tap(find.byKey(const Key('host-refresh')));
      await t.pumpAndSettle();
      expect(dialog, findsNothing);
      expect(card, findsOneWidget);
      await t.tap(find.byKey(const Key('host-finish-install')));
      await t.pumpAndSettle();
      expect(dialog, findsOneWidget);
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(api.of('installNativeDisplay'), isEmpty);
      expect(api.of('installWinfsp'), isEmpty);
      // The virtual screen page offers the same finish, not the older driver.
      await t.tap(find.byKey(const Key('host-nav-2')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('host-display-finish')), findsOneWidget);
      expect(find.byKey(const Key('host-display-setup')), findsNothing);
      expect(api.of('display'), isEmpty);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'A certificate trusted earlier is not asked for again or re-confirmed',
    (t) async {
      final api = FreshHost(winfsp: true)
        ..observed = prerequisites(trusted: true);
      await open(t, api);
      expect(find.text('가상 화면 드라이버를 설치할까요?'), findsOneWidget);
      expect(find.textContaining('신뢰 저장소'), findsNothing);
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pumpAndSettle();
      expect(api.of('installNativeDisplay').single, {
        'action': 'installNativeDisplay',
        'confirmed': true,
      });
      expect(api.of('installWinfsp'), isEmpty);
      expect(
        find.descendant(
          of: result,
          matching: find.text('가상 화면 드라이버를 설치했어요.\nWinFsp는 이미 설치돼 있었어요.'),
        ),
        findsOneWidget,
      );
    },
  );

  testWidgets(
    'A failed driver install says why, still installs WinFsp, keeps the card',
    (t) async {
      final api = FreshHost()
        ..nativeResult = {
          'action': 'skipped',
          'error': 'pnp_install_failed',
          'restart': false,
          'deviceReady': false,
          'certificate': 'added',
          'service': 'unchanged',
          'rolledBack': false,
        };
      await open(t, api);
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pumpAndSettle();
      expect(find.text('설치 마무리를 끝내지 못했어요'), findsOneWidget);
      expect(
        find.descendant(
          of: result,
          matching: find.textContaining('(오류 코드: pnp_install_failed)'),
        ),
        findsOneWidget,
      );
      expect(api.of('installWinfsp'), hasLength(1));
      await t.tap(find.byKey(const Key('host-result-close')));
      await t.pumpAndSettle();
      expect(card, findsOneWidget);
      expect(
        find.text('가상 화면 드라이버가 아직 없어요.\n설치해야 이 데스크톱에 연결할 수 있어요.'),
        findsOneWidget,
      );
      expect(find.byKey(const Key('host-finish-install')), findsOneWidget);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'A restart Windows asked for is said plainly and never performed',
    (t) async {
      final api = FreshHost(winfsp: true)
        ..nativeResult = {
          'action': 'installed',
          'error': '',
          'restart': true,
          'deviceReady': true,
          'certificate': 'added',
          'service': 'restarted',
          'rolledBack': false,
        };
      await open(t, api);
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pumpAndSettle();
      expect(
        find.descendant(
          of: result,
          matching: find.textContaining('자동으로 다시 시작하지 않아요'),
        ),
        findsOneWidget,
      );
      expect(
        api.requests.map((r) => r['action']).toSet(),
        {'status', 'prerequisiteStatus', 'installNativeDisplay'},
      );
    },
  );

  testWidgets(
    'A driver package that cannot be verified is explained, never installed',
    (t) async {
      final api = FreshHost(winfsp: true)
        ..observed = prerequisites(
          state: 'attention',
          reason: 'native_display_package_mismatch',
        );
      await open(t, api);
      expect(dialog, findsNothing);
      expect(card, findsOneWidget);
      expect(
        find.text('가상 화면 드라이버 파일을 확인하지 못했어요.\nHost 설치 파일을 다시 받아 실행하세요.'),
        findsOneWidget,
      );
      expect(find.byKey(const Key('host-finish-install')), findsNothing);
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
    },
  );

  testWidgets(
    'A Host whose display works is never asked; missing WinFsp is one card',
    (t) async {
      // The ordinary fixture: a virtual display is installed.
      final healthy = FakeHostSettings(registered: true)..displays = 1;
      await open(t, healthy);
      expect(dialog, findsNothing);
      expect(card, findsNothing);
      expect(healthy.requests.map((r) => r['action']).toSet(), {'status'});
      await t.pumpWidget(const SizedBox.shrink());

      // Everything a stream needs is there (the server answers too), so the
      // missing WinFsp is the one thing the overview has to say.
      final api = FakeHostSettings(registered: true)
        ..displays = 1
        ..statusOverrides = {
          'broker': {'state': 'online', 'online': true, 'heartbeatAgeSeconds': 0},
        }
        ..devices = {
          'schema': 1,
          'drives': {'winfsp': false, 'package': true, 'webClient': 'usable'},
          'usb': {'supported': true, 'package': true, 'controller': 'started'},
          'printers': {'reason': '', 'spooler': 'running'},
          'audio': {'output': 'present'},
        };
      await open(t, api);
      expect(dialog, findsNothing);
      expect(card, findsOneWidget);
      expect(find.text('설치 마무리 필요'), findsOneWidget);
      expect(
        find.text('공유 폴더 구성 요소(WinFsp)가 아직 없어요.\n설치하면 큰 파일도 바로 열 수 있어요.'),
        findsOneWidget,
      );
      // No extra helper process for a Host that has its display.
      expect(api.requests.map((r) => r['action']).toSet(), {'status'});
      await t.tap(find.byKey(const Key('host-finish-install')));
      await t.pumpAndSettle();
      expect(find.text('WinFsp를 설치할까요?'), findsOneWidget);
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pumpAndSettle();
      expect(
        api.requests.where((r) => r['action'] == 'installWinfsp').single,
        {'action': 'installWinfsp', 'confirmed': true},
      );
      await t.tap(find.byKey(const Key('host-result-close')));
      await t.pumpAndSettle();
      expect(card, findsNothing);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'After the finish the status is read again until the service has settled',
    (t) async {
      // A registered Host whose service needs a moment after the install: the
      // first reads show the display pending and the server connection coming
      // back (what one read right after the step would leave on the rail).
      final api = FreshHost(registered: true, winfsp: true)..settleReads = 3;
      await open(t, api);
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pump();
      await t.pump();
      // While it settles the window says so and shows no result yet.
      await t.pump(const Duration(seconds: 1));
      expect(find.text('Host 상태 확인 중'), findsOneWidget);
      expect(result, findsNothing);
      await t.pumpAndSettle();
      expect(api.readsAfterInstall, greaterThan(api.settleReads));
      expect(result, findsOneWidget);
      expect(find.text('설치를 마쳤어요'), findsOneWidget);
      expect(
        find.descendant(of: result, matching: find.textContaining('다시 시작')),
        findsNothing,
      );
      await t.tap(find.byKey(const Key('host-result-close')));
      await t.pumpAndSettle();
      // The areas show the settled state, not the read taken in between.
      expect(find.text('스트리밍 가능'), findsOneWidget);
      expect(
        t.widget<Text>(find.byKey(const Key('host-broker-state'))).data,
        'cloud.example.com · 방금 확인',
      );
      expect(find.text('가상 모니터 2대 · 준비됨'), findsOneWidget);
      expect(find.textContaining('서버 다시 연결 중'), findsNothing);
      expect(card, findsNothing);
      expect(api.requests.where((r) => r['action'] == 'restartServiceV1'), isEmpty);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'A service that never settles does not hold the result back for long',
    (t) async {
      final api = FreshHost(registered: true, winfsp: true)
        ..settleReads = 1000;
      await open(t, api);
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pumpAndSettle();
      // Ten reads two seconds apart, then the result with what is known.
      expect(result, findsOneWidget);
      expect(api.readsAfterInstall, lessThanOrEqualTo(12));
      await t.tap(find.byKey(const Key('host-result-close')));
      await t.pumpAndSettle();
      expect(find.text('서버 다시 연결 중'), findsWidgets);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('Without administrator rights nothing is read or offered', (
    t,
  ) async {
    final api = FreshHost(administrator: false);
    await open(t, api);
    expect(dialog, findsNothing);
    expect(card, findsNothing);
    expect(api.of('prerequisiteStatus'), isEmpty);
    expect(api.requests.every(isReadOnlyHostRequest), isTrue);
  });

  testWidgets('An older Host tool without the status offers nothing', (
    t,
  ) async {
    // The stock fixture answers an unknown action with a plain status.
    final api = FakeHostSettings()
      ..statusOverrides = {
        'display': {'provider': 'legacy', 'installed': false, 'monitors': []},
      };
    await open(t, api);
    expect(dialog, findsNothing);
    expect(card, findsNothing);
    expect(api.requests.every(isReadOnlyHostRequest), isTrue);
    expect(t.takeException(), isNull);
  });
}

// The Host settings overview (개요): its readiness said ONCE in plain words
// beside the desktop's name (no route picture, no "정상" pill or row
// words), ONE banner with ONE action for the most important problem,
// and plain rows. Every value comes from the status the window already
// reads (FakeHostSettings).
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/host_overview.dart';
import 'package:multitap_client/ui/host_overview_panels.dart';
import 'package:multitap_client/ui/host_settings_navigation.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';
import 'package:multitap_client/ui/themed_app.dart';

import 'host_prerequisites_test.dart' show FreshHost;
import 'helpers/pump_work.dart';
import 'host_settings_test.dart'
    show FakeCloseGuard, FakeHostSettings, isReadOnlyHostRequest;

const _online = {
  'state': 'online',
  'online': true,
  'heartbeatAgeSeconds': 1,
  'lastReadyAt': 1792700400000,
  'lastHeartbeatAt': 1792700400000,
  'failureReason': '',
};

Map<String, dynamic> _autologon(String state, {String account = 'ToTB'}) => {
  'schema': 1,
  'supported': true,
  'administrator': true,
  'state': state,
  'reason': 'none',
  'revision': List.filled(64, 'a').join(),
  'accountLabel': state == 'off' ? '' : account,
  'enabled': state != 'off',
  'loggedIn': true,
};

// The desktop of the mockup: the VM "Code-agent" (pve-173) on vdi.example.com,
// signed in as ToTB with automatic login, its virtual screen ready.
Map<String, dynamic> _desktop() => {
  'version': '1.0.51',
  'desktopName': 'Code-agent',
  'desktopId': 'pve-173',
  'serverOrigin': 'https://vdi.example.com',
  'broker': _online,
  'windowsAutologon': _autologon('observed'),
};

FakeHostSettings _ready([Map<String, dynamic> more = const {}]) =>
    FakeHostSettings(registered: true)
      ..displays = 1
      ..credentialsConfigured = true
      ..credentialUsername = 'ToTB'
      ..statusOverrides = {..._desktop(), ...more};

const _ok = HostRouteState.ok, _warn = HostRouteState.warn;

void main() {
  Future<void> show(
    WidgetTester tester,
    FakeHostSettings api, {
    Size size = const Size(680, 760),
    double textScale = 1,
    ThemeMode mode = ThemeMode.dark,
  }) async {
    tester.view.physicalSize = size;
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final appearance = AppAppearance(mode: mode);
    addTearDown(appearance.dispose);
    await tester.pumpWidget(
      ThemedApp(
        key: UniqueKey(),
        appearance: appearance,
        home: MediaQuery(
          data: MediaQueryData(
            size: size,
            textScaler: TextScaler.linear(textScale),
          ),
          child: HostSettingsScreen(api: api, closeGuard: FakeCloseGuard()),
        ),
      ),
    );
    await pumpWork(tester);
  }

  // The readiness beside the desktop's name: plain words, said once.
  HostReadiness pill(WidgetTester t) =>
      t.widget<HostReadiness>(find.byKey(const Key('host-overview-state')));

  String text(WidgetTester t, String key) =>
      t.widget<Text>(find.byKey(Key(key))).data!;
  Set<int> dots(WidgetTester t) => {
    for (final page in [0, 1, 2, 4, 5, 6])
      if (find.byKey(Key('host-nav-dot-$page')).evaluate().isNotEmpty) page,
  };
  HostSetupState row(WidgetTester t, String key) => t
      .widget<HostAreaRow>(find.byKey(Key(key)))
      .state;

  testWidgets('All normal: the approved first screen from the real status', (
    t,
  ) async {
    final api = _ready();
    await show(t, api);
    expect(text(t, 'host-connection-heading'), 'Code-agent');
    expect(pill(t).label, '스트리밍 가능');
    expect(pill(t).state, _ok);
    expect(text(t, 'host-machine-line'), 'Proxmox VM · pve-173');
    // Readiness is said once, in words, beside the name: no
    // route picture, no pill, no "정상" on every row.
    expect(find.text('스트리밍 가능'), findsOneWidget);
    expect(find.byKey(const Key('host-route')), findsNothing);
    expect(find.byKey(const Key('host-route-ring-0')), findsNothing);
    expect(find.text('클라이언트'), findsNothing);
    expect(find.byType(HostStatusPill), findsNothing);
    expect(find.text('정상'), findsNothing);
    // Nothing is wrong: no banner, no dot in the navigation.
    expect(find.byType(HostProblemBanner), findsNothing);
    expect(find.byType(FilledButton), findsNothing);
    expect(dots(t), isEmpty);
    // 구성 요소: plain rows; a fine area carries no state word.
    for (final (key, detail) in [
      ('host-open-windows', 'ToTB 계정 · 자동 로그인 켜짐'),
      ('host-open-display', '가상 모니터 1대 · 준비됨'),
      ('host-open-devices', '공유 폴더·USB·프린터·사운드카드 준비됨'),
      ('host-open-server', 'vdi.example.com · 방금 확인'),
    ]) {
      expect(row(t, key), HostSetupState.complete, reason: key);
      expect(
        find.descendant(of: find.byKey(Key(key)), matching: find.text(detail)),
        findsOneWidget,
        reason: key,
      );
      expect(
        find.descendant(
          of: find.byKey(Key(key)),
          matching: find.byType(HostSetupBadge),
        ),
        findsNothing,
        reason: key,
      );
    }
    expect(find.byKey(const Key('host-overview-footnote')), findsOneWidget);
    // The header: product, version, quiet icons.
    expect(text(t, 'host-version'), '1.0.51');
    expect(api.requests.every(isReadOnlyHostRequest), isTrue);
    expect(t.takeException(), isNull);
  });

  testWidgets('Windows signed out: one problem, one sentence, one action', (
    t,
  ) async {
    final api = _ready({'interactiveSession': false});
    await show(t, api);
    expect(pill(t).label, 'Windows 로그인 필요');
    expect(pill(t).state, _warn);
    expect(find.text('스트리밍 가능'), findsNothing);
    expect(find.byType(HostProblemBanner), findsOneWidget);
    expect(
      text(t, 'host-problem-message'),
      'Windows에 로그인돼 있지 않거나 잠겨 있어요. '
      '클라이언트의 Console로 로그인하거나 데스크톱을 다시 시작하면 자동으로 로그인돼요.',
    );
    expect(find.byType(FilledButton), findsOneWidget);
    expect(find.widgetWithText(FilledButton, '로그인 설정'), findsOneWidget);
    // Only the Windows sign-in carries the dot; the screen itself is fine.
    expect(dots(t), {5});
    expect(row(t, 'host-open-windows'), HostSetupState.attention);
    expect(row(t, 'host-open-display'), HostSetupState.complete);
    expect(row(t, 'host-open-server'), HostSetupState.complete);
    expect(
      find.descendant(
        of: find.byKey(const Key('host-open-windows')),
        matching: find.text('확인 필요'),
      ),
      findsOneWidget,
    );
    await t.tap(find.byKey(const Key('host-problem-action')));
    await pumpWork(t);
    expect(text(t, 'host-selected-section'), 'Windows 로그인');
    expect(api.requests.every(isReadOnlyHostRequest), isTrue);
    expect(t.takeException(), isNull);
    await t.pumpWidget(const SizedBox.shrink());

    // Without automatic login a restart would not sign in: not promised.
    await show(
      t,
      _ready({
        'interactiveSession': false,
        'windowsAutologon': _autologon('off'),
      }),
    );
    expect(
      text(t, 'host-problem-message'),
      'Windows에 로그인돼 있지 않거나 잠겨 있어요. 클라이언트의 Console로 로그인해 주세요.',
    );
    expect(find.textContaining('자동으로 로그인돼요'), findsNothing);
    expect(t.takeException(), isNull);
  });

  testWidgets('Awaiting approval: the server is the broken place and the code is whole', (
    t,
  ) async {
    final api = FakeHostSettings()
      ..displays = 1
      ..statusOverrides = {'version': '1.0.51'}
      ..connectionRequest = {
        'status': 'pending',
        'origin': 'https://vdi.example.com',
        'requestId': 'fixture-only',
        'verificationCode': '49J2-HQP1',
        'expiresAt': DateTime.now()
            .add(const Duration(minutes: 12))
            .toIso8601String(),
      };
    await show(t, api, size: const Size(584, 481));
    // A desktop the server has not accepted has no name and no kind yet.
    expect(text(t, 'host-connection-heading'), '이 데스크톱');
    expect(text(t, 'host-machine-line'), '아직 서버에 등록하지 않았어요');
    expect(find.textContaining('VM'), findsNothing);
    expect(pill(t).label, '관리자 승인 필요');
    expect(find.byType(HostProblemBanner), findsOneWidget);
    expect(
      text(t, 'host-approval-summary'),
      '관리자 웹의 승인 대기에서 확인 코드가 같은지 본 뒤 승인해 주세요.',
    );
    // The code on one line, even in the smallest window.
    final code = find.byKey(const Key('host-overview-code'));
    expect(t.widget<SelectableText>(code).data, '49J2-HQP1');
    expect(t.widget<SelectableText>(code).maxLines, 1);
    expect(dots(t), contains(1));
    expect(row(t, 'host-open-server'), HostSetupState.approval);
    await t.ensureVisible(find.byKey(const Key('host-open-request')));
    await t.tap(find.byKey(const Key('host-open-request')));
    await pumpWork(t);
    // The server page holds the full request: code, cancel, edit.
    expect(find.byKey(const Key('host-approval-heading')), findsOneWidget);
    expect(find.byKey(const Key('host-verification-code')), findsOneWidget);
    expect(find.byKey(const Key('host-cancel-connection')), findsOneWidget);
    expect(find.byKey(const Key('host-edit-pending-server')), findsOneWidget);
    expect(api.requests.every(isReadOnlyHostRequest), isTrue);
    expect(t.takeException(), isNull);
  });

  testWidgets('A first install still opens its finish by itself and stays as the banner', (
    t,
  ) async {
    final api = FreshHost();
    await show(t, api);
    // Asked once, at once (host-prerequisites-v1).
    final ask = find.byKey(const Key('host-confirm-dialog'));
    expect(ask, findsOneWidget);
    expect(find.text('가상 화면 드라이버의 서명을 신뢰할까요?'), findsOneWidget);
    await t.tap(find.text('취소'));
    await pumpWork(t);
    expect(ask, findsNothing);
    expect(api.requests.every(isReadOnlyHostRequest), isTrue);
    // The unfinished install outranks the missing registration.
    expect(pill(t).label, '설치 마무리 필요');
    expect(pill(t).state, _warn);
    expect(find.byType(HostProblemBanner), findsOneWidget);
    expect(find.byKey(const Key('host-finish-card')), findsOneWidget);
    expect(find.widgetWithText(FilledButton, '설치 마무리'), findsOneWidget);
    expect(row(t, 'host-open-display'), HostSetupState.setup);
    expect(
      find.descendant(
        of: find.byKey(const Key('host-open-display')),
        matching: find.text('드라이버가 아직 없어요'),
      ),
      findsOneWidget,
    );
    // The banner's one action is the same finish.
    await t.tap(find.byKey(const Key('host-finish-install')));
    await pumpWork(t);
    expect(ask, findsOneWidget);
    await t.tap(find.text('취소'));
    await pumpWork(t);
    expect(api.of('installNativeDisplay'), isEmpty);
    expect(t.takeException(), isNull);
  });

  testWidgets('A local PC is never called a VM and is not sent to a Console', (
    t,
  ) async {
    const id = 'pc-0a1b2c3d-1111-4222-8333-444455556666';
    final api = _ready({'desktopName': 'TOTB-GAMING', 'desktopId': id});
    await show(t, api);
    expect(text(t, 'host-connection-heading'), 'TOTB-GAMING');
    expect(text(t, 'host-machine-line'), '로컬 PC');
    expect(pill(t).label, '스트리밍 가능');
    expect(find.textContaining('VM'), findsNothing);
    expect(find.textContaining('Proxmox'), findsNothing);
    expect(find.textContaining('pc-0a1b'), findsNothing);
    await t.pumpWidget(const SizedBox.shrink());

    await show(
      t,
      _ready({
        'desktopName': 'TOTB-GAMING',
        'desktopId': id,
        'interactiveSession': false,
      }),
    );
    expect(
      text(t, 'host-problem-message'),
      'Windows에 로그인돼 있지 않거나 잠겨 있어요. '
      '본체 화면에서 로그인하거나 데스크톱을 다시 시작하면 자동으로 로그인돼요.',
    );
    expect(find.textContaining('Console'), findsNothing);
    expect(find.textContaining('VM'), findsNothing);
    await t.pumpWidget(const SizedBox.shrink());

    // A local PC the server gave no name: still no VM, and no bare UUID.
    await show(t, _ready({'desktopName': '', 'desktopId': id}));
    expect(text(t, 'host-connection-heading'), '이 데스크톱');
    expect(text(t, 'host-machine-line'), '로컬 PC');
    expect(find.textContaining('pc-0a1b'), findsNothing);
    await t.pumpWidget(const SizedBox.shrink());

    // An id that tells nothing: no kind is claimed.
    await show(t, _ready({'desktopId': 'desk-1'}));
    expect(text(t, 'host-machine-line'), '서버에 등록된 데스크톱 · desk-1');
    expect(find.textContaining('VM'), findsNothing);
    expect(t.takeException(), isNull);
  });

  testWidgets('On a local PC every page says 본체 화면, never Proxmox or Console', (
    t,
  ) async {
    Future<void> open(int page) async {
      await t.tap(find.byKey(Key('host-nav-$page')));
      await pumpWork(t);
    }

    const id = 'pc-0a1b2c3d-1111-4222-8333-444455556666';
    final api =
        _ready({'desktopName': 'TOTB-GAMING', 'desktopId': id})
          ..recoveryRequired = true
          ..devices = {
            'schema': 1,
            'drives': {'winfsp': true, 'webClient': 'usable'},
            'usb': {'supported': true, 'package': true, 'controller': 'started'},
            'printers': {'reason': ''},
            'audio': {'output': 'absent'},
          };
    await show(t, api);
    expect(pill(t).label, '가상 화면 복구 필요');
    expect(
      text(t, 'host-display-attention-message'),
      '본체 화면에서 가상 화면 상태를 확인하고 복구를 진행하세요.',
    );
    await open(2);
    expect(
      find.text('이전 화면 변경이 중단됐어요. 본체 화면에서 가상 화면 복구를 실행하세요.'),
      findsOneWidget,
    );
    await open(6);
    expect(
      text(t, 'host-device-audio-next'),
      '소리 출력 장치를 연결하거나 Windows에서 켜세요.',
    );
    await open(5);
    expect(find.textContaining('잠기면 본체 화면에서 로그인하세요.'), findsOneWidget);
    for (final page in [0, 1, 2, 4, 5, 6]) {
      await open(page);
      for (final word in ['Proxmox', 'Console', 'VM']) {
        expect(
          find.textContaining(word),
          findsNothing,
          reason: '$word on page $page',
        );
      }
    }
    expect(api.requests.every(isReadOnlyHostRequest), isTrue);
    expect(t.takeException(), isNull);
    await t.pumpWidget(const SizedBox.shrink());

    // A VM, and an id that does not tell the kind, keep naming both.
    for (final other in ['pve-173', 'desk-1']) {
      await show(t, _ready({'desktopId': other})..recoveryRequired = true);
      expect(
        text(t, 'host-display-attention-message'),
        'Proxmox 콘솔(로컬 PC는 본체 화면)에서 가상 화면 상태를 확인하고 복구를 진행하세요.',
        reason: other,
      );
      await t.pumpWidget(const SizedBox.shrink());
    }
  });

  testWidgets('A stopped service is the first problem and its one action starts it', (
    t,
  ) async {
    final api = _ready({'interactiveSession': false})..running = false;
    await show(t, api);
    // The service outranks the sign-in, and the server's state is not known.
    expect(pill(t).label, 'Host 서비스 꺼짐');
    expect(find.byType(HostProblemBanner), findsOneWidget);
    await t.tap(find.widgetWithText(FilledButton, '서비스 시작'));
    await pumpWork(t);
    expect(find.text('Host 서비스를 시작할까요?'), findsOneWidget);
    expect(api.requests.where((r) => r['action'] == 'service'), isEmpty);
    await t.tap(find.byKey(const Key('host-confirm')));
    await pumpWork(t);
    expect(api.requests.where((r) => r['action'] == 'service').single, {
      'action': 'service',
      'command': 'start',
    });
    await t.tap(find.byKey(const Key('host-result-close')));
    await pumpWork(t);
    // Started: the next problem takes the one banner.
    expect(pill(t).label, 'Windows 로그인 필요');
    expect(find.byType(HostProblemBanner), findsOneWidget);
    expect(t.takeException(), isNull);
  });

  testWidgets('What does not stop a stream is said without breaking the route', (
    t,
  ) async {
    // Automatic login is off: streaming works now, a restart would not sign in.
    final api = _ready({'windowsAutologon': _autologon('off')})
      ..credentialsConfigured = false
      ..credentialUsername = '';
    await show(t, api);
    expect(pill(t).label, '자동 로그인 설정 필요');
    expect(pill(t).state, _warn);
    expect(find.byType(HostProblemBanner), findsOneWidget);
    expect(find.widgetWithText(FilledButton, '로그인 설정'), findsOneWidget);
    expect(dots(t), {5});
    await t.pumpWidget(const SizedBox.shrink());

    // A device feature that cannot work: the last thing the overview names.
    final devices = _ready()
      ..devices = {
        'schema': 1,
        'drives': {'winfsp': true, 'webClient': 'usable'},
        'usb': {'supported': true, 'package': true, 'controller': 'foreign'},
        'printers': {'reason': ''},
        'audio': {'output': 'present'},
      };
    await show(t, devices);
    expect(pill(t).label, '장치 확인 필요');
    expect(dots(t), {6});
    await t.tap(find.widgetWithText(FilledButton, '장치 보기'));
    await pumpWork(t);
    expect(find.byKey(const Key('host-device-usb-action')), findsOneWidget);
    expect(t.takeException(), isNull);
  });

  testWidgets('The most important problem wins and only one banner shows', (
    t,
  ) async {
    // Restart required + no server acknowledgement + signed out.
    final api =
        _ready({
            'broker': {'state': 'reconnecting', 'online': false},
            'interactiveSession': false,
          })
          ..rebootRequired = true;
    await show(t, api);
    expect(pill(t).label, 'Windows 다시 시작 필요');
    expect(find.byType(HostProblemBanner), findsOneWidget);
    expect(find.byKey(const Key('host-display-attention')), findsOneWidget);
    expect(find.byType(FilledButton), findsOneWidget);
    // Each area still says its own state.
    expect(dots(t), {1, 2, 5});
    api.rebootRequired = false;
    await t.tap(find.byKey(const Key('host-refresh')));
    await pumpWork(t);
    expect(pill(t).label, '서버 연결 안 됨');
    expect(find.byType(HostProblemBanner), findsOneWidget);
    api.statusOverrides = {..._desktop(), 'interactiveSession': false};
    await t.tap(find.byKey(const Key('host-refresh')));
    await pumpWork(t);
    expect(pill(t).label, 'Windows 로그인 필요');
    api.statusOverrides = _desktop();
    await t.tap(find.byKey(const Key('host-refresh')));
    await pumpWork(t);
    expect(pill(t).label, '스트리밍 가능');
    expect(find.byType(HostProblemBanner), findsNothing);
    expect(api.requests.every(isReadOnlyHostRequest), isTrue);
    expect(t.takeException(), isNull);
  });

  testWidgets('Before the first status nothing is claimed', (t) async {
    final api = FakeHostSettings(registered: true)..failStatus = true;
    await show(t, api);
    // The read failed: said once, with the way to read again.
    expect(pill(t).label, '상태 읽기 실패');
    expect(find.textContaining('VM'), findsNothing);
    expect(find.textContaining('등록하지 않았어요'), findsNothing);
    expect(find.text('스트리밍 가능'), findsNothing);
    expect(find.text('정상'), findsNothing);
    api.failStatus = false;
    await t.tap(find.widgetWithText(FilledButton, '새로고침'));
    await pumpWork(t);
    expect(pill(t).label, isNot('상태 읽기 실패'));
    expect(t.takeException(), isNull);
  });

  testWidgets(
    'The layout holds from the smallest window up, at 100 to 200 % text, dark and light',
    (t) async {
      final scenes = <String, FakeHostSettings Function()>{
        'normal': _ready,
        'signedout': () => _ready({'interactiveSession': false}),
        'restart': () => _ready()..rebootRequired = true,
        'approval': () => FakeHostSettings()
          ..displays = 1
          ..connectionRequest = {
            'status': 'pending',
            'origin': 'https://a-rather-long-server-name.example.com:18443',
            'requestId': 'fixture-only',
            'verificationCode': '49J2-HQP1',
            'expiresAt': DateTime.now()
                .add(const Duration(minutes: 12))
                .toIso8601String(),
          },
      };
      for (final mode in [ThemeMode.dark, ThemeMode.light]) {
        for (final size in const [
          // Inside the smallest (600×520), the default (640×680) and the
          // mockup's (680×760) windows, and a short wide one.
          Size(584, 481),
          Size(624, 641),
          Size(680, 760),
          Size(1200, 481),
        ]) {
          for (final scale in [1.0, 1.25, 1.5, 2.0]) {
            for (final MapEntry(key: name, value: build) in scenes.entries) {
              final api = build();
              final where = '$name ${mode.name} $size x$scale';
              await show(t, api, size: size, textScale: scale, mode: mode);
              expect(t.takeException(), isNull, reason: where);
              expect(
                find.byKey(const Key('host-overview-state')).hitTestable(),
                findsOneWidget,
                reason: where,
              );
              // Every page opens without an overflow.
              for (final page in [1, 5, 2, 6, 4, 0]) {
                final item = find.byKey(Key('host-nav-$page'));
                await t.ensureVisible(item);
                await t.tap(item);
                await pumpWork(t);
                expect(t.takeException(), isNull, reason: '$where page $page');
              }
              expect(api.requests.every(isReadOnlyHostRequest), isTrue);
              await t.pumpWidget(const SizedBox.shrink());
              await pumpWork(t);
            }
          }
        }
      }
    },
  );
}

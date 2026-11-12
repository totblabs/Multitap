import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_devices.dart';
import 'package:multitap_client/data/host_diagnostics.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_navigation.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';

import 'host_settings_test.dart'
    show FakeCloseGuard, FakeHostSettings, isReadOnlyHostRequest;

Map<String, dynamic> devices({
  Object? winfsp = true,
  Object? winfspPackage = true,
  Object? webClient = 'usable',
  Object? usbSupported = true,
  Object? usbPackage = true,
  Object? controller = 'started',
  Object? print = '',
  Object? spooler = 'running',
  Object? audio = 'present',
}) => {
  'schema': 1,
  'drives': {
    'winfsp': winfsp,
    'package': winfspPackage,
    'webClient': webClient,
  },
  'usb': {
    'supported': usbSupported,
    'package': usbPackage,
    'controller': controller,
  },
  'printers': {'reason': print, 'spooler': spooler},
  'audio': {'output': audio},
};

HostDeviceRow row(Map<String, dynamic> value, String id) =>
    hostDeviceRows(value).singleWhere((item) => item.id == id);

void main() {
  test('Each observed device state has its own level and one next action', () {
    final ready = hostDeviceRows(devices());
    expect(ready.map((r) => r.title), ['공유 폴더', 'USB', '프린터', '사운드카드']);
    expect(ready.every((r) => r.level == HostDeviceLevel.ready), isTrue);
    expect(ready.every((r) => r.next == null), isTrue);
    expect(hostDevicesSettled(ready), isTrue);
    expect(hostDevicesSummary(ready), '공유 폴더·USB·프린터·사운드카드 준비됨');

    for (final c in [
      (devices(winfsp: false), 'drives', HostDeviceLevel.setup, 'WinFsp를 설치'),
      (
        devices(winfsp: false, webClient: 'disabled'),
        'drives',
        HostDeviceLevel.attention,
        'WinFsp를 설치',
      ),
      (
        devices(usbSupported: false),
        'usb',
        HostDeviceLevel.unsupported,
        'Server 제외',
      ),
      (devices(usbPackage: false), 'usb', HostDeviceLevel.attention, 'Host 설치'),
      (
        devices(controller: 'stopped'),
        'usb',
        HostDeviceLevel.attention,
        '자동으로 다시 시작하지 않아요',
      ),
      (
        devices(controller: 'foreign'),
        'usb',
        HostDeviceLevel.attention,
        'Multitap용으로 바꾸세요',
      ),
      (
        devices(controller: 'multiple'),
        'usb',
        HostDeviceLevel.attention,
        'Multitap용으로 바꾸세요',
      ),
      (
        devices(print: 'print_spooler_unavailable'),
        'printers',
        HostDeviceLevel.attention,
        'Print Spooler',
      ),
      (
        devices(print: 'print_driver_unavailable'),
        'printers',
        HostDeviceLevel.attention,
        'Microsoft Print to PDF',
      ),
      (
        devices(audio: 'absent'),
        'audio',
        HostDeviceLevel.setup,
        '관리자가 Proxmox에서 오디오 장치',
      ),
      (
        devices(audio: 'remote'),
        'audio',
        HostDeviceLevel.unknown,
        'Proxmox 콘솔',
      ),
    ]) {
      final item = row(c.$1, c.$2);
      expect(item.level, c.$3, reason: '${c.$2} ${c.$1}');
      expect(
        '${item.status} ${item.next ?? ''}',
        contains(c.$4),
        reason: '${c.$2} ${c.$1}',
      );
      expect(
        hostDevicesSettled(hostDeviceRows(c.$1)),
        c.$3 == HostDeviceLevel.unsupported,
      );
    }
    // USB that installs itself at first use is not a problem to report.
    final automatic = hostDeviceRows(devices(controller: 'none'));
    expect(
      row(devices(controller: 'none'), 'usb').level,
      HostDeviceLevel.automatic,
    );
    expect(row(devices(controller: 'none'), 'usb').next, isNull);
    expect(hostDevicesSettled(automatic), isTrue);
    expect(
      hostDevicesSummary(hostDeviceRows(devices(usbSupported: false))),
      '공유 폴더·프린터·사운드카드 준비됨 · USB 미지원',
    );
    expect(
      hostDevicesSummary(
        hostDeviceRows(
          devices(audio: 'absent', print: 'print_spooler_unavailable'),
        ),
      ),
      '프린터 확인 필요 · 사운드카드 설정 필요',
    );
  });

  test('Missing, older or hostile device replies are never shown as ready', () {
    for (final value in <Object?>[
      null,
      {},
      {'schema': 2},
      'devices',
    ]) {
      expect(hostDeviceRows(value), isEmpty);
      expect(hostDevicesSettled(hostDeviceRows(value)), isFalse);
      expect(hostDevicesSummary(hostDeviceRows(value)), '장치 상태 확인 필요');
    }
    final hostile = hostDeviceRows(
      devices(
        winfsp: 'true',
        webClient: 'C:\\canary',
        usbSupported: 1,
        controller: 'canary',
        print: 'canary printer',
        audio: 'Speakers (canary)',
      ),
    );
    expect(hostile.every((r) => r.level == HostDeviceLevel.unknown), isTrue);
    expect(
      hostile.any((r) => '${r.status}${r.next}'.contains('canary')),
      isFalse,
    );
  });

  test('Copied diagnostics carry the fixed device texts only', () {
    final report = hostDiagnosticsText({
      'devices': devices(audio: 'absent', print: 'canary-printer'),
    });
    expect(report, contains('사운드카드: 설정 필요 · 소리 출력 장치 없음'));
    expect(report, contains('프린터: 확인 불가'));
    expect(report, isNot(contains('canary')));
    expect(hostDiagnosticsText({}), isNot(contains('사운드카드')));
  });

  const online = {'state': 'online', 'online': true, 'heartbeatAgeSeconds': 1};

  Future<void> open(
    WidgetTester t,
    FakeHostSettings api, {
    Size size = const Size(1100, 900),
    double textScale = 1,
    Brightness brightness = Brightness.dark,
  }) async {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = size;
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: brightness),
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context)
              .copyWith(textScaler: TextScaler.linear(textScale)),
          child: child!,
        ),
        home: HostSettingsScreen(api: api, closeGuard: FakeCloseGuard()),
      ),
    );
    await t.pumpAndSettle();
  }

  // The navigation shows no state words: the state is what the screen hands
  // it, and an area that needs the user carries one amber dot.
  HostSetupState area(WidgetTester t, int index) => t
      .widget<HostSettingsNavigation>(find.byType(HostSettingsNavigation))
      .states[index];
  HostSetupState badge(WidgetTester t) {
    final state = area(t, 6);
    expect(
      find.byKey(const Key('host-nav-dot-6')),
      state.needsUser ? findsOneWidget : findsNothing,
    );
    return state;
  }

  testWidgets('The device page sits under the virtual screen and reads only', (
    t,
  ) async {
    final api = FakeHostSettings(registered: true)..displays = 1;
    await open(t, api);
    final screen = t.getRect(find.byKey(const Key('host-nav-2')));
    final device = t.getRect(find.byKey(const Key('host-nav-6')));
    final diagnostics = t.getRect(find.byKey(const Key('host-nav-4')));
    expect(device.top, greaterThan(screen.top));
    expect(device.top, lessThan(diagnostics.top));
    expect(badge(t), HostSetupState.complete);
    expect(find.text('공유 폴더·USB·프린터·사운드카드 준비됨'), findsOneWidget);

    await t.tap(find.byKey(const Key('host-open-devices')));
    await t.pumpAndSettle();
    expect(
      t.widget<Text>(find.byKey(const Key('host-selected-section'))).data,
      '장치',
    );
    for (final id in ['drives', 'usb', 'printers', 'audio']) {
      expect(find.byKey(Key('host-device-$id')), findsOneWidget);
      expect(find.byKey(Key('host-device-$id-next')), findsNothing);
    }
    expect(find.text('준비됨'), findsNWidgets(4));
    // Supporting detail stays folded until asked for.
    expect(find.textContaining('스마트 카드'), findsNothing);
    await t.tap(find.byKey(const Key('host-devices-help')));
    await t.pumpAndSettle();
    expect(find.textContaining('스마트 카드와 직렬 장치는 USB로'), findsOneWidget);
    expect(find.byKey(const Key('host-elevate')), findsNothing);
    expect(api.requests.every(isReadOnlyHostRequest), isTrue);
    expect(t.takeException(), isNull);
  });

  testWidgets('Each device row reads name · state word · one fact', (t) async {
    final api = FakeHostSettings(registered: true)
      ..displays = 1
      ..devices = devices(winfsp: false, controller: 'foreign');
    await open(t, api);
    await t.tap(find.byKey(const Key('host-nav-6')));
    await t.pumpAndSettle();
    // 2026-11-11: no "● 준비됨" badge followed by "준비됐어요." and a
    // "~할 수 있어요" sentence. One line per feature.
    for (final (id, name, state, fact) in [
      ('drives', '공유 폴더', '설정 필요', 'WinFsp 없음, 4 GB 넘는 파일 불가'),
      ('usb', 'USB', '확인 필요', '다른 프로그램이 사용 중'),
      ('printers', '프린터', '준비됨', '연결하면 Multitap 프린터 추가'),
      ('audio', '사운드카드', '준비됨', '출력 장치 있음'),
    ]) {
      final row = find.byKey(Key('host-device-$id'));
      for (final word in [name, state, fact]) {
        expect(
          find.descendant(of: row, matching: find.text(word)),
          findsOneWidget,
          reason: '$id: $word',
        );
      }
      expect(
        t.widget<Text>(find.byKey(Key('host-device-$id-level'))).data,
        state,
      );
      expect(
        t.widget<Text>(find.byKey(Key('host-device-$id-status'))).data,
        fact,
      );
      // The three sit on one line at this width.
      final top = t.getRect(find.byKey(Key('host-device-$id-level'))).center.dy;
      expect(
        t.getRect(find.byKey(Key('host-device-$id-status'))).center.dy,
        closeTo(top, 1),
        reason: id,
      );
      expect(
        find.descendant(of: row, matching: find.byType(HostSetupBadge)),
        findsNothing,
      );
    }
    // A sentence only where there is something to do.
    expect(find.byKey(const Key('host-device-printers-next')), findsNothing);
    expect(find.byKey(const Key('host-device-audio-next')), findsNothing);
    expect(
      t.widget<Text>(find.byKey(const Key('host-device-drives-next'))).data,
      'WinFsp를 설치하세요.',
    );
    expect(
      t.widget<Text>(find.byKey(const Key('host-device-usb-next'))).data,
      'USB 연결 기능을 Multitap용으로 바꾸세요.',
    );
    final page = find.byKey(const Key('host-devices'));
    for (final hedge in ['준비됐어요', '할 수 있어요', '쓸 수 있어요']) {
      expect(
        find.descendant(of: page, matching: find.textContaining(hedge)),
        findsNothing,
        reason: hedge,
      );
    }
    // A state word that says it all has no fact.
    await t.pumpWidget(const SizedBox.shrink());
    final unknown = FakeHostSettings(registered: true)
      ..displays = 1
      ..devices = devices(audio: 'canary');
    await open(t, unknown);
    await t.tap(find.byKey(const Key('host-nav-6')));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('host-device-audio-status')), findsNothing);
    expect(
      t.widget<Text>(find.byKey(const Key('host-device-audio-level'))).data,
      '확인 불가',
    );
    expect(t.takeException(), isNull);
  });

  testWidgets(
    'Before registration the overview keeps its one connection task',
    (t) async {
      await open(t, FakeHostSettings());
      expect(find.byKey(const Key('host-open-devices')), findsNothing);
      expect(find.byKey(const Key('host-nav-6')), findsOneWidget);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('A VM without a sound card says where to add one', (t) async {
    final api = FakeHostSettings(registered: true)
      ..displays = 1
      ..devices = devices(audio: 'absent', controller: 'none');
    await open(t, api);
    expect(badge(t), HostSetupState.setup);
    expect(find.text('사운드카드 설정 필요'), findsOneWidget);
    await t.tap(find.byKey(const Key('host-nav-6')));
    await t.pumpAndSettle();
    expect(
      t.widget<Text>(find.byKey(const Key('host-device-audio-next'))).data,
      '관리자가 Proxmox에서 오디오 장치를 추가해야 해요.',
    );
    expect(
      t.widget<Text>(find.byKey(const Key('host-device-usb-level'))).data,
      '자동 설치',
    );
    expect(t.takeException(), isNull);
  });

  testWidgets('Broken or unreadable device state needs a check, not a pass', (
    t,
  ) async {
    for (final value in <Map<String, dynamic>?>[
      devices(print: 'print_spooler_unavailable'),
      devices(audio: 'remote'),
      null,
    ]) {
      final api = FakeHostSettings(registered: true)
        ..displays = 1
        ..statusOverrides = {
          'broker': online,
          if (value == null) 'devices': null,
        };
      if (value != null) api.devices = value;
      await open(t, api);
      expect(badge(t), HostSetupState.attention, reason: '$value');
      // Devices stay optional: the host itself is still operational.
      expect(area(t, 0), HostSetupState.complete);
      await t.tap(find.byKey(const Key('host-nav-6')));
      await t.pumpAndSettle();
      if (value == null) {
        expect(
          find.byKey(const Key('host-devices-unavailable')),
          findsOneWidget,
        );
      }
      await t.pumpWidget(const SizedBox.shrink());
    }
  });

  test('USB wording is plain, our own earlier install needs nothing, others get one fix', () {
    for (final code in ['foreign', 'multiple']) {
      final item = row(devices(controller: code), 'usb');
      expect(item.status, '다른 프로그램이 사용 중');
      expect(item.action, HostDeviceAction.replaceUsbController);
    }
    for (final code in ['unmarked', 'duplicates']) {
      final item = row(devices(controller: code), 'usb');
      expect(item.level, HostDeviceLevel.ready, reason: code);
      expect(item.next, isNull);
      expect(item.action, isNull);
    }
    expect(row(devices(controller: 'unknown'), 'usb').action, isNull);
    // Never a protocol name or a trip to Device Manager.
    for (final code in [
      'none',
      'started',
      'stopped',
      'unmarked',
      'duplicates',
      'foreign',
      'multiple',
      'unknown',
    ]) {
      final item = row(devices(controller: code), 'usb');
      final text = '${item.status} ${item.next ?? ''}';
      for (final banned in ['USB/IP', 'USBip', 'usbip', '장치 관리자']) {
        expect(text, isNot(contains(banned)), reason: '$code: $banned');
      }
    }
    var result = hostUsbReplaceResult({
      'usb': {'error': '', 'restart': false, 'removed': 1},
    });
    expect(result.ok, isTrue);
    result = hostUsbReplaceResult({
      'usb': {'error': 'usb_restart_required', 'restart': true, 'removed': 1},
    });
    expect(result.ok, isTrue);
    expect(result.message, contains('자동으로 다시 시작하지 않아요'));
    result = hostUsbReplaceResult({
      'usb': {'error': 'usb_replace_remove_failed', 'restart': false},
    });
    expect(result.ok, isFalse);
    expect(hostUsbReplaceResult({}).ok, isFalse);
  });

  test('Display adapter counts parse strictly and never invent a list', () {
    final ok = HostDisplayAdapters.parse({
      'read': true,
      'current': 1,
      'legacy': 0,
      'duplicate': 1,
      'unproven': 4,
      'foreign': 2,
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
        {
          'id': r'ROOT\MULTITAPVIRTUALDISPLAY\0000',
          'kind': 'legacy',
          'present': true,
          'started': true,
        },
      ],
    });
    expect(ok.read, isTrue);
    expect(ok.leftover, 5);
    expect(ok.extras.map((e) => e.kind), ['unproven', 'duplicate', 'legacy']);
    expect(ok.summary, contains('남은 장치 5개'));
    for (final bad in <Object?>[
      null,
      {'read': false},
      {'read': true, 'current': 1},
      {
        'read': true,
        'current': 1,
        'legacy': 0,
        'duplicate': 0,
        'unproven': 99,
        'foreign': 0,
        'extras': [],
      },
      {
        'read': true,
        'current': 1,
        'legacy': 0,
        'duplicate': 0,
        'unproven': 1,
        'foreign': 0,
        'extras': [
          {'id': r'PCI\VEN_10DE', 'kind': 'unproven'},
        ],
      },
      {
        'read': true,
        'current': 1,
        'legacy': 0,
        'duplicate': 0,
        'unproven': 1,
        'foreign': 0,
        'extras': [
          {'id': r'ROOT\DISPLAY\0001', 'kind': 'current'},
        ],
      },
    ]) {
      final parsed = HostDisplayAdapters.parse(bad);
      expect(parsed.read, isFalse, reason: '$bad');
      expect(parsed.extras, isEmpty);
    }
    expect(
      hostAdapterCleanupResult({
        'adapters': {
          'error': '',
          'removed': 3,
          'pending': 1,
          'failed': 0,
          'refused': 0,
        },
      }).message,
      allOf(contains('3개를 지웠어요'), contains('자동으로 다시 시작하지 않아요')),
    );
    expect(
      hostAdapterCleanupResult({
        'adapters': {'error': 'display_adapters_changed'},
      }).ok,
      isFalse,
    );
  });
  test('Only fixable states offer a fix, and Disabled is never one', () {
    expect(
      row(devices(winfsp: false), 'drives').action,
      HostDeviceAction.installWinfsp,
    );
    expect(
      row(devices(winfsp: false, webClient: 'missing'), 'drives').action,
      HostDeviceAction.installWinfsp,
    );
    // Without the carried MSI the setup EXE is the way.
    final uncarried = row(
      devices(winfsp: false, winfspPackage: false),
      'drives',
    );
    expect(uncarried.action, isNull);
    expect(uncarried.next, contains('Host 설치 파일'));
    expect(row(devices(), 'drives').action, isNull);
    final stopped = row(
      devices(print: 'print_spooler_unavailable', spooler: 'stopped'),
      'printers',
    );
    expect(stopped.action, HostDeviceAction.startSpooler);
    final disabled = row(
      devices(print: 'print_spooler_unavailable', spooler: 'disabled'),
      'printers',
    );
    expect(disabled.action, isNull);
    expect(disabled.next, contains('자동으로 켜지 않아요'));
    for (final state in ['starting', 'missing', 'unknown', null]) {
      expect(
        row(
          devices(print: 'print_spooler_unavailable', spooler: state),
          'printers',
        ).action,
        isNull,
        reason: '$state',
      );
    }
    expect(
      row(
        devices(print: 'print_driver_unavailable', spooler: 'stopped'),
        'printers',
      ).action,
      isNull,
    );
  });

  test('Install and spooler results say what happened and what to do', () {
    var result = hostWinfspInstallResult({
      'winfsp': {'action': 'installed', 'restart': false, 'error': ''},
    });
    expect(result.ok, isTrue);
    expect(result.message, isNot(contains('다시 시작')));
    result = hostWinfspInstallResult({
      'winfsp': {'action': 'installed', 'restart': true, 'error': ''},
    });
    expect(result.ok, isTrue);
    expect(result.message, contains('자동으로 다시 시작하지 않아요'));
    result = hostWinfspInstallResult({
      'winfsp': {
        'action': 'skipped',
        'restart': true,
        'error': 'winfsp_load_failed',
      },
    });
    expect(result.ok, isFalse);
    expect(result.message, contains('자동으로 다시 시작하지 않아요'));
    expect(result.message, contains('winfsp_load_failed'));
    expect(result.message, contains('기본 방식'));
    for (final code in [
      'winfsp_busy',
      'winfsp_blocked',
      'winfsp_package_mismatch',
      'winfsp_install_failed',
    ]) {
      final failed = hostWinfspInstallResult({
        'winfsp': {'action': 'skipped', 'restart': false, 'error': code},
      });
      expect(failed.ok, isFalse);
      expect(failed.message, isNot(contains('다시 시작해야')), reason: code);
      expect(failed.message, contains(code));
    }
    expect(hostWinfspInstallResult({}).ok, isFalse);
    expect(
      hostWinfspInstallResult({
        'winfsp': {'error': 'canary text'},
      }).message,
      isNot(contains('canary')),
    );
    expect(
      hostSpoolerStartResult({
        'spooler': {'state': 'running', 'error': ''},
      }).ok,
      isTrue,
    );
    expect(
      hostSpoolerStartResult({
        'spooler': {'state': 'stopped', 'error': ''},
      }).ok,
      isFalse,
    );
    expect(
      hostSpoolerStartResult({
        'spooler': {'state': 'disabled', 'error': 'print_spooler_disabled'},
      }).message,
      contains('자동으로 바꾸지 않아요'),
    );
    expect(hostSpoolerStartResult({}).ok, isFalse);
  });

  Future<void> openDevices(WidgetTester t, FakeHostSettings api) async {
    await open(t, api);
    await t.tap(find.byKey(const Key('host-nav-6')));
    await t.pumpAndSettle();
  }

  testWidgets('WinFsp installs after one confirmation with visible progress', (
    t,
  ) async {
    final api = FakeHostSettings(registered: true)
      ..displays = 1
      ..devices = devices(winfsp: false)
      ..devicePending = Completer<void>();
    await openDevices(t, api);
    final button = find.byKey(const Key('host-device-drives-action'));
    expect(button, findsOneWidget);
    // Cancelling the confirmation changes nothing.
    await t.tap(button);
    await t.pumpAndSettle();
    expect(find.byKey(const Key('host-confirm-dialog')), findsOneWidget);
    expect(find.textContaining('자동으로 다시 시작하지 않아요'), findsOneWidget);
    await t.tap(find.text('취소'));
    await t.pumpAndSettle();
    expect(api.requests.every(isReadOnlyHostRequest), isTrue);

    await t.tap(button);
    await t.pumpAndSettle();
    await t.tap(find.byKey(const Key('host-confirm')));
    await t.pump();
    expect(find.byKey(const Key('host-operation-progress')), findsOneWidget);
    expect(find.text('WinFsp 설치 중'), findsOneWidget);
    expect(api.requests.where((r) => r['action'] == 'installWinfsp').single, {
      'action': 'installWinfsp',
      'confirmed': true,
    });
    api.devicePending!.complete();
    await t.pumpAndSettle();
    expect(find.byKey(const Key('host-result-dialog')), findsOneWidget);
    expect(find.textContaining('WinFsp를 설치했어요'), findsWidgets);
    await t.tap(find.byKey(const Key('host-result-close')));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('host-device-drives-action')), findsNothing);
    expect(row(api.devices, 'drives').level, HostDeviceLevel.ready);
    expect(t.takeException(), isNull);
  });

  testWidgets('A restart MSI asked for is said plainly, never performed', (
    t,
  ) async {
    final api = FakeHostSettings(registered: true)
      ..displays = 1
      ..devices = devices(winfsp: false)
      ..winfspResult = {'action': 'installed', 'restart': true, 'error': ''};
    await openDevices(t, api);
    await t.tap(find.byKey(const Key('host-device-drives-action')));
    await t.pumpAndSettle();
    await t.tap(find.byKey(const Key('host-confirm')));
    await t.pumpAndSettle();
    expect(
      find.descendant(
        of: find.byKey(const Key('host-result-dialog')),
        matching: find.textContaining('자동으로 다시 시작하지 않아요'),
      ),
      findsOneWidget,
    );
    expect(api.requests.map((r) => r['action']).toSet(), {
      'status',
      'installWinfsp',
    });
  });

  testWidgets(
    'A stopped spooler starts from its row; Disabled offers nothing',
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..displays = 1
        ..devices = devices(
          print: 'print_spooler_unavailable',
          spooler: 'stopped',
        );
      await openDevices(t, api);
      await t.tap(find.byKey(const Key('host-device-printers-action')));
      await t.pumpAndSettle();
      // The row explains the button: no confirmation and no success dialog.
      expect(find.byKey(const Key('host-confirm-dialog')), findsNothing);
      expect(find.byKey(const Key('host-result-dialog')), findsNothing);
      expect(
        api.requests.where((r) => r['action'] == 'startPrintSpooler'),
        hasLength(1),
      );
      expect(find.textContaining('인쇄 스풀러를 시작했어요'), findsOneWidget);
      expect(
        find.byKey(const Key('host-device-printers-action')),
        findsNothing,
      );
      await t.pumpWidget(const SizedBox.shrink());

      final disabled = FakeHostSettings(registered: true)
        ..devices = devices(
          print: 'print_spooler_unavailable',
          spooler: 'disabled',
        );
      await openDevices(t, disabled);
      expect(
        find.byKey(const Key('host-device-printers-action')),
        findsNothing,
      );
      expect(disabled.requests.every(isReadOnlyHostRequest), isTrue);
    },
  );

  testWidgets(
    'Without administrator rights the fix is disabled with a reason',
    (t) async {
      final api = FakeHostSettings(registered: true, administrator: false)
        ..devices = devices(winfsp: false);
      await openDevices(t, api);
      final button = t.widget<ButtonStyleButton>(
        find.byKey(const Key('host-device-drives-action')),
      );
      expect(button.onPressed, isNull);
      expect(
        find.ancestor(
          of: find.byKey(const Key('host-device-drives-action')),
          matching: find.byWidgetPredicate(
            (w) => w is Tooltip && w.message == '관리자 권한으로 설정을 열어야 해요.',
          ),
        ),
        findsOneWidget,
      );
      expect(find.byKey(const Key('host-elevate')), findsOneWidget);
    },
  );

  testWidgets(
    "Another program's USB controller is replaced after one confirmation",
    (t) async {
      final api = FakeHostSettings(registered: true)
        ..displays = 1
        ..devices = devices(controller: 'foreign');
      await openDevices(t, api);
      expect(find.text('다른 프로그램이 사용 중'), findsOneWidget);
      await t.tap(find.byKey(const Key('host-device-usb-action')));
      await t.pumpAndSettle();
      expect(find.textContaining('USB 연결 기능은 더 이상 동작하지 않아요'), findsOneWidget);
      await t.tap(find.text('취소'));
      await t.pumpAndSettle();
      expect(api.requests.every(isReadOnlyHostRequest), isTrue);
      await t.tap(find.byKey(const Key('host-device-usb-action')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('host-confirm')));
      await t.pumpAndSettle();
      expect(
        api.requests.where((r) => r['action'] == 'replaceUsbController').single,
        {'action': 'replaceUsbController', 'confirmed': true},
      );
      expect(find.byKey(const Key('host-result-dialog')), findsOneWidget);
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('정리 lists the exact leftover adapters and sends only that list', (
    t,
  ) async {
    final api = FakeHostSettings(registered: true)..displays = 1;
    await open(t, api);
    await t.tap(find.byKey(const Key('host-nav-2')));
    await t.pumpAndSettle();
    final line = find.byKey(const Key('host-display-adapters'));
    await t.ensureVisible(line);
    expect(t.widget<Text>(line).data, contains('남은 장치 2개'));
    await t.tap(find.byKey(const Key('host-display-adapters-clean')));
    await t.pumpAndSettle();
    expect(find.textContaining(r'ROOT\DISPLAY\0001'), findsOneWidget);
    expect(find.textContaining('다른 프로그램이 쓰고 있을 수 있어요'), findsOneWidget);
    await t.tap(find.byKey(const Key('host-confirm')));
    await t.pumpAndSettle();
    expect(
      api.requests.where((r) => r['action'] == 'cleanDisplayAdapters').single,
      {
        'action': 'cleanDisplayAdapters',
        'confirmed': true,
        'ids': [r'ROOT\DISPLAY\0001', r'ROOT\DISPLAY\0002'],
      },
    );
    expect(find.textContaining('2개를 지웠어요'), findsWidgets);
    await t.tap(find.byKey(const Key('host-result-close')));
    await t.pumpAndSettle();
    expect(find.byKey(const Key('host-display-adapters-clean')), findsNothing);
    expect(t.takeException(), isNull);
  });
  testWidgets('Narrow large-text window keeps every device line readable', (
    t,
  ) async {
    for (final brightness in Brightness.values) {
      final api = FakeHostSettings(registered: true)
        ..devices = devices(
          winfsp: false,
          audio: 'absent',
          controller: 'foreign',
        );
      await open(
        t,
        api,
        size: const Size(600, 640),
        textScale: 1.3,
        brightness: brightness,
      );
      final item = find.byKey(const Key('host-nav-6'));
      await t.ensureVisible(item);
      await t.tap(item);
      await t.pumpAndSettle();
      for (final id in ['drives', 'usb', 'audio']) {
        await t.ensureVisible(find.byKey(Key('host-device-$id-next')));
      }
      expect(t.takeException(), isNull, reason: '$brightness');
      await t.pumpWidget(const SizedBox.shrink());
    }
  });
}

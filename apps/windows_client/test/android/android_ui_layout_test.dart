// Layout guarantees of the key Android screens at phone portrait 360×800
// and 412×915, landscape 915×412 and 800×360, tablet 1280×800 and TV
// 960×540, with text at 100 %, 130 % and 200 % (stand-in system bars and
// cutout): nothing overflows, no control label is cut, every touch target is
// at least 48 dp and inside the safe area, and up to 130 % no control label
// wraps (at 200 % a label may take two lines rather than be cut).
// Widget renders with fakes; no device, server or display is touched.
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_launcher.dart'
    show androidTheme;
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/l10n/l10n.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';

import '../remote_session_test.dart' show SessionSocket;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore, vm;
import 'android_session_contract_test.dart' show AndroidNativeFake;
import 'android_ui_audit.dart';
import '../i18n/hangul_audit.dart';
import 'android_updates_test.dart' show FakePlatform, catalog, apk;
import '../helpers/pump_work.dart';

typedef _Screen = ({String label, Size size, EdgeInsets insets, bool tv});

const _screens = <_Screen>[
  (
    label: 'phone 360×800',
    size: Size(360, 800),
    insets: EdgeInsets.only(top: 32, bottom: 16),
    tv: false,
  ),
  (
    label: 'phone 412×915',
    size: Size(412, 915),
    insets: EdgeInsets.only(top: 32, bottom: 16),
    tv: false,
  ),
  (
    label: 'landscape 915×412',
    size: Size(915, 412),
    insets: EdgeInsets.only(top: 24, left: 32, bottom: 16),
    tv: false,
  ),
  (
    label: 'landscape 800×360',
    size: Size(800, 360),
    insets: EdgeInsets.only(top: 24, left: 32, bottom: 16),
    tv: false,
  ),
  (
    label: 'tablet 1280×800',
    size: Size(1280, 800),
    insets: EdgeInsets.only(top: 24, bottom: 16),
    tv: false,
  ),
  (
    label: 'TV 960×540',
    size: Size(960, 540),
    insets: EdgeInsets.zero,
    tv: true,
  ),
];

const _scales = [1.0, 1.3, 2.0];

/// A running VM without a sound device: its card carries the 소리 없음
/// badge.
BrokerDesktop _silent() => BrokerDesktop(
  audioState: 'absent',
  id: 'pve-149',
  name: 'TESTER',
  kind: 'vm',
  description: '',
  os: 'Windows 11 · 4 vCPU',
  status: 'online',
  canConnect: true,
  canStream: true,
  hostConnected: true,
  vmAudio: true,
  powerCanForceStop: true,
  power: BrokerPowerSnapshot(
    state: 'running',
    stale: false,
    checkedAt: DateTime.utc(2026, 11, 2),
  ),
);

/// A local PC whose Host can be updated: the launcher then shows the 구분
/// filter and badges the header's update button.
BrokerDesktop _pc() => BrokerDesktop(
  id: 'pc-gaming',
  name: 'TOWER-PC',
  kind: 'pc',
  description: '',
  os: 'Windows 11',
  status: 'online',
  canConnect: true,
  canStream: true,
  hostConnected: true,
  hostUpdate: const HostUpdateState(
    present: true,
    connected: true,
    supported: true,
    version: '0.2.130',
    latestVersion: '0.2.140',
    newer: true,
    available: true,
  ),
);

class _Broker extends FakeBroker implements HostUpdateBrokerApi {
  _Broker(super.inventory);
}

Uint8List _video(int pts) {
  final bytes = Uint8List(17);
  bytes[0] = 1;
  bytes[1] = 1;
  for (var i = 0; i < 8; i++) {
    bytes[4 + i] = (pts >> (8 * i)) & 255;
  }
  bytes.setAll(12, [0, 0, 0, 1, 0x65]);
  return bytes;
}

void _size(WidgetTester t, _Screen screen, double scale) {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = screen.size;
  final pad = FakeViewPadding(
    left: screen.insets.left,
    top: screen.insets.top,
    right: screen.insets.right,
    bottom: screen.insets.bottom,
  );
  t.view.padding = pad;
  t.view.viewPadding = pad;
  t.platformDispatcher.textScaleFactorTestValue = scale;
  addTearDown(t.view.reset);
  addTearDown(t.platformDispatcher.clearAllTestValues);
}

/// The English pass (docs/i18n.md): the same screens with the device in
/// English, where no Korean may be left but the fixtures' own names.
var _english = false;
const _fixtureText = {'개발용 VM', '검증 VM', '원격 화면 터치 조작을 다듬었어요'};

void _language(bool english) {
  _english = english;
  AppLanguageController.debugSystemLanguage = english ? 'en' : 'ko';
}

/// What is on screen now has no overflow, no cut control label, no target
/// under 48 dp or outside the safe area, and (below 200 %) no wrapped
/// control label. [multiline]: texts that may take two lines by design.
void _expectClean(
  WidgetTester t,
  String where,
  double scale, {
  Set<String> multiline = const {},
}) {
  expect(t.takeException(), isNull, reason: where);
  final findings = [
    for (final finding in auditScreen(t, multiline: multiline))
      if (finding.kind != 'wrap' && finding.kind != 'clip' ||
          finding.kind == 'wrap' && scale < 2 ||
          finding.kind == 'clip' && finding.detail.contains('" in '))
        '$finding',
  ];
  expect(findings, isEmpty, reason: where);
  if (_english) expectNoHangul(t, where, allow: _fixtureText);
}

/// Closes the open menu or dialog (a menu may be an overlay, not a route).
Future<void> _pop(WidgetTester t) async {
  await t.sendKeyEvent(LogicalKeyboardKey.escape);
  await pumpWork(t);
}

Future<bool> _open(WidgetTester t, Finder finder) async {
  if (finder.evaluate().isEmpty) return false;
  await t.ensureVisible(finder.first);
  await pumpWork(t);
  await t.tap(finder.first);
  await pumpWork(t);
  return true;
}

Future<void> _app(
  WidgetTester t,
  List<BrokerDesktop> desktops, {
  bool signIn = true,
  bool tv = false,
  String theme = 'dark',
  String latest = '0.1.0-preview.6',
}) async {
  MemoryStore(
    origin: 'https://vdi.example',
    username: 'tester',
    prefs: {'appearance.theme': theme},
  ).install();
  final broker = _Broker(desktops);
  await t.pumpWidget(
    AndroidVdiApp(
      key: UniqueKey(),
      television: tv,
      apiFactory: (_) => broker,
      updatesFactory: (o) => AndroidUpdates(
        o,
        transport: catalog([
          apk(latest, 'arm64-v8a', changelog: '- 원격 화면 터치 조작을 다듬었어요'),
        ]),
      ),
      updatePlatform: FakePlatform(),
    ),
  );
  await pumpWork(t);
  if (!signIn) return;
  t
          .widget<TextField>(find.byKey(const Key('sign-in-password')))
          .controller!
          .text =
      'secret-pass';
  await t.ensureVisible(find.byKey(const Key('login-submit')));
  await pumpWork(t);
  await t.tap(find.byKey(const Key('login-submit')));
  await pumpWork(t);
  await t.runAsync(
    () => Future<void>.delayed(const Duration(milliseconds: 60)),
  );
  await pumpWork(t);
}

/// A connected streaming session over a blank stand-in picture.
Future<void> _session(WidgetTester t, {bool tv = false}) async {
  AndroidNativeFake().install();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  messenger.setMockMethodCallHandler(
    const MethodChannel('multitap/pen-input'),
    (_) async => null,
  );
  messenger.setMockMethodCallHandler(
    AndroidSessionKeepalive.channel,
    (call) async => call.method == 'start' ? 'started' : true,
  );
  final sockets = <SessionSocket>[];
  await t.pumpWidget(
    MaterialApp(
      key: UniqueKey(),
      theme: androidTheme(buildAppTheme(brightness: Brightness.dark)),
      builder: (context, child) => AndroidTvNavigation(
        television: tv,
        child: TooltipVisibility(visible: false, child: child!),
      ),
      home: AndroidRemoteView(
        name: 'TESTER',
        token: 'not-persisted',
        options: const StreamOptions(),
        density: () async => null,
        displayScale: true,
        load: (options, _, scale) async => BrokerConnection(
          sessionId: 'android-session',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/api/v1/sessions/android-session',
          ),
          options: options,
          displayScale: scale,
        ),
        sessionFactory: (connection, media, _) => RemoteSession(
          connection: connection,
          token: 'not-persisted',
          media: media,
          dialer: (_, _) async {
            final socket = SessionSocket();
            sockets.add(socket);
            return socket;
          },
        ),
        pictureBuilder: (_) => const ColoredBox(color: Color(0xFF1B4F9C)),
      ),
    ),
  );
  for (var i = 0; i < 4; i++) {
    await t.pump(const Duration(milliseconds: 20));
  }
  final size = t.view.physicalSize;
  sockets.last.incoming.add(
    jsonEncode({
      'type': 'stream.info',
      'codec': 'h264',
      'width': size.width.toInt() ~/ 2 * 2,
      'height': size.height.toInt() ~/ 2 * 2,
    }),
  );
  await t.pump(const Duration(milliseconds: 20));
  sockets.last.incoming.add(_video(16667));
  for (var i = 0; i < 6; i++) {
    await t.pump(const Duration(milliseconds: 40));
  }
}

/// The session's menus animate for at most 400 ms; its live picture keeps
/// timers going, so frames are pumped for a bounded while instead of until
/// nothing is scheduled.
Future<void> _settle(WidgetTester t) async {
  for (var i = 0; i < 8; i++) {
    await t.pump(const Duration(milliseconds: 100));
  }
}

Future<void> _endSession(WidgetTester t) async {
  await t.pumpWidget(const SizedBox.shrink());
  await t.pump(const Duration(seconds: 1));
  AndroidNativeFake.uninstall();
}

void main() {
  setUpAll(() async {
    final font = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      font.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await font.load();
    await (FontLoader(
      'MaterialIcons',
    )..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'))).load();
  });
  tearDown(
    () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null),
  );

  for (final english in [false, true]) {
    for (final screen in _screens) {
      for (final scale in _scales) {
        final at =
            '${screen.label}, text ${(scale * 100).round()}%${english ? ', English' : ''}';

        testWidgets('sign-in and server settings fit at $at', (t) async {
          _language(english);
          final semantics = t.ensureSemantics();
          _size(t, screen, scale);
          await _app(t, const [], signIn: false, tv: screen.tv);
          _expectClean(t, 'sign-in · $at', scale);
          expect(
            await _open(t, find.byKey(const Key('open-server-settings'))),
            isTrue,
          );
          _expectClean(t, 'server settings · $at', scale);
          await _pop(t);
          semantics.dispose();
        });

        testWidgets('launcher, card menus and dialogs fit at $at', (t) async {
          _language(english);
          final semantics = t.ensureSemantics();
          _size(t, screen, scale);
          // A local PC beside the VMs brings the 구분 filter; its Host update
          // badges the header's update button (this app itself is current).
          final desktops = [
            _silent(),
            vm(id: 'pve-150', name: '개발용 VM', power: 'stopped', stream: false),
            _pc(),
          ];
          await _app(t, desktops, tv: screen.tv, latest: '0.1.0-preview.4');
          expect(find.byKey(const Key('desktop-kind')), findsOneWidget);
          expect(find.byKey(const Key('host-update-badge')), findsOneWidget);
          _expectClean(t, 'launcher · $at', scale);
          for (final (what, key) in [
            ('⏻ menu', 'desktop-power-pve-149'),
            ('⋯ menu', 'desktop-more-pve-149'),
            ('▾ menu', 'stream-modes-pve-149'),
          ]) {
            expect(await _open(t, find.byKey(Key(key))), isTrue, reason: what);
            _expectClean(t, '$what · $at', scale);
            await _pop(t);
          }
          // The account menu's server line may take two lines at 200 %.
          expect(await _open(t, find.byKey(const Key('account-menu'))), isTrue);
          _expectClean(
            t,
            'account menu · $at',
            scale,
            multiline: const {'vdi.example'},
          );
          await _pop(t);
          // 설정: the header's own ⚙ opens the settings window.
          expect(
            await _open(t, find.byKey(const Key('open-app-settings'))),
            isTrue,
          );
          expect(find.byKey(const Key('app-settings-dialog')), findsOneWidget);
          _expectClean(t, 'settings · $at', scale);
          await _pop(t);
          // The update panel lists the Host it badged, with its own 업데이트
          // (its catalog read finishes outside the fake clock).
          final check = find.byKey(const Key('android-update-check'));
          await t.ensureVisible(check);
          await pumpWork(t);
          await t.tap(check);
          await t.pump();
          await t.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 60)),
          );
          await pumpWork(t);
          expect(find.byKey(const Key('host-update-offer')), findsOneWidget);
          _expectClean(t, 'update panel · $at', scale);
          final host = find.byKey(const Key('open-host-update-pc-gaming'));
          await t.ensureVisible(host);
          await pumpWork(t);
          _expectClean(t, 'update panel Host entry · $at', scale);
          await _pop(t);
          semantics.dispose();
        });

        // As on Windows: every control of the title and filter rows
        // (the short landscape header row too) is one height, at least 48 dp.
        testWidgets('header controls share one height at $at', (t) async {
          _language(english);
          final semantics = t.ensureSemantics();
          _size(t, screen, scale);
          await _app(t, [
            _silent(),
            vm(id: 'pve-150', name: '개발용 VM', power: 'stopped', stream: false),
            vm(id: 'pve-151', name: '검증 VM'),
            _pc(),
          ], tv: screen.tv);
          // A filter brings 필터 해제.
          expect(await _open(t, find.byKey(const Key('kind-vm'))), isTrue);
          const keys = [
            'desktop-kind',
            'desktop-status',
            'desktop-filter-reset',
            'desktop-sort',
            'desktop-previews',
            'refresh-desktops',
            'desktop-search-open',
            'desktop-search',
          ];
          void expectOneHeight(String where, String search) {
            final heights = {
              for (final key in keys)
                if (find.byKey(Key(key)).evaluate().isNotEmpty)
                  key: t.getSize(find.byKey(Key(key))).height,
            };
            expect(heights.keys, containsAll([...keys.take(6), search]));
            expect(
              heights.values.map((h) => h.toStringAsFixed(1)).toSet(),
              hasLength(1),
              reason: '$where $heights',
            );
            expect(heights.values.first, greaterThanOrEqualTo(48));
          }

          expectOneHeight('closed search · $at', 'desktop-search-open');
          _expectClean(t, 'filtered launcher · $at', scale);
          expect(
            await _open(t, find.byKey(const Key('desktop-search-open'))),
            isTrue,
          );
          expectOneHeight('open search · $at', 'desktop-search');
          semantics.dispose();
        });

        testWidgets('card dialogs keep their buttons whole at $at', (t) async {
          _language(english);
          final semantics = t.ensureSemantics();
          _size(t, screen, scale);
          await _app(t, [_silent()], tv: screen.tv);
          // 정지's danger question.
          await _open(t, find.byKey(const Key('desktop-power-pve-149')));
          expect(
            await _open(t, find.byKey(const Key('power-stop-pve-149'))),
            isTrue,
          );
          _expectClean(t, 'stop question · $at', scale);
          await _pop(t);
          // The sound badge is an indicator only: a tap opens
          // nothing, and the card stays clean with it.
          expect(
            await _open(t, find.byKey(const Key('sound-badge-pve-149'))),
            isTrue,
          );
          expect(find.byType(Dialog), findsNothing);
          _expectClean(t, 'sound badge · $at', scale);
          semantics.dispose();
        });

        testWidgets('session tools and panels fit at $at', (t) async {
          _language(english);
          final semantics = t.ensureSemantics();
          _size(t, screen, scale);
          await _session(t, tv: screen.tv);
          _expectClean(t, 'session cluster · $at', scale);
          final more = find.byKey(const Key('expand-session-toolbar'));
          await t.tap(more);
          await _settle(t);
          _expectClean(t, 'session menu · $at', scale);
          // The menu's rows as on the PC's bar, and what each opens: 연결 품질,
          // 화면 (배율 is inside it) and 설정, then from 설정 its 화질,
          // 입력 방식 (not on a TV) and 사용안내.
          for (final (panel, path, shown) in [
            ('connection', ['session-quality'], 'android-connection-panel'),
            ('screen', ['android-session-screen'], 'android-screen-panel'),
            ('more', ['android-session-more'], 'android-more-panel'),
            (
              'settings',
              ['android-session-more', 'android-session-settings'],
              'android-settings-panel',
            ),
            if (!screen.tv)
              (
                'input',
                ['android-session-more', 'android-session-input'],
                'android-input-panel',
              ),
            (
              'help',
              ['android-session-more', 'android-session-help'],
              'android-touch-help',
            ),
          ]) {
            for (final key in path) {
              final row = find.byKey(Key(key));
              await t.ensureVisible(row);
              await _settle(t);
              await t.tap(row);
              await _settle(t);
            }
            expect(
              find.byKey(Key(shown)),
              findsOneWidget,
              reason: '$panel · $at',
            );
            _expectClean(t, '$panel panel · $at', scale);
            // 설정 on a phone on its side fits without scrolling: its 재연결
            // is in sight (it was below the fold).
            if (panel == 'settings' &&
                screen.size.width > screen.size.height &&
                screen.size.height < 500 &&
                scale == 1) {
              final apply = t.getRect(
                find.byKey(const Key('apply-stream-settings')),
              );
              expect(
                apply.bottom,
                lessThanOrEqualTo(screen.size.height - screen.insets.bottom),
                reason: '재연결 in sight · $at',
              );
            }
            // C hamburger closes the complete panel; the next press opens
            // the main menu again. Keep every panel's layout assertions.
            await t.tap(more);
            await _settle(t);
            expect(
              find.byKey(const Key('session-toolbar-pointer')),
              findsNothing,
            );
            await t.tap(more);
            await _settle(t);
            expect(
              find.byKey(const Key('session-command-bar')),
              findsOneWidget,
              reason: 'the main menu again after $panel · $at',
            );
          }
          await t.tap(more);
          await _settle(t);
          // The key row above the phone keyboard.
          await t.tap(find.byKey(const Key('android-remote-keyboard-button')));
          await _settle(t);
          _expectClean(t, 'key row · $at', scale);
          await _endSession(t);
          semantics.dispose();
        });
      }
    }
  }
}

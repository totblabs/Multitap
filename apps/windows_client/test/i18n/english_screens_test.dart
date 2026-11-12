// The Windows client and the Host settings app in English (docs/i18n.md):
// the main screens show no Korean but data values, nothing overflows at the
// usual and narrow window sizes, and the language setting applies at once
// and is remembered. The Android screens are covered by the English pass in
// test/android/android_ui_layout_test.dart, the session bar by the one in
// test/windows_session_toolbar_visual_test.dart.
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/host_settings_api.dart';
import 'package:multitap_client/l10n/l10n.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/virtual_display.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';
import 'package:multitap_client/ui/themed_app.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import '../auth_flow_test.dart' show TestBroker, fixtureOrigin;
import '../host_settings_test.dart' show FakeCloseGuard, FakeHostSettings;
import 'hangul_audit.dart';

const _machines = [
  DesktopMachine(
    id: 'en-149',
    name: 'TESTER',
    description: '',
    kind: MachineKind.vm,
    status: MachineStatus.ready,
    specification: 'Windows 11',
    accent: Colors.blue,
    powerState: 'running',
    powerStale: false,
    powerCanForceStop: true,
    hostOnline: true,
    interactiveSession: true,
    virtualDisplayReady: true,
    virtualMonitorControl: true,
    display: VirtualDisplayState(supported: true, activeCount: 2, stale: false),
    monitors: [
      RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
      RemoteMonitor(id: '1', width: 1920, height: 1080),
    ],
  ),
  DesktopMachine(
    id: 'en-170',
    name: 'DESIGN',
    description: '',
    kind: MachineKind.vm,
    status: MachineStatus.poweredOff,
    specification: 'Windows 11',
    accent: Colors.blue,
    powerState: 'stopped',
    powerStale: false,
    hostOnline: false,
  ),
  DesktopMachine(
    id: 'en-pc',
    name: 'TOWER-PC',
    description: '',
    kind: MachineKind.pc,
    status: MachineStatus.awaitingHost,
    specification: 'Windows 11',
    accent: Colors.teal,
    hostOnline: true,
    interactiveSession: false,
  ),
];

const _sizes = [
  Size(1440, 900),
  Size(900, 600),
  Size(640, 600),
  Size(480, 640),
];

Future<void> _fonts(WidgetTester tester) => tester.runAsync(() async {
  final loader = FontLoader('Pretendard');
  for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
    loader.addFont(
      File('assets/fonts/Pretendard-$weight.otf')
          .readAsBytes()
          .then(ByteData.sublistView),
    );
  }
  await loader.load();
});

void _size(WidgetTester tester, Size size) {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = size;
  addTearDown(tester.view.reset);
}

void _clean(WidgetTester tester, String where, {Set<String> allow = const {}}) {
  expect(tester.takeException(), isNull, reason: where);
  expectNoHangul(tester, where, allow: allow);
}

Future<void> _tap(WidgetTester tester, String key) async {
  final target = find.byKey(Key(key)).first;
  await tester.ensureVisible(target);
  await tester.pumpAndSettle();
  await tester.tap(target);
  await tester.pumpAndSettle();
}

void main() {
  setUp(() => AppLanguageController.debugSystemLanguage = 'en');

  for (final size in _sizes) {
    final at = '${size.width.toInt()}×${size.height.toInt()}';

    testWidgets('sign-in, server settings and the desktop list at $at', (
      tester,
    ) async {
      await _fonts(tester);
      _size(tester, size);
      final api = TestBroker();
      await tester.pumpWidget(
        MultitapApp(api: api, serverOrigin: fixtureOrigin),
      );
      await tester.pumpAndSettle();
      _clean(tester, 'sign-in · $at');
      await _tap(tester, 'open-server-settings');
      _clean(tester, 'server settings · $at');
      await _tap(tester, 'cancel-server-settings');
      await tester.enterText(find.byKey(const Key('sign-in-id')), 'alice');
      await tester.enterText(
        find.byKey(const Key('sign-in-password')),
        'test-password',
      );
      await _tap(tester, 'login-submit');
      _clean(tester, 'desktop list · $at');
      await _tap(tester, 'workspace-account');
      _clean(tester, 'account menu · $at', allow: {'alice'});
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      await _tap(tester, 'open-app-settings');
      expect(find.byKey(const Key('account-language')), findsOneWidget);
      _clean(tester, 'settings · $at');
    });

    testWidgets('the desktop list with every kind of card at $at', (
      tester,
    ) async {
      await _fonts(tester);
      _size(tester, size);
      await tester.pumpWidget(
        ThemedApp(
          appearance: AppAppearance(),
          home: WorkspaceShell(
            onSignOut: () {},
            onRefresh: () {},
            displayName: 'Alex',
            serverOrigin: 'https://cloud.example.com',
            machines: _machines,
            localMonitors: 2,
            onConnect: (_, {options = const StreamOptions()}) async =>
                throw StateError('Render fixture does not connect'),
            onPower: (_, _) async {},
            onConfigureDisplays: (_, _) async => _machines.first,
          ),
        ),
      );
      await tester.pumpAndSettle();
      _clean(tester, 'cards · $at');
      for (final key in [
        'desktop-power-en-149',
        'desktop-more-en-149',
        'stream-modes-en-149',
      ]) {
        if (find.byKey(Key(key)).evaluate().isEmpty) continue;
        await _tap(tester, key);
        _clean(tester, '$key · $at');
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
      }
    });

    testWidgets('the preview, client information and a session at $at', (
      tester,
    ) async {
      await _fonts(tester);
      _size(tester, size);
      await tester.pumpWidget(
        MultitapApp(enableDemo: true, serverOrigin: fixtureOrigin),
      );
      await tester.pumpAndSettle();
      await _tap(tester, 'open-preview');
      _clean(tester, 'preview list · $at');
      await _tap(tester, 'workspace-account');
      await _tap(tester, 'open-settings');
      _clean(tester, 'client information · $at');
      await _tap(tester, 'close-settings');
      await _tap(tester, 'open-sample-office');
      _clean(tester, 'session preview · $at');
    });

    testWidgets('Host settings overview at $at', (tester) async {
      await _fonts(tester);
      _size(tester, size);
      final host = FakeHostSettings(registered: true)..displays = 2;
      await tester.pumpWidget(
        ThemedApp(
          appearance: AppAppearance(),
          title: 'Multitap Host',
          home: HostSettingsScreen(api: host, closeGuard: FakeCloseGuard()),
        ),
      );
      await tester.pumpAndSettle();
      _clean(tester, 'Host settings · $at');
    });
  }

  testWidgets('the language menu switches at once, keeps state and is saved', (
    tester,
  ) async {
    AppLanguageController.debugSystemLanguage = 'ko';
    final saved = <AppLanguage>[];
    AppLanguageController.instance.configure(
      AppLanguage.auto,
      save: (language) async => saved.add(language),
    );
    _size(tester, const Size(1280, 800));
    await tester.pumpWidget(
      MultitapApp(api: TestBroker(), serverOrigin: fixtureOrigin),
    );
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('sign-in-id')), 'kept-id');
    expect(hangulOnScreen(tester), isNotEmpty, reason: 'starts in Korean');
    await _tap(tester, 'appearance-menu');
    await _tap(tester, 'language-en');
    expect(saved, [AppLanguage.en]);
    expect(AppLanguageController.instance.choice, AppLanguage.en);
    // No restart: the same sign-in screen, its typed text kept, in English.
    expect(find.text('kept-id'), findsOneWidget);
    _clean(tester, 'sign-in after switching', allow: {'kept-id'});
    await _tap(tester, 'appearance-menu');
    await _tap(tester, 'language-ko');
    expect(saved, [AppLanguage.en, AppLanguage.ko]);
    expect(hangulOnScreen(tester), isNotEmpty, reason: 'back in Korean');
    expect(find.text('kept-id'), findsOneWidget);
  });

  testWidgets('the settings window language row applies the choice', (
    tester,
  ) async {
    final saved = <AppLanguage>[];
    AppLanguageController.instance.configure(
      AppLanguage.auto,
      save: (language) async => saved.add(language),
    );
    _size(tester, const Size(1280, 800));
    await tester.pumpWidget(
      MultitapApp(enableDemo: true, serverOrigin: fixtureOrigin),
    );
    await tester.pumpAndSettle();
    await _tap(tester, 'open-preview');
    await _tap(tester, 'open-app-settings');
    await _tap(tester, 'language-ko');
    expect(saved, [AppLanguage.ko]);
    expect(AppLanguageController.instance.code, 'ko');
    await tester.pumpAndSettle();
    expect(hangulOnScreen(tester), isNotEmpty);
  });

  test('the choice is stored with the appearance and keeps it', () async {
    final directory = await Directory.systemTemp.createTemp('multitap-lang-');
    addTearDown(() => directory.delete(recursive: true));
    final file = File('${directory.path}/appearance.json');
    final settings = AppearanceSettings(file);
    expect(await settings.loadLanguage(), AppLanguage.auto);
    await settings.save(ThemeMode.light);
    await settings.saveScene(BackgroundScene.meadow);
    await settings.saveLanguage(AppLanguage.en);
    expect(await settings.loadLanguage(), AppLanguage.en);
    expect(await settings.load(), ThemeMode.light);
    expect(await settings.loadScene(), BackgroundScene.meadow);
    await settings.save(ThemeMode.dark);
    expect(await settings.loadLanguage(), AppLanguage.en, reason: 'kept');
    expect(jsonDecode(await file.readAsString()), {
      'theme': 'dark',
      'scene': 'meadow',
      'language': 'en',
    });
    await file.writeAsString('{"language":"fr"}');
    expect(await settings.loadLanguage(), AppLanguage.auto);
  });

  test('auto follows the system: Korean only for ko locales', () {
    expect(resolveSystemLanguage('ko'), 'ko');
    expect(resolveSystemLanguage('KO'), 'ko');
    for (final other in ['en', 'ja', 'zh', 'fr', '', null]) {
      expect(resolveSystemLanguage(other), 'en', reason: '$other');
    }
    AppLanguageController.debugSystemLanguage = 'ko';
    expect(AppLanguageController.instance.code, 'ko');
    expect(acceptLanguage, startsWith('ko'));
    AppLanguageController.debugSystemLanguage = 'en';
    expect(AppLanguageController.instance.code, 'en');
    expect(acceptLanguage, 'en');
    AppLanguageController.instance.debugReset(AppLanguage.ko);
    expect(
      AppLanguageController.instance.code,
      'ko',
      reason: 'the setting wins',
    );
  });

  test('the Host helper is told the app'
      's language (MULTITAP_LANG)', () {
    AppLanguageController.debugSystemLanguage = 'en';
    expect(NativeHostSettingsApi.hostToolEnvironment(), {
      'MULTITAP_LANG': 'en',
    });
    AppLanguageController.debugSystemLanguage = 'ko';
    expect(NativeHostSettingsApi.hostToolEnvironment(), {
      'MULTITAP_LANG': 'ko',
    });
    AppLanguageController.instance.debugReset(AppLanguage.en);
    expect(NativeHostSettingsApi.hostToolEnvironment(), {
      'MULTITAP_LANG': 'en',
    });
  });

  test('placeholders are filled in both languages', () {
    AppLanguageController.debugSystemLanguage = 'en';
    expect(l10n.text13(2, ''), '2 monitors');
    expect(l10n.text13(1, ''), '1 monitor');
    expect(l10n.logErrorCode('42'), 'Error code 42');
    expect(l10n.untilDate('Nov 12 14:00'), 'until Nov 12 14:00');
    expect(formatMonthDay(DateTime(2026, 11, 12)), 'Nov 12');
    AppLanguageController.debugSystemLanguage = 'ko';
    expect(l10n.text13(2, ''), '모니터 2대');
    expect(l10n.logErrorCode('42'), '오류 코드 42');
    expect(formatMonthDay(DateTime(2026, 11, 12)), '11월 12일');
  });

  // English sentences that could read wrong once their pieces are put
  // together.
  test('English sentences read as sentences with what is put into them', () {
    AppLanguageController.debugSystemLanguage = 'en';
    // The release history toggle (was "Whole screen", the zoom's word).
    expect(l10n.releaseHistoryShowAll, 'Show all');
    expect(l10n.wholeScreen, 'Whole screen', reason: 'the Android zoom keeps it');
    // One clean phrase for a row that was not checked.
    final unknown = l10n.notCheckedCheckedOnceEarlier;
    expect(unknown, startsWith(l10n.unknown4));
    expect(unknown, isNot(contains('Not checked')));
    // Was "Can't use Shared folder."
    expect(l10n.cantUseSharedFolders(''), "Shared folders can't be used.");
    expect(l10n.cantUseUsbDevices(''), "USB devices can't be used.");
    // The device word starts its sentence or stands as a label, for the PC
    // and for Android's "This device" alike (was "…stay on PC.", "Share
    // folders from This device").
    for (final device in [l10n.pcDeviceWord, l10n.device]) {
      expect(l10n.shareFoldersFrom(device), 'Folder sharing · $device');
      expect(
        l10n.chooseFolderShare2(device),
        '$device: choose a folder to share',
      );
      expect(
        l10n.programsDesktopHaveFilesFrom('a', 'b', device),
        endsWith('. $device keeps the folder and its files.'),
      );
    }
    expect(l10n.pcDeviceWord, 'Your PC');
    // A USB device moving to this window is not a file transfer.
    expect(l10n.usbMovingHere, 'Moving');
    expect(l10n.starting(l10n.desktop3), 'Starting the desktop');
    expect(l10n.currentVersionUnknown, 'Current version unknown');
    // The field says "ID": so do the lines about it.
    expect(l10n.id, 'ID');
    for (final line in [
      l10n.checkUsernamePassword,
      l10n.changeInitialUsernamePasswordAdmin,
      l10n.couldntRememberUsernameYouCan,
    ]) {
      expect(line.toLowerCase(), isNot(contains('username')), reason: line);
      expect(line, contains('ID'), reason: line);
    }
    AppLanguageController.debugSystemLanguage = 'ko';
    expect(l10n.pcDeviceWord, 'PC');
    expect(l10n.shareFoldersFrom(l10n.pcDeviceWord), 'PC 폴더 공유');
  });

  test('the English catalog matches the Korean one', () {
    Map<String, Object?> read(String name) =>
        jsonDecode(File('lib/l10n/$name').readAsStringSync())
            as Map<String, Object?>;
    final ko = read('app_ko.arb'), en = read('app_en.arb');
    final keys = ko.keys.where((k) => !k.startsWith('@')).toSet();
    expect(en.keys.where((k) => !k.startsWith('@')).toSet(), keys);
    final placeholder = RegExp(r'\{(\w+)[},]');
    final hangul = RegExp(r'[가-힣ᄀ-ᇿ㄰-㆏]');
    final problems = <String>[];
    for (final key in keys) {
      final english = en[key]! as String;
      final declared =
          ((ko['@$key'] as Map?)?['placeholders'] as Map?)?.keys.toSet() ??
          const <Object?>{};
      final used = {for (final m in placeholder.allMatches(english)) m[1]}
          .where(declared.contains)
          .toSet();
      if (used.length != declared.length) problems.add('$key placeholders');
      if (hangul.hasMatch(english) && key != 'languageNameKorean') {
        problems.add('$key Korean in English');
      }
    }
    expect(problems, isEmpty);
  });

  test('no Korean text is written in lib outside the catalog', () {
    // Data compared with what the server or Windows sends, not shown text.
    const allowed = {
      'lib/data/desktop_mapping.dart': ['미확인'],
      'lib/ui/usb_panel.dart': ['공동인증서'],
      // A widget key tests find the 화면 command by, never shown.
      'lib/ui/remote_view.dart': ['stream-menu-화면'],
    };
    const ignored = {
      'lib/android/android_keyboard.dart', // the Hangul IME's own tables
      'lib/ui/korean_text.dart', // Korean line breaking
    };
    final hangul = RegExp(r'[가-힣ᄀ-ᇿ㄰-㆏]');
    final literal = RegExp(r'''r?'(?:[^'\\\n]|\\.)*'|r?"(?:[^"\\\n]|\\.)*"''');
    final found = <String>[];
    for (final file in Directory('lib').listSync(recursive: true)) {
      final path = file.path.replaceAll('\\', '/');
      if (file is! File || !path.endsWith('.dart')) continue;
      if (path.startsWith('lib/l10n/') || ignored.contains(path)) continue;
      final lines = file.readAsLinesSync();
      for (var i = 0; i < lines.length; i++) {
        final code = lines[i].split(RegExp(r'\s//')).first;
        if (code.trimLeft().startsWith('//')) continue;
        for (final m in literal.allMatches(code)) {
          final text = m[0]!;
          if (!hangul.hasMatch(text)) continue;
          if ((allowed[path] ?? const []).any(text.contains)) continue;
          found.add('$path:${i + 1} $text');
        }
      }
    }
    expect(found, isEmpty);
  });
}

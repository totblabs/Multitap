import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:crypto/crypto.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/app_licenses.dart';
import 'package:multitap_client/data/client_build_version.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/app_info_dialog.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_settings_screen.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';

import 'auth_flow_test.dart' show TestLoginMemory;
import 'host_settings_test.dart' show FakeCloseGuard, FakeHostSettings;

const _capture = bool.fromEnvironment('MULTITAP_CAPTURE_LICENSE_UI');
const _captureDirectory = String.fromEnvironment('MULTITAP_LICENSE_CAPTURE_DIR');
const _sourcePin = String.fromEnvironment('MULTITAP_LICENSE_SOURCE_PIN');
final _rendered = <Map<String, Object?>>[];

Future<void> _writeFresh(File file, List<int> bytes) async {
  await file.create(exclusive: true);
  await file.writeAsBytes(bytes, flush: true);
}

Future<void> _fonts() async {
  final font = FontLoader('Pretendard');
  for (final name in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
    font.addFont(rootBundle.load('assets/fonts/Pretendard-$name.otf'));
  }
  await font.load();
  final artifacts = File(Platform.resolvedExecutable).parent.parent.parent;
  final icons = FontLoader('MaterialIcons')
    ..addFont(
      File('${artifacts.path}/material_fonts/MaterialIcons-Regular.otf')
          .readAsBytes()
          .then((bytes) => ByteData.sublistView(bytes)),
    );
  await icons.load();
}

final _longOriginal =
    'ORIGINAL HEADER\n${List.filled(400, 'Complete synthetic license paragraph.').join('\n')}\nneedle-beta\nORIGINAL TAIL';
AppLicenseCatalog _catalog() => AppLicenseCatalog(
  components: [
    AppLicenseComponent('Fixture Alpha', const [
      AppLicenseNotice(text: 'ALPHA ORIGINAL', license: 'Fixture-A'),
    ]),
    AppLicenseComponent('Fixture Beta', [
      AppLicenseNotice(
        text: _longOriginal,
        license: 'Fixture-B',
        version: '2.3',
        source: 'https://fixture.invalid/source',
      ),
    ]),
  ],
);

Future<void> _show(
  WidgetTester tester, {
  Size size = const Size(1920, 1080),
  double scale = 1,
  Brightness brightness = Brightness.dark,
  AppInfoTab tab = AppInfoTab.licenses,
  Future<AppLicenseCatalog> Function()? load,
  Future<ClientBuildVersion> Function()? version,
  VoidCallback? history,
  bool settle = true,
  GlobalKey? boundary,
}) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = size;
  addTearDown(tester.view.resetDevicePixelRatio);
  addTearDown(tester.view.resetPhysicalSize);
  final app = MaterialApp(
    theme: buildAppTheme(brightness: brightness),
    builder: (context, child) => MediaQuery(
      data: MediaQuery.of(context)
          .copyWith(textScaler: TextScaler.linear(scale)),
      child: child!,
    ),
    home: Builder(
      builder: (context) => Scaffold(
        body: TextButton(
          key: const Key('open-fixture-info'),
          onPressed: () => unawaited(
            showDialog<void>(
              context: context,
              builder: (_) => AppInfoDialog(
                initialTab: tab,
                loadLicenses: load ?? () async => _catalog(),
                loadVersion:
                    version ?? () async => ClientBuildVersion.parse('9.8.7+65'),
                onReleaseHistory: history,
              ),
            ),
          ),
          child: const Text('open'),
        ),
      ),
    ),
  );
  await tester.pumpWidget(
    boundary == null ? app : RepaintBoundary(key: boundary, child: app),
  );
  await tester.tap(find.byKey(const Key('open-fixture-info')));
  if (settle) {
    await tester.pumpAndSettle();
  } else {
    await tester.pump(const Duration(milliseconds: 250));
  }
}

void main() {
  setUpAll(() async {
    if (!_capture) return;
    final path = Directory(_captureDirectory).absolute.path
        .replaceAll('/', r'\');
    if (!RegExp(
          r'^C:\\tmp\\gpt-license-ui-render-20261028\\run-[0-9a-f]{32}$',
          caseSensitive: false,
        ).hasMatch(path) ||
        !RegExp(r'^[0-9a-f]{64}$').hasMatch(_sourcePin)) {
      throw StateError(
        'Root must select a fresh bounded output directory and source pin',
      );
    }
    final dir = Directory(path);
    if (await dir.exists()) throw StateError('Preserve existing render output');
    await dir.create(recursive: true);
  });
  tearDownAll(() async {
    if (!_capture) return;
    expect(_rendered.length, 6);
    if (_rendered.length != 6) return;
    await _writeFresh(
      File('$_captureDirectory/fixtures.json'),
      utf8.encode(
        jsonEncode({
          'schema': 1,
          'sourcePin': _sourcePin,
          'fixtures': _rendered,
          'versionIsFake': true,
          'version': '9.8.7+65',
          'nativeRpcCalls': 0,
          'installedWindowAcceptance': false,
        }),
      ),
    );
  });
  for (final fixture in [
    (const Size(1920, 1080), 1.0),
    (const Size(1366, 768), 1.5),
    (const Size(960, 640), 1.5),
  ]) {
    for (final brightness in [Brightness.dark, Brightness.light]) {
      testWidgets('license dialog fits ${fixture.$1}/${fixture.$2}/$brightness', (
        tester,
      ) async {
        AppLicenseCatalog? actual;
        final boundary = _capture ? GlobalKey() : null;
        if (_capture) {
          await tester.runAsync(() async {
            await _fonts();
            actual = await AppLicenseCatalog.load();
          });
          expect(actual!.issues, isEmpty);
        }
        await _show(
          tester,
          size: fixture.$1,
          scale: fixture.$2,
          brightness: brightness,
          load: actual == null ? null : () async => actual!,
          boundary: boundary,
        );
        expect(tester.takeException(), isNull);
        final bounds = tester.getRect(find.byType(Dialog));
        expect(bounds.left, greaterThanOrEqualTo(0));
        expect(bounds.top, greaterThanOrEqualTo(0));
        expect(bounds.right, lessThanOrEqualTo(fixture.$1.width));
        expect(bounds.bottom, lessThanOrEqualTo(fixture.$1.height));
        expect(
          find.byKey(const Key('close-settings')).hitTestable(),
          findsOneWidget,
        );
        if (fixture.$1.width == 960) {
          expect(
            find.byKey(const Key('license-component-select')),
            findsOneWidget,
          );
          expect(
            tester.getTopLeft(find.byKey(const ValueKey('license-text-0'))).dy,
            lessThan(
              tester.getRect(find.byKey(const Key('license-document'))).bottom -
                  24,
            ),
            reason: 'The narrow document initially shows original text, not metadata alone.',
          );
        }
        if (_capture) {
          await tester.runAsync(() async {
            final render =
                boundary!.currentContext!.findRenderObject()!
                    as RenderRepaintBoundary;
            final image = await render.toImage(pixelRatio: 1);
            try {
              final data = await image.toByteData(
                format: ui.ImageByteFormat.png,
              );
              final bytes = data!.buffer.asUint8List();
              final name =
                  '${brightness.name}-${fixture.$1.width.toInt()}x${fixture.$1.height.toInt()}-t${(fixture.$2 * 100).toInt()}.png';
              await _writeFresh(File('$_captureDirectory/$name'), bytes);
              _rendered.add({
                'file': name,
                'size': bytes.length,
                'sha256': sha256.convert(bytes).toString(),
                'theme': brightness.name,
                'logicalWidth': fixture.$1.width,
                'logicalHeight': fixture.$1.height,
                'textScale': fixture.$2,
                'devicePixelRatio': 1,
                'licenseManifest': nativeLicenseManifestAsset,
                'realBundledOriginals': true,
              });
            } finally {
              image.dispose();
            }
          });
        }
        await tester.tap(find.byKey(const Key('close-settings')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('client-info-dialog')), findsNothing);
      });
    }
  }

  testWidgets(
    'project is initially selected, then user selection and filter results are respected',
    (tester) async {
      await _show(
        tester,
        load: () async => AppLicenseCatalog(
          components: [
            AppLicenseComponent('AAA Fixture', const [
              AppLicenseNotice(text: 'common alpha', license: 'Fixture'),
            ]),
            AppLicenseComponent('Multitap', const [
              AppLicenseNotice(text: 'common project-only', license: 'Fixture'),
            ]),
          ],
        ),
      );
      bool selected(String name) =>
          tester.widget<ListTile>(find.widgetWithText(ListTile, name)).selected;
      expect(selected('Multitap'), isTrue);
      await tester.tap(find.widgetWithText(ListTile, 'AAA Fixture'));
      await tester.enterText(find.byKey(const Key('license-search')), 'common');
      await tester.pump();
      expect(selected('AAA Fixture'), isTrue);
      await tester.enterText(
        find.byKey(const Key('license-search')),
        'project-only',
      );
      await tester.pump();
      expect(selected('Multitap'), isTrue);
      await tester.enterText(
        find.byKey(const Key('license-search')),
        'no-match',
      );
      await tester.pump();
      await tester.tap(find.byTooltip('검색 지우기'));
      await tester.pump();
      expect(
        selected('AAA Fixture'),
        isTrue,
        reason: 'filter fallback does not reset user intent to project',
      );
    },
  );

  testWidgets(
    'selecting and searching keeps the entire original in SelectableText',
    (tester) async {
      await _show(tester);
      await tester.tap(find.widgetWithText(ListTile, 'Fixture Beta'));
      await tester.pump();
      final texts = tester.widgetList<SelectableText>(
        find.byType(SelectableText),
      );
      expect(texts.any((widget) => widget.data == _longOriginal), isTrue);
      await tester.enterText(
        find.byKey(const Key('license-search')),
        'needle-beta',
      );
      await tester.pump();
      expect(find.widgetWithText(ListTile, 'Fixture Alpha'), findsNothing);
      expect(find.widgetWithText(ListTile, 'Fixture Beta'), findsOneWidget);
      expect(
        tester
            .widgetList<SelectableText>(find.byType(SelectableText))
            .any((widget) => widget.data == _longOriginal),
        isTrue,
      );
      await tester.enterText(
        find.byKey(const Key('license-search')),
        'no-synthetic-match',
      );
      await tester.pump();
      expect(find.byKey(const Key('license-empty')), findsOneWidget);
      await tester.tap(find.byTooltip('검색 지우기'));
      await tester.pump();
      expect(find.widgetWithText(ListTile, 'Fixture Alpha'), findsOneWidget);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'Ctrl+F enters license search and Escape closes only the info dialog',
    (tester) async {
      await _show(tester, tab: AppInfoTab.information);
      expect(find.byKey(const Key('license-search')), findsNothing);
      await tester.sendKeyDownEvent(LogicalKeyboardKey.controlLeft);
      await tester.sendKeyEvent(LogicalKeyboardKey.keyF);
      await tester.sendKeyUpEvent(LogicalKeyboardKey.controlLeft);
      await tester.pumpAndSettle();
      expect(
        tester
            .widget<TextField>(find.byKey(const Key('license-search')))
            .focusNode!
            .hasFocus,
        isTrue,
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('client-info-dialog')), findsNothing);
      expect(find.byKey(const Key('open-fixture-info')), findsOneWidget);
    },
  );

  // 2026-11-10 (design audit F-05): two rows of information no longer sit
  // in a 960 x 686 window with a second, window-wide 닫기 under them.
  for (final brightness in Brightness.values) {
    testWidgets(
      'the information is as large as what it says; only the license '
      'browser takes the wide dialog (${brightness.name})',
      (tester) async {
        Rect surface() => tester.getRect(
          find
              .descendant(
                of: find.byType(Dialog),
                matching: find.byType(Material),
              )
              .first,
        );
        await _show(
          tester,
          tab: AppInfoTab.information,
          brightness: brightness,
          history: () {},
        );
        final information = surface();
        expect(information.width, 640);
        expect(information.height, lessThan(360));
        // The side list, not the tabs of a narrow window.
        expect(
          find.byKey(const Key('app-info-section-information')),
          findsOneWidget,
        );
        // One way to close (and Esc): the header X.
        expect(find.byKey(const Key('app-info-close')), findsNothing);
        expect(find.text('닫기'), findsNothing);
        expect(
          find.byKey(const Key('close-settings')).hitTestable(),
          findsOneWidget,
        );
        // Information asks for nothing: the grey icon.
        expect(
          tester.widget<AppToneIcon>(find.byType(AppToneIcon)).tone,
          AppTone.neutral,
        );

        await tester.tap(find.byKey(const Key('app-info-section-licenses')));
        await tester.pumpAndSettle();
        final browser = surface();
        expect(browser.width, 960);
        expect(browser.height, greaterThan(information.height + 200));
        expect(find.byKey(const Key('license-component-list')), findsOneWidget);
        expect(tester.takeException(), isNull);

        await tester.tap(
          find.byKey(const Key('app-info-section-information')),
        );
        await tester.pumpAndSettle();
        expect(surface().width, 640);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('client-info-dialog')), findsNothing);
      },
    );
  }

  testWidgets('업데이트 확인 and 변경 내역 are two buttons of one look', (
    tester,
  ) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1280, 800);
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.view.resetPhysicalSize);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: Scaffold(
          body: AppInfoDialog(
            loadVersion: () async => ClientBuildVersion.parse('9.8.7+65'),
            onCheckUpdates: () {},
            onReleaseHistory: () {},
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
    final check = find.byKey(const Key('app-info-check-updates'));
    final history = find.byKey(const Key('client-release-history'));
    expect(tester.widget(check), isA<OutlinedButton>());
    expect(tester.widget(history), isA<OutlinedButton>());
    Material material(Finder button) => tester.widget<Material>(
      find.descendant(of: button, matching: find.byType(Material)).first,
    );
    expect(material(check).shape, material(history).shape);
    expect(material(check).color, material(history).color);
    expect(tester.getSize(check).height, tester.getSize(history).height);
    expect(
      tester.getTopLeft(check).dy,
      tester.getTopLeft(history).dy,
      reason: 'side by side',
    );
  });

  testWidgets(
    'fresh EXE version and history are shown; unavailable version stays unknown',
    (tester) async {
      final previous = runningClientBuild;
      addTearDown(() => runningClientBuild = previous);
      runningClientBuild = ClientBuildVersion.parse('1.2.3+4');
      var histories = 0;
      await _show(
        tester,
        tab: AppInfoTab.information,
        history: () => histories++,
      );
      expect(
        tester
            .widgetList<SelectableText>(find.byType(SelectableText))
            .any((widget) => widget.data == '9.8.7'),
        isTrue,
      );
      await tester.tap(find.byKey(const Key('client-release-history')));
      expect(histories, 1);
      await tester.tap(find.byKey(const Key('close-settings')));
      await tester.pumpAndSettle();
      await _show(
        tester,
        tab: AppInfoTab.information,
        version: () async => throw StateError('synthetic_unavailable_version'),
      );
      final values = tester
          .widgetList<SelectableText>(find.byType(SelectableText))
          .map((widget) => widget.data);
      expect(values, contains('버전 확인 필요'));
      expect(values, isNot(contains('1.2.3')));
      expect(values, isNot(contains('0.4.103')));
    },
  );

  testWidgets(
    'an incomplete catalog remains actionable and retry can recover',
    (tester) async {
      var reads = 0;
      await _show(
        tester,
        load: () async {
          reads++;
          return reads == 1
              ? AppLicenseCatalog(
                  components: const [],
                  issues: const [AppLicenseIssue.nativeManifestUnavailable],
                )
              : _catalog();
        },
      );
      expect(find.byKey(const Key('license-load-warning')), findsOneWidget);
      await tester.tap(find.text('다시 불러오기'));
      await tester.pumpAndSettle();
      expect(reads, 2);
      expect(find.byKey(const Key('license-load-warning')), findsNothing);
      expect(find.widgetWithText(ListTile, 'Fixture Alpha'), findsOneWidget);
    },
  );

  testWidgets('closing while read-only loaders are held ignores late results', (
    tester,
  ) async {
    final licenses = Completer<AppLicenseCatalog>();
    final version = Completer<ClientBuildVersion>();
    await _show(
      tester,
      load: () => licenses.future,
      version: () => version.future,
      settle: false,
    );
    await tester.tap(find.byKey(const Key('close-settings')));
    await tester.pump(const Duration(milliseconds: 300));
    licenses.complete(_catalog());
    version.complete(ClientBuildVersion.parse('9.8.7+65'));
    await tester.pump();
    expect(find.byKey(const Key('client-info-dialog')), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets(
    'sign-in has a compact information entry without starting login',
    (tester) async {
      const channel = MethodChannel('multitap/window');
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(channel, (_) async => '9.8.7+65');
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      var logins = 0;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: SignInScreen(
            onLogin: (_, _) async {
              logins++;
              return SignInResult.rejected;
            },
            serverOrigin: null,
            onServerChanged: (_) async {},
            loginMemory: TestLoginMemory(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      await tester.ensureVisible(find.byKey(const Key('login-app-info')));
      await tester.tap(find.byKey(const Key('login-app-info')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('client-info-dialog')), findsOneWidget);
      expect(logins, 0);
    },
  );

  testWidgets('closing information cancels a pending version deadline', (
    tester,
  ) async {
    final held = Completer<ClientBuildVersion>();
    await _show(
      tester,
      tab: AppInfoTab.information,
      version: () => held.future,
    );
    await tester.tap(find.byKey(const Key('close-settings')));
    await tester.pumpAndSettle();
    await tester.pumpWidget(const SizedBox.shrink());
    expect(tester.takeException(), isNull);
    // Leave the source read unresolved: the framework also checks that no
    // deadline timer survives the disposed dialog at the end of this test.
  });

  testWidgets('an expired version observation rejects a late response', (
    tester,
  ) async {
    final held = Completer<ClientBuildVersion>();
    await _show(
      tester,
      tab: AppInfoTab.information,
      version: () => held.future,
    );
    await tester.pump(const Duration(seconds: 4));
    held.complete(ClientBuildVersion.parse('9.8.7+65'));
    await tester.pumpAndSettle();
    final values = tester
        .widgetList<SelectableText>(find.byType(SelectableText))
        .map((widget) => widget.data);
    expect(values, contains('버전 확인 필요'));
    expect(values, isNot(contains('9.8.7')));
    await tester.pumpWidget(const SizedBox.shrink());
  });

  testWidgets(
    'Host information keeps its manifest and uses fresh tool version without a new request',
    (tester) async {
      final api = FakeHostSettings()..statusOverrides['version'] = '0.2.77';
      final guard = FakeCloseGuard();
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: HostSettingsScreen(api: api, closeGuard: guard),
        ),
      );
      await tester.pumpAndSettle();
      final requests = api.requests.length;
      await tester.tap(find.byKey(const Key('host-header-info')));
      await tester.pumpAndSettle();
      final dialog = tester.widget<AppInfoDialog>(find.byType(AppInfoDialog));
      expect(dialog.manifestAsset, 'assets/licenses/host_native_notices.json');
      expect(find.text('Host 도구 버전'), findsOneWidget);
      expect(
        tester
            .widgetList<SelectableText>(find.byType(SelectableText))
            .any((widget) => widget.data == '0.2.77'),
        isTrue,
      );
      expect(api.requests.length, requests);
      await tester.tap(find.byKey(const Key('close-settings')));
      await tester.pumpAndSettle();
      expect(guard.closes, 0);
      expect(find.byKey(const Key('host-header-info')), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'unavailable Host tool version never borrows the client GUI version',
    (tester) async {
      final previous = runningClientBuild;
      addTearDown(() => runningClientBuild = previous);
      runningClientBuild = ClientBuildVersion.parse('9.8.7+65');
      final api = FakeHostSettings()..failStatus = true;
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: HostSettingsScreen(api: api, closeGuard: FakeCloseGuard()),
        ),
      );
      await tester.pumpAndSettle();
      final requests = api.requests.length;
      await tester.tap(find.byKey(const Key('host-header-info')));
      await tester.pumpAndSettle();
      expect(find.text('Host 도구 버전'), findsOneWidget);
      final values = tester
          .widgetList<SelectableText>(find.byType(SelectableText))
          .map((widget) => widget.data);
      expect(values, contains('버전 확인 필요'));
      expect(values, isNot(contains('9.8.7')));
      expect(api.requests.length, requests);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );
}

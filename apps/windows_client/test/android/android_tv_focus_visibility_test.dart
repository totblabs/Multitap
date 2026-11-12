// Local Flutter focus and actions only. The native editor reply is faked;
// screenshots do not establish physical TV or IME usability.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_application.dart';
import 'package:multitap_client/android/android_launcher.dart';
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/data/broker_probe.dart';
import 'package:multitap_client/ui/app_theme.dart';

bool get _baseline => Platform.environment['TV_FOCUS_BASELINE'] == 'true';

class _Login {
  final id = TextEditingController(text: 'tv-fixture');
  final password = TextEditingController();
  bool remembered = false;
  int server = 0, login = 0, updates = 0, rememberChanges = 0;

  Widget view({bool configured = true}) => StatefulBuilder(
    builder: (_, change) => AndroidSignInView(
      origin: configured ? Uri.parse('https://fixture.invalid') : null,
      id: id,
      password: password,
      remember: remembered,
      busy: false,
      version: null,
      onServer: () {
        server++;
      },
      onIdChanged: (_) {},
      onRemember: (value) => change(() {
        remembered = value;
        rememberChanges++;
      }),
      onLogin: () {
        login++;
      },
      onUpdates: () {
        updates++;
      },
    ),
  );
  void dispose() {
    id.dispose();
    password.dispose();
  }
}

Widget _app(Widget home, GlobalKey boundary, Brightness brightness) =>
    RepaintBoundary(
      key: boundary,
      child: MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: buildAppTheme(brightness: brightness),
        builder: (_, child) =>
            AndroidTvNavigation(television: true, child: child!),
        home: Scaffold(body: home),
      ),
    );

void _viewport(WidgetTester tester, Size size) {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1;
  tester.platformDispatcher.textScaleFactorTestValue = 1.5;
  addTearDown(() {
    tester.view.reset();
    tester.platformDispatcher.clearTextScaleFactorTestValue();
  });
}

bool _related(Element first, Element second) {
  if (identical(first, second)) return true;
  var related = false;
  first.visitAncestorElements((ancestor) {
    if (identical(ancestor, second)) {
      related = true;
      return false;
    }
    return true;
  });
  return related;
}

Finder _owner(Finder target) => find
    .ancestor(
      of: target,
      matching: find.byWidgetPredicate(
        (widget) => widget.runtimeType.toString() == 'AndroidTvFocus',
      ),
    )
    .first;

bool _hasFocus(Finder target) {
  if (FocusManager.instance.primaryFocus is FocusScopeNode) return false;
  final context = FocusManager.instance.primaryFocus?.context;
  if (context is! Element) return false;
  final element = target.evaluate().single;
  return _related(context, element) || _related(element, context);
}

void _focused(WidgetTester tester, Finder target) {
  expect(FocusManager.instance.primaryFocus, isNot(isA<FocusScopeNode>()));
  final context = FocusManager.instance.primaryFocus?.context;
  expect(context, isNotNull);
  final element = target.evaluate().single;
  final focusedElement = context! as Element;
  expect(
    _related(focusedElement, element) || _related(element, focusedElement),
    isTrue,
    reason: 'the actual input focus belongs to the expected control',
  );
  expect(find.byKey(const Key('android-tv-focus-ring')), findsNothing);
  if (_baseline) return;
  final owner = _owner(target);
  expect(owner, findsOneWidget);
  final scale = tester.widget<AnimatedScale>(
    find.descendant(of: owner, matching: find.byType(AnimatedScale)).first,
  );
  expect(scale.scale, 1.025);
  final semantics = tester
      .widgetList<Semantics>(
        find.descendant(of: owner, matching: find.byType(Semantics)),
      )
      .firstWhere((widget) => widget.properties.selected != null);
  expect(semantics.properties.selected, isTrue);
  final active = tester
      .widgetList<AnimatedScale>(find.byType(AnimatedScale))
      .where((scale) => scale.scale > 1);
  expect(
    active,
    hasLength(1),
    reason: 'one control, rather than a window overlay, identifies the current focus',
  );
  final control = tester.widget(target);
  if (control is ButtonStyleButton) {
    final background = control.style?.backgroundColor?.resolve({
      WidgetState.focused,
    });
    final foreground = control.style?.foregroundColor?.resolve({
      WidgetState.focused,
    });
    expect(background, isNotNull);
    expect(foreground, isNotNull);
    final a = background!.computeLuminance(),
        b = foreground!.computeLuminance();
    final contrast = (a > b ? a + .05 : b + .05) / (a > b ? b + .05 : a + .05);
    expect(contrast, greaterThanOrEqualTo(4.5));
  }
}

Future<void> _focus(WidgetTester tester, Finder target) async {
  final widget = tester.widget(target);
  if (widget is TextField) {
    widget.focusNode!.requestFocus();
  } else {
    List<FocusNode> nodesIn(Finder scope) {
      final element = scope.evaluate().single;
      return FocusManager.instance.rootScope.descendants
          .where(
            (node) =>
                node is! FocusScopeNode &&
                node.context != null &&
                node.canRequestFocus &&
                !node.skipTraversal &&
                _related(node.context! as Element, element),
          )
          .toList();
    }

    var nodes = nodesIn(target);
    if (nodes.isEmpty) {
      final row = find
          .ancestor(of: target, matching: find.byType(InkWell))
          .first;
      nodes = nodesIn(row);
    }
    expect(nodes, isNotEmpty);
    nodes.first.requestFocus();
  }
  await tester.pumpAndSettle();
  _focused(tester, target);
}

Future<void> _key(WidgetTester tester, LogicalKeyboardKey key) async {
  await tester.sendKeyEvent(key);
  await tester.pumpAndSettle();
}

Future<void> _capture(
  WidgetTester tester,
  GlobalKey boundary,
  String name,
) async {
  final directory = Platform.environment['TV_FOCUS_CAPTURE_DIR'];
  if (directory == null) return;
  await tester.runAsync(() async {
    final image =
        await (boundary.currentContext!.findRenderObject()
                as RenderRepaintBoundary)
            .toImage(pixelRatio: 1);
    try {
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      await Directory(directory).create(recursive: true);
      await File('$directory/${_baseline ? 'before' : 'after'}-$name.png')
          .writeAsBytes(bytes!.buffer.asUint8List());
    } finally {
      image.dispose();
    }
  });
}

Future<void> _close(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox.shrink());
  await tester.pumpAndSettle();
}

Future<void> _server(
  WidgetTester tester,
  GlobalKey boundary,
  Brightness brightness, {
  required Future<void> Function(Uri) save,
  Future<BrokerProbeResult> Function(Uri)? probe,
}) async {
  await tester.pumpWidget(
    _app(
      Builder(
        builder: (context) => FilledButton(
          key: const Key('show-server'),
          onPressed: () => showDialog<Uri>(
            context: context,
            builder: (_) => AndroidServerDialog(
              initial: Uri.parse('https://fixture.invalid'),
              probe:
                  probe ??
                  (_) async =>
                      const BrokerProbeResult(version: 'fixture', elapsedMs: 1),
              save: save,
            ),
          ),
          child: const Text('서버 설정'),
        ),
      ),
      boundary,
      brightness,
    ),
  );
  await tester.tap(find.byKey(const Key('show-server')));
  await tester.pumpAndSettle();
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  var nativeOpens = 0;
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
  setUp(() {
    nativeOpens = 0;
    AndroidApplication.hardwareNavigation = false;
    AndroidApplication.imeVisible.value = false;
    AndroidApplication.imeInsetVisible.value = false;
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidApplication.channel, (call) async {
          if (call.method == 'tvEditText') nativeOpens++;
          return null; // Cancel: no field edits or actual IME.
        });
  });
  tearDown(
    () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidApplication.channel, null),
  );

  testWidgets(
    'TV35 login D-pad identifies one control and OK invokes only its action',
    (tester) async {
      _viewport(tester, const Size(960, 540));
      final login = _Login();
      addTearDown(login.dispose);
      await tester.pumpWidget(_app(login.view(), GlobalKey(), Brightness.dark));
      await tester.pumpAndSettle();
      final server = find.byKey(const Key('open-server-settings'));
      _focused(tester, server);
      await _key(tester, LogicalKeyboardKey.select);
      expect(login.server, 1);
      await _key(tester, LogicalKeyboardKey.arrowDown);
      _focused(tester, find.byKey(const Key('sign-in-id')));
      await _key(tester, LogicalKeyboardKey.select);
      expect(nativeOpens, 1);
      await _key(tester, LogicalKeyboardKey.arrowDown);
      _focused(tester, find.byKey(const Key('sign-in-password')));
      await _key(tester, LogicalKeyboardKey.select);
      expect(nativeOpens, 2);
      await _key(tester, LogicalKeyboardKey.arrowDown);
      _focused(tester, find.byKey(const Key('remember-password')));
      await _key(tester, LogicalKeyboardKey.select);
      expect(login.remembered, isTrue);
      expect(login.rememberChanges, 1);
      await _key(tester, LogicalKeyboardKey.arrowDown);
      _focused(tester, find.byKey(const Key('login-submit')));
      await _key(tester, LogicalKeyboardKey.select);
      expect(login.login, 1);
      await _key(tester, LogicalKeyboardKey.arrowLeft);
      _focused(tester, find.byKey(const Key('login-update-button')));
      await _key(tester, LogicalKeyboardKey.select);
      expect(login.updates, 1);
      expect(login.server, 1);
      expect(login.login, 1);
      expect(nativeOpens, 2);
      expect(tester.takeException(), isNull);
      await _close(tester);
    },
  );

  testWidgets(
    'TV35 unconfigured login skips disabled fields and remember on the D-pad',
    (tester) async {
      _viewport(tester, const Size(960, 540));
      final login = _Login();
      addTearDown(login.dispose);
      await tester.pumpWidget(
        _app(login.view(configured: false), GlobalKey(), Brightness.light),
      );
      await tester.pumpAndSettle();
      await _key(tester, LogicalKeyboardKey.arrowDown);
      _focused(tester, find.byKey(const Key('login-submit')));
      await _key(tester, LogicalKeyboardKey.select);
      expect(login.server, 1);
      expect(login.rememberChanges, 0);
      expect(nativeOpens, 0);
      expect(tester.takeException(), isNull);
      await _close(tester);
    },
  );

  testWidgets(
    'TV35 server dialog D-pad preserves test save and cancel actions',
    (tester) async {
      _viewport(tester, const Size(960, 540));
      final saved = <Uri>[];
      await _server(
        tester,
        GlobalKey(),
        Brightness.dark,
        save: (uri) async {
          saved.add(uri);
        },
      );
      _focused(tester, find.byType(TextField));
      final test = find.widgetWithText(OutlinedButton, '연결 테스트');
      final cancel = find.widgetWithText(TextButton, '취소');
      final save = find.widgetWithText(FilledButton, '저장');
      expect(tester.widget<FilledButton>(save).onPressed, isNull);
      await _key(tester, LogicalKeyboardKey.arrowDown);
      _focused(tester, test);
      await _key(tester, LogicalKeyboardKey.select);
      _focused(tester, save);
      expect(
        tester.widget<FilledButton>(save).focusNode!.hasFocus,
        isTrue,
        reason: 'a completed successful test points to its next action',
      );
      await _focus(tester, find.byType(TextField));
      await _key(tester, LogicalKeyboardKey.arrowDown);
      _focused(tester, test);
      await _key(tester, LogicalKeyboardKey.arrowDown);
      // A full-width test action is centered above two footer actions. Either
      // equally close footer may receive Down; both must remain reachable.
      if (!_hasFocus(cancel)) {
        _focused(tester, save);
        await _key(tester, LogicalKeyboardKey.arrowLeft);
      }
      _focused(tester, cancel);
      await _key(tester, LogicalKeyboardKey.arrowRight);
      _focused(tester, save);
      await _key(tester, LogicalKeyboardKey.select);
      expect(saved, [Uri.parse('https://fixture.invalid')]);
      expect(find.text('로그인 서버'), findsNothing);
      await tester.tap(find.byKey(const Key('show-server')));
      await tester.pumpAndSettle();
      await _key(tester, LogicalKeyboardKey.arrowDown);
      await _key(tester, LogicalKeyboardKey.arrowDown);
      _focused(tester, cancel);
      await _key(tester, LogicalKeyboardKey.select);
      expect(saved, hasLength(1));
      expect(find.text('로그인 서버'), findsNothing);
      expect(tester.takeException(), isNull);
      await _close(tester);
    },
  );

  testWidgets(
    'TV35 failed server test visibly returns to retry instead of leaving focus in the scope',
    (tester) async {
      _viewport(tester, const Size(960, 540));
      var probes = 0, saves = 0;
      await _server(
        tester,
        GlobalKey(),
        Brightness.light,
        save: (_) async {
          saves++;
        },
        probe: (_) async {
          probes++;
          throw const BrokerProbeException('wrong_server');
        },
      );
      final test = find.widgetWithText(OutlinedButton, '연결 테스트');
      final save = find.widgetWithText(FilledButton, '저장');
      await _key(tester, LogicalKeyboardKey.arrowDown);
      await _key(tester, LogicalKeyboardKey.select);
      _focused(tester, test);
      expect(tester.widget<OutlinedButton>(test).focusNode!.hasFocus, isTrue);
      expect(tester.widget<FilledButton>(save).onPressed, isNull);
      await _key(tester, LogicalKeyboardKey.select);
      expect(probes, 2);
      expect(saves, 0);
      _focused(tester, test);
      expect(tester.takeException(), isNull);
      await _close(tester);
    },
  );

  for (final spec in [
    (const Size(960, 540), Brightness.dark),
    (const Size(1920, 1080), Brightness.light),
  ]) {
    testWidgets('TV35 focus render ${spec.$1.width.toInt()} ${spec.$2.name}', (
      tester,
    ) async {
      _viewport(tester, spec.$1);
      final shadows = debugDisableShadows;
      debugDisableShadows = false;
      addTearDown(() => debugDisableShadows = shadows);
      try {
        final login = _Login();
        addTearDown(login.dispose);
        final boundary = GlobalKey();
        await tester.pumpWidget(_app(login.view(), boundary, spec.$2));
        await tester.pumpAndSettle();
        await tester.runAsync(() async {
          final image = find.byKey(const Key('scene-background-image'));
          await precacheImage(
            tester.widget<Image>(image).image,
            tester.element(image),
          );
        });
        await tester.pumpAndSettle();
        await _key(
          tester,
          LogicalKeyboardKey.arrowDown,
        ); // Actual keyboard focus modality.
        for (final target in [
          ('server', 'open-server-settings'),
          ('id', 'sign-in-id'),
          ('password', 'sign-in-password'),
          ('remember', 'remember-password'),
          ('login', 'login-submit'),
          ('update', 'login-update-button'),
        ]) {
          final control = find.byKey(Key(target.$2));
          await _focus(tester, control);
          final rect = tester.getRect(control);
          expect(rect.left, greaterThanOrEqualTo(0));
          expect(rect.right, lessThanOrEqualTo(spec.$1.width));
          expect(rect.top, greaterThanOrEqualTo(0));
          expect(rect.bottom, lessThanOrEqualTo(spec.$1.height));
          await _capture(
            tester,
            boundary,
            'login-${target.$1}-${spec.$1.width.toInt()}-${spec.$2.name}-150',
          );
        }
        await _close(tester);
        final serverBoundary = GlobalKey();
        await _server(tester, serverBoundary, spec.$2, save: (_) async {});
        for (final target in [
          ('address', find.byType(TextField)),
          ('test', find.widgetWithText(OutlinedButton, '연결 테스트')),
          ('cancel', find.widgetWithText(TextButton, '취소')),
        ]) {
          await _focus(tester, target.$2);
          await _capture(
            tester,
            serverBoundary,
            'server-${target.$1}-${spec.$1.width.toInt()}-${spec.$2.name}-150',
          );
        }
        await _focus(tester, find.widgetWithText(OutlinedButton, '연결 테스트'));
        await _key(tester, LogicalKeyboardKey.select);
        await _focus(tester, find.widgetWithText(FilledButton, '저장'));
        await _capture(
          tester,
          serverBoundary,
          'server-save-${spec.$1.width.toInt()}-${spec.$2.name}-150',
        );
        expect(tester.takeException(), isNull);
        await _close(tester);
      } finally {
        debugDisableShadows = shadows;
      }
    });
  }
}

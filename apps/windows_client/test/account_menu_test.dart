// The account menu's design: its entries and groups, one row grid, neutral icons
// with red only on the rows that end what is open, no highlight on open but
// the keyboard focus ring, and a layout that holds at every window width,
// text size and theme. Widget renders only: nothing is signed out or closed.
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/ui/app_dialog.dart' show AppToneIcon;
import 'package:multitap_client/ui/app_exit.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/themed_app.dart';
import 'package:multitap_client/ui/workspace_header.dart';

/// The commands, top to bottom, and the two choice rows between them.
const _commands = ['open-settings', 'open-client-log', 'sign-out', 'quit-app'];
const _order = [
  'account-identity',
  'open-settings',
  'open-client-log',
  'account-theme',
  'account-scene',
  'sign-out',
  'quit-app',
];
const _destructive = {'sign-out', 'quit-app'};

Future<AppAppearance> _show(
  WidgetTester t, {
  Size size = const Size(1280, 800),
  double scale = 1,
  ThemeMode mode = ThemeMode.dark,
  List<String>? pressed,
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = size;
  t.platformDispatcher.textScaleFactorTestValue = scale;
  addTearDown(t.view.reset);
  addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
  final appearance = AppAppearance(mode: mode);
  addTearDown(appearance.dispose);
  VoidCallback press(String what) =>
      () => pressed?.add(what);
  await t.pumpWidget(
    ThemedApp(
      key: UniqueKey(),
      appearance: appearance,
      home: AppExitScope(
        requestExit: () async => pressed?.add('quit'),
        child: Scaffold(
          body: Column(
            children: [
              WorkspaceHeader(
                side: 16,
                columnWidth: 1400,
                isDemo: false,
                accountLabel: '백종헌',
                server: 'vdi.example.com',
                version: 'v0.4.177',
                update: null,
                onUpdates: () {},
                onClientInfo: press('info'),
                onLog: press('log'),
                onChangePassword: press('password'),
                onSignOut: press('sign-out'),
              ),
            ],
          ),
        ),
      ),
    ),
  );
  await t.pumpAndSettle();
  return appearance;
}

Finder _key(String key) => find.byKey(Key(key));
Rect _rect(WidgetTester t, String key) => t.getRect(_key(key));

Future<void> _open(WidgetTester t, {PointerDeviceKind? kind}) async {
  await t.tap(_key('workspace-account'), kind: kind ?? PointerDeviceKind.touch);
  await t.pumpAndSettle();
}

/// The keyboard focus is on [key] or inside it.
bool _hasFocus(String key) {
  final focused = FocusManager.instance.primaryFocus?.context;
  final target = _key(key).evaluate().singleOrNull;
  if (focused == null || target == null) return false;
  if (focused == target) return true;
  var inside = false;
  focused.visitAncestorElements((ancestor) {
    inside = ancestor == target;
    return !inside;
  });
  return inside;
}

/// The painted outline of a command row (its Material's shape side).
BorderSide _outline(WidgetTester t, String key) {
  final material = t.widget<Material>(
    find.descendant(of: _key(key), matching: find.byType(Material)).first,
  );
  return (material.shape! as OutlinedBorder).side;
}

/// The colour a row's icon and label are painted in.
(Color?, Color?) _ink(WidgetTester t, String key) {
  final icon = t.element(
    find.descendant(of: _key(key), matching: find.byType(Icon)).first,
  );
  final label = t.renderObject<RenderParagraph>(
    find.descendant(of: _key(key), matching: find.byType(RichText)).last,
  );
  return (
    (icon.widget as Icon).color ?? IconTheme.of(icon).color,
    label.text.style?.color,
  );
}

void main() {
  testWidgets(
    'the entries keep their order in four groups under the identity',
    (t) async {
      await _show(t);
      await _open(t);
      final tops = [for (final key in _order) _rect(t, key).top];
      expect(tops, orderedEquals([...tops]..sort()), reason: '$tops');
      expect(find.text('정보·라이선스'), findsOneWidget);
      expect(find.text('완전 종료'), findsOneWidget);
      // 앱 | 모양 | 계정 | 완전 종료: one hairline between each group.
      final rules = [
        for (final element in find.byType(Divider).evaluate())
          t.getRect(find.byWidget(element.widget)).center.dy,
      ]..sort();
      expect(rules, hasLength(3));
      for (final (i, (above, below)) in [
        ('open-client-log', 'account-theme'),
        ('account-scene', 'sign-out'),
        ('sign-out', 'quit-app'),
      ].indexed) {
        expect(rules[i], greaterThan(_rect(t, above).bottom), reason: above);
        expect(rules[i], lessThan(_rect(t, below).top), reason: below);
      }
      // The server and the running build, muted and whole.
      final server = t.widget<Text>(_key('account-server'));
      expect(server.data, 'vdi.example.com · v0.4.177');
      expect(server.maxLines, isNull);
      expect(server.overflow, isNot(TextOverflow.ellipsis));
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'a click opens it with nothing highlighted; the keyboard shows only the focus ring',
    (t) async {
      await _show(t);
      await _open(t, kind: PointerDeviceKind.mouse);
      // The mouse leaves Flutter in its keyboard highlight mode, which used to
      // focus (and so highlight) the first row on every open.
      expect(
        FocusManager.instance.highlightMode,
        FocusHighlightMode.traditional,
      );
      for (final key in _commands) {
        expect(_outline(t, key), BorderSide.none, reason: key);
        expect(_hasFocus(key), isFalse, reason: key);
      }
      expect(
        FocusManager.instance.primaryFocus?.debugLabel,
        'account-menu-button',
      );
      await t.sendKeyEvent(LogicalKeyboardKey.escape);
      await t.pumpAndSettle();
      expect(_key('sign-out'), findsNothing);
      // Enter on the focused button: the first row takes focus and its ring.
      expect(
        FocusManager.instance.primaryFocus?.debugLabel,
        'account-menu-button',
      );
      await t.sendKeyEvent(LogicalKeyboardKey.enter);
      await t.pumpAndSettle();
      final p = AppPalette(true);
      expect(_outline(t, 'open-settings').color, p.amber);
      for (final key in _commands.skip(1)) {
        expect(_outline(t, key), BorderSide.none, reason: key);
      }
      expect(t.takeException(), isNull);
    },
  );

  testWidgets('the keyboard walks every row and both choices', (t) async {
    final appearance = await _show(t);
    t.widget<TextButton>(_key('workspace-account')).focusNode!.requestFocus();
    await t.pump();
    await t.sendKeyEvent(LogicalKeyboardKey.enter);
    await t.pumpAndSettle();
    String? focused() => [
      ..._commands,
      'theme-dark',
      'theme-light',
    ].where(_hasFocus).firstOrNull;

    expect(focused(), 'open-settings');
    await t.sendKeyEvent(LogicalKeyboardKey.arrowDown);
    await t.pumpAndSettle();
    expect(focused(), 'open-client-log');
    await t.sendKeyEvent(LogicalKeyboardKey.arrowDown);
    await t.pumpAndSettle();
    expect(focused(), anyOf('theme-dark', 'theme-light'));
    // Across the track to 라이트, and Enter chooses it with the menu open.
    if (focused() == 'theme-dark') {
      await t.sendKeyEvent(LogicalKeyboardKey.arrowRight);
      await t.pumpAndSettle();
    }
    expect(focused(), 'theme-light');
    await t.sendKeyEvent(LogicalKeyboardKey.enter);
    await t.pumpAndSettle();
    expect(appearance.mode, ThemeMode.light);
    expect(_key('sign-out'), findsOneWidget);
    // Escape closes and comes back to the button.
    await t.sendKeyEvent(LogicalKeyboardKey.escape);
    await t.pumpAndSettle();
    expect(_key('sign-out'), findsNothing);
    expect(
      FocusManager.instance.primaryFocus?.debugLabel,
      'account-menu-button',
    );
    await t.sendKeyEvent(LogicalKeyboardKey.enter);
    await t.pumpAndSettle();
    expect(focused(), 'open-settings');
    await t.sendKeyEvent(LogicalKeyboardKey.escape);
    await t.pumpAndSettle();
    expect(t.takeException(), isNull);
  });

  for (final mode in [ThemeMode.dark, ThemeMode.light]) {
    testWidgets(
      'only 로그아웃 and 완전 종료 are red (${mode.name}); every other icon is neutral',
      (t) async {
        await _show(t, mode: mode);
        await _open(t);
        final p = AppPalette(mode == ThemeMode.dark);
        for (final key in _commands) {
          final (icon, label) = _ink(t, key);
          if (_destructive.contains(key)) {
            expect(icon, p.red, reason: key);
            expect(label, p.red, reason: key);
          } else {
            expect(icon, p.muted, reason: key);
            expect(label, p.ink, reason: key);
          }
        }
        for (final key in ['account-theme', 'account-scene']) {
          final icon = t.widget<Icon>(
            find.descendant(of: _key(key), matching: find.byType(Icon)).first,
          );
          expect(icon.color, p.muted, reason: key);
        }
        // Red appears nowhere else in the menu.
        final reds = find.byWidgetPredicate(
          (w) =>
              (w is Icon && w.color == p.red) ||
              (w is RichText && w.text.style?.color == p.red),
        );
        for (final element in reds.evaluate()) {
          final inside = _destructive.any(
            (key) => find
                .descendant(
                  of: _key(key),
                  matching: find.byWidget(element.widget),
                )
                .evaluate()
                .isNotEmpty,
          );
          expect(inside, isTrue, reason: '${element.widget}');
        }
        expect(t.takeException(), isNull);
      },
    );
  }

  for (final size in const [Size(360, 740), Size(800, 600), Size(1280, 800)]) {
    for (final scale in const [1.0, 1.5, 2.0]) {
      for (final mode in [ThemeMode.dark, ThemeMode.light]) {
        testWidgets(
          'one row grid at ${size.width.toInt()} px, text ${(scale * 100).round()} %, ${mode.name}',
          (t) async {
            await _show(t, size: size, scale: scale, mode: mode);
            await _open(t);
            expect(t.takeException(), isNull);
            // Every command row has one height, at least 36 px.
            final heights = {
              for (final key in _commands)
                _rect(t, key).height.toStringAsFixed(2),
            };
            expect(heights, hasLength(1), reason: '$heights');
            final row = _rect(t, _commands.first).height;
            expect(row, greaterThanOrEqualTo(36));
            // The choice rows: the same height, or twice it with the track
            // under its label where it does not fit beside it.
            final theme = _rect(t, 'account-theme').height;
            expect(theme, closeTo(_rect(t, 'account-scene').height, .01));
            expect(
              theme,
              anyOf(closeTo(row, .01), closeTo(2 * row, .01)),
              reason: 'theme row $theme',
            );
            // Both tracks line up, and so do the icon and label columns.
            expect(_rect(t, 'theme-dark').left, _rect(t, 'scene-peaks').left);
            expect(
              _rect(t, 'theme-light').right,
              _rect(t, 'scene-meadow').right,
            );
            double left(Finder finder) => t.getRect(finder.first).left;
            Finder iconIn(String key) =>
                find.descendant(of: _key(key), matching: find.byType(Icon));
            final icons = {
              for (final key in [
                ..._commands,
                'account-theme',
                'account-scene',
              ])
                left(iconIn(key)),
            };
            expect(icons, hasLength(1), reason: 'icon column $icons');
            final labels = {
              for (final text in ['정보·라이선스', '테마', '배경', '로그아웃', '완전 종료'])
                left(find.text(text)),
            };
            expect(labels, hasLength(1), reason: 'label column $labels');
            // The segments take the whole row height (the hit area).
            expect(_rect(t, 'theme-dark').height, closeTo(row, .01));
            // Labels on one line and whole; the server line wraps, never cut.
            for (final element
                in find
                    .descendant(
                      of: find.byType(MenuItemButton),
                      matching: find.byType(RichText),
                    )
                    .evaluate()) {
              final paragraph = element.renderObject! as RenderParagraph;
              expect(paragraph.didExceedMaxLines, isFalse);
            }
            final server = t.renderObject<RenderParagraph>(
              find
                  .descendant(
                    of: _key('account-server'),
                    matching: find.byType(RichText),
                  )
                  .first,
            );
            expect(server.didExceedMaxLines, isFalse);
            // Inside the window, right-aligned under the button.
            final panel = _rect(t, 'account-identity');
            expect(panel.left, greaterThanOrEqualTo(0));
            expect(panel.right, lessThanOrEqualTo(size.width));
            expect(
              panel.right + 6,
              closeTo(_rect(t, 'workspace-account').right, .5),
            );
            // Every control can be reached (the menu scrolls when it must).
            for (final key in [
              ..._commands,
              'theme-dark',
              'theme-light',
              'scene-peaks',
              'scene-meadow',
            ]) {
              await t.ensureVisible(_key(key));
              await t.pump();
              expect(_key(key).hitTestable(), findsOneWidget, reason: key);
            }
            expect(t.takeException(), isNull);
          },
        );
      }
    }
  }

  testWidgets('a command closes the menu and runs once', (t) async {
    final pressed = <String>[];
    await _show(t, pressed: pressed);
    for (final (key, what) in [
      ('open-settings', 'info'),
      ('open-client-log', 'log'),
      ('sign-out', 'sign-out'),
      ('quit-app', 'quit'),
    ]) {
      await _open(t);
      await t.tap(_key(key));
      await t.pumpAndSettle();
      expect(_key(key), findsNothing, reason: key);
      expect(pressed.last, what);
    }
    expect(pressed, hasLength(4));
  });

  // 설정: the ⚙ right of the timer, left of the account button, opens a
  // window with 언어 / Language and 비밀번호 변경; the account menu does not
  // have them.
  testWidgets('the settings button opens 언어 and 비밀번호 변경', (t) async {
    final pressed = <String>[];
    await _show(t, pressed: pressed);
    await _open(t);
    expect(_key('account-change-password'), findsNothing);
    expect(_key('account-language'), findsNothing);
    await t.sendKeyEvent(LogicalKeyboardKey.escape);
    await t.pumpAndSettle();
    final gear = t.getRect(_key('open-app-settings'));
    expect(
      gear.right,
      lessThanOrEqualTo(t.getRect(_key('workspace-account')).left),
    );
    await t.tap(_key('open-app-settings'));
    await t.pumpAndSettle();
    expect(_key('app-settings-dialog'), findsOneWidget);
    expect(find.text('설정'), findsWidgets);
    expect(_key('account-language'), findsOneWidget);
    expect(_key('language-auto'), findsOneWidget);
    // A plain heading and plain rows: no gear in a circle by
    // the title, no icon before 언어 or any other row. The close × is the
    // only icon left in the window.
    final dialog = _key('app-settings-dialog');
    expect(
      find.descendant(of: dialog, matching: find.byType(AppToneIcon)),
      findsNothing,
    );
    final icons = t
        .widgetList<Icon>(find.descendant(of: dialog, matching: find.byType(Icon)))
        .map((icon) => icon.icon)
        .toList();
    expect(icons, everyElement(Icons.close_rounded));
    final language = find.descendant(
      of: _key('account-language'),
      matching: find.text('언어'),
    );
    final password = find.descendant(
      of: _key('account-change-password'),
      matching: find.text('비밀번호 변경'),
    );
    // The labels start the rows, one under the other.
    expect(
      t.getRect(language).left,
      closeTo(t.getRect(password).left, 1),
    );
    await t.tap(_key('account-change-password'));
    await t.pumpAndSettle();
    expect(_key('app-settings-dialog'), findsNothing);
    expect(pressed, ['password']);
    expect(t.takeException(), isNull);
  });
}

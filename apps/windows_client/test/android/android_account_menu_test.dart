// The Android account menu is the Windows one by finger: the
// same groups and order, one 48 dp row grid with the 테마/배경 choices on it,
// neutral icons with red only on 로그아웃 and 완전 종료, nothing highlighted
// on open, and Back closes it before anything else. Widget renders with
// fakes; nothing is signed out or closed.
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/ui/app_theme.dart';

import 'android_launcher_parity_test.dart' show pumpApp;
import 'android_launcher_test.dart' show FakeBroker, vm;
import 'android_menu_parity_test.dart' show boot, disposeApp;

const _commands = [
  'android-update-menu',
  'android-log-menu',
  'sign-out',
  'android-exit-menu',
];
const _destructive = {'sign-out', 'android-exit-menu'};

Finder _key(String key) => find.byKey(Key(key));
Rect _rect(WidgetTester t, String key) => t.getRect(_key(key));

Future<void> _open(WidgetTester t, {Size dp = const Size(412, 915)}) async {
  await pumpApp(t, FakeBroker([vm()]), dp: dp);
  await t.ensureVisible(_key('account-menu'));
  await t.pumpAndSettle();
  await t.tap(_key('account-menu'));
  await t.pumpAndSettle();
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('the PC menu by finger: groups, order and one 48 dp grid', (
    t,
  ) async {
    await _open(t);
    final order = [
      'account-identity',
      'android-update-menu',
      'android-log-menu',
      'account-theme',
      'account-scene',
      'sign-out',
      'android-exit-menu',
    ];
    final tops = [for (final key in order) _rect(t, key).top];
    expect(tops, orderedEquals([...tops]..sort()), reason: '$tops');
    final menu = Rect.fromLTRB(
      _rect(t, 'account-identity').left,
      _rect(t, 'account-identity').top,
      _rect(t, 'account-identity').right,
      _rect(t, 'android-exit-menu').bottom,
    );
    final popupMaterial = find
        .ancestor(of: _key('account-identity'), matching: find.byType(Material))
        .first;
    final rules = [
      for (final element
          in find
              .descendant(of: popupMaterial, matching: find.byType(Divider))
              .evaluate())
        if (menu.contains(t.getCenter(find.byWidget(element.widget)))) element,
    ];
    expect(rules, hasLength(3));
    // No separate 다크 · 밤 풍경 … rows: the PC's two choice rows.
    expect(find.text('다크 · 밤 풍경'), findsNothing);
    expect(find.text('테마'), findsOneWidget);
    expect(find.text('배경'), findsOneWidget);
    // One row height, at least 48 dp; the choices take the whole row.
    final heights = {
      for (final key in [..._commands, 'account-theme', 'account-scene'])
        _rect(t, key).height.toStringAsFixed(2),
    };
    expect(heights, hasLength(1), reason: '$heights');
    final row = _rect(t, 'sign-out').height;
    expect(row, greaterThanOrEqualTo(48));
    for (final key in ['theme-dark', 'theme-light', 'scene-peaks']) {
      expect(_rect(t, key).height, closeTo(row, .01), reason: key);
      expect(_rect(t, key).width, greaterThanOrEqualTo(48), reason: key);
    }
    // One icon column and one label column.
    final icons = {
      for (final key in [..._commands, 'account-theme', 'account-scene'])
        t
            .getRect(
              find.descendant(of: _key(key), matching: find.byType(Icon)).first,
            )
            .left,
    };
    expect(icons, hasLength(1), reason: '$icons');
    final labels = {
      for (final text in ['정보·라이선스', '테마', '로그아웃', '완전 종료'])
        t.getRect(find.text(text)).left,
    };
    expect(labels, hasLength(1), reason: '$labels');
    // Opened by finger: no row has focus.
    final focused = FocusManager.instance.primaryFocus?.context;
    for (final key in _commands) {
      final row = _key(key).evaluate().single;
      var inside = focused == row;
      focused?.visitAncestorElements((a) {
        inside = inside || a == row;
        return !inside;
      });
      expect(inside, isFalse, reason: key);
    }
    expect(t.takeException(), isNull);
  });

  testWidgets('only 로그아웃 and 완전 종료 are red', (t) async {
    await _open(t);
    final p = AppPalette.of(t.element(_key('sign-out')));
    for (final key in _commands) {
      final icon = t.element(
        find.descendant(of: _key(key), matching: find.byType(Icon)).first,
      );
      final label = t.renderObject<RenderParagraph>(
        find.descendant(of: _key(key), matching: find.byType(RichText)).last,
      );
      final iconColor = (icon.widget as Icon).color ?? IconTheme.of(icon).color;
      if (_destructive.contains(key)) {
        expect(iconColor, p.red, reason: key);
        expect(label.text.style?.color, p.red, reason: key);
      } else {
        expect(iconColor, p.muted, reason: key);
        expect(label.text.style?.color, p.ink, reason: key);
      }
    }
  });

  testWidgets('a theme choice applies at once and the menu stays open', (
    t,
  ) async {
    await _open(t);
    final before = Theme.of(t.element(_key('sign-out'))).brightness;
    await t.tap(_key(before == Brightness.dark ? 'theme-light' : 'theme-dark'));
    await t.pumpAndSettle();
    expect(_key('sign-out'), findsOneWidget);
    expect(Theme.of(t.element(_key('sign-out'))).brightness, isNot(before));
  });

  testWidgets('Back closes the menu first and keeps the launcher', (t) async {
    await _open(t);
    expect(_key('sign-out'), findsOneWidget);
    await t.binding.handlePopRoute();
    await t.pumpAndSettle();
    expect(_key('sign-out'), findsNothing);
    expect(_key('workspace-header'), findsOneWidget);
    expect(t.takeException(), isNull);
  });

  testWidgets(
    'TV: OK opens it on the first row, painted as TV focus; the D-pad walks it',
    (t) async {
      await boot(t, size: const Size(960, 540), count: 1, television: true);
      final button = t.widget<TextButton>(_key('account-menu'));
      button.focusNode!.requestFocus();
      await t.pumpAndSettle();
      await t.sendKeyEvent(LogicalKeyboardKey.select);
      await t.pumpAndSettle();
      bool focused(String key) {
        final context = FocusManager.instance.primaryFocus?.context;
        final row = _key(key).evaluate().single;
        var inside = context == row;
        context?.visitAncestorElements((a) {
          inside = inside || a == row;
          return !inside;
        });
        return inside;
      }

      // 설정 is the header's own ⚙, so the menu opens on 정보·라이선스.
      expect(focused('android-update-menu'), isTrue);
      final material = t.widget<Material>(
        find
            .descendant(
              of: _key('android-update-menu'),
              matching: find.byType(Material),
            )
            .first,
      );
      expect(
        material.color,
        AndroidTvFocus.surface(t.element(_key('android-update-menu'))),
      );
      await t.sendKeyEvent(LogicalKeyboardKey.arrowDown);
      await t.pumpAndSettle();
      expect(focused('android-update-menu'), isFalse);
      await t.sendKeyEvent(LogicalKeyboardKey.escape);
      await t.pumpAndSettle();
      expect(_key('sign-out'), findsNothing);
      expect(t.takeException(), isNull);
      await disposeApp(t);
    },
  );

  testWidgets('the menu fits a 360 dp phone with 200 % text', (t) async {
    t.platformDispatcher.textScaleFactorTestValue = 2;
    addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
    await _open(t, dp: const Size(360, 800));
    expect(t.takeException(), isNull);
    final server = t.renderObject<RenderParagraph>(
      find
          .descendant(
            of: _key('account-server'),
            matching: find.byType(RichText),
          )
          .first,
    );
    expect(server.didExceedMaxLines, isFalse);
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
      expect(_rect(t, key).height, greaterThanOrEqualTo(48), reason: key);
    }
  });
}

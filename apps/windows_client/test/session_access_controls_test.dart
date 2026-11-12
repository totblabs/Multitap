import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/session_activity.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_access_controls.dart';

// Presentation collaborator only. No network, native session or auth changes.
class _Activity extends SessionActivity {
  bool known = true, enabled = true, writable = true, pending = false;
  Duration? left = const Duration(minutes: 23, seconds: 9);
  String? error, terminal;
  int changes = 0, humanCalls = 0;
  Completer<void>? response;

  @override
  bool get idleSupported => known;
  @override
  bool get idleEnabled => enabled;
  @override
  bool get canChangeIdle => writable && known && terminal == null;
  @override
  bool get idleChangePending => pending;
  @override
  String? get idleChangeError => error;
  @override
  String? get reason => terminal;
  @override
  Duration? get remaining => known && enabled ? left : null;
  @override
  void human() => humanCalls++;

  void changed() => notifyListeners();

  @override
  Future<void> setIdleEnabled(bool value) async {
    changes++;
    pending = true;
    error = null;
    notifyListeners();
    try {
      await response!.future;
      enabled = value;
    } catch (_) {
      error = 'test-only failure';
    } finally {
      pending = false;
      notifyListeners();
    }
  }
}

Future<void> _show(
  WidgetTester t,
  _Activity activity, {
  double width = 264,
  double scale = 1,
  bool dark = true,
  bool touch = false,
  bool compact = false,
}) => t.pumpWidget(
  MaterialApp(
    theme: buildAppTheme(brightness: dark ? Brightness.dark : Brightness.light),
    home: MediaQuery(
      data: MediaQueryData(textScaler: TextScaler.linear(scale)),
      child: Scaffold(
        body: Align(
          alignment: Alignment.topLeft,
          child: SizedBox(
            width: width,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                SessionIdleBadge(activity: activity, compact: compact),
                SessionIdleControl(activity: activity, touch: touch),
              ],
            ),
          ),
        ),
      ),
    ),
  ),
);

void main() {
  testWidgets(
    'deadline display follows notifications without generating activity',
    (t) async {
      final state = _Activity();
      addTearDown(state.dispose);
      await _show(t, state);
      expect(find.text('자동 로그아웃 23:09'), findsOneWidget);
      state.left = const Duration(minutes: 23, seconds: 8);
      state.changed();
      await t.pump();
      expect(find.text('자동 로그아웃 23:08'), findsOneWidget);
      await t.pump(const Duration(seconds: 3));
      expect(find.text('자동 로그아웃 23:08'), findsOneWidget);
      expect(state.humanCalls, 0);
      expect(state.changes, 0);
    },
  );

  testWidgets(
    'pending switch keeps confirmed value and refuses a second change',
    (t) async {
      final state = _Activity()..response = Completer<void>();
      addTearDown(state.dispose);
      await _show(t, state);
      await t.tap(find.byKey(const Key('session-idle-switch')));
      await t.pump();
      Switch control() => t.widget<Switch>(find.byType(Switch));
      expect(control().value, isTrue);
      expect(control().onChanged, isNull);
      expect(find.text('설정을 바꾸는 중이에요.'), findsOneWidget);
      await t.tap(find.byType(Switch));
      expect(state.changes, 1);
      state.response!.complete();
      await t.pump();
      expect(control().value, isFalse);
      expect(find.text('자동 로그아웃 꺼짐'), findsOneWidget);
      expect(state.humanCalls, 0);
    },
  );

  testWidgets('failed change keeps confirmed policy and offers retry', (
    t,
  ) async {
    final state = _Activity()..response = Completer<void>();
    addTearDown(state.dispose);
    await _show(t, state);
    await t.tap(find.byType(Switch));
    await t.pump();
    state.response!.completeError(StateError('synthetic transport failure'));
    await t.pump();
    final control = t.widget<Switch>(find.byType(Switch));
    expect(control.value, isTrue);
    expect(control.onChanged, isNotNull);
    expect(find.byKey(const Key('session-idle-error')), findsOneWidget);
    expect(find.textContaining('synthetic transport'), findsNothing);
  });

  testWidgets('unknown policy and read-only child never expose a switch', (
    t,
  ) async {
    final state = _Activity()..known = false;
    addTearDown(state.dispose);
    await _show(t, state);
    expect(find.text('자동 로그아웃 확인 중'), findsOneWidget);
    expect(find.textContaining('30:00'), findsNothing);
    expect(find.byType(Switch), findsNothing);
    state
      ..known = true
      ..writable = false;
    state.changed();
    await t.pump();
    expect(find.text('자동 로그아웃 23:09'), findsOneWidget);
    expect(find.byType(Switch), findsNothing);
    expect(state.changes, 0);
  });

  testWidgets(
    'last minute has warning colour and compact time retains seconds',
    (t) async {
      final state = _Activity()..left = const Duration(seconds: 60);
      addTearDown(state.dispose);
      await _show(t, state, compact: true);
      expect(find.text('1:00'), findsOneWidget);
      final badge = find.byKey(const Key('session-idle-badge'));
      final context = t.element(badge);
      final decoration =
          t.widget<Container>(badge).decoration! as BoxDecoration;
      expect(decoration.color, AppPalette.of(context).amberSoft);
      state.left = const Duration(milliseconds: 1);
      state.changed();
      await t.pump();
      expect(find.text('0:01'), findsOneWidget);
      state.terminal = 'session_idle_timeout';
      state.changed();
      await t.pump();
      expect(find.byKey(const Key('session-idle-badge')), findsNothing);
      expect(find.byType(Switch), findsNothing);
    },
  );

  for (final dark in [false, true]) {
    for (final touch in [false, true]) {
      testWidgets('220px menu with large text: dark=$dark touch=$touch', (
        t,
      ) async {
        final state = _Activity()..error = 'test-only failure';
        addTearDown(state.dispose);
        await _show(t, state, width: 220, scale: 2, dark: dark, touch: touch);
        expect(find.text('자동 로그아웃'), findsOneWidget);
        expect(find.byType(Switch), findsOneWidget);
        expect(find.byKey(const Key('session-idle-error')), findsOneWidget);
        expect(t.takeException(), isNull);
      });
    }
  }
}

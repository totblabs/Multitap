// Source-only proposed regressions. Actual SessionToolbar and popup route;
// no Native API, input injection, connection, or user data.
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_toolbar.dart';

Widget _harness({
  Object? contentKey,
  Object? layoutKey,
  required VoidCallback onCommand,
  PointerDownEventListener? onToolbarPointerDown,
}) => MaterialApp(
  theme: buildAppTheme(brightness: Brightness.dark),
  home: Scaffold(
    body: Listener(
      onPointerDown: onToolbarPointerDown,
      child: SessionToolbar(
        key: const Key('repeat-lifetime-toolbar'),
        title: 'SYNTHETIC VM',
        connected: true,
        latencyMs: 12,
        onInteract: () {},
        contentKey: contentKey,
        layoutKey: layoutKey,
        actions: [
          SessionToolbarMenu.command(
            key: const Key('repeat-lifetime-command'),
            label: '설정',
            icon: Icons.tune_rounded,
            onInteract: () {},
            onPressed: onCommand,
          ),
        ],
      ),
    ),
  ),
);

Future<void> _openQuality(WidgetTester tester, {bool pinned = false}) async {
  await tester.tap(find.byKey(const Key('expand-session-toolbar')));
  await tester.pumpAndSettle();
  if (pinned) {
    // 바 열어 두기 has no button on the bar (v2): the session's 설정 panel
    // (or the narrow 메뉴's row) sets it on the bar's state.
    final state = tester.state<SessionToolbarState>(
      find.byType(SessionToolbar),
    );
    state.setPinned(true);
    await tester.pumpAndSettle();
    expect(state.pinned, isTrue);
    expect(find.byKey(const Key('pin-session-toolbar')), findsNothing);
  }
  await tester.tap(find.byKey(const Key('session-quality')));
  await tester.pumpAndSettle();
  expect(find.text('왕복 지연 · Host 응답'), findsOneWidget);
}

void main() {
  testWidgets(
    'secondary popup dismissal never becomes a primary toolbar command',
    (tester) async {
      var commands = 0;
      await tester.pumpWidget(_harness(onCommand: () => commands++));
      await _openQuality(tester);
      final target = tester.getCenter(
        find.byKey(const Key('repeat-lifetime-command')),
      );
      await tester.tapAt(
        target,
        kind: PointerDeviceKind.mouse,
        buttons: kSecondaryButton,
      );
      await tester.pumpAndSettle();
      expect(commands, 0);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );

  for (final boundary in ['content', 'layout']) {
    testWidgets(
      'a deferred menu-switch press cannot cross the $boundary lifetime',
      (tester) async {
        var oldCommands = 0, newCommands = 0;
        await tester.pumpWidget(
          _harness(
            contentKey: 'old-content',
            layoutKey: 'old-layout',
            onCommand: () => oldCommands++,
          ),
        );
        await _openQuality(tester, pinned: true);
        final target = tester.getCenter(
          find.byKey(const Key('repeat-lifetime-command')),
        );
        // The real popup barrier owns this click. Before its post-frame replay,
        // replace the menu's session/content or its presentation lifetime.
        await tester.tapAt(target, kind: PointerDeviceKind.mouse);
        await tester.pumpWidget(
          _harness(
            contentKey: boundary == 'content' ? 'new-content' : 'old-content',
            layoutKey: boundary == 'layout' ? 'new-layout' : 'old-layout',
            onCommand: () => newCommands++,
          ),
        );
        await tester.pumpAndSettle();
        expect(oldCommands, 0);
        expect(newCommands, 0);
        expect(
          find.byKey(const Key('session-command-bar')),
          findsOneWidget,
          reason: 'a pinned bar stays open; safety must not depend on folding',
        );
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pumpAndSettle();
      },
    );
  }

  testWidgets(
    'a current primary popup click preserves pointer kind and runs once',
    (tester) async {
      var commands = 0;
      final presses = <PointerDownEvent>[];
      await tester.pumpWidget(
        _harness(
          onCommand: () => commands++,
          onToolbarPointerDown: presses.add,
        ),
      );
      await _openQuality(tester);
      presses.clear();
      final target = tester.getCenter(
        find.byKey(const Key('repeat-lifetime-command')),
      );
      await tester.tapAt(target, kind: PointerDeviceKind.mouse);
      await tester.pumpAndSettle();
      expect(commands, 1);
      expect(presses, hasLength(1));
      expect(presses.single.kind, PointerDeviceKind.mouse);
      expect(presses.single.buttons, kPrimaryButton);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );

  testWidgets(
    'a cancelled popup press cannot turn later Escape into a command',
    (tester) async {
      var commands = 0;
      await tester.pumpWidget(_harness(onCommand: () => commands++));
      await _openQuality(tester);
      final target = tester.getCenter(
        find.byKey(const Key('repeat-lifetime-command')),
      );
      final press = await tester.createGesture(
        pointer: 3107,
        kind: PointerDeviceKind.mouse,
      );
      await press.addPointer(location: target);
      await press.down(target);
      await press.cancel();
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pumpAndSettle();
      expect(commands, 0);
      await press.removePointer();
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );
}

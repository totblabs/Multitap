import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_status_panel.dart';

void main() {
  testWidgets(
    'Recovery actions remain accessible in narrow windows and large text without overflow',
    (t) async {
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      for (final brightness in Brightness.values) {
        for (final spec in [
          (1440.0, 900.0, 1.0),
          (640.0, 480.0, 1.25),
          (320.0, 480.0, 2.0),
        ]) {
          t.view.physicalSize = Size(spec.$1, spec.$2);
          final boundary = GlobalKey();
          var backs = 0, retries = 0;
          for (final busy in [false, true]) {
            await t.pumpWidget(
              RepaintBoundary(
                key: boundary,
                child: MaterialApp(
                  debugShowCheckedModeBanner: false,
                  theme: buildAppTheme(brightness: brightness),
                  home: MediaQuery(
                    data: MediaQueryData(
                      size: Size(spec.$1, spec.$2),
                      textScaler: TextScaler.linear(spec.$3),
                    ),
                    child: Scaffold(
                      backgroundColor: const Color(0xFF080D16),
                      body: SessionStatusPanel(
                        machine: 'TESTER · Windows',
                        message: '이 데스크톱은 다른 창이나 기기에서 연결 중이에요. 아래 버튼을 누르면 그쪽 연결을 끊고 여기서 이어서 써요. 데스크톱과 실행 중인 작업은 그대로예요.',
                        busy: busy,
                        backLabel: '원격 창 닫기',
                        retryLabel: '끊고 여기서 연결',
                        retryIcon: Icons.swap_horiz_rounded,
                        onBack: () => backs++,
                        onRetry: () => retries++,
                      ),
                    ),
                  ),
                ),
              ),
            );
            await t.pump();
            expect(t.takeException(), isNull);
            final back = find.byKey(const Key('leave-remote-connection'));
            await t.ensureVisible(back);
            await t.pump();
            await t.tap(back);
            await t.pump();
            if (busy) {
              expect(
                find.byKey(const Key('retry-remote-connection')),
                findsNothing,
              );
            } else {
              final retry = find.byKey(const Key('retry-remote-connection'));
              await t.ensureVisible(retry);
              await t.pump();
              await t.tap(retry);
              await t.pump();
            }
            expect(t.takeException(), isNull);
          }
          expect(backs, 2);
          expect(retries, 1);
        }
      }
    },
  );

  Future<void> showActions(
    WidgetTester t, {
    required Size size,
    required double textScale,
    required VoidCallback onBack,
    required VoidCallback onRetry,
    required VoidCallback onAlternative,
    bool busy = false,
  }) async {
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = size;
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        home: MediaQuery(
          data: MediaQueryData(
            size: size,
            textScaler: TextScaler.linear(textScale),
          ),
          child: Scaffold(
            body: SessionStatusPanel(
              machine: 'WorkPC',
              message: '연결 상태를 확인한 뒤 다시 연결하거나 긴급 콘솔을 열 수 있어요.',
              busy: busy,
              backLabel: '원격 창 닫기',
              retryLabel: '현재 연결을 끊고 여기서 다시 연결',
              onBack: onBack,
              onRetry: onRetry,
              onAlternative: onAlternative,
            ),
          ),
        ),
      ),
    );
    await t.pump();
  }

  testWidgets(
    'Three recovery actions use the actual card width on a wide window at 200 percent text',
    (t) async {
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      var backs = 0, retries = 0, alternatives = 0;
      await showActions(
        t,
        size: const Size(1440, 900),
        textScale: 2,
        onBack: () => backs++,
        onRetry: () => retries++,
        onAlternative: () => alternatives++,
      );
      final actions = find.byType(AppDialogActions);
      // The bar measures its own labels in the card's width.
      final actionBounds = t.getRect(actions);
      final retry = find.byKey(const Key('retry-remote-connection'));
      final alternative = find.byKey(
        const Key('emergency-console-alternative'),
      );
      final back = find.byKey(const Key('leave-remote-connection'));
      final retryBounds = t.getRect(retry);
      final alternativeBounds = t.getRect(alternative);
      final backBounds = t.getRect(back);
      expect(retryBounds.bottom, lessThan(alternativeBounds.top));
      expect(alternativeBounds.bottom, lessThan(backBounds.top));
      for (final bounds in [retryBounds, alternativeBounds, backBounds]) {
        expect(bounds.left, closeTo(actionBounds.left, .01));
        expect(bounds.right, closeTo(actionBounds.right, .01));
      }
      expect(
        find.descendant(of: retry, matching: find.byType(FittedBox)),
        findsNothing,
      );
      final label = find.text('현재 연결을 끊고 여기서 다시 연결');
      final paragraph = t.renderObject<RenderParagraph>(
        find.descendant(of: label, matching: find.byType(RichText)),
      );
      expect(paragraph.textScaler.scale(14), 28);
      expect(paragraph.maxLines, isNull);
      Focus.of(t.element(label)).requestFocus();
      await t.pump();
      await t.sendKeyEvent(LogicalKeyboardKey.enter);
      await t.pump();
      expect(
        retries,
        1,
        reason: 'keyboard activation keeps the real retry callback',
      );
      await t.tap(alternative);
      await t.tap(back);
      expect(alternatives, 1);
      expect(backs, 1);
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Three stacked recovery actions stay reachable in a narrow short window and hide while busy',
    (t) async {
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      var backs = 0, retries = 0, alternatives = 0;
      const size = Size(320, 280);
      await showActions(
        t,
        size: size,
        textScale: 2,
        onBack: () => backs++,
        onRetry: () => retries++,
        onAlternative: () => alternatives++,
      );
      for (final key in [
        'retry-remote-connection',
        'emergency-console-alternative',
        'leave-remote-connection',
      ]) {
        final action = find.byKey(Key(key));
        await t.ensureVisible(action);
        await t.pump();
        final bounds = t.getRect(action);
        expect(bounds.left, greaterThanOrEqualTo(0));
        expect(bounds.right, lessThanOrEqualTo(size.width));
        expect(bounds.top, greaterThanOrEqualTo(0));
        expect(bounds.bottom, lessThanOrEqualTo(size.height));
        expect(action.hitTestable(), findsOneWidget);
        await t.tap(action);
        await t.pump();
      }
      expect((retries, alternatives, backs), (1, 1, 1));
      expect(t.takeException(), isNull);
      await showActions(
        t,
        size: size,
        textScale: 2,
        busy: true,
        onBack: () => backs++,
        onRetry: () => retries++,
        onAlternative: () => alternatives++,
      );
      expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
      expect(
        find.byKey(const Key('emergency-console-alternative')),
        findsNothing,
      );
      final back = find.byKey(const Key('leave-remote-connection'));
      await t.ensureVisible(back);
      await t.pump();
      await t.tap(back);
      expect((retries, alternatives, backs), (1, 1, 2));
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Dialog actions without a local width keep intrinsic-height compatibility',
    (t) async {
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      t.view.devicePixelRatio = 1;
      t.view.physicalSize = const Size(1440, 900);
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: MediaQuery(
            data: const MediaQueryData(
              size: Size(1440, 900),
              textScaler: TextScaler.linear(2),
            ),
            child: Scaffold(
              body: Center(
                child: SizedBox(
                  width: 300,
                  child: IntrinsicHeight(
                    child: AppDialogActions(
                      children: [
                        TextButton(
                          key: const Key('intrinsic-cancel'),
                          onPressed: () {},
                          child: const Text('취소'),
                        ),
                        FilledButton(
                          key: const Key('intrinsic-connect'),
                          onPressed: () {},
                          child: const Text('연결'),
                        ),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      );
      await t.pump();
      final cancel = t.getRect(find.byKey(const Key('intrinsic-cancel')));
      final connect = t.getRect(find.byKey(const Key('intrinsic-connect')));
      expect(cancel.right, lessThan(connect.left));
      expect(cancel.center.dy, closeTo(connect.center.dy, .01));
      expect(t.takeException(), isNull);
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Connection status is shown while busy without exposing recovery actions',
    (t) async {
      var settings = 0, retries = 0, alternatives = 0;
      await t.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SessionStatusPanel(
              machine: 'WorkPC',
              message: '컴퓨터의 응답을 기다리고 있어요.',
              busy: true,
              status: const Text(
                '컴퓨터 응답 대기 · 1회 · 12초',
                key: Key('fixture-connection-status'),
              ),
              onBack: () {},
              onSettings: () => settings++,
              onRetry: () => retries++,
              onAlternative: () => alternatives++,
            ),
          ),
        ),
      );
      await t.pump();
      final status = find.byKey(const Key('fixture-connection-status'));
      expect(status, findsOneWidget);
      expect(
        t.getTopLeft(status).dy,
        greaterThan(t.getBottomLeft(find.text('컴퓨터의 응답을 기다리고 있어요.')).dy),
      );
      expect(find.byKey(const Key('recover-remote-settings')), findsNothing);
      expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
      expect(
        find.byKey(const Key('emergency-console-alternative')),
        findsNothing,
      );
      expect((settings, retries, alternatives), (0, 0, 0));
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Settings recovery is opt-in and invokes only its callback once',
    (t) async {
      var settings = 0, retries = 0;
      Future<void> show({VoidCallback? onSettings}) async {
        await t.pumpWidget(
          MaterialApp(
            home: Scaffold(
              body: SessionStatusPanel(
                machine: 'WorkPC',
                message: '화질 설정을 확인한 뒤 다시 연결하세요.',
                onBack: () {},
                onRetry: () => retries++,
                onSettings: onSettings,
              ),
            ),
          ),
        );
        await t.pump();
      }

      await show();
      expect(find.byKey(const Key('recover-remote-settings')), findsNothing);
      expect(find.text('설정 확인'), findsNothing);
      await show(onSettings: () => settings++);
      final action = find.byKey(const Key('recover-remote-settings'));
      expect(
        find.descendant(of: action, matching: find.text('설정 확인')),
        findsOneWidget,
      );
      expect(t.widget<OutlinedButton>(action).onPressed, isNotNull);
      await t.ensureVisible(action);
      await t.pump();
      await t.tap(action);
      await t.pump();
      expect((settings, retries), (1, 0));
      expect(t.takeException(), isNull);
    },
  );

  testWidgets(
    'Five takeover connections sit under the message and the card stays scrollable at 200 percent in a narrow window',
    (t) async {
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      t.view.devicePixelRatio = 1;
      const size = Size(360, 640);
      t.view.physicalSize = size;
      const message =
          '이 데스크톱은 다른 창이나 기기에서 연결 중이에요. 아래 버튼을 누르면 그쪽 연결을 끊고 여기서 이어서 써요. 데스크톱과 실행 중인 작업은 그대로예요.';
      final details = [
        for (var i = 0; i < 5; i++) '연결 중인 계정 account-$i · 11월 11일 10:2$i부터',
      ];
      var retries = 0, backs = 0;
      await t.pumpWidget(
        MaterialApp(
          home: MediaQuery(
            data: const MediaQueryData(
              size: size,
              textScaler: TextScaler.linear(2),
            ),
            child: Scaffold(
              body: SessionStatusPanel(
                machine: 'WorkPC',
                title: '다른 곳에서 연결 중이에요',
                message: message,
                details: details,
                retryLabel: '끊고 여기서 연결',
                retryKey: const Key('takeover-remote-connection'),
                retryIcon: Icons.swap_horiz_rounded,
                onBack: () => backs++,
                onRetry: () => retries++,
              ),
            ),
          ),
        ),
      );
      await t.pump();
      expect(t.takeException(), isNull);
      // In the order read: the explanation, each connection, the buttons.
      var above = t.getBottomLeft(find.text(message)).dy;
      for (final line in details) {
        expect(find.text(line), findsOneWidget);
        expect(t.getTopLeft(find.text(line)).dy, greaterThanOrEqualTo(above));
        above = t.getBottomLeft(find.text(line)).dy;
      }
      final takeover = find.byKey(const Key('takeover-remote-connection'));
      expect(t.getTopLeft(takeover).dy, greaterThan(above));
      for (final action in [
        takeover,
        find.byKey(const Key('leave-remote-connection')),
      ]) {
        await t.ensureVisible(action);
        await t.pump();
        expect(action.hitTestable(), findsOneWidget);
        expect(t.getSize(action).height, greaterThanOrEqualTo(48));
        await t.tap(action);
        await t.pump();
        expect(t.takeException(), isNull);
      }
      expect((retries, backs), (1, 1));
      await t.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'Four recovery actions and status remain reachable at 200 percent in a short window',
    (t) async {
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      t.view.devicePixelRatio = 1;
      const size = Size(320, 280);
      t.view.physicalSize = size;
      var settings = 0, retries = 0, alternatives = 0, backs = 0;
      await t.pumpWidget(
        MaterialApp(
          home: MediaQuery(
            data: const MediaQueryData(
              size: size,
              textScaler: TextScaler.linear(2),
            ),
            child: Scaffold(
              body: SessionStatusPanel(
                machine: 'WorkPC',
                message: '연결 설정을 확인한 뒤 다시 연결할 수 있어요.',
                status: const Text('연결 대기 종료 · 1회 · 100초'),
                settingsLabel: '화면과 연결 설정 확인',
                onBack: () => backs++,
                onSettings: () => settings++,
                onRetry: () => retries++,
                onAlternative: () => alternatives++,
              ),
            ),
          ),
        ),
      );
      await t.pump();
      expect(t.takeException(), isNull);
      for (final key in [
        'recover-remote-settings',
        'retry-remote-connection',
        'emergency-console-alternative',
        'leave-remote-connection',
      ]) {
        final action = find.byKey(Key(key));
        await t.ensureVisible(action);
        await t.pump();
        final bounds = t.getRect(action);
        expect(bounds.left, greaterThanOrEqualTo(0));
        expect(bounds.right, lessThanOrEqualTo(size.width));
        expect(bounds.top, greaterThanOrEqualTo(0));
        expect(bounds.bottom, lessThanOrEqualTo(size.height));
        expect(bounds.height, greaterThanOrEqualTo(48));
        expect(action.hitTestable(), findsOneWidget);
        await t.tap(action);
        await t.pump();
        expect(t.takeException(), isNull);
      }
      expect((settings, retries, alternatives, backs), (1, 1, 1, 1));
      expect(find.text('화면과 연결 설정 확인'), findsOneWidget);
      expect(
        find.descendant(
          of: find.byKey(const Key('recover-remote-settings')),
          matching: find.byType(FittedBox),
        ),
        findsNothing,
      );
      await t.pumpWidget(const SizedBox.shrink());
    },
  );
}

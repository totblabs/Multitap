// 사용안내 follows what 터치 really is on this connection. With a Host that
// granted touch-input-v1 every finger is a real Windows touch contact
// (android_touch.dart: _Phase.native), so the phone's own mouse gestures (a
// pinch that zooms the picture, a two-finger right click, a double-tap drag, a
// tap held back for a double tap) do not exist there. The panel described them
// anyway (2026-11-10 wording audit D#38).
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_touch.dart';

Future<void> _show(
  WidgetTester tester,
  AndroidTouchMode mode, {
  required bool realTouch,
}) async {
  tester.view.physicalSize = const Size(420, 2400);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(
      home: Scaffold(
        body: AndroidTouchHelp(mode: mode, realTouch: realTouch, onClose: () {}),
      ),
    ),
  );
  await tester.pumpAndSettle();
}

Finder _said(String text) => find.descendant(
  of: find.byKey(const Key('android-touch-help')),
  matching: find.text(text),
);

const _mouseOnly = [
  '두 번 탭하고 누른 채 움직이기',
  '길게 누르기 · 두 손가락 탭',
  '두 손가락 벌리기',
  '한 손가락으로 위아래',
];

void main() {
  testWidgets('터치 with real Windows touch describes real touch', (
    tester,
  ) async {
    await _show(tester, AndroidTouchMode.direct, realTouch: true);
    expect(_said('탭 · 밀기'), findsOneWidget);
    expect(_said('Windows 터치로 그대로 전달 · 터치 화면처럼 조작'), findsOneWidget);
    expect(_said('길게 누르기'), findsOneWidget);
    expect(_said('오른쪽 클릭'), findsOneWidget);
    expect(_said('두 손가락'), findsOneWidget);
    // The gestures the phone keeps for itself work in every mode.
    expect(_said('세 손가락 탭'), findsOneWidget);
    expect(_said('네 손가락 탭'), findsOneWidget);
    // Nothing of the mouse emulation, and no delayed tap.
    for (final gesture in _mouseOnly) {
      expect(_said(gesture), findsNothing, reason: gesture);
    }
    expect(find.textContaining('조금 늦어요'), findsNothing);
    expect(find.textContaining('확대 · 오므리면 전체 보기까지'), findsNothing);
    expect(
      find.textContaining('화면 자체를 확대하려면 입력 방식을 포인터로 바꾸세요.'),
      findsOneWidget,
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('터치 without it is still the mouse at the finger', (
    tester,
  ) async {
    await _show(tester, AndroidTouchMode.direct, realTouch: false);
    expect(_said('탭'), findsOneWidget);
    expect(_said('누른 곳을 클릭 · 두 번 탭은 더블 클릭'), findsOneWidget);
    for (final gesture in [..._mouseOnly, '세 손가락 탭', '네 손가락 탭']) {
      expect(_said(gesture), findsOneWidget, reason: gesture);
    }
    expect(find.textContaining('탭 클릭은 두 번 탭인지 확인하느라 조금 늦어요.'), findsOneWidget);
    expect(find.textContaining('Windows 터치로'), findsNothing);
  });

  testWidgets('포인터 is the same with or without real touch', (tester) async {
    for (final realTouch in [false, true]) {
      await _show(tester, AndroidTouchMode.trackpad, realTouch: realTouch);
      for (final gesture in const [
        '한 손가락으로 밀기',
        '탭',
        '두 번 탭하고 두 번째를 누른 채 움직이기',
        '두 손가락으로 위아래',
        '길게 누르기 · 두 손가락 탭',
        '두 손가락 벌리기',
        '세 손가락 탭',
        '네 손가락 탭',
      ]) {
        expect(_said(gesture), findsOneWidget, reason: '$gesture $realTouch');
      }
      expect(find.textContaining('Windows 터치로'), findsNothing);
      expect(find.textContaining('조금 늦어요'), findsOneWidget);
    }
  });
}

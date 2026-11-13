// The shared marks for work that is under way (lib/ui/app_progress.dart):
// a bar that is filled only by a value it is given, a step count only for
// steps it is told about, and a clock that shows the time it is handed.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/app_progress.dart';
import 'package:multitap_client/ui/app_theme.dart';

void main() {
  Future<void> show(WidgetTester t, Widget child) => t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: Brightness.dark),
      home: Scaffold(body: SizedBox(width: 360, child: child)),
    ),
  );
  final bar = find.byKey(const Key('app-progress-bar'));
  final count = find.byKey(const Key('app-progress-count'));
  final detail = find.byKey(const Key('app-progress-detail'));
  final clock = find.byKey(const Key('app-elapsed'));

  testWidgets('Without a value the bar only runs; nothing is counted or measured', (
    t,
  ) async {
    await show(t, const AppProgressBlock(step: '설치하는 중이에요'));
    expect(t.widget<LinearProgressIndicator>(bar).value, isNull);
    expect(find.text('설치하는 중이에요'), findsOneWidget);
    expect(count, findsNothing);
    expect(detail, findsNothing);
    expect(t.widget<Text>(clock).data, '경과 00:00');
    // It keeps moving: a frame is always scheduled while it is shown.
    await t.pump(const Duration(milliseconds: 300));
    expect(t.binding.hasScheduledFrame, isTrue);
  });

  testWidgets('A value fills the bar to exactly that value', (t) async {
    await show(
      t,
      const AppProgressBlock(
        step: '받는 중이에요',
        value: .4,
        detail: '12.0 / 30.0 MB',
        stepIndex: 2,
        stepCount: 3,
        showElapsed: false,
      ),
    );
    expect(t.widget<LinearProgressIndicator>(bar).value, .4);
    expect(t.widget<Text>(detail).data, '12.0 / 30.0 MB');
    expect(t.widget<Text>(count).data, '2/3단계');
    expect(clock, findsNothing);
    // A single step is not "1/1".
    await show(
      t,
      const AppProgressBlock(step: '확인하는 중이에요', stepIndex: 1, stepCount: 1),
    );
    expect(count, findsNothing);
  });

  testWidgets('The clock shows the time it is handed, and only after its delay', (
    t,
  ) async {
    var elapsed = const Duration(seconds: 2);
    await show(
      t,
      AppElapsedText(
        elapsed: () => elapsed,
        after: const Duration(seconds: 3),
      ),
    );
    expect(clock, findsNothing); // a short wait never flashes a clock
    elapsed = const Duration(minutes: 1, seconds: 15);
    await t.pump(const Duration(seconds: 1)); // its own tick redraws it
    expect(t.widget<Text>(clock).data, '경과 01:15');
    elapsed = const Duration(minutes: 12, seconds: 5);
    await t.pump(const Duration(seconds: 1));
    expect(t.widget<Text>(clock).data, '경과 12:05');
    // Gone with its widget: no timer is left behind.
    await show(t, const SizedBox());
    await t.pump(const Duration(seconds: 5));
    expect(t.takeException(), isNull);
  });

  testWidgets('A busy line is a spinner and its sentence', (t) async {
    await show(
      t,
      const AppBusyLine('장치를 찾고 있어요', textKey: Key('line-text')),
    );
    expect(find.byKey(const Key('app-busy-line-spinner')), findsOneWidget);
    expect(
      find.descendant(
        of: find.byKey(const Key('app-busy-line-spinner')),
        matching: find.byType(CircularProgressIndicator),
      ),
      findsOneWidget,
    );
    expect(t.widget<Text>(find.byKey(const Key('line-text'))).data, '장치를 찾고 있어요');
    expect(t.binding.hasScheduledFrame, isTrue);
  });
}

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/app_menu.dart';
import 'package:multitap_client/ui/session_menu.dart';

void main() {
  for (final reduced in [false, true]) {
    testWidgets('session menu respects reduced motion=$reduced', (
      tester,
    ) async {
      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context).copyWith(disableAnimations: reduced),
            child: child!,
          ),
          home: Scaffold(
            body: Builder(
              builder: (context) => TextButton(
                onPressed: () => showSessionMenu(
                  context,
                  const Rect.fromLTWH(20, 20, 40, 40),
                  [SessionMenuItem('검사 명령', onSelected: () {})],
                ),
                child: const Text('열기'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('열기'));
      await tester.pump();
      await tester.pump();
      final route = ModalRoute.of(tester.element(find.text('검사 명령')))!;
      expect(route.animation!.value, reduced ? 1 : 0);
      if (!reduced) {
        await tester.pump(PopupMotion.enter);
        expect(route.animation!.value, 1);
      }
      expect(find.text('검사 명령').hitTestable(), findsOneWidget);
      expect(
        route.reverseTransitionDuration,
        lessThanOrEqualTo(PopupMotion.enter),
      );
      await tester.tap(find.text('검사 명령'));
      await tester.pump();
      // This Flutter SDK's PopupMenuRoute uses its transitionDuration for
      // reverse too; AnimationStyle.reverseDuration is not consumed there.
      await tester.pump(route.reverseTransitionDuration);
      await tester.pump();
      expect(route.animation!.value, 0);
      expect(route.isActive, isFalse);
      // At the exact end timestamp the controller can still report reverse
      // with zero opacity; the next display tick retires that invisible entry.
      await tester.pump(const Duration(milliseconds: 16));
      expect(find.text('검사 명령'), findsNothing);
      expect(tester.takeException(), isNull);
    });
  }

  testWidgets(
    'reduced-motion anchored panel opens and closes without an animation wait',
    (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          builder: (context, child) => MediaQuery(
            data: MediaQuery.of(context).copyWith(disableAnimations: true),
            child: child!,
          ),
          home: Scaffold(
            body: Builder(
              builder: (context) => TextButton(
                onPressed: () => Navigator.of(context).push(
                  AnchoredPanelRoute<void>(
                    anchor: const Rect.fromLTWH(40, 40, 40, 40),
                    width: 240,
                    label: '검사 패널',
                    reduceMotion: MediaQuery.disableAnimationsOf(context),
                    builder: (context) => TextButton(
                      onPressed: () => Navigator.of(context).pop(),
                      child: const Text('패널 닫기'),
                    ),
                  ),
                ),
                child: const Text('열기'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('열기'));
      await tester.pump();
      await tester.pump();
      final route = ModalRoute.of(tester.element(find.text('패널 닫기')))!;
      expect(route.animation!.value, 1);
      expect(find.text('패널 닫기').hitTestable(), findsOneWidget);
      await tester.tap(find.text('패널 닫기'));
      await tester.pump();
      await tester.pump();
      expect(find.text('패널 닫기'), findsNothing);
      expect(tester.takeException(), isNull);
    },
  );
}

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/screen_transition_progress.dart';

void main() {
  testWidgets(
    'Optional transition status preserves the default and stays reachable with large text',
    (t) async {
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
      t.view.devicePixelRatio = 1;
      const size = Size(320, 280);
      t.view.physicalSize = size;
      var closes = 0;
      for (final showStatus in [false, true]) {
        await t.pumpWidget(
          MaterialApp(
            home: MediaQuery(
              data: const MediaQueryData(
                size: size,
                textScaler: TextScaler.linear(2),
              ),
              child: Scaffold(
                body: ScreenTransitionProgress(
                  target: '듀얼 화면',
                  stage: ScreenTransitionStage.firstPicture,
                  onClose: () => closes++,
                  status: showStatus
                      ? const Text(
                          '첫 화면 대기 · 1회 · 12초',
                          key: Key('fixture-transition-status'),
                        )
                      : null,
                ),
              ),
            ),
          ),
        );
        await t.pump();
        expect(t.takeException(), isNull);
        expect(find.text('첫 화면 기다리는 중'), findsOneWidget);
        expect(find.text('새 화면이 준비되면 입력할 수 있어요.'), findsOneWidget);
        final status = find.byKey(const Key('fixture-transition-status'));
        expect(status, showStatus ? findsOneWidget : findsNothing);
        if (showStatus) {
          expect(
            t.getTopLeft(status).dy,
            greaterThan(
              t
                  .getBottomLeft(
                    find.byKey(const Key('screen-transition-stage')),
                  )
                  .dy,
            ),
          );
          await t.ensureVisible(status);
          await t.pump();
          expect(status.hitTestable(), findsOneWidget);
        }
        final cancel = find.byKey(const Key('screen-transition-cancel'));
        await t.ensureVisible(cancel);
        await t.pump();
        final bounds = t.getRect(cancel);
        expect(bounds.left, greaterThanOrEqualTo(0));
        expect(bounds.right, lessThanOrEqualTo(size.width));
        expect(bounds.top, greaterThanOrEqualTo(0));
        expect(bounds.bottom, lessThanOrEqualTo(size.height));
        expect(cancel.hitTestable(), findsOneWidget);
        await t.tap(cancel);
        await t.pump();
        expect(closes, showStatus ? 2 : 1);
        expect(t.takeException(), isNull);
      }
      await t.pumpWidget(const SizedBox.shrink());
    },
  );
}

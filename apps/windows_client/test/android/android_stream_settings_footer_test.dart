// Production controls with fake callbacks only: no session or native device.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_toolbar.dart';
import 'package:multitap_client/android/android_tv.dart';
import 'package:multitap_client/models/stream_options.dart';

void main() {
  for (final screen in const [
    Size(800, 360),
    Size(915, 412),
    Size(412, 915),
    Size(960, 540),
  ]) {
    for (final textScale in const [1.0, 1.3, 2.0]) {
      final television = screen == const Size(960, 540);
      testWidgets(
        'settings footer stays visible and all rates work at $screen/$textScale TV=$television',
        (tester) async {
          tester.view.physicalSize = screen;
          tester.view.devicePixelRatio = 1;
          tester.view.padding = const FakeViewPadding(
            top: 24,
            bottom: 16,
            left: 24,
          );
          addTearDown(tester.view.reset);
          final applied = <StreamOptions>[];
          var cancels = 0;
          await tester.pumpWidget(
            MaterialApp(
              builder: (context, child) => MediaQuery(
                data: MediaQuery.of(context)
                    .copyWith(textScaler: TextScaler.linear(textScale)),
                child: AndroidTvNavigation(
                  television: television,
                  child: child!,
                ),
              ),
              home: Scaffold(
                body: SafeArea(
                  child: Align(
                    alignment: Alignment.centerRight,
                    child: ConstrainedBox(
                      constraints: const BoxConstraints(maxWidth: 600),
                      child: AndroidStreamSettingsPanel(
                        options: const StreamOptions(bitrate: 8000000),
                        maxBitrate: 100000000,
                        onApply: applied.add,
                        onCancel: () => cancels++,
                      ),
                    ),
                  ),
                ),
              ),
            ),
          );
          await tester.pumpAndSettle();
          final apply = find.byKey(const Key('apply-stream-settings'));
          final cancel = find.byKey(const Key('cancel-stream-settings'));
          final safe = Rect.fromLTRB(24, 24, screen.width, screen.height - 16);
          void inside(Finder target) {
            final rect = tester.getRect(target);
            expect(rect.top, greaterThanOrEqualTo(safe.top));
            expect(rect.bottom, lessThanOrEqualTo(safe.bottom));
            expect(rect.left, greaterThanOrEqualTo(safe.left));
            expect(rect.right, lessThanOrEqualTo(safe.right));
          }

          // Reconnect is disabled until a choice changes, but starts in view.
          inside(apply);
          inside(cancel);
          expect(cancel.hitTestable(), findsOneWidget);
          for (final rate in AndroidStreamQuality.bitrates) {
            final choice = find.byKey(Key('stream-bitrate-$rate'));
            final beforeScroll = tester.getRect(apply);
            await tester.ensureVisible(choice);
            await tester.pumpAndSettle();
            expect(tester.getRect(apply), beforeScroll);
            expect(choice.hitTestable(), findsOneWidget);
            inside(choice);
            await tester.tap(choice);
            await tester.pumpAndSettle();
            inside(apply);
          }
          expect(applied, isEmpty);
          expect(apply.hitTestable(), findsOneWidget);
          await tester.tap(apply);
          await tester.pumpAndSettle();
          expect(applied, hasLength(1));
          expect(applied.single.bitrate, 100000000);
          expect(applied.single.automaticBitrate, false);
          await tester.tap(cancel);
          await tester.pumpAndSettle();
          expect(cancels, 1);
          expect(applied, hasLength(1));
          expect(tester.takeException(), isNull);
        },
      );
    }
  }

  testWidgets('footer panel accepts a naturally sized unbounded parent', (
    tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: SingleChildScrollView(
            child: AndroidToolbarPanel(
              title: 'Fixture',
              footer: TextButton(onPressed: () {}, child: const Text('footer')),
              children: const [SizedBox(height: 900)],
            ),
          ),
        ),
      ),
    );
    await tester.ensureVisible(find.text('footer'));
    expect(find.text('footer').hitTestable(), findsOneWidget);
    expect(tester.takeException(), isNull);
  });
}

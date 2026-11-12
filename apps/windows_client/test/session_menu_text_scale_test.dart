import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_menu.dart';
import 'package:multitap_client/ui/vm_resource_bars.dart';

import 'bulk_host_update_dialog_test.dart' show loadBulkFonts;

void main() {
  for (final width in [320.0, 360.0, 720.0, 1920.0]) {
    testWidgets(
      'quality labels and values remain readable at 200%, width $width',
      (tester) async {
        await loadBulkFonts(tester);
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize = Size(width, 1000);
        addTearDown(tester.view.reset);
        var selected = false;
        await tester.pumpWidget(
          MaterialApp(
            theme: buildAppTheme(),
            builder: (context, child) => MediaQuery(
              data: MediaQuery.of(context)
                  .copyWith(textScaler: const TextScaler.linear(2)),
              child: child!,
            ),
            home: Scaffold(
              body: Align(
                alignment: Alignment.topCenter,
                child: SessionToolbarMenu(
                  compact: true,
                  label: '연결 품질',
                  icon: Icons.network_check_rounded,
                  onInteract: () {},
                  items: [
                    const SessionMenuItem.info(
                      '해상도 · 원격 화면',
                      '2560 × 1440 + 1920 × 1080',
                    ),
                    const SessionMenuItem.info('화면 · PC 디코딩', '60.0 fps'),
                    const SessionMenuItem.info('인코딩 · Host 처리', '12.34 ms'),
                    SessionMenuItem(
                      '연결 통계 보기',
                      icon: Icons.insights_rounded,
                      onSelected: () => selected = true,
                    ),
                  ],
                ),
              ),
            ),
          ),
        );
        await tester.tap(find.byType(SessionToolbarMenu));
        await tester.pumpAndSettle();
        final paragraphs = find.descendant(
          of: find.byType(PopupMenuItem<int>),
          matching: find.byWidgetPredicate((widget) => widget is Text),
        );
        expect(paragraphs, findsNWidgets(7));
        for (final element in paragraphs.evaluate()) {
          final paragraph = tester.renderObject<RenderParagraph>(
            find.descendant(
              of: find.byWidget(element.widget),
              matching: find.byType(RichText),
            ),
          );
          expect(
            paragraph.didExceedMaxLines,
            isFalse,
            reason: paragraph.text.toPlainText(),
          );
          final rect = paragraph.localToGlobal(Offset.zero) & paragraph.size;
          expect(rect.left, greaterThanOrEqualTo(0));
          expect(rect.right, lessThanOrEqualTo(width));
        }
        expect(tester.takeException(), isNull);
        await tester.tap(find.text('연결 통계 보기'));
        await tester.pumpAndSettle();
        expect(selected, isTrue);
      },
    );
  }

  for (final touch in [false, true]) {
    testWidgets('resource labels fit a narrow card at 200%, touch $touch', (
      tester,
    ) async {
      await loadBulkFonts(tester);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: MediaQuery(
            data: const MediaQueryData(textScaler: TextScaler.linear(2)),
            child: Scaffold(
              body: Center(
                child: SizedBox(
                  width: 260,
                  child: VmResourceStrip(
                    touch: touch,
                    running: true,
                    sample: VmResources(
                      vcpus: 4,
                      memoryTotalBytes: 8 * 1024 * 1024 * 1024,
                      cpuUsagePercent: 23,
                      memoryUsedBytes: 5 * 1024 * 1024 * 1024,
                      checkedAt: DateTime.now().toUtc(),
                      stale: false,
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();
      for (final label in ['CPU 4코어', '메모리 8 GB']) {
        final rich = find.descendant(
          of: find.text(label),
          matching: find.byType(RichText),
        );
        final paragraph = tester.renderObject<RenderParagraph>(rich);
        expect(paragraph.didExceedMaxLines, isFalse, reason: label);
      }
      expect(tester.takeException(), isNull);
    });
  }
}

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/app_theme.dart';

void main() {
  for (final platform in [
    TargetPlatform.android,
    TargetPlatform.iOS,
    TargetPlatform.windows,
  ]) {
    for (final scale in [1.0, 2.0]) {
      testWidgets(
        'Dialog close hit target and keyboard action on ${platform.name} at $scale text',
        (t) async {
          addTearDown(t.view.resetDevicePixelRatio);
          addTearDown(t.view.resetPhysicalSize);
          t.view.devicePixelRatio = 1;
          const size = Size(360, 640);
          t.view.physicalSize = size;
          var closes = 0;
          Widget harness(Widget body) => MaterialApp(
            theme: buildAppTheme(brightness: Brightness.dark)
                .copyWith(platform: platform),
            home: MediaQuery(
              data: MediaQueryData(
                size: size,
                textScaler: TextScaler.linear(scale),
              ),
              child: Scaffold(body: body),
            ),
          );
          // Exact close-button style from the pre-change AppDialog snapshot
          // 8013e1526d99e79808c5de67bb0f0047ee7fa5d1ff1fb28843f7db180cbfc9a4.
          // Its authored34dp style and framework48dp hit region are distinct.
          await t.pumpWidget(
            harness(
              Builder(
                builder: (context) {
                  final p = AppPalette.of(context);
                  return Center(
                    child: IconButton(
                      key: const Key('legacy-dialog-close'),
                      tooltip: '닫기',
                      onPressed: () {},
                      style: IconButton.styleFrom(
                        foregroundColor: p.muted,
                        minimumSize: const Size.square(34),
                        maximumSize: const Size.square(34),
                        padding: EdgeInsets.zero,
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(10),
                        ),
                      ),
                      icon: const Icon(Icons.close_rounded, size: 20),
                    ),
                  );
                },
              ),
            ),
          );
          await t.pump();
          final legacySize = t.getSize(
            find.byKey(const Key('legacy-dialog-close')),
          );
          expect(legacySize, const Size.square(48));
          expect(t.takeException(), isNull);
          await t.pumpWidget(
            harness(
              AppDialog(
                title: '연결 설정 확인',
                closeKey: const Key('fixture-dialog-close'),
                onClose: () => closes++,
                body: const [Text('저장한 연결 설정을 확인하세요.')],
              ),
            ),
          );
          await t.pump();
          expect(t.takeException(), isNull);
          final close = find.byKey(const Key('fixture-dialog-close'));
          final bounds = t.getRect(close);
          expect(bounds.size, const Size.square(48));
          if (platform == TargetPlatform.windows) {
            expect(bounds.size, legacySize);
            final style = t.widget<IconButton>(close).style!;
            expect(style.minimumSize!.resolve({}), const Size.square(34));
            expect(style.maximumSize!.resolve({}), const Size.square(34));
            expect(style.visualDensity, isNull);
          }
          expect(close.hitTestable(), findsOneWidget);
          final icon = find.descendant(of: close, matching: find.byType(Icon));
          expect(t.widget<Icon>(icon).size, 20);
          // Exercise the hit area outside the20dp icon, not just its centre.
          await t.tapAt(bounds.topLeft + const Offset(3, 3));
          await t.pump();
          expect(closes, 1);
          Focus.of(t.element(icon)).requestFocus();
          await t.pump();
          await t.sendKeyEvent(LogicalKeyboardKey.enter);
          await t.pump();
          expect(closes, 2);
          expect(t.takeException(), isNull);
          await t.pumpWidget(const SizedBox.shrink());
        },
      );
    }
  }
}

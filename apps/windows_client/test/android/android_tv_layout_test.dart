import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_store.dart';

import 'android_launcher_test.dart' show MemoryStore;

void main() {
  setUpAll(() async {
    final font = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      font.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await font.load();
    await (FontLoader(
      'MaterialIcons',
    )..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'))).load();
  });
  tearDown(
    () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(AndroidStore.channel, null),
  );

  for (final size in [
    const Size(1920, 1080),
    const Size(1366, 768),
    const Size(960, 540),
  ]) {
    for (final theme in ['dark', 'light']) {
      for (final scale in [1.0, 1.5]) {
        testWidgets('TV focus at $size $theme text $scale', (tester) async {
          tester.view.physicalSize = size;
          tester.view.devicePixelRatio = 1;
          tester.platformDispatcher.textScaleFactorTestValue = scale;
          addTearDown(tester.view.reset);
          addTearDown(tester.platformDispatcher.clearAllTestValues);
          MemoryStore(prefs: {'appearance.theme': theme}).install();
          final boundary = GlobalKey();
          await tester.pumpWidget(
            RepaintBoundary(
              key: boundary,
              child: const AndroidVdiApp(television: true),
            ),
          );
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('android-tv-focus-ring')), findsNothing);
          final focused = FocusManager.instance.primaryFocus;
          expect(focused?.hasFocus, true);
          final control = focused!.context!.findRenderObject()! as RenderBox;
          final rect = control.localToGlobal(Offset.zero) & control.size;
          expect(rect.width, greaterThan(20));
          expect(rect.left, greaterThanOrEqualTo(0));
          expect(rect.right, lessThanOrEqualTo(size.width));
          expect(rect.bottom, lessThanOrEqualTo(size.height));
          expect(tester.takeException(), isNull);
          await tester.pumpWidget(const SizedBox.shrink());
          await tester.pumpAndSettle();
        });
      }
    }
  }
}

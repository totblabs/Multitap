// Inspection 2026-11-12: at 200 % text the buttons inside the session panels
// broke their words in the middle ("다시 연 / 결", "Pointe / r", "Preci / se",
// "Norm / al"). They follow the device's text size up to 130 %
// (androidButtonTextScaleCap) and Korean wraps between words only.
// Production widgets with the app's own font; no session or native device.
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_session_ui.dart';
import 'package:multitap_client/android/android_toolbar.dart';
import 'package:multitap_client/models/stream_options.dart';

/// The label's paragraph inside the button [key], and that no word of it is
/// split over two lines.
void _expectWholeWords(WidgetTester tester, Key key, String label) {
  final paragraph = tester.renderObject<RenderParagraph>(
    find.descendant(
      of: find.descendant(of: find.byKey(key), matching: find.text(label)),
      matching: find.byType(RichText),
    ),
  );
  // 14 dp at the 130 % cap, not the device's 200 %.
  expect(paragraph.textScaler.scale(14), closeTo(18.2, .01), reason: label);
  expect(paragraph.didExceedMaxLines, isFalse, reason: label);
  final drawn = paragraph.text.toPlainText();
  var start = 0;
  for (final word in drawn.split(' ')) {
    if (word.isNotEmpty) {
      final boxes = paragraph.getBoxesForSelection(
        TextSelection(baseOffset: start, extentOffset: start + word.length),
      );
      expect(
        boxes.map((box) => box.top.round()).toSet(),
        hasLength(1),
        reason: '"$word" of "$label" is on one line',
      );
    }
    start += word.length + 1;
  }
  // Nothing is drawn outside its button.
  final text = tester.getRect(
    find.descendant(of: find.byKey(key), matching: find.text(label)),
  );
  final button = tester.getRect(find.byKey(key));
  expect(text.left, greaterThanOrEqualTo(button.left - .01), reason: label);
  expect(text.right, lessThanOrEqualTo(button.right + .01), reason: label);
}

Future<void> _pump(WidgetTester tester, Widget panel, double width) async {
  tester.view.physicalSize = const Size(360, 800);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(
          context,
        ).copyWith(textScaler: const TextScaler.linear(2)),
        child: child!,
      ),
      home: Scaffold(
        body: Align(
          alignment: Alignment.topLeft,
          child: SizedBox(width: width, child: panel),
        ),
      ),
    ),
  );
  await tester.pumpAndSettle();
}

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

  // The menu card is 288 dp wide on a 360 dp phone.
  testWidgets('화질 at 200 % text: 취소 and 다시 연결 keep their words whole', (
    tester,
  ) async {
    await _pump(
      tester,
      AndroidStreamSettingsPanel(
        options: const StreamOptions(bitrate: 8000000),
        maxBitrate: 100000000,
        onApply: (_) {},
        onCancel: () {},
      ),
      288,
    );
    _expectWholeWords(tester, const Key('cancel-stream-settings'), '취소');
    _expectWholeWords(tester, const Key('apply-stream-settings'), '다시 연결');
    // 다시 연결 keeps its icon beside the label.
    expect(
      find.descendant(
        of: find.byKey(const Key('apply-stream-settings')),
        matching: find.byIcon(Icons.refresh_rounded),
      ),
      findsOneWidget,
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('입력 방식 in English at 200 % text: Touch, Pointer, Precise, '
      'Normal and Fast are never broken inside the word', (tester) async {
    await _pump(
      tester,
      AndroidToolbarPanel(
        title: 'Input mode',
        children: [
          AndroidPanelRow(
            items: [
              AndroidPanelItem(
                key: const Key('mode-touch'),
                label: 'Touch',
                icon: Icons.touch_app_rounded,
                onPressed: () {},
              ),
              AndroidPanelItem(
                key: const Key('mode-pointer'),
                label: 'Pointer',
                icon: Icons.mouse_rounded,
                selected: true,
                onPressed: () {},
              ),
            ],
          ),
          AndroidPanelRow(
            label: 'Pointer speed',
            items: [
              for (final label in const ['Precise', 'Normal', 'Fast'])
                AndroidPanelItem(
                  key: Key('speed-$label'),
                  label: label,
                  selected: label == 'Normal',
                  onPressed: () {},
                ),
            ],
          ),
        ],
      ),
      288,
    );
    _expectWholeWords(tester, const Key('mode-touch'), 'Touch');
    _expectWholeWords(tester, const Key('mode-pointer'), 'Pointer');
    for (final label in const ['Precise', 'Normal', 'Fast']) {
      _expectWholeWords(tester, Key('speed-$label'), label);
    }
    expect(tester.takeException(), isNull);
  });
}

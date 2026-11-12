// The shared dialog button bar (AppDialogActions) and the dialogs built on
// it never wrap or cut a button label (such as "추가하고 다시 / 시작" on
// two lines in a wide window). Every case
// runs at 360/800/1280/1920 px, text 100/150/200 %, dark and light, with the
// bundled Pretendard so label widths are the real ones. Widgets only.
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/host_update_dialog.dart';

import 'bulk_host_updates_test.dart' show bulkMachine, bulkRelease, bulkState;

const _sizes = [Size(360, 740), Size(800, 600), Size(1280, 800), Size(1920, 1080)];
const _scales = [1.0, 1.5, 2.0];

bool _fonts = false;
Future<void> _loadFonts(WidgetTester t) async {
  if (_fonts) return;
  await t.runAsync(() async {
    final text = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      text.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await text.load();
    final artifacts = File(Platform.resolvedExecutable).parent.parent.parent;
    await (FontLoader('MaterialIcons')..addFont(
          File(
            '${artifacts.path}/material_fonts/MaterialIcons-Regular.otf',
          ).readAsBytes().then(ByteData.sublistView),
        ))
        .load();
  });
  _fonts = true;
}

/// Every button label inside a dialog that does not fit on one line at its
/// laid-out width: wrapped or cut. Empty means every label is whole.
List<String> wrappedButtonLabels(WidgetTester t) {
  final faults = <String>[];
  final dialog = find.byType(Dialog);
  final buttons = find.descendant(
    of: dialog,
    matching: find.byWidgetPredicate((w) => w is ButtonStyleButton),
  );
  for (final button in buttons.evaluate()) {
    void visit(Element element) {
      final paragraph = element.renderObject;
      if (element.widget is RichText && paragraph is RenderParagraph) {
        final natural = paragraph.getMaxIntrinsicWidth(double.infinity);
        if (natural > paragraph.size.width + .5 ||
            paragraph.didExceedMaxLines) {
          faults.add(
            '"${paragraph.text.toPlainText().replaceAll('⁠', '')}" '
            'needs ${natural.toStringAsFixed(0)} px, has '
            '${paragraph.size.width.toStringAsFixed(0)}',
          );
        }
      }
      element.visitChildren(visit);
    }

    button.visitChildren(visit);
  }
  return faults;
}

/// Opens [dialog] at every size, text scale and appearance and checks that
/// it lays out without an error and with every button label on one line.
Future<void> expectWholeLabels(
  WidgetTester t,
  Widget Function() dialog, {
  Future<void> Function(WidgetTester t)? ready,
}) async {
  await _loadFonts(t);
  t.view.devicePixelRatio = 1;
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
  for (final brightness in Brightness.values) {
    for (final size in _sizes) {
      for (final scale in _scales) {
        final at = '${brightness.name} ${size.width.toInt()} px, text ×$scale';
        t.view.physicalSize = size;
        t.platformDispatcher.textScaleFactorTestValue = scale;
        await t.pumpWidget(
          MaterialApp(
            theme: buildAppTheme(brightness: brightness),
            home: Builder(
              builder: (context) => Scaffold(
                body: Center(
                  child: TextButton(
                    onPressed: () => unawaited(
                      showDialog<void>(context: context, builder: (_) => dialog()),
                    ),
                    child: const Text('open'),
                  ),
                ),
              ),
            ),
          ),
        );
        await t.tap(find.text('open'));
        await t.pumpAndSettle();
        if (ready != null) await ready(t);
        expect(t.takeException(), isNull, reason: at);
        expect(find.byType(Dialog), findsOneWidget, reason: at);
        expect(wrappedButtonLabels(t), isEmpty, reason: at);
        await t.pumpWidget(const SizedBox.shrink());
      }
    }
  }
}

void main() {
  group('AppDialogActions', () {
    Widget bar(double width, List<String> labels, {double scale = 1}) =>
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: MediaQuery(
            data: MediaQueryData(textScaler: TextScaler.linear(scale)),
            child: Scaffold(
              body: Align(
                alignment: Alignment.topLeft,
                child: SizedBox(
                  width: width,
                  // IntrinsicHeight: the sign-in card measures the bar.
                  child: IntrinsicHeight(
                    child: AppDialogActions(
                      children: [
                        for (final (index, label) in labels.indexed)
                          FilledButton(
                            key: Key('bar-$index'),
                            onPressed: () {},
                            child: Text(label),
                          ),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ),
        );

    testWidgets(
      'labels that fit share one row in equal widths, main action on the right',
      (t) async {
        await _loadFonts(t);
        await t.pumpWidget(bar(400, ['취소', '추가하고 다시 시작']));
        final cancel = t.getRect(find.byKey(const Key('bar-0')));
        final main = t.getRect(find.byKey(const Key('bar-1')));
        expect(cancel.top, main.top);
        expect(cancel.width, closeTo(main.width, .5));
        expect(main.left, greaterThan(cancel.right));
        expect(cancel.left, 0);
        expect(main.right, closeTo(400, .5));
        expect(wrappedLabelsAnywhere(t), isEmpty);
      },
    );

    testWidgets(
      'a label that would wrap stacks the buttons full width, main action on top, at its intrinsic height',
      (t) async {
        await _loadFonts(t);
        // Three buttons in a 356 px content width.
        await t.pumpWidget(bar(356, ['닫기', '추가만 하기', '추가하고 다시 시작']));
        final close = t.getRect(find.byKey(const Key('bar-0')));
        final add = t.getRect(find.byKey(const Key('bar-1')));
        final main = t.getRect(find.byKey(const Key('bar-2')));
        expect(main.top, lessThan(add.top));
        expect(add.top, lessThan(close.top));
        for (final rect in [close, add, main]) {
          expect(rect.width, closeTo(356, .5));
        }
        expect(wrappedLabelsAnywhere(t), isEmpty);
        // IntrinsicHeight reported the stacked height: nothing overflowed.
        expect(t.takeException(), isNull);
        final box = t.renderObject<RenderBox>(find.byType(AppDialogActions));
        expect(box.size.height, closeTo(close.bottom - main.top, .5));
      },
    );

    testWidgets('large text stacks a row that fits at 100 %', (t) async {
      await _loadFonts(t);
      await t.pumpWidget(bar(260, ['취소', '다시 연결']));
      expect(
        t.getRect(find.byKey(const Key('bar-0'))).top,
        t.getRect(find.byKey(const Key('bar-1'))).top,
      );
      await t.pumpWidget(bar(260, ['취소', '다시 연결'], scale: 2));
      await t.pump();
      expect(
        t.getRect(find.byKey(const Key('bar-1'))).top,
        lessThan(t.getRect(find.byKey(const Key('bar-0'))).top),
      );
      expect(wrappedLabelsAnywhere(t), isEmpty);
    });
  });

  testWidgets('The Host update question with a user connected: three whole buttons', (
    t,
  ) async {
    await expectWholeLabels(
      t,
      () => HostUpdateConfirmDialog(
        machine: bulkMachine(
          'pve-149',
          name: 'TESTER',
          state: bulkState(sessionActive: true),
        ),
        server: 'https://fixture.invalid',
        loadRelease: () async => bulkRelease,
      ),
    );
  });
}

/// [wrappedButtonLabels] without a dialog around the buttons.
List<String> wrappedLabelsAnywhere(WidgetTester t) {
  final faults = <String>[];
  for (final element in find.byType(RichText).evaluate()) {
    final paragraph = element.renderObject;
    if (paragraph is RenderParagraph &&
        paragraph.getMaxIntrinsicWidth(double.infinity) >
            paragraph.size.width + .5) {
      faults.add(paragraph.text.toPlainText());
    }
  }
  return faults;
}

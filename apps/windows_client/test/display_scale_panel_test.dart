import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/display_scale.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/display_scale_panel.dart';

// Widget and memory session state only; no sockets, Windows DPI or native resize.
class _Session extends Fake implements RemoteSession {
  DisplayScaleApplied? applied;
  int? wanted, sourceW = 960, sourceH = 640;
  bool ended = false;
  @override
  DisplayScaleApplied? get displayScaleApplied => applied;
  @override
  int? get displayScaleWanted => wanted;
  @override
  bool get closed => ended;
  @override
  int? get sourceWidth => sourceW;
  @override
  int? get sourceHeight => sourceH;
  @override
  void addListener(VoidCallback listener) {}
  @override
  void removeListener(VoidCallback listener) {}
}

bool _checked(WidgetTester tester, String id) =>
    tester
        .widget<Semantics>(find.byKey(Key('$id-selected')))
        .properties
        .selected ==
    true;

Future<void> _show(
  WidgetTester tester,
  _Session session, {
  required ValueChanged<SessionDisplayChoice> selected,
  Size? viewport = const Size(960, 640),
  bool available = true,
  bool resolutionAvailable = true,
  bool automatic = true,
  double width = 288,
  double textScale = 1,
  Brightness brightness = Brightness.dark,
  bool showTitle = true,
}) => tester.pumpWidget(
  MaterialApp(
    theme: buildAppTheme(brightness: brightness),
    builder: (context, child) => MediaQuery(
      data: MediaQuery.of(context)
          .copyWith(textScaler: TextScaler.linear(textScale)),
      child: child!,
    ),
    home: Scaffold(
      body: Center(
        child: SizedBox(
          width: width,
          child: SingleChildScrollView(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: RepaintBoundary(
                child: DisplayScalePanel(
                  session: session,
                  available: available,
                  viewportPixels: viewport,
                  automaticResolution: automatic,
                  resolutionAvailable: resolutionAvailable,
                  showTitle: showTitle,
                  onSelected: selected,
                ),
              ),
            ),
          ),
        ),
      ),
    ),
  ),
);

void main() {
  test(
    'five viewport recommendations floor width4/height2 without a 4K cap',
    () {
      final choices = recommendedDisplayResolutions(const Size(960, 640));
      expect(choices.map((choice) => (choice.width, choice.height)).toList(), [
        (720, 480),
        (960, 640),
        (1200, 800),
        (1440, 960),
        (1920, 1280),
      ]);
      expect(choices.where((choice) => choice.automatic).length, 1);
      expect(choices[1].automatic, isTrue);
      final wide = recommendedDisplayResolutions(const Size(3840, 2160));
      expect((wide.last.width, wide.last.height), (7680, 4320));
      final uneven = recommendedDisplayResolutions(const Size(987, 654));
      expect((uneven.first.width, uneven.first.height), (740, 490));
      expect((uneven[1].width, uneven[1].height), (984, 654));
      for (final choice in uneven) {
        expect(choice.width! % 4, 0);
        expect(choice.height! % 2, 0);
        expect(choice.scale, isNull);
      }
      expect(recommendedDisplayResolutions(null), isEmpty);
      expect(
        recommendedDisplayResolutions(const Size(double.infinity, 600)),
        isEmpty,
      );
      expect(recommendedDisplayResolutions(Size.zero), isEmpty);
    },
  );

  for (final config in [
    (const Size(1920, 1080), Brightness.dark, 1.0),
    (const Size(1366, 768), Brightness.light, 1.5),
    (const Size(960, 640), Brightness.dark, 2.0),
    (const Size(320, 640), Brightness.light, 2.0),
  ]) {
    testWidgets(
      'five scale steps across ${config.$1} ${config.$2} ${config.$3}',
      (tester) async {
        tester.view.physicalSize = config.$1;
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final session = _Session()
          ..applied = const DisplayScaleApplied(
            ok: true,
            scale: 125,
            maximum: 200,
            code: 0,
          );
        SessionDisplayChoice? selected;
        await _show(
          tester,
          session,
          selected: (choice) => selected = choice,
          textScale: config.$3,
          brightness: config.$2,
        );
        expect(tester.takeException(), isNull);
        // The steps are one row of TextButtons (배율 sits inside the 화면
        // panel, so they lie across).
        final steps = find.byKey(const Key('display-scale-steps'));
        expect(steps, findsOneWidget);
        expect(
          find.descendant(of: steps, matching: find.byType(TextButton)),
          findsNWidgets(5),
        );
        expect(find.byType(TextButton), findsNWidgets(5));
        expect(find.byType(OutlinedButton), findsNothing);
        for (final step in [100, 125, 150, 175, 200]) {
          expect(
            find.descendant(
              of: find.byKey(Key('display-scale-$step')),
              matching: find.text('$step%'),
            ),
            findsOneWidget,
          );
        }
        // The current step is told by its colour and Semantics, not a check.
        expect(
          find.descendant(of: steps, matching: find.byType(Icon)),
          findsNothing,
        );
        expect(find.text('추천 해상도'), findsNothing);
        expect(find.byKey(const Key('display-resolution-100')), findsNothing);
        expect(find.byKey(const Key('display-scale-250')), findsNothing);
        expect(find.text('자동 · 현재 125%'), findsOneWidget);
        expect(_checked(tester, 'display-scale-125'), isTrue);
        for (final step in [100, 150, 175, 200]) {
          expect(_checked(tester, 'display-scale-$step'), isFalse);
        }
        if (const bool.fromEnvironment('MULTITAP_SCALE_ONLY_GOLDENS')) {
          await expectLater(
            find.byKey(const Key('display-scale-panel')),
            matchesGoldenFile(
              'goldens/scale_only_${config.$1.width.toInt()}_${config.$2.name}_${config.$3}.png',
            ),
          );
        }
        final keys = [
          for (final step in [100, 125, 150, 175, 200]) 'display-scale-$step',
        ];
        final height = tester
            .getSize(find.byKey(Key('${keys.first}-row')))
            .height;
        // One height for every step: 36 at the least, growing with the
        // text size up to 52.
        for (final key in keys) {
          expect(tester.getSize(find.byKey(Key('$key-row'))).height, height);
          expect(tester.getSize(find.byKey(Key(key))).height, height);
        }
        expect(height, greaterThanOrEqualTo(36));
        expect(height, lessThanOrEqualTo(52));
        // Side by side on one line, equally wide, in order, inside the
        // steps' container.
        final container = tester.getRect(steps);
        final first = tester.getRect(
          find.byKey(const Key('display-scale-100-row')),
        );
        expect(first.left, greaterThanOrEqualTo(container.left));
        var lastRight = first.right;
        for (final value in [125, 150, 175, 200]) {
          final row = tester.getRect(
            find.byKey(Key('display-scale-$value-row')),
          );
          expect(row.top, first.top);
          expect(row.width, closeTo(first.width, 1e-6));
          expect(row.left, greaterThan(lastRight));
          lastRight = row.right;
        }
        expect(lastRight, lessThanOrEqualTo(container.right));
        expect(first.top, greaterThanOrEqualTo(container.top));
        expect(first.bottom, lessThanOrEqualTo(container.bottom));
        // The status lines are under the steps.
        expect(
          tester.getRect(find.byKey(const Key('display-scale-status'))).top,
          greaterThanOrEqualTo(container.bottom),
        );
        final scale = find.byKey(const Key('display-scale-150'));
        await tester.ensureVisible(scale);
        await tester.tap(scale);
        expect(selected?.scale, 150);
        expect(selected?.width, isNull);
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'current source and scale ACK remain separate from requested values',
    (tester) async {
      final session = _Session()
        ..sourceW = 1200
        ..sourceH = 800
        ..wanted = 175
        ..applied = const DisplayScaleApplied(
          ok: true,
          scale: 125,
          maximum: 150,
          code: 0,
        );
      await _show(tester, session, automatic: false, selected: (_) {});
      expect(_checked(tester, 'display-scale-125'), isTrue);
      expect(_checked(tester, 'display-scale-175'), isFalse);
      expect(find.text('해상도 자동 · 현재 1200 × 800'), findsOneWidget);
      expect(find.text('적용 125% · 요청 175%'), findsOneWidget);
      expect(find.text('이 화면 크기에서 최대 150%'), findsOneWidget);
      for (final scale in [175, 200]) {
        expect(
          tester
              .widget<TextButton>(find.byKey(Key('display-scale-$scale')))
              .onPressed,
          isNull,
        );
      }
      expect(
        tester
            .widget<TextButton>(find.byKey(const Key('display-scale-150')))
            .onPressed,
        isNotNull,
      );
    },
  );

  testWidgets(
    'unsupported and unknown viewport do not advertise available sizes',
    (tester) async {
      final session = _Session()
        ..sourceW = null
        ..sourceH = null;
      await _show(
        tester,
        session,
        viewport: null,
        available: false,
        resolutionAvailable: false,
        selected: (_) => fail('unavailable choice selected'),
      );
      expect(find.byType(TextButton), findsNWidgets(5));
      for (final button in tester.widgetList<TextButton>(
        find.byType(TextButton),
      )) {
        expect(button.onPressed, isNull);
      }
      expect(find.text('해상도 자동 · 현재 크기 확인 전'), findsOneWidget);
      expect(find.text('이 연결에서는 배율을 바꿀 수 없어요.'), findsOneWidget);
      session.applied = const DisplayScaleApplied(
        ok: false,
        scale: 0,
        maximum: 0,
        code: 3,
      );
      await _show(
        tester,
        session,
        resolutionAvailable: false,
        selected: (_) {},
      );
      expect(find.byKey(const Key('display-resolution-100')), findsNothing);
      expect(
        find.text('자동 · 현재 배율 확인 못함\nWindows가 배율 변경을 허용하지 않았어요.'),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets(
    'automatic scale distinguishes awaiting observation, actual readback and failure',
    (tester) async {
      final session = _Session();
      await _show(tester, session, selected: (_) {});
      expect(find.text('자동 · 현재 배율 확인 전'), findsOneWidget);
      for (final scale in [100, 125, 150, 175, 200]) {
        expect(_checked(tester, 'display-scale-$scale'), isFalse);
      }
      session.applied = const DisplayScaleApplied(
        ok: true,
        scale: 125,
        maximum: 200,
        code: 0,
      );
      await _show(tester, session, selected: (_) {});
      expect(find.text('자동 · 현재 125%'), findsOneWidget);
      expect(_checked(tester, 'display-scale-125'), isTrue);
      expect(session.wanted, isNull);
      // A readback outside the five offered choices is still shown truthfully.
      session.applied = const DisplayScaleApplied(
        ok: true,
        scale: 225,
        maximum: 250,
        code: 0,
      );
      await _show(tester, session, selected: (_) {});
      expect(find.text('자동 · 현재 225%'), findsOneWidget);
      for (final scale in [100, 125, 150, 175, 200]) {
        expect(_checked(tester, 'display-scale-$scale'), isFalse);
      }
      session.applied = const DisplayScaleApplied(
        ok: false,
        scale: 0,
        maximum: 0,
        code: 2,
      );
      await _show(tester, session, selected: (_) {});
      expect(
        find.text('자동 · 현재 배율 확인 못함\nWindows 배율을 확인하지 못했어요.'),
        findsOneWidget,
      );
      expect(find.text('자동 · 현재 100%'), findsNothing);
      session.wanted = 150;
      session.applied = const DisplayScaleApplied(
        ok: true,
        scale: 150,
        maximum: 200,
        code: 0,
      );
      await _show(tester, session, selected: (_) {});
      expect(find.text('적용 150%'), findsOneWidget);
      expect(find.text('자동 · 현재 150%'), findsNothing);
      expect(tester.takeException(), isNull);
    },
  );

  // display-scale-outputs-v1: the Host reports each monitor's actual value.
  DisplayScaleApplied dual(
    (int, int, int) first,
    (int, int, int) second, {
    bool ok = true,
    int code = 0,
  }) {
    DisplayScaleApplied output(String id, (int, int, int) value) =>
        DisplayScaleApplied(
          ok: value.$3 == 0,
          scale: value.$1,
          maximum: value.$2,
          code: value.$3,
          id: id,
        );
    return DisplayScaleApplied(
      ok: ok,
      scale: first.$1,
      maximum: first.$2 > second.$2 ? first.$2 : second.$2,
      code: code,
      outputs: [output('0', first), output('1', second)],
    );
  }

  testWidgets('two monitors show one value only when both really use it', (
    tester,
  ) async {
    final session = _Session()
      ..wanted = 150
      ..applied = dual((150, 200, 0), (150, 200, 0));
    await _show(tester, session, automatic: false, selected: (_) {});
    expect(find.text('적용 150%'), findsOneWidget);
    expect(_checked(tester, 'display-scale-150'), isTrue);
    // Windows allows only 175 % on the first monitor: both actual values,
    // no single checked row, and 200 % stays selectable for the second.
    session
      ..wanted = 200
      ..applied = dual((175, 175, 0), (200, 200, 0));
    await _show(tester, session, automatic: false, selected: (_) {});
    expect(find.text('적용 · 모니터 1 175% · 모니터 2 200%'), findsOneWidget);
    expect(find.text('최대 · 모니터 1 175% · 모니터 2 200%'), findsOneWidget);
    for (final scale in [100, 125, 150, 175, 200]) {
      expect(_checked(tester, 'display-scale-$scale'), isFalse);
    }
    expect(
      tester
          .widget<TextButton>(find.byKey(const Key('display-scale-200')))
          .onPressed,
      isNotNull,
    );
    // Automatic: each monitor's observed value, never one invented for both.
    session
      ..wanted = null
      ..applied = dual((100, 200, 0), (125, 200, 0));
    await _show(tester, session, selected: (_) {});
    expect(find.text('자동 · 모니터 1 100% · 모니터 2 125%'), findsOneWidget);
    expect(find.textContaining('최대'), findsNothing);
    // One monitor failed: the reason and what each monitor actually has.
    session
      ..wanted = 150
      ..applied = dual((150, 200, 0), (0, 0, 1), ok: false, code: 1);
    await _show(tester, session, automatic: false, selected: (_) {});
    expect(
      find.text('원격 화면을 찾지 못했어요.\n모니터 1 150% · 모니터 2 확인 못함'),
      findsOneWidget,
    );
    expect(tester.takeException(), isNull);
  });

  testWidgets('the title is left out where the panel around it says 배율', (
    tester,
  ) async {
    await _show(tester, _Session(), selected: (_) {});
    expect(find.text('배율'), findsOneWidget);
    // Inside the 화면 panel the caption above the steps is the title.
    await _show(tester, _Session(), selected: (_) {}, showTitle: false);
    expect(find.text('배율'), findsNothing);
    expect(find.byKey(const Key('display-scale-steps')), findsOneWidget);
    expect(find.byType(TextButton), findsNWidgets(5));
    expect(find.byKey(const Key('display-scale-status')), findsOneWidget);
    expect(find.byKey(const Key('display-resolution-status')), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  testWidgets('keyboard chooses a scale without a manual resolution action', (
    tester,
  ) async {
    SessionDisplayChoice? selected;
    await _show(tester, _Session(), selected: (choice) => selected = choice);
    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    await tester.pump();
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();
    expect(selected?.scale, 100);
    expect(selected?.width, isNull);
    expect(tester.takeException(), isNull);
  });
}

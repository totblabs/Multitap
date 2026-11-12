// Design polish wave 1 (2026-11-10 audit): what the shared theme, palette and
// dialog code promise after it. Widgets only; nothing outside the test
// process is touched.
//
//   F-04 a segmented choice has no amber frame (none around an unchosen
//        segment, none at all)
//   F-18 a switch that is on shows its thumb in the dark theme
//   F-19 a toast is a small card, and a result and a failure look different
//   F-17 a button with keyboard focus has a ring; the light theme's focus
//        colour is readable on white
//   F-03 a failure reads as text beside one red icon; 확인 is the grey button
//   F-16 a dialog's icon is amber only beside an amber main action
//   F-20, F-21, F-22 the palette's new tokens
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_launcher.dart'
    show androidTheme;
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/app_toast.dart';

/// WCAG contrast of two opaque colours.
double contrast(Color a, Color b) {
  double luminance(Color c) {
    double channel(double v) =>
        v <= .03928 ? v / 12.92 : math.pow((v + .055) / 1.055, 2.4).toDouble();
    return .2126 * channel(c.r) + .7152 * channel(c.g) + .0722 * channel(c.b);
  }

  final la = luminance(a), lb = luminance(b);
  return (math.max(la, lb) + .05) / (math.min(la, lb) + .05);
}

bool _amberish(Color c) {
  final hsl = HSLColor.fromColor(c.withValues(alpha: 1));
  return c.a > 0 &&
      hsl.saturation > .35 &&
      hsl.hue >= 20 &&
      hsl.hue <= 55;
}

Future<void> _pump(
  WidgetTester tester,
  Widget home, {
  Brightness brightness = Brightness.dark,
  Size size = const Size(1280, 800),
  double scale = 1,
  ThemeData Function(ThemeData)? theme,
}) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = size;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  final base = buildAppTheme(brightness: brightness);
  await tester.pumpWidget(
    MaterialApp(
      theme: theme?.call(base) ?? base,
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(
          context,
        ).copyWith(textScaler: TextScaler.linear(scale)),
        child: child!,
      ),
      home: Scaffold(body: home),
    ),
  );
}

/// Keyboard focus is drawn (a test's taps are touches, which hide it).
void _keyboard() {
  FocusManager.instance.highlightStrategy =
      FocusHighlightStrategy.alwaysTraditional;
  addTearDown(
    () => FocusManager.instance.highlightStrategy =
        FocusHighlightStrategy.automatic,
  );
}

/// The edge a button is painted with now.
BorderSide _side(WidgetTester tester, Finder button) =>
    (tester
                .widget<Material>(
                  find
                      .descendant(of: button, matching: find.byType(Material))
                      .first,
                )
                .shape!
            as OutlinedBorder)
        .side;

Color? _fill(WidgetTester tester, Finder button) => tester
    .widget<Material>(
      find.descendant(of: button, matching: find.byType(Material)).first,
    )
    .color;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  for (final brightness in Brightness.values) {
    final p = AppPalette(brightness == Brightness.dark);
    final name = brightness.name;

    group('F-04 segmented choice ($name)', () {
      for (final android in [false, true]) {
        testWidgets(
          'no amber anywhere, the chosen segment raised out of a grey groove'
          '${android ? ' (Android theme)' : ''}',
          (tester) async {
            await _pump(
              tester,
              Center(
                child: SegmentedButton<int>(
                  showSelectedIcon: false,
                  segments: const [
                    ButtonSegment(value: 1, label: Text('one')),
                    ButtonSegment(value: 2, label: Text('two')),
                    ButtonSegment(value: 3, label: Text('three')),
                  ],
                  selected: const {2},
                  onSelectionChanged: (_) {},
                ),
              ),
              brightness: brightness,
              theme: android ? androidTheme : null,
            );
            // The frame and the dividers: one line the control resolves once
            // for all its segments, with "selected" set because one is.
            final control = tester.renderObject(
              find.byWidgetPredicate(
                (w) => '${w.runtimeType}'.startsWith(
                  '_SegmentedButtonRenderWidget',
                ),
              ),
            );
            final frame =
                ((control as dynamic).enabledBorder as OutlinedBorder).side;
            expect(frame.color, p.tileLine);
            expect(_amberish(frame.color), isFalse);

            Finder segment(String label) =>
                find.widgetWithText(TextButton, label);
            expect(_fill(tester, segment('two')), p.segmentOn);
            for (final other in ['one', 'three']) {
              expect(_fill(tester, segment(other)), p.segmentTrack);
            }
            Color ink(String label) => tester
                .widget<RichText>(
                  find.descendant(
                    of: segment(label),
                    matching: find.byType(RichText),
                  ),
                )
                .text
                .style!
                .color!;
            expect(ink('two'), p.ink);
            expect(ink('one'), p.muted);
            for (final colour in [
              p.segmentOn,
              p.segmentTrack,
              ink('two'),
              ink('one'),
            ]) {
              expect(_amberish(colour), isFalse);
            }
            // Both labels stay readable on their own fill.
            expect(contrast(p.ink, p.segmentOn), greaterThan(4.5));
            expect(contrast(p.muted, p.segmentTrack), greaterThan(4.5));
            if (android) {
              // Only the size differs by finger.
              expect(
                tester.getSize(segment('two')).height,
                greaterThanOrEqualTo(48),
              );
            }
          },
        );
      }

      testWidgets('the frame changes only for keyboard focus', (tester) async {
        final side = buildAppTheme(
          brightness: brightness,
        ).segmentedButtonTheme.style!.side!;
        expect(
          side.resolve({WidgetState.selected}),
          side.resolve(const <WidgetState>{}),
        );
        // Focus from a click or a touch: no ring.
        FocusManager.instance.highlightStrategy =
            FocusHighlightStrategy.alwaysTouch;
        addTearDown(
          () => FocusManager.instance.highlightStrategy =
              FocusHighlightStrategy.automatic,
        );
        expect(
          side.resolve({WidgetState.selected, WidgetState.focused}),
          BorderSide(color: p.tileLine),
        );
        FocusManager.instance.highlightStrategy =
            FocusHighlightStrategy.alwaysTraditional;
        expect(
          side.resolve({WidgetState.selected, WidgetState.focused}),
          BorderSide(color: p.focus, width: 2),
        );
      });
    });

    test('F-18 a switch that is on shows its thumb ($name)', () {
      final theme = buildAppTheme(brightness: brightness).switchTheme;
      const on = {WidgetState.selected};
      final thumb = theme.thumbColor!.resolve(on)!;
      final track = theme.trackColor!.resolve(on)!;
      expect(contrast(thumb, track), greaterThan(3));
      // Off: the thumb on its track, as before.
      expect(
        contrast(
          theme.thumbColor!.resolve(const {})!,
          theme.trackColor!.resolve(const {})!,
        ),
        greaterThan(3),
      );
    });

    group('F-19 toast ($name)', () {
      Finder card() => find
          .descendant(
            of: find.byType(SnackBar),
            matching: find.byType(Material),
          )
          .first;

      Future<BuildContext> host(
        WidgetTester tester, {
        Size size = const Size(1920, 1080),
        double scale = 1,
      }) async {
        await _pump(
          tester,
          const SizedBox.expand(key: Key('host')),
          brightness: brightness,
          size: size,
          scale: scale,
        );
        return tester.element(find.byKey(const Key('host')));
      }

      testWidgets('a small card at the bottom centre of a wide window', (
        tester,
      ) async {
        final context = await host(tester);
        showAppToast(context, '자동 연결을 취소했어요.');
        await tester.pumpAndSettle();
        final rect = tester.getRect(card());
        expect(rect.width, appToastWidth);
        expect(rect.center.dx, closeTo(960, .5));
        expect(1080 - rect.bottom, closeTo(16, .5));
        ScaffoldMessenger.of(context).removeCurrentSnackBar();
        await tester.pumpAndSettle();
      });

      testWidgets('a SnackBar shown without the helper is the same card', (
        tester,
      ) async {
        final context = await host(tester);
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(const SnackBar(content: Text('plain')));
        await tester.pumpAndSettle();
        final rect = tester.getRect(card());
        expect(rect.width, appToastWidth);
        expect(rect.center.dx, closeTo(960, .5));
        ScaffoldMessenger.of(context).removeCurrentSnackBar();
        await tester.pumpAndSettle();
      });

      testWidgets('a narrow window keeps a 16 px gutter on each side', (
        tester,
      ) async {
        final context = await host(tester, size: const Size(360, 740));
        showAppToast(context, '테마를 저장하지 못했어요. 다시 선택해 주세요.');
        await tester.pumpAndSettle();
        final rect = tester.getRect(card());
        expect(rect.left, closeTo(16, .5));
        expect(rect.right, closeTo(344, .5));
        expect(tester.takeException(), isNull);
        ScaffoldMessenger.of(context).removeCurrentSnackBar();
        await tester.pumpAndSettle();
      });

      testWidgets('large text widens it, never past the window', (
        tester,
      ) async {
        var context = await host(tester, scale: 2);
        showAppToast(context, '진단 정보를 복사했어요.', kind: AppToastKind.done);
        await tester.pumpAndSettle();
        expect(tester.getRect(card()).width, appToastWidth * 1.5);
        ScaffoldMessenger.of(context).removeCurrentSnackBar();
        await tester.pumpAndSettle();

        context = await host(tester, size: const Size(480, 800), scale: 2);
        showAppToast(context, '진단 정보를 복사했어요.', kind: AppToastKind.done);
        await tester.pumpAndSettle();
        expect(tester.getRect(card()).width, 480 - 32);
        expect(tester.takeException(), isNull);
        ScaffoldMessenger.of(context).removeCurrentSnackBar();
        await tester.pumpAndSettle();
      });

      testWidgets('a result, a failure and a plain note look different', (
        tester,
      ) async {
        final context = await host(tester);
        final looks = <AppToastKind, (IconData?, Color?, Color)>{};
        for (final kind in AppToastKind.values) {
          showAppToast(context, '$kind', kind: kind);
          await tester.pumpAndSettle();
          final icons = find.descendant(
            of: find.byType(SnackBar),
            matching: find.byType(Icon),
          );
          final icon = icons.evaluate().isEmpty
              ? null
              : tester.widget<Icon>(icons);
          final edge =
              (tester.widget<Material>(card()).shape!
                      as RoundedRectangleBorder)
                  .side
                  .color;
          looks[kind] = (icon?.icon, icon?.color, edge);
          ScaffoldMessenger.of(context).removeCurrentSnackBar();
          await tester.pumpAndSettle();
        }
        // The card is dark in both themes: the dark theme's status colours.
        const status = AppPalette(true);
        expect(looks[AppToastKind.plain]!.$1, isNull);
        expect(looks[AppToastKind.done]!.$1, Icons.check_circle_rounded);
        expect(looks[AppToastKind.done]!.$2, status.green);
        expect(looks[AppToastKind.failed]!.$1, Icons.error_outline_rounded);
        expect(looks[AppToastKind.failed]!.$2, status.red);
        expect(looks.values.map((look) => look.$3).toSet(), hasLength(3));
        // Each icon stands out from the card.
        for (final colour in [status.green, status.red]) {
          expect(contrast(colour, p.toast), greaterThan(3));
        }
        expect(contrast(AppPalette.toastInk, p.toast), greaterThan(7));
      });
    });

    group('F-17 keyboard focus ($name)', () {
      testWidgets('the main button of a question shows a ring', (tester) async {
        _keyboard();
        await _pump(
          tester,
          const AppConfirmDialog(
            title: '연결을 끊을까요?',
            confirmLabel: '연결 끊기',
            confirmKey: Key('yes'),
            cancelKey: Key('no'),
          ),
          brightness: brightness,
        );
        await tester.pump();
        final ring = _side(tester, find.byKey(const Key('yes')));
        expect(ring.width, AppButtons.focusWidth);
        // On the amber fill the ring is the label's colour, with a rim of
        // the fill left outside it.
        expect(ring.color, p.onAmber);
        expect(ring.strokeAlign, lessThan(BorderSide.strokeAlignInside));
        expect(contrast(ring.color, p.amber), greaterThan(3));
        expect(_side(tester, find.byKey(const Key('no'))), BorderSide.none);
      });

      testWidgets('a question that cuts access rings 취소', (tester) async {
        _keyboard();
        await _pump(
          tester,
          const AppConfirmDialog(
            title: 'Multitap을 종료할까요?',
            confirmLabel: '종료',
            tone: AppTone.danger,
            confirmKey: Key('yes'),
            cancelKey: Key('no'),
          ),
          brightness: brightness,
        );
        await tester.pump();
        final ring = _side(tester, find.byKey(const Key('no')));
        expect(ring.width, AppButtons.focusWidth);
        expect(ring.color, p.focus);
        expect(contrast(ring.color, p.quiet), greaterThan(3));
        expect(contrast(ring.color, p.tile), greaterThan(3));
        expect(_side(tester, find.byKey(const Key('yes'))), BorderSide.none);
      });

      testWidgets('a button focused by a touch shows none', (tester) async {
        FocusManager.instance.highlightStrategy =
            FocusHighlightStrategy.alwaysTouch;
        addTearDown(
          () => FocusManager.instance.highlightStrategy =
              FocusHighlightStrategy.automatic,
        );
        await _pump(
          tester,
          const AppConfirmDialog(
            title: '연결을 끊을까요?',
            confirmLabel: '연결 끊기',
            confirmKey: Key('yes'),
          ),
          brightness: brightness,
        );
        await tester.pump();
        expect(_side(tester, find.byKey(const Key('yes'))), BorderSide.none);
      });

      testWidgets('every shared button look has the ring', (tester) async {
        _keyboard();
        final styles = {
          'quiet': AppButtons.quiet(p),
          'tonal': AppButtons.tonal(p),
          'outlined': AppButtons.outlined(p),
          'link': AppButtons.link(p),
          'filled': AppButtons.filled(p),
          'danger': AppButtons.filled(p, tone: AppTone.danger),
        };
        for (final style in styles.entries) {
          const focused = {WidgetState.focused};
          final ring = style.value.side!.resolve(focused)!;
          expect(ring.width, AppButtons.focusWidth, reason: style.key);
          expect(
            style.value.side!.resolve(const {
              WidgetState.focused,
              WidgetState.disabled,
            }),
            isNot(ring),
            reason: '${style.key}: a button that is off has no ring',
          );
        }
        // An outlined button keeps its own edge when it is not focused.
        expect(
          AppButtons.outlined(p).side!.resolve(const {}),
          BorderSide(color: p.tileLine),
        );
      });

      test('the focus colour is readable on the dialog surface', () {
        expect(contrast(p.focus, p.tile), greaterThan(3));
        final field =
            buildAppTheme(
                  brightness: brightness,
                ).inputDecorationTheme.focusedBorder!
                as OutlineInputBorder;
        expect(field.borderSide.color, p.focus);
        expect(contrast(field.borderSide.color, p.tileWell), greaterThan(3));
      });
    });

    group('F-03 and F-16 dialogs ($name)', () {
      AppTone tone(WidgetTester tester) =>
          tester.widget<AppToneIcon>(find.byType(AppToneIcon)).tone;

      testWidgets('a notice is acknowledged with the grey button at the right', (
        tester,
      ) async {
        await _pump(
          tester,
          Builder(
            builder: (context) => TextButton(
              key: const Key('open'),
              onPressed: () => showAppNotice(
                context,
                icon: Icons.error_outline_rounded,
                tone: AppTone.danger,
                title: 'Windows 화면을 넘겨받지 못했어요',
                message: 'Host의 세션 상태를 확인하세요.',
                closeKey: const Key('ok'),
              ),
              child: const Text('open'),
            ),
          ),
          brightness: brightness,
        );
        await tester.tap(find.byKey(const Key('open')));
        await tester.pumpAndSettle();
        final ok = find.byKey(const Key('ok'));
        expect(tester.widget(ok), isA<TextButton>());
        expect(_fill(tester, ok), p.quiet);
        expect(find.byType(FilledButton), findsNothing);
        // The dialog's own surface (Dialog itself spans the window).
        final dialog = tester.getRect(
          find
              .descendant(
                of: find.byType(Dialog),
                matching: find.byType(Material),
              )
              .first,
        );
        final button = tester.getRect(ok);
        expect(button.width, greaterThanOrEqualTo(AppDialog.loneActionWidth));
        expect(button.width, lessThan(dialog.width / 2));
        expect(dialog.right - button.right, closeTo(22, 1.5));
        // The sentence is ordinary text; the red is the icon.
        final message = tester.widget<Text>(
          find.text('Host의 세션 상태를 확인하세요.'),
        );
        expect(message.style!.color, p.muted);
        expect(tone(tester), AppTone.danger);
        await tester.tap(ok);
        await tester.pumpAndSettle();
      });

      testWidgets('a status line is one tinted icon and ordinary text', (
        tester,
      ) async {
        await _pump(
          tester,
          const AppStatusLine('인쇄하지 못했어요.', textKey: Key('line')),
          brightness: brightness,
        );
        final text = tester.widget<Text>(find.byKey(const Key('line')));
        expect(text.style!.color, p.ink);
        final icon = tester.widget<Icon>(find.byType(Icon));
        expect(icon.color, p.red);
        expect(icon.icon, Icons.error_outline_rounded);
      });

      testWidgets('the icon is amber only beside an amber main action', (
        tester,
      ) async {
        Widget dialog(List<Widget> actions, {AppTone? tone}) => AppDialog(
          key: UniqueKey(),
          icon: Icons.info_outline_rounded,
          tone: tone,
          title: 'title',
          actions: actions,
        );
        final close = TextButton(
          onPressed: () {},
          style: AppButtons.quiet(p),
          child: const Text('닫기'),
        );
        final go = FilledButton(
          onPressed: () {},
          style: AppButtons.filled(p),
          child: const Text('연결'),
        );
        // Information: no action, or only a grey one.
        await _pump(tester, dialog(const []), brightness: brightness);
        expect(tone(tester), AppTone.neutral);
        await _pump(tester, dialog([close]), brightness: brightness);
        expect(tone(tester), AppTone.neutral);
        // A main action: the icon and the button read as a pair.
        await _pump(tester, dialog([close, go]), brightness: brightness);
        expect(tone(tester), AppTone.accent);
        await _pump(
          tester,
          dialog([close, Tooltip(message: 'why', child: go)]),
          brightness: brightness,
        );
        expect(tone(tester), AppTone.accent);
        // A filled button in another look is not the amber main action.
        await _pump(
          tester,
          dialog([
            close,
            FilledButton(
              onPressed: () {},
              style: AppButtons.tonal(p),
              child: const Text('테스트'),
            ),
          ]),
          brightness: brightness,
        );
        expect(tone(tester), AppTone.neutral);
        // A named tone always wins.
        await _pump(
          tester,
          dialog([close], tone: AppTone.accent),
          brightness: brightness,
        );
        expect(tone(tester), AppTone.accent);
        await _pump(
          tester,
          dialog([close, go], tone: AppTone.warn),
          brightness: brightness,
        );
        expect(tone(tester), AppTone.warn);
      });

      testWidgets('the grey icon: a grey circle, an amber spinner when busy', (
        tester,
      ) async {
        await _pump(
          tester,
          const AppToneIcon(icon: Icons.info_outline, tone: AppTone.neutral),
          brightness: brightness,
        );
        final circle =
            tester
                    .widget<Container>(
                      find.descendant(
                        of: find.byType(AppToneIcon),
                        matching: find.byType(Container),
                      ),
                    )
                    .decoration!
                as BoxDecoration;
        expect(circle.color, p.quiet);
        expect(tester.widget<Icon>(find.byType(Icon)).color, p.muted);
        expect(contrast(p.muted, p.quiet), greaterThan(3));
        await _pump(
          tester,
          const AppToneIcon(
            icon: Icons.info_outline,
            tone: AppTone.neutral,
            busy: true,
          ),
          brightness: brightness,
        );
        expect(
          tester
              .widget<CircularProgressIndicator>(
                find.byType(CircularProgressIndicator),
              )
              .color,
          p.amber,
        );
      });

      testWidgets('a text button or a fold with no style of its own is a link', (
        tester,
      ) async {
        await _pump(
          tester,
          AppDialog(
            title: 'title',
            body: [
              TextButton(
                key: const Key('link'),
                onPressed: () {},
                child: const Text('자세히'),
              ),
              const ExpansionTile(
                key: Key('fold'),
                initiallyExpanded: true,
                title: Text('변경 내역'),
                children: [SizedBox(height: 8)],
              ),
            ],
          ),
          brightness: brightness,
        );
        await tester.pumpAndSettle();
        Color ink(Finder scope, String label) => tester
            .widget<RichText>(
              find.descendant(of: scope, matching: find.byType(RichText)),
            )
            .text
            .style!
            .color!;
        expect(ink(find.byKey(const Key('link')), '자세히'), p.ink2);
        expect(_amberish(p.ink2), isFalse);
        expect(contrast(p.ink2, p.tile), greaterThan(4.5));
        final fold = find.byKey(const Key('fold'));
        expect(
          tester
              .widget<RichText>(
                find.descendant(
                  of: fold,
                  matching: find.byWidgetPredicate(
                    (w) => w is RichText && w.text.toPlainText() == '변경 내역',
                  ),
                ),
              )
              .text
              .style!
              .color,
          p.ink,
        );
        final iconTheme = IconTheme.of(
          tester.element(
            find.descendant(of: fold, matching: find.byType(Icon)),
          ),
        );
        expect(iconTheme.color, p.muted);
      });
    });

    test('F-20, F-21, F-22 palette tokens ($name)', () {
      // One strength for a failure box and for a Host-side box.
      expect(p.redSoft, p.red.withValues(alpha: p.dark ? .12 : .08));
      expect(p.redLine, p.red.withValues(alpha: p.dark ? .34 : .30));
      expect(p.blueSoft, p.blue.withValues(alpha: p.dark ? .13 : .09));
      expect(p.blueLine, p.blue.withValues(alpha: p.dark ? .30 : .28));
      // Faint text can be read; a control that is off keeps the old value,
      // and the old name still means that value (the launcher reads it).
      expect(contrast(p.faintText, p.tile), greaterThan(4.5));
      expect(
        p.disabled,
        p.dark ? const Color(0xFF5E6A73) : const Color(0xFF8A949C),
      );
      expect(p.faint, p.disabled);
      // The stage is one dark colour in both themes.
      expect(AppPalette.stage, const Color(0xFF0B0E10));
    });
  }
}

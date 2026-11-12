import 'package:flutter/foundation.dart' show ValueListenable, ValueNotifier;
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/models/session_guide.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/primary_region.dart';
import 'package:multitap_client/ui/session_guide_dialog.dart';

class _Dialog {
  bool completed = false;
  bool? result;
  Future<void> mount(
    WidgetTester tester, {
    Size size = const Size(960, 640),
    Brightness brightness = Brightness.dark,
    double scale = 1,
    bool canRemember = true,
    bool hidden = false,
    bool showPreference = true,
    String? note,
    Rect? primary,
    bool reducedMotion = false,
    bool settleOpening = true,
    ValueListenable<SessionGuidePresentation>? presentation,
  }) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = size;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: brightness),
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context).copyWith(
            textScaler: TextScaler.linear(scale),
            disableAnimations: reducedMotion,
          ),
          child: PrimaryRegionScope(child: child!),
        ),
        home: Scaffold(
          body: Builder(
            builder: (context) => TextButton(
              key: const Key('open-guide'),
              onPressed: () async {
                result = await Navigator.of(context, rootNavigator: true).push(
                  createSessionGuideRoute(
                    context,
                    machine: '설계 작업용 VM',
                    canRemember: canRemember,
                    hidden: hidden,
                    showPreference: showPreference,
                    preferenceNote: note,
                    presentation: presentation,
                    content: SessionGuideContent.fromFacts(
                      printerGranted: true,
                      printerReady: true,
                      usbGranted: true,
                      usbReady: true,
                      foldersGranted: true,
                      foldersReady: true,
                      clipboardGranted: true,
                      clipboardReady: true,
                      policy: const ClipboardPolicy.all(),
                    ),
                  ),
                );
                completed = true;
              },
              child: const Text('열기'),
            ),
          ),
        ),
      ),
    );
    if (primary != null) {
      final context = tester.element(find.byKey(const Key('open-guide')));
      PrimaryRegionScope.maybeOf(context)!.value = primary;
      await tester.pump();
    }
    await tester.tap(find.byKey(const Key('open-guide')));
    await tester.pump();
    if (settleOpening) {
      await tester.pump(const Duration(milliseconds: 500));
    }
    await tester.pump();
    expect(find.byKey(const Key('session-guide-dialog')), findsOneWidget);
  }

  Future<void> hideCheckbox(WidgetTester tester) async {
    final box = find.byKey(const Key('session-guide-hide'));
    await tester.ensureVisible(box);
    await tester.pump();
    await tester.tap(box);
    await tester.pump();
  }

  Future<void> confirm(WidgetTester tester) async {
    await tester.tap(find.byKey(const Key('confirm-session-guide')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    await tester.pump();
  }
}

void main() {
  for (final size in [const Size(960, 640), const Size(320, 640)]) {
    testWidgets(
      'manual content omits preference and returns no write intent at $size',
      (tester) async {
        final dialog = _Dialog();
        await dialog.mount(
          tester,
          size: size,
          canRemember: true,
          hidden: true,
          showPreference: false,
        );
        expect(find.byKey(const Key('session-guide-hide')), findsNothing);
        expect(find.textContaining('다시 보지 않기'), findsNothing);
        await dialog.confirm(tester);
        expect(dialog.completed, isTrue);
        expect(dialog.result, isNull);
        expect(tester.takeException(), isNull);
      },
    );
  }

  for (final size in [
    const Size(1920, 1080),
    const Size(1366, 768),
    const Size(960, 640),
  ]) {
    for (final brightness in Brightness.values) {
      for (final scale in [1.0, 1.5, 2.0]) {
        testWidgets(
          'guide geometry ${size.width}x${size.height} ${brightness.name} $scale',
          (tester) async {
            final dialog = _Dialog();
            await dialog.mount(
              tester,
              size: size,
              brightness: brightness,
              scale: scale,
            );
            final bounds = tester.getRect(find.byType(Dialog));
            expect(bounds.left, greaterThanOrEqualTo(0));
            expect(bounds.top, greaterThanOrEqualTo(0));
            expect(bounds.right, lessThanOrEqualTo(size.width));
            expect(bounds.bottom, lessThanOrEqualTo(size.height));
            expect(
              find.byKey(const Key('close-session-guide')).hitTestable(),
              findsOneWidget,
            );
            expect(
              find.byKey(const Key('confirm-session-guide')).hitTestable(),
              findsOneWidget,
            );
            for (final feature in SessionGuideFeature.values) {
              expect(
                find.byKey(Key('session-guide-tip-${feature.name}')),
                findsOneWidget,
              );
            }
            await dialog.hideCheckbox(tester);
            final checkboxTexts = find.descendant(
              of: find.byKey(const Key('session-guide-hide')),
              matching: find.byType(RichText),
            );
            expect(checkboxTexts, findsAtLeastNWidgets(2));
            for (final element in checkboxTexts.evaluate()) {
              final paragraph = element.renderObject! as RenderParagraph;
              expect(paragraph.softWrap, isTrue);
              expect(
                paragraph.didExceedMaxLines,
                isFalse,
                reason:
                    'The whole opt-out explanation must wrap at enlarged text.',
              );
            }
            await dialog.confirm(tester);
            expect(dialog.completed, isTrue);
            expect(dialog.result, isTrue);
            expect(tester.takeException(), isNull);
          },
        );
      }
    }
  }

  testWidgets('Escape closes without committing a checkbox draft', (
    tester,
  ) async {
    final dialog = _Dialog();
    await dialog.mount(tester);
    await dialog.hideCheckbox(tester);
    await tester.sendKeyEvent(LogicalKeyboardKey.escape);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    await tester.pump();
    expect(dialog.completed, isTrue);
    expect(dialog.result, isNull);
  });

  testWidgets('Enter confirms the current draft through the focused button', (
    tester,
  ) async {
    final dialog = _Dialog();
    await dialog.mount(tester, hidden: true);
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    await tester.pump();
    expect(dialog.completed, isTrue);
    expect(dialog.result, isTrue);
  });

  testWidgets(
    'missing persistent scope disables opt-out and returns no saved choice',
    (tester) async {
      final dialog = _Dialog();
      await dialog.mount(
        tester,
        canRemember: false,
        note: '계정 식별을 확인하지 못해 숨김 설정을 저장할 수 없어요.',
      );
      final checkbox = tester.widget<CheckboxListTile>(
        find.byKey(const Key('session-guide-hide')),
      );
      expect(checkbox.onChanged, isNull);
      await tester.ensureVisible(find.byKey(const Key('session-guide-hide')));
      await tester.pump();
      expect(find.text('계정 식별을 확인하지 못해 숨김 설정을 저장할 수 없어요.'), findsOneWidget);
      await dialog.confirm(tester);
      expect(dialog.result, isNull);
    },
  );

  testWidgets(
    'manual guide can uncheck a saved opt-out to show on the next window',
    (tester) async {
      final dialog = _Dialog();
      await dialog.mount(tester, hidden: true);
      await dialog.hideCheckbox(tester);
      await dialog.confirm(tester);
      expect(dialog.result, isFalse);
    },
  );

  testWidgets(
    'the shared route places the guide wholly on the primary monitor',
    (tester) async {
      final dialog = _Dialog();
      await dialog.mount(
        tester,
        size: const Size(1920, 1080),
        scale: 1.5,
        primary: const Rect.fromLTWH(960, 0, 960, 1080),
      );
      final bounds = tester.getRect(find.byType(Dialog));
      expect(bounds.left, greaterThanOrEqualTo(960));
      expect(bounds.right, lessThanOrEqualTo(1920));
      expect(
        find.byKey(const Key('close-session-guide')).hitTestable(),
        findsOneWidget,
      );
      expect(
        find.byKey(const Key('confirm-session-guide')).hitTestable(),
        findsOneWidget,
      );
      expect(tester.takeException(), isNull);
    },
  );
  testWidgets(
    'reduced-motion guide opens and Escape closes without a transition wait',
    (tester) async {
      final dialog = _Dialog();
      await dialog.mount(tester, reducedMotion: true, settleOpening: false);
      final context = tester.element(
        find.byKey(const Key('session-guide-dialog')),
      );
      final opening = ModalRoute.of(context)?.animation;
      expect(opening, isNotNull);
      expect(
        opening!.status,
        AnimationStatus.completed,
        reason: 'The reduced-motion route must not leave a hidden animated popup pending.',
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pump();
      await tester.pump();
      expect(find.byKey(const Key('session-guide-dialog')), findsNothing);
      expect(dialog.completed, isTrue);
      expect(dialog.result, isNull);
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets(
    'late guide preference updates untouched draft but cannot overwrite an edited choice',
    (tester) async {
      final state = ValueNotifier(
        const SessionGuidePresentation(
          canRemember: false,
          preferenceNote: '숨김 설정을 확인하고 있어요.',
        ),
      );
      addTearDown(state.dispose);
      final dialog = _Dialog();
      await dialog.mount(tester, presentation: state);
      final checkbox = find.byKey(const Key('session-guide-hide'));
      expect(tester.widget<CheckboxListTile>(checkbox).onChanged, isNull);
      state.value = const SessionGuidePresentation(
        canRemember: true,
        hidden: true,
      );
      await tester.pump();
      expect(tester.widget<CheckboxListTile>(checkbox).value, isTrue);
      await dialog.hideCheckbox(tester);
      expect(tester.widget<CheckboxListTile>(checkbox).value, isFalse);
      state.value = const SessionGuidePresentation(
        canRemember: true,
        hidden: true,
        preferenceNote: '합성 상태 갱신',
      );
      await tester.pump();
      expect(
        tester.widget<CheckboxListTile>(checkbox).value,
        isFalse,
        reason: 'Late preference/status refresh must not replace a user-edited draft.',
      );
      await dialog.confirm(tester);
      expect(dialog.result, isFalse);
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );

  testWidgets(
    'pending native release disables commit but primary 200-percent guide stays closable',
    (tester) async {
      final state = ValueNotifier(
        const SessionGuidePresentation(
          canRemember: true,
          hidden: true,
          releasePending: true,
        ),
      );
      addTearDown(state.dispose);
      final dialog = _Dialog();
      await dialog.mount(
        tester,
        size: const Size(1920, 1080),
        scale: 2,
        primary: const Rect.fromLTWH(960, 0, 960, 1080),
        presentation: state,
      );
      final bounds = tester.getRect(find.byType(Dialog));
      expect(bounds.left, greaterThanOrEqualTo(960));
      expect(bounds.right, lessThanOrEqualTo(1920));
      expect(
        tester
            .widget<CheckboxListTile>(
              find.byKey(const Key('session-guide-hide')),
            )
            .onChanged,
        isNull,
      );
      // 확인 is the grey button since 2026-11-10 (it only closes the guide).
      final confirm = tester.widget<TextButton>(
        find.byKey(const Key('confirm-session-guide')),
      );
      expect(confirm.onPressed, isNull);
      expect(
        find.byKey(const Key('close-session-guide')).hitTestable(),
        findsOneWidget,
      );
      await tester.sendKeyEvent(LogicalKeyboardKey.escape);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 500));
      await tester.pump();
      expect(dialog.completed, isTrue);
      expect(dialog.result, isNull);
      expect(find.byKey(const Key('session-guide-dialog')), findsNothing);
      expect(tester.takeException(), isNull);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
}

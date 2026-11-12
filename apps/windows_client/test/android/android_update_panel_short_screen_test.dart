// Inspection 2026-11-12: in English on a phone held sideways the app update
// dialog's three buttons stack and left about 40 px for the versions; at
// 200 % text Close was cut and the layout overflowed by 28 px. On a short
// screen the whole dialog scrolls, so the versions, the notes and every
// button can be reached. Fake catalog and platform; no network or device.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_update_panel.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/l10n/l10n.dart';

import 'android_updates_test.dart' show FakePlatform, apk, catalog, origin;

Future<void> _pump(WidgetTester tester, Size size, double textScale) async {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  final transport = catalog([
    apk('0.1.0-preview.5', 'arm64-v8a', changelog: 'Touch control refined'),
  ]);
  await tester.pumpWidget(
    MaterialApp(
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(
          context,
        ).copyWith(textScaler: TextScaler.linear(textScale)),
        child: child!,
      ),
      home: Scaffold(
        body: AndroidUpdatePanel(
          server: origin.host,
          current: '0.1.0-preview.4',
          updates: () => AndroidUpdates(origin, transport: transport),
          platform: FakePlatform(),
        ),
      ),
    ),
  );
  // The catalog read finishes outside the fake clock.
  await tester.pump(const Duration(milliseconds: 300));
  await tester.runAsync(
    () => Future<void>.delayed(const Duration(milliseconds: 100)),
  );
  await tester.pumpAndSettle();
}

/// [target] can be brought wholly onto the screen and touched there.
Future<void> _expectReachable(WidgetTester tester, Finder target) async {
  expect(target, findsOneWidget);
  await tester.ensureVisible(target);
  await tester.pumpAndSettle();
  final screen =
      Offset.zero & tester.view.physicalSize / tester.view.devicePixelRatio;
  final rect = tester.getRect(target);
  expect(rect.top, greaterThanOrEqualTo(screen.top - .01), reason: '$target');
  expect(
    rect.bottom,
    lessThanOrEqualTo(screen.bottom + .01),
    reason: '$target',
  );
  // All of it at once: its top and its bottom are both in view (a 40 px
  // strip of the body showed only a slice of the versions).
  for (final at in const [Alignment(0, -.9), Alignment.center, Alignment(0, .9)]) {
    expect(target.hitTestable(at: at), findsOneWidget, reason: '$target $at');
  }
}

void main() {
  setUp(() => AppLanguageController.debugSystemLanguage = 'en');

  for (final textScale in const [1.0, 2.0]) {
    testWidgets('English, phone sideways (915×412), text ×$textScale: the '
        'versions and all three buttons are reached, nothing overflows', (
      tester,
    ) async {
      await _pump(tester, const Size(915, 412), textScale);
      expect(tester.takeException(), isNull);
      expect(find.text('Change log'), findsOneWidget);
      // The versions are whole, not a 40 px strip between title and buttons.
      final card = find.byKey(const Key('android-update-version-card'));
      await _expectReachable(tester, card);
      await _expectReachable(
        tester,
        find.byKey(const Key('android-update-new-version')),
      );
      expect(find.text('0.1.0-preview.4'), findsOneWidget);
      for (final key in const [
        'android-update-panel-install',
        'android-open-release-history',
        'android-update-panel-close',
      ]) {
        await _expectReachable(tester, find.byKey(Key(key)));
      }
      expect(tester.takeException(), isNull);
    });
  }

  testWidgets('upright (412×915) the title and the buttons stay in place '
      'around the body', (tester) async {
    await _pump(tester, const Size(412, 915), 1);
    // Nothing needs scrolling: all of it is on screen at once.
    for (final key in const [
      'android-update-version-card',
      'android-update-panel-install',
      'android-open-release-history',
      'android-update-panel-close',
    ]) {
      expect(find.byKey(Key(key)).hitTestable(), findsOneWidget, reason: key);
    }
    expect(tester.takeException(), isNull);
  });
}

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

/// Accepts the real user-facing launcher question. Never skips a missing
/// dialog or calls a production connection callback directly.
Future<void> acceptStreamConnect(WidgetTester tester, String desktopId) async {
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
  final dialog = find.byKey(const Key('connect-confirm-dialog'));
  expect(dialog, findsOneWidget);
  final button = find.byKey(Key('confirm-connect-$desktopId'));
  expect(button, findsOneWidget);
  await tester.ensureVisible(button);
  await tester.pump();
  expect(button.hitTestable(), findsOneWidget);
  await tester.tap(button);
  // A held backend request may animate indefinitely; settle only the modal.
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
  await tester.pump();
  expect(dialog, findsNothing);
}

/// The Windows launcher's one question for a desktop whose Windows screen is
/// held by RDP (one question, not two): the takeover question stands in
/// for the plain connect question. The question stays open for the caller.
Future<void> expectWindowsHandoverQuestion(WidgetTester tester) async {
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
  expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
  expect(find.byKey(const Key('rdp-handover-confirm')), findsOneWidget);
  expect(find.byKey(const Key('rdp-handover-accept')), findsOneWidget);
  expect(find.byKey(const Key('rdp-handover-cancel')), findsOneWidget);
}

/// The Android launcher's one question in the same state. The question stays
/// open for the caller.
Future<void> expectAndroidHandOverQuestion(
  WidgetTester tester,
  String desktopId,
) async {
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
  expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
  expect(find.text('Windows 화면을 가져올까요?'), findsOneWidget);
  expect(
    find.byKey(Key('confirm-console-handover-$desktopId')),
    findsOneWidget,
  );
  expect(find.byKey(const Key('cancel-console-handover')), findsOneWidget);
}

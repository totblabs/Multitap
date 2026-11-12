import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/console_resume_dialog.dart';
import 'package:multitap_client/ui/session_window.dart';

import 'console_resume_flow_test.dart' show ConsoleBroker, desktop, pending;
import 'session_windows_test.dart' show Guard;

SessionWindowLaunch launch() => SessionWindowLaunch(
  origin: Uri.parse('https://fixture.example'),
  token: 'test-token',
  expiresAt: DateTime.now().add(const Duration(hours: 1)),
  desktopId: 'qa-vm',
  name: 'TESTER',
  mode: SessionDisplayMode.windowed,
  options: const StreamOptions(),
);

Future<void> agreeToHandover(
  WidgetTester t,
  ConsoleBroker api, {
  int before = 0,
}) async {
  final accept = find.byKey(const Key('rdp-handover-accept'));
  for (var n = 0; n < 20 && accept.evaluate().isEmpty; n++) {
    await t.pump(const Duration(milliseconds: 20));
  }
  expect(accept, findsOneWidget);
  expect(
    api.transfers,
    before,
    reason: 'No Windows handover before explicit consent',
  );
  await t.tap(accept);
  for (var n = 0; n < 20 && api.transfers == before; n++) {
    await t.pump(const Duration(milliseconds: 20));
  }
  expect(
    api.transfers,
    before + 1,
    reason: t
        .widgetList<Text>(find.byWidgetPredicate((w) => w is Text))
        .map((w) => w.data ?? w.textSpan?.toPlainText())
        .join(' | '),
  );
}

void main() {
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    for (final name in [
      'multitap/window',
      'multitap/window/events',
      'multitap/session-windows',
    ]) {
      messenger.setMockMethodCallHandler(
        MethodChannel(name),
        (_) async => false,
      );
    }
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    for (final name in [
      'multitap/window',
      'multitap/window/events',
      'multitap/session-windows',
    ]) {
      messenger.setMockMethodCallHandler(MethodChannel(name), null);
    }
  });
  testWidgets(
    'Native remote-window entry asks before Windows handover and awaits it before display prechecks or connect',
    (t) async {
      final api = ConsoleBroker()..assigned = [desktop()];
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: launch(), api: api, guard: Guard()),
        ),
      );
      await agreeToHandover(t, api);
      expect(api.transfers, 1);
      expect(api.connects, 0);
      expect(find.text('Windows 화면을 넘겨받는 중'), findsOneWidget);
      expect(find.byKey(const Key('console-resume-close')), findsNothing);
      expect(find.byKey(const Key('console-resume-cancel')), findsOneWidget);
      expect(find.widgetWithText(TextButton, '취소'), findsOneWidget);
      api.start.complete(pending);
      await t.pump();
      expect(api.connects, 0);
      await t.pump(const Duration(seconds: 1));
      await t.pumpAndSettle();
      expect(api.transfers, 1);
      expect(api.polls, 1);
      expect(api.connects, 1);
      expect(api.lastConnectDesktopId, 'qa-vm');
      expect(api.lastConnectToken, 'test-token');
      expect(find.byType(ConsoleResumeDialog), findsNothing);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );
  testWidgets(
    'A failed recovery shows its reason with 확인, connects nothing, and retry in the same native window starts it once more',
    (t) async {
      final api = ConsoleBroker()..assigned = [desktop()];
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: launch(), api: api, guard: Guard()),
        ),
      );
      await agreeToHandover(t, api);
      expect(api.transfers, 1);
      expect(find.byKey(const Key('console-resume-cancel')), findsOneWidget);
      // Do not cancel this request: its eventual failure and explicit retry
      // are the behavior under test.
      await t.pump(const Duration(milliseconds: 300));
      expect(find.text('Windows 화면을 넘겨받는 중'), findsOneWidget);
      api.start.completeError(
        const BrokerException('console_transfer_failed', 409),
      );
      await t.pump();
      await t.pumpAndSettle();
      expect(find.text('Windows 화면을 넘겨받지 못했어요'), findsOneWidget);
      expect(find.byKey(const Key('console-resume-cancel')), findsNothing);
      expect(
        find.text('Windows 세션을 스트리밍 화면으로 복귀하지 못했어요. Host의 세션 상태를 확인하세요.'),
        findsOneWidget,
      );
      expect(api.connects, 0);
      expect(api.polls, 0);
      await t.tap(find.byKey(const Key('console-resume-close')));
      await t.pumpAndSettle();
      expect(find.byType(ConsoleResumeDialog), findsNothing);
      expect(api.transfers, 1);
      expect(api.connects, 0);
      // The Host reports the RDP-used session as recoverable again; only the
      // user's retry starts one new transfer.
      api.assigned = [desktop()];
      await t.tap(find.byKey(const Key('retry-remote-connection')));
      await agreeToHandover(t, api, before: 1);
      expect(api.transfers, 2);
      expect(api.connects, 0);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );
  testWidgets(
    'An already-open failed remote window recovers an RDP-disconnected login on retry',
    (t) async {
      final api = ConsoleBroker()..assigned = [desktop(ready: true)];
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: launch(), api: api, guard: Guard()),
        ),
      );
      await t.pumpAndSettle();
      expect(api.connects, 1);
      api.assigned = [desktop()];
      await t.tap(find.byKey(const Key('retry-remote-connection')));
      await agreeToHandover(t, api);
      expect(find.text('Windows 화면을 넘겨받는 중'), findsOneWidget);
      expect(api.transfers, 1);
      expect(api.connects, 1);
      api.start.complete(pending);
      await t.pump();
      await t.pump(const Duration(seconds: 1));
      await t.pumpAndSettle();
      expect(api.polls, 1);
      expect(api.connects, 2);
      expect(find.byType(ConsoleResumeDialog), findsNothing);
      await t.pumpWidget(const SizedBox());
      await t.pump();
    },
  );
  testWidgets(
    'Closed native window rejects a late recovery result and never opens a stream',
    (t) async {
      final api = ConsoleBroker()..assigned = [desktop()];
      await t.pumpWidget(
        MaterialApp(
          home: SessionWindow(launch: launch(), api: api, guard: Guard()),
        ),
      );
      await agreeToHandover(t, api);
      expect(api.transfers, 1);
      await t.pumpWidget(const SizedBox());
      api.start.complete(pending);
      await t.pump(const Duration(seconds: 2));
      expect(api.polls, 0);
      expect(api.connects, 0);
      expect(t.takeException(), isNull);
    },
  );
}

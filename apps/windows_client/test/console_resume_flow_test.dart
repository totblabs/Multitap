import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/console_preparation.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/console_resume_dialog.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'auth_flow_test.dart' show TestBroker;
import 'power_api_test.dart' show taskId;

import 'helpers/stream_connect_consent.dart';

const pending = ConsolePreparationState(
  supported: true,
  busy: true,
  reason: 'rdp_session_disconnected',
  operation: ConsolePreparationOperation(id: taskId, status: 'pending'),
);
const complete = ConsolePreparationState(
  supported: true,
  reason: 'console_ready',
  operation: ConsolePreparationOperation(
    id: taskId,
    status: 'succeeded',
    attempted: true,
  ),
);
BrokerDesktop desktop({
  bool ready = false,
  ConsolePreparationState? preparation,
}) => BrokerDesktop(
  id: 'qa-vm',
  name: 'TESTER',
  kind: 'vm',
  description: '',
  os: 'Windows',
  status: 'ready',
  canConnect: true,
  canStream: ready,
  hostConnected: true,
  interactiveSession: ready,
  virtualDisplayReady: ready,
  virtualMonitorControl: ready,
  display: VirtualDisplayState(
    supported: ready,
    activeCount: ready ? 1 : 0,
    stale: false,
  ),
  monitors: ready
      ? const [RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true)]
      : const [],
  power: const BrokerPowerSnapshot(state: 'running', stale: false),
  consolePreparation:
      preparation ??
      const ConsolePreparationState(
        supported: true,
        available: true,
        reason: 'rdp_session_disconnected',
      ),
);

class ConsoleBroker extends TestBroker implements ConsoleRecoveryApi {
  int transfers = 0, polls = 0;
  final start = Completer<ConsolePreparationState>();
  @override
  Future<ConsolePreparationState> prepareConsole(
    String token,
    String desktopId,
  ) {
    expect(token, 'test-token');
    expect(desktopId, 'qa-vm');
    transfers++;
    assigned = [desktop(preparation: pending)];
    return start.future;
  }

  @override
  Future<ConsolePreparationState> consolePreparation(
    String token,
    String desktopId, {
    String? operationId,
  }) async {
    expect(operationId, taskId);
    polls++;
    assigned = [desktop(ready: true, preparation: complete)];
    return complete;
  }
}

Future<void> login(WidgetTester tester, ConsoleBroker api) async {
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  for (final name in ['multitap/window', 'multitap/window/events']) {
    final channel = MethodChannel(name);
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => call.method == 'fullscreen' ? call.arguments : null,
    );
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
  }
  tester.view.physicalSize = const Size(1440, 1000);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(
    MultitapApp(api: api, serverOrigin: Uri.parse('https://cloud.example.com')),
  );
  await tester.pumpAndSettle();
  await tester.enterText(find.byKey(const Key('sign-in-id')), 'qa-user');
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'qa-password',
  );
  await tester.tap(find.byKey(const Key('login-submit')));
  await tester.pumpAndSettle();
}

/// The takeover starts by itself once its progress dialog is up; pumps frames
/// (never settling: the progress spinner keeps animating) until it has.
Future<void> untilTransfer(WidgetTester tester, ConsoleBroker api) async {
  for (var i = 0; i < 20 && api.transfers == 0; i++) {
    await tester.pump(const Duration(milliseconds: 50));
  }
  await tester.pump();
}

void main() {
  testWidgets(
    'First-install active RDP session is resumed only by confirmed main streaming connect, without another Host action or Windows credential prompt',
    (tester) async {
      final api = ConsoleBroker()
        ..assigned = [
          desktop(
            preparation: const ConsolePreparationState(
              supported: true,
              available: true,
              reason: 'rdp_session_active',
            ),
          ),
        ];
      await login(tester, api);
      expect(api.transfers, 0, reason: 'Reading the list never moves Windows.');
      expect(api.connects, 0);
      expect(find.text('Windows 화면을 넘겨받아 연결'), findsWidgets);
      await tester.tap(find.byKey(const Key('open-qa-vm')));
      await tester.pumpAndSettle();
      // One question: the takeover question is the connect question here.
      await expectWindowsHandoverQuestion(tester);
      expect(api.transfers, 0, reason: 'Connection still needs confirmation.');
      expect(api.connects, 0);
      await tester.tap(find.byKey(const Key('rdp-handover-accept')));
      await untilTransfer(tester, api);
      expect(api.transfers, 1);
      expect(api.connects, 0);
      expect(find.byType(ConsoleResumeDialog), findsOneWidget);
      expect(find.byKey(const Key('console-resume-cancel')), findsOneWidget);
      expect(
        find.descendant(
          of: find.byType(ConsoleResumeDialog),
          matching: find.widgetWithText(TextButton, '취소'),
        ),
        findsOneWidget,
      );
      expect(find.byType(TextField), findsNothing);
      expect(find.byKey(const Key('host-console-transfer')), findsNothing);
      api.start.complete(pending);
      await tester.pump();
      expect(api.connects, 0);
      await tester.pump(const Duration(seconds: 1));
      await tester.pumpAndSettle();
      for (var n = 0; n < 10 && api.connects == 0; n++) {
        await tester.pump(const Duration(milliseconds: 20));
      }
      expect(api.transfers, 1);
      expect(api.polls, 1);
      expect(
        api.connects,
        1,
        reason: tester
            .widgetList<Text>(find.byWidgetPredicate((w) => w is Text))
            .map((w) => w.data ?? w.textSpan?.toPlainText())
            .join(' | '),
      );
      expect(api.lastConnectDesktopId, 'qa-vm');
      expect(find.byType(ConsoleResumeDialog), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    },
  );

  testWidgets(
    'Real workspace route confirms before takeover and waits for console plus actual display caps before connect',
    (tester) async {
      final api = ConsoleBroker()..assigned = [desktop()];
      await login(tester, api);
      expect(
        tester
            .widget<WorkspaceShell>(find.byType(WorkspaceShell))
            .machines
            .single
            .isReady,
        isFalse,
      );
      await tester.tap(find.byKey(const Key('stream-modes-qa-vm')));
      await tester.pumpAndSettle();
      expect(api.transfers, 0);
      await tester.tap(find.byKey(const Key('stream-mode-fullscreen')));
      await tester.pumpAndSettle();
      await expectWindowsHandoverQuestion(tester);
      expect(api.transfers, 0);
      expect(api.connects, 0);
      await tester.tap(find.byKey(const Key('rdp-handover-accept')));
      await untilTransfer(tester, api);
      expect(api.transfers, 1);
      expect(api.connects, 0);
      expect(find.text('Windows 화면을 넘겨받는 중'), findsOneWidget);
      expect(find.byKey(const Key('console-resume-close')), findsNothing);
      expect(find.byKey(const Key('console-resume-cancel')), findsOneWidget);
      expect(
        find.descendant(
          of: find.byType(ConsoleResumeDialog),
          matching: find.widgetWithText(TextButton, '취소'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: find.byType(ConsoleResumeDialog),
          matching: find.byWidgetPredicate((w) => w is ButtonStyleButton),
        ),
        findsOneWidget,
      );
      expect(
        tester
            .widget<WorkspaceShell>(find.byType(WorkspaceShell))
            .machines
            .single
            .display
            .busy,
        isTrue,
      );
      api.start.complete(pending);
      await tester.pump();
      expect(api.connects, 0);
      await tester.pump(const Duration(seconds: 1));
      await tester.pumpAndSettle();
      for (var n = 0; n < 10 && api.connects == 0; n++) {
        await tester.pump(const Duration(milliseconds: 20));
      }
      expect(api.transfers, 1);
      expect(api.polls, 1);
      expect(api.connects, 1);
      expect(api.lastConnectDesktopId, 'qa-vm');
      expect(find.byType(ConsoleResumeDialog), findsNothing);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    },
  );
  testWidgets(
    'The workspace takeover offers cancel while busy; an uncancelled failure shows 확인 and starts no connection',
    (tester) async {
      final api = ConsoleBroker()..assigned = [desktop()];
      await login(tester, api);
      await tester.tap(find.byKey(const Key('stream-modes-qa-vm')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('stream-mode-windowed')));
      await tester.pumpAndSettle();
      await expectWindowsHandoverQuestion(tester);
      expect(api.transfers, 0);
      await tester.tap(find.byKey(const Key('rdp-handover-accept')));
      await untilTransfer(tester, api);
      // Complete the preceding confirmation's reverse transition before
      // checking which controls belong to the running transfer dialog.
      await tester.pump(const Duration(milliseconds: 300));
      // The agreed question is gone and no second one took its place.
      expect(find.byKey(const Key('rdp-handover-confirm')), findsNothing);
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(api.transfers, 1);
      expect(find.text('Windows 화면을 넘겨받는 중'), findsOneWidget);
      expect(find.byKey(const Key('console-resume-cancel')), findsOneWidget);
      expect(
        find.descendant(
          of: find.byType(ConsoleResumeDialog),
          matching: find.widgetWithText(TextButton, '취소'),
        ),
        findsOneWidget,
      );
      // Keep the request active to inspect its failure; an outside click
      // does not cancel it. Escape/back cancellation is tested separately.
      await tester.tapAt(const Offset(4, 4));
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byType(ConsoleResumeDialog), findsOneWidget);
      expect(find.text('Windows 화면을 넘겨받는 중'), findsOneWidget);
      api.start.completeError(
        const BrokerException('console_transfer_failed', 409),
      );
      await tester.pump();
      await tester.pumpAndSettle();
      expect(find.text('Windows 화면을 넘겨받지 못했어요'), findsOneWidget);
      expect(find.byKey(const Key('console-resume-cancel')), findsNothing);
      expect(
        find.text('Windows 세션을 스트리밍 화면으로 복귀하지 못했어요. Host의 세션 상태를 확인하세요.'),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: find.byKey(const Key('console-resume-close')),
          matching: find.text('확인'),
        ),
        findsOneWidget,
      );
      expect(api.transfers, 1);
      expect(api.polls, 0);
      expect(api.connects, 0);
      await tester.tap(find.byKey(const Key('console-resume-close')));
      await tester.pumpAndSettle();
      expect(find.byType(ConsoleResumeDialog), findsNothing);
      expect(find.byType(RemoteView), findsNothing);
      expect(
        api.transfers,
        1,
        reason: 'Closing a failure never retries the transfer.',
      );
      expect(api.polls, 0);
      expect(api.connects, 0);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    },
  );
}

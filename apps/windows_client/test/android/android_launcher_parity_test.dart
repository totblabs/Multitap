// The Android launcher on the Windows client's tile design, and the console
// hand-over after RDP (2026-10-27: an RDP session made the tile say "다시
// 시작" instead of offering 스트리밍 연결 through the hand-over).
import '../helpers/stream_connect_consent.dart'
    show acceptStreamConnect, expectAndroidHandOverQuestion;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_mapping.dart';
import 'package:multitap_client/models/console_preparation.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/desktop_readiness.dart';

import 'android_launcher_test.dart'
    show FakeBroker, MemoryStore, vm, mainButton;

const _op = '12345678-1234-1234-1234-123456789012';

const _pending = ConsolePreparationState(
  supported: true,
  busy: true,
  reason: 'rdp_session_disconnected',
  operation: ConsolePreparationOperation(id: _op, status: 'pending'),
);
const _done = ConsolePreparationState(
  supported: true,
  reason: 'console_ready',
  operation: ConsolePreparationOperation(
    id: _op,
    status: 'succeeded',
    attempted: true,
  ),
);

/// The VM after an RDP session: running, Host answering, the Windows session
/// still in RDP, so streaming is not ready but the hand-over is offered.
BrokerDesktop afterRdp({
  bool ready = false,
  ConsolePreparationState? prep,
  bool cleanup = false,
}) => BrokerDesktop(
  id: 'pve-149',
  name: 'TESTER',
  kind: 'vm',
  description: '',
  os: 'Windows 11 · 4 vCPU',
  status: 'online',
  canConnect: true,
  canStream: ready,
  hostConnected: true,
  interactiveSession: ready,
  virtualDisplayReady: ready,
  virtualMonitorControl: ready,
  streamPreparation: ready ? null : StreamPreparation.resume,
  sessionCleanup: cleanup
      ? SessionCleanupState(
          pending: true,
          phase: 'pending',
          checkedAt: DateTime.now().toUtc(),
        )
      : const SessionCleanupState(),
  display: VirtualDisplayState(
    supported: ready,
    activeCount: ready ? 1 : 0,
    stale: false,
  ),
  monitors: ready
      ? const [RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true)]
      : const [],
  power: BrokerPowerSnapshot(
    state: 'running',
    stale: false,
    checkedAt: DateTime.utc(2026, 10, 27),
  ),
  powerCanForceStop: true,
  consolePreparation:
      prep ??
      const ConsolePreparationState(
        supported: true,
        available: true,
        reason: 'rdp_session_disconnected',
      ),
);

class ConsoleFake extends FakeBroker implements ConsoleRecoveryApi {
  ConsoleFake(super.inventory);
  int transfers = 0, polls = 0;

  @override
  Future<ConsolePreparationState> prepareConsole(
    String token,
    String desktopId,
  ) async {
    expect(desktopId, 'pve-149');
    transfers++;
    inventory = [afterRdp(prep: _pending)];
    return _pending;
  }

  @override
  Future<ConsolePreparationState> consolePreparation(
    String token,
    String desktopId, {
    String? operationId,
  }) async {
    expect(operationId, _op);
    polls++;
    inventory = [afterRdp(ready: true, prep: _done)];
    return _done;
  }
}

Future<void> pumpApp(
  WidgetTester tester,
  FakeBroker broker, {
  Size dp = const Size(360, 800),
}) async {
  tester.view.physicalSize = dp * 2;
  tester.view.devicePixelRatio = 2;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  MemoryStore(
    origin: 'https://vdi.example',
    username: 'tester',
    prefs: {'launcher.view': 'cards'},
  ).install();
  await tester.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
  await tester.pumpAndSettle();
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'secret-pass',
  );
  await _tapVisible(tester, find.byKey(const Key('login-submit')));
  await tester.pumpAndSettle();
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('after RDP the readiness is the hand-over, not a reboot', () {
    final machine = machineFromBroker(afterRdp());
    expect(machine.isReady, isFalse);
    expect(machine.canRecoverConsole, isTrue);
    // What the Android app did before: no hand-over, so a restart.
    final before = DesktopReadiness.of(machine);
    expect(before.canStream, isFalse);
    // Not a reboot (2026-10-27): a restart is offered only when Windows
    // really asks for one.
    expect(before.primary, ReadinessAction.refresh);
    final now = DesktopReadiness.of(machine, consoleRecovery: true);
    expect(now.console, isTrue);
    expect(now.canStream, isTrue);
    expect(now.headline, 'Windows 화면을 넘겨받아 연결');
  });

  testWidgets('a VM left in RDP offers 스트리밍 연결 and takes the session over', (
    tester,
  ) async {
    final broker = ConsoleFake([afterRdp()]);
    await pumpApp(tester, broker);
    expect(find.text('Windows 화면을 넘겨받아 연결'), findsOneWidget);
    expect(
      find.descendant(
        of: mainButton('pve-149'),
        matching: find.text('스트리밍 연결'),
      ),
      findsOneWidget,
    );
    expect(find.text('다시 시작'), findsNothing);
    await _tapVisible(tester, mainButton('pve-149'));
    await tester.pumpAndSettle();
    // One question (2026-11-11): the hand-over question is the launcher's.
    await expectAndroidHandOverQuestion(tester, 'pve-149');
    expect(find.byType(AppConfirmDialog), findsOneWidget);
    expect(
      find.textContaining('RDP에서 쓰던 Windows 화면으로 이어서 연결해요.'),
      findsOneWidget,
    );
    expect(broker.transfers, 0);
    expect(broker.calls.contains('connect'), isFalse);
    await _tapVisible(
      tester,
      find.byKey(const Key('confirm-console-handover-pve-149')),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(find.text('Windows 화면을 넘겨받는 중'), findsOneWidget);
    expect(broker.transfers, 1);
    // The agreed question closes and no second one takes its place.
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.byType(AppConfirmDialog), findsNothing);
    // The transfer is watched read-only until the display is really ready.
    for (var i = 0; i < 6; i++) {
      await tester.pump(const Duration(seconds: 1));
    }
    await tester.pumpAndSettle();
    expect(broker.transfers, 1);
    expect(broker.polls, 1);
    expect(broker.calls.contains('power:reboot'), isFalse);
    expect(find.byType(AndroidRemoteView), findsOneWidget);
    expect(find.text('Windows 화면을 넘겨받는 중'), findsNothing);
    await tester.pumpWidget(const SizedBox());
    await tester.pump(const Duration(seconds: 30));
  });

  for (final entry in ['ordinary', 'quality', 'queued']) {
    testWidgets(
      '$entry RDP connect keeps explicit consent and cancel never transfers',
      (tester) async {
        final broker = ConsoleFake([afterRdp(cleanup: entry == 'queued')]);
        await pumpApp(tester, broker);
        if (entry == 'quality') {
          await _tapVisible(
            tester,
            find.byKey(const Key('stream-modes-pve-149')),
          );
          await tester.pumpAndSettle();
          await _tapVisible(
            tester,
            find.byKey(const Key('stream-settings-pve-149')),
          );
          await tester.pumpAndSettle();
          expect(find.byType(AndroidStreamDialog), findsOneWidget);
          await _tapVisible(
            tester,
            find.descendant(
              of: find.byType(AndroidStreamDialog),
              matching: find.widgetWithText(FilledButton, '연결'),
            ),
          );
        } else {
          await _tapVisible(tester, mainButton('pve-149'));
        }
        if (entry == 'queued') {
          // Nothing is brought over while the previous connection is cleaned
          // up: the queue gets the plain question, its start the hand-over.
          await acceptStreamConnect(tester, 'pve-149');
          // Cleanup is intentionally pending. Settle would advance the fake
          // clock through the running progress animation and its deadline.
          await tester.pump(const Duration(milliseconds: 300));
          expect(find.byKey(const Key('cancel-start-pve-149')), findsOneWidget);
          expect(
            find.byKey(const Key('confirm-console-handover-pve-149')),
            findsNothing,
          );
          expect(broker.transfers, 0);
          broker.inventory = [afterRdp()];
          final refresh = find.byKey(const Key('refresh-desktops'));
          await tester.ensureVisible(refresh);
          await tester.pump();
          await tester.tap(refresh);
          await tester.pump();
          // The connecting flow still animates behind its consent modal.
          await tester.pump(const Duration(milliseconds: 350));
        } else {
          await tester.pumpAndSettle();
        }
        expect(
          find.byKey(const Key('confirm-console-handover-pve-149')),
          findsOneWidget,
        );
        expect(find.text('Windows 화면을 가져올까요?'), findsOneWidget);
        expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
        // The one question, whichever entry led here.
        expect(find.byType(AppConfirmDialog), findsOneWidget);
        expect(broker.transfers, 0);
        expect(broker.calls.contains('connect'), isFalse);
        final cancel = find.byKey(const Key('cancel-console-handover'));
        await tester.ensureVisible(cancel);
        await tester.pump();
        expect(cancel.hitTestable(), findsOneWidget);
        await tester.tap(cancel);
        await tester.pumpAndSettle();
        expect(find.byType(AppConfirmDialog), findsNothing);
        expect(broker.transfers, 0);
        expect(
          broker.calls.where((c) => c == 'connect' || c.startsWith('power:')),
          isEmpty,
        );
        expect(find.byType(AndroidRemoteView), findsNothing);
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump(const Duration(seconds: 30));
      },
    );
  }

  testWidgets(
    'removing the assignment while handover consent is open prevents any transfer',
    (tester) async {
      final broker = ConsoleFake([afterRdp()]);
      await pumpApp(tester, broker);
      await _tapVisible(tester, mainButton('pve-149'));
      await tester.pumpAndSettle();
      await expectAndroidHandOverQuestion(tester, 'pve-149');
      broker.inventory = [];
      await _tapVisible(
        tester,
        find.byKey(const Key('confirm-console-handover-pve-149')),
      );
      await tester.pumpAndSettle();
      expect(find.byType(AppConfirmDialog), findsNothing);
      expect(broker.transfers, 0);
      expect(broker.calls.contains('connect'), isFalse);
      expect(find.byType(AndroidRemoteView), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump(const Duration(seconds: 30));
    },
  );

  // One question (2026-11-11: connecting asked "연결할까요?" and then the
  // hand-over question).
  testWidgets(
    'a ready desktop keeps the plain connect question and no hand-over question',
    (tester) async {
      final broker = ConsoleFake([afterRdp(ready: true, prep: _done)]);
      await pumpApp(tester, broker);
      await _tapVisible(tester, mainButton('pve-149'));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('connect-confirm-dialog')), findsOneWidget);
      expect(find.text('Windows 화면을 가져올까요?'), findsNothing);
      expect(find.byType(AppConfirmDialog), findsOneWidget);
      expect(find.byType(AndroidRemoteView), findsNothing);
      await acceptStreamConnect(tester, 'pve-149');
      await tester.pumpAndSettle();
      expect(find.byType(AppConfirmDialog), findsNothing);
      expect(find.byType(AndroidRemoteView), findsOneWidget);
      expect(broker.transfers, 0);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump(const Duration(seconds: 30));
    },
  );

  testWidgets(
    'a queued connection asks the hand-over question once when it starts, then connects',
    (tester) async {
      final broker = ConsoleFake([afterRdp(cleanup: true)]);
      await pumpApp(tester, broker);
      await _tapVisible(tester, mainButton('pve-149'));
      await acceptStreamConnect(tester, 'pve-149');
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const Key('cancel-start-pve-149')), findsOneWidget);
      expect(find.byType(AppConfirmDialog), findsNothing);
      expect(broker.transfers, 0);
      broker.inventory = [afterRdp()];
      final refresh = find.byKey(const Key('refresh-desktops'));
      await tester.ensureVisible(refresh);
      await tester.pump();
      await tester.tap(refresh);
      await tester.pump();
      await expectAndroidHandOverQuestion(tester, 'pve-149');
      expect(find.byType(AppConfirmDialog), findsOneWidget);
      expect(broker.transfers, 0);
      await tester.tap(
        find.byKey(const Key('confirm-console-handover-pve-149')),
      );
      for (var i = 0; i < 20 && broker.transfers == 0; i++) {
        await tester.pump(const Duration(milliseconds: 50));
      }
      expect(broker.transfers, 1);
      await tester.pump(const Duration(milliseconds: 300));
      // The queue's question was the only hand-over question.
      expect(find.byType(AppConfirmDialog), findsNothing);
      for (var i = 0; i < 6; i++) {
        await tester.pump(const Duration(seconds: 1));
      }
      for (
        var i = 0;
        i < 20 && find.byType(AndroidRemoteView).evaluate().isEmpty;
        i++
      ) {
        await tester.pump(const Duration(milliseconds: 100));
      }
      expect(broker.transfers, 1);
      expect(broker.polls, 1);
      expect(find.byType(AndroidRemoteView), findsOneWidget);
      expect(find.byType(AppConfirmDialog), findsNothing);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump(const Duration(seconds: 30));
    },
  );

  testWidgets(
    'without hand-over the card preserves the blocked cause and offers checks',
    (tester) async {
      await pumpApp(tester, FakeBroker([afterRdp()]));
      expect(find.text('Windows 화면을 넘겨받아 연결'), findsNothing);
      // A restart is not the fix for a session left in RDP: the launcher keeps
      // the cause and the checklist.
      expect(mainButton('pve-149'), findsOneWidget);
      expect(find.text('다시 시작'), findsNothing);
      expect(find.text('다시 시작…'), findsNothing);
      await _tapVisible(tester, find.byKey(const Key('stream-state-pve-149')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('readiness-verdict')), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );

  testWidgets('every state has one main button of the same size and place', (
    tester,
  ) async {
    final resume = afterRdp();
    final broker = FakeBroker([
      vm(id: 'a', name: 'ONE'),
      vm(id: 'b', name: 'TWO', power: 'stopped', stream: false),
      vm(id: 'c', name: 'THREE', stream: false),
      BrokerDesktop(
        id: 'd',
        name: 'FOUR',
        kind: 'vm',
        description: '',
        os: 'Windows 11',
        status: 'online',
        canConnect: true,
        canStream: false,
        hostConnected: true,
        streamPreparation: StreamPreparation.restart,
        power: resume.power,
      ),
    ]);
    await pumpApp(tester, broker, dp: const Size(1280, 800));
    void expectLabel(String id, String text) => expect(
      find.descendant(of: mainButton(id), matching: find.text(text)),
      findsOneWidget,
      reason: id,
    );
    expectLabel('a', '스트리밍 연결');
    expectLabel('b', '켜기');
    expectLabel('c', '준비 중…');
    // A restart-required state is the one place the footer says it.
    expectLabel('d', '다시 시작…');
    final cards = {
      for (final id in ['a', 'b', 'c', 'd'])
        tester.getSize(find.byKey(Key('card-$id'))),
    };
    // The Windows tile's footer: one height and place in every state (the
    // main button is narrower where ▾ rides beside it).
    final buttons = {
      for (final id in ['a', 'b', 'c', 'd'])
        (
          tester.getRect(mainButton(id)).top -
              tester.getRect(find.byKey(Key('card-$id'))).top,
          tester.getRect(mainButton(id)).left -
              tester.getRect(find.byKey(Key('card-$id'))).left,
          tester.getRect(mainButton(id)).height,
        ),
    };
    expect(cards.length, 1);
    expect(buttons.length, 1);
    for (final id in ['a', 'b', 'c', 'd']) {
      expect(mainButton(id), findsOneWidget);
    }
    expect(find.text('다시 시작'), findsNothing);
  });

  Future<void> expectTiles(WidgetTester tester, Size dp, int columns) async {
    final broker = FakeBroker([
      vm(id: 'a', name: 'ONE'),
      vm(id: 'b', name: 'TWO', power: 'stopped', stream: false),
      vm(id: 'c', name: 'THREE'),
    ]);
    await pumpApp(tester, broker, dp: dp);
    expect(tester.takeException(), isNull);
    // The Windows header and page rhythm.
    expect(find.byKey(const Key('workspace-header')), findsOneWidget);
    expect(find.byKey(const Key('android-update-check')), findsOneWidget);
    expect(find.byKey(const Key('account-menu')), findsOneWidget);
    expect(find.byKey(const Key('desktop-count')), findsOneWidget);
    // Every state retains its actions and power identity. Healthy cards use
    // resources; the powered-off card keeps its actual status instead.
    for (final id in ['a', 'b', 'c']) {
      expect(find.byKey(Key('desktop-power-$id')), findsOneWidget);
      expect(find.byKey(Key('desktop-more-$id')), findsOneWidget);
      expect(find.byKey(Key('power-state-$id')), findsOneWidget);
    }
    expect(find.byKey(const Key('resources-a')), findsOneWidget);
    expect(find.byKey(const Key('resources-c')), findsOneWidget);
    expect(find.byKey(const Key('status-b')), findsOneWidget);
    final tops = {
      for (final id in ['a', 'b', 'c'])
        id: tester.getTopLeft(find.byKey(Key('card-$id'))).dy,
    };
    final inFirstRow = tops.values.where((y) => y == tops['a']).length;
    expect(inFirstRow, columns, reason: '$dp');
    final sizes = {
      for (final id in ['a', 'b', 'c'])
        tester.getSize(find.byKey(Key('card-$id'))),
    };
    expect(sizes.length, 1, reason: 'one card geometry in every state');
    // Touch targets.
    expect(
      tester.getSize(find.byKey(const Key('desktop-power-a'))).shortestSide,
      greaterThanOrEqualTo(47.99),
    );
    expect(tester.getSize(mainButton('a')).height, greaterThanOrEqualTo(48));
  }

  testWidgets('phone uses one readable column without a size preference', (
    tester,
  ) async {
    await expectTiles(tester, const Size(360, 800), 1);
  });
  testWidgets('foldable width fits the fixed cards into two columns', (
    tester,
  ) async {
    await expectTiles(tester, const Size(932, 704), 2);
  });
  testWidgets('tablet width fits the same fixed cards into three columns', (
    tester,
  ) async {
    await expectTiles(tester, const Size(1280, 800), 3);
  });

  testWidgets('the account menu holds appearance, 설정 and sign-out', (
    tester,
  ) async {
    await pumpApp(tester, FakeBroker([vm()]));
    await _tapVisible(tester, find.byKey(const Key('account-menu')));
    await tester.pumpAndSettle();
    expect(
      find.textContaining('vdi.example · 0.1.0-preview.4'),
      findsOneWidget,
    );
    expect(find.byKey(const Key('theme-dark')), findsOneWidget);
    expect(find.byKey(const Key('scene-peaks')), findsOneWidget);
    expect(find.text('비밀번호 변경'), findsNothing);
    expect(find.byKey(const Key('sign-out')), findsOneWidget);
    expect(find.text('정보·라이선스'), findsOneWidget);
    // 비밀번호 변경 is in 설정, the header's own ⚙ (2026-11-11), as on the PC.
    await tester.sendKeyEvent(LogicalKeyboardKey.escape);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('sign-out')), findsNothing);
    await _tapVisible(tester, find.byKey(const Key('open-app-settings')));
    await tester.pumpAndSettle();
    expect(find.text('비밀번호 변경'), findsOneWidget);
    expect(find.byKey(const Key('account-language')), findsOneWidget);
  });
}

Future<void> _tapVisible(WidgetTester tester, Finder target) async {
  await tester.ensureVisible(target);
  await tester.pumpAndSettle();
  await tester.tap(target);
}

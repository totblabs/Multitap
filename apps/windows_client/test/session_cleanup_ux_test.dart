// Card and dialog policy for the broker's stream cleanup barrier. These are
// widget/model checks only: no broker, Host or real session is contacted.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/virtual_display.dart';
import 'package:multitap_client/models/vm_resources.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/workspace_shell.dart';
import 'helpers/stream_connect_consent.dart';

SessionCleanupState _cleanup(
  String phase, {
  bool quiet = false,
  Duration age = Duration.zero,
}) => SessionCleanupState(
  pending: true,
  phase: phase,
  workerQuiescent: quiet,
  checkedAt: DateTime.now().toUtc().subtract(age),
);

DesktopMachine _vm(SessionCleanupState cleanup, {String id = 'pve-149'}) =>
    DesktopMachine(
      id: id,
      name: 'TESTER',
      description: '',
      kind: MachineKind.vm,
      status: MachineStatus.ready,
      specification: 'Windows 11',
      accent: Colors.blue,
      hostOnline: true,
      interactiveSession: true,
      virtualDisplayReady: true,
      powerState: 'running',
      powerStale: false,
      resources: const VmResources(vcpus: 4, memoryTotalBytes: 8589934592),
      virtualMonitorControl: true,
      display: const VirtualDisplayState(
        supported: true,
        activeCount: 1,
        stale: false,
      ),
      monitors: const [
        RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
      ],
      sessionCleanup: cleanup,
    );

class _Calls {
  int opens = 0, refreshes = 0;

  // A list refresh keeps the same authenticated parent's callbacks, as the
  // real App does. Replacing callbacks models a different connection owner.
  Future<RemoteSession> connect(
    String id, {
    StreamOptions options = const StreamOptions(),
  }) async => throw StateError('fixture only');

  Future<SessionWindowOpenResult?> open(
    DesktopMachine machine,
    SessionDisplayMode mode,
    StreamOptions options,
  ) async {
    opens++;
    return null;
  }
}

Future<_Calls> _card(WidgetTester t, DesktopMachine machine) async {
  final calls = _Calls();
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = const Size(1200, 900);
  addTearDown(t.view.resetPhysicalSize);
  addTearDown(t.view.resetDevicePixelRatio);
  await _shell(t, machine, calls);
  await t.pumpAndSettle();
  return calls;
}

Future<void> _shell(WidgetTester t, DesktopMachine machine, _Calls calls) =>
    t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: WorkspaceShell(
          onSignOut: () {},
          onRefresh: () => calls.refreshes++,
          machines: [machine],
          displayName: 'tester',
          serverOrigin: 'https://cloud.example.com',
          localMonitors: 1,
          onConnect: calls.connect,
          onOpenWindow: calls.open,
        ),
      ),
    );

String _text(WidgetTester t, Key key) {
  final widget = t.widget(find.byKey(key));
  return widget is Text
      ? widget.data ?? widget.textSpan!.toPlainText()
      : throw StateError('$key is not text');
}

void main() {
  group('stalled cleanup classification', () {
    final now = DateTime.utc(2026, 10, 25, 5);
    SessionCleanupState at(String phase, Duration age) => SessionCleanupState(
      pending: true,
      phase: phase,
      checkedAt: now.subtract(age),
    );

    test('normal phases wait for the broker deadline', () {
      for (final phase in ['pending', 'awaiting_capabilities']) {
        expect(at(phase, const Duration(seconds: 44)).stalledAt(now), false);
        expect(at(phase, const Duration(seconds: 45)).stalledAt(now), true);
        expect(at(phase, const Duration(minutes: 30)).stalledAt(now), true);
      }
    });

    test('Host-reported or broker-timed-out phases are stalled at once', () {
      for (final phase in ['recovery_required', 'unknown']) {
        expect(at(phase, Duration.zero).stalledAt(now), true);
      }
    });

    test('a broker clock ahead of this PC never shortens the wait', () {
      expect(at('pending', const Duration(minutes: -10)).stalledAt(now), false);
      expect(
        const SessionCleanupState(pending: true).stalledAt(now),
        false,
        reason: 'No timestamp is no evidence of a long wait.',
      );
      expect(const SessionCleanupState().stalledAt(now), false);
    });
  });

  test('stalled wording is actionable and never enables streaming', () {
    final quiet = _vm(_cleanup('unknown', quiet: true));
    final blocked = _vm(_cleanup('recovery_required'));
    final normal = _vm(_cleanup('pending'));
    expect(normal.streamReadinessBadge, '화면 복원 중');
    expect(normal.cleanupStalled, false);
    for (final value in [quiet, blocked]) {
      expect(value.cleanupStalled, true);
      expect(value.streamReadinessBadge, '복원 미확인');
      expect(value.isReady, false);
      expect(value.canConfigureDisplays, false);
      expect(streamDisplayCountReady(value, 1), false);
      for (final text in [
        value.streamReadiness,
        value.cleanupCause,
        value.cleanupNextStep,
      ]) {
        expect(text, isNot(contains('복원했어요')));
        expect(text, isNot(contains('복원됐어요')));
        expect(text, isNot(contains('완료했어요')));
      }
    }
    expect(quiet.streamReadiness, contains('새로고침'));
    expect(blocked.streamReadiness, contains('관리자에게 알려'));
    expect(quiet.cleanupNextStep, isNot(contains('이전 스트리밍 종료도')));
    expect(blocked.cleanupNextStep, contains('이전 스트리밍 종료도 확인되지 않았어요'));
    expect(
      _vm(_cleanup('awaiting_capabilities', age: const Duration(minutes: 2)))
          .cleanupCause,
      contains('새 화면 상태는 아직 확인되지 않았어요'),
    );
  });

  testWidgets(
    'a short normal wait looks ready and connects as soon as it clears',
    (t) async {
      final settling = _vm(
        _cleanup('awaiting_capabilities', age: const Duration(seconds: 5)),
      );
      final calls = await _card(t, settling);
      // No blocking card: the tile reads ready and its button works.
      expect(find.text('이전 연결을 정리하고 있어요'), findsNothing);
      expect(find.byKey(const Key('stream-state-pve-149')), findsNothing);
      expect(
        t.widget<FilledButton>(find.byKey(const Key('open-pve-149'))).onPressed,
        isNotNull,
      );
      await t.tap(find.byKey(const Key('open-pve-149')));
      await acceptStreamConnect(t, 'pve-149');
      // Accepting the connection starts the cleanup wait and its live spinner.
      // Do not settle an intentionally pending operation to its timeout.
      await t.pump();
      await t.pump(const Duration(milliseconds: 500));
      // Still cleaning up: it waits (and looks again) instead of opening.
      expect(calls.opens, 0);
      expect(find.textContaining('연결하는 중'), findsWidgets);
      await t.pump(const Duration(seconds: 2));
      expect(calls.refreshes, greaterThan(0));
      expect(calls.opens, 0);
      // The broker reports the cleanup done: the connection opens once.
      await _shell(t, _vm(const SessionCleanupState()), calls);
      await t.pump();
      await t.pump();
      expect(calls.opens, 1);
      await t.pumpWidget(const SizedBox());
    },
  );

  testWidgets('a wait that stalls drops the pending connection', (t) async {
    final calls = await _card(
      t,
      _vm(_cleanup('awaiting_capabilities', age: const Duration(seconds: 5))),
    );
    await t.tap(find.byKey(const Key('open-pve-149')));
      await acceptStreamConnect(t, 'pve-149');
    await t.pump();
    await t.pump(const Duration(milliseconds: 500));
    expect(find.textContaining('연결하는 중'), findsWidgets);
    await _shell(t, _vm(_cleanup('recovery_required')), calls);
    await t.pump();
    await t.pump();
    expect(calls.opens, 0);
    expect(find.text('화면 복원이 확인되지 않았어요'), findsWidgets);
    expect(find.textContaining('연결하는 중'), findsNothing);
    await t.pumpWidget(const SizedBox());
  });

  testWidgets(
    'stalled wait with a quiescent capture offers refresh, never streaming',
    (t) async {
      final calls = await _card(t, _vm(_cleanup('unknown', quiet: true)));
      // Streaming is not offered at all: the one main button is the refresh.
      expect(find.byKey(const Key('open-pve-149')), findsNothing);
      expect(find.byKey(const Key('stream-modes-pve-149')), findsNothing);
      expect(find.text('화면 복원이 확인되지 않았어요'), findsWidgets);
      expect(
        find.byKey(const Key('stream-state-pve-149-warning')),
        findsOneWidget,
      );
      await t.tap(find.byKey(const Key('stream-state-pve-149')));
      await t.pumpAndSettle();
      expect(find.text('화면 복원을 확인하지 못했어요'), findsOneWidget);
      expect(
        _text(t, const Key('cleanup-next-step')),
        startsWith('새로고침해 다시 확인하고'),
      );
      expect(
        _text(t, const Key('cleanup-evidence')),
        startsWith('Host 확인 시간 초과 · 상태 기록 '),
      );
      expect(
        find.byWidgetPredicate(
          (w) => w is FilledButton && w.key == const Key('cleanup-refresh'),
        ),
        findsOneWidget,
      );
      await t.tap(find.byKey(const Key('cleanup-refresh')));
      await t.pumpAndSettle();
      expect(find.byKey(const Key('session-cleanup-dialog')), findsNothing);
      expect([calls.opens, calls.refreshes], [0, 1]);
      // The tile's own refresh is the same broker re-read.
      await t.tap(find.byKey(const Key('attention-pve-149')));
      await t.pump();
      expect([calls.opens, calls.refreshes], [0, 2]);
    },
  );

  testWidgets(
    'stalled wait without capture exit proof offers refresh only, from the details link',
    (t) async {
      final calls = await _card(t, _vm(_cleanup('recovery_required')));
      expect(find.byKey(const Key('stream-modes-pve-149')), findsNothing);
      expect(
        find.byKey(const Key('stream-state-pve-149-warning')),
        findsOneWidget,
      );
      await t.tap(find.byKey(const Key('stream-state-pve-149')));
      await t.pumpAndSettle();
      expect(find.text('화면 복원을 확인하지 못했어요'), findsOneWidget);
      expect(
        _text(t, const Key('cleanup-next-step')),
        contains('이전 스트리밍 종료도 확인되지 않았어요'),
      );
      expect(
        _text(t, const Key('cleanup-evidence')),
        startsWith('Host 보고 · 복원 미확인'),
      );
      expect(
        find.byWidgetPredicate(
          (w) => w is FilledButton && w.key == const Key('cleanup-refresh'),
        ),
        findsOneWidget,
      );
      await t.tap(find.byKey(const Key('cleanup-refresh')));
      await t.pumpAndSettle();
      // The tile's own action is the same refresh; it never offers streaming.
      expect(find.byKey(const Key('open-pve-149')), findsNothing);
      await t.tap(find.byKey(const Key('attention-pve-149')));
      await t.pumpAndSettle();
      expect([calls.opens, calls.refreshes], [0, 2]);
    },
  );

  testWidgets(
    'workspace wires refresh, withholds it while busy and keeps card size',
    (t) async {
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      final machines = [
        _vm(_cleanup('unknown', quiet: true), id: 'stalled'),
        _vm(_cleanup('pending'), id: 'waiting'),
        _vm(const SessionCleanupState(), id: 'ready'),
      ];
      var refreshes = 0;
      Future<void> show({
        bool busy = false,
        Size size = const Size(1440, 900),
        double scale = 1,
      }) async {
        t.view.physicalSize = size;
        await t.pumpWidget(
          MaterialApp(
            theme: buildAppTheme(),
            home: MediaQuery(
              data: MediaQueryData(
                size: size,
                textScaler: TextScaler.linear(scale),
              ),
              child: WorkspaceShell(
                onSignOut: () {},
                onRefresh: () => refreshes++,
                busy: busy,
                machines: machines,
                displayName: 'tester',
                serverOrigin: 'https://cloud.example.com',
                onConnect: (_, {options = const StreamOptions()}) async =>
                    throw StateError('fixture only'),
              ),
            ),
          ),
        );
        // A busy list shows an indeterminate progress bar that never settles.
        busy
            ? await t.pump(const Duration(milliseconds: 400))
            : await t.pumpAndSettle();
      }

      await show();
      final sizes = [
        for (final vm in machines) t.getSize(find.byKey(Key('card-${vm.id}'))),
      ];
      expect(sizes.toSet(), hasLength(1));
      expect(find.byKey(const Key('open-stalled')), findsNothing);
      // A normal wait connects once it clears (no blocked button).
      expect(
        t.widget<FilledButton>(find.byKey(const Key('open-waiting'))).onPressed,
        isNotNull,
      );
      expect(
        t.widget<FilledButton>(find.byKey(const Key('open-ready'))).onPressed,
        isNotNull,
      );
      await t.tap(find.byKey(const Key('stream-state-stalled')));
      await t.pumpAndSettle();
      await t.tap(find.byKey(const Key('cleanup-refresh')));
      await t.pumpAndSettle();
      expect(refreshes, 1);

      await show(busy: true);
      await t.tap(find.byKey(const Key('stream-state-stalled')));
      await t.pump(const Duration(milliseconds: 400));
      expect(find.byKey(const Key('session-cleanup-dialog')), findsOneWidget);
      expect(find.byKey(const Key('cleanup-refresh')), findsNothing);
      await t.tap(find.byKey(const Key('cleanup-close')));
      await t.pump(const Duration(milliseconds: 400));
      expect(find.byKey(const Key('session-cleanup-dialog')), findsNothing);

      await show(size: const Size(320, 700), scale: 2);
      expect(t.takeException(), isNull);
      await t.tap(find.byKey(const Key('stream-state-stalled')));
      await t.pumpAndSettle();
      for (final action in ['cleanup-close', 'cleanup-refresh']) {
        expect(find.byKey(Key(action)).hitTestable(), findsOneWidget);
      }
      expect(t.takeException(), isNull);
      expect(refreshes, 1);
    },
  );
}

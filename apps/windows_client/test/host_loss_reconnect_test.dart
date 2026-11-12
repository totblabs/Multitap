// A stream the relay ended because it lost the Host's connection (as with 4K
// video: host_error, then a reconnect that fails once as
// display_not_supported while the Host restores its display). Fixtures only:
// the broker socket, decoder and Host snapshots are fakes.
import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_change_robustness_test.dart' show ViewFixture, mockWindow;
import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionSocket;
import 'viewport_session_test.dart' show ResizeMedia;

/// The Host is back but still restoring its virtual display.
DesktopMachine restoring() => DesktopMachine(
  id: 'qa-vm',
  name: 'QA VM',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.awaitingHost,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: true,
  interactiveSession: true,
  streamPreparation: StreamPreparation.display,
);

/// A stream whose Host went away with the VM running (Windows restarting
/// from inside), pumped until the window waits for the desktop. This is
/// the only way into that wait: the window never restarts a VM itself.
Future<
  ({
    List<ViewFixture> fixtures,
    void Function() back,
    int Function() reconnects,
  })
>
_hostAwayWait(WidgetTester tester) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = const Size(800, 600);
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await mockWindow(tester);
  final fixtures = <ViewFixture>[];
  ViewFixture create() {
    final socket = SessionSocket(), media = ResizeMedia();
    final fixture = ViewFixture(
      socket,
      media,
      ViewFixture.build(socket, media),
    );
    fixtures.add(fixture);
    return fixture;
  }

  final away = DesktopMachine(
    id: 'qa-vm',
    name: 'QA VM',
    description: '',
    kind: MachineKind.vm,
    status: MachineStatus.awaitingHost,
    specification: 'Windows',
    accent: Colors.blue,
    hostOnline: false,
    powerState: 'running',
    powerStale: false,
    powerBusy: false,
  );
  final first = create();
  var reconnects = 0, back = false;
  await tester.pumpWidget(
    MaterialApp(
      home: RemoteView(
        machine: machine(1),
        initialOptions: const StreamOptions(),
        initialDisplayMode: SessionDisplayMode.windowed,
        load: () async => first.session,
        onRefreshDesktop: () async => back ? machine(1) : away,
        onReconnect: (_) async {
          reconnects++;
          return create().session;
        },
      ),
    ),
  );
  await tester.pump();
  await tester.pump();
  await first.initialFrame(tester);
  first.socket.incoming.add(
    jsonEncode({
      'type': 'session.ended',
      'reason': 'host_disconnected',
      'detail': 'host_service_stopping',
    }),
  );
  await tester.pump();
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 100));
  // A minute: past the 30 s a Host restart gets.
  for (var n = 0; n < 120; n++) {
    await tester.pump(const Duration(milliseconds: 500));
  }
  return (
    fixtures: fixtures,
    back: () => back = true,
    reconnects: () => reconnects,
  );
}

Future<void> _dispose(WidgetTester tester, List<ViewFixture> fixtures) async {
  await tester.pumpWidget(const SizedBox.shrink());
  for (final fixture in fixtures) {
    unawaited(fixture.session.close());
  }
  await tester.pump(const Duration(seconds: 3));
  for (final fixture in fixtures) {
    unawaited(fixture.media.events.close());
  }
}

void main() {
  testWidgets(
    'The wait for the desktop can be cancelled: no reconnect, the ordinary card with 다시 연결',
    (tester) async {
      final run = await _hostAwayWait(tester);
      try {
        expect(find.byKey(const Key('planned-restart-wait')), findsOneWidget);
        expect(find.text('기다리기 취소'), findsOneWidget);
        await tester.tap(find.text('기다리기 취소'));
        await tester.pump();
        expect(find.byKey(const Key('planned-restart-wait')), findsNothing);
        expect(find.textContaining('기다리기를 취소했어요'), findsOneWidget);
        expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
        run.back();
        for (var n = 0; n < 10; n++) {
          await tester.pump(const Duration(milliseconds: 500));
        }
        expect(run.reconnects(), 0);
        expect(tester.takeException(), isNull);
      } finally {
        await _dispose(tester, run.fixtures);
      }
    },
  );

  testWidgets(
    'A lost Host connection reconnects once by itself and waits for the Host instead of failing; a second loss right away leaves the card',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(800, 600);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await mockWindow(tester);
      final fixtures = <ViewFixture>[];
      ViewFixture create() {
        final socket = SessionSocket(), media = ResizeMedia();
        final fixture = ViewFixture(
          socket,
          media,
          ViewFixture.build(socket, media),
        );
        fixtures.add(fixture);
        return fixture;
      }

      final first = create();
      var reconnects = 0, reads = 0;
      final states = [restoring(), restoring(), machine(1)];
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            initialOptions: const StreamOptions(),
            initialDisplayMode: SessionDisplayMode.windowed,
            load: () async => first.session,
            onRefreshDesktop: () async =>
                states[math.min(reads++, states.length - 1)],
            onReconnect: (_) async {
              reconnects++;
              return create().session;
            },
          ),
        ),
      );
      await tester.pump();
      await tester.pump();
      try {
        await first.initialFrame(tester);
        first.socket.incoming.add(
          jsonEncode({
            'type': 'session.ended',
            'reason': 'host_error',
            'detail': 'host_frame_invalid',
          }),
        );
        await tester.pump();
        await tester.pump();
        expect(first.session.closed, isTrue);
        expect(first.session.endedByHostLoss, isTrue);
        expect(first.session.diagnosticCode, 'host_frame_invalid');
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 100));
        expect(find.text('Host 연결이 끊겨 다시 연결하고 있어요'), findsOneWidget);
        // The Host is still restoring its display: no connection is attempted
        // and no failure is shown until it streams again.
        await tester.pump(const Duration(milliseconds: 300));
        expect(reconnects, 0);
        expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
        for (var n = 0; n < 30 && reconnects == 0; n++) {
          await tester.pump(const Duration(milliseconds: 100));
        }
        expect(reads, 3);
        expect(reconnects, 1);
        final second = fixtures.last;
        await tester.pump();
        await second.initialFrame(tester);
        expect(second.session.closed, isFalse);
        expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
        // The same loss again right away: no loop, the ordinary card.
        second.socket.incoming.add(
          jsonEncode({'type': 'session.ended', 'reason': 'host_disconnected'}),
        );
        await tester.pump(const Duration(seconds: 2));
        await tester.pump();
        expect(reconnects, 1);
        expect(
          find.byKey(const Key('retry-remote-connection')),
          findsOneWidget,
        );
        expect(tester.takeException(), isNull);
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        for (final fixture in fixtures) {
          unawaited(fixture.session.close());
        }
        await tester.pump(const Duration(seconds: 3));
        for (final fixture in fixtures) {
          unawaited(fixture.media.events.close());
        }
      }
    },
  );

  // Windows restarted from inside the VM (Start →
  // 다시 시작). The Host goes away while the VM keeps running, for longer than
  // the 30 s a Host restart gets; the window waits like its own restart.
  testWidgets(
    'A Host that stays away with the VM running is waited for up to 3 minutes and reconnected',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(800, 600);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await mockWindow(tester);
      final fixtures = <ViewFixture>[];
      ViewFixture create() {
        final socket = SessionSocket(), media = ResizeMedia();
        final fixture = ViewFixture(
          socket,
          media,
          ViewFixture.build(socket, media),
        );
        fixtures.add(fixture);
        return fixture;
      }

      final away = DesktopMachine(
        id: 'qa-vm',
        name: 'QA VM',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.awaitingHost,
        specification: 'Windows',
        accent: Colors.blue,
        hostOnline: false,
        powerState: 'running',
        powerStale: false,
        powerBusy: false,
      );
      final first = create();
      var reconnects = 0, back = false;
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            initialOptions: const StreamOptions(),
            initialDisplayMode: SessionDisplayMode.windowed,
            load: () async => first.session,
            onRefreshDesktop: () async => back ? machine(1) : away,
            onReconnect: (_) async {
              reconnects++;
              return create().session;
            },
          ),
        ),
      );
      await tester.pump();
      await tester.pump();
      try {
        await first.initialFrame(tester);
        first.socket.incoming.add(
          jsonEncode({
            'type': 'session.ended',
            'reason': 'host_disconnected',
            'detail': 'host_service_stopping',
          }),
        );
        await tester.pump();
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 100));
        // A minute: past the 30 s a Host restart gets.
        for (var n = 0; n < 120; n++) {
          await tester.pump(const Duration(milliseconds: 500));
        }
        expect(reconnects, 0);
        expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
        expect(find.byKey(const Key('planned-restart-wait')), findsOneWidget);
        expect(find.text('데스크톱을 기다리는 중'), findsOneWidget);
        back = true;
        for (var n = 0; n < 30 && reconnects == 0; n++) {
          await tester.pump(const Duration(milliseconds: 100));
        }
        expect(reconnects, 1);
        await tester.pump();
        await fixtures.last.initialFrame(tester);
        expect(fixtures.last.session.frameReady, isTrue);
        expect(find.byKey(const Key('planned-restart-wait')), findsNothing);
        expect(tester.takeException(), isNull);
      } finally {
        await _dispose(tester, fixtures);
      }
    },
  );

  // A stream window whose VM is signed out or
  // locked offers the Console (opened by the launcher), and only then.
  for (final (code, offered) in [
    ('no_interactive_session', true),
    ('host_offline', true),
    ('display_not_supported', false),
  ]) {
    testWidgets('Console로 열기 on the end card: $code', (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(800, 600);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await mockWindow(tester);
      var consoles = 0;
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            initialOptions: const StreamOptions(),
            initialDisplayMode: SessionDisplayMode.windowed,
            load: () async => throw BrokerException(code, 409),
            onOpenConsole: () async => consoles++,
          ),
        ),
      );
      for (var n = 0; n < 5; n++) {
        await tester.pump(const Duration(milliseconds: 100));
      }
      final button = find.text('Console로 열기');
      expect(button, offered ? findsOneWidget : findsNothing);
      if (offered) {
        await tester.tap(button);
        await tester.pump();
        expect(consoles, 1);
      }
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump(const Duration(seconds: 1));
    });
  }
  // A 로컬 PC has no Console, so its signed-out card does not send
  // the person to one; a VM's card keeps the Console text.
  for (final (kind, text) in [
    (MachineKind.vm, 'Windows가 로그아웃됐거나 잠겨 있어요.\nConsole로 열어 로그인하세요.'),
    (MachineKind.pc, signedOutLocalPcMessage),
  ]) {
    testWidgets('the signed-out end card of a ${kind.name}', (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(800, 600);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await mockWindow(tester);
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: DesktopMachine(
              id: 'qa',
              name: '연결 검사',
              description: '',
              kind: kind,
              status: MachineStatus.ready,
              specification: 'Windows',
              accent: Colors.blue,
            ),
            initialOptions: const StreamOptions(),
            initialDisplayMode: SessionDisplayMode.windowed,
            load: () async =>
                throw const BrokerException('no_interactive_session', 409),
          ),
        ),
      );
      for (var n = 0; n < 5; n++) {
        await tester.pump(const Duration(milliseconds: 100));
      }
      expect(find.text(text), findsOneWidget);
      expect(signedOutLocalPcMessage, isNot(contains('Console')));
      expect(
        find.textContaining('Console로 열어'),
        kind == MachineKind.vm ? findsOneWidget : findsNothing,
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump(const Duration(seconds: 1));
    });
  }
}

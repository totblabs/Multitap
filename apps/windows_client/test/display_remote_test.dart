import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;

DesktopMachine machine(int count, {bool cleanup = false}) => DesktopMachine(
  id: 'qa-vm',
  name: 'QA VM',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: true,
  virtualMonitorControl: true,
  sessionCleanup: SessionCleanupState(pending: cleanup),
  display: VirtualDisplayState(
    supported: true,
    activeCount: count,
    stale: false,
    checkedAt: DateTime.utc(2026, 10, 22),
  ),
  monitors: [
    const RemoteMonitor(id: '0', width: 1920, height: 1080),
    if (count == 2) const RemoteMonitor(id: '1', width: 1920, height: 1080),
  ],
);

void main() {
  for (final negotiated in [false, true]) {
    testWidgets(
      'Remote monitor change releases and closes first; negotiated cleanup=$negotiated; reconnect uses the new topology',
      (tester) async {
        tester.view.physicalSize = const Size(1440, 900);
        tester.view.devicePixelRatio = 1;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        final messenger =
            TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
        const window = MethodChannel('multitap/window'),
            events = MethodChannel('multitap/window/events');
        messenger.setMockMethodCallHandler(window, (_) async => false);
        messenger.setMockMethodCallHandler(events, (_) async => null);
        addTearDown(() {
          messenger.setMockMethodCallHandler(window, null);
          messenger.setMockMethodCallHandler(events, null);
        });
        final socket = SessionSocket(), media = SessionMedia();
        final old = RemoteSession(
          connection: BrokerConnection(
            sessionId: 'fixture-session',
            webSocketUrl: Uri.parse(
              'wss://cloud.example.com/api/v1/sessions/fixture-session',
            ),
            options: const StreamOptions(monitor: 'all', maxWidth: 3840),
          ),
          token: 'fixture-only-token',
          dialer: (_, _) async => socket,
          media: media,
        );
        int changes = 0, reconnects = 0, closedReads = 0;
        StreamOptions? requested;
        await tester.pumpWidget(
          MaterialApp(
            home: RemoteView(
              machine: machine(2),
              load: () async => old,
              onRefreshDesktop: !negotiated
                  ? null
                  : () async {
                      if (!old.closed) return machine(2);
                      return machine(2, cleanup: ++closedReads == 1);
                    },
              onConfigureDisplays: (count) async {
                if (!old.closed || old.captured || count != 1) {
                  throw StateError(
                    'The previous remote input and session must be released first.',
                  );
                }
                changes++;
                if (negotiated) expect(closedReads, 2);
                return machine(1);
              },
              onReconnect: (options) async {
                reconnects++;
                requested = options;
                throw const BrokerException('display_busy');
              },
            ),
          ),
        );
        await tester.pump();
        socket.incoming.add(jsonEncode(info));
        await tester.pump();
        await tester.pump();
        socket.incoming.add(packet());
        await tester.pump();
        await tester.pump();
        // A view that does not derive the count from its screen mode keeps
        // the count in the 화면 panel, under the screen modes (session
        // windows never show it: they prepare one output for a window or
        // full screen and two for 모든 모니터).
        await tester.tap(find.byKey(const Key('expand-session-toolbar')));
        await tester.pump();
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('session-screen-panel')), findsOneWidget);
        expect(find.text('모니터 수…'), findsOneWidget);
        // The panel's row hands its key to its InkWell too: the first match
        // is the row.
        await tester.tap(
          find.byKey(const Key('remote-virtual-displays')).first,
        );
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('session-screen-panel')), findsNothing);
        // The current count is no change.
        FilledButton apply() => tester.widget<FilledButton>(
          find.byKey(const Key('apply-virtual-displays')),
        );
        expect(apply().onPressed, isNull);
        await tester.tap(find.text('1대'));
        await tester.pump();
        expect(apply().onPressed, isNotNull);
        // Applying asks first; cancelling that question sends nothing.
        await tester.tap(find.byKey(const Key('apply-virtual-displays')));
        await tester.pumpAndSettle();
        expect(
          find.byKey(const Key('confirm-virtual-displays')),
          findsOneWidget,
        );
        await tester.tap(find.widgetWithText(TextButton, '취소').last);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('confirm-virtual-displays')), findsNothing);
        expect(changes, 0);
        expect(old.closed, isFalse);
        await tester.tap(find.byKey(const Key('apply-virtual-displays')));
        await tester.pumpAndSettle();
        expect(changes, 0);
        expect(old.closed, isFalse);
        await tester.tap(find.byKey(const Key('confirm-virtual-displays')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 400));
        await tester.runAsync(() async {
          await Future<void>.delayed(const Duration(milliseconds: 10));
        });
        await tester.pumpAndSettle();
        expect(old.closed, isTrue);
        expect(
          media.stopped,
          isTrue,
          reason: 'Production session close must finish the native media stop before display configuration.',
        );
        expect(changes, 1);
        expect(reconnects, 0);
        expect(find.text('모니터를 1대로 바꿨어요'), findsOneWidget);
        await tester.tap(
          find.byKey(const Key('reconnect-after-display-change')),
        );
        await tester.pumpAndSettle();
        // No virtual-monitor choice: the request keeps the one output.
        expect(
          find.descendant(
            of: find.byType(StreamSettingsDialog),
            matching: find.textContaining('가상 화면'),
          ),
          findsNothing,
        );
        expect(
          tester
              .widget<FilledButton>(
                find.byKey(const Key('apply-stream-settings')),
              )
              .onPressed,
          isNotNull,
          reason: 'A completed display change already closed the stream; reconnect must work without changing quality.',
        );
        await tester.tap(find.byKey(const Key('apply-stream-settings')));
        await tester.pumpAndSettle();
        expect(reconnects, 1);
        expect(requested!.monitor, '0');
        expect(requested!.maxWidth, 3840);
        expect(
          find.text(const BrokerException('display_busy').message),
          findsOneWidget,
        );
        await tester.pumpWidget(const SizedBox.shrink());
        await tester.pump();
      },
    );
  }
}

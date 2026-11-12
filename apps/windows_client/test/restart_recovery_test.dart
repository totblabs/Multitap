// Restart recovery (2026-11-03): after a broker update ends the stream as
// server_stopping, the window waits for the broker and then for its desktop's
// Host. Fake time, sockets, media and broker answers; no real broker or Host.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_change_robustness_test.dart' show ViewFixture, mockWindow;
import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionSocket;
import 'stream_display_entry_test.dart' show entryMachine;
import 'viewport_session_test.dart' show ResizeMedia;

/// The VM runs; its Host has not reached the restarted broker yet.
DesktopMachine hostAway({String id = 'pve-149'}) => DesktopMachine(
  id: id,
  name: 'TESTER',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.awaitingHost,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: false,
  powerState: 'running',
  powerStale: false,
);

const refused = BrokerException('network_error', 0, 'connection_refused');
const reset = BrokerException('network_error', 0, 'connection_reset');

void main() {
  testWidgets(
    'A restarting broker that refuses or resets is asked again every 250 ms, inside the recovery wait',
    (t) async {
      // Before: the first refused read left this wait and the view retried the
      // whole connection after 1, 2, 4 and 8 s (ServerRestartWait).
      final answers = <Object>[refused, refused, reset, entryMachine(1)];
      var reads = 0;
      DesktopMachine? ready;
      Object? failure;
      unawaited(
        waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async {
            final answer = answers[reads++];
            if (answer is DesktopMachine) return answer;
            throw answer;
          },
          current: () => true,
          recovering: true,
          refreshFirst: true,
          retryGatewayErrors: true,
        ).then((value) => ready = value, onError: (Object e) => failure = e),
      );
      await t.pump();
      expect(failure, isNull);
      for (var n = 0; n < 3; n++) {
        await t.pump(restartPollInterval);
      }
      expect(reads, 4);
      expect(ready?.id, 'pve-149');
    },
  );

  testWidgets(
    'Outside a transport recovery a refused port still ends the wait at once',
    (t) async {
      Object? failure;
      unawaited(
        waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async => throw refused,
          current: () => true,
          recovering: true,
          refreshFirst: true,
        ).then((value) => value, onError: (Object e) => failure = e),
      );
      await t.pump();
      expect(failure, same(refused));
    },
  );

  testWidgets(
    'The broker\'s host-ready answer ends the wait at once; the list is read once before and once after',
    (t) async {
      final reported = Completer<bool?>();
      var reads = 0, asks = 0;
      DesktopMachine? ready;
      unawaited(
        waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async {
            reads++;
            return reported.isCompleted ? entryMachine(1) : hostAway();
          },
          current: () => true,
          recovering: true,
          refreshFirst: true,
          retryGatewayErrors: true,
          waitHost: () {
            asks++;
            return reported.future;
          },
        ).then((value) => ready = value),
      );
      await t.pump(const Duration(seconds: 4));
      // Before: a full list read every 500 ms (9 by now).
      expect(reads, 1);
      expect(asks, 1);
      expect(ready, isNull);
      reported.complete(true);
      await t.pump();
      await t.pump();
      expect(reads, 2);
      expect(asks, 1);
      expect(ready?.id, 'pve-149');
    },
  );

  testWidgets(
    'A broker without host-ready (null) keeps the 500 ms list reads',
    (t) async {
      var reads = 0, asks = 0;
      DesktopMachine? ready;
      unawaited(
        waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async => ++reads < 4 ? hostAway() : entryMachine(1),
          current: () => true,
          recovering: true,
          refreshFirst: true,
          retryGatewayErrors: true,
          waitHost: () async {
            asks++;
            return null;
          },
        ).then((value) => ready = value),
      );
      await t.pump();
      await t.pump(const Duration(milliseconds: 499));
      expect(reads, 1);
      await t.pump(const Duration(milliseconds: 1));
      expect(reads, 2);
      await t.pump(const Duration(milliseconds: 1000));
      expect(reads, 4);
      expect(ready, isNotNull);
      expect(asks, 3);
    },
  );

  testWidgets(
    'An answer the next list read contradicts does not spin: the list is read at the old pace',
    (t) async {
      var reads = 0, asks = 0;
      unawaited(
        waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async {
            reads++;
            return hostAway();
          },
          current: () => true,
          recovering: true,
          refreshFirst: true,
          retryGatewayErrors: true,
          maxWait: const Duration(seconds: 5),
          waitHost: () async {
            asks++;
            return true;
          },
        ).then((value) => value, onError: (Object _) => hostAway()),
      );
      await t.pump();
      await t.pump(const Duration(seconds: 2));
      // At most one immediate re-read per answer, then 500 ms.
      expect(reads, lessThanOrEqualTo(10));
      expect(asks, lessThanOrEqualTo(5));
      await t.pump(const Duration(seconds: 4));
    },
  );

  testWidgets(
    'A failed host-ready (gateway, refused) falls back to a list read 250 ms later',
    (t) async {
      var reads = 0, asks = 0;
      DesktopMachine? ready;
      unawaited(
        waitForStreamDisplayControl(
          entryMachine(1),
          refresh: () async => ++reads < 2 ? hostAway() : entryMachine(1),
          current: () => true,
          recovering: true,
          refreshFirst: true,
          retryGatewayErrors: true,
          waitHost: () async {
            asks++;
            throw const BrokerException('server_restarting', 502);
          },
        ).then((value) => ready = value),
      );
      await t.pump();
      await t.pump(const Duration(milliseconds: 249));
      expect(reads, 1);
      await t.pump(const Duration(milliseconds: 1));
      await t.pump();
      expect(reads, 2);
      expect(asks, 1);
      expect(ready, isNotNull);
    },
  );

  testWidgets(
    'Simulated: time from the Host reporting to the end of the wait, list polling vs host-ready',
    (t) async {
      // The broker answers the list after `brokerUpMs`, the Host reports at
      // `hostMs`; each list read takes 20 ms. Fake time, not a field result.
      Future<int> lateMs({
        required int brokerUpMs,
        required int hostMs,
        required bool hostReady,
      }) async {
        // Fake milliseconds since the start of this run.
        var ms = 0;
        final tick = Timer.periodic(
          const Duration(milliseconds: 1),
          (_) => ms++,
        );
        int elapsed() => ms;
        final reported = Completer<bool?>();
        Timer(Duration(milliseconds: hostMs), () => reported.complete(true));
        DesktopMachine? ready;
        int? doneAt;
        unawaited(
          waitForStreamDisplayControl(
            entryMachine(1),
            refresh: () async {
              await Future<void>.delayed(const Duration(milliseconds: 20));
              if (elapsed() < brokerUpMs) {
                throw const BrokerException('server_restarting', 502);
              }
              return elapsed() < hostMs ? hostAway() : entryMachine(1);
            },
            current: () => true,
            recovering: true,
            refreshFirst: true,
            retryGatewayErrors: true,
            waitHost: hostReady ? () => reported.future : () async => null,
          ).then((value) {
            ready = value;
            doneAt = elapsed();
          }),
        );
        for (var n = 0; n < 800 && ready == null; n++) {
          await t.pump(const Duration(milliseconds: 5));
        }
        tick.cancel();
        expect(ready, isNotNull);
        return doneAt! - hostMs;
      }

      final rows = <Map<String, int>>[];
      for (final (brokerUpMs, hostMs) in [
        (400, 770),
        (900, 1790),
        (1300, 1790),
      ]) {
        final polling = await lateMs(
          brokerUpMs: brokerUpMs,
          hostMs: hostMs,
          hostReady: false,
        );
        final waiting = await lateMs(
          brokerUpMs: brokerUpMs,
          hostMs: hostMs,
          hostReady: true,
        );
        expect(waiting, lessThanOrEqualTo(polling));
        expect(
          waiting,
          lessThanOrEqualTo(30),
          reason: 'one list read after the answer',
        );
        rows.add({
          'brokerUpMs': brokerUpMs,
          'hostMs': hostMs,
          'listPollingLateMs': polling,
          'hostReadyLateMs': waiting,
        });
      }
      // ignore: avoid_print
      print(jsonEncode({'simulated': true, 'clientWaitAfterHostReport': rows}));
    },
  );

  testWidgets(
    'server_stopping: the window says the server restarts, waits on host-ready and reconnects when the Host reports',
    (t) async {
      t.view.physicalSize = const Size(800, 600);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      await mockWindow(t);
      final fixtures = <ViewFixture>[], released = <String>[];
      ViewFixture create() {
        final socket = SessionSocket(), media = ResizeMedia();
        final id = 'restart-${fixtures.length}';
        final current = RemoteSession(
          connection: BrokerConnection(
            sessionId: id,
            webSocketUrl: Uri.parse('wss://fixture.invalid/sessions/$id'),
            reservation: BrokerSessionReservation(() async => released.add(id)),
            epochMedia: true,
            viewportProvisioning: true,
            options: const StreamOptions(),
          ),
          token: 'fixture-only',
          media: media,
          dialer: (_, _) async => socket,
        );
        final fixture = ViewFixture(socket, media, current);
        fixtures.add(fixture);
        return fixture;
      }

      final first = create();
      final reported = Completer<bool?>();
      var reads = 0, asks = 0, reconnects = 0;
      final hostBack = <Object>[
        refused,
        const BrokerException('server_restarting', 502),
      ];
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            load: () async => first.session,
            onRefreshDesktop: () async {
              final n = reads++;
              if (n == 0) return machine(1); // the first connection's read
              if (n - 1 < hostBack.length) throw hostBack[n - 1];
              return reported.isCompleted ? machine(1) : hostAway(id: 'qa-vm');
            },
            onWaitHostReady: () {
              asks++;
              return reported.future;
            },
            onReconnect: (_) async {
              reconnects++;
              return create().session;
            },
          ),
        ),
      );
      await t.pump();
      await t.pump();
      try {
        await first.initialFrame(t);
        final readsBefore = reads;
        first.socket.incoming.add(
          jsonEncode({'type': 'session.ended', 'reason': 'server_stopping'}),
        );
        for (var pass = 0; pass < 8; pass++) {
          await t.pump(const Duration(milliseconds: 100));
        }
        expect(first.session.diagnosticCode, 'server_stopping');
        expect(released, ['restart-0']);
        // Refused, 502, then the Host is away: within 0.8 s, not after 1 s + backoff.
        expect(reads - readsBefore, 3);
        expect(asks, 1);
        expect(reconnects, 0);
        expect(find.text(serverRestartingMessage), findsOneWidget);
        await t.pump(const Duration(seconds: 3));
        expect(
          reads - readsBefore,
          3,
          reason: 'The list was polled while host-ready waited',
        );
        reported.complete(true);
        await t.pump();
        await t.pump();
        await t.pump();
        expect(reconnects, 1);
        await t.pump();
        await fixtures.last.initialFrame(t);
        expect(find.text(serverRestartingMessage), findsNothing);
      } finally {
        await t.pumpWidget(const SizedBox());
        for (final fixture in fixtures) {
          unawaited(fixture.session.close());
        }
        await t.pump(const Duration(seconds: 3));
        for (final fixture in fixtures) {
          await fixture.media.events.close();
        }
      }
    },
  );
}

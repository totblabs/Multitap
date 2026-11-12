// A Host update ended the stream (host_update): the remote window
// waits for the update and its Host, then reconnects by itself, like after
// the sound-card restart. Fake time, sockets, media and broker answers; no
// real broker, Host or update.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/session_failure.dart';
import 'package:multitap_client/data/stream_display_entry.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_change_robustness_test.dart' show ViewFixture, mockWindow;
import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionSocket;
import 'viewport_session_test.dart' show ResizeMedia;

HostUpdateState _update(String status, {bool online = true}) => HostUpdateState(
  present: true,
  connected: online,
  supported: true,
  version: online ? '0.2.130' : null,
  latestVersion: '0.2.131',
  operation: HostUpdateOperation(
    status: status,
    targetVersion: '0.2.131',
    checkedAt: DateTime.now().toUtc(),
  ),
);

/// The VM runs; its Host is under [status] of an update.
DesktopMachine _updating(
  String status, {
  bool online = true,
  HostMissingAfterUpdate? missing,
}) => DesktopMachine(
  id: 'qa-vm',
  name: 'QA VM',
  description: '',
  kind: MachineKind.vm,
  status: online ? MachineStatus.ready : MachineStatus.awaitingHost,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: online,
  powerState: 'running',
  powerStale: false,
  hostUpdate: _update(status, online: online),
  hostMissingAfterUpdate: missing,
);

/// Back on the new version.
DesktopMachine _updated() => machine(1).withHostUpdate(_update('completed'));

const _manual = 'Host 업데이트 때문에 연결이 끊겼어요. 업데이트가 끝나면 다시 연결하세요.';

class _Run {
  final fixtures = <ViewFixture>[];
  late DesktopMachine Function() list;

  /// Thrown by the next reconnects, in order (then they connect).
  final refusals = <Object>[];
  Completer<bool?>? reported;
  int reads = 0, asks = 0, reconnects = 0;

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
}

/// A stream, then the relay's end [reason]; from then on the list answers
/// [during].
Future<_Run> _ended(
  WidgetTester t, {
  required DesktopMachine Function() during,
  String reason = 'host_update',
  List<Object> refusals = const [],
}) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = const Size(800, 600);
  addTearDown(t.view.reset);
  await mockWindow(t);
  final run = _Run()..list = () => machine(1);
  final first = run.create();
  await t.pumpWidget(
    MaterialApp(
      home: RemoteView(
        machine: machine(1),
        initialOptions: const StreamOptions(),
        initialDisplayMode: SessionDisplayMode.windowed,
        load: () async => first.session,
        onRefreshDesktop: () async {
          run.reads++;
          return run.list();
        },
        onWaitHostReady: () {
          run.asks++;
          return (run.reported ??= Completer<bool?>()).future;
        },
        onReconnect: (_) async {
          run.reconnects++;
          if (run.refusals.isNotEmpty) throw run.refusals.removeAt(0);
          return run.create().session;
        },
      ),
    ),
  );
  await t.pump();
  await t.pump();
  await first.initialFrame(t);
  run.list = during;
  run.refusals.addAll(refusals);
  first.socket.incoming.add(
    jsonEncode({'type': 'session.ended', 'reason': reason}),
  );
  await t.pump();
  await t.pump();
  await t.pump(const Duration(milliseconds: 100));
  return run;
}

Future<void> _dispose(WidgetTester t, _Run run) async {
  await t.pumpWidget(const SizedBox.shrink());
  for (final fixture in run.fixtures) {
    unawaited(fixture.session.close());
  }
  await t.pump(const Duration(seconds: 3));
  for (final fixture in run.fixtures) {
    unawaited(fixture.media.events.close());
  }
}

final _wait = find.byKey(const Key('host-update-wait'));
final _retry = find.byKey(const Key('retry-remote-connection'));

void main() {
  group('the wait (waitForStreamDisplayControl hostUpdating)', () {
    testWidgets(
      'waits while the update runs, reading the list every 2 s, then returns the Host back on its new version',
      (t) async {
        var reads = 0;
        DesktopMachine? ready;
        unawaited(
          waitForStreamDisplayControl(
            machine(1),
            refresh: () async =>
                ++reads < 4 ? _updating('downloading') : _updated(),
            current: () => true,
            recovering: true,
            countControl: false,
            refreshFirst: true,
            hostUpdating: true,
            maxWait: const Duration(minutes: 10),
          ).then((value) => ready = value),
        );
        await t.pump();
        expect(reads, 1);
        await t.pump(const Duration(milliseconds: 1999));
        expect(reads, 1, reason: '2 s, not 500 ms, while the update runs');
        await t.pump(const Duration(milliseconds: 1));
        expect(reads, 2);
        await t.pump(const Duration(seconds: 4));
        expect(reads, 4);
        expect(ready?.hostUpdate.completed, isTrue);
      },
    );

    testWidgets('without hostUpdating the update is not waited for', (t) async {
      DesktopMachine? ready;
      unawaited(
        waitForStreamDisplayControl(
          machine(1),
          refresh: () async => _updating('downloading'),
          current: () => true,
          recovering: true,
          countControl: false,
          refreshFirst: true,
        ).then((value) => ready = value),
      );
      await t.pump();
      expect(ready?.hostUpdate.busy, isTrue);
    });

    testWidgets(
      'a Host that stays away after the update ends the wait at once as host_update',
      (t) async {
        Object? failure;
        unawaited(
          waitForStreamDisplayControl(
            machine(1),
            refresh: () async => _updating(
              'completed',
              online: false,
              missing: const HostMissingAfterUpdate(result: 'completed'),
            ),
            current: () => true,
            recovering: true,
            countControl: false,
            refreshFirst: true,
            hostUpdating: true,
            maxWait: const Duration(minutes: 10),
          ).then((value) => value, onError: (Object e) => failure = e),
        );
        await t.pump();
        expect(failure, isA<BrokerException>());
        expect((failure! as BrokerException).code, 'host_update');
      },
    );

    testWidgets('an update still running at the bound ends as host_update', (
      t,
    ) async {
      Object? failure;
      unawaited(
        waitForStreamDisplayControl(
          machine(1),
          refresh: () async => _updating('installing'),
          current: () => true,
          recovering: true,
          countControl: false,
          refreshFirst: true,
          hostUpdating: true,
          maxWait: const Duration(seconds: 30),
        ).then((value) => value, onError: (Object e) => failure = e),
      );
      await t.pump(const Duration(seconds: 31));
      expect((failure! as BrokerException).code, 'host_update');
    });

    test('a bare relay close for a Host update keeps its reason', () {
      expect(SessionFailure.transportCloseReason('host_update'), 'host_update');
      expect(SessionFailure.ended('host_update').message, _manual);
    });
  });

  testWidgets(
    'host_update: its own line, list reads every 2 s while the update runs, host-ready while the Host restarts, one reconnect once it is back',
    (t) async {
      final run = await _ended(t, during: () => _updating('downloading'));
      try {
        expect(_wait, findsOneWidget);
        expect(find.text('Host 업데이트 중이에요'), findsOneWidget);
        expect(find.text('기다리기 취소'), findsOneWidget);
        expect(_retry, findsNothing);
        expect(find.text(_manual), findsNothing);
        final readsAtEnd = run.reads;
        for (var n = 0; n < 5; n++) {
          await t.pump(const Duration(seconds: 1));
        }
        // Every 2 s (500 ms reads would be about ten by now).
        expect(run.reads - readsAtEnd, inInclusiveRange(2, 3));
        expect(run.reconnects, 0, reason: 'no connect the broker refuses');
        // The installer restarts the Host: the broker answers when it reports.
        run.list = () => _updating('installing', online: false);
        await t.pump(const Duration(seconds: 3));
        expect(run.asks, 1);
        final reads = run.reads;
        await t.pump(const Duration(seconds: 4));
        expect(
          run.reads,
          reads,
          reason: 'no list polling while host-ready waits',
        );
        expect(_wait, findsOneWidget);
        run.list = _updated;
        run.reported!.complete(true);
        for (var n = 0; n < 5 && run.reconnects == 0; n++) {
          await t.pump();
        }
        expect(run.reconnects, 1);
        await t.pump();
        await run.fixtures.last.initialFrame(t);
        expect(run.fixtures.last.session.frameReady, isTrue);
        expect(_wait, findsNothing);
        expect(_retry, findsNothing);
        // Nothing more happens by itself.
        await t.pump(const Duration(seconds: 20));
        expect(run.reconnects, 1);
        expect(t.takeException(), isNull);
      } finally {
        await _dispose(t, run);
      }
    },
  );

  testWidgets(
    'host_update_in_progress refusals during the wait are tried again quietly (a broker without the update view)',
    (t) async {
      final run = await _ended(
        t,
        during: () => machine(1),
        refusals: const [
          BrokerException('host_update_in_progress', 409),
          BrokerException('host_update_in_progress', 409),
        ],
      );
      try {
        expect(run.reconnects, 1);
        expect(_wait, findsOneWidget);
        expect(_retry, findsNothing);
        await t.pump(hostUpdatePollInterval);
        await t.pump(const Duration(milliseconds: 100));
        expect(run.reconnects, 2);
        expect(_wait, findsOneWidget);
        await t.pump(hostUpdatePollInterval);
        await t.pump(const Duration(milliseconds: 100));
        expect(run.reconnects, 3);
        await t.pump();
        await run.fixtures.last.initialFrame(t);
        expect(_wait, findsNothing);
        expect(_retry, findsNothing);
        expect(t.takeException(), isNull);
      } finally {
        await _dispose(t, run);
      }
    },
  );

  testWidgets('기다리기 취소: no reconnect afterwards, the card with 다시 연결', (
    t,
  ) async {
    final run = await _ended(t, during: () => _updating('downloading'));
    try {
      await t.tap(find.text('기다리기 취소'));
      await t.pump();
      expect(_wait, findsNothing);
      expect(find.textContaining('Host 업데이트 기다리기를 취소했어요'), findsOneWidget);
      expect(_retry, findsOneWidget);
      final reads = run.reads;
      run.list = _updated;
      for (var n = 0; n < 20; n++) {
        await t.pump(const Duration(milliseconds: 500));
      }
      expect(run.reconnects, 0);
      expect(run.reads, reads, reason: 'the wait stopped reading too');
      expect(t.takeException(), isNull);
    } finally {
      await _dispose(t, run);
    }
  });

  testWidgets(
    'an update that outlasts 10 minutes leaves the manual message with 다시 연결',
    (t) async {
      final run = await _ended(t, during: () => _updating('installing'));
      try {
        for (var n = 0; n < 59; n++) {
          await t.pump(const Duration(seconds: 10));
        }
        expect(_wait, findsOneWidget, reason: 'still within the bound');
        for (var n = 0; n < 2; n++) {
          await t.pump(const Duration(seconds: 10));
        }
        expect(_wait, findsNothing);
        expect(find.text(_manual), findsOneWidget);
        expect(_retry, findsOneWidget);
        expect(run.reconnects, 0);
        final reads = run.reads;
        await t.pump(const Duration(minutes: 1));
        expect(run.reads, reads, reason: 'nothing polls after the bound');
        expect(t.takeException(), isNull);
      } finally {
        await _dispose(t, run);
      }
    },
  );

  testWidgets(
    'a Host that stays away after the update stops the wait early with the manual message',
    (t) async {
      final run = await _ended(
        t,
        during: () => _updating(
          'completed',
          online: false,
          missing: const HostMissingAfterUpdate(result: 'completed'),
        ),
      );
      try {
        await t.pump(const Duration(seconds: 1));
        expect(_wait, findsNothing);
        expect(find.text(_manual), findsOneWidget);
        expect(_retry, findsOneWidget);
        expect(run.reconnects, 0);
      } finally {
        await _dispose(t, run);
      }
    },
  );

  testWidgets(
    'another end (an administrator\'s permission change) does not wait or reconnect',
    (t) async {
      final run = await _ended(
        t,
        during: () => machine(1),
        reason: 'clipboard_policy_changed',
      );
      try {
        expect(_wait, findsNothing);
        expect(_retry, findsOneWidget);
        await t.pump(const Duration(seconds: 30));
        expect(run.reconnects, 0);
        expect(t.takeException(), isNull);
      } finally {
        await _dispose(t, run);
      }
    },
  );
}

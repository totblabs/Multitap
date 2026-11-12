// Actual RemoteView/RemoteSession with synthetic relay/media and broker reads.
// No authenticated network, Windows window, display, clipboard or file access.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/stream_preparation.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_change_robustness_test.dart' show mockWindow;
import 'remote_session_test.dart' show SessionSocket, packet;
import 'viewport_session_test.dart' show ResizeMedia, startInfo;

DesktopMachine _snapshot({
  String id = 'preparation-vm',
  bool preparing = false,
}) => DesktopMachine(
  id: id,
  name: '준비 검사 VM',
  description: '',
  kind: MachineKind.vm,
  status: preparing ? MachineStatus.awaitingHost : MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: true,
  interactiveSession: true,
  streamPreparation: preparing ? StreamPreparation.maintenance : null,
  virtualMonitorControl: true,
  display: VirtualDisplayState(
    supported: true,
    activeCount: 1,
    stale: false,
    // Deliberately the same old Host observation on every HTTP read. HTTP
    // freshness does not manufacture a newer Host readiness observation.
    checkedAt: DateTime.utc(2026, 10, 27),
  ),
  monitors: const [RemoteMonitor(id: '0', width: 1280, height: 720)],
);

class _Stream {
  _Stream(String id) {
    session = RemoteSession(
      connection: BrokerConnection(
        sessionId: id,
        webSocketUrl: Uri.parse('wss://fixture.invalid/sessions/$id'),
        epochMedia: true,
        viewportProvisioning: true,
        reservation: BrokerSessionReservation(() async => releases++),
      ),
      token: 'synthetic-only',
      dialer: (_, _) async => socket,
      media: media,
    );
  }
  final socket = SessionSocket();
  final media = ResizeMedia();
  late final RemoteSession session;
  int releases = 0;

  Future<void> listening(WidgetTester tester) async {
    await _until(tester, () => socket.incoming.hasListener);
    expect(socket.pingInterval, const Duration(seconds: 10));
    expect(session.closed, isFalse);
  }

  Future<void> metadata(WidgetTester tester) async {
    await listening(tester);
    socket.incoming.add(jsonEncode(startInfo));
    await _until(tester, () => media.dimensions.length == 1);
    await tester.pump();
    expect(session.textureId, isNotNull);
    expect(
      session.frameReady,
      isFalse,
      reason: 'Metadata is not first decoded readiness.',
    );
  }

  Future<void> end(
    WidgetTester tester, {
    String type = 'stream.unavailable',
    String reason = 'host_maintenance',
  }) async {
    await listening(tester);
    socket.incoming.add(
      jsonEncode({
        'type': type,
        if (type == 'stream.unavailable')
          'sessionId': session.connection.sessionId,
        'reason': reason,
      }),
    );
  }

  Future<void> ready(WidgetTester tester) async {
    await metadata(tester);
    socket.incoming.add(packet());
    await tester.pump();
    await tester.pump();
    expect(session.frameReady, isTrue);
  }
}

class _Fixture {
  _Fixture({this.desktopId = 'preparation-vm'});
  final String desktopId;
  final streams = <_Stream>[];
  int connects = 0, reads = 0, takeovers = 0;
  bool preparing = false;
  Future<DesktopMachine> Function()? read;
  Future<RemoteSession> Function()? retry;

  _Stream create() {
    final stream = _Stream('$desktopId-session-${streams.length}');
    streams.add(stream);
    return stream;
  }

  Future<RemoteSession> connect() async {
    connects++;
    if (connects > 1 && retry != null) return retry!();
    return create().session;
  }

  Future<void> mount(
    WidgetTester tester, {
    bool prepareDisplays = false,
    SessionWindowActions? actions,
    bool embedded = false,
  }) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(960, 640);
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.view.resetPhysicalSize);
    await mockWindow(tester);
    addTearDown(() async {
      await tester.pumpWidget(const SizedBox.shrink());
      for (final stream in streams) {
        unawaited(stream.session.close());
      }
      await tester.pump(const Duration(seconds: 3));
      for (final stream in streams) {
        unawaited(stream.media.events.close());
      }
    });
    final view = RemoteView(
      machine: _snapshot(id: desktopId),
      load: connect,
      onReconnect: (_) => connect(),
      initialOptions: const StreamOptions(),
      initialDisplayMode: SessionDisplayMode.windowed,
      prepareDisplaysOnEntry: prepareDisplays,
      windowActions: actions,
      onRefreshDesktop: () {
        reads++;
        return read?.call() ??
            Future.value(_snapshot(id: desktopId, preparing: preparing));
      },
      onTakeover: () async => fail('No unbound legacy release is permitted'),
      onConfirmTakeover: (offer, authorized) async {
        expect(offer.desktopId, desktopId);
        expect(authorized(), isTrue);
        takeovers++;
      },
    );
    await tester.pumpWidget(
      MaterialApp(
        home: embedded
            ? Scaffold(
                body: Builder(
                  builder: (context) => TextButton(
                    key: const Key('open-embedded-preparation'),
                    onPressed: () => Navigator.of(context)
                        .push(MaterialPageRoute<void>(builder: (_) => view)),
                    child: const Text('검사 연결 열기'),
                  ),
                ),
              )
            : view,
      ),
    );
    if (embedded) {
      await tester.tap(find.byKey(const Key('open-embedded-preparation')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 350));
    }
    await _until(tester, () => streams.isNotEmpty);
    await streams.single.listening(tester);
    expect(streams.single.session.status, 'Host 화면 기다리는 중');
  }
}

Future<void> _until(WidgetTester tester, bool Function() done) async {
  for (var n = 0; n < 50 && !done(); n++) {
    await tester.pump(const Duration(milliseconds: 100));
  }
  expect(
    done(),
    isTrue,
    reason: 'Bounded synthetic preparation did not settle.',
  );
}

void main() {
  for (final type in ['stream.unavailable', 'session.ended']) {
    testWidgets(
      '$type before first frame waits for preparation then reconnects once',
      (tester) async {
        final fixture = _Fixture();
        await fixture.mount(tester, prepareDisplays: true);
        final first = fixture.streams.single;
        // Metadata alone is not a displayed/decoded first streamReady.
        await first.metadata(tester);
        fixture.preparing = true;
        await first.end(tester, type: type);
        await _until(tester, () => first.session.closed);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 600));
        expect(fixture.connects, 1);
        expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
        expect(find.text('Host가 가상 화면을 준비하고 있어요. 잠시 기다려 주세요.'), findsOneWidget);
        fixture.preparing = false;
        await _until(tester, () => fixture.connects == 2);
        final second = fixture.streams.last;
        await second.ready(tester);
        expect(first.releases, 1);
        expect(second.session.closed, isFalse);
        expect(fixture.takeovers, 0);
        expect(tester.takeException(), isNull);
      },
    );
  }

  testWidgets(
    'stale ready Host snapshot cannot replenish the single intent retry budget',
    (tester) async {
      final fixture = _Fixture();
      await fixture.mount(tester);
      final first = fixture.streams.single;
      await first.end(tester);
      await _until(tester, () => fixture.connects == 2);
      final second = fixture.streams.last;
      await second.end(tester);
      await _until(tester, () => second.session.closed);
      await tester.pump(const Duration(seconds: 4));
      expect(fixture.connects, 2);
      expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
      // Only a new explicit user intent replenishes the budget.
      await tester.tap(find.byKey(const Key('retry-remote-connection')));
      await _until(tester, () => fixture.connects == 3);
      await fixture.streams.last.end(tester);
      await _until(tester, () => fixture.connects == 4);
      expect(fixture.takeovers, 0);
    },
  );

  testWidgets('maintenance after first actual frame remains a manual end', (
    tester,
  ) async {
    final fixture = _Fixture();
    await fixture.mount(tester);
    final first = fixture.streams.single;
    await first.ready(tester);
    await first.end(tester);
    await _until(tester, () => first.session.closed);
    await tester.pump(const Duration(seconds: 2));
    expect(fixture.connects, 1);
    expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
  });

  testWidgets(
    'internal transport reload keeps the intent first-ready history',
    (tester) async {
      final fixture = _Fixture();
      await fixture.mount(tester);
      final first = fixture.streams.single;
      await first.ready(tester);
      await first.end(tester, type: 'session.ended', reason: 'host_error');
      await _until(tester, () => fixture.connects == 2);
      final second = fixture.streams.last;
      await second.end(tester);
      await _until(tester, () => second.session.closed);
      await tester.pump(const Duration(seconds: 2));
      expect(fixture.connects, 2);
      expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
    },
  );

  testWidgets(
    'an internal transport reload never replenishes the spent initial retry',
    (tester) async {
      final fixture = _Fixture();
      await fixture.mount(tester);
      await fixture.streams.single.end(tester);
      await _until(tester, () => fixture.connects == 2);
      await fixture.streams.last.end(
        tester,
        type: 'session.ended',
        reason: 'host_error',
      );
      await _until(tester, () => fixture.connects == 3);
      final third = fixture.streams.last;
      await third.end(tester);
      await _until(tester, () => third.session.closed);
      await tester.pump(const Duration(seconds: 2));
      expect(fixture.connects, 3);
      expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
    },
  );

  for (final reason in [
    'authorization_revoked',
    'taken_over',
    'host_revoked',
  ]) {
    testWidgets(
      'terminal $reason before first frame does not trigger preparation recovery',
      (tester) async {
        final fixture = _Fixture();
        await fixture.mount(tester);
        final first = fixture.streams.single;
        await first.end(tester, type: 'session.ended', reason: reason);
        await _until(tester, () => first.session.closed);
        await tester.pump(const Duration(seconds: 2));
        expect(fixture.connects, 1);
        expect(fixture.takeovers, 0);
      },
    );
  }

  for (final code in [
    'desktop_busy',
    'controller_in_use',
    'desktop_busy_no_offer',
  ]) {
    testWidgets('preparation retry $code never performs automatic takeover', (
      tester,
    ) async {
      final fixture = _Fixture();
      await fixture.mount(tester);
      final busy = code.startsWith('desktop_busy');
      final offered = code == 'desktop_busy';
      fixture.retry = () async => throw BrokerException(
        busy ? 'desktop_busy' : code,
        409,
        null,
        busy,
        offered
            ? StreamTakeoverOffer.parse({
                'protocol': streamTakeoverProtocol,
                'context': 'C' * 43,
                'expiresInSeconds': 120,
                'desktop': {'id': fixture.desktopId, 'name': '검사 VM'},
                'sessions': [
                  {
                    'username': 'fixture-user',
                    'createdAt': null,
                    'deviceLabel': null,
                  },
                ],
              })
            : null,
      );
      await fixture.streams.single.end(tester);
      await _until(tester, () => fixture.connects == 2);
      await tester.pump();
      await tester.pump(const Duration(seconds: 2));
      expect(fixture.takeovers, 0);
      expect(fixture.connects, 2);
      expect(
        find.byKey(
          Key(
            offered ? 'takeover-remote-connection' : 'retry-remote-connection',
          ),
        ),
        findsOneWidget,
      );
      if (offered) {
        // The card waits for its button: it names the connection it would
        // end and has sent nothing by itself.
        expect(find.text('연결 중인 계정 fixture-user'), findsOneWidget);
        expect(find.byType(Dialog), findsNothing);
        // Only that press takes the desktop over, once.
        await tester.tap(find.byKey(const Key('takeover-remote-connection')));
        await tester.pump();
        expect(find.byType(Dialog), findsNothing);
        expect(fixture.takeovers, 1);
      }
    });
  }

  testWidgets(
    'assignment revocation during fresh read stops without requesting a new session',
    (tester) async {
      final fixture = _Fixture();
      await fixture.mount(tester);
      final before = fixture.reads;
      fixture.read = () async =>
          throw const BrokerException('desktop_not_found', 404);
      await fixture.streams.single.end(tester);
      await _until(tester, () => fixture.reads > before);
      await tester.pump();
      await tester.pump(const Duration(seconds: 2));
      expect(fixture.connects, 1);
      expect(fixture.takeovers, 0);
      expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
    },
  );

  testWidgets('different VM snapshot never enters this preparation retry', (
    tester,
  ) async {
    final fixture = _Fixture();
    await fixture.mount(tester);
    final before = fixture.reads;
    fixture.read = () async => _snapshot(id: 'a-different-vm');
    await fixture.streams.single.end(tester);
    await _until(tester, () => fixture.reads > before);
    await tester.pump();
    expect(fixture.connects, 1);
    expect(fixture.takeovers, 0);
  });

  testWidgets(
    'close while readiness read is held fences its late ready result',
    (tester) async {
      final fixture = _Fixture();
      var closes = 0;
      final actions = SessionWindowActions(closeWindow: () async => closes++);
      await fixture.mount(tester, actions: actions);
      final read = Completer<DesktopMachine>();
      final before = fixture.reads;
      fixture.read = () => read.future;
      await fixture.streams.single.end(tester);
      await _until(tester, () => fixture.reads > before);
      await actions.retire();
      expect(closes, 1);
      read.complete(_snapshot());
      await tester.pump();
      await tester.pump(const Duration(seconds: 2));
      expect(fixture.connects, 1);
      expect(fixture.takeovers, 0);
      expect(tester.takeException(), isNull);
    },
  );

  testWidgets('read deadline stops polling and ignores a late HTTP snapshot', (
    tester,
  ) async {
    final fixture = _Fixture();
    await fixture.mount(tester);
    fixture.preparing = true;
    final before = fixture.reads;
    await fixture.streams.single.end(tester);
    await _until(tester, () => fixture.reads > before);
    expect(find.text('Host가 가상 화면을 준비하고 있어요. 잠시 기다려 주세요.'), findsOneWidget);
    final pending = Completer<DesktopMachine>();
    var heldReads = 0;
    addTearDown(() {
      if (!pending.isCompleted) {
        pending.complete(_snapshot(preparing: true));
      }
    });
    fixture.read = () {
      heldReads++;
      return pending.future;
    };
    await _until(tester, () => heldReads == 1);
    expect(
      fixture.reads,
      greaterThan(before + 1),
      reason: 'The observation read, not just the initial HTTP read, is held.',
    );
    expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
    await tester.pump(const Duration(seconds: 7));
    expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
    expect(pending.isCompleted, isFalse);
    await tester.pump(const Duration(milliseconds: 1100));
    await _until(
      tester,
      () => find
          .byKey(const Key('retry-remote-connection'))
          .evaluate()
          .isNotEmpty,
    );
    final reads = fixture.reads;
    expect(
      pending.isCompleted,
      isFalse,
      reason: 'The read deadline does not complete the underlying HTTP future.',
    );
    pending.complete(_snapshot());
    await tester.pump();
    await tester.pump(const Duration(seconds: 2));
    expect(fixture.connects, 1);
    expect(fixture.reads, reads);
    expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
  });

  testWidgets(
    'continuously preparing Host reaches the bounded wait deadline without a connection loop',
    (tester) async {
      final fixture = _Fixture();
      await fixture.mount(tester, prepareDisplays: true);
      fixture.preparing = true;
      final before = fixture.reads;
      await fixture.streams.single.end(tester);
      await _until(tester, () => fixture.reads > before + 1);
      expect(find.text('Host가 가상 화면을 준비하고 있어요. 잠시 기다려 주세요.'), findsOneWidget);
      expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
      // Waiting must genuinely be active, not the pre-existing manual end card.
      await tester.pump(const Duration(seconds: 25));
      expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
      expect(find.text('Host가 가상 화면을 준비하고 있어요. 잠시 기다려 주세요.'), findsOneWidget);
      await tester.pump(const Duration(seconds: 6));
      await _until(
        tester,
        () => find
            .byKey(const Key('retry-remote-connection'))
            .evaluate()
            .isNotEmpty,
      );
      expect(fixture.connects, 1);
      expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
      final reads = fixture.reads;
      await tester.pump(const Duration(seconds: 3));
      expect(fixture.reads, reads);
      expect(fixture.takeovers, 0);
    },
  );

  testWidgets(
    'connection timeout releases only its own late reservation after a replacement view starts',
    (tester) async {
      final fixture = _Fixture();
      await fixture.mount(tester);
      final pending = Completer<RemoteSession>();
      fixture.retry = () => pending.future;
      await fixture.streams.single.end(tester);
      await _until(tester, () => fixture.connects == 2);
      await tester.pump(const Duration(seconds: 21));
      expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
      final otherFixture = _Fixture(desktopId: 'another-vm');
      await otherFixture.mount(tester);
      final other = otherFixture.streams.single;
      final late = fixture.create();
      pending.complete(late.session);
      await tester.pump();
      await tester.pump();
      expect(late.session.closed, isTrue);
      expect(late.releases, 1);
      expect(other.session.closed, isFalse);
      expect(other.releases, 0);
    },
  );

  testWidgets(
    'foreign session unavailable is a protocol failure, not a preparation retry',
    (tester) async {
      final fixture = _Fixture();
      await fixture.mount(tester);
      final first = fixture.streams.single;
      // Demonstrate the same ordered reader has actually processed a valid
      // control message, while still before any decoded frame/streamReady.
      await first.metadata(tester);
      first.socket.incoming.add(
        jsonEncode({
          'type': 'stream.unavailable',
          'sessionId': 'a-different-session',
          'reason': 'host_maintenance',
        }),
      );
      // Cancellation can complete on the real event queue, which fake time alone
      // does not advance. Yield one bounded real turn without closing the source.
      await tester.pump();
      await tester.runAsync(() async {
        await Future<void>.delayed(Duration.zero)
            .timeout(const Duration(milliseconds: 500));
      });
      await _until(tester, () => first.session.closed);
      await tester.pump(const Duration(seconds: 2));
      expect(fixture.connects, 1);
      expect(first.session.diagnosticCode, 'invalid_stream_message');
    },
  );

  testWidgets('embedded pop fences a held ready result before route disposal', (
    tester,
  ) async {
    final fixture = _Fixture();
    await fixture.mount(tester, embedded: true);
    final first = fixture.streams.single;
    final remoteState = tester.state(find.byType(RemoteView));
    final modal = ModalRoute.of(remoteState.context);
    expect(modal, isNotNull);
    final route = modal!;
    expect(route.isCurrent, isTrue);
    expect(route.isActive, isTrue);
    final reverseDuration = route.reverseTransitionDuration;
    expect(
      reverseDuration.inMicroseconds,
      greaterThan(16000),
      reason: 'The 16ms late-response check must precede route completion.',
    );
    debugPrint('RECONNECT_ROUTE_REVERSE_MS ${reverseDuration.inMilliseconds}');
    final pending = Completer<DesktopMachine>();
    var heldReads = 0;
    addTearDown(() {
      if (!pending.isCompleted) {
        pending.complete(_snapshot(preparing: true));
      }
    });
    fixture.read = () {
      heldReads++;
      return pending.future;
    };
    await first.end(tester);
    await _until(tester, () => heldReads == 1);
    expect(fixture.connects, 1);
    expect(find.text('Host가 가상 화면을 준비하고 있어요. 잠시 기다려 주세요.'), findsOneWidget);
    expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
    await tester.tap(find.byKey(const Key('leave-remote-connection')));
    expect(
      remoteState.mounted,
      isTrue,
      reason: 'The reverse route transition has not disposed RemoteView yet.',
    );
    pending.complete(_snapshot());
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 16));
    expect(
      remoteState.mounted,
      isTrue,
      reason: 'The late HTTP response arrived during the pop animation.',
    );
    expect(
      fixture.connects,
      1,
      reason: 'Pop must fence before mounted becomes false.',
    );
    // MaterialPageRoute's effective duration is theme/platform dependent.
    // Let this route's actual reverse finish, then deliver its completion frame.
    await tester.pump(reverseDuration);
    await tester.pump();
    expect(remoteState.mounted, isFalse);
    expect(find.byKey(const Key('open-embedded-preparation')), findsOneWidget);
    expect(fixture.connects, 1);
    expect(fixture.takeovers, 0);
    expect(first.releases, 1);
    expect(tester.takeException(), isNull);
  });
}

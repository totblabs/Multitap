// Rapid display changes: window size / fullscreen / 모든 모니터 changes in
// quick succession must not end streams as display_change_failed. Older Hosts
// read the lossless opt-in an lt1- ping carries (or, for independent outputs,
// the ping itself) inside their display-change RPC wait and fail the change.
// Fixtures only: the broker socket, decoder and native display are fakes.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_remote_test.dart' show machine;
import 'profile_track_resize_test.dart' as r;
import 'profile_track_session_test.dart' show Fixture, fixturePacket;
import 'remote_session_test.dart' show SessionSocket, packet;
import 'viewport_session_test.dart'
    show ResizeMedia, startInfo, connection, resizeAck, applied;

/// Answers every ping at once, like Session0 does.
class PongSocket extends SessionSocket {
  @override
  void add(dynamic data) {
    super.add(data);
    final value = sent.last;
    if (value['type'] == 'ping') {
      incoming.add(jsonEncode({'type': 'pong', 'id': value['id']}));
    }
  }
}

int pingsAfter(SessionSocket socket, int index) =>
    socket.sent.skip(index).where((e) => e['type'] == 'ping').length;

class ViewFixture {
  ViewFixture(this.socket, this.media, this.session);
  final SessionSocket socket;
  final ResizeMedia media;
  final RemoteSession session;
  int epoch = 0;

  static RemoteSession build(SessionSocket socket, ResizeMedia media) =>
      RemoteSession(
        connection: BrokerConnection(
          sessionId: 'display-change-fixture',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/api/v1/sessions/display-change-fixture',
          ),
          epochMedia: true,
          viewportProvisioning: true,
          options: const StreamOptions(),
        ),
        token: 'fixture-only-not-a-real-token',
        media: media,
        dialer: (_, _) async => socket,
      );

  List<Map<String, dynamic>> get requests =>
      socket.sent.where((event) => event['type'] == 'stream.resize').toList();

  Future<void> initialFrame(WidgetTester tester) async {
    socket.incoming.add(jsonEncode(startInfo));
    await tester.pump();
    await tester.pump();
    socket.incoming.add(packet());
    await tester.pump();
    await tester.pump();
  }

  Future<void> waitFor(WidgetTester tester, int count) async {
    for (var n = 0; n < 25 && requests.length < count; n++) {
      await tester.pump(const Duration(milliseconds: 100));
      await tester.pump();
    }
    expect(requests, hasLength(count));
  }

  Future<Map<String, dynamic>> acknowledge(
    WidgetTester tester,
    Map<String, dynamic> request,
  ) async {
    final ack = <String, dynamic>{
      ...request,
      'type': 'stream.resizing',
      'resizeId': 'rapid-epoch-${++epoch}',
      'epoch': epoch,
      'timeoutMs': 90000,
    };
    socket.incoming.add(jsonEncode(ack));
    await tester.pump();
    await tester.pump();
    return ack;
  }

  Future<void> apply(WidgetTester tester, Map<String, dynamic> ack) async {
    socket.incoming.add(
      jsonEncode({
        ...startInfo,
        'sessionId': session.connection.sessionId,
        'requestId': ack['requestId'],
        'resizeId': ack['resizeId'],
        'epoch': ack['epoch'],
        'width': ack['width'],
        'height': ack['height'],
        'sourceWidth': ack['width'],
        'sourceHeight': ack['height'],
      }),
    );
    await tester.pump();
    await tester.pump();
    final frame = packet();
    frame[2] = epoch & 255;
    frame[3] = epoch >> 8;
    socket.incoming.add(frame);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 60));
  }

  /// The relay ends the session after a Host-side failure of a dispatched
  /// change (the broker's relay.mjs resizeFailed).
  Future<void> hostFails(
    WidgetTester tester,
    Map<String, dynamic> ack, {
    String reason = 'display_change_failed',
  }) async {
    socket.incoming.add(
      jsonEncode({
        'type': 'stream.resize.failed',
        'requestId': ack['requestId'],
        'resizeId': ack['resizeId'],
        'epoch': ack['epoch'],
        'reason': reason,
        'unchanged': false,
      }),
    );
    await tester.pump();
    await tester.pump();
  }
}

Future<void> mockWindow(WidgetTester tester) async {
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const window = MethodChannel('multitap/window');
  const events = MethodChannel('multitap/window/events');
  messenger.setMockMethodCallHandler(window, (_) async => false);
  messenger.setMockMethodCallHandler(events, (_) async => null);
  addTearDown(() {
    messenger.setMockMethodCallHandler(window, null);
    messenger.setMockMethodCallHandler(events, null);
  });
}

void main() {
  test('No ping is sent while the remote display changes (single output)', () async {
    final socket = PongSocket(), media = ResizeMedia();
    final session = RemoteSession(
      connection: connection,
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    socket.incoming.add(jsonEncode(startInfo));
    await Future<void>.delayed(const Duration(milliseconds: 5));
    socket.incoming.add(packet());
    // Ordinary pings keep their one-second cadence before the change.
    await Future<void>.delayed(const Duration(milliseconds: 2100));
    expect(pingsAfter(socket, 0), greaterThanOrEqualTo(2));
    final result = session.resizeViewport(1346, 812);
    await Future<void>.delayed(const Duration(milliseconds: 5));
    final requestAt = socket.sent.indexWhere((e) => e['type'] == 'stream.resize');
    final id = socket.sent[requestAt]['requestId'] as String;
    socket.incoming.add(jsonEncode(resizeAck(id)));
    // The Host's SDK/topology change takes a while; every ping in it used to
    // reach the Host's RPC wait.
    await Future<void>.delayed(const Duration(milliseconds: 2200));
    expect(session.resizing, isTrue);
    expect(pingsAfter(socket, requestAt), 0);
    socket.incoming.add(jsonEncode(applied(id)));
    await Future<void>.delayed(const Duration(milliseconds: 5));
    final fresh = packet();
    fresh[2] = 1;
    socket.incoming.add(fresh);
    expect(await result.timeout(const Duration(seconds: 3)), isTrue);
    final doneAt = socket.sent.length;
    await Future<void>.delayed(const Duration(milliseconds: 1100));
    expect(pingsAfter(socket, doneAt), greaterThanOrEqualTo(1));
    expect(session.closed, isFalse);
    await session.close();
    await running;
    await media.events.close();
  });

  test('No ping reaches the Host while independent outputs change', () async {
    final f = Fixture();
    await f.start();
    final changed = f.session.resizeTracks(r.nextPlan());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    final requestAt = f.socket.sent.indexWhere(
      (e) => e['type'] == 'stream.tracks.resize',
    );
    f.send(f.resizeAck());
    await Future<void>.delayed(const Duration(milliseconds: 2200));
    expect(f.session.resizing, isTrue);
    expect(pingsAfter(f.socket, requestAt), 0);
    f.send(f.info());
    await Future<void>.delayed(const Duration(milliseconds: 15));
    f.socket.incoming.add(fixturePacket(0, epoch: 2));
    f.socket.incoming.add(fixturePacket(1));
    expect(await changed.timeout(const Duration(seconds: 3)), isTrue);
    final doneAt = f.socket.sent.length;
    await Future<void>.delayed(const Duration(milliseconds: 1100));
    expect(pingsAfter(f.socket, doneAt), greaterThanOrEqualTo(1));
    await f.close();
  });

  test('A refused change keeps the stream; a failed dispatched change is marked as a display-change end', () async {
    for (final dispatched in [false, true]) {
      final socket = SessionSocket(), media = ResizeMedia();
      final session = RemoteSession(
        connection: connection,
        token: 'fixture-only',
        media: media,
        dialer: (_, _) async => socket,
      );
      final running = session.start();
      socket.incoming.add(jsonEncode(startInfo));
      await Future<void>.delayed(const Duration(milliseconds: 5));
      final result = session.resizeViewport(1346, 812);
      await Future<void>.delayed(const Duration(milliseconds: 5));
      final id =
          socket.sent.lastWhere((v) => v['type'] == 'stream.resize')['requestId']
              as String;
      if (dispatched) {
        socket.incoming.add(jsonEncode(resizeAck(id)));
        await Future<void>.delayed(const Duration(milliseconds: 5));
      }
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.resize.failed',
          'requestId': id,
          if (dispatched) 'resizeId': 'matching-resize-id',
          if (dispatched) 'epoch': 1,
          'reason': dispatched ? 'display_change_failed' : 'resize_rate_limited',
          'unchanged': !dispatched,
        }),
      );
      expect(await result, isFalse);
      await Future<void>.delayed(const Duration(milliseconds: 5));
      expect(session.closed, dispatched);
      expect(session.endedByDisplayChange, dispatched);
      if (dispatched) {
        expect(session.diagnosticCode, 'display_change_failed');
      } else {
        expect(session.resizeError, RemoteSession.keptSizeMessage);
        expect(session.textureId, isNotNull);
      }
      await session.close();
      await running;
      await media.events.close();
    }
  });

  testWidgets(
    '200 rapid window/fullscreen changes: one request at a time, the latest size wins, the stream never ends',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(800, 600);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await mockWindow(tester);
      final socket = SessionSocket(), media = ResizeMedia();
      final fixture = ViewFixture(
        socket,
        media,
        ViewFixture.build(socket, media),
      );
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            initialOptions: const StreamOptions(),
            initialDisplayMode: SessionDisplayMode.windowed,
            load: () async => fixture.session,
          ),
        ),
      );
      await tester.pump();
      await tester.pump();
      try {
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        await fixture.apply(
          tester,
          await fixture.acknowledge(tester, fixture.requests.single),
        );
        // Window drags, fullscreen on a 1080p/1440p monitor and a window
        // spanning two 1080p monitors, at the pace of a user who keeps
        // changing them while earlier changes are still being applied.
        const sizes = [
          Size(1024, 700),
          Size(1920, 1080),
          Size(1280, 720),
          Size(3840, 1080),
          Size(1600, 900),
          Size(2560, 1440),
          Size(1366, 768),
          Size(1920, 1080),
        ];
        var answered = 1;
        Map<String, dynamic>? inFlight;
        for (var change = 0; change < 200; change++) {
          tester.view.physicalSize = sizes[change % sizes.length];
          await tester.pump();
          // 40-939 ms: some pauses outlast the 650 ms debounce, so changes
          // keep arriving while earlier ones are applied.
          await tester.pump(Duration(milliseconds: 40 + (change * 137) % 900));
          // Never two outstanding requests.
          expect(fixture.requests.length - answered, lessThanOrEqualTo(1));
          if (fixture.requests.length > answered && inFlight == null) {
            inFlight = await fixture.acknowledge(
              tester,
              fixture.requests.last,
            );
          } else if (inFlight != null && change.isOdd) {
            await fixture.apply(tester, inFlight);
            inFlight = null;
            answered = fixture.requests.length;
          }
          expect(
            fixture.session.closed,
            isFalse,
            reason:
                'change $change: ${fixture.session.diagnosticCode} '
                '${fixture.session.error}',
          );
        }
        if (inFlight != null) {
          await fixture.apply(tester, inFlight);
          answered = fixture.requests.length;
        }
        tester.view.physicalSize = const Size(1440, 900);
        await tester.pump();
        for (var n = 0; n < 40; n++) {
          await tester.pump(const Duration(milliseconds: 100));
          if (fixture.requests.length > answered) {
            await fixture.apply(
              tester,
              await fixture.acknowledge(tester, fixture.requests.last),
            );
            answered = fixture.requests.length;
          }
        }
        // Changes were applied while others kept arriving, yet coalesced:
        // fewer requests than changes, and the last one is the final size.
        expect(fixture.requests.length, greaterThan(20));
        expect(fixture.requests.length, lessThan(200));
        expect(
          [fixture.requests.last['width'], fixture.requests.last['height']],
          [1440, 900],
        );
        expect(
          [fixture.session.sourceWidth, fixture.session.sourceHeight],
          [1440, 900],
        );
        expect(fixture.session.closed, isFalse);
        expect(fixture.session.error, isNull);
        expect(tester.takeException(), isNull);
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        unawaited(fixture.session.close());
        await tester.pump(const Duration(seconds: 3));
        unawaited(media.events.close());
      }
    },
  );

  testWidgets(
    'A refused change keeps streaming and says so once',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(800, 600);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await mockWindow(tester);
      final socket = SessionSocket(), media = ResizeMedia();
      final fixture = ViewFixture(
        socket,
        media,
        ViewFixture.build(socket, media),
      );
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            initialOptions: const StreamOptions(),
            initialDisplayMode: SessionDisplayMode.windowed,
            load: () async => fixture.session,
          ),
        ),
      );
      await tester.pump();
      await tester.pump();
      try {
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        socket.incoming.add(
          jsonEncode({
            'type': 'stream.resize.failed',
            'requestId': fixture.requests.single['requestId'],
            'unchanged': true,
            'reason': 'resize_rate_limited',
          }),
        );
        await tester.pump();
        await tester.pump();
        await tester.pump();
        expect(find.text(RemoteSession.keptSizeMessage), findsOneWidget);
        expect(fixture.session.closed, isFalse);
        expect(fixture.session.textureId, isNotNull);
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        unawaited(fixture.session.close());
        await tester.pump(const Duration(seconds: 3));
        unawaited(media.events.close());
      }
    },
  );

  testWidgets(
    'A Host-side change failure that ends the stream reconnects once by itself; a second one in a row leaves the card',
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
      var reconnects = 0;
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            initialOptions: const StreamOptions(),
            initialDisplayMode: SessionDisplayMode.windowed,
            load: () async => first.session,
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
        await first.waitFor(tester, 1);
        await first.hostFails(
          tester,
          await first.acknowledge(tester, first.requests.single),
        );
        expect(first.session.closed, isTrue);
        expect(first.session.endedByDisplayChange, isTrue);
        for (var n = 0; n < 20 && reconnects == 0; n++) {
          await tester.pump(const Duration(milliseconds: 100));
        }
        expect(reconnects, 1);
        expect(find.text('화면 크기를 바꾸지 못해 다시 연결하고 있어요'), findsOneWidget);
        final second = fixtures.last;
        await tester.pump();
        await second.initialFrame(tester);
        await second.waitFor(tester, 1);
        expect(second.session.closed, isFalse);
        // The same failure again right away: no loop, the ordinary card.
        await second.hostFails(
          tester,
          await second.acknowledge(tester, second.requests.single),
        );
        await tester.pump(const Duration(seconds: 2));
        await tester.pump();
        expect(reconnects, 1);
        expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
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
}

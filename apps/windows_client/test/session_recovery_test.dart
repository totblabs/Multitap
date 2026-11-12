import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_failure.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/remote_viewport.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;

class ManualClock extends Fake implements Stopwatch {
  int now = 0;
  @override
  int get elapsedMicroseconds => now;
}

class RecoveryMedia extends SessionMedia {
  @override
  Future<int> start(int width, int height) async {
    starts++;
    return 42;
  }
}

class StalledMedia extends RecoveryMedia {
  bool stallCleanup = false;
  int? reportedFrames;
  @override
  Future<bool> capture(bool enabled) =>
      stallCleanup ? Completer<bool>().future : super.capture(enabled);
  @override
  Future<void> stop() => stallCleanup ? Completer<void>().future : super.stop();
  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    if (reportedFrames != null) 'decodedFrames': reportedFrames,
  };
}

class DelayedCaptureMedia extends RecoveryMedia {
  final pendingCapture = Completer<bool>();
  @override
  Future<bool> capture(bool enabled) =>
      enabled ? pendingCapture.future : super.capture(enabled);
}

const selectedOptions = StreamOptions(
  maxWidth: 1280,
  maxHeight: 720,
  fps: 30,
  bitrate: 8000000,
  audio: false,
);
const machine = DesktopMachine(
  id: 'qa',
  name: '연결 검사 VM',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.ready,
  specification: 'Windows',
  accent: Colors.blue,
);
RemoteSession session(
  SessionSocket socket,
  SessionMedia media, {
  Stopwatch? clock,
}) => RemoteSession(
  connection: BrokerConnection(
    sessionId: 'qa-session',
    webSocketUrl: Uri.parse(
      'wss://cloud.example.com/api/v1/sessions/qa-session',
    ),
    options: selectedOptions,
  ),
  token: 'fixture-only-token',
  media: media,
  clock: clock,
  dialer: (_, _) async => socket,
);
Future<void> connected(
  WidgetTester t,
  RemoteSession session,
  SessionSocket socket,
) async {
  unawaited(session.start());
  await t.pump();
  socket.incoming.add(jsonEncode(info));
  await t.pump();
  expect(
    session.closed,
    isFalse,
    reason: '${session.error} / ${session.diagnosticCode}',
  );
  socket.incoming.add(packet());
  for (var i = 0; i < 10 && session.receivedBytes == 0; i++) {
    await t.pump(const Duration(milliseconds: 1));
  }
  expect(
    session.receivedBytes,
    packet().length,
    reason:
        'width=${session.width} texture=${session.textureId} closed=${session.closed} error=${session.error} code=${session.diagnosticCode} paused=${socket.incoming.isPaused}',
  );
  expect(session.closed, isFalse, reason: session.error);
}

void mockPresentation() {
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  for (final name in ['multitap/window', 'multitap/window/events']) {
    final channel = MethodChannel(name);
    messenger.setMockMethodCallHandler(channel, (_) async => false);
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
  }
}

Future<void> openView(
  WidgetTester t,
  Future<RemoteSession> Function() load, {
  Future<RemoteSession> Function(StreamOptions)? retry,
  Future<void> Function()? takeover,
  Future<void> Function(StreamTakeoverOffer, bool Function())?
  confirmedTakeover,
}) async {
  mockPresentation();
  await t.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(),
      home: Builder(
        builder: (context) => Scaffold(
          body: TextButton(
            onPressed: () => Navigator.push(
              context,
              MaterialPageRoute<void>(
                builder: (_) => RemoteView(
                  machine: machine,
                  initialOptions: selectedOptions,
                  load: load,
                  onReconnect: retry,
                  onTakeover: takeover,
                  onConfirmTakeover: confirmedTakeover,
                ),
              ),
            ),
            child: const Text('데스크톱 목록'),
          ),
        ),
      ),
    ),
  );
  await t.tap(find.text('데스크톱 목록'));
  await t.pumpAndSettle();
}

void main() {
  testWidgets(
    'A delayed capture acknowledgement cannot recapture input after focus loss',
    (t) async {
      final socket = SessionSocket(), media = DelayedCaptureMedia();
      final current = session(socket, media);
      await connected(t, current, socket);
      final capturing = current.capture(true);
      final releasing = current.capture(false);
      expect(socket.sent.last['type'], 'input.reset');
      expect(current.captured, isFalse);
      await releasing;
      media.pendingCapture.complete(true);
      await capturing;
      expect(current.captured, isFalse);
      await current.close();
      await media.events.close();
    },
  );

  testWidgets(
    'Duplicate starts share one controller and a closed session cannot restart',
    (t) async {
      final socket = SessionSocket(), media = RecoveryMedia();
      final current = session(socket, media);
      final first = current.start(), second = current.start();
      expect(identical(first, second), isTrue);
      await t.pump();
      await current.close();
      await first;
      await current.start();
      expect(current.closed, isTrue);
      expect(media.starts, 0);
      await media.events.close();
    },
  );

  testWidgets(
    'A timed-out streaming allocation arriving late is closed without claiming the input engine',
    (t) async {
      mockPresentation();
      final pending = Completer<RemoteSession>(),
          socket = SessionSocket(),
          media = RecoveryMedia();
      final late = session(socket, media);
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: RemoteView(machine: machine, load: () => pending.future),
        ),
      );
      await t.pump();
      await t.pump(const Duration(seconds: 21));
      await t.pump();
      expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
      pending.complete(late);
      await t.pump();
      await t.pump();
      expect(late.closed, isTrue);
      expect(media.starts, 0);
      await t.pumpWidget(const SizedBox.shrink());
      await late.close();
      await media.events.close();
    },
  );

  test('Revocation blocks retry and unknown peer text never enters the failure message', () {
    expect(SessionFailure.ended('authorization_revoked').canRetry, isFalse);
    expect(SessionFailure.ended('host_revoked').canRetry, isFalse);
    expect(
      SessionFailure.request(const BrokerException('unexpected', 401)).canRetry,
      isFalse,
    );
    expect(
      SessionFailure.request(const BrokerException('desktop_not_found', 404))
          .canRetry,
      isFalse,
    );
    expect(SessionFailure.ended('host_timeout').canRetry, isTrue);
    expect(
      SessionFailure.ended('sensitive-peer-text').message,
      isNot(contains('sensitive-peer-text')),
    );
    expect(
      SessionFailure.request(const SocketException('sensitive-peer-text'))
          .message,
      isNot(contains('sensitive-peer-text')),
    );
  });

  testWidgets(
    'A hung local capture/stop cannot retain the authorized controller on disconnect',
    (t) async {
      final socket = SessionSocket(), media = StalledMedia();
      final current = session(socket, media);
      await connected(t, current, socket);
      await current.capture(true);
      media.stallCleanup = true;
      final closing = current.close();
      var done = false;
      unawaited(closing.then((_) => done = true));
      expect(current.closed, isTrue);
      expect(current.captured, isFalse);
      expect(
        socket.sent.where((m) => m['type'] == 'session.stop'),
        hasLength(1),
      );
      expect(socket.sent.where((m) => m['type'] == 'input.reset'), isNotEmpty);
      await t.pump(const Duration(seconds: 2));
      await t.pump();
      expect(done, isTrue);
      expect(socket.wasClosed, isTrue);
      await media.events.close();
    },
  );

  testWidgets(
    'An audio channel and heartbeat cannot hide a stalled video feed',
    (t) async {
      final socket = SessionSocket(),
          media = RecoveryMedia(),
          clock = ManualClock();
      final current = session(socket, media, clock: clock);
      await connected(t, current, socket);
      clock.now = 16000001;
      socket.incoming.add(Uint8List(16)..[0] = 2);
      await t.pump();
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      expect(current.closed, isTrue);
      expect(current.diagnosticCode, 'video_timeout', reason: current.error);
      await current.close();
      await media.events.close();
    },
  );

  testWidgets(
    'Encoded packets arriving without decoded progress fail instead of leaving a black frozen surface',
    (t) async {
      final socket = SessionSocket(),
          media = StalledMedia()..reportedFrames = 0,
          clock = ManualClock();
      final current = session(socket, media, clock: clock);
      await connected(t, current, socket);
      clock.now = 1000000;
      await t.pump(const Duration(seconds: 1));
      final ping = socket.sent.lastWhere((m) => m['type'] == 'ping');
      socket.incoming.add(jsonEncode({'type': 'pong', 'id': ping['id']}));
      await t.pump();
      clock.now = 17000000;
      socket.incoming.add(packet());
      await t.pump();
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      expect(current.closed, isTrue);
      expect(
        current.diagnosticCode,
        'initial_frame_timeout',
        reason: current.error,
      );
      await current.close();
      await media.events.close();
    },
  );

  testWidgets(
    'Late timer polling checks current decoded progress before declaring a stall',
    (t) async {
      final socket = SessionSocket(),
          media = RecoveryMedia(),
          clock = ManualClock();
      final current = session(socket, media, clock: clock);
      await connected(t, current, socket);
      clock.now = 17000000;
      socket.incoming.add(packet());
      await t.pump();
      await t.pump(const Duration(seconds: 1));
      expect(current.closed, isFalse, reason: current.error);
      await current.close();
      await media.events.close();
    },
  );

  testWidgets(
    'The server stream refusal is preserved instead of replaced by a generic session-ended label',
    (t) async {
      final socket = SessionSocket(), media = RecoveryMedia();
      final current = session(socket, media);
      unawaited(current.start());
      await t.pump();
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.unavailable',
          'sessionId': 'qa-session',
          'reason': 'no_interactive_session',
        }),
      );
      await t.pump();
      await t.pump();
      expect(current.closed, isTrue);
      expect(current.error, contains('Console로 열어 로그인하세요'));
      expect(current.diagnosticCode, 'no_interactive_session');
      await current.close();
      await media.events.close();
    },
  );

  testWidgets(
    'An initial connection error can retry with a fresh request and the chosen stream options',
    (t) async {
      final socket = SessionSocket(), media = RecoveryMedia();
      final next = session(socket, media);
      var retries = 0;
      final pending = Completer<RemoteSession>();
      await openView(
        t,
        () => Future.error(const SocketException('fixture')),
        retry: (options) {
          retries++;
          expect(options.toJson(), selectedOptions.toJson());
          return pending.future;
        },
      );
      expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
      await t.tap(find.byKey(const Key('retry-remote-connection')));
      await t.pump();
      expect(retries, 1);
      expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
      pending.complete(next);
      await t.pump();
      socket.incoming.add(jsonEncode(info));
      await t.pump();
      await t.pump();
      expect(find.byType(RemoteViewport), findsNothing);
      socket.incoming.add(packet());
      await t.pump();
      await t.pump();
      expect(find.byType(RemoteViewport), findsOneWidget);
      await next.close();
      await t.pumpWidget(const SizedBox.shrink());
      await t.pump();
      await media.events.close();
    },
  );

  testWidgets(
    'A busy VM card releases the exact offered connection on its one press, then reconnects',
    (t) async {
      final socket = SessionSocket(), media = RecoveryMedia();
      final next = session(socket, media);
      var takeovers = 0, retries = 0;
      final pending = Completer<RemoteSession>();
      await openView(
        t,
        () => Future.error(
          BrokerException(
            'desktop_busy',
            409,
            'controller_in_use',
            true,
            StreamTakeoverOffer.parse({
              'protocol': streamTakeoverProtocol,
              'context': 'C' * 43,
              'expiresInSeconds': 120,
              'desktop': {'id': 'qa', 'name': '연결 검사 VM'},
              'sessions': [
                {'username': 'alice', 'createdAt': null, 'deviceLabel': null},
              ],
            }),
          ),
        ),
        retry: (_) {
          retries++;
          return pending.future;
        },
        confirmedTakeover: (offer, authorized) async {
          expect(offer.desktopId, 'qa');
          expect(authorized(), isTrue);
          takeovers++;
        },
      );
      expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
      // The card itself says what the button does.
      expect(find.textContaining('그쪽 연결을 끊고 여기서 이어서'), findsOneWidget);
      expect(find.text('끊고 여기서 연결'), findsOneWidget);
      // ...and whose connection it ends. Nothing is sent before the press.
      expect(find.text('연결 중인 계정 alice'), findsOneWidget);
      expect(takeovers, 0);
      expect(retries, 0);
      // The card is the one question: its button releases, no dialog between.
      await t.tap(find.byKey(const Key('takeover-remote-connection')));
      expect(find.byType(Dialog), findsNothing);
      for (var i = 0; i < 4; i++) {
        await t.pump();
      }
      expect(takeovers, 1);
      expect(retries, 1);
      expect(find.byKey(const Key('takeover-remote-connection')), findsNothing);
      pending.complete(next);
      await t.pump();
      socket.incoming.add(jsonEncode(info));
      socket.incoming.add(packet());
      await t.pump();
      await t.pump();
      await t.pump();
      expect(find.byType(RemoteViewport), findsOneWidget);
      await next.close();
      await t.pumpWidget(const SizedBox.shrink());
      await t.pump();
      await media.events.close();
    },
  );

  testWidgets(
    'Another user\'s connection and released brokers keep the plain retry',
    (t) async {
      await openView(
        t,
        () => Future.error(
          const BrokerException(
            'desktop_busy',
            409,
            'controller_in_use',
            false,
          ),
        ),
        retry: (_) => Completer<RemoteSession>().future,
        takeover: () async => fail('not allowed'),
      );
      expect(find.byKey(const Key('takeover-remote-connection')), findsNothing);
      expect(find.byKey(const Key('retry-remote-connection')), findsOneWidget);
      expect(find.textContaining('다른 사용자가 이 데스크톱을'), findsOneWidget);
      expect(
        const BrokerException('desktop_busy', 409, 'controller_in_use').message,
        contains('그 연결을 끊은 뒤'),
      );
      expect(SessionFailure.ended('taken_over').code, 'taken_over');
      expect(
        SessionFailure.ended('taken_over').message,
        contains('다른 창이나 기기에서'),
      );
    },
  );

  testWidgets(
    'Authorization errors return to the desktop list without offering a stale retry',
    (t) async {
      await openView(
        t,
        () => Future.error(const BrokerException('session_expired', 401)),
      );
      expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
      await t.tap(find.byKey(const Key('leave-remote-connection')));
      await t.pumpAndSettle();
      expect(find.text('데스크톱 목록'), findsOneWidget);
    },
  );

  testWidgets(
    'A connection returned after leaving the view is closed without starting native media',
    (t) async {
      final pending = Completer<RemoteSession>(),
          socket = SessionSocket(),
          media = RecoveryMedia();
      final late = session(socket, media);
      mockPresentation();
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: RemoteView(machine: machine, load: () => pending.future),
        ),
      );
      await t.pump();
      await t.pumpWidget(const SizedBox.shrink());
      pending.complete(late);
      await t.pump();
      await t.pump();
      expect(late.closed, isTrue);
      expect(media.starts, 0);
      expect(media.stopped, isTrue);
      await late.close();
      await media.events.close();
    },
  );
}

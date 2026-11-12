// session-resume-v1 (session_resume.dart, Broker session-resume.mjs): a lost
// socket is replaced with the broker's single-use token and the stream goes
// on; anything the broker ended on purpose ends as before. Sockets, media and
// clocks are fakes; nothing touches a real display or network.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_resume.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_change_robustness_test.dart' show mockWindow;
import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionSocket, packet;
import 'session_recovery_test.dart'
    show ManualClock, RecoveryMedia, connected, selectedOptions;

const sessionId = 'qa-session';
final tokenA = 'A' * 43, tokenB = 'B' * 43;

class Dials {
  final calls = <String>[];
  final sockets = <SessionSocket>[];
  Object? failure;
  Future<WebSocket> call(Uri url, String token, String resumeToken) async {
    // No expect() here: it runs inside the tester's pump.
    if (url.path != '/api/v1/sessions/$sessionId' ||
        token != 'fixture-only-token') {
      calls.add('wrong target');
    }
    calls.add(resumeToken);
    final error = failure;
    if (error != null) throw error;
    final socket = SessionSocket();
    sockets.add(socket);
    return socket;
  }
}

RemoteSession resumable(
  SessionSocket socket,
  Dials dials, {
  bool granted = true,
  Stopwatch? clock,
}) => RemoteSession(
  connection: BrokerConnection(
    sessionId: sessionId,
    webSocketUrl: Uri.parse(
      'wss://cloud.example.com/api/v1/sessions/$sessionId',
    ),
    options: selectedOptions,
    sessionResume: granted,
    resumeGraceMs: 15000,
  ),
  token: 'fixture-only-token',
  media: RecoveryMedia(),
  clock: clock,
  dialer: (_, _) async => socket,
  resumeDialer: dials.call,
);

String message(String type, [String? token]) => jsonEncode({
  'type': type,
  'sessionId': sessionId,
  'token': ?token,
  if (token != null) 'graceMs': 15000,
});

Future<void> streaming(
  WidgetTester t,
  RemoteSession session,
  SessionSocket socket, {
  String? token,
}) async {
  await connected(t, session, socket);
  if (token != null) {
    socket.incoming.add(message('session.resume.token', token));
  }
  for (var n = 0; n < 10 && !session.frameReady; n++) {
    await t.pump(const Duration(milliseconds: 1));
  }
  expect(session.frameReady, isTrue, reason: session.error);
}

Future<void> settle(WidgetTester t) async {
  for (var n = 0; n < 20; n++) {
    await t.pump(const Duration(milliseconds: 1));
  }
}

// One tick of the session's one-second watchdog at the manual clock's time.
Future<void> tick(WidgetTester t, ManualClock clock, int seconds) async {
  for (var n = 0; n < seconds; n++) {
    clock.now += 1000000;
    await t.pump(const Duration(seconds: 1));
    await settle(t);
  }
}

void main() {
  test(
    'Only the broker marker advertises resume; tokens and windows are bounded',
    () {
      expect(
        sessionResumeAdvertised({'sessionResumeProtocol': 'session-resume-v1'}),
        isTrue,
      );
      expect(
        sessionResumeAdvertised({'sessionResumeProtocol': 'session-resume-v2'}),
        isFalse,
      );
      expect(sessionResumeAdvertised(null), isFalse);
      expect(resumeToken(tokenA), tokenA);
      for (final bad in [null, 42, 'short', '${'A' * 42}=', 'A' * 44]) {
        expect(resumeToken(bad), isNull, reason: '$bad');
      }
      expect(validResumeGraceMs(15000), 15000);
      for (final bad in [999, 120001, 1.5, '15000', null]) {
        expect(validResumeGraceMs(bad), isNull, reason: '$bad');
      }
    },
  );

  test(
    'A grant carries its window; an unrequested or malformed grant is refused',
    () {
      final origin = Uri.parse('https://cloud.example.com');
      const id = 'f0e1d2c3-b4a5-4697-8879-6a5b4c3d2e1f';
      Map<String, dynamic> grant([Map<String, dynamic> extra = const {}]) => {
        'sessionId': id,
        'webSocketUrl': 'wss://cloud.example.com/api/v1/sessions/$id',
        'mode': 'stream',
        'transport': 'media-v1',
        ...extra,
      };
      const both = {
        'sessionResumeProtocol': 'session-resume-v1',
        'resumeGraceMs': 15000,
      };
      final granted = BrokerConnection.fromJson(
        grant(both),
        origin,
        requestedSessionResume: true,
      );
      expect(granted.sessionResume, isTrue);
      expect(granted.resumeGraceMs, 15000);
      expect(
        BrokerConnection.fromJson(
          grant(),
          origin,
          requestedSessionResume: true,
        ).sessionResume,
        isFalse,
      );
      expect(
        () => BrokerConnection.fromJson(grant(both), origin),
        throwsA(isA<BrokerException>()),
        reason: 'A grant this request did not ask for',
      );
      for (final bad in [
        {'sessionResumeProtocol': 'session-resume-v1'},
        {'sessionResumeProtocol': 'session-resume-v2', 'resumeGraceMs': 15000},
        {'resumeGraceMs': 15000},
        {'sessionResumeProtocol': 'session-resume-v1', 'resumeGraceMs': 500},
      ]) {
        expect(
          () => BrokerConnection.fromJson(
            grant(bad),
            origin,
            requestedSessionResume: true,
          ),
          throwsA(isA<BrokerException>()),
          reason: '$bad',
        );
      }
    },
  );

  testWidgets(
    'A lost socket is replaced with the broker token and the stream goes on',
    (t) async {
      final first = SessionSocket(), dials = Dials();
      final session = resumable(first, dials);
      await streaming(t, session, first, token: tokenA);
      final bytes = session.receivedBytes;
      unawaited(first.incoming.close()); // dropped without a close frame
      await settle(t);
      expect(
        dials.calls,
        [tokenA],
        reason:
            'closed=${session.closed} code=${session.diagnosticCode} error=${session.error}',
      );
      expect(session.closed, isFalse, reason: session.error);
      expect(
        first.wasClosed,
        isTrue,
        reason: 'The replaced socket stayed open',
      );
      final second = dials.sockets.single;
      second.incoming.add(message('session.resumed', tokenB));
      second.incoming.add(packet());
      await settle(t);
      expect(session.receivedBytes, greaterThan(bytes));
      expect(session.resuming, isFalse);
      // The watchdog's ping goes out on the new socket.
      await t.pump(const Duration(seconds: 1));
      await settle(t);
      expect(second.sent.any((event) => event['type'] == 'ping'), isTrue);
      // The next loss uses the rotated token.
      unawaited(second.incoming.close());
      await settle(t);
      expect(dials.calls, [tokenA, tokenB]);
      expect(session.closed, isFalse);
      await session.close();
      await settle(t);
    },
  );

  testWidgets(
    'A deliberate end, a refused resume, no grant or no token end the session as before',
    (t) async {
      {
        // The broker closed the socket with its reason: no resume.
        final socket = SessionSocket()..closeReason = 'client_backpressure';
        final dials = Dials(), session = resumable(socket, dials);
        await streaming(t, session, socket, token: tokenA);
        unawaited(socket.incoming.close());
        await settle(t);
        expect(dials.calls, isEmpty);
        expect(session.closed, isTrue);
        expect(session.diagnosticCode, 'client_backpressure');
      }
      {
        // The broker refused the token: one attempt, then the ordinary failure.
        final socket = SessionSocket(),
            dials = Dials()..failure = const WebSocketException('refused', 401);
        final session = resumable(socket, dials);
        await streaming(t, session, socket, token: tokenA);
        unawaited(socket.incoming.close());
        await settle(t);
        expect(dials.calls, [tokenA]);
        expect(session.closed, isTrue);
        expect(session.diagnosticCode, 'relay_closed');
      }
      {
        // A connection without the grant ignores a token and never resumes.
        final socket = SessionSocket(), dials = Dials();
        final session = resumable(socket, dials, granted: false);
        await streaming(t, session, socket, token: tokenA);
        unawaited(socket.incoming.close());
        await settle(t);
        expect(dials.calls, isEmpty);
        expect(session.closed, isTrue);
      }
      {
        // No token yet: nothing to resume with.
        final socket = SessionSocket(), dials = Dials();
        final session = resumable(socket, dials);
        await streaming(t, session, socket);
        unawaited(socket.incoming.close());
        await settle(t);
        expect(dials.calls, isEmpty);
        expect(session.closed, isTrue);
      }
    },
  );

  testWidgets(
    'While the Host is away the picture stays and nothing times out',
    (t) async {
      final clock = ManualClock(), socket = SessionSocket(), dials = Dials();
      final session = resumable(socket, dials, clock: clock);
      await streaming(t, session, socket, token: tokenA);
      socket.incoming.add(message('session.resuming'));
      await settle(t);
      expect(session.resuming, isTrue);
      await tick(t, clock, 30);
      expect(session.closed, isFalse, reason: session.diagnosticCode);
      expect(dials.calls, isEmpty, reason: 'An open socket was replaced');
      socket.incoming.add(message('session.resumed'));
      await settle(t);
      expect(session.resuming, isFalse);
      await tick(t, clock, 10);
      expect(session.closed, isFalse, reason: session.diagnosticCode);
      // Afterwards the ordinary watchdogs apply again (the silence first
      // replaces the socket once, then the video timeout ends the stream).
      await tick(t, clock, 15);
      expect(session.closed, isTrue);
      expect(dials.calls, [tokenA]);
    },
  );

  testWidgets('A silent socket is replaced once while the stream waits', (
    t,
  ) async {
    final clock = ManualClock(), socket = SessionSocket(), dials = Dials();
    final session = resumable(socket, dials, clock: clock);
    await streaming(t, session, socket, token: tokenA);
    await tick(t, clock, 4);
    expect(dials.calls, isEmpty, reason: 'Resumed before the silence bound');
    await tick(t, clock, 3);
    expect(dials.calls, [tokenA]);
    expect(session.closed, isFalse, reason: session.error);
    expect(socket.wasClosed, isTrue);
    await session.close();
    await settle(t);
  });

  testWidgets(
    'The remote window shows one short line while the stream resumes',
    (t) async {
      t.view.physicalSize = const Size(800, 600);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      await mockWindow(t);
      final socket = SessionSocket(), dials = Dials();
      final session = resumable(socket, dials);
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(machine: machine(1), load: () async => session),
        ),
      );
      await t.pump();
      await t.pump();
      try {
        socket.incoming.add(message('session.resume.token', tokenA));
        socket.incoming.add(
          jsonEncode({
            'type': 'stream.info',
            'codec': 'h264',
            'width': 1280,
            'height': 720,
          }),
        );
        await settle(t);
        socket.incoming.add(packet());
        await settle(t);
        expect(session.frameReady, isTrue, reason: session.error);
        expect(find.byKey(const Key('resume-veil')), findsNothing);
        socket.incoming.add(message('session.resuming'));
        await t.pump(const Duration(milliseconds: 300));
        await settle(t);
        expect(find.byKey(const Key('resume-veil')), findsOneWidget);
        expect(find.text('다시 연결 중'), findsOneWidget);
        socket.incoming.add(message('session.resumed'));
        await settle(t);
        expect(find.byKey(const Key('resume-veil')), findsNothing);
      } finally {
        await t.pumpWidget(const SizedBox());
        unawaited(session.close());
        await t.pump(const Duration(seconds: 3));
      }
    },
  );
}

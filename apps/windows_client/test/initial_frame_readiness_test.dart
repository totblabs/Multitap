// Relay/decoder fixture coverage only, not physical screen presentation proof.
import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, packet, info;

class _Clock extends Stopwatch {
  int now = 0;
  @override
  int get elapsedMicroseconds => now;
}

class _Media extends SessionMedia {
  int decoded = 0, error = 0;
  Completer<Map<String, dynamic>>? delayed;
  @override
  Future<int> start(int width, int height) async {
    starts++;
    return 42;
  }

  @override
  Future<Map<String, dynamic>> stats() async =>
      delayed?.future ??
      Future.value({
        'decodedFrames': decoded,
        'error': error,
        'audioPlayedFrames': 0,
      });
}

class _Fixture {
  final socket = SessionSocket(), media = _Media(), clock = _Clock();
  late final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: 'first-frame',
      webSocketUrl: Uri.parse('wss://fixture.invalid/session'),
    ),
    token: 'fixture-only',
    media: media,
    clock: clock,
    dialer: (_, _) async => socket,
  );
  Future<void> step(WidgetTester t, [int milliseconds = 0]) async {
    clock.now += milliseconds * 1000;
    await t.pump(Duration(milliseconds: milliseconds));
    await t.pump();
  }

  Future<void> start(WidgetTester t) async {
    unawaited(session.start());
    await step(t);
    socket.incoming.add(jsonEncode(info));
    await step(t);
    await step(t);
  }

  Future<void> close(WidgetTester t) async {
    unawaited(session.close());
    await step(t);
    unawaited(media.events.close());
    await step(t);
  }
}

void main() {
  testWidgets(
    'initial texture, accepted packets and stale decoder counters cannot claim a usable desktop',
    (t) async {
      final f = _Fixture();
      try {
        await f.start(t);
        expect(
          f.session.textureId,
          42,
          reason:
              '${f.session.status} / ${f.session.error} / ${f.session.diagnosticCode}',
        );
        expect(f.session.frameReady, isFalse);
        expect(f.session.status, contains('첫 화면'));
        await f.session.capture(true);
        expect(f.session.captured, isFalse);
        f.media.decoded = 8;
        f.socket.incoming.add(packet()..[1] = 0);
        await f.step(t, 1000);
        expect(f.media.packets, isEmpty);
        expect(f.session.frameReady, isFalse);
        f.media.decoded = 0;
        f.socket.incoming.add(packet());
        await f.step(t);
        expect(f.media.packets, hasLength(1));
        expect(f.session.frameReady, isFalse);
        f.media.decoded = 1;
        await f.step(t, 60);
        expect(f.session.frameReady, isTrue);
        expect(f.session.status, '연결됨');
        await f.session.capture(true);
        expect(f.session.captured, isTrue);
      } finally {
        await f.close(t);
      }
    },
  );

  testWidgets(
    'late first-frame decoder result after deadline never admits input',
    (t) async {
      final f = _Fixture();
      try {
        await f.start(t);
        f.media.delayed = Completer<Map<String, dynamic>>();
        f.socket.incoming.add(packet());
        await f.step(t);
        // Advance the media deadline without firing the separate three-second
        // channel timeout: this exercises a successful but late decoder reply.
        f.clock.now = 16000000;
        f.media.delayed!.complete({'decodedFrames': 1, 'error': 0});
        await f.step(t);
        expect(f.session.frameReady, isFalse);
        expect(f.session.closed, isTrue);
        expect(f.session.diagnosticCode, 'initial_frame_timeout');
        await f.session.capture(true);
        expect(f.session.captured, isFalse);
      } finally {
        await f.close(t);
      }
    },
  );

  testWidgets(
    'close during first-frame decode cannot be undone by a stale success callback',
    (t) async {
      final f = _Fixture();
      try {
        await f.start(t);
        f.media.delayed = Completer<Map<String, dynamic>>();
        f.socket.incoming.add(packet());
        await f.step(t);
        await f.close(t);
        f.media.delayed!.complete({'decodedFrames': 1, 'error': 0});
        await f.step(t);
        expect(f.session.frameReady, isFalse);
        expect(f.session.closed, isTrue);
      } finally {
        if (!f.session.closed) await f.close(t);
      }
    },
  );
}

// Protocol and decoder-boundary regression; no real VM or display is used.
import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart' show SessionSocket, packet;
import 'viewport_session_test.dart'
    show ResizeMedia, connection, startInfo, resizeAck, applied;

class _Clock extends Stopwatch {
  int now = 0;
  @override
  int get elapsedMicroseconds => now;
}

class _Media extends ResizeMedia {
  int decoded = 0, error = 0;
  Completer<Map<String, dynamic>>? delayedStats;
  @override
  Future<int> start(int width, int height) async {
    decoded = 0;
    return super.start(width, height);
  }

  @override
  Future<Map<String, dynamic>> stats() async =>
      delayedStats?.future ??
      Future.value({
        'decodedFrames': decoded,
        'textureFrames': 0, // A minimized window need not consume a texture.
        'error': error,
        'audioPlayedFrames': 0,
      });
}

class _Fixture {
  final socket = SessionSocket(), media = _Media(), clock = _Clock();
  late final session = RemoteSession(
    connection: connection,
    token: 'fixture-only',
    media: media,
    clock: clock,
    dialer: (_, _) async => socket,
  );
  bool? result;
  late String id;

  Future<void> step(WidgetTester tester, [int milliseconds = 0]) async {
    clock.now += milliseconds * 1000;
    await tester.pump(Duration(milliseconds: milliseconds));
    await tester.pump();
  }

  Future<void> begin(WidgetTester tester) async {
    unawaited(session.start());
    socket.incoming.add(jsonEncode(startInfo));
    await step(tester);
    socket.incoming.add(packet());
    await step(tester);
    unawaited(session.resizeViewport(1346, 812).then((v) => result = v));
    await step(tester);
    id = socket.sent.lastWhere(
      (v) => v['type'] == 'stream.resize',
    )['requestId'];
    socket.incoming.add(jsonEncode(resizeAck(id)));
    await step(tester);
    socket.incoming.add(jsonEncode(applied(id)));
    await step(tester);
  }

  Future<void> frame(
    WidgetTester tester, {
    int epoch = 1,
    bool key = true,
  }) async {
    final bytes = packet();
    bytes[1] = key ? 1 : 0;
    bytes[2] = epoch;
    socket.incoming.add(bytes);
    await step(tester);
  }

  Future<void> dispose(WidgetTester tester) async {
    unawaited(session.close());
    await step(tester);
    unawaited(media.events.close());
    await step(tester);
  }
}

void main() {
  testWidgets(
    'A late decoder reply cannot complete resize after its frame deadline',
    (tester) async {
      final f = _Fixture();
      try {
        await f.begin(tester);
        await f.frame(tester);
        f.media.delayedStats = Completer<Map<String, dynamic>>();
        f.clock.now = 14000000;
        await f.step(tester, 50);
        expect(f.result, isNull);
        await f.step(tester, 2000);
        expect(f.result, isNull);
        f.media.delayedStats!.complete({'decodedFrames': 1, 'error': 0});
        await f.step(tester);
        expect(f.result, isFalse);
        expect(f.session.closed, isTrue);
        expect(f.session.diagnosticCode, 'viewport_frame_timeout');
      } finally {
        await f.dispose(tester);
      }
    },
  );
  testWidgets(
    'Resize waits for a matching keyframe and actual decoder output, not metadata or packet acceptance',
    (tester) async {
      final f = _Fixture();
      try {
        await f.begin(tester);
        expect(f.result, isNull, reason: 'Metadata is not a decoded frame');
        expect(f.session.resizing, isTrue);
        expect(await f.session.resizeViewport(1600, 900), isFalse);
        await f.session.capture(true);
        expect(f.session.captured, isFalse);
        f.media.decoded = 4;
        await f.frame(tester, epoch: 0);
        await f.frame(tester, key: false);
        await f.step(tester, 1000);
        expect(
          f.result,
          isNull,
          reason: 'Old frames and non-IDR cannot complete a new epoch',
        );
        f.media.decoded = 0;
        await f.frame(tester);
        await f.step(tester, 1000);
        expect(
          f.result,
          isNull,
          reason: 'Native queue acceptance is not decoding',
        );
        expect(f.media.packets, hasLength(2));
        f.media.decoded = 1;
        await f.step(tester, 60);
        expect(f.result, isTrue);
        expect(f.session.resizing, isFalse);
        expect(f.session.closed, isFalse);
        await f.session.capture(true);
        expect(f.session.captured, isTrue);
      } finally {
        await f.dispose(tester);
      }
    },
  );

  testWidgets(
    'A decoder error after resize metadata fails the pending operation',
    (tester) async {
      final f = _Fixture();
      try {
        await f.begin(tester);
        f.media.error = -1;
        await f.frame(tester);
        await f.step(tester, 1000);
        expect(f.result, isFalse);
        expect(f.session.closed, isTrue);
        expect(f.session.error, contains('화면을 표시하지 못해'));
      } finally {
        await f.dispose(tester);
      }
    },
  );

  testWidgets(
    'Missing post-resize decoded video times out without a success result',
    (tester) async {
      final f = _Fixture();
      try {
        await f.begin(tester);
        await f.frame(tester);
        await f.step(tester, 16000);
        expect(f.result, isFalse);
        expect(f.session.closed, isTrue);
        expect(f.session.diagnosticCode, 'viewport_frame_timeout');
      } finally {
        await f.dispose(tester);
      }
    },
  );

  testWidgets(
    'Closing while new-frame evidence is pending cannot be undone by late decoder statistics',
    (tester) async {
      final f = _Fixture();
      try {
        await f.begin(tester);
        await f.frame(tester);
        f.media.delayedStats = Completer<Map<String, dynamic>>();
        await f.step(tester, 1000);
        await f.dispose(tester);
        expect(f.result, isFalse);
        f.media.delayedStats!.complete({'decodedFrames': 1, 'error': 0});
        await f.step(tester);
        expect(f.result, isFalse);
        expect(f.session.closed, isTrue);
      } finally {
        if (!f.session.closed) await f.dispose(tester);
      }
    },
  );
}

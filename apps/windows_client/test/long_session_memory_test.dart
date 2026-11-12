// Long sessions keep a bounded memory (backlog #60): the real RemoteSession,
// RemoteView and ClipboardSession driven through eight simulated hours of one
// stream and through hundreds of reconnects, session resumes, viewport resizes
// and copies. After each run the live objects of this isolate are counted after
// a full garbage collection (heap_probe.dart) and must not have grown with the
// number of cycles. Sockets, media engine, clock and clipboard are fixtures.
//
// Needs the VM service: flutter test --enable-vmservice test/long_session_memory_test.dart
// (an ordinary `flutter test` run skips these tests).
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'package:multitap_client/data/desktop_previews.dart';
import 'package:multitap_client/models/desktop_machine.dart';

import 'clipboard_session_test.dart'
    show ClipboardSocket, MemoryClipboard, ready, sessionId, settle;
import 'desktop_preview_test.dart' as previews;
import 'display_change_robustness_test.dart' show ViewFixture, mockWindow;
import 'display_remote_test.dart' show machine;
import 'heap_probe.dart';
import 'remote_session_test.dart' show SessionSocket, flush, packet;
import 'viewport_session_test.dart' show ResizeMedia, connection, startInfo;

/// The native engine as a fixture that keeps nothing per packet.
class CountingMedia implements RemoteMedia, HeldPictureMedia {
  final events = StreamController<Map<String, dynamic>>.broadcast();
  int decoded = 0, texture = 40, serial = 0;

  /// Engine statistics carry one new 300 ms stall and a watched frame.
  bool stalls = false;
  @override
  Stream<Map<String, dynamic>> get input => events.stream;
  @override
  Future<int> start(int width, int height) async {
    decoded = 0;
    return ++texture;
  }

  @override
  bool get holdsPicture => true;
  @override
  Future<int> startHolding(int width, int height) => start(width, height);
  @override
  Future<void> releaseHeld(int texture) async {}
  @override
  Future<void> push(Uint8List packet) async {
    if (packet[0] == 1) decoded++;
  }

  @override
  Future<bool> capture(bool enabled) async => enabled;
  @override
  Future<Map<String, dynamic>> stats() async => {
    'decodedFrames': decoded,
    'error': 0,
    'audioError': -1,
    'audioPlayedFrames': 0,
    if (stalls) ...{
      'stalls': [
        [1, ++serial, 300, 0, 0, 2, 2, 3, 0, 2, 9000, 1, 0, 0, 16],
      ],
      'inputFrames': [
        [serial, 1500],
      ],
    },
  };
  @override
  Future<void> stop() async {}
}

class _Clock implements Stopwatch {
  _Clock(this.now);
  final int Function() now;
  @override
  int get elapsedMicroseconds => now();
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

/// Counted besides every object of this app: timers and stream plumbing that
/// a forgotten cancel or listener would leave behind.
const plumbing = [
  '_Timer',
  '_ControllerSubscription',
  '_BroadcastSubscription',
  '_SyncStreamController',
  '_AsyncStreamController',
  '_AsyncCompleter',
];

/// Grows to its fixed cap (256) while a run records; not a per-cycle leak.
const capped = {'ProductLogEntry'};

/// What may differ between two counts of the same steady state (a toast, a
/// pending timer or a log write in flight), far below one object per cycle.
const slack = 24;

void expectBounded(
  String run,
  Map<String, int> before,
  Map<String, int> after,
  Map<String, int> limits,
) {
  // ignore: avoid_print
  print(
    '$run: ${[for (final k in after.keys) '$k ${before[k]}->${after[k]}'].join(', ')}',
  );
  expect(
    after['app']! - before['app']!,
    lessThanOrEqualTo(slack),
    reason: '$run: app objects grew',
  );
  for (final name in plumbing) {
    expect(
      after[name]! - before[name]!,
      lessThanOrEqualTo(slack),
      reason: '$run: $name grew',
    );
  }
  limits.forEach((name, limit) {
    expect(after[name], lessThanOrEqualTo(limit), reason: '$run: $name');
  });
}

void main() {
  HeapProbe? probe;
  setUpAll(() async => probe = await HeapProbe.connect());
  tearDownAll(() async => probe?.close());
  Future<Map<String, int>> count(Iterable<String> names) =>
      probe!.live([...plumbing, ...names], notApp: capped);

  testWidgets(
    'Eight hours of one stream: statistics, stalls, inputs, pings and feedback stay bounded',
    (t) async {
      if (probe == null) return markTestSkipped('needs --enable-vmservice');
      final socket = SessionSocket(), media = CountingMedia()..stalls = true;
      var clock = 0, stallRows = 0, statsRows = 0;
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'qa-session',
          webSocketUrl: Uri.parse(
            'wss://cloud.example.com/api/v1/sessions/qa-session',
          ),
          bitrateAdaptive: true,
          inputLatency: true,
          observability: true,
        ),
        token: 'fixture-only-token',
        media: media,
        clock: _Clock(() => clock),
        observe: true,
        dialer: (_, _) async => socket,
        statsSink: (_) => statsRows++,
        stallSink: (_) => stallRows++,
      );
      unawaited(session.start());
      await t.pump();
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.info',
          'codec': 'h264',
          'width': 1280,
          'height': 720,
        }),
      );
      await t.pump();
      var second = 0;
      // One second of a busy stream: a picture, the Host's statistics with its
      // stall fields, a sampled key press with its acknowledgement and frame tag,
      // and the answer to every ping.
      Future<void> advance(int seconds) async {
        for (var i = 0; i < seconds; i++) {
          clock = ++second * 1000000;
          for (final sent in socket.sent) {
            if (sent['type'] == 'ping') {
              socket.incoming.add(
                jsonEncode({'type': 'pong', 'id': sent['id']}),
              );
            }
            if (sent['sample'] case final int sample) {
              socket.incoming.add(
                jsonEncode({
                  'type': 'input.ack',
                  'sessionId': 'qa-session',
                  'sample': sample,
                  'ok': true,
                  'injectUs': 100,
                  'holdUs': 200,
                }),
              );
              socket.incoming.add(
                jsonEncode({
                  'type': 'input.frame',
                  'sessionId': 'qa-session',
                  'sample': sample,
                  'pts': second,
                }),
              );
            }
          }
          socket.sent.clear();
          socket.incoming.add(
            jsonEncode({
              'type': 'stream.stats',
              'capturedFps': 60,
              'encodedFps': 60,
              'stall': {
                'encodeGapMs': 30,
                'keyFrames': 1,
                'targetMinKbps': 8000,
              },
              'udpBacklogKeys': second,
            }),
          );
          final video = packet();
          video[4] = second & 0xff;
          video[5] = (second >> 8) & 0xff;
          video[6] = (second >> 16) & 0xff;
          socket.incoming.add(video);
          media.events
            ..add({'type': 'input', 'event': 'key', 'vk': 0x41, 'down': true})
            ..add({'type': 'input', 'event': 'key', 'vk': 0x41, 'down': false});
          await t.pump(const Duration(seconds: 1));
        }
      }

      await advance(5);
      expect(session.frameReady, isTrue, reason: session.error);
      await session.capture(true);
      // Past every window: the observation report's 600 rows, the minute caps.
      await advance(15 * 60);
      final before = (await t.runAsync(
        () => count(['RemoteSession', 'StallWatch', 'InputLatencyMeter']),
      ))!;
      await advance(8 * 3600);
      final after = (await t.runAsync(
        () => count(['RemoteSession', 'StallWatch', 'InputLatencyMeter']),
      ))!;
      expect(session.closed, isFalse, reason: session.diagnosticCode);
      expect(stallRows, 30, reason: 'the per-session cap of stall rows');
      expect(
        statsRows,
        greaterThan(900),
        reason: 'a statistics window every 30 s',
      );
      expectBounded('8 h stream', before, after, {
        'RemoteSession': 1,
        'StallWatch': 1,
        'InputLatencyMeter': 1,
      });
      await session.close();
      await media.events.close();
    },
    timeout: const Timeout(Duration(minutes: 15)),
  );

  test(
    '200 reconnects: every ended session and its plumbing is released',
    () async {
      if (probe == null) return markTestSkipped('needs --enable-vmservice');
      Future<void> cycle() async {
        final socket = SessionSocket(), media = CountingMedia();
        final session = RemoteSession(
          connection: connection,
          token: 'fixture-only',
          media: media,
          dialer: (_, _) async => socket,
        );
        final running = session.start();
        socket.incoming.add(jsonEncode(startInfo));
        await flush();
        socket.incoming.add(packet());
        await flush();
        expect(session.frameReady, isTrue, reason: session.error);
        await session.close();
        await running;
        await media.events.close();
      }

      for (var i = 0; i < 20; i++) {
        await cycle();
      }
      final names = [
        'RemoteSession',
        'SessionSocket',
        'CountingMedia',
        'RelayInboxProbe',
      ];
      final before = await count(names);
      for (var i = 0; i < 200; i++) {
        await cycle();
      }
      final after = await count(names);
      expectBounded('200 reconnects', before, after, {
        for (final n in names) n: 0,
      });
    },
    timeout: const Timeout(Duration(minutes: 10)),
  );

  test(
    '200 session resumes: each replaced socket and its inbox is released',
    () async {
      if (probe == null) return markTestSkipped('needs --enable-vmservice');
      SessionSocket? dialed;
      final first = SessionSocket(), media = CountingMedia();
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'qa-session',
          webSocketUrl: Uri.parse(
            'wss://cloud.example.com/api/v1/sessions/qa-session',
          ),
          sessionResume: true,
          resumeGraceMs: 15000,
        ),
        token: 'fixture-only-token',
        media: media,
        dialer: (_, _) async => first,
        resumeDialer: (_, _, _) async => dialed = SessionSocket(),
      );
      final running = session.start();
      first.incoming.add(
        jsonEncode({
          'type': 'stream.info',
          'codec': 'h264',
          'width': 1280,
          'height': 720,
        }),
      );
      await flush();
      first.incoming.add(packet());
      await flush();
      var current = first, tokens = 0;
      String token() => String.fromCharCode(65 + tokens++ % 26) * 43;
      current.incoming.add(
        jsonEncode({
          'type': 'session.resume.token',
          'sessionId': 'qa-session',
          'token': token(),
          'graceMs': 15000,
        }),
      );
      await flush();
      Future<void> resume() async {
        unawaited(current.incoming.close()); // lost without a close frame
        await flush();
        expect(dialed, isNotNull, reason: '${session.diagnosticCode}');
        current = dialed!;
        dialed = null;
        current.incoming
          ..add(
            jsonEncode({
              'type': 'session.resumed',
              'sessionId': 'qa-session',
              'token': token(),
              'graceMs': 15000,
            }),
          )
          ..add(packet());
        await flush();
      }

      for (var i = 0; i < 20; i++) {
        await resume();
      }
      final names = ['RemoteSession', 'SessionSocket', 'RelayInboxProbe'];
      final before = await count(names);
      for (var i = 0; i < 200; i++) {
        await resume();
      }
      final after = await count(names);
      expect(session.closed, isFalse, reason: session.diagnosticCode);
      expectBounded('200 resumes', before, after, {
        'RemoteSession': 1,
        'SessionSocket': 2,
      });
      await session.close();
      await running;
      await media.events.close();
    },
    timeout: const Timeout(Duration(minutes: 10)),
  );

  test('200 viewport resizes with a held picture: nothing of an earlier size stays', () async {
    if (probe == null) return markTestSkipped('needs --enable-vmservice');
    final socket = SessionSocket(), media = CountingMedia();
    final session = RemoteSession(
      connection: connection,
      token: 'fixture-only',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    socket.incoming.add(jsonEncode(startInfo));
    await flush();
    socket.incoming.add(packet());
    await flush();
    var epoch = 0;
    Future<void> resize() async {
      epoch++;
      final width = epoch.isOdd ? 1346 : 1280, height = epoch.isOdd ? 812 : 720;
      final result = session.resizeViewport(width, height);
      await flush();
      final id =
          socket.sent.lastWhere(
                (v) => v['type'] == 'stream.resize',
              )['requestId']
              as String;
      socket.sent.clear();
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.resizing',
          'requestId': id,
          'resizeId': 'r$epoch',
          'epoch': epoch,
          'width': width,
          'height': height,
          'fps': 60,
          'monitor': '0',
          'timeoutMs': 90000,
        }),
      );
      await flush();
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.info',
          'codec': 'h264',
          'sessionId': 'resize-session',
          'requestId': id,
          'resizeId': 'r$epoch',
          'epoch': epoch,
          'width': width,
          'height': height,
          'sourceWidth': width,
          'sourceHeight': height,
          'fpsTarget': 60,
        }),
      );
      await flush();
      final fresh = packet()
        ..[2] = epoch & 0xff
        ..[3] = epoch >> 8;
      socket.incoming.add(fresh);
      expect(await result, isTrue, reason: session.resizeError);
    }

    for (var i = 0; i < 20; i++) {
      await resize();
    }
    final names = ['RemoteSession', '_ViewportChange'];
    final before = await count(names);
    for (var i = 0; i < 200; i++) {
      await resize();
    }
    // The last held picture's 250 ms release.
    await Future<void>.delayed(const Duration(milliseconds: 400));
    final after = await count(names);
    expectBounded('200 resizes', before, after, {
      'RemoteSession': 1,
      '_ViewportChange': 0,
    });
    await session.close();
    await running;
    await media.events.close();
  }, timeout: const Timeout(Duration(minutes: 10)));

  testWidgets(
    '100 remote-window reconnects after a lost Host: the window keeps one session',
    (t) async {
      if (probe == null) return markTestSkipped('needs --enable-vmservice');
      t.view.devicePixelRatio = 1;
      t.view.physicalSize = const Size(800, 600);
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      await mockWindow(t);
      ViewFixture create() {
        final socket = SessionSocket(), media = ResizeMedia();
        return ViewFixture(socket, media, ViewFixture.build(socket, media));
      }

      var current = create(), reconnects = 0;
      await t.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            initialOptions: const StreamOptions(),
            initialDisplayMode: SessionDisplayMode.windowed,
            load: () async => current.session,
            onRefreshDesktop: () async => machine(1),
            onReconnect: (_) async {
              reconnects++;
              return (current = create()).session;
            },
          ),
        ),
      );
      await t.pump();
      await t.pump();
      await current.initialFrame(t);
      Future<void> cycle() async {
        final before = reconnects, old = current;
        old.socket.incoming.add(
          jsonEncode({'type': 'session.ended', 'reason': 'host_disconnected'}),
        );
        await t.pump();
        await t.pump(const Duration(seconds: 2));
        for (var n = 0; n < 60 && reconnects == before; n++) {
          final retry = find.byKey(const Key('retry-remote-connection'));
          if (retry.evaluate().isNotEmpty) await t.tap(retry);
          await t.pump(const Duration(milliseconds: 100));
        }
        expect(reconnects, before + 1);
        await t.pump();
        await current.initialFrame(t);
        expect(current.session.frameReady, isTrue);
        unawaited(old.media.events.close());
      }

      for (var i = 0; i < 10; i++) {
        await cycle();
      }
      final names = [
        'RemoteSession',
        'SessionSocket',
        'ResizeMedia',
        'OverlayEntry',
      ];
      final before = (await t.runAsync(() => count(names)))!;
      for (var i = 0; i < 100; i++) {
        await cycle();
      }
      final after = (await t.runAsync(() => count(names)))!;
      expectBounded('100 window reconnects', before, after, {
        'RemoteSession': 2,
        'SessionSocket': 2,
      });
      await t.pumpWidget(const SizedBox.shrink());
      unawaited(current.session.close());
      await t.pump(const Duration(seconds: 3));
      await current.media.events.close();
    },
    timeout: const Timeout(Duration(minutes: 15)),
  );

  test('300 clipboard copies: finished transfers are not kept', () async {
    if (probe == null) return markTestSkipped('needs --enable-vmservice');
    final socket = ClipboardSocket(), bridge = MemoryClipboard();
    final session = ClipboardSession(
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/$sessionId/clipboard',
      ),
      token: 'fixture-only',
      sessionId: sessionId,
      policy: const ClipboardPolicy.all(),
      syncSupported: true,
      bridge: bridge,
      dialer: (_, _) async => socket,
    );
    final starting = session.start();
    await settle();
    socket.emit({...ready, 'clipboardSync': true});
    await starting;
    Future<void> ack(Map<String, dynamic> value, String type) async {
      socket.emit({
        'type': type,
        'requestId': value['requestId'],
        if (type != 'clipboard.complete') 'index': value['index'],
        if (type == 'clipboard.chunk.ack')
          'offset':
              (value['offset'] as int) +
              base64Decode(value['data'] as String).length,
      });
      await settle();
    }

    Future<void> copy() async {
      final operation = session.sendLocal();
      await ack(
        await socket.nextMessage('clipboard.chunk'),
        'clipboard.chunk.ack',
      );
      await ack(
        await socket.nextMessage('clipboard.item.end'),
        'clipboard.item.ack',
      );
      await ack(
        await socket.nextMessage('clipboard.commit'),
        'clipboard.complete',
      );
      await operation;
      socket.sent.clear();
      bridge.events.clear();
      bridge.sourceReads.clear();
    }

    // Past the 256 retired transfer IDs a session remembers.
    for (var i = 0; i < 270; i++) {
      await copy();
    }
    final names = ['ClipboardSession', '_Transfer'];
    final before = await count(names);
    for (var i = 0; i < 300; i++) {
      await copy();
    }
    final after = await count(names);
    expectBounded('300 copies', before, after, {
      'ClipboardSession': 1,
      '_Transfer': 0,
    });
    await session.close();
  }, timeout: const Timeout(Duration(minutes: 10)));

  test(
    'Preview request times of desktops gone from the list are dropped',
    () async {
      if (probe == null) return markTestSkipped('needs --enable-vmservice');
      var now = DateTime(2026, 11, 3, 9);
      final source = previews.FakePreviews();
      var listed = <DesktopMachine>[];
      final board = DesktopPreviewBoard(changed: () {}, clock: () => now);
      board.start(source, machines: () => listed, visible: () => true);
      // A day of VMs that come and go in the inventory.
      for (var i = 0; i < 200; i++) {
        final id = 'pve-${1000 + i}';
        source.offeredIds.add(id);
        listed = [previews.machine(id)];
        board.listed(listed);
        await flush();
        now = now.add(desktopPreviewNudge);
      }
      board.listed(listed);
      expect(
        source.reads,
        hasLength(200),
        reason: 'each new desktop asked once',
      );
      expect(
        await probe!.fieldLength('DesktopPreviewBoard', '_asked'),
        lessThanOrEqualTo(1),
      );
      board.stop();
    },
  );
}

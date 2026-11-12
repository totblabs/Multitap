import 'dart:convert';

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';
import 'package:multitap_client/data/product_log.dart';

import 'clipboard_onpaste_test.dart' as onpaste;
import 'clipboard_pull_test.dart' as pull;
import 'clipboard_session_test.dart' as base;

/// A platform that asks for files ahead of their reader (the native pull queue).
class _QueueClipboard extends onpaste.OnPasteClipboard
    implements ClipboardPullQueueBridge {
  @override
  bool get supportsPullQueue => true;
  @override
  int pullDepth = 1;
}

// The relay's pull queue (pull-queue-4-v1, 2026-11-02): a paste reads files one after another and each pull was a round trip, one at
// a time. With the queue the session sends up to four pulls at once (the offering side serves them in order), and serves the Host's
// queued pulls in order instead of refusing all but one.
void main() {
  late base.ClipboardSocket socket;
  late _QueueClipboard bridge;
  late ClipboardSession session;

  setUp(() {
    socket = base.ClipboardSocket();
    bridge = _QueueClipboard();
    session = ClipboardSession(
      policy: const ClipboardPolicy.all(),
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/${base.sessionId}/clipboard',
      ),
      token: 'test-token',
      sessionId: base.sessionId,
      bridge: bridge,
      dialer: (url, token) async => socket,
      pullLinger: Duration.zero,
    );
  });
  tearDown(() => session.close());

  Future<void> connect({bool queue = true}) async {
    final starting = session.start();
    await base.settle();
    socket.emit({
      ...onpaste.onPasteReady(),
      if (queue) 'clipboardPullQueue': clipboardPullQueueProtocol,
    });
    await starting;
  }

  List<Map<String, dynamic>> sentOf(String type) => [
    for (final value in socket.sent)
      if (value['type'] == type) value,
  ];

  /// The VM's copy of [sizes.length] files, published here.
  Future<String> vmOffer(List<int> sizes) async {
    final receiving = session.receiveRemote();
    final read = await socket.nextMessage('clipboard.read');
    final id = read['requestId'] as String;
    socket.emit({
      'type': 'clipboard.offer',
      'requestId': id,
      'format': 'tree',
      'entries': sizes.length,
      'files': sizes.length,
      'bytes': sizes.fold(0, (a, b) => a + b),
      'tops': sizes.length,
      'skipped': 0,
    });
    await base.settle();
    socket.emit({
      'type': 'clipboard.entries',
      'requestId': id,
      'start': 0,
      'entries': [
        for (var i = 0; i < sizes.length; i++)
          {
            'path': 'f$i.bin',
            'kind': 'file',
            'size': sizes[i],
            'attrs': 32,
            'mtime': 1761500000123,
          },
      ],
    });
    await socket.nextMessage('clipboard.applied');
    socket.emit({'type': 'clipboard.complete', 'requestId': id});
    await receiving;
    return id;
  }

  void serve(String pullId, int index, Uint8List data) {
    socket.emit({
      'type': 'clipboard.chunk',
      'requestId': pullId,
      'index': index,
      'offset': 0,
      'data': base64Encode(data),
    });
    socket.emit({
      'type': 'clipboard.item.end',
      'requestId': pullId,
      'index': index,
      'sha256': sha256.convert(data).toString(),
    });
  }

  test(
    'negotiation: the header is announced, the depth reaches the platform',
    () async {
      expect(
        session.dialHeaders[clipboardPullQueueHeader],
        clipboardPullQueueHeaderValue,
      );
      await connect();
      expect(bridge.pullDepth, clipboardPullQueueDepth);
    },
  );

  test(
    'a pair without the queue keeps one pull at a time and depth 1',
    () async {
      await connect(queue: false);
      expect(bridge.pullDepth, 1);
      final id = await vmOffer([10, 20]);
      bridge.platform(ClipboardPullRequest(1, id, 0, 0));
      bridge.platform(ClipboardPullRequest(2, id, 1, 0));
      await base.settle();
      expect(sentOf('clipboard.pull').map((v) => v['index']), [0]);
    },
  );

  test(
    'four files asked for ahead are four pulls at once, received in order',
    () async {
      await connect();
      final id = await vmOffer([10, 20, 30, 40, 50]);
      for (var i = 0; i < 5; i++) {
        bridge.platform(ClipboardPullRequest(i + 1, id, i, 0));
      }
      await base.settle();
      final pulls = sentOf('clipboard.pull');
      expect(pulls.map((v) => v['index']), [
        0,
        1,
        2,
        3,
      ], reason: 'the queue sends four pulls without waiting for the first');
      final data = [
        for (var i = 0; i < 5; i++)
          Uint8List.fromList(List.filled(10 * (i + 1), i + 1)),
      ];
      for (var i = 0; i < 4; i++) {
        serve(pulls[i]['requestId'] as String, i, data[i]);
        await base.settle();
      }
      // The fifth goes out as soon as one ends.
      final fifth = sentOf('clipboard.pull').where((v) => v['index'] == 4);
      expect(fifth, hasLength(1));
      serve(fifth.single['requestId'] as String, 4, data[4]);
      await base.settle();
      expect([for (final d in bridge.data) d.$1], [1, 2, 3, 4, 5]);
      expect([for (final e in bridge.ended) e.$1], [1, 2, 3, 4, 5]);
      expect(session.closed, isFalse);
    },
  );

  test('a queued pull the platform no longer wants is cancelled at the relay; one the relay ends fails quietly', () async {
    await connect();
    final id = await vmOffer([10, 20, 30]);
    for (var i = 0; i < 3; i++) {
      bridge.platform(ClipboardPullRequest(i + 1, id, i, 0));
    }
    await base.settle();
    final pulls = sentOf('clipboard.pull');
    expect(pulls, hasLength(3));
    socket.sent.clear();
    bridge.platform(const ClipboardPullCancel(2));
    await base.settle();
    expect(
      sentOf('clipboard.cancel').single['requestId'],
      pulls[1]['requestId'],
    );
    socket.emit({
      'type': 'clipboard.error',
      'requestId': pulls[2]['requestId'],
      'reason': 'clipboard_offer_expired',
    });
    await base.settle();
    expect(bridge.failedRanges, contains((3, 'clipboard_offer_expired')));
    serve(
      pulls[0]['requestId'] as String,
      0,
      Uint8List.fromList(List.filled(10, 1)),
    );
    await base.settle();
    expect([for (final e in bridge.ended) e.$1], [1]);
    expect(session.closed, isFalse);
    expect(session.error, isNull);
  });

  test('the Host\'s queued pulls of this PC\'s offer are served in order, not refused', () async {
    await connect();
    bridge.offerCapture = ClipboardCapture(
      format: 'tree',
      items: [pull.tree('a.bin', 3), pull.tree('b.bin', 4)],
      tops: 2,
      offer: true,
    );
    await session.setSharingActive(true);
    final sending = session.sendLocal(automatic: true);
    final offer = await socket.nextMessage('clipboard.offer');
    await socket.nextMessage('clipboard.entries');
    socket.emit({
      'type': 'clipboard.complete',
      'requestId': offer['requestId'],
    });
    await sending;
    final id = offer['requestId'] as String;
    bridge.files[id] = [
      Uint8List.fromList([1, 2, 3]),
      Uint8List.fromList([4, 5, 6, 7]),
    ];
    socket.sent.clear();
    for (var i = 0; i < 2; i++) {
      socket.emit({
        'type': 'clipboard.pull',
        'requestId': 'pullh-${i + 1}',
        'offerId': id,
        'index': i,
        'offset': 0,
      });
    }
    await base.settle();
    expect(sentOf('clipboard.error'), isEmpty);
    final ends = sentOf('clipboard.item.end');
    expect(ends.map((v) => v['requestId']), ['pullh-1', 'pullh-2']);
  });

  // The served offer goes away during a paste in the VM. Without this, one
  // action-13 failure (clipboard_file_unavailable, error 1) comes with 8
  // action-8 records of unknown_failure: the raw clipboard_offer_expired is no
  // diagnostic code, and every range of the Host's queue records its own
  // failure. A refused queued pull would also leave the ones behind it
  // unanswered.
  test('the Host\'s queued pulls failing with the served offer are one known failure, all answered', () async {
    await connect();
    bridge.offerCapture = ClipboardCapture(
      format: 'tree',
      items: [
        pull.tree('a.bin', 5 * 1024 * 1024),
        pull.tree('b.bin', 3),
        pull.tree('c.bin', 3),
        pull.tree('d.bin', 3),
      ],
      tops: 4,
      offer: true,
    );
    await session.setSharingActive(true);
    final sending = session.sendLocal(automatic: true);
    final offer = await socket.nextMessage('clipboard.offer');
    await socket.nextMessage('clipboard.entries');
    socket.emit({
      'type': 'clipboard.complete',
      'requestId': offer['requestId'],
    });
    await sending;
    final id = offer['requestId'] as String;
    bridge.files[id] = [
      Uint8List(5 * 1024 * 1024),
      Uint8List(3),
      Uint8List(3),
      Uint8List(3),
    ];
    socket.sent.clear();
    for (var i = 0; i < 4; i++) {
      socket.emit({
        'type': 'clipboard.pull',
        'requestId': 'pullh-${i + 1}',
        'offerId': id,
        'index': i,
        'offset': 0,
      });
      await base.settle();
    }
    // The first file filled its window; the other three wait in order.
    final chunks = sentOf('clipboard.chunk');
    expect(chunks, isNotEmpty);
    expect(chunks.every((c) => c['requestId'] == 'pullh-1'), isTrue);
    expect(sentOf('clipboard.error'), isEmpty);
    // The offer is gone in the platform: reading on and opening fail alike.
    const expired = 'clipboard_offer_expired';
    bridge.readFailure = PlatformException(code: expired);
    bridge.openFailure = PlatformException(code: expired);
    final records = <ProductLogEntry>[];
    final recording = ProductLog.instance.localRecords.listen(records.add);
    addTearDown(recording.cancel);
    final last = chunks.last;
    socket.emit({
      'type': 'clipboard.chunk.ack',
      'requestId': 'pullh-1',
      'index': 0,
      'offset':
          (last['offset'] as int) +
          base64Decode(last['data'] as String).length,
    });
    await base.settle();
    final errors = sentOf('clipboard.error');
    expect(errors.map((e) => e['requestId']), [
      'pullh-1',
      'pullh-2',
      'pullh-3',
      'pullh-4',
    ], reason: 'every queued pull is answered, in order');
    expect(errors.every((e) => e['reason'] == expired), isTrue);
    final pulled = records
        .where((e) => e.event == LogEvent.clipboard && e.values['action'] == 8)
        .toList();
    expect(pulled, hasLength(1), reason: 'one failed transfer, not one per range');
    expect(pulled.single.code, 'clipboard_file_unavailable');
    expect(pulled.single.values['error'], 1);
    expect(pulled.single.values['remote'], isTrue);
    expect(pulled.single.values['ok'], isFalse);
    expect(records.where((e) => e.code == 'unknown_failure'), isEmpty);
    expect(session.error, expired);
    expect(session.closed, isFalse);
  });
}

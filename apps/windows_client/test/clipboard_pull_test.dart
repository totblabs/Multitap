import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_formats_test.dart' as fmt;
import 'clipboard_session_test.dart' as base;

/// The relay's `limits` for two peers that announced the formats and pull profiles.
Map<String, dynamic> pullLimits() => {
  ...fmt.formatsLimits(),
  'pullProfile': 1,
  'maxOffers': 2,
};
Map<String, dynamic> pullReady({Map<String, dynamic>? policy}) => {
  ...base.ready,
  'limits': pullLimits(),
  'clipboardPolicy': ?policy,
};

/// The Windows adapter's stand-in with the paste-pull profile: it records what the
/// session hands the platform and serves files out of memory.
class PullClipboard extends fmt.FormatsClipboard implements ClipboardPullBridge {
  @override
  bool get supportsPull => true;
  bool pullOn = false;
  @override
  set pull(bool value) => pullOn = value;
  final _events = StreamController<ClipboardPullEvent>.broadcast(sync: true);
  @override
  Stream<ClipboardPullEvent> get pullEvents => _events.stream;
  void platform(ClipboardPullEvent event) => _events.add(event);

  ClipboardCapture? offerCapture;
  String? offerIdUsed;
  @override
  Future<ClipboardCapture> captureWithOffer(
    String owner,
    String offerId, {
    bool skipOwn = false,
  }) async {
    events.add('captureWithOffer');
    offerIdUsed = offerId;
    return offerCapture ?? await capture(owner, skipOwn: skipOwn);
  }

  final publishedOffers = <(String, List<ClipboardItem>)>[];
  Object? publishFailure;
  @override
  Future<void> publishOffer(
    String owner,
    String offerId,
    List<ClipboardItem> entries,
  ) async {
    events.add('publishOffer');
    if (publishFailure != null) throw publishFailure!;
    publishedOffers.add((offerId, entries));
  }

  final data = <(int, int, Uint8List)>[];
  final ended = <(int, String)>[];
  final failedRanges = <(int, String)>[];
  final shutdowns = <String>[];
  bool offersClosed = false;
  Object? dataFailure;
  @override
  Future<void> pullData(int rangeId, int offset, Uint8List bytes) async {
    if (dataFailure != null) throw dataFailure!;
    data.add((rangeId, offset, bytes));
  }

  @override
  Future<void> pullEnd(int rangeId, String digest) async =>
      ended.add((rangeId, digest));
  @override
  Future<void> pullFail(int rangeId, String reason) async =>
      failedRanges.add((rangeId, reason));
  @override
  Future<void> pullShutdown(String owner, String reason) async =>
      shutdowns.add(reason);
  @override
  Future<void> closeOffers(String owner) async => offersClosed = true;

  // Serving: offers this side kept.
  final files = <String, List<Uint8List>>{};
  Object? openFailure, readFailure;
  Uint8List? current;
  int position = 0;
  bool closed = false;
  final opened = <(String, int, int)>[];
  String served = 'b' * 64;
  @override
  Future<int> pullOpen(String owner, String offerId, int index, int offset) async {
    opened.add((offerId, index, offset));
    if (openFailure != null) throw openFailure!;
    final list = files[offerId];
    if (list == null) {
      throw PlatformException(code: 'clipboard_offer_expired');
    }
    current = list[index];
    position = offset;
    closed = false;
    return current!.length;
  }

  int reads = 0;
  @override
  Future<Uint8List> pullRead(String owner, int length) async {
    reads++;
    if (readFailure != null) throw readFailure!;
    final bytes = Uint8List.sublistView(current!, position, position + length);
    position += length;
    return bytes;
  }

  @override
  Future<String> pullDigest(String owner) async => served;
  @override
  Future<void> pullClose(String owner) async => closed = true;
}

ClipboardItem tree(String path, int size) => ClipboardItem(
  size: size,
  name: path,
  kind: 'file',
  attributes: 32,
  mtime: 1761500000123,
);
ClipboardItem folder(String path) =>
    ClipboardItem(size: 0, name: path, kind: 'dir', mtime: 1761500000000);

void main() {
  late base.ClipboardSocket socket;
  late PullClipboard bridge;
  late ClipboardSession session;

  ClipboardSession make({
    ClipboardPolicy policy = const ClipboardPolicy.all(),
    ClipboardBridge? with_,
  }) => ClipboardSession(
    policy: policy,
    url: Uri.parse(
      'wss://cloud.example.com/api/v1/sessions/${base.sessionId}/clipboard',
    ),
    token: 'test-token',
    sessionId: base.sessionId,
    bridge: with_ ?? bridge,
    dialer: (url, token) async => socket,
    pullLinger: Duration.zero,
  );
  Future<void> connect([Map<String, dynamic>? ready]) async {
    final starting = session.start();
    await base.settle();
    socket.emit(ready ?? pullReady());
    await starting;
  }

  setUp(() {
    socket = base.ClipboardSocket();
    bridge = PullClipboard();
    session = make();
  });
  tearDown(() async => session.close());

  List<Map<String, dynamic>> sentOf(String type) => [
    for (final value in socket.sent)
      if (value['type'] == type) value,
  ];

  group('negotiation', () {
    test('the limits parser accepts exactly the formats numbers plus the pull profile', () {
      final parsed = ClipboardLimits.parse(pullLimits(), chunkBytes: 32768);
      expect(parsed, same(ClipboardLimits.pullProfile));
      expect(parsed!.pull, isTrue);
      expect(parsed.formats, isTrue);
      expect(parsed.maxOffers, 2);
      expect(
        ClipboardLimits.parse(fmt.formatsLimits(), chunkBytes: 32768),
        same(ClipboardLimits.formatsProfile),
      );
      expect(ClipboardLimits.formatsProfile.pull, isFalse);
      expect(ClipboardLimits.formatsProfile.maxOffers, 0);
      for (final broken in [
        {...pullLimits(), 'pullProfile': 2},
        {...pullLimits(), 'maxOffers': 3},
        {...pullLimits()}..remove('maxOffers'),
        {...pullLimits(), 'extra': 1},
        {...pullLimits(), 'windowUnits': 31},
      ]) {
        expect(ClipboardLimits.parse(broken, chunkBytes: 32768), isNull);
      }
    });

    test('only a bridge that supports pulling announces it; it needs the formats headers too', () {
      expect(session.dialHeaders, {
        clipboardLimitsHeader: clipboardLimitsHeaderValue,
        clipboardFormatsHeader: clipboardFormatsHeaderValue,
        clipboardPullHeader: clipboardPullHeaderValue,
      });
      final plain = make(with_: fmt.FormatsClipboard());
      expect(plain.dialHeaders.containsKey(clipboardPullHeader), isFalse);
      expect(plain.dialHeaders.containsKey(clipboardFormatsHeader), isTrue);
      addTearDown(plain.close);
      final released = make(with_: base.MemoryClipboard());
      expect(released.dialHeaders.length, 1);
      addTearDown(released.close);
    });

    test('a pull pair tells the bridge; a formats pair does not and keeps pushing', () async {
      await connect();
      expect(bridge.pullOn, isTrue);
      expect(session.limits.pull, isTrue);
      await session.close();
      socket = base.ClipboardSocket();
      bridge = PullClipboard();
      session = make();
      await connect(fmt.formatsReady());
      expect(bridge.pullOn, isFalse);
      expect(session.limits.pull, isFalse);
      // A big folder copy is captured the ordinary way and pushed with begin.
      bridge.captured = ClipboardCapture(
        format: 'tree',
        items: [folder('d'), tree('d/a.bin', 3)],
        tops: 1,
      );
      final sending = session.sendLocal();
      final begin = await socket.nextMessage('clipboard.begin');
      expect(begin['format'], 'tree');
      expect(bridge.events.contains('captureWithOffer'), isFalse);
      expect(sentOf('clipboard.offer'), isEmpty);
      socket.emit({'type': 'clipboard.error', 'requestId': begin['requestId'], 'reason': 'clipboard_cancelled'});
      await sending.catchError((_) {});
    });

    test('a pull limits object for a bridge that cannot pull closes the channel', () async {
      final released = make(with_: fmt.FormatsClipboard());
      final starting = released.start();
      starting.ignore();
      await base.settle();
      socket.emit(pullReady());
      await expectLater(starting, throwsA(isA<ClipboardTransferException>()));
      expect(released.ready, isFalse);
      await released.close();
    });
  });

  group('offering', () {
    ClipboardCapture offerOf(List<ClipboardItem> items, {int tops = 1}) =>
        ClipboardCapture(format: 'tree', items: items, tops: tops, offer: true);

    test('a large copy sends the manifest only, shows no transfer and ends at complete', () async {
      await connect();
      bridge.offerCapture = offerOf([
        folder('Photos'),
        tree('Photos/a.jpg', 5000000),
        tree('Photos/b.jpg', 7000000),
      ]);
      final sending = session.sendLocal();
      final offer = await socket.nextMessage('clipboard.offer');
      expect(bridge.offerIdUsed, offer['requestId']);
      expect(offer, {
        'type': 'clipboard.offer',
        'requestId': offer['requestId'],
        'format': 'tree',
        'entries': 3,
        'files': 2,
        'bytes': 12000000,
        'tops': 1,
        'skipped': 0,
      });
      final entries = await socket.nextMessage('clipboard.entries');
      expect(entries['requestId'], offer['requestId']);
      expect(entries['entries'], [
        {'path': 'Photos', 'kind': 'dir', 'attrs': 0, 'mtime': 1761500000000},
        {'path': 'Photos/a.jpg', 'kind': 'file', 'size': 5000000, 'attrs': 32, 'mtime': 1761500000123},
        {'path': 'Photos/b.jpg', 'kind': 'file', 'size': 7000000, 'attrs': 32, 'mtime': 1761500000123},
      ]);
      // No bytes, no commit, no progress at copy time.
      await base.settle();
      expect(sentOf('clipboard.chunk'), isEmpty);
      expect(sentOf('clipboard.commit'), isEmpty);
      expect(session.progress, isNull);
      expect(session.transferring, isTrue);
      socket.emit({'type': 'clipboard.complete', 'requestId': offer['requestId']});
      await sending;
      expect(session.transferring, isFalse);
      expect(session.error, isNull);
      expect(session.status, contains('붙여넣'));
    });

    test('the receiver may refuse an offer; the sender reports it', () async {
      await connect();
      bridge.offerCapture = offerOf([tree('a.bin', 20000000)]);
      final sending = session.sendLocal();
      final offer = await socket.nextMessage('clipboard.offer');
      socket.emit({'type': 'clipboard.error', 'requestId': offer['requestId'], 'reason': 'clipboard_write_failed'});
      await expectLater(sending, throwsA(isA<ClipboardTransferException>()));
      expect(session.error, 'clipboard_write_failed');
    });

    Future<String> offered(List<Uint8List> files) async {
      bridge.offerCapture = offerOf([
        for (var i = 0; i < files.length; i++) tree('f$i.bin', files[i].length),
      ], tops: files.length);
      final sending = session.sendLocal();
      final offer = await socket.nextMessage('clipboard.offer');
      await socket.nextMessage('clipboard.entries');
      socket.emit({'type': 'clipboard.complete', 'requestId': offer['requestId']});
      await sending;
      bridge.files[offer['requestId'] as String] = files;
      return offer['requestId'] as String;
    }

    Map<String, dynamic> pullRequest(String id, String offer, int index, [int offset = 0]) => {
      'type': 'clipboard.pull',
      'requestId': id,
      'offerId': offer,
      'index': index,
      'offset': offset,
    };

    test('a pull is answered with the windowed chunk stream, then the item end', () async {
      await connect();
      final data = Uint8List.fromList(List.generate(2 * 1024 * 1024 + 17, (i) => i % 251));
      final id = await offered([Uint8List(0), data]);
      socket.emit(pullRequest('pullh-1', id, 1));
      await base.settle();
      await Future<void>.delayed(const Duration(milliseconds: 30));
      expect(bridge.opened, [(id, 1, 0)]);
      // The window holds 1 MiB (22 chunks of 45 KiB); nothing more until it is acknowledged.
      var chunks = sentOf('clipboard.chunk');
      expect(chunks.length, 22);
      expect(chunks.first['requestId'], 'pullh-1');
      expect(chunks.first['index'], 1);
      expect(chunks.first['offset'], 0);
      expect(base64Decode(chunks.first['data'] as String).length, 46080);
      expect(sentOf('clipboard.item.end'), isEmpty);
      var sent = 22 * 46080;
      // Acknowledge in steps until the whole file went out.
      while (sentOf('clipboard.item.end').isEmpty) {
        socket.emit({'type': 'clipboard.chunk.ack', 'requestId': 'pullh-1', 'index': 1, 'offset': sent});
        await Future<void>.delayed(const Duration(milliseconds: 30));
        final now = sentOf('clipboard.chunk');
        if (now.length == chunks.length && sentOf('clipboard.item.end').isEmpty) fail('stalled');
        chunks = now;
        final last = chunks.last;
        sent = (last['offset'] as int) + base64Decode(last['data'] as String).length;
      }
      expect(sent, data.length);
      final builder = BytesBuilder();
      var offset = 0;
      for (final chunk in chunks) {
        expect(chunk['offset'], offset);
        final bytes = base64Decode(chunk['data'] as String);
        builder.add(bytes);
        offset += bytes.length;
      }
      expect(builder.toBytes(), data);
      // One platform read per block of 22 chunks (2026-11-03), not one per chunk: 46 chunks are three reads.
      expect(bridge.reads, 3);
      final end = sentOf('clipboard.item.end').single;
      expect(end, {'type': 'clipboard.item.end', 'requestId': 'pullh-1', 'index': 1, 'sha256': bridge.served});
      expect(bridge.closed, isTrue);
      // The relay drops late acknowledgements of an ended pull; this side ignores them too.
      socket.emit({'type': 'clipboard.item.ack', 'requestId': 'pullh-1', 'index': 1});
      socket.emit({'type': 'clipboard.chunk.ack', 'requestId': 'pullh-1', 'index': 1, 'offset': 46080});
      await base.settle();
      expect(session.ready, isTrue);
      // The next pull (an empty file; then a range from an offset) works.
      socket.emit(pullRequest('pullh-2', id, 0));
      await base.settle();
      expect(sentOf('clipboard.item.end').length, 2);
      socket.emit(pullRequest('pullh-3', id, 1, 2 * 1024 * 1024));
      await base.settle();
      final tail = sentOf('clipboard.chunk').where((c) => c['requestId'] == 'pullh-3').toList();
      expect(tail.single['offset'], 2 * 1024 * 1024);
      expect(base64Decode(tail.single['data'] as String).length, 17);
    });

    test('a progress notice follows the served bytes while the peer pastes', () async {
      await connect();
      final id = await offered([Uint8List(3 * 1024 * 1024)]);
      socket.emit(pullRequest('pullh-1', id, 0));
      await Future<void>.delayed(const Duration(milliseconds: 30));
      final progress = session.progress!;
      expect(progress.sending, isTrue);
      expect(progress.format, 'tree');
      expect(progress.fileCount, 1);
      expect(progress.total, 3 * 1024 * 1024);
      expect(progress.transferred, 22 * 46080);
      expect(session.pulling, isTrue);
      expect(session.transferring, isTrue);
      socket.emit({'type': 'clipboard.cancel', 'requestId': 'pullh-1'});
      await base.settle();
      expect(session.pulling, isFalse);
    });

    test('failures answer the pull with the reason and leave the channel alone', () async {
      await connect();
      final id = await offered([Uint8List(10)]);
      for (final (failure, reason) in [
        (PlatformException(code: 'clipboard_selected_file_changed'), 'clipboard_selected_file_changed'),
        (PlatformException(code: 'clipboard_offer_expired'), 'clipboard_offer_expired'),
        (PlatformException(code: 'clipboard_file_locked'), 'clipboard_file_locked'),
        (PlatformException(code: 'something_else'), 'clipboard_transfer_failed'),
      ]) {
        socket.sent.clear();
        bridge.openFailure = failure;
        socket.emit(pullRequest('pullh-${reason.length}${failure.code.length}', id, 0));
        await base.settle();
        expect(sentOf('clipboard.error').single['reason'], reason);
      }
      bridge.openFailure = null;
      socket.sent.clear();
      bridge.readFailure = PlatformException(code: 'clipboard_selected_file_changed');
      socket.emit(pullRequest('pullh-90', id, 0));
      await base.settle();
      expect(sentOf('clipboard.error').single, {
        'type': 'clipboard.error',
        'requestId': 'pullh-90',
        'reason': 'clipboard_selected_file_changed',
      });
      expect(bridge.closed, isTrue);
      expect(session.status, contains('바뀌'));
      expect(session.ready, isTrue);
    });

    test('the peer may cancel; a second pull while one runs is refused as busy', () async {
      await connect();
      final data = Uint8List(3 * 1024 * 1024);
      final id = await offered([data]);
      socket.emit(pullRequest('pullh-1', id, 0));
      await base.settle();
      socket.emit(pullRequest('pullh-2', id, 0));
      await base.settle();
      expect(sentOf('clipboard.error').single, {
        'type': 'clipboard.error',
        'requestId': 'pullh-2',
        'reason': 'clipboard_busy',
      });
      socket.emit({'type': 'clipboard.cancel', 'requestId': 'pullh-1'});
      await base.settle();
      expect(bridge.closed, isTrue);
      final before = sentOf('clipboard.chunk').length;
      socket.emit({'type': 'clipboard.chunk.ack', 'requestId': 'pullh-1', 'index': 0, 'offset': 46080});
      await base.settle();
      expect(sentOf('clipboard.chunk').length, before);
      socket.emit(pullRequest('pullh-3', id, 0));
      await base.settle();
      expect(sentOf('clipboard.chunk').length, greaterThan(before));
    });

    test('without the files permission nothing is read', () async {
      session = make(policy: const ClipboardPolicy(textToRemote: true));
      await connect(pullReady(policy: {
        'textToRemote': true,
        'textToLocal': true,
        'filesToRemote': false,
        'filesToLocal': true,
      }));
      socket.emit(pullRequest('pullh-1', 'whatever', 0));
      await base.settle();
      expect(sentOf('clipboard.error').single['reason'], 'clipboard_policy_denied');
      expect(bridge.opened, isEmpty);
    });

    test('malformed pull requests and out-of-window acknowledgements close the channel', () async {
      for (final bad in <Map<String, dynamic>>[
        {'type': 'clipboard.pull', 'requestId': 'pullh-1', 'offerId': 'x', 'index': -1, 'offset': 0},
        {'type': 'clipboard.pull', 'requestId': 'pullh-1', 'offerId': 'x', 'index': 0},
        {'type': 'clipboard.pull', 'requestId': 'pullh-1', 'offerId': 'x', 'index': 0, 'offset': 0, 'extra': 1},
        {'type': 'clipboard.pull', 'requestId': 'pullh-1', 'offerId': 7, 'index': 0, 'offset': 0},
      ]) {
        socket = base.ClipboardSocket();
        bridge = PullClipboard();
        session = make();
        await connect();
        socket.emit(bad);
        await base.settle();
        expect(session.ready, isFalse, reason: '$bad');
        expect(session.error, 'clipboard_invalid_message');
      }
      socket = base.ClipboardSocket();
      bridge = PullClipboard();
      session = make();
      await connect();
      final id = await offered([Uint8List(3 * 1024 * 1024)]);
      socket.emit(pullRequest('pullh-1', id, 0));
      await Future<void>.delayed(const Duration(milliseconds: 30));
      socket.emit({'type': 'clipboard.chunk.ack', 'requestId': 'pullh-1', 'index': 0, 'offset': 77});
      await base.settle();
      expect(session.ready, isFalse);
    });
  });

  group('receiving', () {
    Map<String, dynamic> offerHeader(String id) => {
      'type': 'clipboard.offer',
      'requestId': id,
      'format': 'tree',
      'entries': 3,
      'files': 2,
      'bytes': 170000,
      'tops': 2,
      'skipped': 0,
    };
    List<Map<String, dynamic>> wireEntries() => [
      {'path': 'Pics', 'kind': 'dir', 'attrs': 0, 'mtime': 1761500000000},
      {'path': 'Pics/a.bin', 'kind': 'file', 'size': 100000, 'attrs': 32, 'mtime': 1761500000123},
      {'path': 'b.bin', 'kind': 'file', 'size': 70000, 'attrs': 32, 'mtime': 1761500000123},
    ];

    Future<String> receiveOffer() async {
      final receiving = session.receiveRemote();
      final read = await socket.nextMessage('clipboard.read');
      final id = read['requestId'] as String;
      socket.emit(offerHeader(id));
      await base.settle();
      socket.emit({'type': 'clipboard.entries', 'requestId': id, 'start': 0, 'entries': wireEntries()});
      await socket.nextMessage('clipboard.applied');
      expect(bridge.publishedOffers.single.$1, id);
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await receiving;
      return id;
    }

    test('an offer is published as virtual files; no data, no commit, no progress', () async {
      await connect();
      final id = await receiveOffer();
      final (_, published) = bridge.publishedOffers.single;
      expect([for (final e in published) e.name], ['Pics', 'Pics/a.bin', 'b.bin']);
      expect([for (final e in published) e.mtime], [1761500000000, 1761500000123, 1761500000123]);
      expect(published.first.isDirectory, isTrue);
      expect(bridge.events.contains('begin'), isFalse);
      expect(bridge.events.contains('commit'), isFalse);
      expect(session.progress, isNull);
      expect(session.transferring, isFalse);
      expect(session.error, isNull);
      expect(id, isNotEmpty);
    });

    test('an offer on a pair without the pull profile, or without the permission, is invalid', () async {
      await connect(fmt.formatsReady());
      final receiving = session.receiveRemote();
      receiving.ignore();
      final read = await socket.nextMessage('clipboard.read');
      socket.emit(offerHeader(read['requestId'] as String));
      await base.settle();
      expect(session.ready, isFalse);
      await session.close();

      socket = base.ClipboardSocket();
      bridge = PullClipboard();
      session = make();
      await connect(pullReady(policy: {
        'textToRemote': true,
        'textToLocal': true,
        'filesToRemote': true,
        'filesToLocal': false,
      }));
      expect(session.canReceive, isTrue);
      final other = session.receiveRemote();
      other.ignore();
      final again = await socket.nextMessage('clipboard.read');
      socket.emit(offerHeader(again['requestId'] as String));
      await base.settle();
      expect(session.ready, isFalse);
    });

    test('entries of an offer carry a write time', () async {
      await connect();
      final receiving = session.receiveRemote();
      receiving.ignore();
      final read = await socket.nextMessage('clipboard.read');
      socket.emit(offerHeader(read['requestId'] as String));
      await base.settle();
      final broken = wireEntries()..[1].remove('mtime');
      socket.emit({'type': 'clipboard.entries', 'requestId': read['requestId'], 'start': 0, 'entries': broken});
      await base.settle();
      expect(session.ready, isFalse);
      expect(bridge.publishedOffers, isEmpty);
    });

    test('a platform that cannot publish fails the transfer without a commit', () async {
      await connect();
      bridge.publishFailure = PlatformException(code: 'clipboard_write_failed');
      final receiving = session.receiveRemote();
      receiving.ignore();
      final read = await socket.nextMessage('clipboard.read');
      socket.emit(offerHeader(read['requestId'] as String));
      await base.settle();
      socket.emit({'type': 'clipboard.entries', 'requestId': read['requestId'], 'start': 0, 'entries': wireEntries()});
      final error = await socket.nextMessage('clipboard.error');
      expect(error['reason'], 'clipboard_write_failed');
      expect(sentOf('clipboard.applied'), isEmpty);
    });

    Future<String> published() => receiveOffer();

    test('a paste pulls one range at a time, hands the bytes over and acknowledges what was consumed', () async {
      await connect();
      final id = await published();
      bridge.platform(ClipboardPullRequest(11, id, 0, 0));
      final pull = await socket.nextMessage('clipboard.pull');
      expect(pull, {'type': 'clipboard.pull', 'requestId': 'pullc-1', 'offerId': id, 'index': 0, 'offset': 0});
      expect(session.pulling, isTrue);
      expect(session.progress!.sending, isFalse);
      expect(session.progress!.fileCount, 2);
      expect(session.progress!.folderCount, 1);
      expect(session.progress!.total, 170000);
      // A second range waits for the first (the relay allows one pull at a time).
      bridge.platform(ClipboardPullRequest(12, id, 1, 0));
      await base.settle();
      expect(sentOf('clipboard.pull'), isEmpty);
      final first = Uint8List.fromList(List.generate(100000, (i) => i % 200));
      for (var at = 0; at < first.length; at += 46080) {
        final part = first.sublist(at, at + 46080 > first.length ? first.length : at + 46080);
        socket.emit({'type': 'clipboard.chunk', 'requestId': 'pullc-1', 'index': 0, 'offset': at, 'data': base64Encode(part)});
      }
      await base.settle();
      expect([for (final d in bridge.data) d.$2], [0, 46080, 92160]);
      expect(bridge.data.every((d) => d.$1 == 11), isTrue);
      expect(session.progress!.transferred, 100000);
      // The platform's consumption drives the acknowledgement.
      bridge.platform(ClipboardPullAck(11, 0, 92160));
      final ack = await socket.nextMessage('clipboard.chunk.ack');
      expect(ack, {'type': 'clipboard.chunk.ack', 'requestId': 'pullc-1', 'index': 0, 'offset': 92160});
      socket.emit({'type': 'clipboard.item.end', 'requestId': 'pullc-1', 'index': 0, 'sha256': 'c' * 64});
      await base.settle();
      expect(bridge.ended, [(11, 'c' * 64)]);
      // The queued range starts now.
      final next = await socket.nextMessage('clipboard.pull');
      expect(next['requestId'], 'pullc-2');
      expect(next['index'], 1);
      // The platform ack of the finished range is not sent on.
      bridge.platform(ClipboardPullAck(11, 0, 100000));
      await base.settle();
      expect(sentOf('clipboard.chunk.ack'), isEmpty);
      final second = Uint8List(70000);
      socket.emit({'type': 'clipboard.chunk', 'requestId': 'pullc-2', 'index': 1, 'offset': 0, 'data': base64Encode(second.sublist(0, 46080))});
      socket.emit({'type': 'clipboard.chunk', 'requestId': 'pullc-2', 'index': 1, 'offset': 46080, 'data': base64Encode(second.sublist(46080))});
      socket.emit({'type': 'clipboard.item.end', 'requestId': 'pullc-2', 'index': 1, 'sha256': 'd' * 64});
      await base.settle();
      expect(bridge.ended.length, 2);
      expect(session.pulling, isFalse);
      expect(session.error, isNull);
      expect(session.transferring, isFalse);
    });

    test('Explorer\'s asynchronous operation marks the progress as shown by Windows; a reader without one is ours to show', () async {
      await connect();
      final id = await published();
      // A consumer that reads synchronously (no StartOperation): the progress is ours.
      bridge.platform(ClipboardPullRequest(21, id, 0, 0));
      await socket.nextMessage('clipboard.pull');
      expect(session.progress!.asyncConsumer, isFalse);
      expect(session.progress!.fileName, 'a.bin', reason: 'the leaf of Pics/a.bin');
      await session.cancelTransfer();
      await base.settle();
      // Explorer: StartOperation, then its own copy dialog shows the progress.
      bridge.platform(ClipboardPullOperation(true, offerId: id));
      bridge.platform(ClipboardPullRequest(22, id, 1, 0));
      await socket.nextMessage('clipboard.pull');
      expect(session.progress!.asyncConsumer, isTrue);
      expect(session.progress!.fileName, 'b.bin');
      // Repeated starts of the same native epoch are idempotent.
      bridge.platform(ClipboardPullOperation(true, offerId: id));
      bridge.platform(ClipboardPullOperation(false, offerId: id));
      expect(session.progress!.asyncConsumer, isFalse);
      bridge.platform(ClipboardPullOperation(false, offerId: id));
      expect(session.progress!.asyncConsumer, isFalse);
      // An EndOperation without a start never goes below zero.
      bridge.platform(ClipboardPullOperation(false, offerId: id));
      bridge.platform(ClipboardPullOperation(true, offerId: id, operationId: 2));
      expect(session.progress!.asyncConsumer, isTrue);
    });

    test('a range the platform gave up on is cancelled at the relay and the user hears why', () async {
      await connect();
      final id = await published();
      bridge.platform(ClipboardPullRequest(5, id, 1, 0));
      await socket.nextMessage('clipboard.pull');
      bridge.platform(const ClipboardPullCancel(5));
      bridge.platform(const ClipboardPullFailed(5, 1, 'clipboard_timeout'));
      final cancel = await socket.nextMessage('clipboard.cancel');
      expect(cancel['requestId'], 'pullc-1');
      expect(session.error, 'clipboard_timeout');
      expect(session.status, contains('시간'));
      expect(session.pulling, isFalse);
      // A cancelled range that was only queued sends nothing.
      bridge.platform(ClipboardPullRequest(6, id, 0, 0));
      await socket.nextMessage('clipboard.pull');
      bridge.platform(ClipboardPullRequest(7, id, 1, 0));
      await base.settle();
      bridge.platform(const ClipboardPullCancel(7));
      bridge.platform(const ClipboardPullCancel(6));
      await socket.nextMessage('clipboard.cancel');
      await base.settle();
      expect(sentOf('clipboard.pull'), isEmpty);
    });

    test('the source reporting a changed file, an expired offer or a cancel fails that range only', () async {
      await connect();
      final id = await published();
      for (final (reason, expected) in [
        ('clipboard_selected_file_changed', '바뀌'),
        ('clipboard_offer_expired', '남아'),
        ('clipboard_file_locked', '사용 중'),
      ]) {
        bridge.platform(ClipboardPullRequest(20, id, 0, 0));
        final pull = await socket.nextMessage('clipboard.pull');
        socket.emit({'type': 'clipboard.error', 'requestId': pull['requestId'], 'reason': reason});
        await base.settle();
        expect(bridge.failedRanges.last, (20, reason));
        expect(session.error, reason);
        expect(session.status, contains(expected));
        expect(session.ready, isTrue);
        socket.sent.clear();
      }
      bridge.platform(ClipboardPullRequest(21, id, 0, 0));
      final pull = await socket.nextMessage('clipboard.pull');
      socket.emit({'type': 'clipboard.cancel', 'requestId': pull['requestId']});
      await base.settle();
      expect(bridge.failedRanges.last, (21, 'clipboard_cancelled'));
      expect(session.ready, isTrue);
    });

    test('a pull of an unknown offer, or beyond the file, is refused locally', () async {
      await connect();
      final id = await published();
      bridge.platform(ClipboardPullRequest(1, 'old-offer', 0, 0));
      bridge.platform(ClipboardPullRequest(2, id, 5, 0));
      bridge.platform(ClipboardPullRequest(3, id, 0, 100001));
      await base.settle();
      expect(bridge.failedRanges.map((r) => r.$1), [1, 2, 3]);
      expect(sentOf('clipboard.pull'), isEmpty);
    });

    // 2026-11-09 edge-case audit (RDP keeps a paste's clipboard data locked): newer copies no longer end a
    // paste under way; the oldest offer no paste is reading leaves instead, and the pull goes on.
    test('two newer copies keep the offer a paste is still pulling from: its data goes on arriving, the channel stays up', () async {
      await connect();
      Future<String> offerAgain() async {
        final receiving = session.receiveRemote();
        final read = await socket.nextMessage('clipboard.read');
        final id = read['requestId'] as String;
        socket.emit(offerHeader(id));
        await base.settle();
        socket.emit({'type': 'clipboard.entries', 'requestId': id, 'start': 0, 'entries': wireEntries()});
        await socket.nextMessage('clipboard.applied');
        socket.emit({'type': 'clipboard.complete', 'requestId': id});
        await receiving;
        return id;
      }

      final first = await offerAgain();
      bridge.platform(ClipboardPullRequest(31, first, 0, 0));
      await socket.nextMessage('clipboard.pull');
      await offerAgain();
      await offerAgain();
      socket.emit({'type': 'clipboard.chunk', 'requestId': 'pullc-1', 'index': 0, 'offset': 0, 'data': base64Encode([1, 2, 3])});
      await base.settle();
      expect(session.ready, isTrue, reason: 'the channel must stay up');
      expect(bridge.failedRanges, isEmpty, reason: 'the paste under way was not ended');
      expect(bridge.data, hasLength(1));
      expect((bridge.data.single.$1, bridge.data.single.$2), (31, 0));
      expect(bridge.data.single.$3, [1, 2, 3]);
    });

    test('data that does not fit the active pull is a protocol violation', () async {
      for (final message in <Map<String, dynamic>>[
        {'type': 'clipboard.chunk', 'requestId': 'pullc-1', 'index': 0, 'offset': 7, 'data': base64Encode([1])},
        {'type': 'clipboard.chunk', 'requestId': 'pullc-1', 'index': 1, 'offset': 0, 'data': base64Encode([1])},
        {'type': 'clipboard.chunk', 'requestId': 'pullc-1', 'index': 0, 'offset': 0, 'data': 'AA'},
        {'type': 'clipboard.chunk', 'requestId': 'pullc-1', 'index': 0, 'offset': 0, 'data': base64Encode(Uint8List(100001))},
        {'type': 'clipboard.chunk', 'requestId': 'pullc-1', 'index': 0, 'offset': 0, 'data': base64Encode([1]), 'x': 1},
        {'type': 'clipboard.item.end', 'requestId': 'pullc-1', 'index': 0, 'sha256': 'e' * 64},
        {'type': 'clipboard.item.end', 'requestId': 'pullc-1', 'index': 0, 'sha256': 'XYZ'},
      ]) {
        socket = base.ClipboardSocket();
        bridge = PullClipboard();
        session = make();
        await connect();
        final id = await published();
        bridge.platform(ClipboardPullRequest(1, id, 0, 0));
        await socket.nextMessage('clipboard.pull');
        socket.emit(message);
        await base.settle();
        expect(session.ready, isFalse, reason: '$message');
        expect(session.error, 'clipboard_invalid_message');
        expect(bridge.ended, isEmpty);
      }
    });

    test('a platform that cannot take data ends that pull, not the channel', () async {
      await connect();
      final id = await published();
      bridge.platform(ClipboardPullRequest(1, id, 0, 0));
      await socket.nextMessage('clipboard.pull');
      bridge.dataFailure = PlatformException(code: 'clipboard_transfer_failed');
      socket.emit({'type': 'clipboard.chunk', 'requestId': 'pullc-1', 'index': 0, 'offset': 0, 'data': base64Encode([1, 2, 3])});
      final cancel = await socket.nextMessage('clipboard.cancel');
      expect(cancel['requestId'], 'pullc-1');
      expect(session.ready, isTrue);
      expect(session.error, 'clipboard_transfer_failed');
    });

    test('the notice cancel ends the running and the queued pulls', () async {
      await connect();
      final id = await published();
      bridge.platform(ClipboardPullRequest(1, id, 0, 0));
      await socket.nextMessage('clipboard.pull');
      bridge.platform(ClipboardPullRequest(2, id, 1, 0));
      await base.settle();
      await session.cancelTransfer();
      await socket.nextMessage('clipboard.cancel');
      expect(bridge.failedRanges, [(1, 'clipboard_cancelled'), (2, 'clipboard_cancelled')]);
      expect(session.pulling, isFalse);
      expect(session.ready, isTrue);
    });

    test('closing the session shuts the pulls down and retires the offers', () async {
      await connect();
      final id = await published();
      bridge.platform(ClipboardPullRequest(1, id, 0, 0));
      await socket.nextMessage('clipboard.pull');
      await session.close(reason: 'authorization_revoked');
      expect(bridge.shutdowns, ['authorization_revoked']);
      expect(bridge.offersClosed, isTrue);
      expect(session.pulling, isFalse);
    });

    test('ids rise and never repeat within the pair', () async {
      await connect();
      final id = await published();
      final seen = <String>[];
      for (var i = 0; i < 3; i++) {
        bridge.platform(ClipboardPullRequest(30 + i, id, 1, 0));
        final pull = await socket.nextMessage('clipboard.pull');
        seen.add(pull['requestId'] as String);
        socket.emit({'type': 'clipboard.error', 'requestId': pull['requestId'], 'reason': 'clipboard_cancelled'});
        await base.settle();
      }
      expect(seen, ['pullc-1', 'pullc-2', 'pullc-3']);
    });
  });
}

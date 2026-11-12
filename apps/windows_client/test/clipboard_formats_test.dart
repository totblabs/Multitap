import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/clipboard_window.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_session_test.dart' as base;

const mib = 1024 * 1024, gib = 1024 * mib;

/// The `limits` object a relay sends two peers that both announced the formats profile.
Map<String, dynamic> formatsLimits() => {
  'chunkBytes': 46080,
  'maxFiles': 128,
  'maxFileBytes': 4 * gib,
  'maxTotalBytes': 8 * gib,
  'maxTextUnits': mib,
  'maxTextBytes': mib,
  'limitsProfile': 2,
  'formatsProfile': 2,
  'maxEntries': 10000,
  'maxDepth': 32,
  'maxPathChars': 160,
  'maxRichBytes': 8 * mib,
  'maxImageBytes': 256 * mib,
  'maxImagePixels': 64000000,
  'windowBytes': mib,
  'windowUnits': 32,
};
Map<String, dynamic> formatsReady({Map<String, dynamic>? policy}) => {
  ...base.ready,
  'limits': formatsLimits(),
  'clipboardPolicy': ?policy,
};
Map<String, dynamic> v2Ready() => {
  ...base.ready,
  'limits': {
    'chunkBytes': 32768,
    'maxFiles': 128,
    'maxFileBytes': 4 * gib,
    'maxTotalBytes': 8 * gib,
    'maxTextUnits': mib,
    'maxTextBytes': mib,
    'limitsProfile': 2,
  },
};

/// The Windows adapter's stand-in: it can handle the formats profile.
class FormatsClipboard extends base.MemoryClipboard
    implements ClipboardFormatsBridge {
  @override
  bool get supportsFormats => true;
  bool formatsOn = false;
  @override
  set formats(bool value) => formatsOn = value;
  String? beganFormat;
  List<ClipboardItem> beganItems = const [];
  @override
  Future<void> begin(
    String owner,
    String requestId,
    String format,
    List<ClipboardItem> items,
  ) async {
    beganFormat = format;
    beganItems = items;
    // A tree's folders carry no data: the data items are its files, by ordinal.
    final data = items.where((item) => !item.isDirectory);
    await super.begin(owner, requestId, format, data.toList());
  }
}

ClipboardItem file(String path, int size) =>
    ClipboardItem(size: size, name: path, kind: 'file', attributes: 32);
ClipboardItem dir(String path) =>
    ClipboardItem(size: 0, name: path, kind: 'dir');

void main() {
  late base.ClipboardSocket socket;
  late FormatsClipboard bridge;
  late ClipboardSession session;

  ClipboardSession make({ClipboardPolicy policy = const ClipboardPolicy.all()}) =>
      ClipboardSession(
        policy: policy,
        url: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/${base.sessionId}/clipboard',
        ),
        token: 'test-token',
        sessionId: base.sessionId,
        bridge: bridge,
        dialer: (url, token) async => socket,
      );
  Future<void> connect([Map<String, dynamic>? ready]) async {
    final starting = session.start();
    await base.settle();
    socket.emit(ready ?? formatsReady());
    await starting;
  }

  setUp(() {
    socket = base.ClipboardSocket();
    bridge = FormatsClipboard();
    session = make();
  });
  tearDown(() async => session.close());

  List<Map<String, dynamic>> sentOf(String type) => [
    for (final value in socket.sent)
      if (value['type'] == type) value,
  ];
  Future<Map<String, dynamic>> pullRequest() async {
    final receiving = session.receiveRemote();
    receiving.ignore();
    return socket.nextMessage('clipboard.read');
  }

  group('negotiation', () {
    test('only the exact formats profile is accepted', () {
      expect(
        ClipboardLimits.parse(formatsLimits(), chunkBytes: 32768),
        same(ClipboardLimits.formatsProfile),
      );
      expect(ClipboardLimits.formatsProfile.formats, isTrue);
      expect(ClipboardLimits.formatsProfile.windowUnits, 32);
      expect(ClipboardLimits.formatsProfile.chunkBytes, 46080);
      // Released and extended-only pairs are stop and wait.
      for (final limits in [
        ClipboardLimits.v1,
        ClipboardLimits.v1Extended,
        ClipboardLimits.v2,
      ]) {
        expect(limits.formats, isFalse);
        expect(limits.windowUnits, 1);
        expect(limits.chunkBytes, 32768);
      }
      for (final change in <String, Object>{
        'maxEntries': 20000,
        'maxDepth': 64,
        'maxPathChars': 260,
        'maxRichBytes': 16 * mib,
        'maxImageBytes': gib,
        'maxImagePixels': 100000000,
        'windowBytes': 4 * mib,
        'windowUnits': 64,
        'formatsProfile': 3,
        'limitsProfile': 3,
        'chunkBytes': 65536,
        'maxFileBytes': 8 * gib,
      }.entries) {
        expect(
          ClipboardLimits.parse({
            ...formatsLimits(),
            change.key: change.value,
          }, chunkBytes: 32768),
          isNull,
          reason: change.key,
        );
      }
      expect(
        ClipboardLimits.parse(
          {...formatsLimits()}..remove('windowUnits'),
          chunkBytes: 32768,
        ),
        isNull,
      );
      expect(
        ClipboardLimits.parse(
          {...formatsLimits(), 'extra': 1},
          chunkBytes: 32768,
        ),
        isNull,
      );
    });

    test('the formats header is announced only by a bridge that can handle the formats', () {
      expect(session.dialHeaders, containsPair(clipboardFormatsHeader, '2'));
      expect(session.dialHeaders, containsPair(clipboardLimitsHeader, '2'));
      final plain = ClipboardSession(
        url: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/${base.sessionId}/clipboard',
        ),
        token: 't',
        sessionId: base.sessionId,
        bridge: base.MemoryClipboard(),
        dialer: (url, token) async => socket,
      );
      expect(plain.dialHeaders.containsKey(clipboardFormatsHeader), isFalse);
      expect(plain.dialHeaders, containsPair(clipboardLimitsHeader, '2'));
      expect(clipboardFormatsHeader, 'X-Multitap-Clipboard-Formats');
    });

    test('a negotiated profile switches the bridge on; an older peer keeps text and files', () async {
      await connect();
      expect(session.limits.formats, isTrue);
      expect(bridge.formatsOn, isTrue);
      await session.close();
      socket = base.ClipboardSocket();
      bridge = FormatsClipboard();
      session = make();
      await connect(v2Ready());
      expect(session.limits.formats, isFalse);
      expect(bridge.formatsOn, isFalse);
    });

    test('a peer without the formats bridge refuses a ready that offers the profile', () async {
      await session.close();
      final plainBridge = base.MemoryClipboard();
      session = ClipboardSession(
        policy: const ClipboardPolicy.all(),
        url: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/${base.sessionId}/clipboard',
        ),
        token: 't',
        sessionId: base.sessionId,
        bridge: plainBridge,
        dialer: (url, token) async => socket,
      );
      final starting = session.start();
      starting.ignore();
      await base.settle();
      socket.emit(formatsReady());
      await expectLater(starting, throwsA(base.code('clipboard_invalid_message')));
    });

    test('permission mapping: rich text is text, images and trees are files', () {
      const textOnly = ClipboardPolicy(textToRemote: true, textToLocal: true);
      const filesOnly = ClipboardPolicy(filesToRemote: true, filesToLocal: true);
      for (final toRemote in [true, false]) {
        expect(textOnly.allows('text', toRemote: toRemote), isTrue);
        expect(textOnly.allows('richtext', toRemote: toRemote), isTrue);
        for (final format in ['files', 'image', 'tree']) {
          expect(textOnly.allows(format, toRemote: toRemote), isFalse, reason: format);
          expect(filesOnly.allows(format, toRemote: toRemote), isTrue, reason: format);
        }
        expect(filesOnly.allows('richtext', toRemote: toRemote), isFalse);
        expect(const ClipboardPolicy.all().allows('bitmap', toRemote: toRemote), isFalse);
      }
    });
  });

  group('window and ack state machines', () {
    test('the window bounds units and bytes and acknowledgements are cumulative', () {
      final window = ClipboardSendWindow(maxUnits: 4, maxBytes: 100);
      window
        ..sentChunk(0, 40, 40)
        ..sentChunk(0, 80, 40);
      expect(window.canSendChunk(40), isFalse);
      expect(window.canSendChunk(20), isTrue);
      window
        ..sentChunk(0, 100, 20)
        ..sentItemEnd(0);
      expect(window.canSendItemEnd, isFalse);
      expect(window.canSendChunk(0), isFalse);
      expect(() => window.sentChunk(1, 10, 10), throwsA(isA<ClipboardWindowException>()));
      expect(() => window.ackChunk(0, 50), throwsA(isA<ClipboardWindowException>()));
      expect(window.ackChunk(0, 80), 80);
      expect(window.units, 2);
      expect(() => window.ackChunk(0, 80), throwsA(isA<ClipboardWindowException>()));
      expect(window.ackItem(0), 20);
      expect(window.isEmpty && window.bytes == 0, isTrue);
    });

    test('a pair that is not windowed has a window of one chunk', () {
      final window = ClipboardSendWindow.forLimits(ClipboardLimits.v2);
      expect(window.canSendChunk(32768), isTrue);
      window.sentChunk(0, 32768, 32768);
      expect(window.canSendChunk(1), isFalse);
      expect(window.canSendItemEnd, isFalse);
      window.ackChunk(0, 32768);
      expect(window.canSendItemEnd, isTrue);
    });

    test('the receiver acknowledges every chunk on stop and wait, every fourth or 128 KiB when windowed', () {
      final stopAndWait = ClipboardAckPolicy(windowed: false);
      expect(stopAndWait.chunk(1), isTrue);
      expect(stopAndWait.chunk(1), isTrue);
      final windowed = ClipboardAckPolicy(windowed: true);
      expect([for (var i = 0; i < 8; i++) windowed.chunk(1000)], [false, false, false, true, false, false, false, true]);
      expect(windowed.chunk(200 * 1024), isTrue);
      windowed.chunk(10);
      windowed.reset();
      expect(windowed.chunk(10), isFalse);
    });
  });

  group('sending', () {
    test('rich text goes out as text + HTML + RTF items with a window of chunks, committed only after every ack', () async {
      await connect();
      final html = Uint8List(100000)..fillRange(0, 100000, 104);
      bridge.captured = ClipboardCapture(
        format: 'richtext',
        items: [
          const ClipboardItem(size: 5, kind: 'text', units: 5),
          ClipboardItem(size: html.length, kind: 'html'),
          const ClipboardItem(size: 8, kind: 'rtf'),
        ],
      );
      bridge.source = [Uint8List(5), html, Uint8List(8)];
      final sending = session.sendLocal();
      final begin = await socket.nextMessage('clipboard.begin');
      expect(begin['format'], 'richtext');
      expect(begin['items'], [
        {'kind': 'text', 'size': 5},
        {'kind': 'html', 'size': 100000},
        {'kind': 'rtf', 'size': 8},
      ]);
      await base.settle();
      // Everything fits the window: no acknowledgement is needed to send it all.
      final chunks = sentOf('clipboard.chunk');
      expect(chunks.map((c) => [c['index'], c['offset']]), [
        [0, 0],
        [1, 0],
        [1, 46080],
        [1, 92160],
        [2, 0],
      ]);
      expect(sentOf('clipboard.item.end').map((e) => e['index']), [0, 1, 2]);
      expect(sentOf('clipboard.commit'), isEmpty, reason: 'nothing acknowledged yet');
      final id = begin['requestId'];
      // One cumulative ack for the chunks up to html offset 92160 + 7840.
      socket.emit({'type': 'clipboard.chunk.ack', 'requestId': id, 'index': 1, 'offset': 100000});
      await base.settle();
      expect(sentOf('clipboard.commit'), isEmpty);
      socket.emit({'type': 'clipboard.item.ack', 'requestId': id, 'index': 2});
      await base.settle();
      expect(sentOf('clipboard.commit'), hasLength(1), reason: 'the item ack of the last item releases every earlier unit');
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await sending;
      expect(session.error, isNull);
      expect(session.status, '클립보드 전송 완료');
    });

    test('the window stops at 1 MiB in flight and an ack lets the sender continue', () async {
      await connect();
      const size = 3 * mib;
      bridge.captured = ClipboardCapture(
        format: 'files',
        items: [ClipboardItem(size: size, name: 'big.bin')],
      );
      bridge.source = [Uint8List(size)];
      final sending = session.sendLocal();
      sending.ignore();
      final begin = await socket.nextMessage('clipboard.begin');
      final id = begin['requestId'];
      await base.settle();
      await base.settle();
      final inWindow = mib ~/ 46080;
      expect(sentOf('clipboard.chunk'), hasLength(inWindow));
      socket.emit({'type': 'clipboard.chunk.ack', 'requestId': id, 'index': 0, 'offset': 4 * 46080});
      await base.settle();
      await base.settle();
      expect(sentOf('clipboard.chunk').length, inWindow + 4);
      expect(session.progress?.transferred, 4 * 46080);
    });

    test('an acknowledgement for data that was never sent closes the channel', () async {
      await connect();
      bridge.captured = ClipboardCapture(
        format: 'files',
        items: [ClipboardItem(size: 200000, name: 'x.bin')],
      );
      bridge.source = [Uint8List(200000)];
      final sending = session.sendLocal();
      sending.ignore();
      final begin = await socket.nextMessage('clipboard.begin');
      await base.settle();
      socket.emit({'type': 'clipboard.chunk.ack', 'requestId': begin['requestId'], 'index': 0, 'offset': 123});
      await base.settle();
      expect(session.error, 'clipboard_invalid_message');
      expect(session.ready, isFalse);
    });

    test('a pair that is not windowed stays stop and wait with 32 KiB chunks', () async {
      await session.close();
      socket = base.ClipboardSocket();
      bridge = FormatsClipboard();
      session = make();
      await connect(v2Ready());
      bridge.captured = ClipboardCapture(
        format: 'files',
        items: [ClipboardItem(size: 100000, name: 'x.bin')],
      );
      bridge.source = [Uint8List(100000)];
      final sending = session.sendLocal();
      sending.ignore();
      final begin = await socket.nextMessage('clipboard.begin');
      await base.settle();
      expect(sentOf('clipboard.chunk'), hasLength(1));
      expect(base64Decode(sentOf('clipboard.chunk').single['data'] as String), hasLength(32768));
      socket.emit({'type': 'clipboard.chunk.ack', 'requestId': begin['requestId'], 'index': 0, 'offset': 32768});
      await base.settle();
      expect(sentOf('clipboard.chunk'), hasLength(2));
    });

    test('rich text, images and trees are refused before anything is sent on a pair without the profile', () async {
      await session.close();
      socket = base.ClipboardSocket();
      bridge = FormatsClipboard();
      session = make();
      await connect(v2Ready());
      for (final capture in [
        const ClipboardCapture(format: 'richtext', items: [ClipboardItem(size: 3, kind: 'text')]),
        const ClipboardCapture(format: 'image', items: [ClipboardItem(size: 3, kind: 'png', width: 1, height: 1)]),
        ClipboardCapture(format: 'tree', items: [dir('d')], tops: 1),
      ]) {
        bridge.captured = capture;
        await expectLater(session.sendLocal(), throwsA(isA<ClipboardTransferException>()));
        expect(sentOf('clipboard.begin'), isEmpty);
      }
    });

    test('an image carries its size in pixels and is bounded', () async {
      await connect();
      bridge.captured = const ClipboardCapture(
        format: 'image',
        items: [ClipboardItem(size: 10, kind: 'png', width: 3, height: 2)],
      );
      bridge.source = [Uint8List(10)];
      final sending = session.sendLocal();
      sending.ignore();
      final begin = await socket.nextMessage('clipboard.begin');
      expect(begin['items'], [{'kind': 'png', 'size': 10, 'width': 3, 'height': 2}]);
      await session.cancelTransfer();
      socket.emit({'type': 'clipboard.cancel', 'requestId': begin['requestId']});
      await base.settle();
      for (final item in [
        const ClipboardItem(size: 10, kind: 'png', width: 8001, height: 8000),
        const ClipboardItem(size: 256 * mib + 1, kind: 'png', width: 1, height: 1),
      ]) {
        bridge.captured = ClipboardCapture(format: 'image', items: [item]);
        await expectLater(session.sendLocal(), throwsA(base.code('clipboard_limits_exceeded')));
        expect(session.errorDetail, 'image');
        expect(session.status, contains('이미지가 너무 커요'));
      }
    });

    test('a folder tree is announced with counts and sent as entries, then its files by ordinal', () async {
      await connect();
      final entries = [
        dir('Docs'),
        file('Docs/a.txt', 70000),
        dir('Docs/Empty'),
        file('Docs/Sub/a.txt', 3),
      ];
      entries.insert(3, dir('Docs/Sub'));
      bridge.captured = ClipboardCapture(format: 'tree', items: entries, tops: 1, skipped: 2);
      bridge.source = [Uint8List(70000), Uint8List(3)];
      final sending = session.sendLocal();
      final begin = await socket.nextMessage('clipboard.begin');
      expect(begin, {
        'type': 'clipboard.begin',
        'requestId': begin['requestId'],
        'format': 'tree',
        'entries': 5,
        'files': 2,
        'bytes': 70003,
        'tops': 1,
        'skipped': 2,
      });
      await base.settle();
      final pages = sentOf('clipboard.entries');
      expect(pages, hasLength(1));
      expect(pages.single['start'], 0);
      expect((pages.single['entries'] as List).map((e) => (e as Map)['path']), [
        'Docs', 'Docs/a.txt', 'Docs/Empty', 'Docs/Sub', 'Docs/Sub/a.txt',
      ]);
      expect((pages.single['entries'] as List)[1], {'path': 'Docs/a.txt', 'kind': 'file', 'size': 70000, 'attrs': 32});
      expect((pages.single['entries'] as List)[0], {'path': 'Docs', 'kind': 'dir', 'attrs': 0});
      // File ordinals: 0 = Docs/a.txt (two chunks), 1 = Docs/Sub/a.txt.
      expect(sentOf('clipboard.chunk').map((c) => c['index']), [0, 0, 1]);
      final id = begin['requestId'];
      socket.emit({'type': 'clipboard.item.ack', 'requestId': id, 'index': 1});
      await base.settle();
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await sending;
      expect(session.status, contains('링크나 쓸 수 없는 이름 2개는 건너뛰었어요'));
      expect(session.progress, isNull);
    });

    test('a long tree list is sent in pages of at most 200 entries', () async {
      await connect();
      final entries = [dir('d'), for (var i = 0; i < 450; i++) file('d/f$i', 0)];
      bridge.captured = ClipboardCapture(format: 'tree', items: entries, tops: 1);
      bridge.source = [for (var i = 0; i < 450; i++) Uint8List(0)];
      final sending = session.sendLocal();
      sending.ignore();
      await socket.nextMessage('clipboard.begin');
      await base.settle();
      expect(sentOf('clipboard.entries').map((p) => [p['start'], (p['entries'] as List).length]), [
        [0, 200], [200, 200], [400, 51],
      ]);
    });

    test('hostile or oversized trees are refused before anything is sent', () async {
      await connect();
      Future<void> refuse(List<ClipboardItem> entries, int tops, {String? detail}) async {
        bridge.captured = ClipboardCapture(format: 'tree', items: entries, tops: tops);
        await expectLater(
          session.sendLocal(),
          throwsA(isA<ClipboardTransferException>()),
        );
        expect(sentOf('clipboard.begin'), isEmpty);
        if (detail != null) expect(session.errorDetail, detail);
      }

      await refuse([file('../x', 1)], 1);
      await refuse([file('/abs', 1)], 1);
      await refuse([file('C:', 1)], 1);
      await refuse([file('a\\b', 1)], 1);
      await refuse([file('a/b', 1)], 1); // parent missing
      await refuse([dir('a'), dir('A')], 2); // folded duplicate
      await refuse([dir('NUL')], 1);
      await refuse([dir('a'), file('a/b', 1)], 2); // wrong top-level count
      await refuse([
        for (var i = 0; i < 33; i++) dir(List.filled(i + 1, 'd').join('/')),
      ], 1, detail: 'depth');
      await refuse([
        dir('x' * 100),
        file('${'x' * 100}/${'y' * 100}', 1),
      ], 1, detail: 'path:201'); // the longest relative path is named
      await refuse([
        for (var i = 0; i < 10001; i++) file('f$i', 0),
      ], 10001, detail: 'entries');
      await refuse([for (var i = 0; i < 129; i++) dir('t$i')], 129, detail: 'count');
      await refuse([file('big', 4 * gib + 1)], 1, detail: 'file');
    });
  });

  group('receiving', () {
    test('rich text arrives as items; windowed acks are cumulative and the item end is acknowledged on its own', () async {
      await connect();
      final read = await pullRequest();
      final id = read['requestId'];
      const html = 110000;
      socket.emit({
        'type': 'clipboard.begin',
        'requestId': id,
        'format': 'richtext',
        'items': [
          {'kind': 'text', 'size': 3},
          {'kind': 'html', 'size': html},
        ],
      });
      await base.settle();
      expect(bridge.beganFormat, 'richtext');
      expect(bridge.beganItems.map((i) => i.kind), ['text', 'html']);
      socket.emit({'type': 'clipboard.chunk', 'requestId': id, 'index': 0, 'offset': 0, 'data': base64Encode([97, 98, 99])});
      socket.emit({'type': 'clipboard.item.end', 'requestId': id, 'index': 0, 'sha256': base.digest});
      await base.settle();
      expect(sentOf('clipboard.chunk.ack'), isEmpty, reason: 'one chunk of a windowed pair is acknowledged by the item ack');
      expect(sentOf('clipboard.item.ack'), hasLength(1));
      for (var i = 0; i < 3; i++) {
        final length = i < 2 ? 46080 : html - 2 * 46080;
        socket.emit({'type': 'clipboard.chunk', 'requestId': id, 'index': 1, 'offset': i * 46080, 'data': base64Encode(Uint8List(length))});
        await base.settle();
      }
      expect(sentOf('clipboard.chunk.ack'), isEmpty, reason: 'three chunks are below the ack threshold of four');
      socket.emit({'type': 'clipboard.item.end', 'requestId': id, 'index': 1, 'sha256': base.digest});
      socket.emit({'type': 'clipboard.commit', 'requestId': id});
      await base.settle();
      expect(sentOf('clipboard.applied'), hasLength(1));
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await base.settle();
      expect(session.error, isNull);
      expect(bridge.published, isTrue);
    });

    test('a windowed receiver acknowledges after four chunks, or earlier once 128 KiB are unacknowledged', () async {
      await connect();
      final read = await pullRequest();
      final id = read['requestId'];
      socket.emit({
        'type': 'clipboard.begin',
        'requestId': id,
        'format': 'image',
        'items': [
          {'kind': 'png', 'size': 40000 + 3 * 46080, 'width': 10, 'height': 10},
        ],
      });
      await base.settle();
      for (var i = 0; i < 4; i++) {
        socket.emit({'type': 'clipboard.chunk', 'requestId': id, 'index': 0, 'offset': i * 10000, 'data': base64Encode(Uint8List(10000))});
        await base.settle();
        expect(sentOf('clipboard.chunk.ack'), hasLength(i < 3 ? 0 : 1));
      }
      expect(sentOf('clipboard.chunk.ack').single, {
        'type': 'clipboard.chunk.ack', 'requestId': id, 'index': 0, 'offset': 40000,
      });
      // Three 45 KiB chunks already exceed 128 KiB: acknowledged at the third.
      for (var i = 0; i < 3; i++) {
        socket.emit({'type': 'clipboard.chunk', 'requestId': id, 'index': 0, 'offset': 40000 + i * 46080, 'data': base64Encode(Uint8List(46080))});
        await base.settle();
      }
      expect(sentOf('clipboard.chunk.ack').last['offset'], 40000 + 3 * 46080);
      expect(sentOf('clipboard.chunk.ack'), hasLength(2));
    });

    test('a folder tree is checked entry by entry, then handed to the native side with its folders', () async {
      await connect();
      final read = await pullRequest();
      final id = read['requestId'];
      socket.emit({
        'type': 'clipboard.begin',
        'requestId': id,
        'format': 'tree',
        'entries': 4,
        'files': 2,
        'bytes': 8,
        'tops': 2,
        'skipped': 3,
      });
      await base.settle();
      expect(bridge.beganFormat, isNull, reason: 'the entries are not complete yet');
      socket.emit({
        'type': 'clipboard.entries',
        'requestId': id,
        'start': 0,
        'entries': [
          {'path': 'Docs', 'kind': 'dir', 'attrs': 0},
          {'path': 'Docs/a.txt', 'kind': 'file', 'size': 5, 'attrs': 32},
        ],
      });
      await base.settle();
      expect(bridge.beganFormat, isNull);
      socket.emit({
        'type': 'clipboard.entries',
        'requestId': id,
        'start': 2,
        'entries': [
          {'path': 'Docs/Empty', 'kind': 'dir', 'attrs': 0},
          {'path': 'a.txt', 'kind': 'file', 'size': 3, 'attrs': 0},
        ],
      });
      await base.settle();
      expect(bridge.beganFormat, 'tree');
      expect(bridge.beganItems.map((i) => i.name), ['Docs', 'Docs/a.txt', 'Docs/Empty', 'a.txt']);
      expect(bridge.beganItems.where((i) => i.isDirectory), hasLength(2));
      expect(session.progress?.folderCount, 2);
      expect(session.progress?.fileCount, 2);
      socket.emit({'type': 'clipboard.chunk', 'requestId': id, 'index': 0, 'offset': 0, 'data': base64Encode(Uint8List(5))});
      socket.emit({'type': 'clipboard.item.end', 'requestId': id, 'index': 0, 'sha256': base.digest});
      socket.emit({'type': 'clipboard.chunk', 'requestId': id, 'index': 1, 'offset': 0, 'data': base64Encode(Uint8List(3))});
      socket.emit({'type': 'clipboard.item.end', 'requestId': id, 'index': 1, 'sha256': base.digest});
      socket.emit({'type': 'clipboard.commit', 'requestId': id});
      await base.settle();
      await base.settle();
      expect(sentOf('clipboard.item.ack').map((e) => e['index']), [0, 1]);
      expect(sentOf('clipboard.applied'), hasLength(1));
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await base.settle();
      expect(session.status, contains('링크나 쓸 수 없는 이름 3개는 건너뛰었어요'));
    });

    test('hostile entries, wrong totals and unannounced formats close the channel', () async {
      Future<void> expectClosed(Future<void> Function(String id) act, {Map<String, dynamic>? ready}) async {
        await session.close();
        socket = base.ClipboardSocket();
        bridge = FormatsClipboard();
        session = make();
        await connect(ready);
        final id = (await pullRequest())['requestId'] as String;
        await act(id);
        await base.settle();
        expect(session.error, isNotNull);
        expect(session.ready, isFalse);
        expect(bridge.beganFormat, isNull);
      }

      Future<void> tree(String id, List<Map<String, Object?>> entries, {int bytes = 1, int files = 1, int tops = 1}) async {
        socket.emit({'type': 'clipboard.begin', 'requestId': id, 'format': 'tree', 'entries': entries.length, 'files': files, 'bytes': bytes, 'tops': tops, 'skipped': 0});
        await base.settle();
        socket.emit({'type': 'clipboard.entries', 'requestId': id, 'start': 0, 'entries': entries});
      }

      await expectClosed((id) => tree(id, [{'path': '../evil', 'kind': 'file', 'size': 1, 'attrs': 0}]));
      await expectClosed((id) => tree(id, [{'path': 'a/b', 'kind': 'file', 'size': 1, 'attrs': 0}]));
      await expectClosed((id) => tree(id, [{'path': 'C:', 'kind': 'file', 'size': 1, 'attrs': 0}]));
      await expectClosed((id) => tree(id, [{'path': 'f', 'kind': 'file', 'size': 2, 'attrs': 0}]));
      await expectClosed((id) => tree(id, [{'path': 'f', 'kind': 'file', 'size': 1, 'attrs': 0x10}]));
      await expectClosed((id) => tree(id, [{'path': 'f', 'kind': 'link', 'size': 1, 'attrs': 0}]));
      await expectClosed((id) async {
        socket.emit({'type': 'clipboard.begin', 'requestId': id, 'format': 'tree', 'entries': 1, 'files': 1, 'bytes': 1, 'tops': 1, 'skipped': 0});
        await base.settle();
        socket.emit({'type': 'clipboard.entries', 'requestId': id, 'start': 5, 'entries': [{'path': 'f', 'kind': 'file', 'size': 1, 'attrs': 0}]});
      });
      // Not negotiated: the formats are invalid on a v2 pair.
      await expectClosed((id) async {
        socket.emit({'type': 'clipboard.begin', 'requestId': id, 'format': 'richtext', 'items': [{'kind': 'text', 'size': 1}]});
      }, ready: v2Ready());
      await expectClosed((id) async {
        socket.emit({'type': 'clipboard.begin', 'requestId': id, 'format': 'image', 'items': [{'kind': 'png', 'size': 1, 'width': 1, 'height': 1}]});
      }, ready: v2Ready());
      // Out-of-order rich text parts and an oversized image.
      await expectClosed((id) async {
        socket.emit({'type': 'clipboard.begin', 'requestId': id, 'format': 'richtext', 'items': [{'kind': 'html', 'size': 1}]});
      });
      await expectClosed((id) async {
        socket.emit({'type': 'clipboard.begin', 'requestId': id, 'format': 'richtext', 'items': [{'kind': 'text', 'size': 1}, {'kind': 'rtf', 'size': 1}, {'kind': 'html', 'size': 1}]});
      });
    });

    test('permissions decide by class: an image needs the files permission, rich text the text permission', () async {
      await session.close();
      socket = base.ClipboardSocket();
      bridge = FormatsClipboard();
      session = make();
      await connect(formatsReady(policy: {
        'textToRemote': true,
        'textToLocal': true,
        'filesToRemote': false,
        'filesToLocal': false,
      }));
      var id = (await pullRequest())['requestId'] as String;
      socket.emit({'type': 'clipboard.begin', 'requestId': id, 'format': 'image', 'items': [{'kind': 'png', 'size': 1, 'width': 1, 'height': 1}]});
      await base.settle();
      expect(session.ready, isFalse, reason: 'denied formats are a protocol violation of the relay');
      await session.close();
      socket = base.ClipboardSocket();
      bridge = FormatsClipboard();
      session = make();
      await connect(formatsReady(policy: {
        'textToRemote': false,
        'textToLocal': false,
        'filesToRemote': true,
        'filesToLocal': true,
      }));
      id = (await pullRequest())['requestId'] as String;
      socket.emit({'type': 'clipboard.begin', 'requestId': id, 'format': 'richtext', 'items': [{'kind': 'text', 'size': 1}]});
      await base.settle();
      expect(session.ready, isFalse);
    });
  });

  group('cancellation', () {
    test('cancelling a window of chunks sends one cancel and late acks are ignored', () async {
      await connect();
      bridge.captured = ClipboardCapture(
        format: 'files',
        items: [ClipboardItem(size: 3 * mib, name: 'big.bin')],
      );
      bridge.source = [Uint8List(3 * mib)];
      final sending = session.sendLocal();
      sending.ignore();
      final begin = await socket.nextMessage('clipboard.begin');
      await base.settle();
      final id = begin['requestId'];
      final before = sentOf('clipboard.chunk').length;
      await session.cancelTransfer();
      expect(sentOf('clipboard.cancel'), hasLength(1));
      // A cumulative ack that crossed the cancel does not start sending again and does not fail the channel.
      socket.emit({'type': 'clipboard.chunk.ack', 'requestId': id, 'index': 0, 'offset': 46080});
      await base.settle();
      expect(sentOf('clipboard.chunk').length, before);
      socket.emit({'type': 'clipboard.cancel', 'requestId': id});
      await base.settle();
      await expectLater(sending, throwsA(base.code('clipboard_cancelled')));
      expect(session.ready, isTrue);
      // The channel is usable again.
      bridge.captured = const ClipboardCapture(format: 'text', items: [ClipboardItem(size: 3)]);
      bridge.source = [Uint8List.fromList([97, 98, 99])];
      final again = session.sendLocal();
      again.ignore();
      expect((await socket.nextMessage('clipboard.begin'))['format'], 'text');
    });
  });
}

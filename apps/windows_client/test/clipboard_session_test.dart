import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_sync.dart';
import 'package:multitap_client/data/product_log.dart';

const digest =
    'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa';
const sessionId = 'clipboard-test';
const ready = {
  'type': 'clipboard.ready',
  'sessionId': sessionId,
  'protocol': 'clipboard-v1',
  'clipboardSync': false,
  'clipboardPolicy': {
    'textToRemote': true,
    'textToLocal': true,
    'filesToRemote': true,
    'filesToLocal': true,
  },
  'limits': {
    'chunkBytes': 32768,
    'maxFiles': 128,
    'maxFileBytes': 268435456,
    'maxTotalBytes': 536870912,
    'maxTextUnits': 32768,
  },
};

class ClipboardSocket extends Fake implements WebSocket {
  final incoming = StreamController<dynamic>();
  final sent = <Map<String, dynamic>>[];
  bool wasClosed = false;
  @override
  Duration? pingInterval;
  @override
  StreamSubscription<dynamic> listen(
    void Function(dynamic)? onData, {
    Function? onError,
    void Function()? onDone,
    bool? cancelOnError,
  }) => incoming.stream.listen(
    onData,
    onError: onError,
    onDone: onDone,
    cancelOnError: cancelOnError,
  );
  @override
  void add(dynamic data) =>
      sent.add(jsonDecode(data as String) as Map<String, dynamic>);
  void emit(Map<String, dynamic> value) => incoming.add(jsonEncode(value));
  @override
  Future<void> close([int? code, String? reason]) async {
    wasClosed = true;
    unawaited(incoming.close());
  }

  Future<Map<String, dynamic>> nextMessage(String type) async {
    for (var i = 0; i < 1000; i++) {
      final index = sent.indexWhere((value) => value['type'] == type);
      if (index >= 0) return sent.removeAt(index);
      await Future<void>.delayed(const Duration(milliseconds: 1));
    }
    throw StateError('No $type message');
  }
}

class MemoryClipboard implements ClipboardBridge {
  ClipboardCapture captured = const ClipboardCapture(
    format: 'text',
    items: [ClipboardItem(size: 3)],
  );
  List<Uint8List> source = [
    Uint8List.fromList([97, 98, 99]),
  ];
  final received = <BytesBuilder>[];
  final sourceReads = <int>[];
  final events = <String>[];
  bool published = false;
  Completer<void>? captureGate, appendGate, commitGate;
  Object? captureFailure, appendFailure, sourceFailure, commitFailure;
  @override
  Future<ClipboardCapture> capture(String owner, {bool skipOwn = false}) async {
    events.add('capture');
    await captureGate?.future;
    if (captureFailure != null) throw captureFailure!;
    return captured;
  }

  @override
  Future<Uint8List> sourceChunk(
    String owner,
    int index,
    int offset,
    int length,
  ) async {
    events.add('sourceChunk');
    sourceReads.add(length);
    if (sourceFailure != null) throw sourceFailure!;
    return Uint8List.sublistView(source[index], offset, offset + length);
  }

  @override
  Future<String> sourceDigest(String owner, int index) async => digest;
  @override
  Future<void> begin(
    String owner,
    String requestId,
    String format,
    List<ClipboardItem> items,
  ) async {
    events.add('begin');
    received.clear();
    received.addAll(items.map((_) => BytesBuilder()));
  }

  @override
  Future<void> append(
    String owner,
    String requestId,
    int index,
    int offset,
    Uint8List bytes,
  ) async {
    events.add('append');
    await appendGate?.future;
    if (appendFailure != null) throw appendFailure!;
    expect(received[index].length, offset);
    received[index].add(bytes);
  }

  @override
  Future<void> finish(
    String owner,
    String requestId,
    int index,
    String hash,
  ) async {
    events.add('finish');
    if (hash != digest) {
      throw PlatformException(code: 'clipboard_transfer_failed');
    }
  }

  @override
  Future<void> commit(
    String owner,
    String requestId, {
    int? unlessSequence,
  }) async {
    events.add('commit');
    await commitGate?.future;
    if (commitFailure != null) throw commitFailure!;
    published = true;
  }

  @override
  Future<void> cancel(String owner) async => events.add('cancel');
  @override
  Future<void> release(String owner) async => events.add('release');
}

/// A bridge that can claim the clipboard, recording when it did.
class ClaimingClipboard extends MemoryClipboard
    implements ClipboardClaimBridge {
  bool refuse = false;
  @override
  Future<int?> claim(String owner, String requestId) async {
    events.add('claim:$requestId');
    if (refuse) throw PlatformException(code: 'clipboard_busy');
    return 99;
  }
}

Future<void> settle() => Future<void>.delayed(const Duration(milliseconds: 5));
Matcher code(String value) => isA<ClipboardTransferException>().having(
  (failure) => failure.code,
  'code',
  value,
);

void main() {
  late ClipboardSocket socket;
  late MemoryClipboard bridge;
  late ClipboardSession session;
  var dials = 0;
  setUp(() {
    socket = ClipboardSocket();
    bridge = MemoryClipboard();
    dials = 0;
    session = ClipboardSession(
      policy: const ClipboardPolicy.all(),
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/$sessionId/clipboard',
      ),
      token: 'test-token',
      sessionId: sessionId,
      bridge: bridge,
      dialer: (url, token) async {
        dials++;
        expect(url.host, 'cloud.example.com');
        expect(token, 'test-token');
        return socket;
      },
    );
  });
  tearDown(() async => session.close());
  test('Closing an unopened clipboard session does not access the native clipboard bridge', () async {
    await session.close();
    expect(dials, 0);
    expect(bridge.events, isEmpty);
  });
  Future<void> connect() async {
    final starting = session.start();
    await settle();
    socket.emit(ready);
    await starting;
  }

  test(
    'An established transport closure permits a fresh pair after cleanup',
    () async {
      await connect();
      socket.incoming.addError(StateError('closed'));
      await settle();
      expect(session.closed, isTrue);
      expect(session.canReconnect, isTrue);
      await session.close();
      expect(bridge.events.last, 'release');
    },
  );

  test(
    'Explicit closure and relay policy rejection never request replacement',
    () async {
      await connect();
      socket.emit({
        'type': 'clipboard.error',
        'reason': 'authorization_revoked',
      });
      await settle();
      expect(session.closed, isTrue);
      expect(session.canReconnect, isFalse);
    },
  );

  test(
    'A rejected handshake is not treated as a transient connected socket',
    () async {
      await session.close();
      expect(session.canReconnect, isFalse);
      session = ClipboardSession(
        url: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/$sessionId/clipboard',
        ),
        token: 'test-token',
        sessionId: sessionId,
        bridge: bridge,
        dialer: (_, _) async => throw const WebSocketException('rejected'),
      );
      await expectLater(
        session.start(),
        throwsA(code('clipboard_transport_closed')),
      );
      expect(session.canReconnect, isFalse);
      expect(bridge.events.where((event) => event == 'capture'), isEmpty);
    },
  );

  test('Malformed peer traffic cannot request automatic replacement', () async {
    await connect();
    socket.incoming.add('not json');
    await settle();
    expect(session.closed, isTrue);
    expect(session.canReconnect, isFalse);
  });

  Future<void> ack(Map<String, dynamic> value, String type) async {
    socket.emit({
      'type': type,
      'requestId': value['requestId'],
      if (type == 'clipboard.chunk.ack' || type == 'clipboard.item.ack')
        'index': value['index'],
      if (type == 'clipboard.chunk.ack')
        'offset':
            (value['offset'] as int) +
            base64Decode(value['data'] as String).length,
    });
    await settle();
  }

  test('Negotiated clipboard supports more than 256 copies and an evicted reply cannot affect a fresh transfer', () async {
    session = ClipboardSession(
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
    String? first;
    String? prefix;
    for (var n = 1; n <= 300; n++) {
      final operation = session.sendLocal();
      final begin = await socket.nextMessage('clipboard.begin');
      final id = begin['requestId'] as String;
      first ??= id;
      expect(RegExp(r'^[0-9a-f]{32}-[0-9a-f]{16}$').hasMatch(id), isTrue);
      prefix ??= id.substring(0, 32);
      expect(id.substring(0, 32), prefix);
      expect(int.parse(id.substring(33), radix: 16), n);
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
    }
    final current = session.sendLocal();
    final begin = await socket.nextMessage('clipboard.begin');
    final chunk = await socket.nextMessage('clipboard.chunk');
    for (final type in [
      'clipboard.complete',
      'clipboard.cancel',
      'clipboard.error',
      'clipboard.chunk.ack',
    ]) {
      socket.emit({
        'type': type,
        'requestId': first,
        'reason': 'clipboard_cancelled',
        'index': 0,
        'offset': 3,
      });
    }
    await settle();
    expect(session.closed, isFalse);
    expect(session.busy, isTrue);
    expect((begin['requestId'] as String).substring(33), '000000000000012d');
    await ack(chunk, 'clipboard.chunk.ack');
    await ack(
      await socket.nextMessage('clipboard.item.end'),
      'clipboard.item.ack',
    );
    await ack(
      await socket.nextMessage('clipboard.commit'),
      'clipboard.complete',
    );
    await current;
    expect(session.busy, isFalse);
    expect(session.closed, isFalse);
  });

  Future<String> receiveBegin({
    String format = 'text',
    List<ClipboardItem>? files,
    int size = 3,
  }) async {
    final request = await socket.nextMessage('clipboard.read');
    final id = request['requestId'] as String;
    socket.emit({
      'type': 'clipboard.begin',
      'requestId': id,
      'format': format,
      if (format == 'text') 'size': size,
      if (format == 'files')
        'files': [
          for (final item in files!) {'name': item.name, 'size': item.size},
        ],
    });
    await settle();
    return id;
  }

  Future<void> receiveItem(String id, int index, List<int> data) async {
    for (var offset = 0; offset < data.length;) {
      final end = offset + 32768 < data.length ? offset + 32768 : data.length;
      socket.emit({
        'type': 'clipboard.chunk',
        'requestId': id,
        'index': index,
        'offset': offset,
        'data': base64Encode(data.sublist(offset, end)),
      });
      final received = await socket.nextMessage('clipboard.chunk.ack');
      expect(received['offset'], end);
      offset = end;
    }
    socket.emit({
      'type': 'clipboard.item.end',
      'requestId': id,
      'index': index,
      'sha256': digest,
    });
    expect((await socket.nextMessage('clipboard.item.ack'))['index'], index);
  }

  test(
    'Legacy automatic file capture sends no manifest or file bytes',
    () async {
      bridge.captured = const ClipboardCapture(
        format: 'files',
        items: [ClipboardItem(name: 'a.bin', size: 3)],
      );
      await connect();
      await session.setSharingActive(true);
      await expectLater(
        session.sendLocal(automatic: true),
        throwsA(code('clipboard_policy_denied')),
      );
      expect(
        socket.sent.where(
          (m) =>
              m['type'] == 'clipboard.begin' || m['type'] == 'clipboard.chunk',
        ),
        isEmpty,
      );
      expect(bridge.events.where((event) => event == 'sourceChunk'), isEmpty);
      expect(session.ready, isTrue);
      expect(session.closed, isFalse);
    },
  );

  test('Legacy automatic remote read fails closed before requesting unknown content', () async {
    await connect();
    await expectLater(
      session.receiveRemote(automatic: true),
      throwsA(code('clipboard_policy_denied')),
    );
    expect(socket.sent.where((m) => m['type'] == 'clipboard.read'), isEmpty);
    expect(bridge.events, isEmpty);
    // An explicit manual receive remains a user action on an old peer.
    final next = session.receiveRemote();
    final nextId = await receiveBegin(size: 3);
    await receiveItem(nextId, 0, [97, 98, 99]);
    socket.emit({'type': 'clipboard.commit', 'requestId': nextId});
    await socket.nextMessage('clipboard.applied');
    socket.emit({'type': 'clipboard.complete', 'requestId': nextId});
    await next;
    expect(bridge.published, isTrue);
    expect(session.closed, isFalse);
  });
  test('Paired readiness does not access the native clipboard or transmit paths or bearer tokens', () async {
    await connect();
    expect(session.ready, isTrue);
    expect(session.busy, isFalse);
    expect(bridge.events, isEmpty);
    expect(socket.sent, isEmpty);
    expect(dials, 1);
    await session.start();
    expect(dials, 1);
  });

  test('An idle transfer times out alone and the next copy succeeds on the same pair', () async {
    await session.close();
    session = ClipboardSession(
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/$sessionId/clipboard',
      ),
      token: 'test-token',
      sessionId: sessionId,
      bridge: bridge,
      policy: const ClipboardPolicy.all(),
      dialer: (_, _) async { dials++; return socket; },
      idleTimeout: const Duration(milliseconds: 1),
    );
    await connect();
    final first = session.sendLocal();
    final failed = expectLater(first, throwsA(code('clipboard_timeout')));
    final old = await socket.nextMessage('clipboard.begin');
    await socket.nextMessage('clipboard.chunk');
    await failed;
    expect(
      (await socket.nextMessage('clipboard.cancel'))['requestId'],
      old['requestId'],
    );
    expect(session.closed, isFalse);
    expect(session.ready, isTrue);
    final next = session.sendLocal();
    await socket.nextMessage('clipboard.begin');
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
    await next;
    expect(session.error, isNull);
    expect(dials, 1);
  });

  test(
    'Readiness binds the exact session and negotiated protocol limits',
    () async {
      final result = expectLater(
        session.start(),
        throwsA(code('clipboard_invalid_message')),
      );
      await settle();
      socket.emit({...ready, 'sessionId': 'another-session'});
      await result;
      expect(session.closed, isTrue);
      expect(bridge.events.where((value) => value != 'release'), isEmpty);
    },
  );

  test('Push uses one 32 KiB chunk at a time, native digest, and completes only after remote applied', () async {
    final bytes = Uint8List.fromList(utf8.encode('한글 abc\r\n' * 6000));
    bridge.captured = ClipboardCapture(
      format: 'text',
      items: [ClipboardItem(size: bytes.length)],
    );
    bridge.source = [bytes];
    await connect();
    final sending = session.sendLocal();
    final begin = await socket.nextMessage('clipboard.begin');
    expect(begin['size'], bytes.length);
    final chunks = BytesBuilder();
    while (chunks.length < bytes.length) {
      final part = await socket.nextMessage('clipboard.chunk');
      chunks.add(base64Decode(part['data'] as String));
      expect(
        socket.sent.where((value) => value['type'] == 'clipboard.chunk'),
        isEmpty,
      );
      expect(bridge.sourceReads.length, (chunks.length + 32767) ~/ 32768);
      await ack(part, 'clipboard.chunk.ack');
    }
    expect(chunks.takeBytes(), bytes);
    expect(bridge.sourceReads.every((size) => size <= 32768), isTrue);
    final end = await socket.nextMessage('clipboard.item.end');
    expect(end['sha256'], digest);
    await ack(end, 'clipboard.item.ack');
    final commit = await socket.nextMessage('clipboard.commit');
    expect(session.busy, isTrue);
    await ack(commit, 'clipboard.complete');
    await sending;
    expect(session.transferredBytes, bytes.length);
    expect(session.busy, isFalse);
    expect(bridge.events.last, 'release');
  });

  List<Map<String, Object>> failedTransfers(List<ProductLogEntry> records) => [
    for (final entry in records)
      if (entry.event == LogEvent.clipboard &&
          entry.values['action'] == 2 &&
          entry.values['ok'] == false)
        entry.values,
  ];

  test('A failure inside the native chunk read is attributed to its native step in the diagnostics (origin 1, step 2)', () async {
    final records = <ProductLogEntry>[];
    final recording = ProductLog.instance.localRecords.listen(records.add);
    addTearDown(recording.cancel);
    // The generic code alone said nothing about which side failed (field failure 2026-10-27).
    bridge.captured = const ClipboardCapture(
      format: 'files',
      items: [ClipboardItem(name: 'a.bin', size: 3)],
    );
    bridge.source = [
      Uint8List.fromList([1, 2, 3]),
    ];
    bridge.sourceFailure = PlatformException(code: 'clipboard_transfer_failed');
    await connect();
    await expectLater(
      session.sendLocal(),
      throwsA(code('clipboard_transfer_failed')),
    );
    final failure = failedTransfers(records).single;
    expect(failure['stage'], 2, reason: 'files');
    expect(failure['remote'], isTrue);
    expect(
      failure['error'],
      12,
      reason: 'origin 1 (native) * 10 + step 2 (read a chunk)',
    );
  });

  test('An error the peer reports through the relay is attributed to the peer (origin 3), not to this client', () async {
    final records = <ProductLogEntry>[];
    final recording = ProductLog.instance.localRecords.listen(records.add);
    addTearDown(recording.cancel);
    await connect();
    final sending = expectLater(
      session.sendLocal(),
      throwsA(code('clipboard_write_failed')),
    );
    final begin = await socket.nextMessage('clipboard.begin');
    socket.emit({
      'type': 'clipboard.error',
      'requestId': begin['requestId'],
      'reason': 'clipboard_write_failed',
    });
    await sending;
    final failure = failedTransfers(records).single;
    expect((failure['error'] as int) ~/ 10, 3);
  });

  test('A channel that closes mid-transfer is attributed to the transport (origin 4)', () async {
    final records = <ProductLogEntry>[];
    final recording = ProductLog.instance.localRecords.listen(records.add);
    addTearDown(recording.cancel);
    bridge.source = [
      Uint8List.fromList([1, 2, 3]),
    ];
    bridge.captured = const ClipboardCapture(
      format: 'files',
      items: [ClipboardItem(name: 'a.bin', size: 3)],
    );
    bridge.sourceFailure = null;
    await connect();
    final sending = session.sendLocal();
    final begin = await socket.nextMessage('clipboard.begin');
    await socket.nextMessage('clipboard.chunk');
    socket.incoming.addError(StateError('closed'));
    await expectLater(sending, throwsA(code('clipboard_transport_closed')));
    expect(begin['format'], 'files');
    final failure = failedTransfers(records).single;
    expect((failure['error'] as int) ~/ 10, 4);
  });
  test('A failed write of this PC clipboard keeps its Win32 detail in the diagnostics (cause * 100 + origin 1 * 10 + step 7) and its own reason', () async {
    final records = <ProductLogEntry>[];
    final recording = ProductLog.instance.localRecords.listen(records.add);
    addTearDown(recording.cancel);
    await connect();
    // 0x10005: ERROR_ACCESS_DENIED while another program held the clipboard open (writeFailureDetail bits 16-17 = 1).
    bridge.commitFailure = PlatformException(
      code: 'clipboard_write_failed',
      message: 'win32:65541',
    );
    final receiving = session.receiveRemote();
    final expectation = expectLater(
      receiving,
      throwsA(code('clipboard_write_failed')),
    );
    final id = await receiveBegin(size: 3);
    await receiveItem(id, 0, [97, 98, 99]);
    socket.emit({'type': 'clipboard.commit', 'requestId': id});
    await expectation;
    final failure = failedTransfers(records).single;
    expect(failure['remote'], isFalse);
    expect(failure['error'], 65541 * 100 + 1 * 10 + 7);
    expect(ClipboardSession.win32Detail('win32:65541'), 65541);
    expect(
      ClipboardSession.win32Detail('Windows clipboard operation failed.'),
      0,
    );
    expect(ClipboardSession.win32Detail(null), 0);
  });

  List<Map<String, Object>> closedChannels(List<ProductLogEntry> records) => [
    for (final entry in records)
      if (entry.event == LogEvent.clipboard &&
          entry.values['action'] == 3 &&
          entry.values['ok'] == false)
        entry.values,
  ];

  test('What ended the channel is recorded: a message the session cannot accept (1), a broken socket (4), the relay ending it (6)', () async {
    final records = <ProductLogEntry>[];
    final recording = ProductLog.instance.localRecords.listen(records.add);
    addTearDown(recording.cancel);
    await connect();
    socket.emit({
      'type': 'clipboard.chunk',
      'requestId': 'nobody',
      'index': 0,
      'offset': 0,
      'data': 'AA==',
    });
    await settle();
    expect(session.ready, isFalse);
    expect(closedChannels(records).single['error'], 1);

    records.clear();
    socket = ClipboardSocket();
    bridge = MemoryClipboard();
    session = ClipboardSession(
      policy: const ClipboardPolicy.all(),
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/$sessionId/clipboard',
      ),
      token: 'test-token',
      sessionId: sessionId,
      bridge: bridge,
      dialer: (url, token) async => socket,
    );
    await connect();
    socket.incoming.addError(StateError('closed'));
    await settle();
    expect(closedChannels(records).single['error'], 4);

    records.clear();
    socket = ClipboardSocket();
    session = ClipboardSession(
      policy: const ClipboardPolicy.all(),
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/$sessionId/clipboard',
      ),
      token: 'test-token',
      sessionId: sessionId,
      bridge: bridge,
      dialer: (url, token) async => socket,
    );
    await connect();
    socket.emit({'type': 'clipboard.error', 'reason': 'clipboard_timeout'});
    await settle();
    expect(closedChannels(records).single['error'], 6);
  });

  // Tuning #7: normal outcomes are ok, their code kept as information.
  test('Nothing to send and a session end are ok records; a read failure stays a failure', () async {
    final records = <ProductLogEntry>[];
    final recording = ProductLog.instance.localRecords.listen(records.add);
    addTearDown(recording.cancel);
    ProductLogEntry last(int action) => records.lastWhere(
      (e) => e.event == LogEvent.clipboard && e.values['action'] == action,
    );
    await connect();
    // The local clipboard holds nothing this client shares (field: 69 in 24 h).
    bridge.captureFailure = PlatformException(code: 'clipboard_empty');
    await expectLater(session.sendLocal(), throwsA(code('clipboard_empty')));
    expect(last(2).code, 'clipboard_empty');
    expect(last(2).values['ok'], isTrue);
    expect(last(2).values['error'], 11, reason: 'native (1) * 10 + capture (1)');
    for (final normal in [
      'unsupported_clipboard_format',
      'clipboard_policy_denied',
      'clipboard_changed',
      'clipboard_cancelled',
    ]) {
      bridge.captureFailure = PlatformException(code: normal);
      await expectLater(session.sendLocal(), throwsA(code(normal)));
      expect(last(2).values['ok'], isTrue, reason: normal);
    }
    expect(failedTransfers(records), isEmpty);
    bridge.captureFailure = PlatformException(code: 'clipboard_read_failed');
    await expectLater(session.sendLocal(), throwsA(code('clipboard_read_failed')));
    expect(failedTransfers(records).single['error'], 11);
    // The relay ends every channel of an ended session with session_ended.
    socket.emit({'type': 'clipboard.error', 'reason': 'session_ended'});
    await settle();
    expect(last(3).code, 'session_ended');
    expect(last(3).values['ok'], isTrue);
    expect(closedChannels(records), isEmpty);
  });

  test('Automatic copies ignored by design are ok; given up or blocked ones are failures', () {
    for (final reason in [
      ClipboardIgnored.inactive,
      ClipboardIgnored.denied,
      ClipboardIgnored.duplicate,
      ClipboardIgnored.readAfterLeaving,
      ClipboardIgnored.superseded,
    ]) {
      expect(ClipboardSession.ignoredIsOk(reason, 0), isTrue, reason: '$reason');
    }
    expect(ClipboardSession.ignoredIsOk(ClipboardIgnored.notStarted, 3), isTrue);
    for (final error in [1, 2, 4]) {
      expect(ClipboardSession.ignoredIsOk(ClipboardIgnored.notStarted, error), isFalse);
    }
    expect(ClipboardSession.ignoredIsOk(ClipboardIgnored.gaveUp, 0), isFalse);
  });

  test('Pull preserves Korean UTF-8 chunks and commits native clipboard only after every hash', () async {
    await connect();
    final text = '한' * 32768;
    final bytes = utf8.encode(text);
    final receiving = session.receiveRemote();
    final id = await receiveBegin(size: bytes.length);
    expect(bridge.events, ['begin']);
    await receiveItem(id, 0, bytes);
    expect(bridge.published, isFalse);
    socket.emit({'type': 'clipboard.commit', 'requestId': id});
    await socket.nextMessage('clipboard.applied');
    expect(utf8.decode(bridge.received.single.toBytes()), text);
    expect(bridge.published, isTrue);
    expect(session.busy, isTrue);
    socket.emit({'type': 'clipboard.complete', 'requestId': id});
    await receiving;
    expect(session.transferredBytes, 98304);
  });

  test('Pull materializes multiple regular file manifests including an empty file without accepting remote paths', () async {
    await connect();
    final receiving = session.receiveRemote();
    final id = await receiveBegin(
      format: 'files',
      files: const [
        ClipboardItem(name: '문서.txt', size: 3),
        ClipboardItem(name: 'empty.bin', size: 0),
      ],
    );
    await receiveItem(id, 0, [97, 98, 99]);
    await receiveItem(id, 1, []);
    socket.emit({'type': 'clipboard.commit', 'requestId': id});
    await socket.nextMessage('clipboard.applied');
    socket.emit({'type': 'clipboard.complete', 'requestId': id});
    await receiving;
    expect(bridge.received.map((item) => item.toBytes()).toList(), [
      [97, 98, 99],
      [],
    ]);
    expect(bridge.published, isTrue);
  });

  test(
    'Wrong offsets and noncanonical base64 never reach native append or commit',
    () async {
      await connect();
      final receiving = expectLater(
        session.receiveRemote(),
        throwsA(code('clipboard_invalid_message')),
      );
      final id = await receiveBegin(size: 1);
      socket.emit({
        'type': 'clipboard.chunk',
        'requestId': id,
        'index': 0,
        'offset': 0,
        'data': 'YR==',
      });
      await receiving;
      expect(bridge.events, ['begin', 'release']);
      expect(bridge.published, isFalse);
      expect(session.closed, isTrue);
    },
  );

  test('A hash rejected by native code fails this transfer and leaves video-independent clipboard session available', () async {
    await connect();
    final receiving = expectLater(
      session.receiveRemote(),
      throwsA(code('clipboard_transfer_failed')),
    );
    final id = await receiveBegin(size: 0);
    socket.emit({
      'type': 'clipboard.item.end',
      'requestId': id,
      'index': 0,
      'sha256': 'b' * 64,
    });
    final error = await socket.nextMessage('clipboard.error');
    expect(error['reason'], 'clipboard_transfer_failed');
    await receiving;
    expect(bridge.events, ['begin', 'finish', 'release']);
    expect(bridge.published, isFalse);
    expect(session.ready, isTrue);
    socket.emit(error); // Relay echoes are retired, not a second failure.
    await settle();
    expect(session.closed, isFalse);
  });

  test('Cancellation during a pending native append waits for cleanup and never sends an ACK or commits', () async {
    await connect();
    bridge.appendGate = Completer<void>();
    final receiving = expectLater(
      session.receiveRemote(),
      throwsA(code('clipboard_cancelled')),
    );
    final id = await receiveBegin();
    socket.emit({
      'type': 'clipboard.chunk',
      'requestId': id,
      'index': 0,
      'offset': 0,
      'data': 'YWJj',
    });
    await settle();
    await session.cancelTransfer();
    expect((await socket.nextMessage('clipboard.cancel'))['requestId'], id);
    expect(session.busy, isTrue);
    socket.emit({'type': 'clipboard.cancel', 'requestId': id});
    bridge.appendGate!.complete();
    await receiving;
    expect(
      socket.sent.where((value) => value['type'] == 'clipboard.chunk.ack'),
      isEmpty,
    );
    expect(bridge.published, isFalse);
    expect(bridge.events.last, 'release');
    expect(session.ready, isTrue);
  });

  test('Cancel before native capture returns sends no unreserved server transfer or native source bytes', () async {
    await connect();
    bridge.captureGate = Completer<void>();
    final sending = expectLater(
      session.sendLocal(),
      throwsA(code('clipboard_cancelled')),
    );
    await settle();
    final cancelling = session.cancelTransfer();
    await settle();
    expect(session.busy, isTrue);
    bridge.captureGate!.complete();
    await cancelling;
    await sending;
    expect(socket.sent, isEmpty);
    expect(bridge.events, ['capture', 'release']);
  });

  test('Transport loss during local commit reports uncertainty and does not revoke already published file paths', () async {
    await connect();
    bridge.commitGate = Completer<void>();
    final receiving = expectLater(
      session.receiveRemote(),
      throwsA(code('clipboard_commit_uncertain')),
    );
    final id = await receiveBegin(
      format: 'files',
      files: const [ClipboardItem(name: 'complete.txt', size: 0)],
    );
    await receiveItem(id, 0, []);
    socket.emit({'type': 'clipboard.commit', 'requestId': id});
    await settle();
    final closing = session.close();
    bridge.commitGate!.complete();
    await closing;
    await receiving;
    expect(bridge.published, isTrue);
    expect(bridge.events, ['begin', 'finish', 'commit', 'release']);
    expect(
      socket.sent.where((value) => value['type'] == 'clipboard.applied'),
      isEmpty,
    );
    expect(session.status, contains('확인'));
  });

  test(
    'Retired replies cannot apply data or finish a newer operation',
    () async {
      await connect();
      final first = expectLater(
        session.receiveRemote(),
        throwsA(code('clipboard_empty')),
      );
      final id = (await socket.nextMessage('clipboard.read'))['requestId'];
      socket.emit({
        'type': 'clipboard.error',
        'requestId': id,
        'reason': 'clipboard_empty',
      });
      await first;
      final second = expectLater(
        session.receiveRemote(),
        throwsA(code('clipboard_cancelled')),
      );
      final newId = (await socket.nextMessage('clipboard.read'))['requestId'];
      expect(newId, isNot(id));
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      socket.emit({
        'type': 'clipboard.begin',
        'requestId': id,
        'format': 'text',
        'size': 0,
      });
      await settle();
      expect(bridge.events.where((value) => value == 'begin'), isEmpty);
      expect(session.busy, isTrue);
      await session.cancelTransfer();
      socket.emit({'type': 'clipboard.cancel', 'requestId': newId});
      await second;
      expect(session.ready, isTrue);
    },
  );

  test('Concurrent commands are rejected and an unavailable native clipboard does not create a relay transfer', () async {
    await connect();
    bridge.captureGate = Completer<void>();
    bridge.captureFailure = PlatformException(
      code: 'clipboard_empty',
      message: 'private path must not be sent',
    );
    final sending = expectLater(
      session.sendLocal(),
      throwsA(code('clipboard_empty')),
    );
    await settle();
    await expectLater(session.receiveRemote(), throwsA(code('clipboard_busy')));
    bridge.captureGate!.complete();
    await sending;
    expect(socket.sent, isEmpty);
    expect(session.ready, isTrue);
    expect(session.error, 'clipboard_empty');
  });

  test('An automatic receive that cannot start is a not-started refusal and leaves an action 9 record; a manual one stays silent', () async {
    final records = <ProductLogEntry>[];
    final recording = ProductLog.instance.localRecords.listen(records.add);
    addTearDown(recording.cancel);
    await connect();
    bridge.captureGate = Completer<void>();
    final sending = session.sendLocal().catchError((Object _) {});
    await settle();
    await expectLater(
      session.receiveRemote(automatic: true),
      throwsA(isA<ClipboardNotStartedException>()),
    );
    await expectLater(
      session.receiveRemote(),
      throwsA(isA<ClipboardNotStartedException>()),
    );
    final refused = [
      for (final entry in records)
        if (entry.event == LogEvent.clipboard && entry.values['action'] == 9)
          entry.values,
    ];
    expect(
      refused,
      hasLength(1),
      reason: 'only the automatic attempt is recorded',
    );
    expect(refused.single['stage'], 4, reason: 'could not start');
    expect(refused.single['error'], 1, reason: 'busy');
    expect(refused.single['remote'], isTrue, reason: 'VM -> this PC');
    expect(refused.single['auto'], isTrue);
    // Retire the held capture without starting an unacknowledged relay send.
    bridge.captureFailure = PlatformException(code: 'clipboard_empty');
    bridge.captureGate!.complete();
    await sending;
  });
  test('Traversal in a file manifest and premature completion are rejected before native writes', () async {
    await connect();
    final receiving = expectLater(
      session.receiveRemote(),
      throwsA(code('clipboard_invalid_message')),
    );
    await receiveBegin(
      format: 'files',
      files: const [ClipboardItem(name: '../outside.txt', size: 0)],
    );
    await receiving;
    expect(bridge.events.where((value) => value != 'release'), isEmpty);
    expect(bridge.published, isFalse);
  });

  test('Dial completion after close cannot attach to a newer desktop or read its clipboard', () async {
    final gate = Completer<WebSocket>();
    session = ClipboardSession(
      policy: const ClipboardPolicy.all(),
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/$sessionId/clipboard',
      ),
      token: 'token',
      sessionId: sessionId,
      bridge: bridge,
      dialer: (_, _) => gate.future,
    );
    final starting = session.start();
    await session.close();
    gate.complete(socket);
    await starting;
    expect(socket.wasClosed, isTrue);
    expect(session.ready, isFalse);
    expect(bridge.events.where((value) => value != 'release'), isEmpty);
  });

  test('Actual loopback WebSocket preserves ACK flow and surfaces a transport close independently', () async {
    // Real socket framing and onDone cleanup. TLS verification is covered by
    // the separate broker WSS suite and is not claimed by this loopback test.
    await session.close();
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    final allBytes = BytesBuilder();
    final socketTasks = <Future<void>>[];
    final serverSubscription = server.listen((request) {
      expect(
        request.headers.value(HttpHeaders.authorizationHeader),
        'Bearer loopback-token',
      );
      socketTasks.add(() async {
        final peer = await WebSocketTransformer.upgrade(request);
        peer.add(jsonEncode(ready));
        await for (final message in peer) {
          final value = jsonDecode(message as String) as Map<String, dynamic>;
          final id = value['requestId'];
          switch (value['type']) {
            case 'clipboard.chunk':
              final bytes = base64Decode(value['data'] as String);
              expect(bytes.length, lessThanOrEqualTo(32768));
              allBytes.add(bytes);
              peer.add(
                jsonEncode({
                  'type': 'clipboard.chunk.ack',
                  'requestId': id,
                  'index': value['index'],
                  'offset': (value['offset'] as int) + bytes.length,
                }),
              );
            case 'clipboard.item.end':
              peer.add(
                jsonEncode({
                  'type': 'clipboard.item.ack',
                  'requestId': id,
                  'index': value['index'],
                }),
              );
            case 'clipboard.commit':
              peer.add(
                jsonEncode({'type': 'clipboard.complete', 'requestId': id}),
              );
            case 'clipboard.read':
              await peer.close();
          }
        }
      }());
    });
    addTearDown(() async {
      await session.close();
      await serverSubscription.cancel();
      await server.close(force: true);
      await Future.wait(socketTasks);
    });
    final bytes = Uint8List.fromList(utf8.encode('한글 abc\r\n' * 6000));
    bridge.source = [bytes];
    bridge.captured = ClipboardCapture(
      format: 'text',
      items: [ClipboardItem(size: bytes.length)],
    );
    session = ClipboardSession(
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/$sessionId/clipboard',
      ),
      token: 'loopback-token',
      sessionId: sessionId,
      policy: const ClipboardPolicy.all(),
      bridge: bridge,
      dialer: (_, token) => WebSocket.connect(
        'ws://127.0.0.1:${server.port}/fixture',
        headers: {HttpHeaders.authorizationHeader: 'Bearer $token'},
      ),
    );
    await session.start();
    await session.sendLocal().timeout(const Duration(seconds: 3));
    expect(allBytes.takeBytes(), bytes);
    await expectLater(
      session.receiveRemote(),
      throwsA(code('clipboard_transport_closed')),
    );
    expect(session.ready, isFalse);
    expect(session.closed, isTrue);
    expect(bridge.events.where((event) => event == 'commit'), isEmpty);
  });

  test('Only a manual transfer or a waiting paste counts as somebody waiting; background sync does not', () async {
    await connect();
    expect(session.userWaiting, isFalse);
    await session.setSharingActive(true);
    final automatic = session.sendLocal(automatic: true);
    await socket.nextMessage('clipboard.begin');
    expect(
      session.userWaiting,
      isFalse,
      reason: 'a copy shared in the background',
    );
    await ack(await socket.nextMessage('clipboard.chunk'), 'clipboard.chunk.ack');
    await ack(await socket.nextMessage('clipboard.item.end'), 'clipboard.item.ack');
    await ack(await socket.nextMessage('clipboard.commit'), 'clipboard.complete');
    await automatic;
    final manual = session.receiveRemote();
    final id2 = await receiveBegin();
    expect(session.userWaiting, isTrue, reason: 'the user asked for this one');
    await receiveItem(id2, 0, [97, 98, 99]);
    socket.emit({'type': 'clipboard.commit', 'requestId': id2});
    await socket.nextMessage('clipboard.applied');
    socket.emit({'type': 'clipboard.complete', 'requestId': id2});
    await manual;
    expect(session.userWaiting, isFalse);
  });

  test('The progress names the file in progress and its leaf only', () async {
    await connect();
    final receiving = session.receiveRemote();
    final id = await receiveBegin(
      format: 'files',
      files: const [
        ClipboardItem(name: '문서.txt', size: 3),
        ClipboardItem(name: 'empty.bin', size: 0),
      ],
    );
    expect(session.progress?.fileName, '문서.txt');
    await receiveItem(id, 0, [97, 98, 99]);
    expect(session.progress?.fileName, 'empty.bin');
    expect(session.progress?.asyncConsumer, isFalse);
    expect(clipboardLeaf(r'photos\2026\IMG 1.jpg'), 'IMG 1.jpg');
    expect(clipboardLeaf('a/b/c.txt'), 'c.txt');
    expect(clipboardLeaf(''), isNull);
    expect(clipboardLeaf(null), isNull);
    await receiveItem(id, 1, []);
    socket.emit({'type': 'clipboard.commit', 'requestId': id});
    await socket.nextMessage('clipboard.applied');
    socket.emit({'type': 'clipboard.complete', 'requestId': id});
    await receiving;
  });

  group('claiming this PC\'s clipboard for a copy that is still arriving', () {
    late ClaimingClipboard claiming;
    Future<void> connectClaiming({
      Duration claimAfter = const Duration(milliseconds: 30),
    }) async {
      claiming = ClaimingClipboard();
      session = ClipboardSession(
        policy: const ClipboardPolicy.all(),
        url: Uri.parse(
          'wss://cloud.example.com/api/v1/sessions/$sessionId/clipboard',
        ),
        token: 'test-token',
        sessionId: sessionId,
        bridge: claiming,
        claimAfter: claimAfter,
        dialer: (_, _) async => socket,
      );
      await connect();
    }

    Future<void> finishReceive(String id, Future<void> receiving) async {
      socket.emit({'type': 'clipboard.commit', 'requestId': id});
      await socket.nextMessage('clipboard.applied');
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await receiving;
    }

    test('a files copy that takes longer than the claim delay empties the clipboard before it is published', () async {
      await connectClaiming();
      final receiving = session.receiveRemote();
      final id = await receiveBegin(
        format: 'files',
        files: const [ClipboardItem(name: 'a.bin', size: 3)],
      );
      expect(claiming.events, ['begin']);
      await Future<void>.delayed(const Duration(milliseconds: 80));
      expect(claiming.events, ['begin', 'claim:$id']);
      await receiveItem(id, 0, [97, 98, 99]);
      await finishReceive(id, receiving);
      // The claim came first; the content replaces it at the commit.
      expect(claiming.events, [
        'begin',
        'claim:$id',
        'append',
        'finish',
        'commit',
        'release',
      ]);
      expect(claiming.published, isTrue);
      expect(session.error, isNull);
    });

    test(
      'a copy that is done before the delay never touches the clipboard early',
      () async {
        await connectClaiming(claimAfter: const Duration(seconds: 5));
        final receiving = session.receiveRemote();
        final id = await receiveBegin(
          format: 'files',
          files: const [ClipboardItem(name: 'a.bin', size: 3)],
        );
        await receiveItem(id, 0, [97, 98, 99]);
        await finishReceive(id, receiving);
        await Future<void>.delayed(const Duration(milliseconds: 20));
        expect(
          claiming.events.where((event) => event.startsWith('claim')),
          isEmpty,
        );
      },
    );

    test('plain text is never claimed (it arrives in one piece)', () async {
      await connectClaiming(claimAfter: Duration.zero);
      final receiving = session.receiveRemote();
      final id = await receiveBegin();
      await Future<void>.delayed(const Duration(milliseconds: 40));
      expect(
        claiming.events.where((event) => event.startsWith('claim')),
        isEmpty,
      );
      await receiveItem(id, 0, [97, 98, 99]);
      await finishReceive(id, receiving);
    });

    test('a transfer cancelled before the delay claims nothing and keeps the old content', () async {
      await connectClaiming(claimAfter: const Duration(milliseconds: 200));
      unawaited(session.receiveRemote().catchError((Object _) {}));
      final id = await receiveBegin(
        format: 'files',
        files: const [ClipboardItem(name: 'a.bin', size: 3)],
      );
      final cancelling = session.cancelTransfer();
      await socket.nextMessage('clipboard.cancel');
      socket.emit({'type': 'clipboard.cancel', 'requestId': id});
      await cancelling;
      await Future<void>.delayed(const Duration(milliseconds: 300));
      expect(
        claiming.events.where((event) => event.startsWith('claim')),
        isEmpty,
      );
      expect(claiming.published, isFalse);
    });
    test('a claim the platform refuses never fails the transfer', () async {
      await connectClaiming();
      claiming.refuse = true;
      final receiving = session.receiveRemote();
      final id = await receiveBegin(
        format: 'files',
        files: const [ClipboardItem(name: 'a.bin', size: 3)],
      );
      await Future<void>.delayed(const Duration(milliseconds: 80));
      expect(claiming.events, contains('claim:$id'));
      await receiveItem(id, 0, [97, 98, 99]);
      await finishReceive(id, receiving);
      expect(claiming.published, isTrue);
      expect(session.error, isNull);
    });
  });
}

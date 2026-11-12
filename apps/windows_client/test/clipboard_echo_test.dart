import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';
import 'package:multitap_client/data/product_log.dart';

import 'clipboard_onpaste_test.dart' as onpaste;

// Automatic Ctrl+C/Ctrl+V sharing between a session and its peer, with a fake
// Windows clipboard that behaves like the real one where echo suppression was
// fragile: the sequence number read when the change notification is handled
// can be newer than the one sampled inside our own write (another listener
// adding a format, the shell, a late notification). No platform channel or
// real clipboard is used.

const sessionId = 'echo-session';
String digestOf(List<int> bytes) => sha256.convert(bytes).toString();

class PeerSocket extends Fake implements WebSocket {
  final incoming = StreamController<dynamic>();
  final sent = <Map<String, dynamic>>[];
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
    unawaited(incoming.close());
  }

  Iterable<Map<String, dynamic>> ofType(String type) =>
      sent.where((value) => value['type'] == type);

  // A window may leave retired packets queued after cancellation. A known
  // request must consume only its own replies, never ACK old data under a new ID.
  Future<Map<String, dynamic>> next(String type, {String? requestId}) async {
    for (var i = 0; i < 2000; i++) {
      final index = sent.indexWhere(
        (value) =>
            value['type'] == type &&
            (requestId == null || value['requestId'] == requestId),
      );
      if (index >= 0) return sent.removeAt(index);
      await Future<void>.delayed(const Duration(milliseconds: 1));
    }
    // Fixed protocol labels/reasons only: never dump clipboard payloads here.
    throw StateError(
      'No $type message; replies: ${sent.map((v) => '${v['type']}:${v['reason'] ?? ''}').join(',')}',
    );
  }
}

class Content {
  Content.text(String value)
    : format = 'text',
      names = const [null],
      bytes = [Uint8List.fromList(utf8.encode(value))];
  Content.files(Map<String, String> files)
    : format = 'files',
      names = files.keys.toList(),
      bytes = [
        for (final value in files.values)
          Uint8List.fromList(utf8.encode(value)),
      ];
  Content.received(this.format, this.names, this.bytes);
  final String format;
  final List<String?> names;
  final List<Uint8List> bytes;
}

class FakeWindowsClipboard
    implements
        ClipboardBridge,
        ClipboardObservationBridge,
        ClipboardPublishDiagnostics,
        ClipboardFormatsBridge,
        ClipboardPullBridge,
        ClipboardScaleBridge,
        ClipboardOnPasteBridge {
  FakeWindowsClipboard(this.content);
  // Virtual-stream plumbing is delegated separately: the older memory fixture
  // has a boolean named `published`, while this fixture tests that diagnostics method.
  final _virtual = onpaste.OnPasteClipboard();
  Map<String, List<Uint8List>> get files => _virtual.files;
  Uint8List? get current => _virtual.current;
  bool get onPasteOn => _virtual.onPasteOn;
  void platform(ClipboardPullEvent event) => _virtual.platform(event);
  @override
  bool get supportsFormats => true;
  @override
  set formats(bool value) => _virtual.formats = value;
  @override
  bool get supportsPull => true;
  @override
  set pull(bool value) => _virtual.pull = value;
  @override
  bool get supportsScale => true;
  @override
  set scale(bool value) => _virtual.scale = value;
  @override
  set richBytes(int value) => _virtual.richBytes = value;
  @override
  bool get supportsOnPaste => true;
  @override
  set onPaste(bool value) => _virtual.onPaste = value;
  @override
  Stream<ClipboardPullEvent> get pullEvents => _virtual.pullEvents;
  @override
  Future<void> publishOffer(
    String owner,
    String offerId,
    List<ClipboardItem> entries,
  ) => publishOfferOnPaste(owner, offerId, 'tree', entries);
  @override
  Future<void> pullFail(int range, String reason) =>
      _virtual.pullFail(range, reason);
  @override
  Future<void> pullShutdown(String owner, String reason) =>
      _virtual.pullShutdown(owner, reason);
  @override
  Future<int> pullOpen(String owner, String offerId, int index, int offset) =>
      _virtual.pullOpen(owner, offerId, index, offset);
  @override
  Future<void> pullClose(String owner) => _virtual.pullClose(owner);
  @override
  Future<void> closeOffers(String owner) => _virtual.closeOffers(owner);
  @override
  Future<void> pullAbort(String owner) => _virtual.pullAbort(owner);
  @override
  Future<void> pullSupersede(String owner) => _virtual.pullSupersede(owner);
  @override
  Future<void> pullRetire(String owner, String offerId) =>
      _virtual.pullRetire(owner, offerId);
  @override
  Future<void> withdrawOffer(String owner, String offerId) =>
      _virtual.withdrawOffer(owner, offerId);
  Content content;
  // What the native self-check reports for the next files publish.
  ({bool readable, int error, int flags}) nextCheck = (
    readable: true,
    error: 0,
    flags: 1,
  );
  @override
  ({bool readable, int error, int flags})? lastPublishCheck;
  @override
  Future<({bool owned, int sequence})> published(String owner) async =>
      (owned: ownedBy == owner, sequence: sequence);
  // The Dart scope whose received content our window published, while that
  // window still owns the clipboard (the native GetClipboardOwner() check).
  String? ownedBy;
  int sequence = 1, publishes = 0, skippedCommits = 0;
  bool drift = true, notifyBeforeResult = true;
  final events = <String>[];
  int ownCaptures = 0, fileBytesSent = 0;
  final _changes = StreamController<ClipboardChange>.broadcast(sync: true);
  String? _watcher;
  Content? _capture;
  String? _receiveFormat;
  List<String?> _receiveNames = const [];
  List<BytesBuilder> _received = const [];
  @override
  int? lastCommittedSequence;
  @override
  Stream<ClipboardChange> get changes => _changes.stream;
  @override
  Future<int> watch(String owner, bool enabled) async {
    _watcher = enabled ? owner : null;
    return sequence;
  }

  void _notify() {
    final watcher = _watcher;
    if (watcher != null) _changes.add(ClipboardChange(watcher, sequence));
  }

  /// A genuine copy in a local application replaces the content and owner.
  void userCopy(Content value) {
    content = value;
    ownedBy = null;
    sequence++;
    _notify();
  }

  @override
  Future<ClipboardCapture> capture(String owner, {bool skipOwn = false}) async {
    events.add('capture');
    // Like the native adapter: only an automatic capture asks, and then no
    // source is opened.
    if (skipOwn && ownedBy == owner) {
      ownCaptures++;
      return ClipboardCapture(
        format: '',
        items: const [],
        sequence: sequence,
        own: true,
      );
    }
    _capture = content;
    return ClipboardCapture(
      format: content.format,
      sequence: sequence,
      items: [
        for (var i = 0; i < content.bytes.length; i++)
          ClipboardItem(size: content.bytes[i].length, name: content.names[i]),
      ],
    );
  }

  @override
  Future<Uint8List> sourceChunk(
    String owner,
    int index,
    int offset,
    int length,
  ) async {
    if (_capture!.format == 'files') fileBytesSent += length;
    return Uint8List.sublistView(
      _capture!.bytes[index],
      offset,
      offset + length,
    );
  }

  @override
  Future<String> sourceDigest(String owner, int index) async =>
      digestOf(_capture!.bytes[index]);
  @override
  Future<void> begin(
    String owner,
    String requestId,
    String format,
    List<ClipboardItem> items,
  ) async {
    events.add('begin');
    _receiveFormat = format;
    _receiveNames = [for (final item in items) item.name];
    _received = [for (final _ in items) BytesBuilder()];
  }

  @override
  Future<void> append(
    String owner,
    String requestId,
    int index,
    int offset,
    Uint8List bytes,
  ) async => _received[index].add(bytes);
  @override
  Future<void> finish(
    String owner,
    String requestId,
    int index,
    String digest,
  ) async {
    if (digestOf(_received[index].toBytes()) != digest) {
      throw PlatformException(code: 'clipboard_transfer_failed');
    }
  }

  @override
  Future<void> commit(
    String owner,
    String requestId, {
    int? unlessSequence,
  }) async {
    if (unlessSequence != null && unlessSequence == sequence) {
      skippedCommits++;
      lastCommittedSequence = sequence;
      lastPublishCheck = null;
      events.add('commit-skipped');
      return;
    }
    events.add('commit');
    content = Content.received(_receiveFormat!, _receiveNames, [
      for (final item in _received) item.toBytes(),
    ]);
    _published(owner, check: _receiveFormat == 'files' ? nextCheck : null);
  }

  void _published(
    String owner, {
    ({bool readable, int error, int flags})? check,
  }) {
    ownedBy = owner;
    publishes++;
    sequence++;
    lastCommittedSequence = sequence;
    lastPublishCheck = check;
    if (!drift) {
      _notify();
      return;
    }
    // The notification is handled after something else touched the
    // clipboard: its sequence differs from the one our write returned.
    if (notifyBeforeResult) {
      sequence++;
      _notify();
    } else {
      Timer(Duration.zero, () {
        sequence++;
        _notify();
      });
    }
  }

  @override
  Future<ClipboardCapture> captureWithOffer(
    String owner,
    String offerId, {
    bool skipOwn = false,
  }) async {
    final captured = await capture(owner, skipOwn: skipOwn);
    if (captured.own || captured.format != 'files' || !onPasteOn) {
      return captured;
    }
    files[offerId] = _capture!.bytes;
    return ClipboardCapture(
      format: 'tree',
      sequence: captured.sequence,
      items: [
        for (final item in captured.items)
          ClipboardItem(
            name: item.name,
            size: item.size,
            kind: 'file',
            attributes: 32,
            mtime: 0,
          ),
      ],
      tops: captured.items.length,
      offer: true,
    );
  }

  String? receivedOffer;
  final _rangeIndexes = <int, int>{}, _rangeBuffers = <int, BytesBuilder>{};
  final _rangeDone = <int, Completer<void>>{};
  int _nextRange = 0;
  @override
  Future<void> publishOfferOnPaste(
    String owner,
    String offerId,
    String format,
    List<ClipboardItem> entries,
  ) async {
    expect(format, 'tree');
    events.add('publishOfferOnPaste');
    receivedOffer = offerId;
    final items = entries.where((item) => !item.isDirectory).toList();
    content = Content.received(
      'files',
      [for (final item in items) item.name],
      [for (final _ in items) Uint8List(0)],
    );
    _published(owner);
  }

  Future<void> requestFile(int index) {
    final range = ++_nextRange;
    _rangeIndexes[range] = index;
    _rangeBuffers[range] = BytesBuilder();
    final done = _rangeDone[range] = Completer<void>();
    platform(ClipboardPullRequest(range, receivedOffer!, index, 0));
    return done.future;
  }

  @override
  Future<void> pullData(int rangeId, int offset, Uint8List bytes) async {
    final buffer = _rangeBuffers[rangeId]!;
    expect(buffer.length, offset);
    buffer.add(bytes);
    platform(ClipboardPullAck(rangeId, _rangeIndexes[rangeId]!, buffer.length));
  }

  @override
  Future<void> pullEnd(int rangeId, String digest) async {
    final bytes = _rangeBuffers.remove(rangeId)!.takeBytes();
    expect(digestOf(bytes), digest);
    content.bytes[_rangeIndexes.remove(rangeId)!] = bytes;
    _rangeDone.remove(rangeId)!.complete();
  }

  @override
  Future<Uint8List> pullRead(String owner, int length) async {
    fileBytesSent += length;
    return _virtual.pullRead(owner, length);
  }

  @override
  Future<String> pullDigest(String owner) async => digestOf(current!);
  @override
  Future<List<String>> dropOffers(String owner) async {
    final retired = files.keys.toList();
    files.clear();
    return retired;
  }

  @override
  Future<void> cancel(String owner) async => events.add('cancel');
  @override
  Future<void> release(String owner) async => events.add('release');
}

class Fixture {
  // The default is a current pair. Legacy tests opt in explicitly and use a
  // manual receive where they exercise the old committed-file diagnostics.
  Fixture(Content local, {this.legacy = false})
    : bridge = FakeWindowsClipboard(local) {
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
      publishCheckDelay: const Duration(milliseconds: 150),
    );
  }
  final socket = PeerSocket();
  final FakeWindowsClipboard bridge;
  final bool legacy;
  late final ClipboardSession session;
  int _remoteSequence = 100;

  Future<void> connect() async {
    final starting = session.start();
    await Future<void>.delayed(const Duration(milliseconds: 5));
    socket.emit({
      'type': 'clipboard.ready',
      'sessionId': sessionId,
      'protocol': 'clipboard-v1',
      'clipboardSync': true,
      'clipboardPolicy': {
        'textToRemote': true,
        'textToLocal': true,
        'filesToRemote': true,
        'filesToLocal': true,
      },
      'limits': legacy
          ? {
              'chunkBytes': 32768,
              'maxFiles': 128,
              'maxFileBytes': 268435456,
              'maxTotalBytes': 536870912,
              'maxTextUnits': 32768,
            }
          : onpaste.onPasteLimits(),
    });
    await starting;
    await session.setSharingActive(true);
  }

  /// Plays the peer for one outgoing transfer; returns its manifest.
  Future<void> idle() async {
    for (var n = 0; n < 2000 && session.busy; n++) {
      await Future<void>.delayed(const Duration(milliseconds: 1));
    }
    expect(
      session.busy,
      isFalse,
      reason: 'the prior native cleanup must finish',
    );
  }

  Future<Map<String, dynamic>> acceptOutgoing() async {
    if (bridge.content.format == 'files' && !legacy) {
      final offer = await socket.next('clipboard.offer');
      final page = await socket.next(
        'clipboard.entries',
        requestId: offer['requestId'] as String,
      );
      expect(socket.ofType('clipboard.chunk'), isEmpty);
      expect(
        bridge.fileBytesSent,
        0,
        reason: 'copy announces files without reading/sending bytes',
      );
      socket.emit({
        'type': 'clipboard.complete',
        'requestId': offer['requestId'],
      });
      await idle();
      return {...offer, 'entries': page['entries']};
    }
    final begin = await socket.next('clipboard.begin');
    final id = begin['requestId'] as String;
    final count = begin['format'] == 'text'
        ? 1
        : (begin['files'] as List).length;
    for (var index = 0; index < count; index++) {
      final size = begin['format'] == 'text'
          ? begin['size'] as int
          : ((begin['files'] as List)[index] as Map)['size'] as int;
      for (var offset = 0; offset < size;) {
        final chunk = await socket.next('clipboard.chunk', requestId: id);
        offset += base64Decode(chunk['data'] as String).length;
        socket.emit({
          'type': 'clipboard.chunk.ack',
          'requestId': id,
          'index': index,
          'offset': offset,
        });
      }
      await socket.next('clipboard.item.end', requestId: id);
      socket.emit({
        'type': 'clipboard.item.ack',
        'requestId': id,
        'index': index,
      });
    }
    await socket.next('clipboard.commit', requestId: id);
    socket.emit({'type': 'clipboard.complete', 'requestId': id});
    await idle();
    return begin;
  }

  /// The peer announces a change and serves [content] when it is read.
  Future<void> remoteCopy(Content content) async {
    announce();
    await serveRead(content);
  }

  Future<void> manualRemoteCopy(Content content) async {
    await idle();
    final receiving = session.receiveRemote();
    await serveRead(content, eager: true);
    await receiving;
  }

  void announce() => socket.emit({
    'type': 'clipboard.changed',
    'sessionId': sessionId,
    'sequence': ++_remoteSequence,
  });

  Content? _offeredRemote;
  int remoteFileBytesSent = 0;
  Future<void> serveRead(Content content, {bool eager = false}) async {
    final read = await socket.next('clipboard.read');
    final id = read['requestId'] as String;
    if (content.format == 'files' && !eager) {
      expect(legacy, isFalse, reason: 'legacy automatic reads must not start');
      _offeredRemote = content;
      socket.emit({
        'type': 'clipboard.offer',
        'requestId': id,
        'format': 'tree',
        'entries': content.bytes.length,
        'files': content.bytes.length,
        'bytes': content.bytes.fold<int>(0, (sum, bytes) => sum + bytes.length),
        'tops': content.bytes.length,
        'skipped': 0,
      });
      socket.emit({
        'type': 'clipboard.entries',
        'requestId': id,
        'start': 0,
        'entries': [
          for (var i = 0; i < content.bytes.length; i++)
            {
              'path': content.names[i],
              'kind': 'file',
              'size': content.bytes[i].length,
              'attrs': 32,
              'mtime': 0,
            },
        ],
      });
      await socket.next('clipboard.applied', requestId: id);
      socket.emit({'type': 'clipboard.complete', 'requestId': id});
      expect(
        socket.ofType('clipboard.pull'),
        isEmpty,
        reason: 'copy does not ask for file bytes',
      );
      expect(bridge.content.bytes.every((bytes) => bytes.isEmpty), isTrue);
      return;
    }
    socket.emit({
      'type': 'clipboard.begin',
      'requestId': id,
      'format': content.format,
      if (content.format == 'text') 'size': content.bytes.single.length,
      if (content.format == 'files')
        'files': [
          for (var i = 0; i < content.bytes.length; i++)
            {'name': content.names[i], 'size': content.bytes[i].length},
        ],
    });
    for (var index = 0; index < content.bytes.length; index++) {
      final bytes = content.bytes[index];
      if (bytes.isNotEmpty) {
        socket.emit({
          'type': 'clipboard.chunk',
          'requestId': id,
          'index': index,
          'offset': 0,
          'data': base64Encode(bytes),
        });
        if (legacy) await socket.next('clipboard.chunk.ack', requestId: id);
      }
      socket.emit({
        'type': 'clipboard.item.end',
        'requestId': id,
        'index': index,
        'sha256': digestOf(bytes),
      });
      await socket.next('clipboard.item.ack', requestId: id);
    }
    socket.emit({'type': 'clipboard.commit', 'requestId': id});
    await socket.next('clipboard.applied', requestId: id);
    socket.emit({'type': 'clipboard.complete', 'requestId': id});
  }

  Future<void> pasteReceived() async {
    final source = _offeredRemote!;
    for (var index = 0; index < source.bytes.length; index++) {
      final pasted = bridge.requestFile(index);
      final request = await socket.next('clipboard.pull');
      expect(request['offerId'], bridge.receivedOffer);
      final bytes = source.bytes[index];
      if (bytes.isNotEmpty) {
        remoteFileBytesSent += bytes.length;
        socket.emit({
          'type': 'clipboard.chunk',
          'requestId': request['requestId'],
          'index': index,
          'offset': 0,
          'data': base64Encode(bytes),
        });
        await socket.next(
          'clipboard.chunk.ack',
          requestId: request['requestId'] as String,
        );
      }
      socket.emit({
        'type': 'clipboard.item.end',
        'requestId': request['requestId'],
        'index': index,
        'sha256': digestOf(bytes),
      });
      await pasted.timeout(const Duration(seconds: 2));
    }
  }

  int _hostPull = 0;
  Future<List<Uint8List>> pasteOutgoing(Map<String, dynamic> offer) async {
    final result = <Uint8List>[];
    final entries = offer['entries'] as List;
    for (var index = 0; index < entries.length; index++) {
      final id = 'pullh-${++_hostPull}';
      socket.emit({
        'type': 'clipboard.pull',
        'requestId': id,
        'offerId': offer['requestId'],
        'index': index,
        'offset': 0,
      });
      final bytes = BytesBuilder();
      while (bytes.length < (entries[index] as Map)['size']) {
        final chunk = await socket.next('clipboard.chunk', requestId: id);
        expect(chunk['requestId'], id);
        bytes.add(base64Decode(chunk['data'] as String));
        socket.emit({
          'type': 'clipboard.chunk.ack',
          'requestId': id,
          'index': index,
          'offset': bytes.length,
        });
      }
      final end = await socket.next('clipboard.item.end', requestId: id);
      final copied = bytes.takeBytes();
      expect(end['sha256'], digestOf(copied));
      result.add(copied);
    }
    return result;
  }

  Future<void> quiet() =>
      Future<void>.delayed(const Duration(milliseconds: 300));
}

void main() {
  late Fixture f;
  tearDown(() async => f.session.close());

  for (final (label, content) in [
    ('files', Content.files({'report.txt': 'quarterly', 'empty.bin': ''})),
    ('text', Content.text('VM 안의 텍스트')),
  ]) {
    for (final early in [true, false]) {
      test(
        '$label copied in the VM is published once and never sent back '
        '(change handled ${early ? 'before' : 'after'} the commit result)',
        () async {
          f = Fixture(Content.text('local'));
          f.bridge.notifyBeforeResult = early;
          await f.connect();
          await f.acceptOutgoing(); // activation shares the existing local copy
          await f.remoteCopy(content);
          await f.quiet();
          expect(f.bridge.publishes, 1);
          expect(f.bridge.content.format, content.format);
          expect(
            f.bridge.ownCaptures,
            greaterThanOrEqualTo(1),
            reason: 'native ownership, not a file-policy refusal, suppresses the drifted echo',
          );
          if (content.format == 'files') {
            expect(f.remoteFileBytesSent, 0);
            await f.pasteReceived();
            expect(f.bridge.content.bytes, content.bytes);
          }
          expect(
            f.socket.ofType('clipboard.begin'),
            isEmpty,
            reason: 'the received content must not be echoed to the VM',
          );
          // Ctrl+V in the session pastes what the VM already has.
          expect(
            await f.session.preparePaste().timeout(const Duration(seconds: 2)),
            isTrue,
          );
          await f.quiet();
          expect(f.socket.ofType('clipboard.begin'), isEmpty);
          expect(f.bridge.publishes, 1);
        },
      );
    }
  }

  test('without a sequence drift the applied sequence alone suppresses the '
      'echo (the case the earlier fixtures covered)', () async {
    f = Fixture(Content.text('local'));
    f.bridge.drift = false;
    await f.connect();
    await f.acceptOutgoing();
    await f.remoteCopy(Content.files({'a.txt': 'vm'}));
    await f.quiet();
    expect(f.bridge.publishes, 1);
    expect(f.socket.ofType('clipboard.begin'), isEmpty);
    expect(f.bridge.events.where((e) => e == 'capture').length, 1);
  });

  test(
    'an older Host cannot trigger automatic file transmission or a remote read',
    () async {
      final original = Content.files({'photo.jpg': 'jpeg bytes'});
      f = Fixture(original, legacy: true);
      await f.connect();
      f.announce();
      await f.quiet();
      expect(f.bridge.publishes, 0, reason: 'the user\'s own paths stay');
      expect(f.bridge.skippedCommits, 0);
      expect(identical(f.bridge.content, original), isTrue);
      expect(f.socket.ofType('clipboard.begin'), isEmpty);
      expect(f.socket.ofType('clipboard.offer'), isEmpty);
      expect(f.socket.ofType('clipboard.chunk'), isEmpty);
      expect(f.socket.ofType('clipboard.read'), isEmpty);
      expect(f.bridge.fileBytesSent, 0);
      expect(f.session.closed, isFalse);
    },
  );

  test('an older Host still permits silent local text, while unknown remote content is never automatically read', () async {
    f = Fixture(Content.text('local'), legacy: true);
    await f.connect();
    await f.acceptOutgoing();
    f.announce();
    await f.quiet();
    expect(f.socket.ofType('clipboard.read'), isEmpty);
    f.bridge.userCopy(Content.text('new local text'));
    expect((await f.acceptOutgoing())['format'], 'text');
    expect(f.session.closed, isFalse);
  });

  test('a legacy explicit manual file roundtrip keeps original local paths and stops its echo', () async {
    final original = Content.files({'photo.jpg': 'jpeg bytes'});
    f = Fixture(original, legacy: true);
    await f.connect();
    await f.quiet();
    final sending = f.session.sendLocal();
    await f.acceptOutgoing();
    await sending;
    await f.manualRemoteCopy(Content.files({'photo.jpg': 'jpeg bytes'}));
    await f.quiet();
    expect(f.bridge.publishes, 0);
    expect(f.bridge.skippedCommits, 1);
    expect(identical(f.bridge.content, original), isTrue);
    expect(f.socket.ofType('clipboard.begin'), isEmpty);
    expect(f.socket.ofType('clipboard.read'), isEmpty);
  });

  test('identical content is still published when the local clipboard '
      'changed since it was sent', () async {
    f = Fixture(Content.text('same'));
    await f.connect();
    await f.acceptOutgoing();
    f.bridge.sequence += 3; // changed while not observed
    await f.remoteCopy(Content.text('same'));
    expect(f.bridge.publishes, 1);
    expect(f.bridge.skippedCommits, 0);
  });

  test('legacy manual copies with different bytes under the same names are published', () async {
    f = Fixture(Content.files({'a.txt': 'one'}), legacy: true);
    await f.connect();
    await f.quiet();
    final sending = f.session.sendLocal();
    await f.acceptOutgoing();
    await sending;
    await f.manualRemoteCopy(Content.files({'a.txt': 'two'}));
    expect(f.bridge.publishes, 1);
    expect(utf8.decode(f.bridge.content.bytes.single), 'two');
  });

  test('latest file manifests with the same names fetch the new bytes only when pasted', () async {
    f = Fixture(Content.files({'a.txt': 'one'}));
    await f.connect();
    await f.acceptOutgoing();
    await f.remoteCopy(Content.files({'a.txt': 'two'}));
    expect(f.bridge.publishes, 1);
    expect(f.remoteFileBytesSent, 0);
    expect(f.bridge.fileBytesSent, 0);
    await f.pasteReceived();
    expect(utf8.decode(f.bridge.content.bytes.single), 'two');
  });

  test('a new local copy after a received one is still shared', () async {
    f = Fixture(Content.text('local'));
    await f.connect();
    await f.acceptOutgoing();
    await f.remoteCopy(Content.files({'from-vm.txt': 'vm'}));
    await f.quiet();
    f.bridge.userCopy(Content.files({'mine.txt': 'pc'}));
    final begin = await f.acceptOutgoing();
    expect((begin['entries'] as List).single['path'], 'mine.txt');
    expect(f.bridge.fileBytesSent, 0);
    expect(utf8.decode((await f.pasteOutgoing(begin)).single), 'pc');
  });

  test('an explicit send still sends content received from the VM', () async {
    f = Fixture(Content.text('local'));
    await f.connect();
    await f.acceptOutgoing();
    await f.remoteCopy(Content.text('from vm'));
    await f.quiet();
    final sending = f.session.sendLocal();
    final begin = await f.acceptOutgoing();
    await sending;
    expect(begin['format'], 'text');
  });

  late List<ProductLogEntry> records;
  late StreamSubscription<ProductLogEntry> recording;
  setUp(() {
    records = [];
    recording = ProductLog.instance.localRecords.listen(records.add);
  });
  tearDown(() => recording.cancel());
  List<Map<String, Object>> clipboardRecords(int action) => [
    for (final entry in records)
      if (entry.event == LogEvent.clipboard && entry.values['action'] == action)
        entry.values,
  ];

  test('latest file offer records its manifest without checking or transferring files before paste', () async {
    f = Fixture(Content.text('local'));
    await f.connect();
    await f.acceptOutgoing();
    await f.remoteCopy(
      Content.files({'private-fixture.txt': 'private fixture data'}),
    );
    await Future<void>.delayed(const Duration(milliseconds: 50));
    expect(
      [
        for (final values in clipboardRecords(2))
          (values['remote'], values['stage']),
      ],
      [(true, 1), (false, 5)],
    );
    expect(
      clipboardRecords(6),
      isEmpty,
      reason: 'a virtual offer has no committed paths to check yet',
    );
    expect(f.remoteFileBytesSent, 0);
    expect(f.bridge.fileBytesSent, 0);
    final encoded = records.map((entry) => jsonEncode(entry.toJson())).join();
    expect(encoded, isNot(contains('private-fixture')));
    expect(encoded, isNot(contains('private fixture data')));
    await f.pasteReceived();
    expect(f.remoteFileBytesSent, utf8.encode('private fixture data').length);
  });

  test('legacy explicitly received files are checked for the desktop user and '
      'again a few seconds later (fixed fields only)', () async {
    f = Fixture(Content.text('local'), legacy: true);
    f.bridge.nextCheck = (readable: false, error: 5, flags: 1 | 2);
    await f.connect();
    await f.acceptOutgoing();
    await f.manualRemoteCopy(Content.files({'report.txt': 'quarterly'}));
    await Future<void>.delayed(const Duration(milliseconds: 50));
    final checks = clipboardRecords(6);
    expect(checks, [
      {
        'action': 6,
        'ok': false,
        'error': 5,
        'stage': 3,
        'remote': false,
        'auto': false,
      },
    ]);
    await Future<void>.delayed(const Duration(milliseconds: 400));
    final later = clipboardRecords(7);
    expect(later.single['ok'], isTrue, reason: 'our copy still owns it');
    expect(
      later.single['changed'],
      isTrue,
      reason: 'the fake clipboard moved on after the publish (drift)',
    );
    // Content never reaches the log.
    final text = records.map((entry) => jsonEncode(entry.toJson())).join();
    expect(text.contains('report'), isFalse);
    expect(text.contains('quarterly'), isFalse);
  });

  test('a burst of VM announcements is read once, after the VM clipboard '
      'is quiet, and the file list is what this PC ends with', () async {
    f = Fixture(Content.text('local'));
    await f.connect();
    await f.acceptOutgoing();
    for (var i = 0; i < 3; i++) {
      f.announce();
      await Future<void>.delayed(const Duration(milliseconds: 120));
      expect(
        f.socket.ofType('clipboard.read'),
        isEmpty,
        reason: 'read while the VM clipboard was still changing',
      );
    }
    await f.serveRead(Content.files({'a.txt': 'from the vm'}));
    await Future<void>.delayed(const Duration(milliseconds: 450));
    expect(f.socket.ofType('clipboard.read'), isEmpty, reason: 'read once');
    expect(f.bridge.publishes, 1);
    expect(f.bridge.content.format, 'files');
    expect(f.remoteFileBytesSent, 0);
    await f.pasteReceived();
    expect(utf8.decode(f.bridge.content.bytes.single), 'from the vm');
  });

  test(
    'a later empty or failed VM read leaves the published file list',
    () async {
      f = Fixture(Content.text('local'));
      await f.connect();
      await f.acceptOutgoing();
      await f.remoteCopy(Content.files({'a.txt': 'from the vm'}));
      await f.pasteReceived();
      f.announce();
      final read = await f.socket.next('clipboard.read');
      f.socket.emit({
        'type': 'clipboard.error',
        'requestId': read['requestId'],
        'reason': 'clipboard_empty',
      });
      await f.quiet();
      expect(f.bridge.publishes, 1);
      expect(f.bridge.content.format, 'files');
      expect(utf8.decode(f.bridge.content.bytes.single), 'from the vm');
      expect(f.session.closed, isFalse);
    },
  );

  test('legacy manual transfer records carry the format and the channel record the '
      'effective permissions', () async {
    f = Fixture(Content.text('local'), legacy: true);
    await f.connect();
    await f.acceptOutgoing();
    await f.manualRemoteCopy(Content.files({'a.txt': 'from the vm'}));
    await f.manualRemoteCopy(Content.text('vm text'));
    await Future<void>.delayed(const Duration(milliseconds: 50));
    expect(clipboardRecords(1).single['stage'], 15);
    expect(
      [
        for (final values in clipboardRecords(2))
          (values['remote'], values['stage']),
      ],
      [(true, 1), (false, 2), (false, 1)],
    );
  });

  test('legacy manual text and kept own copies record no file check', () async {
    f = Fixture(Content.files({'photo.jpg': 'jpeg bytes'}), legacy: true);
    await f.connect();
    await f.quiet();
    final sending = f.session.sendLocal();
    await f.acceptOutgoing();
    await sending;
    await f.manualRemoteCopy(
      Content.files({'photo.jpg': 'jpeg bytes'}),
    ); // kept
    await f.manualRemoteCopy(Content.text('from vm'));
    await Future<void>.delayed(const Duration(milliseconds: 400));
    expect(clipboardRecords(6), isEmpty);
    expect(clipboardRecords(7), isEmpty);
  });
}

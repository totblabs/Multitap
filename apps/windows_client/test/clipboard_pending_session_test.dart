import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_onpaste_test.dart' as onpaste;
import 'clipboard_session_test.dart' as base;

// Implements the Windows identity without constructing NativeClipboardBridge.
// There is no MethodChannel, clipboard, input, socket or filesystem operation.
class _PendingNative extends Fake implements NativeClipboardBridge {
  bool traceClosing = false;
  final closingEvents = <String>[];
  void Function(String)? closingPrinter;

  void _noteClosing(String stage) {
    if (!traceClosing) return;
    closingEvents.add(stage);
    closingPrinter?.call('PENDING_CLOSE_NATIVE $stage');
  }

  @override
  dynamic noSuchMethod(Invocation invocation) {
    _noteClosing('noSuchMethod');
    throw StateError('unexpected platform call: ${invocation.memberName}');
  }

  @override
  bool allowedText = true, allowedFiles = true;
  @override
  bool get announceOnPaste => true;
  @override
  bool get android => false;
  @override
  void restrictFormats({required bool text, required bool files}) {}
  @override
  bool get supportsPending => true;
  @override
  bool get supportsFormats => true;
  @override
  bool get supportsPull => true;
  @override
  bool get supportsScale => true;
  @override
  bool get supportsOnPaste => true;
  @override
  bool get supportsPullQueue => false;
  @override
  set pullDepth(int value) {}
  @override
  set formats(bool value) {}
  @override
  set pull(bool value) {}
  @override
  set scale(bool value) {}
  @override
  set richBytes(int value) {}
  @override
  set onPaste(bool value) {}
  final events = <String>[], retired = <String>[];
  final failedRanges = <(int, String)>[];
  final received = <BytesBuilder>[];
  final publishedContent = <(String, String, List<ClipboardItem>)>[];
  final pullsController = StreamController<ClipboardPullEvent>.broadcast(
    sync: true,
  );
  @override
  Stream<ClipboardPullEvent> get pullEvents => pullsController.stream;
  void platform(ClipboardPullEvent event) => pullsController.add(event);
  @override
  Future<void> pullSupersede(String owner) async {}
  @override
  Future<void> pullRetire(String owner, String offerId) async {
    retired.add(offerId);
  }

  @override
  Future<void> pullAbort(String owner) async {}
  @override
  Future<void> pullShutdown(String owner, String reason) async {
    _noteClosing('pullShutdown.begin');
    _noteClosing('pullShutdown.bodyEnd');
  }

  @override
  Future<void> pullClose(String owner) async {
    _noteClosing('pullClose.begin');
    _noteClosing('pullClose.bodyEnd');
  }

  @override
  Future<void> closeOffers(String owner) async {}
  @override
  Future<void> pullFail(int rangeId, String reason) async {
    failedRanges.add((rangeId, reason));
  }

  final withdrawn = <String>[];
  @override
  Future<void> withdrawOffer(String owner, String offerId) async {
    withdrawn.add(offerId);
  }

  @override
  Future<void> cancel(String owner) async {}
  @override
  Future<void> release(String owner) async {
    _noteClosing('release.begin');
    _noteClosing('release.bodyEnd');
  }

  @override
  Future<int?> claim(String owner, String requestId) async {
    events.add('claim');
    return null;
  }

  final changesController = StreamController<ClipboardChange>.broadcast(
    sync: true,
  );
  @override
  Stream<ClipboardChange> get changes => changesController.stream;
  @override
  int? lastCommittedSequence;
  int sequence = 10, nextToken = 0;
  String? owner, request;
  bool owned = false, automaticPublication = false;
  final closedEpochs = <int>{}, cancelledTokens = <String>[];
  final bindings = <String>[], manual = <String>[];
  final publications = <(String, String)>{};
  @override
  bool publicationMatches(
    String owner,
    String requestId, {
    ClipboardPendingLease? lease,
  }) =>
      publications.contains((owner, requestId)) &&
      (lease == null || identical(lease, this.lease));
  ClipboardPendingLease? lease;
  Completer<Uint8List>? consumer;
  bool consumerCompleted = false, requestDuringPublish = false;
  Completer<void>? offerReply;
  Object? offerFailure;
  Object? beginFailure;
  Completer<bool>? bindReply;
  @override
  Future<int> watch(String owner, bool enabled) async {
    if (!enabled) _noteClosing('watch.false.begin');
    this.owner = owner;
    if (!enabled) _noteClosing('watch.false.bodyEnd');
    return sequence;
  }

  // The writer window owns the clipboard while [owned] (placeholder or commit).
  @override
  Future<({int sequence, bool own})> watchOwned(String owner) async {
    this.owner = owner;
    return (sequence: sequence, own: owned);
  }

  final captures = <bool>[];
  @override
  Future<ClipboardCapture> capture(String owner, {bool skipOwn = false}) {
    captures.add(skipOwn);
    throw PlatformException(code: 'clipboard_empty');
  }

  @override
  Future<ClipboardCapture> captureWithOffer(
    String owner,
    String offerId, {
    bool skipOwn = false,
  }) => capture(owner, skipOwn: skipOwn);

  @override
  Future<int> observePendingSequence(String owner, {required int epoch}) async {
    this.owner = owner;
    return sequence;
  }

  @override
  Future<ClipboardPendingLease?> beginPending(
    String owner, {
    required int expectedLocalSequence,
    required int sourceSequence,
    required String kind,
    required int epoch,
  }) async {
    if (closedEpochs.contains(epoch) || sequence != expectedLocalSequence) {
      return null;
    }
    owned = true;
    sequence++;
    automaticPublication = true;
    lastCommittedSequence = sequence;
    lease = ClipboardPendingLease(
      token: 'pending-${++nextToken}',
      sequence: sequence,
      sourceSequence: sourceSequence,
      kind: kind,
      epoch: epoch,
    );
    return lease;
  }

  @override
  Future<bool> bindPending(
    String owner,
    ClipboardPendingLease lease,
    String requestId,
  ) async {
    if (!owned ||
        !identical(this.lease, lease) ||
        closedEpochs.contains(lease.epoch)) {
      return false;
    }
    bindings.add(requestId);
    request = requestId;
    if (bindReply != null) return bindReply!.future;
    return true;
  }

  @override
  Future<void> cancelPending(String owner, ClipboardPendingLease lease) async {
    cancelledTokens.add(lease.token);
    if (identical(this.lease, lease) && !consumerCompleted) failConsumer();
  }

  @override
  Future<void> cancelPendingOwner(String owner, int epoch) async {
    _noteClosing('cancelPendingOwner.begin');
    closedEpochs.add(epoch);
    if (lease?.epoch == epoch && !consumerCompleted) failConsumer();
    _noteClosing('cancelPendingOwner.bodyEnd');
  }

  @override
  Future<void> allowManualRequest(String owner, String requestId) async {
    manual.add(requestId);
  }

  void copyLocal() {
    sequence++;
    owned = false;
  }

  Future<Uint8List> getData() {
    consumer = Completer<Uint8List>();
    return consumer!.future;
  }

  void failConsumer() {
    if (consumer case final c?) {
      if (!c.isCompleted) c.completeError(StateError('pending cancelled'));
    }
  }

  @override
  Future<void> begin(
    String owner,
    String requestId,
    String format,
    List<ClipboardItem> items,
  ) async {
    if (beginFailure case final failure?) throw failure;
    if (automaticPublication) expectSync(bindings, contains(requestId));
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
    expectSync(received[index].length, offset);
    received[index].add(bytes);
  }

  @override
  Future<void> finish(
    String owner,
    String requestId,
    int index,
    String hash,
  ) async {
    expectSync(sha256.convert(received[index].toBytes()).toString(), hash);
  }

  @override
  Future<void> commit(
    String owner,
    String requestId, {
    int? unlessSequence,
  }) async {
    expectSync(request, requestId);
    expectSync(owned, isTrue);
    lastCommittedSequence = sequence;
    consumerCompleted = true;
    publications.add((owner, requestId));
    if (consumer case final c?) {
      if (!c.isCompleted) c.complete(received.single.toBytes());
    }
  }

  @override
  Future<({bool owned, int sequence})> published(String owner) async =>
      (owned: owned, sequence: sequence);
  @override
  Future<void> publishOfferOnPaste(
    String owner,
    String offerId,
    String format,
    List<ClipboardItem> entries,
  ) async {
    // Files and folders have no placeholder to bind.
    if (format != 'tree') expectSync(bindings, contains(offerId));
    // An already waiting GetData asks for its first range before publication
    // replies. No request is emitted here when the consumer has not pasted.
    if (requestDuringPublish) platform(ClipboardPullRequest(1, offerId, 0, 0));
    await offerReply?.future;
    if (offerFailure case final failure?) throw failure;
    publishedContent.add((offerId, format, entries));
    publications.add((owner, offerId));
  }
}

const _policy = ClipboardPolicy(textToLocal: true, filesToLocal: true);
const _bothWays = ClipboardPolicy(
  textToRemote: true,
  textToLocal: true,
  filesToRemote: true,
  filesToLocal: true,
);
Map<String, dynamic> _ready({bool intent = true, bool toRemote = false}) => {
  ...onpaste.onPasteReady(
    policy: {
      'textToRemote': toRemote,
      'textToLocal': true,
      'filesToRemote': toRemote,
      'filesToLocal': true,
    },
  ),
  'clipboardSync': true,
  'clipboardSettled': clipboardSettledProtocol,
  if (intent) 'clipboardIntent': clipboardIntentProtocol,
};

void main() {
  List<({Future<void> Function() close, String Function() describe})>?
  activeClosers;

  Future<void> drainClose(
    WidgetTester tester,
    Future<void> Function() close,
    String Function() describe,
  ) async {
    var done = false;
    Object? failure;
    StackTrace? failureStack;
    final closing = close().then<void>(
      (_) {
        done = true;
      },
      onError: (Object error, StackTrace stack) {
        failure = error;
        failureStack = stack;
        done = true;
      },
    );
    // The product has 2s sync, 2s pull and 1s socket close bounds. Pump only
    // while this owned cleanup is pending; do not suppress binding invariants.
    for (var tick = 0; !done && tick <= 60; tick++) {
      await tester.runAsync(() async {
        await Future<void>.delayed(Duration.zero);
      });
      if (done) break;
      await tester.pump(
        tick == 0 ? Duration.zero : const Duration(milliseconds: 100),
      );
    }
    if (!done) {
      throw StateError(
        'pending fixture cleanup did not drain in six seconds; ${describe()}',
      );
    }
    await closing;
    if (failure case final error?) {
      Error.throwWithStackTrace(error, failureStack!);
    }
  }

  void pendingTestWidgets(
    String description,
    Future<void> Function(WidgetTester) body, {
    required TargetPlatformVariant variant,
  }) {
    testWidgets(description, (tester) async {
      final previous = activeClosers;
      final closers =
          <({Future<void> Function() close, String Function() describe})>[];
      activeClosers = closers;
      try {
        await body(tester);
      } finally {
        activeClosers = previous;
        for (final entry in closers.reversed) {
          await drainClose(tester, entry.close, entry.describe);
        }
      }
    }, variant: variant);
  }

  Future<void> until(WidgetTester tester, bool Function() predicate) async {
    for (var i = 0; i < 100 && !predicate(); i++) {
      await tester.pump(const Duration(milliseconds: 1));
      // Cancellation may complete in the real async zone (StreamSubscription).
      // Let that existing completion run while preserving the bounded fake time.
      await tester.runAsync(() async {});
    }
    expect(predicate(), isTrue, reason: 'bounded in-memory state transition');
  }

  List<Map<String, dynamic>> sent(base.ClipboardSocket socket, String type) =>
      socket.sent.where((v) => v['type'] == type).toList();
  Future<
    ({
      ClipboardSession session,
      base.ClipboardSocket socket,
      _PendingNative bridge,
      int epoch,
    })
  >
  connect(
    WidgetTester tester, {
    bool intent = true,
    bool toRemote = false,
    Duration idleTimeout = const Duration(seconds: 30),
  }) async {
    final socket = base.ClipboardSocket(), bridge = _PendingNative();
    final session = ClipboardSession(
      policy: toRemote ? _bothWays : _policy,
      url: Uri.parse(
        'wss://synthetic.invalid/api/v1/sessions/${base.sessionId}/clipboard',
      ),
      token: 'fixture-only',
      sessionId: base.sessionId,
      bridge: bridge,
      syncSupported: true,
      idleTimeout: idleTimeout,
      dialer: (_, _) async => socket,
      pullLinger: Duration.zero,
    );
    Future<void>? cleanup;
    Future<void> closeActual() async {
      bridge.traceClosing = true;
      bridge.closingPrinter = tester.printToConsole;
      tester.printToConsole('PENDING_CLOSE session.begin');
      await session.close();
      tester.printToConsole('PENDING_CLOSE session.end');
      tester.printToConsole('PENDING_CLOSE changes.begin');
      await bridge.changesController.close();
      tester.printToConsole('PENDING_CLOSE changes.end');
      tester.printToConsole('PENDING_CLOSE pulls.begin');
      await bridge.pullsController.close();
      tester.printToConsole('PENDING_CLOSE pulls.end');
    }

    Future<void> closeFixture() => cleanup ??= closeActual();
    addTearDown(() async {
      if (cleanup == null) await closeFixture();
    });
    final closers = activeClosers;
    if (closers == null) {
      throw StateError('pending fixture requires a scoped test body');
    }
    closers.add((
      close: closeFixture,
      describe: () =>
          'closed=${session.closed} busy=${session.busy} status=${session.status} '
          'events=${bridge.events} closingEvents=${bridge.closingEvents}',
    ));
    Object? startFailure;
    StackTrace? startStack;
    final started = session.start().catchError((
      Object error,
      StackTrace stack,
    ) {
      startFailure = error;
      startStack = stack;
    });
    await tester.pump(Duration.zero);
    if (!session.closed) {
      socket.emit(_ready(intent: intent, toRemote: toRemote));
    }
    await tester.pump(Duration.zero);
    await started;
    if (startFailure case final failure?) {
      Error.throwWithStackTrace(failure, startStack!);
    }
    await session.setSharingActive(true);
    await until(tester, () => sent(socket, 'clipboard.watch').isNotEmpty);
    return (
      session: session,
      socket: socket,
      bridge: bridge,
      epoch: intent
          ? sent(socket, 'clipboard.watch').last['watchEpoch'] as int
          : 0,
    );
  }

  Future<void> observe(
    WidgetTester tester,
    base.ClipboardSocket socket,
    int epoch, {
    int sequence = 41,
    String kind = 'text',
    bool settled = true,
  }) async {
    socket.emit({
      'type': 'clipboard.intent',
      'sessionId': base.sessionId,
      'sequence': sequence,
      'kind': kind,
      'watchEpoch': epoch,
    });
    await tester.pump(Duration.zero);
    socket.emit({
      'type': 'clipboard.changed',
      'sessionId': base.sessionId,
      'sequence': sequence,
      if (settled) 'settled': clipboardSettledProtocol,
      'watchEpoch': epoch,
    });
    await tester.pump(Duration.zero);
  }

  pendingTestWidgets(
    'pending actual Session holds old text until validated small payload and exact binding arrive',
    (tester) async {
      final f = await connect(tester);
      expect(
        f.session.dialHeaders[clipboardIntentHeader],
        clipboardIntentHeaderValue,
      );
      f.socket.emit({
        'type': 'clipboard.intent',
        'sessionId': base.sessionId,
        'sequence': 41,
        'kind': 'text',
        'watchEpoch': f.epoch,
      });
      await until(tester, () => f.bridge.lease != null);
      final pasted = f.bridge.getData();
      var completed = false;
      unawaited(
        pasted.then((_) {
          completed = true;
        }),
      );
      expect(sent(f.socket, 'clipboard.read'), isEmpty);
      expect(completed, isFalse);
      f.socket.emit({
        'type': 'clipboard.changed',
        'sessionId': base.sessionId,
        'sequence': 41,
        'settled': clipboardSettledProtocol,
        'watchEpoch': f.epoch,
      });
      await until(tester, () => sent(f.socket, 'clipboard.read').isNotEmpty);
      final read = sent(f.socket, 'clipboard.read').last,
          id = read['requestId'] as String;
      expect(read, {
        'type': 'clipboard.read',
        'requestId': id,
        'expectedSequence': 41,
        'textImageOnly': true,
        'watchEpoch': f.epoch,
      });
      final bytes = Uint8List.fromList(utf8.encode('new VM text'));
      f.socket.emit({
        'type': 'clipboard.begin',
        'requestId': id,
        'format': 'text',
        'size': bytes.length,
      });
      await until(tester, () => f.bridge.events.contains('begin'));
      expect(completed, isFalse);
      f.socket.emit({
        'type': 'clipboard.chunk',
        'requestId': id,
        'index': 0,
        'offset': 0,
        'data': base64Encode(bytes),
      });
      f.socket.emit({
        'type': 'clipboard.item.end',
        'requestId': id,
        'index': 0,
        'sha256': sha256.convert(bytes).toString(),
      });
      f.socket.emit({'type': 'clipboard.commit', 'requestId': id});
      await until(tester, () => sent(f.socket, 'clipboard.applied').isNotEmpty);
      expect(await pasted, orderedEquals(bytes));
      expect(f.bridge.bindings, [id]);
      expect(f.bridge.events, isNot(contains('claim')));
      f.socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await tester.pump(Duration.zero);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  // No wait of a second or two: a paste right after a VM file
  // copy must take the new files. The Host already waits 250 ms of quiet before
  // announcing any copy, so the file copy is read at once; and the copy's
  // intent alone retires the previous VM copy on this PC, before the Host
  // withdraws it, so a paste in between never takes the old files.
  Future<String> fileCopy(
    WidgetTester tester,
    ({
      ClipboardSession session,
      base.ClipboardSocket socket,
      _PendingNative bridge,
      int epoch,
    })
    f,
    int sequence, {
    Duration readWithin = const Duration(milliseconds: 400),
  }) async {
    f.socket.sent.clear();
    f.socket.emit({
      'type': 'clipboard.intent',
      'sessionId': base.sessionId,
      'sequence': sequence,
      'kind': 'other',
      'watchEpoch': f.epoch,
    });
    await tester.pump(Duration.zero);
    f.socket.emit({
      'type': 'clipboard.changed',
      'sessionId': base.sessionId,
      'sequence': sequence,
      'watchEpoch': f.epoch,
    });
    for (
      var waited = Duration.zero;
      sent(f.socket, 'clipboard.read').isEmpty && waited < readWithin;
      waited += const Duration(milliseconds: 1)
    ) {
      await tester.pump(const Duration(milliseconds: 1));
      await tester.runAsync(() async {});
    }
    expect(
      sent(f.socket, 'clipboard.read'),
      hasLength(1),
      reason: 'the copy is read within $readWithin of its announcement',
    );
    final read = sent(f.socket, 'clipboard.read').single,
        id = read['requestId'] as String;
    expect(read['expectedSequence'], sequence);
    expect(read['textImageOnly'], isFalse);
    f.socket.emit({
      'type': 'clipboard.offer',
      'requestId': id,
      'format': 'tree',
      'entries': 1,
      'files': 1,
      'bytes': 1000,
      'tops': 1,
      'skipped': 0,
    });
    await tester.pump(Duration.zero);
    f.socket.emit({
      'type': 'clipboard.entries',
      'requestId': id,
      'start': 0,
      'entries': [
        {
          'path': 'a.bin',
          'kind': 'file',
          'size': 1000,
          'attrs': 32,
          'mtime': 1761500000123,
        },
      ],
    });
    await until(tester, () => sent(f.socket, 'clipboard.applied').isNotEmpty);
    f.socket.emit({'type': 'clipboard.complete', 'requestId': id});
    await until(tester, () => !f.session.busy);
    return id;
  }

  pendingTestWidgets(
    'a VM file copy is read at once: the Host already waited for quiet',
    (tester) async {
      final f = await connect(tester);
      // Within 2 ms of fake time: the older 300 ms quiet here would miss it.
      final id = await fileCopy(
        tester,
        f,
        41,
        readWithin: const Duration(milliseconds: 2),
      );
      expect(f.bridge.publishedContent.map((p) => p.$1), [id]);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  pendingTestWidgets(
    'the next VM copy\'s intent retires the previous file copy before the Host withdraws it',
    (tester) async {
      final f = await connect(tester);
      final first = await fileCopy(tester, f, 41);
      expect(f.bridge.publishedContent.map((p) => p.$1), [first]);
      expect(f.bridge.retired, isEmpty);
      expect(f.bridge.withdrawn, isEmpty);
      // The next VM copy is announced: its intent arrives first.
      f.socket.sent.clear();
      f.socket.emit({
        'type': 'clipboard.intent',
        'sessionId': base.sessionId,
        'sequence': 42,
        'kind': 'other',
        'watchEpoch': f.epoch,
      });
      await until(tester, () => f.bridge.withdrawn.contains(first));
      expect(f.bridge.retired, [first]);
      // A paste of the previous copy now ends at once; nothing is pulled.
      f.bridge.platform(ClipboardPullRequest(5, first, 0, 0));
      await tester.pump(Duration.zero);
      expect(sent(f.socket, 'clipboard.pull'), isEmpty);
      expect(f.bridge.failedRanges, [(5, 'clipboard_offer_expired')]);
      // The Host's own withdrawal later changes nothing more.
      f.socket.emit({'type': 'clipboard.offer.withdraw', 'requestId': first});
      await tester.pump(Duration.zero);
      expect(f.session.closed, isFalse);
      // A text intent keeps its placeholder path: no file copy is retired.
      final second = await fileCopy(tester, f, 43);
      f.bridge.retired.clear();
      f.socket.emit({
        'type': 'clipboard.intent',
        'sessionId': base.sessionId,
        'sequence': 44,
        'kind': 'text',
        'watchEpoch': f.epoch,
      });
      await until(tester, () => f.bridge.lease != null);
      expect(f.bridge.retired, isEmpty);
      expect(second, isNot(first));
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  // A VM copy that fails at its binding (clipboard_changed) leaves our own
  // dead placeholder for the next automatic PC capture to read back
  // (clipboard_read_failed). Returning to the window within the grace must
  // neither take our own placeholder for a PC copy nor re-arm the watch.
  Future<void> deliverText(
    WidgetTester tester,
    base.ClipboardSocket socket,
    _PendingNative bridge,
    int epoch,
  ) async {
    socket.emit({
      'type': 'clipboard.changed',
      'sessionId': base.sessionId,
      'sequence': 41,
      'settled': clipboardSettledProtocol,
      'watchEpoch': epoch,
    });
    await until(tester, () => sent(socket, 'clipboard.read').isNotEmpty);
    final id = sent(socket, 'clipboard.read').last['requestId'] as String;
    expect(sent(socket, 'clipboard.read').last['watchEpoch'], epoch);
    final bytes = Uint8List.fromList(utf8.encode('VM text'));
    socket.emit({
      'type': 'clipboard.begin',
      'requestId': id,
      'format': 'text',
      'size': bytes.length,
    });
    socket.emit({
      'type': 'clipboard.chunk',
      'requestId': id,
      'index': 0,
      'offset': 0,
      'data': base64Encode(bytes),
    });
    socket.emit({
      'type': 'clipboard.item.end',
      'requestId': id,
      'index': 0,
      'sha256': sha256.convert(bytes).toString(),
    });
    socket.emit({'type': 'clipboard.commit', 'requestId': id});
    await until(tester, () => sent(socket, 'clipboard.applied').isNotEmpty);
    expect(bridge.bindings, [id]);
    socket.emit({'type': 'clipboard.complete', 'requestId': id});
    await tester.pump(Duration.zero);
  }

  for (final rendered in [false, true]) {
    pendingTestWidgets(
      'a focus blip while a VM copy arrives keeps it '
      '(${rendered ? 'placeholder already rendered' : 'placeholder waiting'})',
      (tester) async {
        final f = await connect(tester, toRemote: true);
        // Activation offers this PC's current clipboard once (empty here).
        await until(tester, () => f.bridge.captures.isNotEmpty);
        await tester.pump(const Duration(milliseconds: 10));
        f.bridge.captures.clear();
        f.socket.emit({
          'type': 'clipboard.intent',
          'sessionId': base.sessionId,
          'sequence': 41,
          'kind': 'text',
          'watchEpoch': f.epoch,
        });
        await until(tester, () => f.bridge.lease != null);
        // A paste or clipboard history rendered a format: the sequence moved
        // on while our window still owns the clipboard.
        if (rendered) f.bridge.sequence++;
        final watches = sent(f.socket, 'clipboard.watch').length;
        await f.session.setSharingActive(false);
        await tester.pump(const Duration(milliseconds: 300));
        await f.session.setSharingActive(true);
        await tester.pump(const Duration(milliseconds: 100));
        expect(f.bridge.closedEpochs, isEmpty);
        expect(f.bridge.cancelledTokens, isEmpty);
        expect(sent(f.socket, 'clipboard.watch').length, watches);
        await deliverText(tester, f.socket, f.bridge, f.epoch);
        expect(f.bridge.captures, isEmpty);
        expect(f.session.error, isNull);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.windows),
    );
  }
  pendingTestWidgets(
    'a paste in the VM right after copying there leaves the VM copy arriving',
    (tester) async {
      final f = await connect(tester, toRemote: true);
      await until(tester, () => f.bridge.captures.isNotEmpty);
      await tester.pump(const Duration(milliseconds: 10));
      f.bridge.captures.clear();
      f.socket.emit({
        'type': 'clipboard.intent',
        'sessionId': base.sessionId,
        'sequence': 41,
        'kind': 'text',
        'watchEpoch': f.epoch,
      });
      await until(tester, () => f.bridge.lease != null);
      f.bridge.sequence++; // clipboard history rendered the placeholder
      var ready = false;
      unawaited(f.session.preparePaste().then((value) => ready = value));
      await until(tester, () => ready);
      expect(f.bridge.captures, isEmpty);
      expect(f.bridge.cancelledTokens, isEmpty);
      await deliverText(tester, f.socket, f.bridge, f.epoch);
      expect(f.bridge.captures, isEmpty);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  pendingTestWidgets(
    'a PC copy made while away still re-arms the watch on return',
    (tester) async {
      final f = await connect(tester, toRemote: true);
      await until(tester, () => f.bridge.captures.isNotEmpty);
      await tester.pump(const Duration(milliseconds: 10));
      f.bridge.captures.clear();
      await f.session.setSharingActive(false);
      await tester.pump(const Duration(milliseconds: 300));
      f.bridge.copyLocal();
      await f.session.setSharingActive(true);
      await until(tester, () => f.bridge.closedEpochs.contains(f.epoch));
      final epochs = [
        for (final watch in sent(f.socket, 'clipboard.watch'))
          if (watch['enabled'] == true) watch['watchEpoch'],
      ];
      expect(epochs.last, greaterThan(f.epoch));
      await until(tester, () => f.bridge.captures.isNotEmpty);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  pendingTestWidgets(
    'late remote hint cannot steal a newer PC copy and never falls into ordinary publication',
    (tester) async {
      final f = await connect(tester);
      f.bridge.copyLocal();
      await observe(tester, f.socket, f.epoch);
      await tester.pump(const Duration(milliseconds: 301));
      expect(f.bridge.owned, isFalse);
      expect(f.bridge.lease, isNull);
      expect(sent(f.socket, 'clipboard.read'), isEmpty);
      expect(f.bridge.events, isNot(contains('begin')));
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  pendingTestWidgets(
    'large content announces only metadata until paste and admits a first pull before native reply',
    (tester) async {
      final f = await connect(tester);
      await observe(tester, f.socket, f.epoch);
      await until(tester, () => sent(f.socket, 'clipboard.read').isNotEmpty);
      final id = sent(f.socket, 'clipboard.read').last['requestId'] as String;
      f.bridge.offerReply = Completer<void>();
      f.socket.emit({
        'type': 'clipboard.offer',
        'requestId': id,
        'format': 'text',
        'size': 2 * 1024 * 1024,
      });
      await until(tester, () => f.bridge.bindings.contains(id));
      expect(sent(f.socket, 'clipboard.pull'), isEmpty);
      expect(sent(f.socket, 'clipboard.applied'), isEmpty);
      // This is the consumer's first paste, while the native publish RPC waits.
      f.bridge.platform(ClipboardPullRequest(1, id, 0, 0));
      await until(tester, () => sent(f.socket, 'clipboard.pull').isNotEmpty);
      expect(f.bridge.failedRanges, isEmpty);
      expect(sent(f.socket, 'clipboard.chunk'), isEmpty);
      f.bridge.offerReply!.complete();
      await until(tester, () => sent(f.socket, 'clipboard.applied').isNotEmpty);
      f.socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await tester.pump(Duration.zero);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  pendingTestWidgets(
    'pending offer failure retires its exact id while a newer local copy remains intact',
    (tester) async {
      final f = await connect(tester);
      await observe(tester, f.socket, f.epoch);
      await until(tester, () => sent(f.socket, 'clipboard.read').isNotEmpty);
      final id = sent(f.socket, 'clipboard.read').last['requestId'] as String;
      f.bridge.offerReply = Completer<void>();
      f.bridge.offerFailure = StateError('synthetic publish failure');
      f.socket.emit({
        'type': 'clipboard.offer',
        'requestId': id,
        'format': 'text',
        'size': 2 * 1024 * 1024,
      });
      await until(tester, () => f.bridge.bindings.contains(id));
      f.bridge.copyLocal();
      final localSequence = f.bridge.sequence;
      f.bridge.offerReply!.complete();
      await until(tester, () => f.bridge.retired.contains(id));
      expect(f.bridge.retired, [id]);
      expect(f.bridge.owned, isFalse);
      expect(f.bridge.sequence, localSequence);
      expect(sent(f.socket, 'clipboard.applied'), isEmpty);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  pendingTestWidgets(
    'already waiting GetData can issue its first range synchronously inside native publication',
    (tester) async {
      final f = await connect(tester);
      await observe(tester, f.socket, f.epoch);
      await until(tester, () => sent(f.socket, 'clipboard.read').isNotEmpty);
      final id = sent(f.socket, 'clipboard.read').last['requestId'] as String;
      f.bridge.requestDuringPublish = true;
      f.bridge.offerReply = Completer<void>();
      f.socket.emit({
        'type': 'clipboard.offer',
        'requestId': id,
        'format': 'text',
        'size': 2 * 1024 * 1024,
      });
      await until(tester, () => sent(f.socket, 'clipboard.pull').isNotEmpty);
      expect(f.bridge.failedRanges, isEmpty);
      expect(sent(f.socket, 'clipboard.applied'), isEmpty);
      expect(sent(f.socket, 'clipboard.pull').single['offerId'], id);
      f.bridge.offerReply!.complete();
      await until(tester, () => sent(f.socket, 'clipboard.applied').isNotEmpty);
      f.socket.emit({'type': 'clipboard.complete', 'requestId': id});
      await tester.pump(Duration.zero);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  pendingTestWidgets(
    'older negotiated pair retains its exact watch shape and does not publish a placeholder',
    (tester) async {
      final f = await connect(tester, intent: false);
      expect(sent(f.socket, 'clipboard.watch').single, {
        'type': 'clipboard.watch',
        'sessionId': base.sessionId,
        'enabled': true,
      });
      expect(f.bridge.lease, isNull);
      expect(f.bridge.closedEpochs, isEmpty);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  pendingTestWidgets(
    'local copy between intent and manifest refuses only this automatic transfer',
    (tester) async {
      final f = await connect(tester);
      await observe(tester, f.socket, f.epoch);
      await until(tester, () => sent(f.socket, 'clipboard.read').isNotEmpty);
      final id = sent(f.socket, 'clipboard.read').last['requestId'] as String;
      f.bridge.copyLocal();
      final localSequence = f.bridge.sequence;
      f.socket.emit({
        'type': 'clipboard.begin',
        'requestId': id,
        'format': 'text',
        'size': 3,
      });
      await until(tester, () => sent(f.socket, 'clipboard.error').isNotEmpty);
      expect(sent(f.socket, 'clipboard.error').last, {
        'type': 'clipboard.error',
        'requestId': id,
        'reason': 'clipboard_changed',
      });
      expect(f.session.ready, isTrue);
      expect(f.session.closed, isFalse);
      expect(f.socket.wasClosed, isFalse);
      expect(f.session.failureQuiet, isTrue);
      expect(f.session.error, 'clipboard_changed');
      expect(f.bridge.events, isNot(contains('begin')));
      expect(f.bridge.owned, isFalse);
      expect(f.bridge.sequence, localSequence);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  pendingTestWidgets(
    'invalid protocol exception still closes the full negotiated pair',
    (tester) async {
      final f = await connect(tester);
      await observe(tester, f.socket, f.epoch);
      await until(tester, () => sent(f.socket, 'clipboard.read').isNotEmpty);
      final id = sent(f.socket, 'clipboard.read').last['requestId'] as String;
      f.bridge.beginFailure = const ClipboardTransferException(
        'clipboard_invalid_message',
      );
      f.socket.emit({
        'type': 'clipboard.begin',
        'requestId': id,
        'format': 'text',
        'size': 3,
      });
      await until(tester, () => f.socket.wasClosed);
      expect(f.session.closed, isTrue);
      expect(f.session.ready, isFalse);
      expect(f.session.error, 'clipboard_invalid_message');
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  pendingTestWidgets(
    'late A bind refusal after timeout cannot cancel a newly reserved explicit B',
    (tester) async {
      final f = await connect(
        tester,
        idleTimeout: const Duration(milliseconds: -1),
      );
      await observe(tester, f.socket, f.epoch);
      await until(tester, () => sent(f.socket, 'clipboard.read').isNotEmpty);
      final a = sent(f.socket, 'clipboard.read').last['requestId'] as String;
      f.bridge.bindReply = Completer<bool>();
      f.socket.emit({
        'type': 'clipboard.begin',
        'requestId': a,
        'format': 'text',
        'size': 3,
      });
      await until(tester, () => f.bridge.bindings.contains(a));
      await tester.pump(const Duration(seconds: 1));
      await until(tester, () => !f.session.busy);
      final b = f.session.receiveRemote();
      unawaited(b.catchError((Object _) {}));
      await until(tester, () => sent(f.socket, 'clipboard.read').length == 2);
      final bId = sent(f.socket, 'clipboard.read').last['requestId'] as String;
      expect(bId, isNot(a));
      f.bridge.bindReply!.complete(false);
      await tester.pump(Duration.zero);
      expect(f.session.ready, isTrue);
      expect(f.session.closed, isFalse);
      expect(f.session.busy, isTrue);
      expect(
        sent(f.socket, 'clipboard.error').where((v) => v['requestId'] == bId),
        isEmpty,
      );
      expect(f.bridge.manual, [bId]);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
  pendingTestWidgets(
    'late native PlatformException from A bind cannot cancel explicit B',
    (tester) async {
      final f = await connect(
        tester,
        idleTimeout: const Duration(milliseconds: -1),
      );
      await observe(tester, f.socket, f.epoch);
      await until(tester, () => sent(f.socket, 'clipboard.read').isNotEmpty);
      final a = sent(f.socket, 'clipboard.read').last['requestId'] as String;
      f.bridge.bindReply = Completer<bool>();
      f.socket.emit({
        'type': 'clipboard.begin',
        'requestId': a,
        'format': 'text',
        'size': 3,
      });
      await until(tester, () => f.bridge.bindings.contains(a));
      await tester.pump(const Duration(seconds: 1));
      await until(tester, () => !f.session.busy);
      final b = f.session.receiveRemote();
      unawaited(b.catchError((Object _) {}));
      await until(tester, () => sent(f.socket, 'clipboard.read').length == 2);
      final bId = sent(f.socket, 'clipboard.read').last['requestId'] as String;
      f.bridge.bindReply!.completeError(
        PlatformException(code: 'clipboard_changed'),
      );
      await tester.pump(Duration.zero);
      expect(f.session.ready, isTrue);
      expect(f.session.closed, isFalse);
      expect(f.session.busy, isTrue);
      expect(
        sent(f.socket, 'clipboard.error').where((v) => v['requestId'] == bId),
        isEmpty,
      );
      expect(f.bridge.manual, [bId]);
    },
    variant: TargetPlatformVariant.only(TargetPlatform.windows),
  );
}

import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_onpaste_test.dart' as onpaste;
import 'clipboard_pull_test.dart' as pull;
import 'clipboard_session_test.dart' as base;

// An actual observation-capable on-paste session, with every native accessor
// substituted. Sequence metadata, not content or hashes, crosses recovery.
class RecoveryClipboard extends onpaste.OnPasteClipboard
    implements ClipboardObservationBridge {
  int sequence = 10;
  String? _watcher;
  final _changes = StreamController<ClipboardChange>.broadcast(sync: true);
  @override
  Stream<ClipboardChange> get changes => _changes.stream;
  @override
  int? lastCommittedSequence;
  @override
  Future<int> watch(String owner, bool enabled) async {
    _watcher = enabled ? owner : null;
    return sequence;
  }

  void userCopy() {
    sequence++;
    final owner = _watcher;
    if (owner != null) _changes.add(ClipboardChange(owner, sequence));
  }

  @override
  Future<ClipboardCapture> captureWithOffer(
    String owner,
    String offerId, {
    bool skipOwn = false,
  }) async {
    final content = await super.captureWithOffer(
      owner,
      offerId,
      skipOwn: skipOwn,
    );
    return ClipboardCapture(
      format: content.format,
      items: content.items,
      tops: content.tops,
      skipped: content.skipped,
      offer: content.offer,
      own: content.own,
      sequence: sequence,
    );
  }

  @override
  Future<void> publishOfferOnPaste(
    String owner,
    String offerId,
    String format,
    List<ClipboardItem> entries,
  ) async {
    await super.publishOfferOnPaste(owner, offerId, format, entries);
    lastCommittedSequence = ++sequence;
  }

  Future<void> closeChanges() => _changes.close();
}

void main() {
  late RecoveryClipboard bridge;
  final sessions = <ClipboardSession>[];
  ClipboardSession make(
    base.ClipboardSocket socket, {
    ClipboardPolicy policy = const ClipboardPolicy.all(),
    ClipboardSession? previous,
  }) {
    final session = ClipboardSession(
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/${base.sessionId}/clipboard',
      ),
      token: 'fixture-only',
      sessionId: base.sessionId,
      policy: policy,
      bridge: bridge,
      syncSupported: true,
      dialer: (_, _) async => socket,
    );
    if (previous != null) session.inheritRecoveryFrom(previous);
    sessions.add(session);
    return session;
  }

  Future<void> connect(
    ClipboardSession session,
    base.ClipboardSocket socket,
  ) async {
    final starting = session.start();
    await base.settle();
    socket.emit({...onpaste.onPasteReady(), 'clipboardSync': true});
    await starting;
  }

  setUp(() {
    sessions.clear();
    bridge = RecoveryClipboard()
      ..offerCapture = ClipboardCapture(
        format: 'tree',
        items: [pull.tree('fixture.bin', 0)],
        tops: 1,
        offer: true,
      );
  });
  tearDown(() async {
    for (final session in sessions.reversed) {
      await session.close();
    }
    await bridge.closeChanges();
  });

  test('an uncertain file publication is not automatically reoffered by a fresh owner, but a new copy works', () async {
    final oldSocket = base.ClipboardSocket(), old = make(oldSocket);
    await connect(old, oldSocket);
    final uncertain = expectLater(
      old.sendLocal(),
      throwsA(base.code('clipboard_commit_uncertain')),
    );
    final first = await oldSocket.nextMessage('clipboard.offer');
    await oldSocket.nextMessage('clipboard.entries');
    oldSocket.incoming.addError(StateError('closed'));
    await uncertain;
    await old.close();
    expect(old.canReconnect, isTrue);

    final socket = base.ClipboardSocket(), fresh = make(socket, previous: old);
    await connect(fresh, socket);
    await fresh.setSharingActive(true);
    await Future<void>.delayed(const Duration(milliseconds: 110));
    expect(
      socket.sent.where(
        (v) => [
          'clipboard.offer',
          'clipboard.begin',
          'clipboard.chunk',
        ].contains(v['type']),
      ),
      isEmpty,
    );
    expect(await fresh.preparePaste(), isFalse);
    // No initial remote announcement exists on an empty Host: do not blindly
    // discard the first real remote copy after it starts watching.
    socket.emit({
      'type': 'clipboard.changed',
      'sessionId': base.sessionId,
      'sequence': 1,
    });
    final read = await socket.nextMessage('clipboard.read');
    socket.emit({
      'type': 'clipboard.error',
      'requestId': read['requestId'],
      'reason': 'clipboard_empty',
    });
    await base.settle();
    bridge.userCopy();
    final next = await socket.nextMessage('clipboard.offer');
    await socket.nextMessage('clipboard.entries');
    expect(next['requestId'], isNot(first['requestId']));
    socket.emit({'type': 'clipboard.complete', 'requestId': next['requestId']});
    await base.settle();
    expect(fresh.closed, isFalse);
    expect(await fresh.preparePaste(), isTrue);
  });

  test('recovery retains the already read remote sequence and accepts the next genuine one', () async {
    const policy = ClipboardPolicy(textToLocal: true);
    final oldSocket = base.ClipboardSocket(),
        old = make(oldSocket, policy: policy);
    await connect(old, oldSocket);
    await old.setSharingActive(true);
    oldSocket.emit({
      'type': 'clipboard.changed',
      'sessionId': base.sessionId,
      'sequence': 70,
    });
    final read = await oldSocket.nextMessage('clipboard.read');
    oldSocket.emit({
      'type': 'clipboard.offer',
      'requestId': read['requestId'],
      'format': 'text',
      'size': 1,
    });
    await oldSocket.nextMessage('clipboard.applied');
    oldSocket.incoming.addError(StateError('closed'));
    await base.settle();
    await old.close();
    expect(old.error, 'clipboard_commit_uncertain');
    expect(old.canReconnect, isTrue);

    final socket = base.ClipboardSocket(),
        fresh = make(socket, policy: policy, previous: old);
    await connect(fresh, socket);
    await fresh.setSharingActive(true);
    socket.emit({
      'type': 'clipboard.changed',
      'sessionId': base.sessionId,
      'sequence': 70,
    });
    await Future<void>.delayed(const Duration(milliseconds: 330));
    expect(socket.sent.where((v) => v['type'] == 'clipboard.read'), isEmpty);
    socket.emit({
      'type': 'clipboard.changed',
      'sessionId': base.sessionId,
      'sequence': 71,
    });
    final next = await socket.nextMessage('clipboard.read');
    socket.emit({
      'type': 'clipboard.error',
      'requestId': next['requestId'],
      'reason': 'clipboard_empty',
    });
    await base.settle();
    expect(fresh.closed, isFalse);
  });
}

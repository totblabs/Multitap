import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_pending.dart';
import 'package:multitap_client/data/native_clipboard.dart';

// All metadata/ownership is in memory; no MethodChannel or OS is available.
class _Bridge implements ClipboardPendingBridge {
  @override
  bool get supportsPending => true;
  int sequence = 10, counter = 0;
  bool owned = false;
  final closed = <int>{},
      cancelled = <String>[],
      begins = <int>[],
      bindings = <String>[];
  final records = <String, ClipboardPendingLease>{};
  final manual = <String>[];
  Completer<int>? observation;
  Completer<ClipboardPendingLease?>? delayed;
  ClipboardPendingLease? delayedLease;
  @override
  Future<int> observePendingSequence(
    String owner, {
    required int epoch,
  }) async => observation == null ? sequence : await observation!.future;
  @override
  Future<ClipboardPendingLease?> beginPending(
    String owner, {
    required int expectedLocalSequence,
    required int sourceSequence,
    required String kind,
    required int epoch,
  }) async {
    begins.add(expectedLocalSequence);
    final ownChain =
        owned &&
        records.values.any(
          (r) => r.epoch == epoch && r.sequence == expectedLocalSequence,
        );
    if (closed.contains(epoch) ||
        (sequence != expectedLocalSequence && !ownChain)) {
      return null;
    }
    owned = true;
    sequence++;
    final lease = ClipboardPendingLease(
      token: 'token-${++counter}',
      sequence: sequence,
      sourceSequence: sourceSequence,
      kind: kind,
      epoch: epoch,
    );
    records[lease.token] = lease;
    if (delayed != null) {
      delayedLease = lease;
      return delayed!.future;
    }
    return lease;
  }

  @override
  Future<bool> bindPending(
    String owner,
    ClipboardPendingLease lease,
    String requestId,
  ) async {
    if (!owned ||
        closed.contains(lease.epoch) ||
        cancelled.contains(lease.token)) {
      return false;
    }
    bindings.add(requestId);
    return true;
  }

  @override
  Future<void> cancelPending(String owner, ClipboardPendingLease lease) async {
    cancelled.add(lease.token);
  }

  @override
  Future<void> cancelPendingOwner(String owner, int epoch) async {
    closed.add(epoch);
  }

  @override
  Future<void> allowManualRequest(String owner, String requestId) async {
    manual.add(requestId);
  }

  void localCopy() {
    sequence++;
    owned = false;
  }
}

Future<void> turn() => Future<void>.delayed(Duration.zero);
void main() {
  test('anchor predates watch; a newer PC copy refuses remote intent without resampling it', () async {
    final b = _Bridge();
    final c = ClipboardPending('scope', b);
    await c.arm();
    b.localCopy();
    await c.source(41, 'text', c.epoch);
    expect(b.begins, [10]);
    expect(b.sequence, 11);
    expect(b.owned, isFalse);
    expect(c.refused(41), isTrue);
    expect(await c.bind(41, 'request', 'text'), isFalse);
    await c.close();
  });
  test('normal text, richtext and image bind exact source and request without copying data', () async {
    for (final kind in ['text', 'richtext', 'image']) {
      final b = _Bridge();
      final p = ClipboardPending('owner', b);
      await p.arm();
      await p.source(0, kind, p.epoch);
      expect(await p.bind(0, 'request-$kind', kind), isTrue);
      expect(await p.bind(0, 'wrong', 'tree'), isFalse);
      expect(b.bindings, ['request-$kind']);
      await p.close();
    }
  });
  test('A late own publication is chained to B; a response cannot turn into arbitrary local baseline', () async {
    final b = _Bridge();
    final c = ClipboardPending('scope', b);
    await c.arm();
    b.delayed = Completer<ClipboardPendingLease?>();
    final a = c.source(41, 'text', c.epoch);
    await turn();
    final first = b.delayedLease!;
    final pending = b.delayed!;
    b.delayed = null;
    final next = c.source(42, 'text', c.epoch);
    pending.complete(first);
    await a;
    await next;
    expect(b.begins, [10, 11]);
    expect(b.cancelled, contains(first.token));
    final binding = await c.bindAdmission(c.admissionFor(42)!, 'new', 'text');
    expect(binding, isNotNull);
    expect(b.sequence, 12);
    await c.failed(41);
    expect(c.acceptsBinding(binding!), isTrue);
    expect(await c.bind(42, 'still-new', 'text'), isFalse);
    expect(b.bindings, ['new']);
    await c.close();
  });
  test(
    'C made while A reply is delayed wins over both A and queued B',
    () async {
      final b = _Bridge();
      final p = ClipboardPending('owner', b);
      await p.arm();
      b.delayed = Completer<ClipboardPendingLease?>();
      final a = p.source(41, 'text', p.epoch);
      await turn();
      final lease = b.delayedLease!, completion = b.delayed!;
      b.delayed = null;
      b.localCopy();
      final next = p.source(42, 'text', p.epoch);
      completion.complete(lease);
      await a;
      await next;
      expect(b.sequence, 12);
      expect(b.owned, isFalse);
      expect(p.refused(42), isTrue);
      expect(await p.bind(42, 'new', 'text'), isFalse);
      await p.close();
    },
  );
  test(
    'epoch close invalidates a delayed arm and ignores old watch callbacks',
    () async {
      final b = _Bridge();
      final c = ClipboardPending('scope', b);
      b.observation = Completer<int>();
      final arm = c.arm();
      await turn();
      await c.end();
      b.observation!.complete(10);
      await arm;
      b.observation = null;
      expect(c.active, isFalse);
      expect(b.closed, contains(1));
      await c.arm();
      expect(c.epoch, 2);
      await c.source(41, 'text', 1);
      expect(b.begins, isEmpty);
      await c.close();
    },
  );
  test('owner close fences a begin already in flight and retires its late exact lease', () async {
    final b = _Bridge();
    final c = ClipboardPending('scope', b);
    await c.arm();
    b.delayed = Completer<ClipboardPendingLease?>();
    final source = c.source(41, 'text', c.epoch);
    await turn();
    final lease = b.delayedLease!;
    var closed = false;
    final closing = c.close().then((_) => closed = true);
    await turn();
    expect(c.active, isFalse);
    expect(closed, isFalse);
    b.delayed!.complete(lease);
    await source;
    await closing;
    expect(closed, isTrue);
    expect(c.active, isFalse);
    expect(b.closed, contains(1));
    expect(b.cancelled, contains(lease.token));
    expect(await c.bind(41, 'late', 'text'), isFalse);
  });
  test('binding waits for the same source publication rather than creating a second placeholder', () async {
    final b = _Bridge();
    final c = ClipboardPending('scope', b);
    await c.arm();
    b.delayed = Completer<ClipboardPendingLease?>();
    final source = c.source(41, 'text', c.epoch);
    await turn();
    final binding = c.bind(41, 'request', 'text');
    await turn();
    expect(b.begins, [10]);
    b.delayed!.complete(b.delayedLease);
    await source;
    expect(await binding, isTrue);
    expect(b.begins, [10]);
    await c.close();
  });
  test('unknown metadata waits for actual manifest and still uses the prior anchor CAS', () async {
    final b = _Bridge();
    final c = ClipboardPending('scope', b);
    await c.arm();
    await c.source(41, 'other', c.epoch);
    expect(b.begins, isEmpty);
    b.localCopy();
    expect(await c.bind(41, 'actual-text', 'text'), isFalse);
    expect(b.begins, [10]);
    expect(b.sequence, 11);
    expect(b.owned, isFalse);
    await c.close();
  });
  test('own render sequence increments are accepted only through the exact owned chain', () async {
    final b = _Bridge();
    final c = ClipboardPending('scope', b);
    await c.arm();
    await c.source(41, 'text', c.epoch);
    b.sequence += 2;
    await c.source(42, 'text', c.epoch);
    expect(b.begins, [10, 11]);
    expect(await c.bind(42, 'next', 'text'), isTrue);
    b.localCopy();
    await c.localCopy();
    await c.source(43, 'text', c.epoch);
    expect(c.refused(43), isTrue);
    expect(b.owned, isFalse);
    await c.close();
  });
}

// Metadata-only pending admission contract; no platform or native calls.
import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_pending.dart';
import 'package:multitap_client/data/native_clipboard.dart';

const _owner = 'pending-admission-fixture';

// Scripted metadata replies only; no native ownership/cleanup policy is copied.
class _PendingReplies implements ClipboardPendingBridge {
  int sequence = 10, tokens = 0;
  final bindReplies = <bool>[];
  final cancelled = <ClipboardPendingLease>[];
  final cancelledOwners = <int>[];
  Completer<bool>? bindGate;
  Completer<void>? cancelOwnerGate;

  @override
  bool get supportsPending => true;

  @override
  Future<int> observePendingSequence(
    String owner, {
    required int epoch,
  }) async => sequence;

  @override
  Future<ClipboardPendingLease?> beginPending(
    String owner, {
    required int expectedLocalSequence,
    required int sourceSequence,
    required String kind,
    required int epoch,
  }) async => ClipboardPendingLease(
    token: 'pending-fixture-${++tokens}',
    sequence: ++sequence,
    sourceSequence: sourceSequence,
    kind: kind,
    epoch: epoch,
  );

  @override
  Future<bool> bindPending(
    String owner,
    ClipboardPendingLease lease,
    String requestId,
  ) async => bindGate != null
      ? await bindGate!.future
      : bindReplies.isEmpty
      ? true
      : bindReplies.removeAt(0);

  @override
  Future<void> cancelPending(String owner, ClipboardPendingLease lease) async {
    cancelled.add(lease);
  }

  @override
  Future<void> cancelPendingOwner(String owner, int epoch) async {
    cancelledOwners.add(epoch);
    await cancelOwnerGate?.future;
  }

  @override
  Future<void> allowManualRequest(String owner, String requestId) async {}
}

void main() {
  test('watch-off after close joins the same native owner barrier', () async {
    final bridge = _PendingReplies()..cancelOwnerGate = Completer<void>();
    final pending = ClipboardPending(_owner, bridge);
    await pending.arm();
    var closed = false, ended = false;
    final closing = pending.close().then((_) {
      closed = true;
    });
    final ending = pending.end().then((_) {
      ended = true;
    });
    await Future<void>.delayed(Duration.zero);
    expect(bridge.cancelledOwners, [1]);
    expect(closed, false);
    expect(ended, false);
    bridge.cancelOwnerGate!.complete();
    await Future.wait([closing, ending]);
    expect(closed, true);
    expect(ended, true);
    await pending.end();
    expect(bridge.cancelledOwners, [
      1,
    ], reason: 'closed metadata cannot create another native cleanup');
  });

  test(
    'a late old bind refusal cannot cancel the newer source binding',
    () async {
      final bridge = _PendingReplies();
      final pending = ClipboardPending(_owner, bridge);
      try {
        await pending.arm();
        await pending.source(41, 'text', pending.epoch);
        final oldAdmission = pending.admissionFor(41)!;
        final gate = bridge.bindGate = Completer<bool>();
        final old = pending.bindAdmission(oldAdmission, 'old-request', 'text');
        await Future<void>.delayed(Duration.zero);
        bridge.bindGate = null;
        await pending.source(42, 'text', pending.epoch);
        final current = (await pending.bindAdmission(
          pending.admissionFor(42)!,
          'new-request',
          'text',
        ))!;
        final before = bridge.cancelled.length;
        gate.complete(false);
        expect(await old, isNull);
        await pending.failedAdmission(oldAdmission);
        expect(pending.acceptsBinding(current), true);
        expect(
          bridge.cancelled
              .skip(before)
              .where((lease) => lease.token == current.lease.token),
          isEmpty,
        );
      } finally {
        await pending.close();
      }
    },
  );

  test(
    'old epoch failure cannot refuse the same sequence in a new epoch',
    () async {
      final bridge = _PendingReplies();
      final pending = ClipboardPending(_owner, bridge);
      try {
        await pending.arm();
        final oldEpoch = pending.epoch;
        await pending.source(41, 'text', oldEpoch);
        final oldAdmission = pending.admissionFor(41)!;
        final oldBinding = (await pending.bindAdmission(
          oldAdmission,
          'request-old',
          'text',
        ))!;
        await pending.end();
        await pending.arm();
        final newEpoch = pending.epoch;
        await pending.source(41, 'text', newEpoch);
        final currentAdmission = pending.admissionFor(41)!;
        final current = (await pending.bindAdmission(
          currentAdmission,
          'request-current',
          'text',
        ))!;
        final before = bridge.cancelled.length;

        await pending.failedAdmission(oldAdmission);
        await pending.failedBinding(oldBinding);

        expect(newEpoch, greaterThan(oldEpoch));
        expect(pending.acceptsAdmission(oldAdmission), isFalse);
        expect(pending.acceptsBinding(oldBinding), isFalse);
        expect(pending.acceptsAdmission(currentAdmission), isTrue);
        expect(pending.acceptsBinding(current), isTrue);
        expect(pending.refused(41), isFalse);
        expect(
          bridge.cancelled
              .skip(before)
              .any((lease) => lease.token == current.lease.token),
          isFalse,
        );
      } finally {
        await pending.close().timeout(const Duration(seconds: 2));
      }
    },
  );

  test('rejected same-lease rebind preserves the first binding', () async {
    final bridge = _PendingReplies()..bindReplies.addAll([true, false]);
    final pending = ClipboardPending(_owner, bridge);
    try {
      await pending.arm();
      await pending.source(41, 'text', pending.epoch);
      final admission = pending.admissionFor(41)!;
      final first = (await pending.bindAdmission(
        admission,
        'request-first',
        'text',
      ))!;
      final before = bridge.cancelled.length;
      final rejected = await pending.bindAdmission(
        admission,
        'request-second',
        'text',
      );

      expect(rejected, isNull);
      expect(pending.acceptsBinding(first), isTrue);
      expect(pending.refused(41), isFalse);
      expect(bridge.cancelled.length, before);
    } finally {
      await pending.close().timeout(const Duration(seconds: 2));
    }
  });
}

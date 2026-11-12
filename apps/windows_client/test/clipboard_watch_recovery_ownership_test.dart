// SOURCE-ONLY addon for the separately owned recovery candidate. The original
// public22 and RED fixtures are unchanged. No clipboard, HWND or native channel.
import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_pending.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_session_test.dart'
    show ClipboardSocket, MemoryClipboard, ready, digest, sessionId;

// Substitute only platform metadata. The actual Pending controller owns epoch,
// admission, source queue, binding and cancellation decisions. No mock recovery
// controller or private product state assignment is used.
class _MetadataBridge implements ClipboardPendingBridge {
  @override
  bool get supportsPending => true;
  int nextSequence = 20, observationsToFail = 0, nextToken = 0;
  final observed = <({String owner, int epoch})>[];
  final cancelledOwners = <({String owner, int epoch})>[];
  final cancelledLeases = <({String owner, ClipboardPendingLease lease})>[];
  final begun = <({String owner, int epoch, int source, int anchor})>[];
  final bound = <({String owner, String token, String request})>[];
  final manual = <({String owner, String request})>[];
  Completer<int>? heldObservation;
  Completer<ClipboardPendingLease?>? heldBegin;
  Object? cancelOwnerFailure;

  @override
  Future<int> observePendingSequence(String owner, {required int epoch}) async {
    observed.add((owner: owner, epoch: epoch));
    if (observationsToFail > 0) {
      observationsToFail--;
      throw PlatformException(code: 'clipboard_busy');
    }
    return heldObservation == null ? nextSequence : await heldObservation!.future;
  }

  @override
  Future<void> cancelPendingOwner(String owner, int epoch) async {
    cancelledOwners.add((owner: owner, epoch: epoch));
    if (cancelOwnerFailure != null) throw cancelOwnerFailure!;
  }

  @override
  Future<ClipboardPendingLease?> beginPending(
    String owner, {
    required int expectedLocalSequence,
    required int sourceSequence,
    required String kind,
    required int epoch,
  }) async {
    begun.add((owner: owner, epoch: epoch, source: sourceSequence, anchor: expectedLocalSequence));
    if (heldBegin != null) return heldBegin!.future;
    return ClipboardPendingLease(token: 'lease-${++nextToken}',
      sequence: ++nextSequence, sourceSequence: sourceSequence, kind: kind, epoch: epoch);
  }

  @override
  Future<bool> bindPending(String owner, ClipboardPendingLease lease, String requestId) async {
    bound.add((owner: owner, token: lease.token, request: requestId));
    return true;
  }

  @override
  Future<void> cancelPending(String owner, ClipboardPendingLease lease) async {
    cancelledLeases.add((owner: owner, lease: lease));
  }

  @override
  Future<void> allowManualRequest(String owner, String requestId) async {
    manual.add((owner: owner, request: requestId));
  }
}

Future<ClipboardPendingInitialization> _failedInitial(
  ClipboardPending pending, _MetadataBridge bridge,
) async {
  bridge.observationsToFail = 1;
  await expectLater(pending.arm(), throwsA(isA<PlatformException>()
      .having((error) => error.code, 'code', 'clipboard_busy')));
  final ticket = pending.failedInitialization;
  expect(ticket, isNotNull, reason: 'A fresh metadata observation failure is recoverable.');
  expect(pending.active, isFalse);
  return ticket!;
}

Future<void> _flush() async {
  for (var i = 0; i < 12; i++) {
    await Future<void>.delayed(Duration.zero);
  }
}

void main() {
  test('Virgin initialization proof is owner-specific and expires after arm or end', () async {
    final bridge = _MetadataBridge();
    final first = ClipboardPending('virgin-first', bridge);
    final other = ClipboardPending('virgin-other', bridge);
    final ended = ClipboardPending('virgin-ended', bridge);
    addTearDown(first.close);
    addTearDown(other.close);
    addTearDown(ended.close);
    final firstTicket = first.virginInitialization;
    final otherTicket = other.virginInitialization;
    final endedTicket = ended.virginInitialization;
    expect(firstTicket, isNotNull);
    expect(otherTicket, isNotNull);
    expect(endedTicket, isNotNull);
    final exactFirst = firstTicket!;
    final exactOther = otherTicket!;
    final exactEnded = endedTicket!;
    expect(first.epoch, 0);
    expect(first.active, isFalse);
    expect(first.failedInitialization, isNull);
    expect(first.publicationSequence, isNull);
    expect(first.initializationCurrent(exactFirst), isTrue);
    expect(first.initializationCurrent(exactOther), isFalse);
    expect(await first.retryInitialization(exactOther), isFalse);
    expect(bridge.observed, isEmpty);
    expect(bridge.cancelledOwners, isEmpty);
    expect(other.initializationCurrent(exactOther), isTrue);

    await ended.end();
    final currentEndedTicket = ended.virginInitialization;
    expect(currentEndedTicket, isNotNull,
      reason: 'Root-approved never-armed proof binds the current revision, including after grace end.');
    final exactCurrentEnded = currentEndedTicket!;
    expect(identical(exactCurrentEnded, exactEnded), isFalse);
    expect(ended.initializationCurrent(exactCurrentEnded), isTrue);
    expect(ended.failedInitialization, isNull);
    expect(ended.initializationCurrent(exactEnded), isFalse);
    expect(await ended.retryInitialization(exactEnded), isFalse);
    expect(bridge.observed, isEmpty,
      reason: 'The old pre-end proof cannot initialize the owner at its newer revision.');
    final retired = List.of(bridge.cancelledOwners);

    expect(await first.retryInitialization(exactFirst), isTrue);
    expect(first.active, isTrue);
    expect(first.epoch, greaterThan(0));
    expect(first.virginInitialization, isNull);
    expect(first.failedInitialization, isNull);
    expect(first.initializationCurrent(exactFirst), isFalse);
    expect(bridge.observed, [(owner: 'virgin-first', epoch: first.epoch)]);
    expect(bridge.cancelledOwners, retired,
      reason: 'A truly virgin initialization has no prior owner barrier to cancel.');
    expect(other.initializationCurrent(exactOther), isTrue);
    expect(first.publicationSequence, isNull);
    expect(bridge.begun, isEmpty);
    expect(bridge.bound, isEmpty);
    expect(bridge.manual, isEmpty);
    final observations = bridge.observed.length;
    expect(await first.retryInitialization(exactFirst), isFalse);
    expect(bridge.observed.length, observations);

    // The old proof stayed refused. A distinct current-revision proof may now
    // initialize the still-never-armed owner, without reopening source work.
    expect(await ended.retryInitialization(exactCurrentEnded), isTrue);
    expect(ended.active, isTrue);
    expect(ended.epoch, greaterThan(0));
    expect(ended.virginInitialization, isNull);
    expect(ended.initializationCurrent(exactEnded), isFalse);
    expect(ended.initializationCurrent(exactCurrentEnded), isFalse);
    expect(bridge.observed, [(owner: 'virgin-first', epoch: first.epoch),
      (owner: 'virgin-ended', epoch: ended.epoch)]);
    expect(bridge.cancelledOwners, retired,
      reason: 'Both attempts were never armed; no previous native epoch can be cancelled.');
    expect(first.active, isTrue);
    expect(other.initializationCurrent(exactOther), isTrue);
    expect(bridge.begun, isEmpty);
    expect(bridge.bound, isEmpty);
    expect(bridge.manual, isEmpty);
  });

  test('Late old retry returns false after a successor arm and bound source succeed', () async {
    final bridge = _MetadataBridge();
    final pending = ClipboardPending('superseded-owner', bridge);
    addTearDown(pending.close);
    final failedTicket = await _failedInitial(pending, bridge);
    final firstEpoch = pending.epoch;
    final oldObservation = Completer<int>();
    bridge.heldObservation = oldObservation;
    final oldRetry = pending.retryInitialization(failedTicket);
    await _flush();
    expect(bridge.observed, hasLength(2));
    final oldRetryEpoch = pending.epoch;
    expect(oldRetryEpoch, greaterThan(firstEpoch));
    expect(pending.active, isFalse);

    // The held call already captured this future. Only the independent public
    // successor arm receives the immediate, fresh metadata observation.
    bridge.heldObservation = null;
    await pending.arm();
    final successorEpoch = pending.epoch;
    expect(successorEpoch, greaterThan(oldRetryEpoch));
    expect(pending.active, isTrue);
    expect(bridge.observed, hasLength(3));
    final successorAnchor = bridge.nextSequence;
    await pending.source(81, 'text', successorEpoch);
    final admission = pending.admissionFor(81)!;
    final binding = await pending.bindAdmission(admission, 'successor-request', 'text');
    expect(binding, isNotNull);
    final successorBinding = binding!;
    final successorPublication = pending.publicationSequence;
    expect(pending.acceptsBinding(successorBinding), isTrue);
    expect(bridge.begun, [(owner: 'superseded-owner', epoch: successorEpoch,
      source: 81, anchor: successorAnchor)]);
    final observedBefore = List.of(bridge.observed);
    final boundBefore = List.of(bridge.bound);

    // Old _arm may return silently after detecting supersession. The public
    // retry result must remain about its own attempt, even though active is
    // now true because a different arm succeeded.
    oldObservation.complete(777);
    expect(await oldRetry, isFalse,
      reason: 'Success of a later owner is not success of this old retry.');
    expect(pending.epoch, successorEpoch);
    expect(pending.active, isTrue);
    expect(pending.arriving, isTrue);
    expect(pending.publicationSequence, successorPublication);
    expect(pending.acceptsBinding(successorBinding), isTrue);
    expect(pending.failedInitialization, isNull);
    expect(pending.virginInitialization, isNull);
    expect(pending.initializationCurrent(failedTicket), isFalse);
    expect(bridge.observed, observedBefore);
    expect(bridge.begun, hasLength(1));
    expect(bridge.bound, boundBefore);
    expect(bridge.cancelledLeases, isEmpty);
    expect(bridge.cancelledOwners.every((row) => row.owner == 'superseded-owner'
      && row.epoch != successorEpoch), isTrue,
      reason: 'Late completion may retire its old epoch, never the current binding.');
    expect(bridge.manual, isEmpty);
    expect(await pending.retryInitialization(failedTicket), isFalse);
    expect(bridge.observed, observedBefore);
  });

  test('Safe INIT observation failure recovers once for its exact live ticket', () async {
    final bridge = _MetadataBridge();
    final pending = ClipboardPending('owner-a', bridge);
    addTearDown(pending.close);
    final ticket = await _failedInitial(pending, bridge);
    expect(pending.initializationCurrent(ticket), isTrue);
    final oldEpoch = pending.epoch;
    expect(await pending.retryInitialization(ticket), isTrue);
    expect(pending.active, isTrue);
    expect(pending.epoch, greaterThan(oldEpoch));
    expect(bridge.observed, [(owner: 'owner-a', epoch: oldEpoch),
      (owner: 'owner-a', epoch: pending.epoch)]);
    expect(bridge.cancelledOwners, [(owner: 'owner-a', epoch: oldEpoch)]);
    expect(pending.failedInitialization, isNull);
    expect(pending.initializationCurrent(ticket), isFalse);
    final observed = bridge.observed.length, cancellations = bridge.cancelledOwners.length;
    expect(await pending.retryInitialization(ticket), isFalse);
    expect(bridge.observed.length, observed);
    expect(bridge.cancelledOwners.length, cancellations);
    expect(bridge.begun, isEmpty);
    expect(bridge.bound, isEmpty);
    expect(bridge.manual, isEmpty);
  });

  test('Failed old-owner cancel barrier never mints or reuses a recovery ticket', () async {
    final bridge = _MetadataBridge();
    final pending = ClipboardPending('barrier-owner', bridge);
    addTearDown(() async { bridge.cancelOwnerFailure = null; await pending.close(); });
    final oldTicket = await _failedInitial(pending, bridge);
    final oldEpoch = pending.epoch;
    final failure = PlatformException(code: 'clipboard_transport_closed');
    bridge.cancelOwnerFailure = failure;
    await expectLater(pending.arm(), throwsA(same(failure)));
    expect(bridge.cancelledOwners, [(owner: 'barrier-owner', epoch: oldEpoch)]);
    expect(pending.failedInitialization, isNull,
      reason: 'An uncertain cancellation must not be treated as safe INIT failure.');
    expect(pending.initializationCurrent(oldTicket), isFalse);
    expect(pending.active, isFalse);
    final observed = bridge.observed.length, cancellations = bridge.cancelledOwners.length;
    bridge.cancelOwnerFailure = null;
    expect(await pending.retryInitialization(oldTicket), isFalse);
    expect(bridge.observed.length, observed);
    expect(bridge.cancelledOwners.length, cancellations);
    expect(bridge.begun, isEmpty);
    expect(bridge.cancelledLeases, isEmpty);
  });

  test('One owner cannot consume another owner\'s failed initialization ticket', () async {
    final bridge = _MetadataBridge();
    final first = ClipboardPending('first-owner', bridge);
    final second = ClipboardPending('second-owner', bridge);
    addTearDown(first.close);
    addTearDown(second.close);
    final firstTicket = await _failedInitial(first, bridge);
    final secondTicket = await _failedInitial(second, bridge);
    final observed = bridge.observed.length;
    expect(second.initializationCurrent(firstTicket), isFalse);
    expect(await second.retryInitialization(firstTicket), isFalse);
    expect(bridge.observed.length, observed);
    expect(bridge.cancelledOwners, isEmpty);
    expect(first.initializationCurrent(firstTicket), isTrue);
    expect(second.initializationCurrent(secondTicket), isTrue);
    expect(await first.retryInitialization(firstTicket), isTrue);
    expect(second.initializationCurrent(secondTicket), isTrue);
    expect(bridge.cancelledOwners.every((row) => row.owner == 'first-owner'), isTrue);
  });

  test('Old recovery ticket cannot discard a current source lease or bound offer', () async {
    final bridge = _MetadataBridge();
    final pending = ClipboardPending('offer-owner', bridge);
    addTearDown(pending.close);
    final oldTicket = await _failedInitial(pending, bridge);
    await pending.arm();
    final epoch = pending.epoch;
    await pending.source(71, 'text', epoch);
    final admission = pending.admissionFor(71)!;
    final binding = await pending.bindAdmission(admission, 'live-request', 'text');
    expect(binding, isNotNull);
    final currentBinding = binding!;
    expect(pending.acceptsBinding(currentBinding), isTrue);
    expect(pending.arriving, isTrue);
    final sequence = pending.publicationSequence;
    final observed = bridge.observed.length, cancellations = bridge.cancelledOwners.length;
    expect(pending.failedInitialization, isNull);
    expect(await pending.retryInitialization(oldTicket), isFalse);
    expect(pending.epoch, epoch);
    expect(pending.publicationSequence, sequence);
    expect(pending.acceptsBinding(currentBinding), isTrue);
    expect(bridge.observed.length, observed);
    expect(bridge.cancelledOwners.length, cancellations);
    expect(bridge.cancelledLeases, isEmpty);
    expect(bridge.begun, hasLength(1));
    expect(bridge.bound, [(owner: 'offer-owner', token: currentBinding.lease.token, request: 'live-request')]);
    expect(bridge.manual, isEmpty);
  });

  test('Held old source completes only exact cancellation after owner end; recovery never replays it', () async {
    final bridge = _MetadataBridge();
    final pending = ClipboardPending('source-owner', bridge);
    addTearDown(pending.close);
    final oldTicket = await _failedInitial(pending, bridge);
    await pending.arm();
    final epoch = pending.epoch;
    bridge.heldBegin = Completer<ClipboardPendingLease?>();
    final source = pending.source(72, 'image', epoch);
    await _flush();
    expect(bridge.begun, hasLength(1));
    expect(pending.arriving, isTrue);
    final ended = pending.end();
    final observed = bridge.observed.length;
    expect(await pending.retryInitialization(oldTicket), isFalse);
    final exactLease = ClipboardPendingLease(token: 'exact-held-lease',
      sequence: 23, sourceSequence: 72, kind: 'image', epoch: epoch);
    bridge.heldBegin!.complete(exactLease);
    await source;
    await ended;
    expect(pending.active, isFalse);
    expect(pending.publicationSequence, isNull);
    expect(pending.admissionFor(72), isNull);
    expect(bridge.observed.length, observed);
    expect(bridge.begun, hasLength(1));
    expect(bridge.cancelledLeases, [(owner: 'source-owner', lease: exactLease)]);
    expect(bridge.cancelledOwners.last, (owner: 'source-owner', epoch: epoch));
    expect(bridge.bound, isEmpty);
    expect(bridge.manual, isEmpty);
  });

  for (final terminal in ['end', 'close']) {
    test('Late initialization observation cannot revive ownership after $terminal', () async {
      final bridge = _MetadataBridge();
      final pending = ClipboardPending('late-owner', bridge);
      addTearDown(pending.close);
      final ticket = await _failedInitial(pending, bridge);
      bridge.heldObservation = Completer<int>();
      final retry = pending.retryInitialization(ticket);
      await _flush();
      expect(bridge.observed, hasLength(2));
      final retryEpoch = pending.epoch;
      final retiring = terminal == 'close' ? pending.close() : pending.end();
      bridge.heldObservation!.complete(29);
      expect(await retry, isFalse);
      await retiring;
      expect(pending.active, isFalse);
      expect(pending.failedInitialization, isNull);
      expect(pending.initializationCurrent(ticket), isFalse);
      expect(await pending.retryInitialization(ticket), isFalse);
      expect(bridge.observed, hasLength(2));
      expect(bridge.cancelledOwners.where((row) => row.epoch == retryEpoch), isNotEmpty);
      expect(bridge.cancelledOwners.every((row) => row.owner == 'late-owner'), isTrue);
      expect(bridge.begun, isEmpty);
      expect(bridge.bound, isEmpty);
      expect(bridge.manual, isEmpty);
    });
  }

  test('Public Session recovery hook never reopens or replays an uncertain committed publication', () async {
    // Actual Session transfer/close protocol. This in-memory legacy bridge is
    // deliberately not evidence of the separately tested Windows opt-in gate.
    final socket = ClipboardSocket(), bridge = MemoryClipboard();
    final session = ClipboardSession(policy: const ClipboardPolicy.all(),
      url: Uri.parse('wss://fixture.invalid/api/v1/sessions/$sessionId/clipboard'), token: 'fixture',
      sessionId: sessionId, bridge: bridge, dialer: (_, _) async => socket);
    addTearDown(session.close);
    final starting = session.start();
    await _flush();
    socket.emit(ready);
    await starting;
    bridge.commitGate = Completer<void>();
    final receiving = expectLater(session.receiveRemote(), throwsA(isA<ClipboardTransferException>()
      .having((error) => error.code, 'code', 'clipboard_commit_uncertain')));
    final request = await socket.nextMessage('clipboard.read');
    final id = request['requestId'] as String;
    socket.emit({'type': 'clipboard.begin', 'requestId': id, 'format': 'files',
      'files': [{'name': 'complete.txt', 'size': 0}]});
    await _flush();
    socket.emit({'type': 'clipboard.item.end', 'requestId': id, 'index': 0, 'sha256': digest});
    await _flush();
    socket.emit({'type': 'clipboard.commit', 'requestId': id});
    await _flush();
    expect(bridge.events.where((event) => event == 'commit'), hasLength(1));
    final closing = session.close();
    bridge.commitGate!.complete();
    await closing;
    await receiving;
    expect(bridge.published, isTrue);
    expect(session.closed, isTrue);
    final events = List<String>.of(bridge.events);
    final wire = List<Map<String, dynamic>>.of(socket.sent);
    await session.retrySharingInitialization();
    await session.setSharingActive(true);
    await session.retrySharingInitialization();
    await _flush();
    expect(bridge.published, isTrue);
    expect(bridge.events, events, reason: 'Do not read, cancel, revoke or republish an uncertain commit.');
    expect(socket.sent, wire, reason: 'No fresh watch, request, ACK or completion may be sent after closure.');
    expect(socket.sent.where((value) => value['type'] == 'clipboard.applied'), isEmpty);
    expect(session.automatic, isFalse);
  });
}

// Source-only lifecycle coverage. Production ClipboardPulls, memory-only native
// adapter; no OS clipboard, user files, network, or polling timers.
import 'dart:async';
import 'dart:convert';

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_pull.dart';
import 'package:multitap_client/data/clipboard_pull_observation.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_onpaste_test.dart' as memory;
import 'clipboard_pull_test.dart' as fixture_base;

const _bound = Duration(seconds: 1);
const _bytes = <int>[7, 8, 9];
final _digest = sha256.convert(_bytes).toString();

class _LifecycleNative extends memory.OnPasteClipboard {
  Completer<void>? endGate, failGate, abortGate;
  final endEntered = Completer<void>();
  final failEntered = Completer<void>();
  final abortEntered = Completer<void>();
  PlatformException? endFailure, failFailure;
  final endCalls = <int>[];
  final failCalls = <int>[];
  final deliveredByRange = <int, List<int>>{};
  final completedByRange = <int, List<int>>{};
  final unknown = <int>{};

  @override
  Future<void> pullData(int rangeId, int offset, Uint8List bytes) async {
    await super.pullData(rangeId, offset, bytes);
    final into = deliveredByRange.putIfAbsent(rangeId, () => []);
    expect(offset, into.length);
    into.addAll(bytes);
  }

  @override
  Future<void> pullEnd(int rangeId, String digest) async {
    endCalls.add(rangeId);
    if (rangeId == 1) {
      if (!endEntered.isCompleted) endEntered.complete();
      await endGate?.future;
      if (endFailure != null) {
        unknown.add(rangeId);
        throw endFailure!;
      }
    }
    await super.pullEnd(rangeId, digest);
    completedByRange[rangeId] = List<int>.of(deliveredByRange[rangeId] ?? []);
  }

  @override
  Future<void> pullFail(int rangeId, String reason) async {
    failCalls.add(rangeId);
    if (rangeId == 1) {
      if (!failEntered.isCompleted) failEntered.complete();
      await failGate?.future;
      if (failFailure != null) {
        unknown.add(rangeId);
        throw failFailure!;
      }
    }
    await super.pullFail(rangeId, reason);
  }

  @override
  Future<void> pullAbort(String owner) async {
    if (!abortEntered.isCompleted) abortEntered.complete();
    await abortGate?.future;
    await super.pullAbort(owner);
  }

  void releaseGates() {
    for (final gate in [endGate, failGate, abortGate]) {
      if (gate != null && !gate.isCompleted) gate.complete();
    }
  }
}

class _Fixture {
  _Fixture() {
    pulls = ClipboardPulls(
      owner: 'terminal-lifecycle-fixture',
      bridge: bridge,
      onPaste: bridge,
      send: sent.add,
      limits: () => ClipboardLimits.pullProfile,
      idPrefix: 'pullc',
      changed: () => changes++,
      started: () {},
      failed: failures.add,
      observed: observations.add,
      mayServe: (_) => true,
      linger: Duration.zero,
    )..start();
  }

  final bridge = _LifecycleNative();
  final sent = <Map<String, Object?>>[];
  final failures = <String>[];
  final observations = <ClipboardPullObservation>[];
  int changes = 0;
  late final ClipboardPulls pulls;
  List<Map<String, Object?>> get requests =>
      sent.where((value) => value['type'] == 'clipboard.pull').toList();

  Future<void> request(int range, String offer, int index) async {
    bridge.platform(ClipboardPullRequest(range, offer, index, 0));
    await Future<void>.value();
  }

  Future<void> data(Map<String, Object?> request) => pulls.handle({
    'type': 'clipboard.chunk', 'requestId': request['requestId'],
    'index': request['index'], 'offset': 0, 'data': base64Encode(_bytes),
  });

  Future<void> end(Map<String, Object?> request) => pulls.handle({
    'type': 'clipboard.item.end', 'requestId': request['requestId'],
    'index': request['index'], 'sha256': _digest,
  });

  Future<void> initialize() async {
    pulls.offerPublished('completed-before-race', [fixture_base.tree('saved.bin', 3)]);
    await request(90, 'completed-before-race', 0);
    await data(requests.single);
    await end(requests.single);
    expect(bridge.completedByRange[90], _bytes);
    sent.clear(); failures.clear(); observations.clear();
    pulls.offerPublished('race-offer', [
      fixture_base.tree('first.bin', 3), fixture_base.tree('second.bin', 3),
    ]);
    await request(1, 'race-offer', 0);
    await data(requests.single);
    await request(2, 'race-offer', 1);
  }

  Future<void> finish(Map<String, Object?> request) async {
    await data(request).timeout(_bound);
    await end(request).timeout(_bound);
  }

  Future<void> close() async {
    bridge.releaseGates();
    await pulls.close('clipboard_session_closed').timeout(_bound);
  }
}

void main() {
  for (final terminal in ['clipboard.error', 'clipboard.cancel']) {
    test('held $terminal native pullFail failure diagnoses old identity without '
        'overwriting newer queued intent or starting it early', () async {
      final f = _Fixture();
      addTearDown(f.close);
      await f.initialize();
      final first = f.requests.single;
      f.bridge.failGate = Completer<void>();
      f.bridge.failFailure = PlatformException(code: 'clipboard_transfer_failed');
      final ending = f.pulls.handle({
        'type': terminal, 'requestId': first['requestId'],
        if (terminal == 'clipboard.error') 'reason': 'clipboard_read_failed',
      });
      await f.bridge.failEntered.future.timeout(_bound);
      await f.request(3, 'race-offer', 0);
      expect(f.requests, hasLength(1));
      expect(f.bridge.failCalls, [1]);
      f.bridge.failGate!.complete();
      await ending.timeout(_bound);
      expect(f.requests, hasLength(2));
      expect(f.failures, isEmpty, reason: 'Old error must not replace new UI.');
      expect(f.observations.where((value) => value.stage == 6), hasLength(1));
      await f.finish(f.requests.last);
      expect(f.requests, hasLength(3));
      await f.finish(f.requests.last);
      expect(f.pulls.active, isFalse);
      expect(f.pulls.userWaiting, isFalse);
      expect(f.bridge.completedByRange[90], _bytes);
      expect(f.bridge.completedByRange[2], _bytes);
      expect(f.bridge.completedByRange[3], _bytes);
      expect(f.bridge.completedByRange.containsKey(1), isFalse);
      expect(f.bridge.unknown, {1});
      expect(f.bridge.failCalls, [1]);
      expect(f.failures, isEmpty);
    });
  }

  test('cancel yield detaches only old queue; held end is not reentered and '
      'late cancel continuation preserves the newer active pull', () async {
    final f = _Fixture();
    addTearDown(f.close);
    await f.initialize();
    final first = f.requests.single;
    f.bridge.endGate = Completer<void>();
    f.bridge.abortGate = Completer<void>();
    final ending = f.end(first);
    await f.bridge.endEntered.future.timeout(_bound);
    final cancelling = f.pulls.cancel();
    await f.bridge.abortEntered.future.timeout(_bound);
    await f.request(3, 'race-offer', 0);
    expect(f.requests, hasLength(1));
    expect(f.bridge.endCalls.where((range) => range == 1), hasLength(1));
    f.bridge.endGate!.complete();
    await ending.timeout(_bound);
    expect(f.requests, hasLength(2));
    final newer = f.requests.last;
    expect(newer['index'], 0, reason: 'Cancelled old range2/index1 never starts.');
    f.bridge.abortGate!.complete();
    await cancelling.timeout(_bound);
    expect(f.bridge.failCalls, [2], reason: 'No native fail reentry for held1/new3.');
    expect(f.pulls.active, isTrue);
    expect(f.requests, hasLength(2));
    await f.finish(newer);
    expect(f.bridge.completedByRange[90], _bytes);
    expect(f.bridge.completedByRange[1], _bytes, reason: 'Actual end success persists.');
    expect(f.bridge.completedByRange[3], _bytes);
    expect(f.bridge.completedByRange.containsKey(2), isFalse);
    expect(f.pulls.active, isFalse);
    expect(f.pulls.userWaiting, isFalse);
    expect(f.failures, isEmpty);
  });

  test('close while terminal is held then late native exception cannot revive '
      'the feature, submit queued work or overwrite UI', () async {
    final f = _Fixture();
    addTearDown(f.close);
    await f.initialize();
    final first = f.requests.single;
    f.bridge.endGate = Completer<void>();
    f.bridge.endFailure = PlatformException(code: 'clipboard_transfer_failed');
    final ending = f.end(first);
    await f.bridge.endEntered.future.timeout(_bound);
    await f.pulls.close('clipboard_session_closed').timeout(_bound);
    final observationsAfterClose = f.observations.length;
    final changesAfterClose = f.changes;
    f.bridge.endGate!.complete();
    await ending.timeout(_bound);
    await f.request(3, 'race-offer', 0); // closed native event subscription
    expect(f.requests, hasLength(1));
    expect(f.pulls.active, isFalse);
    expect(f.pulls.userWaiting, isFalse);
    expect(f.failures, isEmpty);
    expect(f.observations, hasLength(observationsAfterClose));
    expect(f.changes, changesAfterClose);
    expect(f.bridge.completedByRange[90], _bytes);
    expect(f.bridge.completedByRange.containsKey(1), isFalse);
    expect(f.bridge.unknown, {1});
    expect(f.bridge.shutdowns, ['clipboard_session_closed']);
    expect(f.bridge.endCalls.where((range) => range == 1), hasLength(1));
  });
}

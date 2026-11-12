// Source-only regression proposal. No real clipboard, OS file, or network.
// Production ClipboardPulls is exercised directly, never reimplemented.
import 'dart:async';
import 'dart:convert';

import 'package:crypto/crypto.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_pull.dart';
import 'package:multitap_client/data/clipboard_pull_observation.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_pull_test.dart' as fixture_base;

const _payload = <int>[11, 22, 33];
final _digest = sha256.convert(_payload).toString();

class _TerminalNative extends fixture_base.PullClipboard {
  final endFailure = <int, PlatformException>{};
  final failFailure = <int, PlatformException>{};
  final endGates = <int, Completer<void>>{};
  final endAttempts = <int>[];
  final failAttempts = <int>[];
  final finalizationUnknown = <int>{};
  final deliveredBytes = <int, List<int>>{};
  final confirmedBytes = <int, List<int>>{};

  @override
  Future<void> pullData(int rangeId, int offset, Uint8List bytes) async {
    await super.pullData(rangeId, offset, bytes);
    final delivered = deliveredBytes.putIfAbsent(rangeId, () => []);
    expect(offset, delivered.length);
    delivered.addAll(bytes);
  }

  @override
  Future<void> pullEnd(int rangeId, String digest) async {
    endAttempts.add(rangeId);
    await endGates[rangeId]?.future;
    final failure = endFailure[rangeId];
    if (failure != null) {
      // A thrown adapter completion is not native finalization evidence.
      finalizationUnknown.add(rangeId);
      throw failure;
    }
    await super.pullEnd(rangeId, digest);
    confirmedBytes[rangeId] = List<int>.of(deliveredBytes[rangeId] ?? []);
  }

  @override
  Future<void> pullFail(int rangeId, String reason) async {
    failAttempts.add(rangeId);
    final failure = failFailure[rangeId];
    if (failure != null) {
      finalizationUnknown.add(rangeId);
      throw failure;
    }
    await super.pullFail(rangeId, reason);
  }

  void releaseGates() {
    for (final gate in endGates.values) {
      if (!gate.isCompleted) gate.complete();
    }
  }
}

class _Fixture {
  _Fixture() {
    pulls = ClipboardPulls(
      owner: 'terminal-fixture-owner',
      bridge: bridge,
      send: (value) => sent.add(Map<String, Object?>.of(value)),
      limits: () => ClipboardLimits.pullProfile,
      idPrefix: 'pullc',
      changed: () => changes++,
      started: () => starts++,
      failed: failures.add,
      mayServe: (_) => true,
      linger: Duration.zero,
      observed: observations.add,
    )..start();
  }

  final bridge = _TerminalNative();
  final sent = <Map<String, Object?>>[];
  final failures = <String>[];
  final observations = <ClipboardPullObservation>[];
  int changes = 0, starts = 0;
  late final ClipboardPulls pulls;

  List<Map<String, Object?>> get requests =>
      sent.where((value) => value['type'] == 'clipboard.pull').toList();

  void publish(String offerId, int files) {
    pulls.offerPublished(offerId, [
      for (var i = 0; i < files; i++) fixture_base.tree('fixture-$i.bin', 3),
    ]);
  }

  Future<void> request(int range, String offerId, int index) async {
    bridge.platform(ClipboardPullRequest(range, offerId, index, 0));
    // No elapsed-time test or polling: flush the fake event's async continuation.
    await Future<void>.value();
  }

  Future<void> data(Map<String, Object?> request) => pulls.handle({
    'type': 'clipboard.chunk',
    'requestId': request['requestId'],
    'index': request['index'],
    'offset': 0,
    'data': base64Encode(_payload),
  });

  Future<void> end(Map<String, Object?> request) => pulls.handle({
    'type': 'clipboard.item.end',
    'requestId': request['requestId'],
    'index': request['index'],
    'sha256': _digest,
  });

  Future<void> warmupCompletedBytes() async {
    publish('prior-completed-offer', 1);
    await request(90, 'prior-completed-offer', 0);
    final request90 = requests.single;
    await data(request90);
    await end(request90);
    expect(bridge.confirmedBytes[90], _payload);
    expect(pulls.active, isFalse);
    sent.clear();
    failures.clear();
    observations.clear();
  }

  void expectPreservedAndUnknown() {
    expect(bridge.confirmedBytes[90], _payload,
        reason: 'Already completed local fixture bytes must persist.');
    expect(bridge.deliveredBytes[1], _payload);
    expect(bridge.finalizationUnknown, contains(1));
    expect(bridge.confirmedBytes.containsKey(1), isFalse,
        reason: 'Thrown native completion is not a committed-file receipt.');
    expect(bridge.ended.any((value) => value.$1 == 1), isFalse);
  }

  Future<void> close() async {
    bridge.releaseGates();
    await pulls.close('clipboard_session_closed');
  }
}

void main() {
  test('pullEnd PlatformException settles its own failure and starts queued '
      'pull2 without discarding completed local bytes', () async {
    final f = _Fixture();
    addTearDown(f.close);
    await f.warmupCompletedBytes();
    f.publish('tested-offer', 2);
    await f.request(1, 'tested-offer', 0);
    await f.request(2, 'tested-offer', 1);
    final first = f.requests.single;
    await f.data(first);
    f.bridge.endFailure[1] = PlatformException(code: 'clipboard_transfer_failed');
    await f.end(first);

    // Intended regression: baseline currently leaves only request1 submitted.
    expect(f.requests, hasLength(2), reason: 'Queued pull2 must start.');
    expect(f.failures, ['clipboard_transfer_failed']);
    expect(f.observations.where((value) => value.stage == 6), hasLength(1));
    expect(f.pulls.active, isTrue);
    final second = f.requests.last;
    expect(second['index'], 1);
    expect(second['requestId'], isNot(first['requestId']));
    await f.data(second);
    await f.end(second);
    expect(f.bridge.confirmedBytes[2], _payload);
    expect(f.pulls.active, isFalse);
    expect(f.pulls.userWaiting, isFalse);
    f.expectPreservedAndUnknown();
  });

  for (final terminal in ['clipboard.error', 'clipboard.cancel']) {
    test('$terminal pullFail PlatformException still starts and settles queued '
        'pull2; it must not claim native finalization succeeded', () async {
      final f = _Fixture();
      addTearDown(f.close);
      await f.warmupCompletedBytes();
      f.publish('tested-offer', 2);
      await f.request(1, 'tested-offer', 0);
      await f.request(2, 'tested-offer', 1);
      final first = f.requests.single;
      await f.data(first);
      f.bridge.failFailure[1] = PlatformException(code: 'clipboard_transfer_failed');
      await f.pulls.handle({
        'type': terminal,
        'requestId': first['requestId'],
        if (terminal == 'clipboard.error') 'reason': 'clipboard_read_failed',
      });

      // Intended regression: baseline currently skips _startNext and failed().
      expect(f.requests, hasLength(2), reason: 'Queued pull2 must start.');
      expect(f.failures, hasLength(1));
      expect(f.failures.single, 'clipboard_transfer_failed');
      expect(f.observations.where((value) => value.stage == 6), hasLength(1));
      expect(f.bridge.failAttempts, [1]);
      expect(f.bridge.failedRanges.any((value) => value.$1 == 1), isFalse);
      final second = f.requests.last;
      await f.data(second);
      await f.end(second);
      expect(f.bridge.confirmedBytes[2], _payload);
      expect(f.pulls.active, isFalse);
      expect(f.pulls.userWaiting, isFalse);
      f.expectPreservedAndUnknown();
    });
  }

  test('held old native terminal serializes queued requests; its failure is '
      'diagnosed once without overwriting newer revision UI', () async {
    final f = _Fixture();
    addTearDown(f.close);
    await f.warmupCompletedBytes();
    f.publish('tested-offer', 2);
    await f.request(1, 'tested-offer', 0);
    await f.request(2, 'tested-offer', 1);
    final first = f.requests.single;
    await f.data(first);
    final gate = Completer<void>();
    f.bridge.endGates[1] = gate;
    f.bridge.endFailure[1] = PlatformException(code: 'clipboard_transfer_failed');
    final oldTerminal = f.end(first);
    await Future<void>.value();
    expect(f.bridge.endAttempts, contains(1));

    // Newer intent may queue while old terminal work is held. Native terminal
    // finalization still owns admission: neither queued2 nor new3 may start yet.
    await f.request(3, 'tested-offer', 0);
    // Intended regression: baseline currently starts queued2 prematurely here.
    expect(f.requests, hasLength(1), reason: 'Await old native terminal first.');
    expect(f.bridge.ended.map((value) => value.$1), [90]);
    expect(f.failures, isEmpty);
    expect(f.pulls.active, isTrue);
    gate.complete();
    await oldTerminal;
    expect(f.requests, hasLength(2), reason: 'Start queued2 exactly once.');
    final newer = f.requests.last;
    expect(newer['index'], 1);
    expect(newer['requestId'], isNot(first['requestId']));
    expect(f.pulls.active, isTrue);
    expect(f.pulls.progress?.fileIndex, 2);
    // The request3 revision superseded old UI intent. Old failure still needs
    // its own diagnostic, but global failed() must not overwrite new status.
    expect(f.failures, isEmpty);
    expect(f.observations.where((value) => value.stage == 6), hasLength(1));
    f.expectPreservedAndUnknown();

    await f.data(newer);
    await f.end(newer);
    expect(f.bridge.confirmedBytes[2], _payload);
    expect(f.requests, hasLength(3));
    final third = f.requests.last;
    // A late duplicate terminal for request1 must not clear request3 either.
    await f.end(first);
    expect(f.requests, hasLength(3));
    expect(f.pulls.active, isTrue);
    expect(f.pulls.progress?.fileIndex, 1);
    await f.data(third);
    await f.end(third);
    expect(f.bridge.confirmedBytes[3], _payload);
    expect(f.pulls.active, isFalse);
    expect(f.pulls.userWaiting, isFalse);
    f.expectPreservedAndUnknown();
    expect(f.failures, isEmpty, reason: 'No stale global UI failure.');
    expect(f.observations.where((value) => value.stage == 6), hasLength(1));
  });
}

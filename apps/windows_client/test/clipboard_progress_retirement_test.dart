import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_pull.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_pull_test.dart' as fixture;

class _Bridge extends fixture.PullClipboard {
  Completer<void>? failGate, endGate;
  @override
  Future<void> pullFail(int rangeId, String reason) async {
    await failGate?.future;
    await super.pullFail(rangeId, reason);
  }

  @override
  Future<void> pullEnd(int rangeId, String digest) async {
    await endGate?.future;
    await super.pullEnd(rangeId, digest);
  }

  void releaseGates() {
    for (final gate in [failGate, endGate]) {
      if (gate != null && !gate.isCompleted) gate.complete();
    }
  }
}

class _Transfer {
  _Transfer() {
    pulls = ClipboardPulls(
      owner: 'progress-retirement-fixture',
      bridge: bridge,
      send: (message) => sent.add(Map<String, dynamic>.of(message)),
      limits: () => ClipboardLimits.pullProfile,
      idPrefix: 'pullc',
      changed: () {},
      started: () {},
      failed: failures.add,
      mayServe: (_) => true,
      linger: Duration.zero,
    )..start();
  }
  final bridge = _Bridge();
  final sent = <Map<String, dynamic>>[];
  final failures = <String>[];
  late final ClipboardPulls pulls;

  void publish(String id, {int size = 8}) => pulls.offerPublished(id, [
    fixture.tree('$id.bin', size),
  ]);

  Future<Map<String, dynamic>> request(String id, int range) async {
    bridge.platform(ClipboardPullRequest(range, id, 0, 0));
    await Future<void>.value();
    return sent.lastWhere((message) => message['type'] == 'clipboard.pull');
  }

  Future<void> data(Map<String, dynamic> request, int offset, List<int> bytes) =>
      pulls.handle({
        'type': 'clipboard.chunk',
        'requestId': request['requestId'],
        'index': 0,
        'offset': offset,
        'data': base64Encode(bytes),
      });

  Future<void> close() async {
    bridge.releaseGates();
    await pulls.close('clipboard_session_closed');
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('withdrawn active range keeps its known progress until native retirement', () async {
    final copy = _Transfer();
    try {
      copy.publish('old');
      final request = await copy.request('old', 1);
      await copy.data(request, 0, [1, 2, 3, 4]);
      expect(copy.pulls.progress?.transferred, 4);
      copy.bridge.failGate = Completer<void>();
      copy.pulls.offerWithdrawn('old');
      expect(copy.pulls.active, isTrue);
      expect(copy.pulls.progress?.total, 8);
      expect(copy.pulls.progress?.transferred, 4);
      expect(copy.pulls.progress?.fileName, 'old.bin');

      final retirement = copy.data(request, 4, [5, 6, 7, 8]);
      await Future<void>.value();
      expect(copy.pulls.progress?.total, 8);
      expect(copy.bridge.data, hasLength(1));
      expect(copy.sent.where((m) => m['type'] == 'clipboard.cancel'), hasLength(1));
      copy.bridge.releaseGates();
      await retirement;
      expect(copy.bridge.failedRanges.single.$2, 'clipboard_offer_expired');
      expect(copy.pulls.progress, isNull);
    } finally {
      await copy.close();
    }
  });

  // 2026-11-09: newer copies keep the offer a paste is reading (the unread "next" leaves instead), so the paste
  // finishes; progress then moves to the newest copy's own paste.
  test('offer-cache eviction cannot replace active progress with a newer copy', () async {
    final copy = _Transfer();
    try {
      copy.publish('old');
      final request = await copy.request('old', 2);
      await copy.data(request, 0, [1, 2, 3, 4]);
      copy.publish('next', size: 32);
      copy.publish('newest', size: 64);
      expect(copy.pulls.active, isTrue);
      expect(copy.pulls.progress?.fileName, 'old.bin');
      expect(copy.pulls.progress?.total, 8);
      expect(copy.pulls.progress?.transferred, 4);

      await copy.data(request, 4, [5, 6, 7, 8]);
      expect(copy.bridge.data, hasLength(2));
      expect(copy.bridge.failedRanges, isEmpty);
      expect(copy.pulls.progress?.transferred, 8);
      await copy.pulls.handle({
        'type': 'clipboard.item.end',
        'requestId': request['requestId'],
        'index': 0,
        'sha256': 'a' * 64,
      });
      await copy.request('newest', 3);
      expect(copy.pulls.progress?.fileName, 'newest.bin');
      expect(copy.pulls.progress?.total, 64);
      expect(copy.pulls.progress?.transferred, 0);
    } finally {
      await copy.close();
    }
  });

  // What a newer copy in the VM leaves alone (ClipboardSession._retirePeerOffers).
  test('pasting: a running range, then a file just finished; never an unread offer', () async {
    final copy = _Transfer();
    try {
      copy.publish('old');
      copy.publish('other');
      expect(copy.pulls.pasting('old'), isFalse, reason: 'nothing read yet');
      final request = await copy.request('old', 5);
      expect(copy.pulls.pasting('old'), isTrue);
      await copy.data(request, 0, [1, 2, 3, 4, 5, 6, 7, 8]);
      await copy.pulls.handle({
        'type': 'clipboard.item.end',
        'requestId': request['requestId'],
        'index': 0,
        'sha256': 'a' * 64,
      });
      expect(copy.pulls.active, isFalse);
      expect(copy.pulls.pasting('old'), isTrue, reason: 'the next file follows');
      expect(copy.pulls.pasting('other'), isFalse);
    } finally {
      await copy.close();
    }
  });

  test('held native finalization retains exact progress without completing a new offer', () async {
    final copy = _Transfer();
    try {
      copy.publish('old');
      final request = await copy.request('old', 4);
      await copy.data(request, 0, [1, 2, 3, 4, 5, 6, 7, 8]);
      copy.bridge.endGate = Completer<void>();
      final finish = copy.pulls.handle({
        'type': 'clipboard.item.end',
        'requestId': request['requestId'],
        'index': 0,
        'sha256': 'a' * 64,
      });
      await Future<void>.value();
      copy.publish('next');
      copy.publish('newest', size: 64);
      expect(copy.pulls.active, isTrue);
      expect(copy.pulls.progress?.total, 8);
      expect(copy.pulls.progress?.transferred, 8);
      expect(copy.pulls.progress?.fileName, 'old.bin');
      expect(copy.bridge.ended, isEmpty);
      copy.bridge.releaseGates();
      await finish;
      await copy.request('newest', 5);
      expect(copy.pulls.progress?.transferred, 0);
      expect(copy.pulls.progress?.total, 64);
    } finally {
      await copy.close();
    }
  });
}

// Protective source-only regression. No OS clipboard, files, or network.
import 'dart:async';
import 'dart:convert';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_pull.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_pull_test.dart' as fixture_base;

class _HeldEnd extends fixture_base.PullClipboard {
  final gate = Completer<void>();
  int endCalls = 0, failCalls = 0;
  @override
  Future<void> pullEnd(int rangeId, String digest) async {
    endCalls++;
    if (rangeId == 1) await gate.future;
    await super.pullEnd(rangeId, digest);
  }

  @override
  Future<void> pullFail(int rangeId, String reason) async {
    failCalls++;
    await super.pullFail(rangeId, reason);
  }
}

void main() {
  test('held terminal owns native finalization once; duplicate/end/error/cancel '
      'cannot reenter it or start the next pull early', () async {
    final bridge = _HeldEnd();
    final sent = <Map<String, Object?>>[];
    final failures = <String>[];
    final pulls = ClipboardPulls(
      owner: 'held-terminal-fixture',
      bridge: bridge,
      send: sent.add,
      limits: () => ClipboardLimits.pullProfile,
      idPrefix: 'pullc',
      changed: () {},
      started: () {},
      failed: failures.add,
      mayServe: (_) => true,
      linger: Duration.zero,
    )..start();
    addTearDown(() async {
      if (!bridge.gate.isCompleted) bridge.gate.complete();
      await pulls.close('clipboard_session_closed');
    });
    pulls.offerPublished('held-offer', [fixture_base.tree('fixture.bin', 3)]);
    bridge.platform(const ClipboardPullRequest(1, 'held-offer', 0, 0));
    await Future<void>.value();
    final first = sent.single;
    final payload = [4, 5, 6];
    await pulls.handle({
      'type': 'clipboard.chunk', 'requestId': first['requestId'],
      'index': 0, 'offset': 0, 'data': base64Encode(payload),
    });
    final terminal = {
      'type': 'clipboard.item.end', 'requestId': first['requestId'],
      'index': 0, 'sha256': sha256.convert(payload).toString(),
    };
    final ending = pulls.handle(terminal);
    await Future<void>.value();
    expect(bridge.endCalls, 1);
    for (final duplicate in [
      terminal,
      {'type': 'clipboard.error', 'requestId': first['requestId'], 'reason': 'clipboard_read_failed'},
      {'type': 'clipboard.cancel', 'requestId': first['requestId']},
    ]) {
      final reply = pulls.handle(duplicate);
      await Future<void>.value();
      expect(bridge.endCalls, 1);
      expect(bridge.failCalls, 0);
      // Bound a faulty duplicate await without depending on a fixed number of
      // async continuation turns. There is no external work or polling here.
      await reply.timeout(const Duration(seconds: 1));
    }
    bridge.platform(const ClipboardPullRequest(2, 'held-offer', 0, 0));
    await Future<void>.value();
    expect(sent.where((value) => value['type'] == 'clipboard.pull'), hasLength(1));
    bridge.gate.complete();
    await ending;
    expect(bridge.ended.map((value) => value.$1), [1]);
    expect(sent.where((value) => value['type'] == 'clipboard.pull'), hasLength(2));
    expect(bridge.endCalls, 1);
    expect(bridge.failCalls, 0);
    expect(failures, isEmpty);
  });
}

import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_pull.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_pull_test.dart' as fixture;

/// A paste of several files on this PC: Explorer asks for each file only after
/// it has written (and Defender has scanned) the one before. The progress
/// notice stays through that gap, so three files are one notice (1/3, 2/3,
/// 3/3), not three notices.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  late fixture.PullClipboard bridge;
  late List<Map<String, dynamic>> sent;
  late ClipboardPulls pulls;

  setUp(() {
    bridge = fixture.PullClipboard();
    sent = [];
    pulls = ClipboardPulls(
      owner: 'between-files-fixture',
      bridge: bridge,
      send: (message) => sent.add(Map<String, dynamic>.of(message)),
      limits: () => ClipboardLimits.pullProfile,
      idPrefix: 'pullc',
      changed: () {},
      started: () {},
      failed: (_) {},
      mayServe: (_) => true,
      linger: const Duration(milliseconds: 30),
      betweenFiles: const Duration(milliseconds: 600),
    )..start();
    pulls.offerPublished('copy', [
      fixture.tree('a.bin', 4),
      fixture.tree('b.bin', 4),
    ]);
  });

  tearDown(() => pulls.close('clipboard_session_closed'));

  Future<Map<String, dynamic>> request(int range, int index) async {
    bridge.platform(ClipboardPullRequest(range, 'copy', index, 0));
    await Future<void>.value();
    return sent.lastWhere((message) => message['type'] == 'clipboard.pull');
  }

  Future<void> receive(Map<String, dynamic> pull, int index) async {
    await pulls.handle({
      'type': 'clipboard.chunk',
      'requestId': pull['requestId'],
      'index': index,
      'offset': 0,
      'data': base64Encode([1, 2, 3, 4]),
    });
    await pulls.handle({
      'type': 'clipboard.item.end',
      'requestId': pull['requestId'],
      'index': index,
      'sha256': 'a' * 64,
    });
  }

  test('the notice waits for the next file of the paste, and ends with the last', () async {
    await receive(await request(1, 0), 0);
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(
      pulls.active,
      isTrue,
      reason: 'longer than the short linger: the second file is still to come',
    );
    expect(pulls.progress?.fileIndex, 1);
    expect(pulls.progress?.fileCount, 2);
    expect(pulls.progress?.transferred, 4);
    expect(pulls.progress?.total, 8);

    await receive(await request(2, 1), 1);
    expect(pulls.active, isFalse, reason: 'the last file ended');
  });

  test('a cancelled file keeps only the short linger', () async {
    final pull = await request(1, 0);
    await pulls.handle({
      'type': 'clipboard.chunk',
      'requestId': pull['requestId'],
      'index': 0,
      'offset': 0,
      'data': base64Encode([1, 2]),
    });
    await pulls.handle({
      'type': 'clipboard.cancel',
      'requestId': pull['requestId'],
    });
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(pulls.active, isFalse);
  });

  test('the notice ends after the wait when the next file never comes', () async {
    await receive(await request(1, 0), 0);
    await Future<void>.delayed(const Duration(milliseconds: 900));
    expect(pulls.active, isFalse);
  });
}

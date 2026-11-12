import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_pull.dart';
import 'package:multitap_client/data/native_clipboard.dart';
import 'package:multitap_client/data/paste_target.dart';
import 'package:multitap_client/ui/clipboard_transfer_notice.dart';

import 'clipboard_pull_test.dart' as fixture;

class _Probe implements PasteTargetProbe {
  _Probe(this.folder, this.times);
  final PasteDestination? folder;
  final Map<String, DateTime> times;
  final looked = <String>[];
  Completer<void>? gate;
  @override
  Future<PasteDestination?> foregroundFolder() async => folder;
  @override
  Future<DateTime?> created(String path) async {
    looked.add(path);
    final value = times[path];
    await gate?.future;
    return value;
  }

  @override
  Future<bool> open(String folder, {String? select}) async => true;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('numbers read plainly', () {
    test('counts are grouped by thousands', () {
      expect(groupDigits(0), '0');
      expect(groupDigits(999), '999');
      expect(groupDigits(1702), '1,702');
      expect(groupDigits(20343), '20,343');
      expect(groupDigits(1234567), '1,234,567');
    });

    test('the time left is rounded so it does not tick', () {
      expect(transferEta(const Duration(seconds: 4)), '곧 끝나요');
      expect(transferEta(const Duration(seconds: 41)), '약 45초 남음');
      expect(transferEta(const Duration(seconds: 45)), '약 45초 남음');
      expect(transferEta(const Duration(seconds: 57)), '약 1분 남음');
      expect(transferEta(const Duration(seconds: 250)), '약 4분 남음');
      expect(transferEta(const Duration(minutes: 83)), '약 1시간 20분 남음');
      expect(transferEta(const Duration(minutes: 120)), '약 2시간 남음');
      expect(transferEta(const Duration(days: 2)), isNull);
      expect(transferEta(null), isNull);
    });

    test('a failure splits into its cause and the next step', () {
      expect(
        splitReason('받는 쪽 디스크 공간이 부족해요. 공간을 확보한 뒤 다시 시도하세요.'),
        ('받는 쪽 디스크 공간이 부족해요.', '공간을 확보한 뒤 다시 시도하세요.'),
      );
      expect(splitReason('클립보드 응답 시간이 초과됐어요.'), ('클립보드 응답 시간이 초과됐어요.', ''));
      expect(splitReason('공간 부족'), ('공간 부족', ''));
    });
  });

  group('the destination of a paste', () {
    final start = DateTime(2026, 11, 2, 9);
    const downloads = PasteDestination(
      path: r'C:\Users\<user>\Downloads',
      name: '다운로드',
    );

    test('is confirmed by an item created there as the paste ran', () async {
      final probe = _Probe(downloads, {
        r'C:\Users\<user>\Downloads\b.txt': start.add(const Duration(seconds: 1)),
      });
      final check = PasteDestinationCheck(probe, since: start)..start();
      expect(await check.check(['a', 'b.txt']), isTrue);
      expect(check.confirmed, downloads);
      expect(check.item, 'b.txt');
    });

    test('allows the item to have been created a moment before the first read', () async {
      final probe = _Probe(downloads, {
        r'C:\Users\<user>\Downloads\a': start.subtract(const Duration(milliseconds: 1500)),
      });
      final check = PasteDestinationCheck(probe, since: start)..start();
      expect(await check.check(['a']), isTrue);
    });

    test('ignores an older item of the same name', () async {
      final probe = _Probe(downloads, {
        r'C:\Users\<user>\Downloads\a': start.subtract(const Duration(seconds: 3)),
      });
      final check = PasteDestinationCheck(probe, since: start)..start();
      expect(await check.check(['a']), isFalse);
      expect(check.confirmed, isNull);
    });

    test('never looks outside the folder for odd names', () async {
      final probe = _Probe(downloads, {});
      final check = PasteDestinationCheck(probe, since: start)..start();
      expect(await check.check(['..', r'x\y', 'c:z', '', '.']), isFalse);
      expect(probe.looked, isEmpty);
    });

    test('claims nothing without a folder in front, or before start', () async {
      final none = PasteDestinationCheck(_Probe(null, {}), since: start)..start();
      expect(await none.check(['a']), isFalse);
      final unstarted = PasteDestinationCheck(
        _Probe(downloads, {r'C:\Users\<user>\Downloads\a': start}),
        since: start,
      );
      expect(await unstarted.check(['a']), isFalse);
    });

    test('a look asked while another runs looks again after it', () async {
      final probe = _Probe(downloads, {})..gate = Completer<void>();
      final check = PasteDestinationCheck(probe, since: start)..start();
      final first = check.check(['a']);
      while (probe.looked.isEmpty) {
        await Future<void>.value();
      }
      // The item appears while the first look is still out.
      final second = check.check(['a']);
      probe.times[r'C:\Users\<user>\Downloads\a'] = start;
      probe.gate!.complete();
      expect(await first, isFalse);
      expect(await second, isTrue);
      expect(probe.looked, hasLength(2));
    });
  });

  group('the runner channel', () {
    const channel = MethodChannel('multitap/paste-target-test');
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    tearDown(() => messenger.setMockMethodCallHandler(channel, null));

    test('reads a folder, and nothing from an odd or missing reply', () async {
      Object? reply = {'path': r'C:\Users\<user>\Desktop', 'name': '바탕 화면'};
      messenger.setMockMethodCallHandler(channel, (call) async {
        expect(call.method, 'capture');
        return reply;
      });
      const probe = NativePasteTargetProbe(channel);
      expect(
        await probe.foregroundFolder(),
        const PasteDestination(path: r'C:\Users\<user>\Desktop', name: '바탕 화면'),
      );
      reply = null;
      expect(await probe.foregroundFolder(), isNull);
      reply = {'path': '', 'name': 'x'};
      expect(await probe.foregroundFolder(), isNull);
      reply = {'path': 3};
      expect(await probe.foregroundFolder(), isNull);
      messenger.setMockMethodCallHandler(channel, null);
      expect(await probe.foregroundFolder(), isNull, reason: 'no runner');
    });

    test('opens a folder with the pasted item selected', () async {
      final calls = <Object?>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call.arguments);
        return true;
      });
      const probe = NativePasteTargetProbe(channel);
      expect(await probe.open(r'C:\a', select: 'b'), isTrue);
      expect(calls.single, {'folder': r'C:\a', 'select': 'b'});
    });
  });

  test('a paste names its own items: the first segments, a few at most', () async {
    final bridge = fixture.PullClipboard();
    final pulls = ClipboardPulls(
      owner: 'paste-destination-fixture',
      bridge: bridge,
      send: (_) {},
      limits: () => ClipboardLimits.pullProfile,
      idPrefix: 'pullc',
      changed: () {},
      started: () {},
      failed: (_) {},
      mayServe: (_) => true,
      linger: Duration.zero,
    )..start();
    try {
      pulls.offerPublished('o', [
        fixture.folder('사진'),
        fixture.tree(r'사진\a.jpg', 4),
        fixture.tree(r'사진\b.jpg', 4),
        fixture.tree('readme.txt', 4),
        fixture.tree('c.bin', 4),
        fixture.tree('d.bin', 4),
        fixture.tree('e.bin', 4),
      ]);
      bridge.platform(ClipboardPullRequest(1, 'o', 0, 0));
      await Future<void>.value();
      expect(pulls.progress?.sending, isFalse);
      expect(pulls.progress?.topLevel, ['사진', 'readme.txt', 'c.bin', 'd.bin']);
    } finally {
      await pulls.close('clipboard_session_closed');
    }
  });
}

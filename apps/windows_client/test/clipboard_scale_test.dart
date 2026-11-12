import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_echo_test.dart' as echo;
import 'clipboard_pull_test.dart' as pull;
import 'clipboard_session_test.dart' as base;

// The scale revision (X-Multitap-Clipboard-Scale: 1): no limit on file sizes, totals or top-level items, text and HTML up to 64 MiB,
// an entry guard, 1024-character paths on the wire of which 259 can be pasted, a precise reason for every limit that remains, and
// a newer copy that replaces an older transfer still crossing.
const mib = 1024 * 1024, gib = 1024 * mib;

/// The relay's `limits` for two peers that announced the pull profile and the scale revision.
Map<String, dynamic> scaleLimits() => {
  ...pull.pullLimits(),
  'scaleProfile': 1,
  'maxFiles': 200000,
  'maxFileBytes': 9007199254740991,
  'maxTotalBytes': 9007199254740991,
  'maxTextUnits': 64 * mib,
  'maxTextBytes': 64 * mib,
  'maxEntries': 200000,
  'maxDepth': 512,
  'maxPathChars': 1024,
  'maxRichBytes': 64 * mib,
};
Map<String, dynamic> scaleReady() => {...base.ready, 'limits': scaleLimits()};

/// The Windows adapter's stand-in with the scale revision.
class ScaleClipboard extends pull.PullClipboard implements ClipboardScaleBridge {
  @override
  bool get supportsScale => true;
  bool scaleOn = false;
  int richBytesSet = 0;
  @override
  set scale(bool value) => scaleOn = value;
  @override
  set richBytes(int value) => richBytesSet = value;
}

void main() {
  late base.ClipboardSocket socket;
  late ScaleClipboard bridge;
  late ClipboardSession session;

  ClipboardSession make({ClipboardBridge? with_}) => ClipboardSession(
    policy: const ClipboardPolicy.all(),
    url: Uri.parse('wss://cloud.example.com/api/v1/sessions/${base.sessionId}/clipboard'),
    token: 'test-token',
    sessionId: base.sessionId,
    bridge: with_ ?? bridge,
    dialer: (url, token) async => socket,
    pullLinger: Duration.zero,
  );
  Future<void> connect([Map<String, dynamic>? ready]) async {
    final starting = session.start();
    await base.settle();
    socket.emit(ready ?? scaleReady());
    await starting;
  }

  setUp(() {
    socket = base.ClipboardSocket();
    bridge = ScaleClipboard();
    session = make();
  });
  tearDown(() async => session.close());

  group('negotiation', () {
    test('the scale numbers are accepted exactly, and only on top of the pull profile', () {
      final parsed = ClipboardLimits.parse(scaleLimits(), chunkBytes: 32768);
      expect(parsed, same(ClipboardLimits.scaleRevision));
      expect(parsed!.scale && parsed.pull && parsed.formats, isTrue);
      expect(parsed.maxFileBytes, 9007199254740991);
      expect(parsed.maxTextBytes, 64 * mib);
      expect(parsed.maxComponentChars, 255);
      expect(parsed.pastePathChars, 259);
      expect(ClipboardLimits.pullProfile.scale, isFalse);
      expect(ClipboardLimits.pullProfile.maxComponentChars, 240);
      expect(ClipboardLimits.pullProfile.pastePathChars, 160);
      for (final change in <String, Object>{
        'scaleProfile': 2,
        'maxFiles': 128,
        'maxFileBytes': 8 * gib,
        'maxTotalBytes': 8 * gib,
        'maxTextBytes': mib,
        'maxTextUnits': mib,
        'maxEntries': 10000,
        'maxDepth': 32,
        'maxPathChars': 160,
        'maxRichBytes': 8 * mib,
        'pullProfile': 2,
        'maxOffers': 3,
        'extra': 1,
      }.entries) {
        expect(ClipboardLimits.parse({...scaleLimits(), change.key: change.value}, chunkBytes: 32768), isNull, reason: change.key);
      }
      expect(ClipboardLimits.parse({...scaleLimits()}..remove('maxOffers'), chunkBytes: 32768), isNull);
      // The older profiles are untouched.
      expect(ClipboardLimits.parse(pull.pullLimits(), chunkBytes: 32768), same(ClipboardLimits.pullProfile));
    });

    test('only a bridge that says so announces the revision; the relay may not impose it on another', () async {
      expect(session.dialHeaders, {
        clipboardLimitsHeader: clipboardLimitsHeaderValue,
        clipboardFormatsHeader: clipboardFormatsHeaderValue,
        clipboardPullHeader: clipboardPullHeaderValue,
        clipboardScaleHeader: clipboardScaleHeaderValue,
      });
      final plain = make(with_: pull.PullClipboard());
      expect(plain.dialHeaders.containsKey(clipboardScaleHeader), isFalse);
      addTearDown(plain.close);
      final other = pull.PullClipboard();
      final refused = make(with_: other);
      addTearDown(refused.close);
      final starting = refused.start();
      await base.settle();
      socket.emit(scaleReady());
      await expectLater(starting, throwsA(anything));
    });

    test('the bridge is told, and HTML is read up to the pair\'s limit', () async {
      await connect();
      expect(bridge.scaleOn, isTrue);
      expect(bridge.richBytesSet, 64 * mib);
      expect(session.limits.scale, isTrue);
      await session.close();
      socket = base.ClipboardSocket();
      bridge = ScaleClipboard();
      session = make();
      await connect(pull.pullReady());
      expect(bridge.scaleOn, isFalse);
      expect(bridge.richBytesSet, 8 * mib);
    });
  });

  group('what is no longer a limit', () {
    Future<void> sentOk(ClipboardCapture capture) async {
      bridge.offerCapture = capture;
      final sending = session.sendLocal();
      sending.ignore();
      final begin = await socket.nextMessage('clipboard.offer');
      expect(begin['format'], 'tree');
      socket.emit({'type': 'clipboard.error', 'requestId': begin['requestId'], 'reason': 'clipboard_cancelled'});
      await sending.catchError((Object _) {});
    }

    test('a file above 4 GiB and more than 128 top-level items are offered', () async {
      await connect();
      final items = <ClipboardItem>[
        pull.tree('big.bin', 5 * gib + 7),
        for (var i = 0; i < 300; i++) pull.tree('f$i.txt', 1),
      ];
      await sentOk(ClipboardCapture(format: 'tree', items: items, tops: 301, offer: true));
      expect(session.errorDetail, isNull);
    });

    test('the same copy is refused by a pair without the revision with the precise size and count reasons', () async {
      await connect(pull.pullReady());
      for (final (items, tops, detail) in [
        ([pull.tree('big.bin', 4 * gib + 1)], 1, 'file'),
        ([for (var i = 0; i < 129; i++) pull.folder('t$i')], 129, 'count'),
      ]) {
        bridge.offerCapture = ClipboardCapture(format: 'tree', items: items, tops: tops, offer: true);
        await expectLater(session.sendLocal(), throwsA(isA<ClipboardTransferException>()));
        expect(session.error, 'clipboard_limits_exceeded');
        expect(session.errorDetail, detail);
      }
    });

    test('the entry guard and the path rules name themselves', () async {
      await connect();
      Future<void> refuse(List<ClipboardItem> entries, int tops, String detail) async {
        bridge.offerCapture = ClipboardCapture(format: 'tree', items: entries, tops: tops, offer: true);
        await expectLater(session.sendLocal(), throwsA(isA<ClipboardTransferException>()));
        expect(session.error, 'clipboard_limits_exceeded');
        expect(session.errorDetail, detail);
      }

      await refuse([for (var i = 0; i < 200001; i++) pull.tree('f$i', 0)], 200001, 'entries');
      // 260 characters: Explorer cannot create the item, whatever the wire allows; the longest path is named.
      await refuse([pull.folder('a' * 100), pull.folder('${'a' * 100}/${'b' * 100}'), pull.tree('${'a' * 100}/${'b' * 100}/${'c' * 58}', 1)], 1, 'path:260');
      await refuse([pull.folder('a' * 255), pull.tree('${'a' * 255}/${'b' * 100}', 1)], 1, 'path:356');
      // 259 characters pass, and so does a 255-character name.
      bridge.offerCapture = ClipboardCapture(
        format: 'tree',
        items: [pull.folder('a' * 100), pull.folder('${'a' * 100}/${'b' * 100}'), pull.tree('${'a' * 100}/${'b' * 100}/${'c' * 57}', 1)],
        tops: 1,
        offer: true,
      );
      final sending = session.sendLocal();
      sending.ignore();
      final offer = await socket.nextMessage('clipboard.offer');
      expect(offer['entries'], 3);
      socket.emit({'type': 'clipboard.error', 'requestId': offer['requestId'], 'reason': 'clipboard_cancelled'});
      await sending.catchError((Object _) {});
    });

    test('text and HTML up to 64 MiB are announced, one byte more is refused as text', () async {
      await connect();
      bridge.captured = ClipboardCapture(format: 'text', items: [ClipboardItem(size: 64 * mib)]);
      final sending = session.sendLocal();
      sending.ignore();
      final begin = await socket.nextMessage('clipboard.begin');
      expect(begin['size'], 64 * mib);
      socket.emit({'type': 'clipboard.error', 'requestId': begin['requestId'], 'reason': 'clipboard_cancelled'});
      await sending.catchError((Object _) {});
      bridge.captured = ClipboardCapture(format: 'text', items: [ClipboardItem(size: 64 * mib + 1)]);
      await expectLater(session.sendLocal(), throwsA(isA<ClipboardTransferException>()));
      expect(session.errorDetail, 'text');
      expect(session.status, contains('64 MB'));
    });
  });

  group('the user is told which limit', () {
    test('messages', () {
      const s = ClipboardLimits.scaleRevision;
      expect(s.message('path:283'), contains('경로가 너무 긴 파일이 있어요'));
      expect(s.message('path:283'), contains('가장 긴 경로 283자'));
      expect(s.message('path:283'), contains('259자까지'));
      expect(s.message('path'), contains('경로가 너무 긴 파일이 있어요'));
      expect(s.message('path'), isNot(contains('가장 긴 경로')));
      expect(s.message('text'), contains('64 MB'));
      expect(s.message('entries'), contains('200,000개'));
      expect(s.message('depth'), contains('512단계'));
      // Never the sentence that lists every limit once the actual one is known; the scale revision has no such sentence.
      for (final detail in ['path:300', 'text', 'entries', 'depth', 'image', null]) {
        final text = s.message(detail);
        expect(text, isNot(contains('파일 하나에')), reason: '$detail');
        expect(text, isNot(contains('128개')), reason: '$detail');
      }
      expect(s.message(), '클립보드로 옮길 수 있는 한도를 넘었어요.');
      // The profile before it: 160 characters, and the all-limits sentence only when the reason is unknown.
      const v = ClipboardLimits.pullProfile;
      expect(v.message('path:200'), contains('160자까지'));
      expect(v.message('path:200'), contains('가장 긴 경로 200자'));
      expect(v.message(), contains('파일 하나에'));
      expect(v.message('file'), contains('4 GB'));
      expect(v.message('count'), contains('128개'));
      expect(v.message('text'), contains('1 MB'));
    });

    test('every limit this pair names crosses the wire as its own reason', () async {
      await connect();
      for (final (detail, reason) in [
        ('path:300', 'clipboard_path_too_long'),
        ('text', 'clipboard_text_too_large'),
        ('entries', 'clipboard_entry_count_limit'),
        ('depth', 'clipboard_depth_limit'),
        ('image', 'clipboard_image_too_large'),
      ]) {
        bridge.captured = const ClipboardCapture(format: 'text', items: [ClipboardItem(size: 3)]);
        bridge.sourceFailure = PlatformException(code: 'clipboard_limits_exceeded', message: detail);
        await expectLater(session.sendLocal(), throwsA(isA<ClipboardTransferException>()));
        final error = await socket.nextMessage('clipboard.error');
        expect(error['reason'], reason, reason: detail);
        expect(session.error, 'clipboard_limits_exceeded');
        expect(session.errorDetail, detail);
        await socket.nextMessage('clipboard.begin');
        socket.sent.clear();
      }
      // A size above the 2^53 bytes the pair can express has no precise reason: the general one.
      bridge.sourceFailure = PlatformException(code: 'clipboard_limits_exceeded', message: 'file');
      await expectLater(session.sendLocal(), throwsA(isA<ClipboardTransferException>()));
      expect((await socket.nextMessage('clipboard.error'))['reason'], 'clipboard_limits_exceeded');
    });

    test('a pair without the revision keeps the general reason on the wire', () async {
      await connect(pull.pullReady());
      bridge.captured = const ClipboardCapture(format: 'text', items: [ClipboardItem(size: 3)]);
      bridge.sourceFailure = PlatformException(code: 'clipboard_limits_exceeded', message: 'path:300');
      await expectLater(session.sendLocal(), throwsA(isA<ClipboardTransferException>()));
      expect((await socket.nextMessage('clipboard.error'))['reason'], 'clipboard_limits_exceeded');
    });

    test('a peer\'s specific reason is shown with its own text, in a pair of the revision and from the relay', () async {
      await connect();
      for (final (reason, text) in [
        ('clipboard_path_too_long', '경로가 너무 긴 파일이 있어요'),
        ('clipboard_text_too_large', '텍스트가 너무 길어요'),
        ('clipboard_entry_count_limit', '항목이 너무 많아요'),
        ('clipboard_depth_limit', '폴더가 너무 깊어요'),
        ('clipboard_image_too_large', '이미지가 너무 커요'),
      ]) {
        final receiving = session.receiveRemote();
        receiving.ignore();
        final read = await socket.nextMessage('clipboard.read');
        socket.emit({'type': 'clipboard.error', 'requestId': read['requestId'], 'reason': reason});
        await receiving.catchError((Object _) {});
        expect(session.error, 'clipboard_limits_exceeded', reason: reason);
        expect(session.status, contains(text), reason: reason);
        expect(session.status, isNot(contains('파일 하나에')), reason: reason);
      }
      // The relay itself ending the pair names the limit as well.
      socket.emit({'type': 'clipboard.error', 'reason': 'clipboard_path_too_long'});
      await base.settle();
      expect(session.status, contains('경로가 너무 긴 파일이 있어요'));
    });
  });

  group('a newer copy', () {
    test('cancels the older automatic transfer that is still crossing and is sent at once', () async {
      final f = echo.Fixture(echo.Content.text('a' * 100000));
      addTearDown(f.session.close);
      await f.connect();
      // Activation shares the existing copy: the peer never acknowledges its first chunk, so it stays under way.
      final begin = await f.socket.next('clipboard.begin');
      await f.socket.next('clipboard.chunk');
      expect(f.session.busy, isTrue);
      final older = begin['requestId'];
      f.bridge.userCopy(echo.Content.text('newer copy'));
      final cancel = await f.socket.next('clipboard.cancel');
      expect(cancel['requestId'], older);
      // A full-profile sender can already have its whole window queued. Never
      // acknowledge these retired chunks under the newer copy's request ID.
      expect(f.socket.ofType('clipboard.chunk').where((v) => v['requestId'] == older), isNotEmpty);
      // The relay answers a cancel to both peers.
      f.socket.emit({'type': 'clipboard.cancel', 'requestId': older});
      final next = await f.acceptOutgoing();
      expect(next['requestId'], isNot(older));
      expect(next['size'], 'newer copy'.length, reason: 'the newer copy, not the old one');
      expect(f.session.error == null || f.session.error == 'clipboard_cancelled', isTrue);
      expect(f.session.closed, isFalse);
      expect(f.session.ready, isTrue);
    });

    test('a transfer the user started is not cancelled by a newer copy', () async {
      final f = echo.Fixture(echo.Content.text('x'));
      addTearDown(f.session.close);
      await f.connect();
      await f.acceptOutgoing();
      await f.quiet();
      f.bridge.content = echo.Content.text('b' * 100000);
      final manual = f.session.sendLocal();
      manual.ignore();
      await f.socket.next('clipboard.begin');
      await f.socket.next('clipboard.chunk');
      f.bridge.userCopy(echo.Content.text('another'));
      await Future<void>.delayed(const Duration(milliseconds: 300));
      expect(f.socket.ofType('clipboard.cancel'), isEmpty);
      await f.session.cancelTransfer();
    });
  });
}

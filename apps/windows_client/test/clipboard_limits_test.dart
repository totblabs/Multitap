import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_limits.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/native_clipboard.dart';

import 'clipboard_session_test.dart' as base;

const gib = 1024 * 1024 * 1024, mib = 1024 * 1024;

/// What a newer relay sends a newer peer: the pair's effective limits plus
/// maxTextBytes and the profile marker.
Map<String, dynamic> extendedReady({bool bothExtended = true}) => {
  ...base.ready,
  'limits': bothExtended
      ? {
          'chunkBytes': 32768,
          'maxFiles': 128,
          'maxFileBytes': 4 * gib,
          'maxTotalBytes': 8 * gib,
          'maxTextUnits': mib,
          'maxTextBytes': mib,
          'limitsProfile': 2,
        }
      : {
          'chunkBytes': 32768,
          'maxFiles': 128,
          'maxFileBytes': 256 * mib,
          'maxTotalBytes': 512 * mib,
          'maxTextUnits': 32768,
          'maxTextBytes': 128 * 1024,
          'limitsProfile': 2,
        },
};

void main() {
  late base.ClipboardSocket socket;
  late base.MemoryClipboard bridge;
  late ClipboardSession session;

  Future<void> connect(Map<String, dynamic> readyMessage) async {
    final starting = session.start();
    await base.settle();
    socket.emit(readyMessage);
    await starting;
  }

  setUp(() {
    socket = base.ClipboardSocket();
    bridge = base.MemoryClipboard();
    session = ClipboardSession(
      policy: const ClipboardPolicy.all(),
      url: Uri.parse(
        'wss://cloud.example.com/api/v1/sessions/${base.sessionId}/clipboard',
      ),
      token: 'test-token',
      sessionId: base.sessionId,
      bridge: bridge,
      dialer: (url, token) async => socket,
    );
  });
  tearDown(() async => session.close());

  group('ClipboardLimits', () {
    test('only the two known profiles are accepted from the relay', () {
      expect(
        ClipboardLimits.parse(base.ready['limits'], chunkBytes: 32768),
        same(ClipboardLimits.v1),
      );
      expect(
        ClipboardLimits.parse(
          extendedReady()['limits'],
          chunkBytes: 32768,
        ),
        same(ClipboardLimits.v2),
      );
      expect(
        ClipboardLimits.parse(
          extendedReady(bothExtended: false)['limits'],
          chunkBytes: 32768,
        ),
        same(ClipboardLimits.v1Extended),
      );
      expect(ClipboardLimits.v1Extended.extended, isTrue);
      expect(ClipboardLimits.v1.extended, isFalse);
      for (final changed in <String, Object>{
        'maxFileBytes': 8 * gib,
        'maxTotalBytes': 16 * gib,
        'maxTextUnits': 2 * mib,
        'maxFiles': 256,
        'limitsProfile': 3,
        'chunkBytes': 65536,
      }.entries) {
        final limits = {
          ...(extendedReady()['limits'] as Map<String, dynamic>),
          changed.key: changed.value,
        };
        expect(
          ClipboardLimits.parse(limits, chunkBytes: 32768),
          isNull,
          reason: changed.key,
        );
      }
      expect(ClipboardLimits.parse(null, chunkBytes: 32768), isNull);
      expect(ClipboardLimits.parse({'chunkBytes': 32768}, chunkBytes: 32768), isNull);
    });

    test('Android has no 공유 폴더, so its advice never points there', () {
      for (final detail in [
        'count',
        'file',
        'total',
        'entries',
        'depth',
        'path:300',
        null,
      ]) {
        final text = ClipboardLimits.v2.message(detail, false);
        expect(text, isNot(contains('공유 폴더')), reason: '$detail');
        expect(ClipboardLimits.v2.message(detail), contains('공유 폴더'));
      }
      expect(ClipboardLimits.v2.message('count', false), contains('압축해서'));
      expect(ClipboardLimits.v2.message('file', false), contains('나눠'));
    });

    test('the messages name the actual limits of the pair', () {
      expect(
        ClipboardLimits.v2.message('file'),
        contains('파일 하나에 4 GB, 한 번에 8 GB'),
      );
      expect(ClipboardLimits.v2.message('file'), contains('공유 폴더'));
      expect(ClipboardLimits.v2.message('total'), contains('8 GB'));
      expect(ClipboardLimits.v2.message('count'), contains('128개'));
      expect(ClipboardLimits.v2.message('text'), contains('1 MB'));
      expect(
        ClipboardLimits.v1.message('file'),
        contains('파일 하나에 256 MB, 한 번에 512 MB'),
      );
      expect(ClipboardLimits.v1.message('text'), contains('128 KB'));
      final unknown = ClipboardLimits.v2.message();
      expect(unknown, contains('4 GB'));
      expect(unknown, contains('1 MB'));
      expect(unknown, contains('128개'));
    });
  });

  group('limits are checked before anything is sent', () {
    Future<void> sendExpectingLimits(
      ClipboardCapture capture,
      String detail,
    ) async {
      bridge.captured = capture;
      final sending = expectLater(
        session.sendLocal(),
        throwsA(base.code('clipboard_limits_exceeded')),
      );
      await sending;
      expect(
        socket.sent.where((value) => value['type'] == 'clipboard.begin'),
        isEmpty,
        reason: 'the sender must not start a transfer the pair refuses',
      );
      expect(session.error, 'clipboard_limits_exceeded');
      expect(session.errorDetail, detail);
      expect(session.failureQuiet, isFalse);
    }

    test('released peers: 256 MiB per file', () async {
      await connect(base.ready);
      await sendExpectingLimits(
        ClipboardCapture(
          format: 'files',
          items: [ClipboardItem(size: 256 * mib + 1, name: 'big.iso')],
        ),
        'file',
      );
      expect(session.status, contains('파일 하나에 256 MB, 한 번에 512 MB'));
    });

    test('released peers: 512 MiB per transfer', () async {
      await connect(base.ready);
      await sendExpectingLimits(
        ClipboardCapture(
          format: 'files',
          items: [
            for (var i = 0; i < 3; i++)
              ClipboardItem(size: 200 * mib, name: 'part$i.bin'),
          ],
        ),
        'total',
      );
    });

    test('released peers: text of more than 128 KiB or 32768 units', () async {
      await connect(base.ready);
      await sendExpectingLimits(
        const ClipboardCapture(
          format: 'text',
          items: [ClipboardItem(size: 128 * 1024 + 1)],
        ),
        'text',
      );
      expect(session.status, contains('128 KB'));
      await sendExpectingLimits(
        const ClipboardCapture(
          format: 'text',
          items: [ClipboardItem(size: 60000, units: 32769)],
        ),
        'text',
      );
    });

    test('more than 128 files is a count failure on any pair', () async {
      await connect(extendedReady());
      await sendExpectingLimits(
        ClipboardCapture(
          format: 'files',
          items: [
            for (var i = 0; i < 129; i++)
              ClipboardItem(size: 1, name: 'f$i.txt'),
          ],
        ),
        'count',
      );
      expect(session.status, contains('128개'));
    });

    test('extended pair: a 3 GiB file and 1 MiB of text are sent, 4 GiB + 1 is not', () async {
      await connect(extendedReady());
      expect(session.limits, same(ClipboardLimits.v2));
      bridge.captured = ClipboardCapture(
        format: 'files',
        items: [ClipboardItem(size: 3 * gib, name: 'disk.vhdx')],
      );
      bridge.source = [Uint8List(32768)];
      final sending = session.sendLocal();
      final begin = await socket.nextMessage('clipboard.begin');
      expect((begin['files'] as List).single['size'], 3 * gib);
      socket.emit({'type': 'clipboard.cancel', 'requestId': begin['requestId']});
      await expectLater(sending, throwsA(base.code('clipboard_cancelled')));
    });

    test('extended pair: the new maximums are refused one byte above', () async {
      await connect(extendedReady());
      await sendExpectingLimits(
        ClipboardCapture(
          format: 'files',
          items: [ClipboardItem(size: 4 * gib + 1, name: 'huge.bin')],
        ),
        'file',
      );
      expect(session.status, contains('파일 하나에 4 GB, 한 번에 8 GB'));
      await sendExpectingLimits(
        ClipboardCapture(
          format: 'files',
          items: [
            ClipboardItem(size: 4 * gib, name: 'a.bin'),
            ClipboardItem(size: 4 * gib, name: 'b.bin'),
            ClipboardItem(size: 1, name: 'c.bin'),
          ],
        ),
        'total',
      );
      await sendExpectingLimits(
        const ClipboardCapture(
          format: 'text',
          items: [ClipboardItem(size: mib + 1)],
        ),
        'text',
      );
    });

    test('extended pair with a released peer keeps the released numbers and says so up front', () async {
      await connect(extendedReady(bothExtended: false));
      expect(session.limits, same(ClipboardLimits.v1Extended));
      await sendExpectingLimits(
        ClipboardCapture(
          format: 'files',
          items: [ClipboardItem(size: 300 * mib, name: 'big.iso')],
        ),
        'file',
      );
      expect(session.status, contains('256 MB'));
    });
  });

  group('native failures keep their reason', () {
    Future<void> failCapture(String nativeCode, {String? message}) async {
      bridge.captureFailure = PlatformException(
        code: nativeCode,
        message: message,
      );
      await expectLater(session.sendLocal(), throwsA(base.code(nativeCode)));
    }

    test('size, count and length violations from the native side are limits failures with the limits in the text', () async {
      await connect(extendedReady());
      for (final detail in ['text', 'count', 'file', 'total']) {
        await failCapture('clipboard_limits_exceeded', message: detail);
        expect(session.errorDetail, detail);
        expect(session.status, isNot(contains('읽지 못했어요')));
      }
      expect(session.status, contains('4 GB'));
      await failCapture('clipboard_limits_exceeded', message: 'text');
      expect(session.status, contains('텍스트가 너무 길어요'));
      expect(session.status, contains('1 MB'));
    });

    test('a folder says folders cannot be moved yet', () async {
      await connect(base.ready);
      await failCapture('unsupported_clipboard_format');
      expect(session.status, contains('지금은 텍스트와 파일만 옮길 수 있어요'));
    });

    test('a file locked by another program has its own message', () async {
      await connect(base.ready);
      await failCapture('clipboard_file_locked');
      expect(session.status, contains('다른 프로그램이 파일을 사용 중'));
      expect(session.failureQuiet, isFalse);
    });

    test('a full disk has its own message', () async {
      await connect(base.ready);
      await failCapture('clipboard_disk_full');
      expect(session.status, contains('디스크 공간이 부족'));
    });

    test('an unknown native failure is a generic transfer failure', () async {
      await connect(base.ready);
      bridge.captureFailure = PlatformException(code: 'something_native');
      await expectLater(
        session.sendLocal(),
        throwsA(base.code('clipboard_transfer_failed')),
      );
    });
  });

  group('automatic sharing stays quiet for expected non-errors', () {
    Future<void> connectSharing() async {
      await connect(base.ready);
      await session.setSharingActive(true);
    }
    test('empty, unsupported and policy-denied automatic copies are recorded but not shown', () async {
      await connectSharing();
      for (final nativeCode in [
        'clipboard_empty',
        'unsupported_clipboard_format',
        'clipboard_policy_denied',
        'clipboard_changed',
      ]) {
        bridge.captureFailure = PlatformException(code: nativeCode);
        await expectLater(
          session.sendLocal(automatic: true),
          throwsA(base.code(nativeCode)),
        );
        expect(session.failureQuiet, isTrue, reason: nativeCode);
      }
    });

    test('the same failures are not quiet when the user started the transfer', () async {
      await connectSharing();
      bridge.captureFailure = PlatformException(
        code: 'unsupported_clipboard_format',
      );
      await expectLater(
        session.sendLocal(),
        throwsA(base.code('unsupported_clipboard_format')),
      );
      expect(session.failureQuiet, isFalse);
    });

    test('limits, locked files and a full disk are never quiet, even automatic', () async {
      await connectSharing();
      for (final nativeCode in [
        'clipboard_limits_exceeded',
        'clipboard_file_locked',
        'clipboard_disk_full',
        'clipboard_read_failed',
      ]) {
        bridge.captureFailure = PlatformException(code: nativeCode);
        await expectLater(
          session.sendLocal(automatic: true),
          throwsA(isA<ClipboardTransferException>()),
        );
        expect(session.failureQuiet, isFalse, reason: nativeCode);
      }
    });

    test('a paste after a quiet failure explains why once', () async {
      await connectSharing();
      bridge.captureFailure = PlatformException(
        code: 'unsupported_clipboard_format',
      );
      await expectLater(
        session.sendLocal(automatic: true),
        throwsA(base.code('unsupported_clipboard_format')),
      );
      expect(session.pasteNotice, isNull);
      session.pasteBlocked();
      expect(session.takePasteNotice(), contains('지금은 텍스트와 파일만 옮길 수 있어요'));
      expect(session.takePasteNotice(), isNull);
      // Nothing left to explain: the ordinary blocked-paste text.
      session.pasteBlocked();
      expect(session.pasteNotice, isNull);
      expect(session.status, contains('다시 복사'));
    });

    test('an empty clipboard is explained to no one', () async {
      await connectSharing();
      bridge.captureFailure = PlatformException(code: 'clipboard_empty');
      await expectLater(
        session.sendLocal(automatic: true),
        throwsA(base.code('clipboard_empty')),
      );
      session.pasteBlocked();
      expect(session.pasteNotice, isNull);
    });
  });

  group('failures reported to the relay', () {
    Future<void> receiveFailingAppend(String nativeCode) async {
      final receiving = session.receiveRemote();
      unawaited(receiving.catchError((Object _) {}));
      final request = await socket.nextMessage('clipboard.read');
      socket.emit({
        'type': 'clipboard.begin',
        'requestId': request['requestId'],
        'format': 'files',
        'files': [
          {'name': 'big.bin', 'size': 10},
        ],
      });
      await base.settle();
      bridge.appendFailure = PlatformException(code: nativeCode);
      socket.emit({
        'type': 'clipboard.chunk',
        'requestId': request['requestId'],
        'index': 0,
        'offset': 0,
        'data': 'AQID',
      });
      await base.settle();
    }

    test('a full disk reaches an extended relay as clipboard_disk_full', () async {
      await connect(extendedReady());
      await receiveFailingAppend('clipboard_disk_full');
      final error = await socket.nextMessage('clipboard.error');
      expect(error['reason'], 'clipboard_disk_full');
      expect(session.error, 'clipboard_disk_full');
    });

    test('a released relay would reject that reason, so it gets the closest released one', () async {
      await connect(base.ready);
      expect(session.limits.extended, isFalse);
      await receiveFailingAppend('clipboard_disk_full');
      final error = await socket.nextMessage('clipboard.error');
      expect(error['reason'], 'clipboard_write_failed');
      // The user still sees the real reason.
      expect(session.error, 'clipboard_disk_full');
      expect(session.status, contains('디스크 공간이 부족'));
    });

    test('an incoming manifest above the pair limits is refused with the limits reason', () async {
      await connect(extendedReady(bothExtended: false));
      final receiving = session.receiveRemote();
      unawaited(receiving.catchError((Object _) {}));
      final request = await socket.nextMessage('clipboard.read');
      socket.emit({
        'type': 'clipboard.begin',
        'requestId': request['requestId'],
        'format': 'files',
        'files': [
          {'name': 'big.bin', 'size': 300 * mib},
        ],
      });
      final error = await socket.nextMessage('clipboard.error');
      expect(error['reason'], 'clipboard_limits_exceeded');
      expect(session.status, contains('256 MB'));
      expect(bridge.events, isNot(contains('begin')));
    });

    test('a peer reporting the limits reason shows the limits of this pair', () async {
      await connect(extendedReady());
      final receiving = session.receiveRemote();
      unawaited(receiving.catchError((Object _) {}));
      final request = await socket.nextMessage('clipboard.read');
      socket.emit({
        'type': 'clipboard.error',
        'requestId': request['requestId'],
        'reason': 'clipboard_limits_exceeded',
      });
      await base.settle();
      expect(session.error, 'clipboard_limits_exceeded');
      expect(session.status, contains('4 GB'));
      expect(session.status, contains('공유 폴더'));
    });
  });

  test('the channel announces the extended limits when it connects', () {
    expect(clipboardLimitsHeader, 'X-Multitap-Clipboard-Limits');
    expect(clipboardLimitsHeaderValue, '2');
  });
}

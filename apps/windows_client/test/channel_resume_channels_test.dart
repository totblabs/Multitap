// channel-resume-v1 in the side channels themselves: what a channel that
// replaces an ended one takes over (the shared folders, under the same ids and
// names), how a refused dial is told from a lost network, when an ended USB
// channel has let its devices go, and what the notices say while a channel is
// coming back by itself. Fake sockets, files and USB backend.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/drive_files.dart';
import 'package:multitap_client/data/drive_session.dart';
import 'package:multitap_client/data/print_session.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/data/usb_redirection.dart';
import 'package:multitap_client/ui/drive_panel.dart';

import 'drive_session_test.dart'
    show DriveSocket, FakeFiles, driveUrl, flush, ready, sessionId;
import 'usb_redirection_test.dart' show FakeBackend;

void main() {
  late FakeFiles files;
  late ProductLog log;

  DriveSession drive(Future<WebSocket> Function() dial) => DriveSession(
    url: driveUrl,
    token: 'not-persisted',
    sessionId: sessionId,
    files: files,
    dialer: (_, _) => dial(),
    log: log,
  );

  setUp(() {
    files = FakeFiles();
    log = ProductLog();
  });

  Future<void> share(DriveSession session, String path, String name) async {
    files.pick = DrivePick(path, name);
    expect(await session.addFolder(), isNull);
    files.pick = null;
  }

  test('the channel that replaces an ended one shares the same folders again, '
      'under the same ids and names, read-only where they were', () async {
    final first = DriveSocket();
    final before = drive(() async => first);
    await before.start();
    first.emit(ready);
    await flush();
    await share(before, r'C:\Data', 'Data');
    await share(before, r'C:\Photos', 'Photos');
    await before.setReadOnly(before.shares.last.id, true);
    final offered = first.lastOf('drive.shares')['shares'] as List;
    expect(offered, hasLength(2));
    first.emit({
      'type': 'drive.mounted',
      'shares': [
        {'id': before.shares[0].id, 'state': 'mapped', 'letter': 'Z'},
        {'id': before.shares[1].id, 'state': 'mapped', 'letter': 'Y'},
      ],
    });
    await flush();
    // The pair ends because the stream is away.
    first.emit({'type': 'drive.error', 'reason': 'drive_stream_away'});
    await flush();
    expect(before.closed, isTrue);
    expect(before.error, 'drive_stream_away');
    expect(before.shares, isEmpty, reason: 'An ended channel shares nothing');
    expect(before.sharesAtEnd, hasLength(2));
    expect(files.opened, isEmpty, reason: 'Its folders were let go');

    final second = DriveSocket();
    final after = drive(() async => second)..inherit(before);
    await after.start();
    expect(second.sent, isEmpty, reason: 'Nothing before the pair is ready');
    second.emit(ready);
    await flush();
    // Opened again on this PC, then offered exactly as before.
    expect(files.opened.keys.toSet(), {
      for (final item in offered) (item as Map)['id'],
    });
    expect(files.opened.values.map((item) => item.path).toSet(), {
      r'C:\Data',
      r'C:\Photos',
    });
    expect(second.lastOf('drive.shares')['shares'], offered);
    expect(after.shares.map((item) => item.state).toSet(), {
      DriveShareState.pending,
    });
    expect(after.shares.every((item) => item.letter == null), isTrue);
    // The desktop maps them again: the same letters when they are still free.
    second.emit({
      'type': 'drive.mounted',
      'shares': [
        {'id': after.shares[0].id, 'state': 'mapped', 'letter': 'Z'},
        {'id': after.shares[1].id, 'state': 'mapped', 'letter': 'Y'},
      ],
    });
    await flush();
    expect(after.shares.map((item) => item.letter).toList(), ['Z', 'Y']);
    expect(after.shares.last.readOnly, isTrue);
    await after.close();
  });

  test('a folder that is gone when the channel comes back is listed as '
      'unavailable and not offered; a folder the person stopped sharing is '
      'not brought back', () async {
    final first = DriveSocket();
    final before = drive(() async => first);
    await before.start();
    first.emit(ready);
    await flush();
    await share(before, r'C:\Data', 'Data');
    await share(before, r'C:\Stick', 'Stick');
    await share(before, r'C:\Old', 'Old');
    // The person ends one share before the outage.
    await before.remove(before.shares.last.id);
    final data = before.shares.first.id;
    await first.incoming.close();
    await flush();
    expect(before.error, 'drive_transport_closed');
    expect(before.sharesAtEnd.map((item) => item.path), [
      r'C:\Data',
      r'C:\Stick',
    ]);
    // The USB stick was pulled during the outage.
    files.failOpen[r'C:\Stick'] = 'drive_not_found';
    final second = DriveSocket();
    final after = drive(() async => second)..inherit(before);
    await after.start();
    second.emit(ready);
    await flush();
    expect(
      (second.lastOf('drive.shares')['shares'] as List).map(
        (item) => (item as Map)['id'],
      ),
      [data],
    );
    expect(after.shares, hasLength(2));
    expect(after.shares.last.state, DriveShareState.unavailable);
    expect(after.shares.last.error, 'drive_not_found');
    await after.close();
    // A channel without a predecessor starts empty, as before.
    final third = DriveSocket();
    final plain = drive(() async => third);
    await plain.start();
    third.emit(ready);
    await flush();
    expect(third.lastOf('drive.shares')['shares'], isEmpty);
    await plain.close();
  });

  test('a refused dial is told from "not now" and a lost network', () async {
    Future<DriveSession> dialled(Object failure) async {
      final session = drive(() async => throw failure);
      await session.start();
      expect(session.closed, isTrue);
      expect(session.error, 'drive_transport_closed');
      return session;
    }

    expect(
      (await dialled(const WebSocketException('refused', 409))).dialRefused,
      isTrue,
    );
    expect(
      (await dialled(const WebSocketException('refused', 401))).dialRefused,
      isTrue,
    );
    expect(
      (await dialled(const WebSocketException('away', 503))).dialRefused,
      isFalse,
    );
    expect(
      (await dialled(const SocketException('offline'))).dialRefused,
      isFalse,
    );

    Future<PrintSession> printed(Object failure) async {
      final session = PrintSession(
        url: Uri.parse('wss://fixture.invalid/api/v1/sessions/qa/print'),
        token: 'fixture-only',
        sessionId: 'qa',
        dialer: (_, _) async => throw failure,
      );
      await session.start();
      expect(session.closed, isTrue);
      expect(session.error, 'print_transport_closed');
      return session;
    }

    expect(
      (await printed(const WebSocketException('refused', 409))).dialRefused,
      isTrue,
    );
    expect(
      (await printed(const WebSocketException('away', 503))).dialRefused,
      isFalse,
    );
    expect(
      (await printed(const SocketException('offline'))).dialRefused,
      isFalse,
    );

    Future<UsbSession> plugged(Object failure) async {
      final session = UsbSession(
        url: Uri.parse('wss://fixture.invalid/api/v1/sessions/qa/usb'),
        token: 'fixture-only',
        sessionId: 'qa',
        backend: FakeBackend(),
        dialer: (_, _) async => throw failure,
      );
      await session.start();
      expect(session.closed, isTrue);
      expect(session.error, 'usb_transport_closed');
      // The ended channel has let everything go: a new one may start.
      await session.settled.timeout(const Duration(seconds: 2));
      return session;
    }

    expect(
      (await plugged(const WebSocketException('refused', 409))).dialRefused,
      isTrue,
    );
    expect(
      (await plugged(const WebSocketException('away', 503))).dialRefused,
      isFalse,
    );
  });

  test('no "connect again" while a channel is coming back by itself; the '
      'true notice after it gave up', () async {
    final socket = DriveSocket();
    final session = drive(() async => socket);
    await session.start();
    socket.emit(ready);
    await flush();
    await socket.incoming.close();
    await flush();
    var notified = 0;
    session.addListener(() => notified++);
    session.recovering = true;
    expect(notified, 1, reason: 'The views hear that it is coming back');
    expect(driveEndedMessage(session), isNull);
    session.recovering = true;
    expect(notified, 1);
    session.recovering = false;
    expect(notified, 2, reason: 'and that it gave up');
    expect(
      driveEndedMessage(session),
      '공유 폴더 연결이 끊겼어요. 데스크톱에 다시 연결한 뒤 폴더를 다시 고르세요.',
    );

    final usb = UsbSession(
      url: Uri.parse('wss://fixture.invalid/api/v1/sessions/qa/usb'),
      token: 'fixture-only',
      sessionId: 'qa',
      backend: FakeBackend(),
      dialer: (_, _) async => throw const SocketException('offline'),
    );
    await usb.start();
    usb.recovering = true;
    expect(usbEndedMessage(usb), isNull);
    usb.recovering = false;
    expect(usbEndedMessage(usb), 'USB 연결이 끊겼어요');

    // A clipboard pair that ended because the stream was away reads like any
    // lost channel once it really stays down.
    expect(
      clipboardStopNotice('clipboard_stream_away', ''),
      '클립보드 공유가 멈췄어요. 데스크톱에 다시 연결하면 쓸 수 있어요.',
    );
    expect(printLostNotice(1), '인쇄하던 문서 1개를 받지 못했어요. 데스크톱에서 다시 인쇄해 주세요.');
  });
}

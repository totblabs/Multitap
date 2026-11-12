// Delayed native capture is fake. No window, display, input or clipboard API
// is invoked; this verifies the last await before an authorized resize send.
import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart' show SessionSocket, flush, packet;
import 'viewport_session_test.dart' show ResizeMedia, connection, startInfo;

class _DelayedMedia extends ResizeMedia {
  Completer<bool>? release;
  @override
  Future<bool> capture(bool enabled) {
    if (!enabled && release != null) return release!.future;
    return super.capture(enabled);
  }
}

void main() {
  for (final command in ['viewport', 'shortcut']) {
    for (final late in [false, true]) {
      test(
        'file-copy block ${late ? 'during capture release' : 'before send'} prevents a $command request',
        () async {
          final socket = SessionSocket(), media = _DelayedMedia();
          final session = RemoteSession(
            connection: connection,
            token: 'fixture-only',
            media: media,
            dialer: (_, _) async => socket,
          );
          try {
            unawaited(session.start());
            socket.incoming.add(jsonEncode(startInfo));
            await flush();
            socket.incoming.add(packet());
            await flush();
            expect(session.frameReady, isTrue);
            Future<bool> request() => command == 'viewport'
                ? session.resizeViewport(1346, 812)
                : session.shortcut('windows');
            Future<bool> resize;
            if (late) {
              media.release = Completer<bool>();
              resize = request();
              await flush();
              session.setClipboardTransferBlocked(true);
              media.release!.complete(false);
            } else {
              session.setClipboardTransferBlocked(true);
              resize = request();
            }
            expect(await resize, isFalse);
            expect(session.resizing, isFalse);
            expect(
              socket.sent.where(
                (event) =>
                    event['type'] == 'stream.resize' ||
                    (event['type'] == 'input' && event['event'] == 'key'),
              ),
              isEmpty,
            );
            session.setClipboardTransferBlocked(false);
          } finally {
            await session.close();
            unawaited(media.events.close());
          }
        },
      );
    }
  }
}

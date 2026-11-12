// Regression: real production RemoteSession public socket/resize/
// compose paths; no native OS cursor, Host, network, or input call.
import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/foundation.dart' show debugPrintSynchronously;
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_cursor.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_cursor_test.dart'
    show cursorPacket, shapeRecord, stateRecord;
import 'remote_session_test.dart'
    show SessionMedia, SessionSocket, flush;
import 'viewport_session_test.dart'
    show startInfo;

// Mirrors the current native CursorPresenter::Reset/Apply epoch/cache and
// successful-void S_FALSE contract. This adapter does not test a real HWND,
// cursor factory, capture probe, OS hit-test, or physical presentation.
class NativeSemanticsMedia extends SessionMedia implements RemoteCursorMedia {
  final attempts = <Map<String, Object?>>[];
  final shapes = <int>{};
  int? nativeEpoch;
  int lastShapeId = 0, activeId = 0, texture = 0, decoded = 0;
  bool stateSeen = false, visible = false;

  @override
  Future<int> start(int width, int height) async {
    texture = 40 + ++starts;
    nativeEpoch = null;
    shapes.clear();
    lastShapeId = activeId = decoded = 0;
    stateSeen = visible = false;
    return texture;
  }

  @override
  Future<void> push(Uint8List packet) async {
    await super.push(packet);
    if (packet[0] == 1) decoded++;
  }

  @override
  Future<Map<String, dynamic>> stats() async => {
    ...await super.stats(),
    'decodedFrames': decoded,
  };

  @override
  Future<void> cursorView(CursorView view) async {}

  @override
  Future<void> cursor(int epoch, Uint8List record) async {
    final attempt = <String, Object?>{
      'texture': texture,
      'epoch': epoch,
      'type': record[0],
      'id': ByteData.sublistView(record).getUint32(4, Endian.little),
    };
    attempts.add(attempt);
    if (!validCursorRecord(record) || epoch < 0 || epoch > 65535) {
      attempt['result'] = 'rejected';
      throw PlatformException(code: 'invalid_cursor');
    }
    if (nativeEpoch != null && epoch < nativeEpoch!) {
      attempt['result'] = 'ignored_stale_success_void';
      return;
    }
    final id = attempt['id'] as int;
    if (record[0] == cursorShapeRecord) {
      if (id <= lastShapeId) {
        attempt['result'] = 'rejected';
        throw PlatformException(code: 'invalid_cursor');
      }
      nativeEpoch = epoch;
      lastShapeId = id;
      shapes.add(id);
      attempt['result'] = 'applied';
      return;
    }
    final nextVisible = record[1] & 1 != 0;
    if (nextVisible ? id < 1 || id > lastShapeId : id != 0) {
      attempt['result'] = 'rejected';
      throw PlatformException(code: 'invalid_cursor');
    }
    nativeEpoch = epoch;
    visible = nextVisible;
    activeId = id;
    stateSeen = true;
    if (visible && !shapes.contains(id)) {
      attempt['result'] = 'rejected_missing_shape';
      throw PlatformException(code: 'invalid_cursor');
    }
    attempt['result'] = 'applied';
  }
}

Uint8List video({required int epoch, bool key = false}) =>
    Uint8List.fromList([
      1, key ? 1 : 0, epoch & 255, epoch >> 8,
      ...List<int>.filled(8, 0),
      0, 0, 0, 1, key ? 0x65 : 0x41,
    ]);

Future<void> runCase({required bool oldVisible, required bool resize}) async {
  final socket = SessionSocket(), media = NativeSemanticsMedia();
  final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: 'held-epoch-fixture',
      webSocketUrl: Uri.parse('wss://fixture.invalid/held-epoch-fixture'),
      cursor: true,
      cursorCompose: true,
      relativeMouse: true,
      epochMedia: true,
      viewportProvisioning: true,
    ),
    token: 'fixture',
    media: media,
    dialer: (_, _) async => socket,
  );
  Future<bool>? resizing;
  final defects = <String>[];
  try {
    unawaited(session.start());
    await flush();
    socket.incoming.add(jsonEncode(startInfo));
    await flush();
    socket.incoming.add(video(epoch: 0, key: true));
    socket.incoming.add(cursorPacket(shapeRecord(1)));
    socket.incoming.add(cursorPacket(stateRecord(true, 1)));
    await flush();
    expect(session.localCursorFallback, isFalse);
    expect(media.nativeEpoch, 0);

    expect(session.composeCursor(true), isTrue);
    socket.incoming.add(cursorPacket(stateRecord(false, 0)));
    await flush();
    expect(session.remoteCursorHidden, isTrue);
    expect(session.composeCursor(false), isTrue);
    if (oldVisible) socket.incoming.add(cursorPacket(shapeRecord(2)));
    socket.incoming.add(cursorPacket(stateRecord(oldVisible, oldVisible ? 2 : 0)));
    await flush();
    expect(session.cursorSettling, isTrue);
    final heldAt = media.attempts.length;

    if (resize) {
      resizing = session.resizeViewport(1346, 812);
      await flush();
      final requestId = socket.sent.lastWhere(
        (event) => event['type'] == 'stream.resize',
      )['requestId'] as String;
      socket.incoming.add(jsonEncode({
        'type': 'stream.resizing',
        'requestId': requestId,
        'resizeId': 'held-epoch-resize',
        'epoch': 1,
        'width': 1346,
        'height': 812,
        'fps': 60,
        'monitor': '0',
        'timeoutMs': 90000,
      }));
      await flush();
      socket.incoming.add(jsonEncode({
        'type': 'stream.info',
        'codec': 'h264',
        'sessionId': 'held-epoch-fixture',
        'requestId': requestId,
        'resizeId': 'held-epoch-resize',
        'epoch': 1,
        'width': 1346,
        'height': 812,
        'sourceWidth': 1346,
        'sourceHeight': 812,
        'fpsTarget': 60,
      }));
      await flush();
      expect(session.textureId, 42);
      expect(media.nativeEpoch, isNull, reason: 'native Reset at actual start');
      expect(session.localCursorFallback, isTrue);
      expect(session.resizing, isTrue);
    }

    // Actual public compose-off timer, no calls to private product methods.
    await Future<void>.delayed(const Duration(milliseconds: 320));
    final replayed = media.attempts.skip(heldAt).toList();
    final beforeCurrentIdr = <String, Object?>{
      'resize': resize,
      'oldVisible': oldVisible,
      'texture': session.textureId,
      'resizing': session.resizing,
      'settling': session.cursorSettling,
      'localFallback': session.localCursorFallback,
      'remoteHidden': session.remoteCursorHidden,
      'nativeEpoch': media.nativeEpoch,
      'nativeVisible': media.visible,
      'nativeId': media.activeId,
      'attemptsAfterHold': replayed,
    };
    if (resize) {
      if (replayed.isNotEmpty) defects.add('retired held epoch reached new native texture');
      if (!session.localCursorFallback) defects.add('retired held STATE established new texture readiness before current IDR');
      if (media.nativeEpoch != null) defects.add('new native texture adopted retired capture epoch');
    } else {
      expect(replayed.length, oldVisible ? 2 : 1);
      expect(session.localCursorFallback, isFalse);
      expect(media.nativeEpoch, 0);
      expect(media.visible, oldVisible);
    }

    final currentEpoch = resize ? 1 : 0;
    socket.incoming.add(video(epoch: currentEpoch, key: true));
    socket.incoming.add(cursorPacket(shapeRecord(3), epoch: currentEpoch));
    socket.incoming.add(cursorPacket(stateRecord(true, 3), epoch: currentEpoch));
    await flush();
    // Production short readiness timer polls the new decoder after50ms.
    // This checks that the latest IDR completes the real public resize path.
    await Future<void>.delayed(const Duration(milliseconds: 80));
    expect(session.frameReady, isTrue);
    expect(session.resizing, isFalse);
    if (resizing != null) {
      expect(await resizing.timeout(const Duration(seconds: 1)), isTrue);
    }
    expect(session.localCursorFallback, isFalse);
    expect(session.remoteCursorHidden, isFalse);
    expect(media.nativeEpoch, currentEpoch);
    expect(media.activeId, 3);
    final currentAttempts = media.attempts.length;
    if (resize) {
      // Retired packets on the public wire are filtered before native.
      socket.incoming.add(cursorPacket(stateRecord(false, 0), epoch: 0));
      await flush();
      expect(media.attempts.length, currentAttempts);
      expect(session.localCursorFallback, isFalse);
      expect(session.remoteCursorHidden, isFalse);
    }
    debugPrintSynchronously('CURSOR_EPOCH_OBSERVATION ${jsonEncode({
      'beforeCurrentIdr': beforeCurrentIdr,
      'afterCurrentIdr': {
        'texture': session.textureId,
        'localFallback': session.localCursorFallback,
        'nativeEpoch': media.nativeEpoch,
        'nativeId': media.activeId,
        'frameReady': session.frameReady,
        'resizeComplete': !session.resizing,
        'currentCursorRecovered': true,
        'staleWireFiltered': resize,
      },
      'defects': defects,
    })}');
    expect(defects, isEmpty,
      reason: 'Only a current-epoch cursor STATE may establish current texture readiness.');
  } finally {
    await session.close();
    await media.events.close();
    if (resizing != null) await resizing;
  }
}

void main() {
  for (final oldVisible in [false, true]) {
    test('same epoch compose-off preserves ${oldVisible ? 'visible' : 'hidden'} held cursor',
      () => runCase(oldVisible: oldVisible, resize: false));
    test('resize retires ${oldVisible ? 'SHAPE+visible STATE' : 'hidden STATE'} held by compose-off',
      () => runCase(oldVisible: oldVisible, resize: true));
  }
}

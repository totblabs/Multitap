// decoder-prestart-v1: the session asks its media engine to start the decoder
// while it still waits for a stream info: at the start, before the relay dial
// returns, and at a viewport request. The hint failing changes nothing.
// NativeRemoteMedia sends it to the Windows runner only. Socket and decoder
// are fixtures: no native decoder or timing is proved here (see
// test/native/decoder_prestart_smoke.cpp).
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart' show SessionSocket, flush, packet;
import 'viewport_hold_test.dart' show HoldMedia, requestId;
import 'viewport_session_test.dart' show connection, startInfo;

class PrestartMedia extends HoldMedia implements DecoderPrestartMedia {
  PrestartMedia({this.refuse = false});
  final bool refuse;
  final prepared = <List<int>>[];
  @override
  Future<void> prepareDecoder(
    int width,
    int height,
    int fps,
    int outputs,
  ) async {
    prepared.add([width, height, fps, outputs]);
    if (refuse) throw PlatformException(code: 'invalid_arguments');
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  for (final refuse in [false, true]) {
    test('The decoder is prepared before the dial returns and before a '
        'viewport request (refused hint: $refuse)', () async {
      final socket = SessionSocket(), media = PrestartMedia(refuse: refuse);
      List<List<int>>? atDial;
      final session = RemoteSession(
        connection: connection,
        token: 'fixture-only',
        media: media,
        dialer: (_, _) async {
          atDial = [...media.prepared];
          return socket;
        },
      );
      final running = session.start();
      await flush();
      // No viewport or track plan known: the default guess at the session's rate.
      expect(atDial, [
        [1920, 1080, 60, 1],
      ]);
      socket.incoming.add(jsonEncode(startInfo));
      await flush();
      socket.incoming.add(packet());
      await flush();
      expect(session.textureId, 41, reason: 'the stream starts either way');
      final result = session.resizeViewport(1346, 812);
      await flush();
      requestId(socket);
      expect(media.prepared.last, [1346, 812, 60, 1]);
      expect(media.prepared, hasLength(2));
      await session.close();
      await running;
      expect(await result, isFalse);
      await media.events.close();
    });
  }

  test('NativeRemoteMedia sends prepareDecoder on Windows only', () async {
    const channel = MethodChannel('multitap/remote');
    final calls = <MethodCall>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          calls.add(call);
          return null;
        });
    addTearDown(
      () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null),
    );
    final media = NativeRemoteMedia();
    debugDefaultTargetPlatformOverride = TargetPlatform.android;
    await media.prepareDecoder(2560, 1440, 60, 2);
    expect(calls, isEmpty);
    debugDefaultTargetPlatformOverride = TargetPlatform.windows;
    await media.prepareDecoder(2560, 1440, 60, 2);
    debugDefaultTargetPlatformOverride = null;
    expect(calls.single.method, 'prepareDecoder');
    expect(calls.single.arguments, {
      'width': 2560,
      'height': 1440,
      'fps': 60,
      'outputs': 2,
    });
  });
}

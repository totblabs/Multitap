// A blurry picture after a resolution change: with a 1920x1080 encoder box a
// 2960x760 window streams as 1920x492 (STATS: scaled video 1920x492 from
// src 2960x760) and the client upscales the picture. The box is the codec
// ceiling (the Host keeps a pixel budget, stream_encode_size.hpp), and a
// resize never stretches the old picture while the new one is prepared.
// Fixtures only: socket, decoder and native display are fakes.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_remote_test.dart' show machine;
import 'initial_viewport_test.dart' as initial;
import 'remote_session_test.dart' show SessionSocket, flush, packet;
import 'viewport_encoder_limits_test.dart' as limits;
import 'viewport_session_test.dart' show ResizeMedia, startInfo;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('The encoder box is the codec ceiling by default and after 모든 모니터', () {
    for (final options in [
      const StreamOptions(),
      StreamOptions.recommended,
    ]) {
      expect(options.maxWidth, StreamOptions.codecMaxWidth);
      expect(options.maxHeight, StreamOptions.codecMaxHeight);
      expect(options.toJson()['maxWidth'], 3840);
      expect(options.toJson()['maxHeight'], 2160);
    }
    // 모든 모니터's box is one output's size; a single output gets the
    // ceiling back and keeps the other choices.
    const layout = StreamOptions(
      maxWidth: 1920,
      maxHeight: 1080,
      fps: 30,
      bitrate: 8000000,
      audio: false,
      monitor: 'all',
    );
    expect(layout.forSingleDisplay().withCodecCeiling().toJson(), {
      'maxWidth': 3840,
      'maxHeight': 2160,
      'fps': 30,
      'bitrate': 8000000,
      'audio': false,
      'monitor': '0',
    });
    const ceiling = StreamOptions();
    expect(identical(ceiling.withCodecCeiling(), ceiling), isTrue);
  });

  test('A 2960x760 viewport is decoded at its own size, not 1920x492', () async {
    final socket = SessionSocket(), media = ResizeMedia();
    final session = RemoteSession(
      connection: limits.connection(options: const StreamOptions()),
      token: 'fixture',
      media: media,
      dialer: (_, _) async => socket,
    );
    final running = session.start();
    // The initial 2560x1440 viewport, encoded at its own size.
    socket.incoming.add(
      jsonEncode({...initial.info(), 'width': 2560, 'height': 1440}),
    );
    await flush();
    socket.incoming.add(packet()..[2] = 1);
    await flush();
    expect(media.dimensions.last, [2560, 1440]);
    final completion = session.resizeViewport(2960, 760);
    await flush();
    final request = socket.sent.lastWhere((v) => v['type'] == 'stream.resize');
    final correlation = {
      'sessionId': initial.sessionId,
      'requestId': request['requestId'],
      'resizeId': 'resize-2',
      'epoch': 2,
    };
    socket.incoming.add(
      jsonEncode({
        'type': 'stream.resizing',
        ...correlation,
        'width': 2960,
        'height': 760,
        'monitor': '0',
        'fps': 60,
        'timeoutMs': 90000,
      }),
    );
    await flush();
    socket.incoming.add(
      jsonEncode({
        ...initial.info(),
        ...correlation,
        'width': 2960,
        'height': 760,
        'sourceWidth': 2960,
        'sourceHeight': 760,
      }),
    );
    await flush();
    socket.incoming.add(packet()..[2] = 2);
    expect(await completion.timeout(const Duration(seconds: 2)), isTrue);
    expect(media.dimensions.last, [2960, 760]);
    expect([session.width, session.height], [2960, 760]);
    expect([session.sourceWidth, session.sourceHeight], [2960, 760]);
    expect(session.closed, isFalse);
    await session.close();
    await running;
    await media.events.close();
  });

  testWidgets(
    // 2026-11-02 (tuning backlog #1, "the picture pauses or blacks out while
    // the window is resized or maximised"): while the size is followed, the
    // old picture fills the window, scaled, instead of sitting 1:1 in black
    // (it was never stretched since 2026-10-26); the new size is 1:1 again.
    'While the remote size changes the old picture fills the window until the new one is decoded',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1600, 1000);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      const window = MethodChannel('multitap/window');
      const events = MethodChannel('multitap/window/events');
      messenger.setMockMethodCallHandler(window, (_) async => false);
      messenger.setMockMethodCallHandler(events, (_) async => null);
      addTearDown(() {
        messenger.setMockMethodCallHandler(window, null);
        messenger.setMockMethodCallHandler(events, null);
      });
      final socket = SessionSocket(), media = ResizeMedia();
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'encode-size-fixture',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/api/v1/sessions/encode-size-fixture',
          ),
          epochMedia: true,
          viewportProvisioning: true,
        ),
        token: 'fixture-only',
        media: media,
        dialer: (_, _) async => socket,
      );
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(1),
            initialOptions: const StreamOptions(),
            initialDisplayMode: SessionDisplayMode.windowed,
            load: () async => session,
          ),
        ),
      );
      await tester.pump();
      await tester.pump();
      final picture = find.byKey(const Key('remote-image-bounds'));
      try {
        // A 1280x720 picture in a 1600x1000 window while the window's size
        // is about to be asked for: scaled to the window, aspect kept.
        socket.incoming.add(jsonEncode(startInfo));
        await tester.pump();
        await tester.pump();
        socket.incoming.add(packet());
        await tester.pump();
        await tester.pump();
        expect(tester.getSize(picture), const Size(1600, 900));
        // The window's own size is requested after the settle; until the
        // new picture is decoded the old one keeps filling the window.
        List<Map<String, dynamic>> requests() =>
            socket.sent.where((e) => e['type'] == 'stream.resize').toList();
        for (var n = 0; n < 20 && requests().isEmpty; n++) {
          await tester.pump(const Duration(milliseconds: 100));
          if (picture.evaluate().isNotEmpty) {
            expect(tester.getSize(picture), const Size(1600, 900));
          }
        }
        final request = requests().single;
        expect([request['width'], request['height']], [1600, 1000]);
        socket.incoming.add(
          jsonEncode({
            ...request,
            'type': 'stream.resizing',
            'resizeId': 'encode-size-epoch-1',
            'epoch': 1,
            'timeoutMs': 90000,
          }),
        );
        await tester.pump();
        await tester.pump();
        for (var n = 0; n < 5; n++) {
          await tester.pump(const Duration(milliseconds: 100));
          if (picture.evaluate().isNotEmpty) {
            expect(tester.getSize(picture), const Size(1600, 900));
          }
        }
        socket.incoming.add(
          jsonEncode({
            ...startInfo,
            'sessionId': 'encode-size-fixture',
            'requestId': request['requestId'],
            'resizeId': 'encode-size-epoch-1',
            'epoch': 1,
            'width': 1600,
            'height': 1000,
            'sourceWidth': 1600,
            'sourceHeight': 1000,
          }),
        );
        await tester.pump();
        await tester.pump();
        final frame = packet();
        frame[2] = 1;
        socket.incoming.add(frame);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 60));
        expect(session.resizing, isFalse);
        // The new picture is the window's own size, 1:1.
        expect(tester.getSize(picture), const Size(1600, 1000));
        expect(tester.takeException(), isNull);
      } finally {
        await tester.pumpWidget(const SizedBox.shrink());
        unawaited(session.close());
        await tester.pump(const Duration(seconds: 3));
        unawaited(media.events.close());
      }
    },
  );
}

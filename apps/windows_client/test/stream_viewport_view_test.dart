// Widget/session protocol regression only. The broker socket, H.264 decoder
// and native display are fixtures; this is not real virtual-display acceptance.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionSocket, packet;
import 'viewport_hold_test.dart' show HoldMedia;
import 'viewport_session_test.dart' show ResizeMedia, startInfo;

/// Windows' size/move loop as the runner reports it (WM_ENTER/EXITSIZEMOVE).
Future<void> sizeMove(WidgetTester tester, bool active) async {
  await TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
      .handlePlatformMessage(
        'multitap/window/events',
        const StandardMethodCodec().encodeSuccessEnvelope({
          'type': 'sizeMove',
          'active': active,
        }),
        (_) {},
      );
  await tester.pump();
}

Finder picture(int texture) => find.byWidgetPredicate(
  (widget) => widget is Texture && widget.textureId == texture,
);

/// The session toolbar rests as a handle; a click opens it.
Future<void> openBar(WidgetTester tester) async {
  if (find.byKey(const Key('session-command-bar')).evaluate().isEmpty) {
    await tester.tap(find.byKey(const Key('expand-session-toolbar')));
    await tester.pump();
  }
}

class _ViewportFixture {
  _ViewportFixture(this.socket, this.media, this.session);
  final SessionSocket socket;
  final ResizeMedia media;
  final RemoteSession session;
  int epoch = 0;
  bool disposed = false;

  List<Map<String, dynamic>> get requests =>
      socket.sent.where((event) => event['type'] == 'stream.resize').toList();

  static Future<_ViewportFixture> mount(
    WidgetTester tester, {
    StreamOptions options = const StreamOptions(),
    Size physical = const Size(800, 600),
    double pixelRatio = 1,
    Future<RemoteSession> Function(StreamOptions)? onReconnect,
    Future<Object?> Function(MethodCall)? windowHandler,
    SessionDisplayMode displayMode = SessionDisplayMode.fullscreen,
    ResizeMedia? resizeMedia,
  }) async {
    tester.view.devicePixelRatio = pixelRatio;
    tester.view.physicalSize = physical;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    const window = MethodChannel('multitap/window');
    const events = MethodChannel('multitap/window/events');
    messenger.setMockMethodCallHandler(
      window,
      windowHandler ?? (_) async => false,
    );
    messenger.setMockMethodCallHandler(events, (_) async => null);
    addTearDown(() {
      messenger.setMockMethodCallHandler(window, null);
      messenger.setMockMethodCallHandler(events, null);
    });
    final socket = SessionSocket(), media = resizeMedia ?? ResizeMedia();
    final session = RemoteSession(
      connection: BrokerConnection(
        sessionId: 'viewport-ui-fixture',
        webSocketUrl: Uri.parse(
          'wss://fixture.invalid/api/v1/sessions/viewport-ui-fixture',
        ),
        epochMedia: true,
        viewportProvisioning: true,
        options: options,
      ),
      token: 'fixture-only-not-a-real-token',
      media: media,
      dialer: (_, _) async => socket,
    );
    await tester.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: machine(1),
          initialOptions: options,
          initialDisplayMode: displayMode,
          load: () async => session,
          onReconnect: onReconnect,
        ),
      ),
    );
    await tester.pump();
    await tester.pump();
    return _ViewportFixture(socket, media, session);
  }

  Future<void> initialFrame(WidgetTester tester) async {
    socket.incoming.add(jsonEncode(startInfo));
    await tester.pump();
    await tester.pump();
    socket.incoming.add(packet());
    await tester.pump();
    await tester.pump();
    expect(media.packets, hasLength(1));
  }

  Future<void> waitFor(WidgetTester tester, int count) async {
    for (var n = 0; n < 25 && requests.length < count; n++) {
      await tester.pump(const Duration(milliseconds: 100));
      await tester.pump();
    }
    expect(requests, hasLength(count));
  }

  Future<void> refuse(WidgetTester tester, Map<String, dynamic> request) async {
    socket.incoming.add(
      jsonEncode({
        'type': 'stream.resize.failed',
        'requestId': request['requestId'],
        'unchanged': true,
        'reason': 'unsupported_display_mode',
      }),
    );
    await tester.pump();
    await tester.pump();
  }

  Future<Map<String, dynamic>> acknowledge(
    WidgetTester tester,
    Map<String, dynamic> request,
  ) async {
    final ack = <String, dynamic>{
      ...request,
      'type': 'stream.resizing',
      'resizeId': 'ui-epoch-${++epoch}',
      'epoch': epoch,
      'timeoutMs': 90000,
    };
    socket.incoming.add(jsonEncode(ack));
    await tester.pump();
    await tester.pump();
    expect(session.resizing, isTrue);
    return ack;
  }

  Future<void> apply(WidgetTester tester, Map<String, dynamic> ack) async {
    socket.incoming.add(
      jsonEncode({
        ...startInfo,
        'sessionId': session.connection.sessionId,
        'requestId': ack['requestId'],
        'resizeId': ack['resizeId'],
        'epoch': ack['epoch'],
        'width': ack['width'],
        'height': ack['height'],
        'sourceWidth': ack['width'],
        'sourceHeight': ack['height'],
      }),
    );
    await tester.pump();
    await tester.pump();
    final frame = packet();
    frame[2] = epoch & 255;
    frame[3] = epoch >> 8;
    socket.incoming.add(frame);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 60));
    expect(session.resizing, isFalse);
    expect(
      [session.sourceWidth, session.sourceHeight],
      [ack['width'], ack['height']],
    );
  }

  Future<void> close(WidgetTester tester) async {
    if (disposed) return;
    disposed = true;
    await tester.pumpWidget(const SizedBox.shrink());
    unawaited(session.close());
    await tester.pump(const Duration(seconds: 3));
    await tester.pump();
    unawaited(media.events.close());
    expect(session.closed, isTrue);
    expect(socket.wasClosed, isTrue);
  }
}

void main() {
  for (final refused in [false, true]) {
    testWidgets(
      'scale-only menu retains automatic workspace resize, refused=$refused',
      (tester) async {
        final fixture = await _ViewportFixture.mount(
          tester,
          physical: const Size(960, 640),
          displayMode: SessionDisplayMode.windowed,
        );
        try {
          await fixture.initialFrame(tester);
          await fixture.waitFor(tester, 1);
          await fixture.apply(
            tester,
            await fixture.acknowledge(tester, fixture.requests.last),
          );
          final sessionId = fixture.session.connection.sessionId;
          await openBar(tester);
          await tester.pump(const Duration(milliseconds: 200));
          // 배율 is in the 화면 panel, under the screen modes: five steps
          // and no resolution choice.
          await tester.tap(find.byKey(const Key('stream-menu-화면')));
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('session-screen-panel')), findsOneWidget);
          expect(find.byKey(const Key('remote-display-scale')), findsOneWidget);
          expect(
            find.descendant(
              of: find.byKey(const Key('display-scale-steps')),
              matching: find.byType(TextButton),
            ),
            findsNWidgets(5),
          );
          expect(find.byKey(const Key('display-resolution-125')), findsNothing);
          expect(find.text('해상도 자동 · 현재 960 × 640'), findsOneWidget);
          Navigator.of(
            tester.element(find.byKey(const Key('display-scale-panel'))),
          ).pop();
          await tester.pumpAndSettle();
          // Actual window pixels, not a manual resolution choice, drive epoch2.
          tester.view.physicalSize = const Size(1200, 800);
          await fixture.waitFor(tester, 2);
          expect(
            [fixture.requests.last['width'], fixture.requests.last['height']],
            [1200, 800],
          );
          if (refused) {
            await fixture.refuse(tester, fixture.requests.last);
            expect(
              [fixture.session.sourceWidth, fixture.session.sourceHeight],
              [960, 640],
            );
          } else {
            await fixture.apply(
              tester,
              await fixture.acknowledge(tester, fixture.requests.last),
            );
            expect(
              [fixture.session.sourceWidth, fixture.session.sourceHeight],
              [1200, 800],
            );
          }
          // No fixed/manual intent can suppress a later window resize.
          tester.view.physicalSize = const Size(1000, 700);
          await fixture.waitFor(tester, 3);
          expect(
            [fixture.requests.last['width'], fixture.requests.last['height']],
            [1000, 700],
          );
          await fixture.apply(
            tester,
            await fixture.acknowledge(tester, fixture.requests.last),
          );
          expect(fixture.session.connection.sessionId, sessionId);
          expect(fixture.session.closed, isFalse);
          expect(tester.view.physicalSize, const Size(1000, 700));
          expect(tester.takeException(), isNull);
        } finally {
          await fixture.close(tester);
        }
      },
    );
  }
  testWidgets(
    'Fullscreen and return request physical pixels after native DPI/size changes',
    (tester) async {
      final fixture = await _ViewportFixture.mount(
        tester,
        displayMode: SessionDisplayMode.windowed,
        windowHandler: (call) async {
          if (call.method != 'fullscreen') return null;
          await Future<void>.value();
          final full = call.arguments == true;
          tester.view.devicePixelRatio = full ? 1.5 : 1.25;
          tester.view.physicalSize = full
              ? const Size(2560, 1440)
              : const Size(1000, 750);
          return full;
        },
      );
      // The bar's 화면 panel: each screen mode is a tile.
      Future<void> mode(String key) async {
        await openBar(tester);
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(Key(key)));
        await tester.pump();
        await tester.pump();
      }

      try {
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        await fixture.apply(
          tester,
          await fixture.acknowledge(tester, fixture.requests.last),
        );
        await mode('stream-fullscreen');
        await fixture.waitFor(tester, 2);
        expect(
          [fixture.requests.last['width'], fixture.requests.last['height']],
          [2560, 1440],
        );
        await fixture.apply(
          tester,
          await fixture.acknowledge(tester, fixture.requests.last),
        );
        await mode('stream-windowed');
        await fixture.waitFor(tester, 3);
        expect(
          [fixture.requests.last['width'], fixture.requests.last['height']],
          [1000, 750],
        );
        await fixture.apply(
          tester,
          await fixture.acknowledge(tester, fixture.requests.last),
        );
        expect(tester.takeException(), isNull);
      } finally {
        await fixture.close(tester);
      }
    },
  );

  testWidgets(
    'Explicit screen mode selection can retry a size whose automatic retries were exhausted',
    (tester) async {
      final fixture = await _ViewportFixture.mount(
        tester,
        windowHandler: (call) async =>
            call.method == 'fullscreen' ? call.arguments : null,
      );
      try {
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        await fixture.refuse(tester, fixture.requests.last);
        await fixture.waitFor(tester, 2);
        await fixture.refuse(tester, fixture.requests.last);
        await tester.pump(const Duration(seconds: 2));
        expect(fixture.requests, hasLength(2));
        await openBar(tester);
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('stream-windowed')));
        await tester.pump();
        await tester.pump();
        await fixture.waitFor(tester, 3);
        await fixture.apply(
          tester,
          await fixture.acknowledge(tester, fixture.requests.last),
        );
        expect(tester.takeException(), isNull);
      } finally {
        await fixture.close(tester);
      }
    },
  );

  testWidgets(
    'Applying streaming settings keeps actual viewport resizing enabled after reconnect',
    (tester) async {
      _ViewportFixture? replacement;
      final fixture = await _ViewportFixture.mount(
        tester,
        onReconnect: (options) async {
          final socket = SessionSocket(), media = ResizeMedia();
          final session = RemoteSession(
            connection: BrokerConnection(
              sessionId: 'viewport-ui-fixture',
              webSocketUrl: Uri.parse(
                'wss://fixture.invalid/api/v1/sessions/viewport-ui-fixture',
              ),
              epochMedia: true,
              viewportProvisioning: true,
              options: options,
            ),
            token: 'fixture-only',
            media: media,
            dialer: (_, _) async => socket,
          );
          replacement = _ViewportFixture(socket, media, session);
          return session;
        },
      );
      try {
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        await fixture.apply(
          tester,
          await fixture.acknowledge(tester, fixture.requests.single),
        );
        // 설정, then its 화질 row (the row hands its key to its InkWell
        // too: the first match is the row): the settings open in its place.
        await openBar(tester);
        await tester.tap(find.byKey(const Key('remote-settings')));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('remote-stream-settings')).first);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('session-settings-panel')), findsNothing);
        expect(find.byKey(const Key('stream-settings-panel')), findsOneWidget);
        expect(find.text('화질'), findsOneWidget);
        expect(find.byKey(const Key('stream-resolution')), findsNothing);
        // The panel applies a change only: here a lower video bitrate (the
        // frame rate stays, so the Host's stream info still matches).
        expect(
          tester
              .widget<FilledButton>(
                find.byKey(const Key('apply-stream-settings')),
              )
              .onPressed,
          isNull,
        );
        await tester.tap(find.byKey(const Key('stream-advanced')));
        await tester.pumpAndSettle();
        await tester.drag(
          find.byKey(const Key('stream-bitrate')),
          const Offset(-120, 0),
        );
        await tester.pump();
        await tester.tap(find.byKey(const Key('apply-stream-settings')));
        for (var n = 0; n < 20 && replacement == null; n++) {
          await tester.pump(const Duration(milliseconds: 100));
        }
        expect(replacement, isNotNull);
        expect(replacement!.session.connection.options.maxWidth, 3840);
        expect(replacement!.session.connection.options.maxHeight, 2160);
        await replacement!.initialFrame(tester);
        await replacement!.waitFor(tester, 1);
        expect(
          [
            replacement!.requests.single['width'],
            replacement!.requests.single['height'],
          ],
          [800, 600],
        );
        await replacement!.apply(
          tester,
          await replacement!.acknowledge(tester, replacement!.requests.single),
        );
        expect(
          [replacement!.session.sourceWidth, replacement!.session.sourceHeight],
          [800, 600],
        );
        expect(tester.takeException(), isNull);
      } finally {
        await replacement?.close(tester);
        await fixture.close(tester);
      }
    },
  );
  testWidgets(
    'A nondefault stream preset still requests the physical client viewport once after initial media',
    (tester) async {
      final fixture = await _ViewportFixture.mount(
        tester,
        options: const StreamOptions(maxWidth: 1280, maxHeight: 720),
        physical: const Size(1200, 800),
        pixelRatio: 1.25,
      );
      try {
        await tester.pump(const Duration(milliseconds: 800));
        expect(
          fixture.requests,
          isEmpty,
          reason: 'No resize before native media initialization.',
        );
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        expect(
          [fixture.requests.single['width'], fixture.requests.single['height']],
          [1200, 800],
        );
        final ack = await fixture.acknowledge(tester, fixture.requests.single);
        await fixture.apply(tester, ack);
        await tester.pump(const Duration(seconds: 2));
        expect(fixture.requests, hasLength(1));
        expect(tester.takeException(), isNull);
      } finally {
        await fixture.close(tester);
      }
    },
  );

  testWidgets(
    'Rapid streaming window changes send only the latest debounced viewport',
    (tester) async {
      final fixture = await _ViewportFixture.mount(tester);
      try {
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        await fixture.apply(
          tester,
          await fixture.acknowledge(tester, fixture.requests.single),
        );
        for (final size in [
          const Size(900, 620),
          const Size(1000, 700),
          const Size(1100, 740),
        ]) {
          tester.view.physicalSize = size;
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 180));
        }
        expect(fixture.requests, hasLength(1));
        tester.view.physicalSize = const Size(1120, 760);
        await tester.pump();
        // The settle (RemoteView.viewportSettle, 250 ms) is not over yet.
        await tester.pump(const Duration(milliseconds: 200));
        expect(fixture.requests, hasLength(1));
        await fixture.waitFor(tester, 2);
        expect(
          [fixture.requests.last['width'], fixture.requests.last['height']],
          [1120, 760],
        );
      } finally {
        await fixture.close(tester);
      }
    },
  );

  testWidgets(
    'An unchanged refusal retries the same viewport at most twice in total without stopping the old decoder',
    (tester) async {
      final fixture = await _ViewportFixture.mount(tester);
      try {
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        final firstId = fixture.requests.single['requestId'];
        await fixture.refuse(tester, fixture.requests.single);
        await fixture.waitFor(tester, 2);
        expect(fixture.requests.last['requestId'], isNot(firstId));
        expect(
          [fixture.requests.last['width'], fixture.requests.last['height']],
          [800, 600],
        );
        await fixture.refuse(tester, fixture.requests.last);
        await tester.pump(const Duration(seconds: 4));
        await tester.pump();
        expect(
          fixture.requests,
          hasLength(2),
          reason: 'One initial attempt plus one bounded retry.',
        );
        expect(fixture.session.closed, isFalse);
        expect(fixture.session.textureId, isNotNull);
        expect(fixture.media.dimensions, [
          [1280, 720],
        ]);
        expect(fixture.media.stopped, isFalse);
        expect(find.text('해상도 다시 시도'), findsOneWidget);
        await tester.tap(find.byKey(const Key('viewport-status')));
        await tester.pump();
        await fixture.waitFor(tester, 3);
        // While a resize is in flight there is no badge; the veil covers
        // the decoder swap once the host acknowledges.
        expect(find.byKey(const Key('viewport-status')), findsNothing);
        final acknowledged = await fixture.acknowledge(
          tester,
          fixture.requests.last,
        );
        // The decoder swap shows the quiet veil, never the connecting card.
        expect(find.byKey(const Key('resize-veil')), findsOneWidget);
        expect(find.text('데스크톱에 연결하는 중'), findsNothing);
        await fixture.apply(tester, acknowledged);
        expect(find.byKey(const Key('resize-veil')), findsNothing);
        expect(find.byKey(const Key('viewport-status')), findsNothing);
        expect(fixture.session.closed, isFalse);
      } finally {
        await fixture.close(tester);
      }
    },
  );

  testWidgets(
    'A new desired viewport during a failed request supersedes the old size and gets its own bounded retry',
    (tester) async {
      final fixture = await _ViewportFixture.mount(tester);
      try {
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        final old = fixture.requests.single;
        tester.view.physicalSize = const Size(1000, 700);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 700));
        expect(
          fixture.requests,
          hasLength(1),
          reason: 'Native display changes must not overlap.',
        );
        await fixture.refuse(tester, old);
        await fixture.waitFor(tester, 2);
        expect(
          [fixture.requests.last['width'], fixture.requests.last['height']],
          [1000, 700],
        );
        await fixture.refuse(tester, fixture.requests.last);
        await fixture.waitFor(tester, 3);
        expect(
          [fixture.requests.last['width'], fixture.requests.last['height']],
          [1000, 700],
        );
        await fixture.refuse(tester, fixture.requests.last);
        await tester.pump(const Duration(seconds: 3));
        expect(fixture.requests, hasLength(3));
      } finally {
        await fixture.close(tester);
      }
    },
  );

  testWidgets(
    'Returning from B to an earlier A while B is in flight requests A again after the accepted epoch',
    (tester) async {
      final fixture = await _ViewportFixture.mount(tester);
      try {
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        await fixture.apply(
          tester,
          await fixture.acknowledge(tester, fixture.requests.single),
        );
        tester.view.physicalSize = const Size(1000, 700);
        await tester.pump();
        await fixture.waitFor(tester, 2);
        final b = await fixture.acknowledge(tester, fixture.requests.last);
        tester.view.physicalSize = const Size(800, 600);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 700));
        expect(fixture.requests, hasLength(2));
        await fixture.apply(tester, b);
        await fixture.waitFor(tester, 3);
        expect(
          [fixture.requests.last['width'], fixture.requests.last['height']],
          [800, 600],
        );
        await fixture.apply(
          tester,
          await fixture.acknowledge(tester, fixture.requests.last),
        );
        expect(fixture.media.dimensions, [
          [1280, 720],
          [800, 600],
          [1000, 700],
          [800, 600],
        ]);
      } finally {
        await fixture.close(tester);
      }
    },
  );

  testWidgets(
    'Disposing the stream view cancels retry and in-flight follow-up work',
    (tester) async {
      for (final failBeforeDispose in [true, false]) {
        final fixture = await _ViewportFixture.mount(tester);
        try {
          await fixture.initialFrame(tester);
          await fixture.waitFor(tester, 1);
          // Disposed within the settle (RemoteView.viewportSettle, 250 ms).
          if (failBeforeDispose) {
            await fixture.refuse(tester, fixture.requests.single);
            await tester.pump(const Duration(milliseconds: 200));
          } else {
            tester.view.physicalSize = const Size(1000, 700);
            await tester.pump();
            await tester.pump(const Duration(milliseconds: 200));
          }
          await fixture.close(tester);
          await tester.pump(const Duration(seconds: 2));
          expect(fixture.requests, hasLength(1));
          expect(tester.takeException(), isNull);
        } finally {
          await fixture.close(tester);
        }
      }
    },
  );

  testWidgets(
    'A window resize keeps the previous picture, scaled to the window, until '
    'the new size has a frame',
    (tester) async {
      final media = HoldMedia();
      final fixture = await _ViewportFixture.mount(
        tester,
        physical: const Size(1600, 1000),
        resizeMedia: media,
      );
      try {
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        final ack = await fixture.acknowledge(tester, fixture.requests.single);
        // The Host is changing its display: the 1280x720 picture stays on
        // screen, scaled up to the 1600x1000 window (not black, not 1:1).
        expect(picture(41), findsOneWidget);
        expect(tester.getSize(picture(41)), const Size(1600, 900));
        expect(find.text('화면 크기를 맞추고 있어요.'), findsNothing);
        await tester.pump(const Duration(milliseconds: 300));
        expect(find.byKey(const Key('resize-veil')), findsOneWidget);
        await fixture.apply(tester, ack);
        expect(picture(41), findsNothing);
        expect(picture(42), findsOneWidget);
        expect(find.byKey(const Key('resize-veil')), findsNothing);
        expect(media.released, isEmpty);
        await tester.pump(const Duration(milliseconds: 300));
        expect(media.released, [41]);
        expect(media.stopped, isFalse);
      } finally {
        await fixture.close(tester);
      }
    },
  );

  testWidgets('A window size change is asked for after a short settle', (
    tester,
  ) async {
    final fixture = await _ViewportFixture.mount(tester);
    try {
      await fixture.initialFrame(tester);
      await fixture.waitFor(tester, 1);
      await fixture.apply(
        tester,
        await fixture.acknowledge(tester, fixture.requests.single),
      );
      tester.view.physicalSize = const Size(1000, 700);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 200));
      expect(fixture.requests, hasLength(1));
      // 250 ms after the last change (it was 650 ms).
      await tester.pump(const Duration(milliseconds: 100));
      expect(fixture.requests, hasLength(2));
      expect(
        [fixture.requests.last['width'], fixture.requests.last['height']],
        [1000, 700],
      );
    } finally {
      await fixture.close(tester);
    }
  });

  testWidgets('A border drag asks once, for its final size, when it ends', (
    tester,
  ) async {
    final fixture = await _ViewportFixture.mount(tester);
    try {
      await fixture.initialFrame(tester);
      await fixture.waitFor(tester, 1);
      await fixture.apply(
        tester,
        await fixture.acknowledge(tester, fixture.requests.single),
      );
      await sizeMove(tester, true);
      // Pauses longer than the ordinary settle, still one drag.
      for (final size in const [
        Size(900, 640),
        Size(1000, 700),
        Size(1100, 760),
      ]) {
        tester.view.physicalSize = size;
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 400));
      }
      expect(
        fixture.requests,
        hasLength(1),
        reason: 'nothing is asked for while the border is held',
      );
      await sizeMove(tester, false);
      await tester.pump(const Duration(milliseconds: 150));
      expect(fixture.requests, hasLength(2));
      expect(
        [fixture.requests.last['width'], fixture.requests.last['height']],
        [1100, 760],
      );
      await fixture.apply(
        tester,
        await fixture.acknowledge(tester, fixture.requests.last),
      );
      await tester.pump(const Duration(seconds: 2));
      expect(fixture.requests, hasLength(2));
    } finally {
      await fixture.close(tester);
    }
  });

  testWidgets(
    'Input comes back by itself after a resize when the picture had it',
    (tester) async {
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      final fixture = await _ViewportFixture.mount(tester);
      try {
        await fixture.initialFrame(tester);
        await fixture.waitFor(tester, 1);
        await fixture.apply(
          tester,
          await fixture.acknowledge(tester, fixture.requests.single),
        );
        await tester.tap(find.byType(Texture));
        await tester.pump(Duration.zero);
        expect(fixture.session.captured, isTrue);
        tester.view.physicalSize = const Size(1000, 700);
        await fixture.waitFor(tester, 2);
        expect(
          fixture.session.captured,
          isFalse,
          reason: 'released while the display changes',
        );
        await fixture.apply(
          tester,
          await fixture.acknowledge(tester, fixture.requests.last),
        );
        await tester.pump(Duration.zero);
        expect(fixture.session.captured, isTrue);
      } finally {
        await fixture.close(tester);
      }
    },
  );
}

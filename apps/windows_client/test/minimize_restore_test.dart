// Minimize and restore of a remote window: minimizing must not end the
// connection or need a restart of the client. The actual RemoteView with an
// in-memory relay and media; no native window, monitor or decoder is used.
import 'dart:convert';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'remote_session_test.dart' show SessionSocket, packet;
import 'stream_display_entry_test.dart' show entryMachine;
import 'track_stream_test.dart' as track_fixture;
import 'viewport_session_test.dart' as viewport_fixture;

const _changedLayout = '모니터 구성이 변경됐어요. 연결을 끊은 뒤 화면 모드를 다시 선택하세요.';

/// The two local monitors the fixture's outputs pair with.
Map<String, Object?> _monitors() => {
  'x': 0,
  'y': 0,
  'width': 1920,
  'height': 720,
  'monitors': [
    {
      'id': '0',
      'x': 0,
      'y': 0,
      'width': 640,
      'height': 360,
      'primary': true,
      'dpi': 96,
    },
    {
      'id': '1',
      'x': 640,
      'y': 0,
      'width': 1280,
      'height': 720,
      'primary': false,
      'dpi': 144,
    },
  ],
};

/// The Host's per-output rates, as the relay passes them about once a second;
/// each one is a session change the view hears.
String _hostStats() => jsonEncode({
  'type': 'stream.tracks.stats',
  'sessionId': 'track-session',
  'topologyEpoch': 1,
  'measuredMs': 1000,
  'tracks': [
    for (final id in ['0', '1'])
      {
        'id': id,
        'epoch': 1,
        'capturedFps': 60.0,
        'encodedFps': 60.0,
        'bitrateKbps': 4000.0,
      },
  ],
});

Future<void> _connected(
  WidgetTester t,
  Future<void> Function(RemoteSession session, SessionSocket socket) body,
) async {
  t.view.devicePixelRatio = 1;
  t.view.physicalSize = const Size(1920, 720);
  addTearDown(t.view.resetDevicePixelRatio);
  addTearDown(t.view.resetPhysicalSize);
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const window = MethodChannel('multitap/window');
  const events = MethodChannel('multitap/window/events');
  messenger.setMockMethodCallHandler(events, (_) async => null);
  messenger.setMockMethodCallHandler(window, (call) async {
    if (call.method == 'monitorLayout') return _monitors();
    if (call.method == 'primaryRegion') return null;
    return call.method == 'allMonitors' || call.method == 'fullscreen';
  });
  addTearDown(() {
    messenger.setMockMethodCallHandler(window, null);
    messenger.setMockMethodCallHandler(events, null);
  });
  final socket = SessionSocket();
  final media = track_fixture.TrackMedia();
  final session = RemoteSession(
    connection: track_fixture.connection(),
    token: 'fixture-only-token',
    media: media,
    dialer: (_, _) async => socket,
  );
  try {
    await t.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: entryMachine(2),
          initialDisplayMode: SessionDisplayMode.allMonitors,
          load: () async => session,
          onConnectTracks: (_, _) async => session,
        ),
      ),
    );
    for (var i = 0; i < 6; i++) {
      await t.pump();
    }
    socket.incoming.add(jsonEncode(track_fixture.info()));
    await t.pump();
    await t.pump();
    socket.incoming.add(track_fixture.packet(0));
    socket.incoming.add(track_fixture.packet(1));
    media.currentStats = track_fixture.trackStats();
    await t.pump(const Duration(seconds: 1));
    await t.pump();
    await t.pump();
    expect(session.frameReady, isTrue, reason: session.error);
    expect(find.byKey(const Key('local-output-0')), findsOneWidget);
    expect(find.byKey(const Key('local-output-1')), findsOneWidget);
    await body(session, socket);
    expect(session.closed, isFalse, reason: session.error);
  } finally {
    t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await t.pumpWidget(const SizedBox.shrink());
    final closing = session.close();
    await t.pump(const Duration(seconds: 3));
    await t.pump();
    await closing;
    await media.events.close();
  }
}

/// [span] of steady session traffic, a change about every 300 ms.
Future<void> _traffic(
  WidgetTester t,
  SessionSocket socket,
  Duration span,
) async {
  for (var waited = Duration.zero; waited < span;) {
    socket.incoming.add(_hostStats());
    await t.pump(const Duration(milliseconds: 300));
    waited += const Duration(milliseconds: 300);
  }
}

void main() {
  testWidgets(
    'A minimized single-output window keeps its picture, size and input after restore',
    (t) async {
      t.view.devicePixelRatio = 1;
      t.view.physicalSize = const Size(1280, 720);
      addTearDown(t.view.resetDevicePixelRatio);
      addTearDown(t.view.resetPhysicalSize);
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
      final socket = SessionSocket(), media = viewport_fixture.ResizeMedia();
      final session = RemoteSession(
        connection: viewport_fixture.connection,
        token: 'fixture-only-token',
        media: media,
        dialer: (_, _) async => socket,
      );
      try {
        await t.pumpWidget(
          MaterialApp(
            home: RemoteView(
              machine: entryMachine(1),
              load: () async => session,
            ),
          ),
        );
        await t.pump();
        await t.pump();
        socket.incoming.add(jsonEncode(viewport_fixture.startInfo));
        await t.pump();
        await t.pump();
        socket.incoming.add(packet());
        await t.pump();
        await t.pump();
        expect(session.frameReady, isTrue, reason: session.error);
        expect(find.byType(Texture), findsOneWidget);
        socket.sent.clear();
        t.view.physicalSize = Size.zero;
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
        for (var n = 0; n < 7; n++) {
          socket.incoming.add(packet());
          await t.pump(const Duration(milliseconds: 300));
        }
        t.view.physicalSize = const Size(1280, 720);
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
        await t.pump();
        expect(find.byType(Texture), findsOneWidget);
        for (var n = 0; n < 7; n++) {
          socket.incoming.add(packet());
          await t.pump(const Duration(milliseconds: 300));
        }
        expect(session.closed, isFalse, reason: session.error);
        expect(find.byType(Texture), findsOneWidget);
        expect(
          socket.sent.where((e) => e['type'] == 'stream.resize'),
          isEmpty,
          reason: 'an empty minimized view never resizes the remote screen',
        );
        final picture = t.getRect(find.byKey(const Key('remote-image-bounds')));
        final mouse = await t.createGesture(kind: PointerDeviceKind.mouse);
        await mouse.addPointer(location: picture.center);
        await mouse.down(picture.center);
        await t.pump();
        await t.pump();
        expect(session.captured, isTrue);
        await mouse.up();
        await t.pump();
        await mouse.removePointer();
      } finally {
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
        await t.pumpWidget(const SizedBox.shrink());
        final closing = session.close();
        await t.pump(const Duration(seconds: 3));
        await t.pump();
        await closing;
        await media.events.close();
      }
    },
  );

  testWidgets(
    'A minimized 모든 모니터 window shows both outputs again at once after restore',
    (t) async {
      await _connected(t, (session, socket) async {
        // Windows minimize: the view becomes empty, then the app is hidden.
        t.view.physicalSize = Size.zero;
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
        await _traffic(t, socket, const Duration(seconds: 2));
        // Restore: the same window size and scale come back.
        t.view.physicalSize = const Size(1920, 720);
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
        t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
        await t.pump();
        expect(
          find.text(_changedLayout),
          findsNothing,
          reason: 'restoring the same window is not a monitor change',
        );
        expect(find.byKey(const Key('local-output-0')), findsOneWidget);
        expect(find.byKey(const Key('local-output-1')), findsOneWidget);
        await _traffic(t, socket, const Duration(seconds: 2));
        expect(find.text(_changedLayout), findsNothing);
        // Input comes back with a click into the picture, like after Alt+Tab.
        socket.sent.clear();
        final surface = find.byKey(const Key('local-output-1'));
        final mouse = await t.createGesture(kind: PointerDeviceKind.mouse);
        await mouse.addPointer(location: t.getCenter(surface));
        await mouse.down(t.getCenter(surface));
        await t.pump();
        await t.pump();
        expect(session.captured, isTrue);
        expect(
          socket.sent.where(
            (e) =>
                e['type'] == 'input' &&
                e['event'] == 'button' &&
                e['down'] == true &&
                e['track'] == '1',
          ),
          hasLength(1),
        );
        await mouse.up();
        await t.pump();
        await mouse.removePointer();
      });
    },
  );

  testWidgets(
    'Steady session traffic cannot postpone the re-read of a changed layout',
    (t) async {
      await _connected(t, (session, socket) async {
        // A real change (another scale on the same pixels) needs the re-read.
        t.view.devicePixelRatio = 1.5;
        await t.pump();
        expect(find.text(_changedLayout), findsOneWidget);
        // Stats, pongs and Host rates arrive about three times a second; each
        // used to restart the 650 ms wait, so the read never came.
        await _traffic(t, socket, const Duration(milliseconds: 1200));
        expect(find.text(_changedLayout), findsNothing);
        expect(find.byKey(const Key('local-output-0')), findsOneWidget);
        expect(find.byKey(const Key('local-output-1')), findsOneWidget);
      });
    },
  );
}

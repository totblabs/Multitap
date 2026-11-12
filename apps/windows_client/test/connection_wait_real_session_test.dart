// The connecting list in the real session view, driven by a real session
// (injected socket and media): which row a window-size connection is at, from
// the request to the first picture.
//
// On a Host with the window-size entry the list must not stand on 가상 화면
// 준비 from before the request until the picture comes: every other stage is
// reached in its turn. Nothing outside the test process is touched.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'initial_viewport_test.dart' show connection, info, requestedOptions;
import 'remote_session_test.dart' show SessionSocket, SessionMedia, packet;
import 'stream_display_entry_test.dart' show entryMachine;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('a window-size connection goes down the list: 연결 요청, '
      '가상 화면 준비, 첫 화면 수신 대기, then the picture', (t) async {
    t.view.physicalSize = const Size(2560, 1440);
    t.view.devicePixelRatio = 1.5;
    addTearDown(t.view.resetPhysicalSize);
    addTearDown(t.view.resetDevicePixelRatio);
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    for (final name in ['multitap/window', 'multitap/window/events']) {
      messenger.setMockMethodCallHandler(MethodChannel(name), (_) async => true);
      addTearDown(
        () => messenger.setMockMethodCallHandler(MethodChannel(name), null),
      );
    }
    final refresh = Completer<void>(), connect = Completer<void>();
    final socket = SessionSocket(), media = SessionMedia();
    final session = RemoteSession(
      connection: connection(),
      token: 'fixture-token',
      media: media,
      dialer: (_, _) async => socket,
    );
    String now() =>
        t.widget<Text>(find.byKey(const Key('connection-wait-step'))).data!;
    List<String> done() => [
      for (final stage in ['request', 'relay', 'display', 'firstFrame'])
        if (find
            .byKey(ValueKey('connection-wait-done-$stage'))
            .evaluate()
            .isNotEmpty)
          stage,
    ];
    await t.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: entryMachine(1, initialViewport: true),
          prepareDisplaysOnEntry: true,
          initialOptions: requestedOptions,
          load: () async => throw StateError('Legacy capture must not run.'),
          onRefreshDesktop: () async {
            await refresh.future;
            return entryMachine(1, initialViewport: true);
          },
          onConnectViewport: (viewport, options) async {
            await connect.future;
            return session;
          },
        ),
      ),
    );
    await t.pump();
    // Reading the desktop, then the request itself: the first row.
    expect(now(), '연결 요청');
    expect(done(), isEmpty);
    await t.pump(const Duration(milliseconds: 300));
    expect(now(), '연결 요청');
    refresh.complete();
    for (var i = 0; i < 3; i++) {
      await t.pump(const Duration(milliseconds: 100));
    }
    expect(now(), '연결 요청', reason: 'the request is still unanswered');
    expect(done(), isEmpty);
    // Answered and connected: the Host sets the display that was asked for.
    connect.complete();
    for (var i = 0; i < 3; i++) {
      await t.pump(const Duration(milliseconds: 100));
    }
    expect(session.preparingInitialDisplay, isTrue);
    expect(now(), '가상 화면 준비');
    expect(done(), ['request', 'relay']);
    // The stream's size arrived: the display is set, the picture is awaited.
    socket.incoming.add(jsonEncode(info()));
    await t.pump(const Duration(milliseconds: 10));
    await t.pump();
    expect(now(), '첫 화면 수신 대기');
    expect(done(), ['request', 'relay', 'display']);
    // The first decoded picture ends the list.
    socket.incoming.add(packet()..[2] = 1);
    await t.pump();
    await t.pump();
    await t.pump(const Duration(milliseconds: 60));
    await t.pump();
    expect(session.frameReady, isTrue);
    expect(find.byKey(const Key('connection-wait-step')), findsNothing);
    unawaited(session.close());
    await t.pump(const Duration(seconds: 3));
    await t.pumpWidget(const SizedBox());
    unawaited(media.events.close());
    await t.pump();
  });
}

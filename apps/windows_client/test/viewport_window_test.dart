import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionSocket, packet;
import 'viewport_session_test.dart' show ResizeMedia, connection, startInfo;

void main() {
  testWidgets(
    'An encoding size preference does not disable actual automatic viewport resolution',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(800, 600);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      const window = MethodChannel('multitap/window'),
          events = MethodChannel('multitap/window/events');
      messenger.setMockMethodCallHandler(window, (_) async => false);
      messenger.setMockMethodCallHandler(events, (_) async => null);
      addTearDown(() {
        messenger.setMockMethodCallHandler(window, null);
        messenger.setMockMethodCallHandler(events, null);
      });
      final socket = SessionSocket(), media = ResizeMedia();
      final session = RemoteSession(
        connection: BrokerConnection(
          sessionId: connection.sessionId,
          webSocketUrl: connection.webSocketUrl,
          epochMedia: true,
          viewportProvisioning: true,
          options: const StreamOptions(maxWidth: 1280, maxHeight: 720),
        ),
        token: 'fixture-only',
        media: media,
        dialer: (_, _) async => socket,
      );
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(machine: machine(1), load: () async => session),
        ),
      );
      await tester.pump();
      socket.incoming.add(jsonEncode(startInfo));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 1400));
      expect(socket.sent.where((v) => v['type'] == 'stream.resize').length, 1);
      final requested = socket.sent.singleWhere(
        (v) => v['type'] == 'stream.resize',
      );
      expect([requested['width'], requested['height']], [800, 600]);
      await tester.pumpWidget(const SizedBox());
      unawaited(session.close());
      await tester.pump(const Duration(seconds: 3));
      expect(session.closed, true);
      unawaited(media.events.close());
    },
  );
  testWidgets(
    'Window drags keep the latest physical viewport, debounce requests and never overlap hardware changes',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(800, 600);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      const window = MethodChannel('multitap/window'),
          events = MethodChannel('multitap/window/events');
      messenger.setMockMethodCallHandler(window, (_) async => false);
      messenger.setMockMethodCallHandler(events, (_) async => null);
      addTearDown(() {
        messenger.setMockMethodCallHandler(window, null);
        messenger.setMockMethodCallHandler(events, null);
      });
      final socket = SessionSocket(), media = ResizeMedia();
      final session = RemoteSession(
        connection: connection,
        token: 'fixture-only',
        media: media,
        dialer: (_, _) async => socket,
      );
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(machine: machine(1), load: () async => session),
        ),
      );
      await tester.pump();
      socket.incoming.add(jsonEncode(startInfo));
      await tester.pump();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 700));
      await tester.pump();
      List<Map<String, dynamic>> requests() =>
          socket.sent.where((v) => v['type'] == 'stream.resize').toList();
      expect(requests().length, 1);
      final first = requests().single;
      expect([first['width'], first['height']], [800, 600]);
      socket.incoming.add(
        jsonEncode({
          ...first,
          'type': 'stream.resizing',
          'resizeId': 'window-resize-1',
          'epoch': 1,
          'timeoutMs': 90000,
        }),
      );
      await tester.pump();
      tester.view.physicalSize = const Size(1000, 700);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 700));
      tester.view.physicalSize = const Size(800, 600);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 700));
      expect(requests().length, 1);
      socket.incoming.add(
        jsonEncode({
          ...startInfo,
          'sessionId': 'resize-session',
          'requestId': first['requestId'],
          'resizeId': 'window-resize-1',
          'epoch': 1,
          'width': 800,
          'height': 600,
          'sourceWidth': 800,
          'sourceHeight': 600,
        }),
      );
      await tester.pump();
      socket.incoming.add(packet()..[2] = 1);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 800));
      expect(requests().length, 1);
      expect(media.dimensions.last, [800, 600]);
      tester.view.physicalSize = const Size(1100, 700);
      await tester.pump();
      // Still within the settle (RemoteView.viewportSettle, 250 ms).
      await tester.pump(const Duration(milliseconds: 200));
      expect(requests().length, 1);
      // Too small to resize (a window shrunk below 320x180); larger than
      // 3840x2160 is now clamped to the largest supported shape instead.
      tester.view.physicalSize = const Size(300, 170);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 700));
      expect(
        requests().length,
        1,
        reason: 'An unsupported viewport cancels an older queued size instead of changing to stale geometry.',
      );
      tester.view.physicalSize = const Size(1200, 800);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 700));
      await tester.pump();
      expect(requests().length, 2);
      expect(
        [requests().last['width'], requests().last['height']],
        [1200, 800],
      );
      await tester.pumpWidget(const SizedBox());
      unawaited(session.close());
      await tester.pump(const Duration(seconds: 3));
      await tester.pump();
      expect(session.closed, isTrue);
      expect(media.stopped, isTrue);
      unawaited(media.events.close());
    },
  );
}

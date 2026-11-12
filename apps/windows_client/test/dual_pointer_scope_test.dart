// Actual RemoteView pointer/metrics orchestration with in-memory media/socket.
// No native window, physical monitor, SendInput or VM acceptance is claimed.
import 'dart:convert';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'remote_session_test.dart' show SessionSocket;
import 'stream_display_entry_test.dart' show entryMachine;
import 'track_stream_test.dart' as track_fixture;

Map<String, Object?> _layout() => {
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

void main() {
  testWidgets(
    'DPI change withdraws queued movement and an old surface Down cannot reuse the last source point',
    (t) async {
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
        if (call.method == 'monitorLayout') return _layout();
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
      var elapsed = Duration.zero;
      try {
        await t.pumpWidget(
          MaterialApp(
            home: RemoteView(
              machine: entryMachine(2),
              initialDisplayMode: SessionDisplayMode.allMonitors,
              load: () async => session,
              onConnectTracks: (_, _) async => session,
              pointerElapsed: () => elapsed,
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
        expect(
          session.canResizeTracks,
          isFalse,
          reason: 'ordinary tracks have no in-place resize capability',
        );
        final surface = find.byKey(const Key('local-output-0'));
        expect(surface, findsOneWidget);
        final oldPoint = t.getCenter(surface);
        final oldListener = t.widget<Listener>(
          find.descendant(of: surface, matching: find.byType(Listener)).first,
        );
        final mouse = await t.createGesture(kind: PointerDeviceKind.mouse);
        await mouse.addPointer(location: oldPoint);
        await mouse.down(oldPoint);
        await t.pump();
        await t.pump();
        expect(session.captured, isTrue);
        expect(
          socket.sent.where(
            (e) =>
                e['type'] == 'input' &&
                e['event'] == 'button' &&
                e['down'] == true,
          ),
          hasLength(1),
        );
        await mouse.up();
        await t.pump();
        socket.sent.clear();
        await mouse.moveTo(oldPoint + const Offset(20, 0));
        expect(
          socket.sent.where((e) => e['type'] == 'input'),
          isEmpty,
          reason: 'movement is still coalesced before metrics change',
        );
        // This is a different logical viewport over the same physical window.
        // No 650 ms re-observation is allowed to retain the old pointer lease.
        t.view.devicePixelRatio = 1.5;
        expect(
          session.captured,
          isFalse,
          reason: 'metrics must withdraw ownership before the next frame',
        );
        // A held Flutter hit-test route may still call the old surface closure.
        // It must not click the former source point or revive queued movement.
        oldListener.onPointerDown!(
          PointerDownEvent(
            position: oldPoint,
            kind: PointerDeviceKind.mouse,
            buttons: kPrimaryMouseButton,
          ),
        );
        await t.pump();
        await t.pump();
        elapsed += const Duration(milliseconds: 16);
        await t.pump(const Duration(milliseconds: 16));
        expect(session.captured, isFalse);
        expect(socket.sent.where((e) => e['type'] == 'input'), isEmpty);
        expect(
          socket.sent.where((e) => e['type'] == 'input.reset'),
          isNotEmpty,
        );
        expect(
          find.text('모니터 구성이 변경됐어요. 연결을 끊은 뒤 화면 모드를 다시 선택하세요.'),
          findsOneWidget,
        );
        // Still connected (the card's headline must not read
        // "연결돼 있지 않아요").
        expect(find.text('화면을 표시할 수 없어요'), findsOneWidget);
        expect(find.text('연결돼 있지 않아요'), findsNothing);
        expect(find.text('연결 안 됨'), findsNothing);
        await mouse.removePointer();
      } finally {
        await t.pumpWidget(const SizedBox.shrink());
        final closing = session.close();
        await t.pump(const Duration(seconds: 3));
        await t.pump();
        await closing;
        await media.events.close();
      }
    },
  );
}

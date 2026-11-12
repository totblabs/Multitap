// Real RemoteView orchestration, injected window RPC, socket and media only.
// No physical monitors, OS input, clipboard, product process or broker network.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/screen_transition_progress.dart';

import 'remote_session_test.dart' as single;
import 'stream_display_entry_test.dart' show entryMachine;
import 'stream_display_mode_transition_test.dart' as mode;
import 'track_stream_test.dart' as tracks;

Future<void> settleSteps(WidgetTester tester, [int count = 8]) async {
  for (var i = 0; i < count; i++) {
    await tester.pump();
  }
}

Map<String, Object?> dualLayout() => {
  'x': 0,
  'y': 0,
  'width': 1920,
  'height': 720,
  'monitors': [
    {
      'id': '0', 'x': 0, 'y': 0, 'width': 640, 'height': 360,
      'primary': true, 'dpi': 96,
    },
    {
      'id': '1', 'x': 640, 'y': 0, 'width': 1280, 'height': 720,
      'primary': false, 'dpi': 96,
    },
  ],
};

class DualTransitionFixture {
  final socket = single.SessionSocket(), media = tracks.TrackMedia();
  final calls = <String>[];
  late final session = RemoteSession(
    connection: tracks.connection(),
    token: 'fixture-only-token',
    media: media,
    dialer: (_, _) async => socket,
  );

  Future<void> mount(
    WidgetTester tester, {
    required Future<RemoteSession> Function() reconnect,
    required Future<DesktopMachine> Function(int) configure,
  }) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1920, 720);
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.view.resetPhysicalSize);
    await tester.pumpWidget(MaterialApp(
      home: RemoteView(
        machine: entryMachine(2),
        initialDisplayMode: SessionDisplayMode.allMonitors,
        prepareDisplaysOnEntry: true,
        load: () async => session,
        onConnectTracks: (_, _) async => session,
        onReconnect: (_) => reconnect(),
        onConfigureDisplays: configure,
      ),
    ));
    await settleSteps(tester);
    socket.incoming.add(jsonEncode(tracks.info()));
    await settleSteps(tester);
    socket.incoming.add(tracks.packet(0));
    socket.incoming.add(tracks.packet(1));
    media.currentStats = tracks.trackStats();
    await tester.pump(const Duration(seconds: 1));
    await settleSteps(tester);
    expect(session.frameReady, isTrue, reason: session.error);
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  const window = MethodChannel('multitap/window');
  const events = MethodChannel('multitap/window/events');
  setUp(() => messenger.setMockMethodCallHandler(events, (_) async => null));
  tearDown(() {
    messenger.setMockMethodCallHandler(window, null);
    messenger.setMockMethodCallHandler(events, null);
  });

  testWidgets('Preflight progress is immediate and cancellation cannot retire the old stream', (tester) async {
    final fixture = mode.ModeFixture();
    Completer<Map<String, Object?>>? pending;
    messenger.setMockMethodCallHandler(window, (call) async {
      fixture.calls.add(call.method);
      if (call.method == 'monitorLayout') return pending?.future ?? mode.layout(2);
      return call.method == 'allMonitors';
    });
    await fixture.mount(tester);
    await mode.openScreenMenu(tester);
    await tester.pumpAndSettle();
    pending = Completer<Map<String, Object?>>();
    fixture.calls.clear();
    await tester.tap(find.byKey(const Key('stream-all-monitors')));
    await tester.pump();
    expect(find.byKey(const Key('screen-transition-overlay')), findsOneWidget);
    expect(find.text('PC의 모니터 확인 중'), findsOneWidget);
    expect(tester.widget<LinearProgressIndicator>(find.byKey(const Key('screen-transition-progress'))).value, isNull);
    expect(fixture.socket.wasClosed, isFalse);
    await tester.tap(find.byKey(const Key('screen-transition-cancel')));
    await tester.pump();
    expect(find.text('전환 취소 중'), findsOneWidget);
    expect(fixture.socket.wasClosed, isFalse);
    pending.complete(mode.layout(2));
    await settleSteps(tester);
    expect(find.byKey(const Key('screen-transition-overlay')), findsNothing);
    expect(fixture.calls, ['monitorLayout']);
    expect(fixture.socket.wasClosed, isFalse);
    expect(fixture.session.frameReady, isTrue);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets('Cancelling an in-flight native mode waits for its reply then restores before release', (tester) async {
    final fixture = mode.ModeFixture();
    final applied = Completer<bool>();
    messenger.setMockMethodCallHandler(window, (call) async {
      fixture.calls.add(call.method);
      if (call.method == 'monitorLayout') return mode.layout(2);
      if (call.method == 'allMonitors') return applied.future;
      return false;
    });
    await fixture.mount(tester);
    await mode.openScreenMenu(tester);
    await tester.pumpAndSettle();
    fixture.calls.clear();
    await tester.tap(find.byKey(const Key('stream-all-monitors')));
    await settleSteps(tester);
    expect(fixture.calls, ['monitorLayout', 'allMonitors']);
    await tester.tap(find.byKey(const Key('screen-transition-cancel')));
    await tester.pump();
    expect(fixture.socket.wasClosed, isFalse);
    expect(find.byKey(const Key('screen-transition-overlay')), findsOneWidget);
    applied.complete(true);
    await settleSteps(tester);
    expect(fixture.calls, ['monitorLayout', 'allMonitors', 'fullscreen']);
    expect(fixture.socket.wasClosed, isFalse);
    expect(find.byKey(const Key('screen-transition-overlay')), findsNothing);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets('Dual to single native refusal keeps the exact dual transport alive', (tester) async {
    final fixture = DualTransitionFixture();
    var refuse = false, reconnects = 0, counts = 0;
    messenger.setMockMethodCallHandler(window, (call) async {
      fixture.calls.add(call.method);
      if (call.method == 'monitorLayout') return dualLayout();
      if (call.method == 'fullscreen' && refuse) {
        throw PlatformException(code: 'presentation_failed');
      }
      return call.method == 'allMonitors';
    });
    await fixture.mount(tester,
      reconnect: () async { reconnects++; throw const BrokerException('network_error'); },
      configure: (count) async { counts++; return entryMachine(count); },
    );
    refuse = true;
    fixture.calls.clear();
    await mode.openScreenMenu(tester);
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('stream-windowed')));
    await settleSteps(tester);
    expect(fixture.calls, containsAllInOrder(['fullscreen', 'allMonitors']));
    expect(fixture.socket.wasClosed, isFalse);
    expect(fixture.media.stopped, isFalse);
    expect(reconnects, 0);
    expect(counts, 0);
    expect(find.byKey(const Key('screen-transition-overlay')), findsNothing);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets('Dual to single waits for output verification and a fresh first picture', (tester) async {
    final fixture = DualTransitionFixture();
    final configured = Completer<DesktopMachine>();
    final nextSocket = single.SessionSocket(), nextMedia = single.SessionMedia();
    final next = RemoteSession(
      connection: BrokerConnection.fromJson({
        'sessionId': 'next-single',
        'webSocketUrl': 'wss://fixture.invalid/api/v1/sessions/next-single',
      }, Uri.parse('https://fixture.invalid')),
      token: 'fixture-only-token', media: nextMedia,
      dialer: (_, _) async => nextSocket,
    );
    var reconnects = 0;
    messenger.setMockMethodCallHandler(window, (call) async {
      if (call.method == 'monitorLayout') return dualLayout();
      if (call.method == 'fullscreen') {
        expect(fixture.socket.wasClosed, isFalse);
      }
      return call.method == 'allMonitors';
    });
    await fixture.mount(tester,
      reconnect: () async { reconnects++; return next; },
      configure: (count) async { expect(count, 1); return configured.future; },
    );
    await mode.openScreenMenu(tester);
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('stream-windowed')));
    await settleSteps(tester);
    expect(fixture.socket.wasClosed, isTrue);
    expect(reconnects, 0);
    expect(find.text('원격 모니터 구성 중'), findsOneWidget);
    configured.complete(entryMachine(1));
    await settleSteps(tester);
    expect(reconnects, 1);
    expect(find.text('첫 화면 기다리는 중'), findsOneWidget);
    expect(next.frameReady, isFalse);
    expect(nextMedia.captures.where((value) => value), isEmpty);
    nextSocket.incoming.add(jsonEncode(single.info));
    nextSocket.incoming.add(single.packet());
    await tester.pump(const Duration(seconds: 1));
    await settleSteps(tester);
    expect(next.frameReady, isTrue, reason: next.error);
    expect(find.byKey(const Key('screen-transition-overlay')), findsNothing);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets('Transition controls remain readable with large text and no percentage', (tester) async {
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.view.resetPhysicalSize);
    for (final brightness in Brightness.values) {
      tester.view.physicalSize = const Size(480, 400);
      tester.view.devicePixelRatio = 1;
      await tester.pumpWidget(MaterialApp(
        theme: ThemeData(brightness: brightness),
        home: MediaQuery(
          data: const MediaQueryData(size: Size(480, 400), textScaler: TextScaler.linear(2)),
          child: ScreenTransitionProgress(
            target: '모든 모니터', stage: ScreenTransitionStage.hostReady,
            onCancel: () {}, onClose: () {},
          ),
        ),
      ));
      await tester.pump();
      expect(tester.takeException(), isNull);
      expect(find.text('Host 화면 준비 기다리는 중'), findsOneWidget);
      expect(tester.getSize(find.byKey(const Key('screen-transition-cancel'))).height, greaterThanOrEqualTo(44));
      expect(tester.widget<LinearProgressIndicator>(find.byKey(const Key('screen-transition-progress'))).value, isNull);
    }
    tester.view.resetDevicePixelRatio();
    tester.view.resetPhysicalSize();
    await tester.pumpWidget(const SizedBox.shrink());
  });
}

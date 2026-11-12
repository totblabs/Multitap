// There is no 마우스 가두기: 마우스 잠금 is the one mouse lock. A session window
// on a PC where an earlier client saved "confine on" never clips the cursor,
// offers no confine entry and drops the saved choice. Media and socket are
// fixtures; the native pointer channel only records.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionSocket, SessionMedia, packet;
import 'viewport_session_test.dart' show startInfo;

/// What earlier clients clipped the cursor through.
const _pointer = MethodChannel('multitap/pointer');

void main() {
  testWidgets('An old saved "confine on" is ignored and dropped, and the bar '
      'has no confine entry', (tester) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1280, 800);
    addTearDown(tester.view.reset);
    final directory = Directory.systemTemp.createTempSync('multitap-confine-');
    addTearDown(() => directory.deleteSync(recursive: true));
    final launcher = File('${directory.path}/launcher.json')
      ..writeAsStringSync('{}');
    final saved = File('${directory.path}/session.json')
      ..writeAsStringSync(jsonEncode({'confinePointer': true}));
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final pointerCalls = <String>[];
    final handlers = <MethodChannel, Future<Object?> Function(MethodCall)>{
      const MethodChannel('multitap/window'): (_) async => false,
      const MethodChannel('multitap/window/events'): (_) async => null,
      const MethodChannel('multitap/relative-mouse'): (_) async => null,
      _pointer: (call) async {
        pointerCalls.add(call.method);
        return call.method == 'confine' ? true : null;
      },
    };
    for (final MapEntry(key: channel, value: handler) in handlers.entries) {
      messenger.setMockMethodCallHandler(channel, handler);
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    }
    final socket = SessionSocket();
    final media = SessionMedia();
    final session = RemoteSession(
      connection: BrokerConnection(
        sessionId: 'confine-removed',
        webSocketUrl: Uri.parse(
          'wss://fixture.invalid/api/v1/sessions/confine-removed',
        ),
      ),
      token: 'fixture-only-token',
      media: media,
      dialer: (_, _) async => socket,
    );
    await tester.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: machine(1),
          load: () async => session,
          windowActions: SessionWindowActions(closeWindow: () async {}),
          pointerMemory: FileLauncherMemory(launcher),
        ),
      ),
    );
    await tester.pump();
    await tester.pump();
    socket.incoming.add(jsonEncode(startInfo));
    await tester.pump();
    await tester.pump();
    socket.incoming.add(packet());
    await tester.pump();
    await tester.pump();
    expect(session.frameReady, isTrue);
    try {
      // The real file work finishes outside the fake clock, then the frames
      // after it run (an earlier client read the choice here and clipped).
      for (var i = 0; i < 20 && saved.existsSync(); i++) {
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: 50)),
        );
        await tester.pump();
      }
      await tester.pump(const Duration(milliseconds: 100));
      await tester.pump();
      expect(pointerCalls, isNot(contains('confine')), reason: 'no clip');
      expect(saved.existsSync(), isFalse, reason: 'the old choice is dropped');
      expect(launcher.readAsStringSync(), '{}');
      expect(find.textContaining('가두기'), findsNothing, reason: 'no hint');
      await tester.tap(find.byKey(const Key('expand-session-toolbar')));
      await tester.pump();
      expect(find.byKey(const Key('session-command-bar')), findsOneWidget);
      expect(find.byKey(const Key('session-pointer-confine')), findsNothing);
      expect(
        find.byWidgetPredicate(
          (widget) =>
              widget is Tooltip && (widget.message ?? '').contains('가두기'),
        ),
        findsNothing,
      );
      // Nor in the bar's 설정 panel, where the mouse lock's switch is now.
      await tester.tap(find.byKey(const Key('remote-settings')));
      await tester.pump();
      await tester.pump();
      expect(find.byKey(const Key('session-settings-panel')), findsOneWidget);
      expect(find.byKey(const Key('session-pointer-confine')), findsNothing);
      expect(find.textContaining('가두기'), findsNothing, reason: 'no entry');
      expect(pointerCalls, isEmpty, reason: 'nothing uses the old channel');
    } finally {
      await tester.pumpWidget(const SizedBox.shrink());
      unawaited(session.close());
      await tester.pump(const Duration(seconds: 3));
      await tester.pump();
      unawaited(media.events.close());
    }
  });
}

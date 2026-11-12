import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/session_failure.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'session_recovery_test.dart' show machine;
import 'stream_display_mode_transition_test.dart' show openScreenMenu;

void main() {
  test('Unsupported dual request names the supported modes without retrying', () {
    final failure = SessionFailure.request(
      const BrokerException('track_streaming_unavailable'),
    );
    expect(failure.code, 'track_streaming_unavailable');
    expect(
      failure.message,
      '모든 모니터로 연결하지 못했어요.\n목록에서 창 모드나 전체 화면으로 연결하세요.',
    );
    expect(failure.canRetry, isFalse);
    expect(failure.takeover, isFalse);
  });

  test('Authentication and assignment failures take priority over dual guidance', () {
    for (final status in [401, 403, 404]) {
      final failure = SessionFailure.request(
        BrokerException('track_streaming_unavailable', status),
      );
      expect(failure.code, status == 401 ? 'authorization_revoked' : 'desktop_not_found');
      expect(failure.canRetry, isFalse);
      expect(failure.takeover, isFalse);
      expect(failure.message, isNot(contains('창 모드 또는 전체 화면')));
    }
  });

  testWidgets('Unsupported initial dual can explicitly enter either supported window mode', (tester) async {
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    const window = MethodChannel('multitap/window');
    const events = MethodChannel('multitap/window/events');
    messenger.setMockMethodCallHandler(events, (_) async => null);
    addTearDown(() {
      messenger.setMockMethodCallHandler(window, null);
      messenger.setMockMethodCallHandler(events, null);
    });
    for (final fullscreen in [false, true]) {
      final calls = <String>[];
      var connects = 0;
      messenger.setMockMethodCallHandler(window, (call) async {
        calls.add(call.method);
        if (call.method == 'fullscreen') {
          expect(call.arguments, fullscreen);
          return fullscreen;
        }
        return null;
      });
      await tester.pumpWidget(MaterialApp(
        home: RemoteView(
          machine: machine,
          initialDisplayMode: SessionDisplayMode.allMonitors,
          load: () async {
            expect(calls, contains('fullscreen'));
            connects++;
            throw const BrokerException('network_error');
          },
        ),
      ));
      await tester.pumpAndSettle();
      expect(calls, isEmpty);
      expect(connects, 0);
      expect(find.textContaining('모든 모니터로 연결하지 못했어요.'), findsOneWidget);
      expect(find.byKey(const Key('retry-remote-connection')), findsNothing);
      await openScreenMenu(tester);
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(Key(fullscreen ? 'stream-fullscreen' : 'stream-windowed')));
      for (var i = 0; i < 8; i++) {
        await tester.pump();
      }
      expect(calls.where((call) => call == 'fullscreen'), hasLength(1));
      expect(calls, isNot(contains('allMonitors')));
      expect(connects, 1);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pump();
    }
  });
}

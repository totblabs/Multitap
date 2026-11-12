// Real RemoteView + RemoteSession with in-memory relay/decoder only.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_change_robustness_test.dart' show mockWindow;
import 'remote_session_test.dart' show SessionSocket;
import 'signin_launcher_entry_test.dart' show machine;
import 'signin_stream_contract_test.dart'
    show preparePhase, signinBindingJson, signinSessionId;
import 'signin_stream_session_test.dart'
    show SigninMedia, phaseInfo, signinFrame;

Future<void> until(WidgetTester tester, bool Function() ready) async {
  for (var n = 0; n < 50 && !ready(); n++) {
    await tester.pump(const Duration(milliseconds: 20));
  }
  expect(ready(), isTrue);
}

void main() {
  testWidgets(
    'decoded login-to-desktop promotes once with original audio and bitrate intent',
    (tester) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(960, 640);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(tester.view.resetPhysicalSize);
      await mockWindow(tester);
      const wanted = StreamOptions(
        audio: true,
        bitrateMode: StreamBitrateMode.auto,
        bitrate: 18000000,
      );
      final socket = SessionSocket(), media = SigninMedia();
      final signin = RemoteSession(
        connection: BrokerConnection(
          sessionId: signinSessionId,
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/sessions/$signinSessionId',
          ),
          epochMedia: true,
          signin: SigninBinding.parse(signinBindingJson()),
          options: const StreamOptions(
            audio: false,
            bitrateMode: StreamBitrateMode.fixed,
          ),
        ),
        token: 'synthetic',
        dialer: (_, _) async => socket,
        media: media,
      );
      final nextSocket = SessionSocket(), nextMedia = SigninMedia();
      final ordinary = RemoteSession(
        connection: BrokerConnection(
          sessionId: 'ordinary-fixture',
          webSocketUrl: Uri.parse(
            'wss://fixture.invalid/sessions/ordinary-fixture',
          ),
          epochMedia: true,
          options: wanted,
        ),
        token: 'synthetic',
        dialer: (_, _) async => nextSocket,
        media: nextMedia,
      );
      final requests = <StreamOptions>[];
      var ordinaryReady = false, reads = 0;
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox.shrink());
        unawaited(signin.close());
        unawaited(ordinary.close());
        await tester.pump(const Duration(seconds: 3));
        unawaited(media.events.close());
        unawaited(nextMedia.events.close());
      });
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine(),
            initialOptions: wanted,
            initialDisplayMode: SessionDisplayMode.windowed,
            sessionGuideEnabled: false,
            load: () async => signin,
            onReconnect: (options) async {
              requests.add(options);
              return ordinary;
            },
            onRefreshDesktop: () async {
              reads++;
              return machine(ordinary: ordinaryReady);
            },
          ),
        ),
      );
      await until(tester, () => socket.incoming.hasListener);
      final initialReads = reads;
      socket.incoming.add(jsonEncode(phaseInfo()));
      await until(tester, () => media.calls.contains('start'));
      socket.incoming.add(signinFrame());
      await tester.pump();
      media.decoded = 1;
      await until(tester, () => signin.frameReady);
      expect(requests, isEmpty);
      expect(reads, initialReads);
      socket.incoming.add(jsonEncode(preparePhase()));
      await until(tester, () => !signin.frameReady);
      socket.incoming.add(
        jsonEncode(phaseInfo(generation: 2, epoch: 2, phase: 'desktop')),
      );
      await tester.pump();
      await tester.pump();
      expect(requests, isEmpty);
      socket.incoming.add(signinFrame(2));
      await tester.pump();
      media.decoded = 1;
      await until(tester, () => signin.signinDesktopReady);
      expect(
        requests,
        isEmpty,
        reason: 'Decoded scoped desktop alone is not ordinary Host readiness.',
      );
      ordinaryReady = true;
      await until(tester, () => requests.isNotEmpty);
      expect(requests, hasLength(1));
      expect(requests.single.audio, isTrue);
      expect(requests.single.bitrateMode, StreamBitrateMode.auto);
      expect(requests.single.bitrate, 18000000);
      expect(signin.closed, isTrue);
      await tester.pump(const Duration(milliseconds: 600));
      expect(
        requests,
        hasLength(1),
        reason: 'The retired decoder cannot start a second promotion.',
      );
      expect(tester.takeException(), isNull);
    },
  );
}

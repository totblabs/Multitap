import '../helpers/stream_connect_consent.dart' show acceptStreamConnect;
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/track_stream.dart';
import 'package:multitap_client/models/stream_options.dart';

import '../remote_session_test.dart' show SessionSocket;
import '../track_stream_test.dart' as tracks;
import 'android_large_desktop_test.dart' show openLargeDesktopScreenPanel;
import 'android_launcher_test.dart' show FakeBroker, MemoryStore;
import 'android_session_contract_test.dart' show AndroidNativeFake;
import 'android_stream_preparation_test.dart' show desktop;

class ProfileAdvertisedBroker extends FakeBroker
    implements ProfileTrackBrokerApi {
  ProfileAdvertisedBroker() : super([desktop(profileTracks: true)]);
  int profileRequests = 0;
  @override
  Future<BrokerConnection> connectProfileTracks(
    String token,
    String desktopId,
    TrackPlan plan,
    StreamOptions options, {
    int? displayScale,
  }) async {
    profileRequests++;
    throw StateError('Normal Android admission must not create two outputs');
  }
}

void main() {
  setUp(() {
    AndroidNativeFake().install();
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    for (final channel in [
      const MethodChannel('multitap/pen-input'),
      AndroidSessionKeepalive.channel,
    ]) {
      messenger.setMockMethodCallHandler(channel, (_) async => null);
    }
  });
  tearDown(() {
    AndroidNativeFake.uninstall();
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    for (final channel in [
      const MethodChannel('multitap/pen-input'),
      AndroidSessionKeepalive.channel,
      AndroidStore.channel,
    ]) {
      messenger.setMockMethodCallHandler(channel, null);
    }
  });
  testWidgets(
    'actual AndroidApp never enables output creation even when Host and API advertise profile tracks',
    (t) async {
      t.view.physicalSize = const Size(1080, 2400);
      t.view.devicePixelRatio = 3;
      addTearDown(t.view.reset);
      MemoryStore(
        origin: 'https://vdi.example',
        username: 'tester',
        prefs: {'launcher.view': 'cards'},
      ).install();
      final broker = ProfileAdvertisedBroker();
      try {
        await t.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
        await t.pumpAndSettle();
        await t.enterText(
          find.byKey(const Key('sign-in-password')),
          'fixture-password',
        );
        await t.tap(find.byKey(const Key('login-submit')));
        await t.pumpAndSettle();
        await t.ensureVisible(find.byKey(const Key('play-pve-149')));
        await t.tap(find.byKey(const Key('play-pve-149')));
        await acceptStreamConnect(t, 'pve-149');
        await t.pumpAndSettle();
        expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
        final remote = t.widget<AndroidRemoteView>(
          find.byType(AndroidRemoteView),
        );
        expect(remote.dualOutputs, isFalse);
        expect(find.byKey(const Key('android-dual-output')), findsNothing);
        expect(broker.profileRequests, 0);
      } finally {
        await t.pumpWidget(const SizedBox.shrink());
        await t.pump(const Duration(seconds: 3));
      }
    },
  );
  testWidgets(
    'normal AndroidRemote sanitizes legacy monitor all and keeps old received-track selection without creation controls',
    (t) async {
      t.view.physicalSize = const Size(915, 412);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      final socket = SessionSocket(),
          media = tracks.TrackMedia()..currentStats = tracks.trackStats();
      final requests = <StreamOptions>[];
      late RemoteSession session;
      try {
        await t.pumpWidget(
          MaterialApp(
            home: AndroidRemoteView(
              name: 'Existing track compatibility',
              token: 'fixture',
              options: tracks.options,
              density: () async => null,
              settings: const AndroidSessionSettings(
                helpSeen: true,
                touch: AndroidTouchMode.direct,
              ),
              load: (options, _, _) async {
                requests.add(options);
                return tracks.connection();
              },
              sessionFactory: (connection, _, _) => session = RemoteSession(
                connection: connection,
                token: 'fixture',
                media: media,
                dialer: (_, _) async => socket,
              ),
              pictureBuilder: (_) => const ColoredBox(color: Color(0xff164253)),
            ),
          ),
        );
        for (var n = 0; n < 6; n++) {
          await t.pump(const Duration(milliseconds: 20));
        }
        expect(
          tracks.options.monitor,
          'all',
          reason: 'Input actually carries the legacy dual option',
        );
        expect(requests, hasLength(1));
        expect(requests.single.monitor, '0');
        socket.incoming.add(jsonEncode(tracks.info()));
        await t.pump(const Duration(milliseconds: 20));
        socket.incoming.add(tracks.packet(0));
        socket.incoming.add(tracks.packet(1));
        await t.pump(const Duration(milliseconds: 1100));
        await t.pump();
        expect(session.frameReady, isTrue);
        await openLargeDesktopScreenPanel(t);
        expect(find.byKey(const Key('android-dual-output')), findsNothing);
        expect(find.byKey(const Key('android-single-output')), findsNothing);
        expect(find.byKey(const Key('android-output-0')), findsOneWidget);
        expect(find.byKey(const Key('android-output-1')), findsOneWidget);
        await t.ensureVisible(find.byKey(const Key('android-output-0')));
        await t.tap(find.byKey(const Key('android-output-0')));
        await t.pumpAndSettle();
        expect(
          requests,
          hasLength(1),
          reason: 'Selecting an already received track does not create/reconnect displays',
        );
        expect(t.takeException(), isNull);
      } finally {
        await t.pumpWidget(const SizedBox.shrink());
        await t.pump(const Duration(seconds: 3));
        await media.events.close();
      }
    },
  );
}

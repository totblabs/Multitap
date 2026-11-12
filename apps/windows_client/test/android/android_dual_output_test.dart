import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/ui/app_theme.dart';

import '../remote_session_test.dart' show SessionSocket;
import '../track_stream_test.dart' as f;
import 'android_large_desktop_test.dart' show openLargeDesktopScreenPanel;
import 'android_session_contract_test.dart' show AndroidNativeFake;

void main() {
  testWidgets(
    'Android selects each independently decoded output and targets absolute input there',
    (t) async {
      t.view.devicePixelRatio = 1;
      t.view.physicalSize = const Size(915, 412);
      addTearDown(t.view.reset);
      AndroidNativeFake().install();
      addTearDown(AndroidNativeFake.uninstall);
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      const pen = MethodChannel('multitap/pen-input');
      for (final channel in [
        pen,
        AndroidSessionKeepalive.channel,
        AndroidStore.channel,
      ]) {
        messenger.setMockMethodCallHandler(channel, (_) async => null);
      }
      addTearDown(() {
        for (final channel in [
          pen,
          AndroidSessionKeepalive.channel,
          AndroidStore.channel,
        ]) {
          messenger.setMockMethodCallHandler(channel, null);
        }
      });
      final socket = SessionSocket(),
          media = f.TrackMedia()..currentStats = f.trackStats();
      late RemoteSession session;
      await t.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: AndroidRemoteView(
            name: 'Dual fixture',
            token: 'qa',
            options: f.options,
            dualOutputs: true,
            density: () async => null,
            settings: const AndroidSessionSettings(
              helpSeen: true,
              touch: AndroidTouchMode.direct,
            ),
            load: (_, _, _) async => f.connection(),
            sessionFactory: (connection, _, _) => session = RemoteSession(
              connection: connection,
              token: 'qa',
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
      socket.incoming.add(jsonEncode(f.info()));
      await t.pump(const Duration(milliseconds: 20));
      socket.incoming.add(f.packet(0));
      socket.incoming.add(f.packet(1));
      await t.pump(const Duration(milliseconds: 1100));
      await t.pump();
      expect(session.frameReady, true);
      await openLargeDesktopScreenPanel(t);
      expect(find.byKey(const Key('android-dual-output')), findsOneWidget);
      expect(find.byKey(const Key('android-output-0')), findsOneWidget);
      expect(find.byKey(const Key('android-output-1')), findsOneWidget);
      await t.tap(find.byKey(const Key('android-output-0')));
      await t.pumpAndSettle();
      expect(t.takeException(), isNull);
      final surface = find.byKey(const Key('android-remote-surface'));
      await t.tapAt(t.getRect(surface).centerLeft + const Offset(180, 0));
      await t.pump(const Duration(milliseconds: 500));
      final input = socket.sent
          .where((event) => event['type'] == 'input' && event.containsKey('x'))
          .toList();
      expect(input, isNotEmpty);
      expect(
        input.every(
          (event) =>
              event['track'] == '0' &&
              event['epoch'] == 1 &&
              event['topologyEpoch'] == 1,
        ),
        true,
      );
      expect(
        media.starts,
        1,
        reason: 'view selection keeps both decoder lanes alive',
      );
      await t.pumpWidget(const SizedBox.shrink());
      await t.pump(const Duration(seconds: 1));
      await session.close();
      await media.events.close();
    },
  );
}

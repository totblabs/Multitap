import '../helpers/stream_connect_consent.dart' show acceptStreamConnect;
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_remote.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/models/stream_options.dart';

import 'android_launcher_test.dart'
    show FakeBroker, MemoryStore, vm, mainButton;

Future<void> _tap(WidgetTester t, Finder finder) async {
  await t.ensureVisible(finder);
  await t.tap(finder);
  await t.pumpAndSettle();
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUp(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          AndroidSessionKeepalive.channel,
          (call) async => call.method == 'start' ? true : null,
        );
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(
          const MethodChannel('multitap/pen-input'),
          (_) async => null,
        );
  });
  tearDown(() {
    for (final channel in [
      AndroidStore.channel,
      AndroidSessionKeepalive.channel,
      const MethodChannel('multitap/pen-input'),
    ]) {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null);
    }
  });
  for (final dialog in [false, true]) {
    testWidgets(
      '${dialog ? 'quality dialog' : 'quick connect'} retires saved audio false and preserves custom frame rate and bitrate',
      (t) async {
        t.view.physicalSize = const Size(360, 800);
        t.view.devicePixelRatio = 1;
        addTearDown(t.view.reset);
        final store = MemoryStore(
          origin: 'https://vdi.example',
          username: 'tester',
          prefs: {'stream.quality': '{"fps":30,"bitrate":100,"audio":false}'},
        );
        store.install();
        final broker = FakeBroker([vm()]);
        await t.pumpWidget(AndroidVdiApp(apiFactory: (_) => broker));
        await t.pumpAndSettle();
        await t.enterText(
          find.byKey(const Key('sign-in-password')),
          'synthetic',
        );
        await _tap(t, find.byKey(const Key('login-submit')));
        if (dialog) {
          await _tap(t, find.byKey(const Key('stream-modes-pve-149')));
          await _tap(t, find.byKey(const Key('stream-settings-pve-149')));
          expect(find.byKey(const Key('android-c-stream-audio')), findsNothing);
          expect(find.text('컴퓨터 소리 재생'), findsNothing);
          expect(
            t
                .widget<Text>(find.byKey(const Key('android-c-stream-summary')))
                .data,
            contains('30 fps · 100 Mbps'),
          );
          await _tap(t, find.widgetWithText(FilledButton, '연결'));
          await acceptStreamConnect(t, 'pve-149');
        } else {
          await _tap(t, mainButton('pve-149'));
          await acceptStreamConnect(t, 'pve-149');
          expect(find.textContaining('소리 끔'), findsNothing);
          expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
        }
        final remote = t.widget<AndroidRemoteView>(
          find.byType(AndroidRemoteView),
        );
        expect(remote.options.audio, true);
        expect(remote.options.fps, 30);
        expect(remote.options.bitrate, 100000000);
        expect(remote.options.automaticBitrate, false);
        expect(remote.options.monitor, '0');
        expect(remote.options.maxWidth, StreamOptions.codecMaxWidth);
        expect(remote.options.maxHeight, StreamOptions.codecMaxHeight);
        remote.onOptions!(
          const StreamOptions(fps: 60, bitrate: 80000000, audio: false),
        );
        await t.pumpAndSettle();
        final saved = jsonDecode(store.prefs['stream.quality']!) as Map;
        expect(
          saved['audio'],
          true,
          reason: 'A legacy session callback must not restore hidden mute.',
        );
        expect(saved['fps'], 60);
        expect(saved['bitrate'], 80);
        expect(
          broker.calls.where((call) => call.startsWith('power:')),
          isEmpty,
        );
        expect(t.takeException(), isNull);
        await t.pumpWidget(const SizedBox.shrink());
        await t.pumpAndSettle();
      },
    );
  }
}

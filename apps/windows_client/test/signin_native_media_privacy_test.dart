import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'signin_stream_session_test.dart' show phaseConnection;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('multitap/remote');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final privacyFailure = isA<PlatformException>().having(
    (e) => e.code,
    'code',
    'thumbnail_privacy_unknown',
  );
  for (final target in ['start', 'restart', 'holding', 'tracks']) {
    test(
      'Private $target payload requires a durable same-lease native privacy latch',
      () async {
        final calls = <MethodCall>[];
        messenger.setMockMethodCallHandler(channel, (call) async {
          calls.add(call);
          return switch (call.method) {
            'start' => 91,
            'startTracks' => {'0': 91},
            'thumbnailPrivacy' => 7,
            'thumbnailPrivacyStatus' || 'thumbnailPrivacyLocked' => true,
            _ => null,
          };
        });
        addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
        final media = NativeRemoteMedia(thumbnailAllowed: false);
        switch (target) {
          case 'start':
            await media.start(1280, 720);
          case 'restart':
            await media.restart(1280, 720);
          case 'holding':
            await media.startHolding(1280, 720);
          case 'tracks':
            await media.startTracks({'tracks': []});
        }
        final start = calls.first;
        expect((start.arguments as Map)['thumbnailAllowed'], isFalse);
        expect(calls.map((c) => c.method), [
          start.method,
          'thumbnailPrivacy',
          'thumbnailPrivacyStatus',
          'thumbnailPrivacyLocked',
        ]);
        final lease = (start.arguments as Map)['lease'];
        expect(
          calls.every((c) => (c.arguments as Map)['lease'] == lease),
          isTrue,
        );
        await media.stop();
      },
    );
  }
  for (final reply in ['missing', 'false', 'null']) {
    test(
      'An old runner or unverified latch ($reply) cannot supply a private decoder',
      () async {
        final calls = <MethodCall>[];
        messenger.setMockMethodCallHandler(channel, (call) async {
          calls.add(call);
          if (call.method == 'thumbnailPrivacyLocked') {
            if (reply == 'missing') throw MissingPluginException();
            return reply == 'false' ? false : null;
          }
          return switch (call.method) {
            'start' => 92,
            'thumbnailPrivacy' => 0,
            _ => null,
          };
        });
        addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
        final media = NativeRemoteMedia(thumbnailAllowed: false);
        await expectLater(media.start(1280, 720), throwsA(privacyFailure));
        await Future<void>.delayed(const Duration(milliseconds: 5));
        final lease = (calls.first.arguments as Map)['lease'];
        expect(
          calls
              .where((c) => c.method == 'stop')
              .every((c) => (c.arguments as Map)['lease'] == lease),
          isTrue,
        );
        expect(calls.any((c) => c.method == 'stop'), isTrue);
      },
    );
  }
  test(
    'No-target privacy still requires the new native window latch',
    () async {
      final calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        return switch (call.method) {
          'start' => 93,
          'thumbnailPrivacy' => 0,
          'thumbnailPrivacyLocked' => true,
          _ => null,
        };
      });
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final media = NativeRemoteMedia(thumbnailAllowed: false);
      expect(await media.start(1280, 720), 93);
      expect(calls.map((c) => c.method), [
        'start',
        'thumbnailPrivacy',
        'thumbnailPrivacyLocked',
      ]);
      await media.stop();
    },
  );
  test(
    'Late private latch ACK cannot stop or adopt a replacement native owner',
    () async {
      final held = Completer<bool>(), calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        if (call.method == 'thumbnailPrivacyLocked') return held.future;
        return switch (call.method) {
          'start' => 94,
          'thumbnailPrivacy' => 0,
          'stats' => {'decodedFrames': 1},
          _ => null,
        };
      });
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final old = NativeRemoteMedia(thumbnailAllowed: false),
          fresh = NativeRemoteMedia();
      final pending = old.start(1280, 720);
      final rejected = expectLater(
        pending,
        throwsA(
          isA<PlatformException>().having(
            (e) => e.code,
            'code',
            'session_replaced',
          ),
        ),
      );
      await Future<void>.delayed(const Duration(milliseconds: 5));
      final oldLease = (calls.first.arguments as Map)['lease'];
      await fresh.start(1280, 720);
      final newLease = (calls.last.arguments as Map)['lease'];
      held.complete(true);
      await rejected;
      await Future<void>.delayed(const Duration(milliseconds: 5));
      expect(newLease, isNot(oldLease));
      expect(
        calls
            .where((c) => c.method == 'stop')
            .every((c) => (c.arguments as Map)['lease'] == oldLease),
        isTrue,
      );
      expect(await fresh.stats(), {'decodedFrames': 1});
      await fresh.stop();
    },
  );
  test(
    'Ordinary native media omits the new field and makes no privacy query',
    () async {
      final calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        return call.method == 'start' ? 95 : null;
      });
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final media = NativeRemoteMedia();
      await media.start(1280, 720);
      expect(calls, hasLength(1));
      expect(
        (calls.single.arguments as Map).containsKey('thumbnailAllowed'),
        isFalse,
      );
      await media.stop();
    },
  );
  test('A negotiated phase cannot inject an ordinary native media owner', () {
    expect(
      () => RemoteSession(
        connection: phaseConnection(),
        token: 'synthetic',
        media: NativeRemoteMedia(),
      ),
      throwsFormatException,
    );
  });
}

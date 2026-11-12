import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('multitap/remote'),
      events = MethodChannel('multitap/remote/input');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  test('Unopened media performs no native cleanup or capture and audio-off is explicit at startup', () async {
    final calls = <MethodCall>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return call.method == 'start' ? 73 : null;
    });
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    final media = NativeRemoteMedia(audioEnabled: false);
    expect(await media.capture(false), isFalse);
    await media.stop();
    expect(calls, isEmpty);
    await expectLater(media.stats(), throwsA(isA<PlatformException>()));
    expect(calls, isEmpty);
    expect(await media.start(1280, 720), 73);
    expect(calls.single.arguments, containsPair('audioEnabled', false));
    await media.stop();
  });

  test('A resize on the same media object invalidates its old stats and capture replies', () async {
    final pendingStats = Completer<Map<String, dynamic>>(),
        pendingCapture = Completer<bool>();
    final leases = <int>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      if (call.method == 'start') {
        leases.add((call.arguments as Map)['lease'] as int);
        return leases.length + 40;
      }
      if (call.method == 'stats') return pendingStats.future;
      if (call.method == 'capture') return pendingCapture.future;
      return null;
    });
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    final media = NativeRemoteMedia();
    await media.start(1280, 720);
    final oldStats = media.stats(), oldCapture = media.capture(true);
    final rejected = expectLater(oldStats, throwsA(isA<PlatformException>()));
    await media.start(1600, 900);
    pendingStats.complete({'decodedFrames': 900});
    pendingCapture.complete(true);
    await rejected;
    expect(await oldCapture, isFalse);
    expect(leases.length, 2);
    expect(leases.first, isNot(leases.last));
    await media.stop();
  });

  test(
    'A late start failure cannot invalidate a newer start on the same object',
    () async {
      final retiredStart = Completer<int>();
      var starts = 0;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'start') {
          return ++starts == 1 ? retiredStart.future : 88;
        }
        if (call.method == 'stats') return {'decodedFrames': 7};
        return null;
      });
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final media = NativeRemoteMedia();
      final retired = media.start(1280, 720);
      final rejected = expectLater(retired, throwsA(isA<PlatformException>()));
      await Future<void>.delayed(Duration.zero);
      expect(await media.start(1600, 900), 88);
      retiredStart.completeError(PlatformException(code: 'old_start_failure'));
      await rejected;
      expect(await media.stats(), {'decodedFrames': 7});
      await media.stop();
    },
  );

  test('Delayed stats and capture responses cannot observe or recapture a replacement engine', () async {
    final stats = Completer<Map<String, dynamic>>(),
        capture = Completer<bool>();
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => switch (call.method) {
        'start' => 42,
        'stats' => stats.future,
        'capture' => capture.future,
        _ => null,
      },
    );
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    final first = NativeRemoteMedia(), next = NativeRemoteMedia();
    await first.start(1280, 720);
    final oldStats = first.stats(), oldCapture = first.capture(true);
    final rejectedStats = expectLater(
      oldStats,
      throwsA(
        isA<PlatformException>().having(
          (e) => e.code,
          'code',
          'session_replaced',
        ),
      ),
    );
    await next.start(1920, 1080);
    stats.complete({'decodedFrames': 15});
    capture.complete(true);
    await rejectedStats;
    expect(await oldCapture, isFalse);
    await next.stop();
  });

  test('Only active-engine input is forwarded and the native lease stays off the wire', () async {
    final calls = <MethodCall>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return call.method == 'start' ? 42 : null;
    });
    messenger.setMockMethodCallHandler(events, (_) async => null);
    addTearDown(() {
      messenger.setMockMethodCallHandler(channel, null);
      messenger.setMockMethodCallHandler(events, null);
    });
    final first = NativeRemoteMedia(), next = NativeRemoteMedia();
    final oldEvents = <Map<String, dynamic>>[],
        newEvents = <Map<String, dynamic>>[];
    final oldSub = first.input.listen(oldEvents.add),
        newSub = next.input.listen(newEvents.add);
    await Future<void>.delayed(Duration.zero);
    await first.start(1280, 720);
    final oldLease = (calls.last.arguments as Map)['lease'];
    await next.start(1920, 1080);
    final newLease = (calls.last.arguments as Map)['lease'];
    for (final lease in [oldLease, newLease]) {
      await messenger.handlePlatformMessage(
        'multitap/remote/input',
        const StandardMethodCodec().encodeSuccessEnvelope({
          'lease': lease,
          'type': 'input',
          'event': 'key',
          'vk': 65,
          'down': true,
        }),
        (_) {},
      );
    }
    await Future<void>.delayed(Duration.zero);
    expect(oldEvents, isEmpty);
    expect(newEvents, [
      {'type': 'input', 'event': 'key', 'vk': 65, 'down': true},
    ]);
    await oldSub.cancel();
    await newSub.cancel();
    await next.stop();
    await Future<void>.delayed(Duration.zero);
  });
}

import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/remote_session.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('multitap/remote');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  final replaced = isA<PlatformException>().having(
    (e) => e.code,
    'code',
    'session_replaced',
  );
  Map<String, dynamic> configuration() => {
    'topologyEpoch': 1,
    'audioEnabled': false,
    'tracks': [
      for (final id in ['0', '1'])
        {
          'id': id,
          'epoch': 1,
          'width': 640,
          'height': 360,
          'sourceWidth': 640,
          'sourceHeight': 360,
          'left': id == '0' ? 0 : 640,
          'top': 0,
          'primary': id == '0',
        },
    ],
  };

  test('Every native media command carries its issuing owner, including bytes and cleanup', () async {
    final calls = <MethodCall>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return switch (call.method) {
        'start' => 70,
        'capture' => true,
        'stats' => {'decodedFrames': 1},
        _ => null,
      };
    });
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    final media = NativeRemoteMedia(audioEnabled: false);
    await media.start(640, 360);
    final lease = (calls.single.arguments as Map)['lease'];
    final bytes = Uint8List.fromList([1, 0, 0, 0, 1]);
    await media.push(bytes);
    expect(await media.capture(true), isTrue);
    await media.stats();
    await media.stop();
    expect(calls.map((c) => c.method), [
      'start',
      'push',
      'capture',
      'stats',
      'stop',
    ]);
    expect(calls.every((c) => (c.arguments as Map)['lease'] == lease), isTrue);
    expect((calls[1].arguments as Map)['bytes'], bytes);
  });

  test('A late push completion cannot report success for a replacement on the same object', () async {
    final pushed = Completer<void>(), calls = <MethodCall>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (call.method == 'push') {
        await pushed.future;
        return null;
      }
      return call.method == 'start'
          ? 71
          : call.method == 'stats'
          ? {'decodedFrames': 2}
          : null;
    });
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    final media = NativeRemoteMedia();
    await media.start(640, 360);
    final oldLease = (calls.single.arguments as Map)['lease'];
    final pending = media.push(Uint8List.fromList([1]));
    final rejected = expectLater(pending, throwsA(replaced));
    await media.start(800, 600);
    pushed.complete();
    await rejected;
    expect(
      (calls.firstWhere((c) => c.method == 'push').arguments as Map)['lease'],
      oldLease,
    );
    expect(await media.stats(), {'decodedFrames': 2});
    await media.stop();
  });

  test(
    'Pending stop keeps its original lease and does not retire a later start',
    () async {
      final stopped = Completer<void>(), calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        if (call.method == 'stop' && !stopped.isCompleted) {
          await stopped.future;
          return null;
        }
        return call.method == 'start'
            ? 72
            : call.method == 'stats'
            ? {'decodedFrames': 3}
            : null;
      });
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final media = NativeRemoteMedia();
      await media.start(640, 360);
      final oldLease = (calls.single.arguments as Map)['lease'];
      final oldStop = media.stop();
      await media.start(800, 600);
      final newLease = (calls.last.arguments as Map)['lease'];
      stopped.complete();
      await oldStop;
      expect(newLease, isNot(oldLease));
      expect(
        (calls.firstWhere((c) => c.method == 'stop').arguments as Map)['lease'],
        oldLease,
      );
      await media.stats();
      expect((calls.last.arguments as Map)['lease'], newLease);
      await media.stop();
    },
  );

  test(
    'Independent outputs and legacy streaming share one native owner namespace',
    () async {
      final calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        return switch (call.method) {
          'start' => 73,
          'startTracks' => {'0': 80, '1': 81},
          'capture' => true,
          'stats' => {'tracksReady': true},
          _ => null,
        };
      });
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final first = NativeRemoteMedia(), next = NativeRemoteMedia();
      await first.start(640, 360);
      final oldLease = (calls.single.arguments as Map)['lease'];
      expect(await next.startTracks(configuration()), {'0': 80, '1': 81});
      final lease = (calls.last.arguments as Map)['lease'];
      expect(lease, isNot(oldLease));
      final count = calls.length;
      await first.stop();
      expect(await first.capture(false), isFalse);
      await expectLater(first.stats(), throwsA(replaced));
      expect(calls.length, count);
      await next.push(Uint8List.fromList([0x54, 0x56, 0x54, 0x31]));
      await next.stats();
      await next.capture(true);
      await next.stop();
      expect(
        calls.skip(count).every((c) => (c.arguments as Map)['lease'] == lease),
        isTrue,
      );
    },
  );

  test(
    'Invalid independent texture reply closes only its own failed native owner',
    () async {
      final calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        return switch (call.method) {
          'startTracks' => {'0': 82, '1': 82},
          'start' => 83,
          'stats' => {'decodedFrames': 4},
          _ => null,
        };
      });
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final media = NativeRemoteMedia();
      await expectLater(
        media.startTracks(configuration()),
        throwsA(isA<PlatformException>()),
      );
      final failedLease = (calls.first.arguments as Map)['lease'];
      await media.start(800, 600);
      await Future<void>.delayed(Duration.zero);
      expect(
        calls
            .where((c) => c.method == 'stop')
            .map((c) => (c.arguments as Map)['lease']),
        [failedLease],
      );
      expect(await media.stats(), {'decodedFrames': 4});
      await media.stop();
    },
  );

  test('Late independent startup reply cannot invalidate a newer legacy start or select its textures', () async {
    final pending = Completer<Map<String, int>>(), calls = <MethodCall>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return switch (call.method) {
        'startTracks' => pending.future,
        'start' => 84,
        'stats' => {'decodedFrames': 5},
        _ => null,
      };
    });
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    final media = NativeRemoteMedia();
    final retired = media.startTracks(configuration());
    final rejected = expectLater(retired, throwsA(replaced));
    await Future<void>.delayed(Duration.zero);
    final oldLease = (calls.single.arguments as Map)['lease'];
    await media.start(800, 600);
    pending.complete({'0': 85, '1': 86});
    await rejected;
    await Future<void>.delayed(Duration.zero);
    expect(
      (calls.firstWhere((c) => c.method == 'stop').arguments as Map)['lease'],
      oldLease,
    );
    expect(await media.stats(), {'decodedFrames': 5});
    await media.stop();
  });

  test(
    'A sparse connector1 texture is retained without inventing output0',
    () async {
      final calls = <MethodCall>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        calls.add(call);
        return call.method == 'startTracks' ? {'1': 87} : null;
      });
      addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
      final value = configuration();
      value['tracks'] = [(value['tracks'] as List).last];
      final media = NativeRemoteMedia();
      expect(await media.startTracks(value), {'1': 87});
      await media.stop();
    },
  );
}

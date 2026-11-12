import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_session_keepalive.dart';

Future<Object?> deliverNotificationStop(String id) async {
  final reply = Completer<Object?>();
  await TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
      .handlePlatformMessage(
        AndroidSessionKeepalive.channel.name,
        const StandardMethodCodec().encodeMethodCall(
          MethodCall('disconnect', {'lease': id}),
        ),
        (data) => reply.complete(
          data == null
              ? null
              : const StandardMethodCodec().decodeEnvelope(data),
        ),
      );
  return reply.future;
}

/// Models native lease comparison only; no service, OS priority or networking.
class _KeepaliveChannel {
  final starts = <String, Completer<String>>{};
  final stops = <String>[];
  String? nativeLease;
  bool failStart = false;
  Completer<void>? stopGate;

  Future<Object?> call(MethodCall call) async {
    final id = (call.arguments as Map)['lease'] as String;
    if (call.method == 'start') {
      nativeLease = id;
      if (failStart) throw PlatformException(code: 'start_failed');
      final reply = starts[id] = Completer<String>();
      return reply.future;
    }
    if (call.method == 'stop') {
      stops.add(id);
      final gate = stopGate;
      if (gate != null) await gate.future;
      // The production native handler must enforce this same ownership rule.
      if (nativeLease != id) return false;
      nativeLease = null;
      return true;
    }
    throw MissingPluginException();
  }

  void install() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(AndroidSessionKeepalive.channel, call);
    addTearDown(
      () => messenger.setMockMethodCallHandler(
        AndroidSessionKeepalive.channel,
        null,
      ),
    );
  }
}

void main() {
  testWidgets(
    'notification disconnect is current-lease only and deduplicated',
    (tester) async {
      final native = _KeepaliveChannel()..install();
      var disconnects = 0;
      final closing = Completer<void>();
      final owner = AndroidSessionKeepaliveOwner(
        onDisconnect: () {
          disconnects++;
          return closing.future;
        },
      );
      final starting = owner.start();
      await tester.pump();
      final id = native.starts.keys.single;
      native.starts[id]!.complete('started');
      await tester.pump();
      expect(await starting, isTrue);
      expect(await deliverNotificationStop('0' * 32), isFalse);
      expect(disconnects, 0);
      final first = deliverNotificationStop(id);
      final second = deliverNotificationStop(id);
      await tester.pump();
      expect(disconnects, 1);
      expect(owner.started, isTrue, reason: 'request is not completion');
      closing.complete();
      await tester.pump();
      expect(await first, isTrue);
      expect(await second, isTrue);
      owner.stop();
      await tester.pump();
      expect(native.nativeLease, isNull);
      expect(await deliverNotificationStop(id), isFalse);
    },
  );
  testWidgets('closing during acquire releases its eventual successful lease', (
    tester,
  ) async {
    final native = _KeepaliveChannel()..install();
    final owner = AndroidSessionKeepaliveOwner();
    final starting = owner.start();
    await tester.pump();
    final id = native.starts.keys.single;
    owner.stop();
    expect(owner.started, isFalse);
    native.starts[id]!.complete('started');
    await tester.pump();
    expect(await starting, isFalse);
    await tester.pump();
    expect(native.stops, contains(id));
    expect(native.nativeLease, isNull);
    expect(owner.started, isFalse);
  });

  testWidgets('late A reply and delayed A stop cannot retire B native lease', (
    tester,
  ) async {
    final native = _KeepaliveChannel()..install();
    final a = AndroidSessionKeepaliveOwner(),
        b = AndroidSessionKeepaliveOwner();
    final first = a.start();
    await tester.pump();
    final aId = native.starts.keys.single;
    a.stop();
    final second = b.start();
    await tester.pump();
    final bId = native.starts.keys.last;
    expect(bId, isNot(aId));
    native.starts[bId]!.complete('started');
    await tester.pump();
    expect(await second, isTrue);
    native.stopGate = Completer<void>();
    native.starts[aId]!.complete('started');
    await tester.pump();
    expect(native.stops, contains(aId));
    expect(native.nativeLease, bId);
    native.stopGate!.complete();
    await tester.pump();
    expect(await first, isFalse);
    expect(native.nativeLease, bId);
    expect(b.started, isTrue);
    b.stop();
    await tester.pump();
    expect(native.nativeLease, isNull);
  });

  testWidgets(
    'start failure and twelve-second timeout clean up only their lease',
    (tester) async {
      final native = _KeepaliveChannel()..install();
      native.failStart = true;
      final failed = AndroidSessionKeepaliveOwner();
      final failure = failed.start();
      await tester.pump();
      expect(await failure, isFalse);
      expect(failed.started, isFalse);
      expect(native.stops, hasLength(1));
      expect(native.nativeLease, isNull);
      failed.stop();
      await tester.pump();

      native.failStart = false;
      final timedOut = AndroidSessionKeepaliveOwner();
      final waiting = timedOut.start();
      await tester.pump();
      final id = native.starts.keys.single;
      await tester.pump(const Duration(seconds: 11));
      expect(native.stops, isNot(contains(id)));
      await tester.pump(const Duration(seconds: 1));
      await tester.pump();
      expect(await waiting, isFalse);
      expect(timedOut.started, isFalse);
      expect(native.stops, contains(id));
      expect(native.nativeLease, isNull);
      // Completing the old platform response does not revive the expired owner.
      native.starts[id]!.complete('started');
      await tester.pump();
      expect(timedOut.started, isFalse);
      expect(native.nativeLease, isNull);
      timedOut.stop();
      await tester.pump();
    },
  );
}

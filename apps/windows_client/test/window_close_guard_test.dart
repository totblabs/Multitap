import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/window_close_guard.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('multitap/window/close');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));
  test('Window bridge reports unavailable protection instead of silently allowing work', () async {
    final guard = NativeWindowCloseGuard()..attach(() async {});
    expect(await guard.update(busy: true, dirty: false), isFalse);
    expect(await guard.close(), isFalse);
    guard.dispose();
    await Future<void>.delayed(Duration.zero);
  });
  test('Only lifetime flags cross the bridge and a disposed lease cannot clear a newer guard', () async {
    final calls = <MethodCall>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      return true;
    });
    var oldRequested = 0, newRequested = 0;
    final old = NativeWindowCloseGuard()
      ..attach(() async {
        oldRequested++;
      });
    expect(await old.update(busy: true, dirty: false), isTrue);
    final current = NativeWindowCloseGuard()
      ..attach(() async {
        newRequested++;
      });
    old.dispose();
    expect(calls, hasLength(1));
    expect(await current.update(busy: false, dirty: true), isTrue);
    final reply = Completer<void>();
    await messenger.handlePlatformMessage(
      'multitap/window/close',
      const StandardMethodCodec().encodeMethodCall(
        const MethodCall('requested'),
      ),
      (_) => reply.complete(),
    );
    await reply.future;
    expect(oldRequested, 0);
    expect(newRequested, 1);
    expect(calls.last.arguments, {'busy': false, 'dirty': true});
    expect(await current.close(), isTrue);
    expect(calls.last.method, 'close');
    current.dispose();
    await Future<void>.delayed(Duration.zero);
    expect(calls.last.arguments, {'busy': false, 'dirty': false});
  });
}

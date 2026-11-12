import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/console_keyboard.dart';
import 'package:multitap_client/data/console_rfb.dart';

import 'console_rfb_test.dart' as fixture;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('test/console-keyboard');
  const owner = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa';
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  Map<String, Object?> reply(
    MethodCall call, {
    int lease = 1,
    bool captured = false,
  }) => {
    'protocol': 'console-input-v1',
    'owner': owner,
    'generation': (call.arguments as Map)['generation'],
    'lease': lease,
    'accepted': true,
    'captured': captured,
  };
  Future<void> event(Map<String, Object?> fields) async {
    final done = Completer<void>();
    await messenger.handlePlatformMessage(
      channel.name,
      const StandardMethodCodec().encodeMethodCall(
        MethodCall('event', {
          'owner': owner,
          'generation': 1,
          'lease': 1,
          ...fields,
        }),
      ),
      (_) => done.complete(),
    );
    await done.future;
  }

  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  test(
    'native VK maps left/right Windows, Alt+Tab and modifier scan identity',
    () {
      expect(consoleVirtualKeysym(0x5b, 0, false), 0xffeb);
      expect(consoleVirtualKeysym(0x5c, 0, false), 0xffec);
      expect(consoleVirtualKeysym(0x12, 0, false), 0xffe9);
      expect(consoleVirtualKeysym(0x12, 0, true), 0xffea);
      expect(consoleVirtualKeysym(9, 0, false), 0xff09);
      expect(consoleVirtualKeysym(0x10, 0x36, false), 0xffe2);
    },
  );

  test('fake native hook is sole key producer; wrong identity and reset are isolated', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => call.method == 'release'
          ? null
          : reply(call, captured: call.method == 'capture'),
    );
    final wire = <List<int>>[];
    final rfb = ConsoleRfb(send: wire.add);
    rfb.add(fixture.initRfb());
    wire.clear();
    addTearDown(rfb.dispose);
    final keys = <List<Object>>[];
    var resets = 0;
    final keyboard = NativeConsoleKeyboard(channel: channel, owner: owner);
    keyboard.onKey = (key, down) {
      keys.add([key, down]);
      rfb.key(key, down);
    };
    keyboard.onReset = () {
      resets++;
      rfb.releaseInput();
    };
    addTearDown(keyboard.dispose);
    expect(await keyboard.acquire(1), isTrue);
    expect(await keyboard.capture(true), isTrue);
    await event({
      'type': 'input',
      'event': 'key',
      'vk': 0x5b,
      'scan': 91,
      'down': true,
      'extended': true,
    });
    await event({
      'type': 'input',
      'event': 'key',
      'vk': 9,
      'scan': 15,
      'down': true,
      'extended': false,
    });
    await event({
      'type': 'input',
      'event': 'key',
      'vk': 9,
      'scan': 15,
      'down': false,
      'extended': false,
    });
    await event({
      'owner': 'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',
      'type': 'input',
      'event': 'key',
      'vk': 65,
      'scan': 30,
      'down': true,
      'extended': false,
    });
    expect(keys, [
      [0xffeb, true],
      [0xff09, true],
      [0xff09, false],
    ]);
    final before = resets;
    await event({'type': 'input.reset'});
    expect(keyboard.captured, isFalse);
    expect(resets, before + 1);
    expect(wire.last[0], 4);
    expect(wire.last[1], 0);
    expect(
      ByteData.sublistView(Uint8List.fromList(wire.last)).getUint32(4),
      0xffeb,
    );
    await event({
      'type': 'input',
      'event': 'key',
      'vk': 65,
      'scan': 30,
      'down': true,
      'extended': false,
    });
    expect(keys.length, 3);
  });

  test(
    'late grant after release is retired by exact lease and cannot capture',
    () async {
      final held = Completer<Map<String, Object?>>(),
          entered = Completer<void>();
      final releases = <Map>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'acquire') {
          entered.complete();
          return held.future;
        }
        if (call.method == 'release') {
          releases.add(call.arguments as Map);
          return null;
        }
        return reply(call);
      });
      final keyboard = NativeConsoleKeyboard(channel: channel, owner: owner);
      addTearDown(keyboard.dispose);
      final pending = keyboard.acquire(1);
      await entered.future;
      await keyboard.release();
      held.complete({
        'protocol': 'console-input-v1',
        'owner': owner,
        'generation': 1,
        'lease': 7,
        'accepted': true,
        'captured': false,
      });
      expect(await pending, isFalse);
      expect(releases.single['lease'], 7);
      expect(keyboard.captured, isFalse);
    },
  );

  test(
    'late capture ACK cannot revive a released or newer generation',
    () async {
      final held = Completer<Map<String, Object?>>(),
          entered = Completer<void>();
      var nextLease = 0;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'acquire') return reply(call, lease: ++nextLease);
        if (call.method == 'capture') {
          entered.complete();
          return held.future;
        }
        return null;
      });
      final keyboard = NativeConsoleKeyboard(channel: channel, owner: owner);
      addTearDown(keyboard.dispose);
      expect(await keyboard.acquire(1), isTrue);
      final pending = keyboard.capture(true);
      await entered.future;
      await keyboard.release();
      expect(await keyboard.acquire(2), isTrue);
      held.complete({
        'protocol': 'console-input-v1',
        'owner': owner,
        'generation': 1,
        'lease': 1,
        'accepted': true,
        'captured': true,
      });
      expect(await pending, isFalse);
      expect(keyboard.captured, isFalse);
    },
  );

  test('keys arriving before capture ACK replay in order only after current acceptance', () async {
    final held = Completer<Map<String, Object?>>(), entered = Completer<void>();
    final keys = <List<Object>>[];
    messenger.setMockMethodCallHandler(channel, (call) async {
      if (call.method == 'acquire') return reply(call);
      if (call.method == 'capture') {
        entered.complete();
        return held.future;
      }
      return null;
    });
    final keyboard = NativeConsoleKeyboard(channel: channel, owner: owner);
    keyboard.onKey = (k, d) => keys.add([k, d]);
    addTearDown(keyboard.dispose);
    await keyboard.acquire(1);
    final pending = keyboard.capture(true);
    await entered.future;
    await event({
      'type': 'input',
      'event': 'key',
      'vk': 0x12,
      'scan': 56,
      'down': true,
      'extended': false,
    });
    await event({
      'type': 'input',
      'event': 'key',
      'vk': 9,
      'scan': 15,
      'down': true,
      'extended': false,
    });
    await event({
      'type': 'input',
      'event': 'key',
      'vk': 9,
      'scan': 15,
      'down': false,
      'extended': false,
    });
    expect(keys, isEmpty);
    held.complete({
      'protocol': 'console-input-v1',
      'owner': owner,
      'generation': 1,
      'lease': 1,
      'accepted': true,
      'captured': true,
    });
    expect(await pending, isTrue);
    expect(keys, [
      [0xffe9, true],
      [0xff09, true],
      [0xff09, false],
    ]);
  });

  test('native focus reset before held ACK invalidates capture rather than reviving it', () async {
    final held = Completer<Map<String, Object?>>(), entered = Completer<void>();
    var keys = 0;
    messenger.setMockMethodCallHandler(channel, (call) async {
      if (call.method == 'acquire') return reply(call);
      if (call.method == 'capture') {
        entered.complete();
        return held.future;
      }
      return null;
    });
    final keyboard = NativeConsoleKeyboard(channel: channel, owner: owner);
    keyboard.onKey = (_, _) => keys++;
    addTearDown(keyboard.dispose);
    await keyboard.acquire(1);
    final pending = keyboard.capture(true);
    await entered.future;
    await event({
      'type': 'input',
      'event': 'key',
      'vk': 0x5b,
      'scan': 91,
      'down': true,
      'extended': true,
    });
    await event({'type': 'input.reset'});
    held.complete({
      'protocol': 'console-input-v1',
      'owner': owner,
      'generation': 1,
      'lease': 1,
      'accepted': true,
      'captured': true,
    });
    expect(await pending, isFalse);
    expect(keyboard.captured, isFalse);
    expect(keys, 0);
  });

  testWidgets(
    'observer timeout preserves actual pending acquire cleanup without adoption',
    (tester) async {
      final held = Completer<Map<String, Object?>>();
      final releases = <Map>[];
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'acquire') return held.future;
        if (call.method == 'release') releases.add(call.arguments as Map);
        return null;
      });
      final keyboard = NativeConsoleKeyboard(channel: channel, owner: owner);
      addTearDown(keyboard.dispose);
      final pending = keyboard.acquire(1);
      await tester.pump();
      await tester.pump(const Duration(seconds: 3));
      expect(await pending, isFalse);
      held.complete({
        'protocol': 'console-input-v1',
        'owner': owner,
        'generation': 1,
        'lease': 9,
        'accepted': true,
        'captured': false,
      });
      await tester.pump();
      expect(releases.single['lease'], 9);
      expect(keyboard.captured, isFalse);
    },
  );

  testWidgets(
    'timed-out capture releases exact old hook and late ACK cannot capture successor',
    (tester) async {
      final held = Completer<Map<String, Object?>>();
      final releases = <Map>[];
      var nextLease = 0;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'acquire') return reply(call, lease: ++nextLease);
        if (call.method == 'capture') return held.future;
        if (call.method == 'release') releases.add(call.arguments as Map);
        return null;
      });
      final keyboard = NativeConsoleKeyboard(channel: channel, owner: owner);
      addTearDown(keyboard.dispose);
      await keyboard.acquire(1);
      final pending = keyboard.capture(true);
      await tester.pump();
      await tester.pump(const Duration(seconds: 3));
      expect(await pending, isFalse);
      expect(
        releases.any((v) => v['lease'] == 1 && v['generation'] == 1),
        isTrue,
      );
      expect(await keyboard.acquire(2), isTrue);
      held.complete({
        'protocol': 'console-input-v1',
        'owner': owner,
        'generation': 1,
        'lease': 1,
        'accepted': true,
        'captured': true,
      });
      await tester.pump();
      expect(keyboard.captured, isFalse);
      expect(
        releases.every((v) => v['lease'] == 1 && v['generation'] == 1),
        isTrue,
      );
    },
  );
}

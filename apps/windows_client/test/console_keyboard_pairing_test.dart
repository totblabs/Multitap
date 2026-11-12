import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/console_keyboard.dart';
import 'package:multitap_client/data/console_rfb.dart';
import 'package:multitap_client/ui/emergency_console_view.dart';

import 'console_rfb_test.dart' as wire;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('test/console-key-pairing');
  const owner = 'cccccccccccccccccccccccccccccccc';
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));
  Future<void> event(
    int vk,
    int scan,
    bool down, {
    bool extended = false,
    int generation = 1,
    String type = 'input',
  }) async {
    final done = Completer<void>();
    await messenger.handlePlatformMessage(
      channel.name,
      const StandardMethodCodec().encodeMethodCall(
        MethodCall('event', {
          'owner': owner,
          'generation': generation,
          'lease': 1,
          'type': type,
          'event': 'key',
          'vk': vk,
          'scan': scan,
          'down': down,
          'extended': extended,
        }),
      ),
      (_) => done.complete(),
    );
    await done.future;
  }

  test('Windows and Flutter Hangul/Hanja keys have explicit RFB symbols', () {
    expect(consoleVirtualKeysym(0x15, 0xf2, false), 0xff31);
    expect(consoleVirtualKeysym(0x19, 0xf1, false), 0xff34);
    expect(consoleKeysym(LogicalKeyboardKey.hangulMode), 0xff31);
    expect(consoleKeysym(LogicalKeyboardKey.lang1), 0xff31);
    expect(consoleKeysym(LogicalKeyboardKey.hanjaMode), 0xff34);
    expect(consoleKeysym(LogicalKeyboardKey.lang2), 0xff34);
  });

  test(
    'repeat and key-up retain the original physical press despite VK changes',
    () async {
      messenger.setMockMethodCallHandler(
        channel,
        (call) async => call.method == 'release'
            ? null
            : {
                'protocol': 'console-input-v1',
                'owner': owner,
                'generation': (call.arguments as Map)['generation'],
                'accepted': true,
                'lease': 1,
                'captured': call.method == 'capture',
              },
      );
      final keys = <(int, bool)>[];
      final keyboard = NativeConsoleKeyboard(channel: channel, owner: owner)
        ..onKey = (key, down) => keys.add((key, down));
      addTearDown(keyboard.dispose);
      expect(await keyboard.acquire(1), isTrue);
      expect(await keyboard.capture(true), isTrue);
      await event(0x41, 30, true);
      await event(0x51, 30, true); // Same hardware press, changed VK/layout.
      await event(0x51, 30, false);
      await event(0x15, 0xf2, true);
      await event(
        0xa5,
        0xf2,
        false,
      ); // IME/sided-VK key-up still owns the down.
      await event(0x10, 0x36, true);
      await event(0xa1, 0x36, false);
      await event(0x5b, 0x5b, true, extended: true, generation: 2);
      expect(keys, [
        (97, true),
        (97, true),
        (97, false),
        (0xff31, true),
        (0xff31, false),
        (0xffe2, true),
        (0xffe2, false),
      ]);
      await keyboard.release();
      await event(0x41, 30, true);
      expect(keys.length, 7);
    },
  );

  test(
    'keyboard release preserves a held pointer until full focus/end release',
    () {
      final sent = <Uint8List>[];
      final rfb = ConsoleRfb(send: sent.add);
      addTearDown(rfb.dispose);
      rfb.add(wire.initRfb());
      rfb.pointer(1, 1, 1);
      rfb.key(0xffe3, true);
      sent.clear();
      rfb.releaseKeyboard();
      expect(sent.length, 1);
      expect(sent.single[0], 4);
      expect(sent.single[1], 0);
      rfb.releaseInput();
      expect(sent.length, 2);
      expect(sent.last[0], 5);
      expect(sent.last[1], 0);
    },
  );

  test('native physical callback is sole producer and retains first code across VK changes', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => call.method == 'release'
          ? null
          : {
              'protocol': 'console-input-v1',
              'owner': owner,
              'generation': (call.arguments as Map)['generation'],
              'accepted': true,
              'lease': 1,
              'captured': call.method == 'capture',
            },
    );
    final strokes = <ConsoleKeyStroke>[];
    var legacy = 0;
    final keyboard = NativeConsoleKeyboard(channel: channel, owner: owner);
    keyboard.onKey = (_, _) => legacy++;
    keyboard.onPhysicalKey = strokes.add;
    addTearDown(keyboard.dispose);
    await keyboard.acquire(1);
    await keyboard.capture(true);
    await event(0x15, 0xf2, true);
    await event(0xa5, 0xf2, true);
    await event(0xa5, 0xf2, false);
    await event(0xa3, 0x1d, true, extended: true);
    await event(0x11, 0x1d, false, extended: true);
    await event(0x41, 30, true, generation: 2);
    expect(legacy, 0);
    expect(strokes.map((s) => (s.keysym, s.down, s.qemuKeyCode)), [
      (0xff31, true, 0x72),
      (0xff31, true, 0x72),
      (0xff31, false, 0x72),
      (0xffe4, true, 0x9d),
      (0xffe4, false, 0x9d),
    ]);
    await keyboard.release();
    await event(0x41, 30, true);
    expect(strokes.length, 5);
  });

  test('an old reset callback cannot report capture loss into its reentrant successor', () async {
    messenger.setMockMethodCallHandler(
      channel,
      (call) async => call.method == 'release'
          ? null
          : {
              'protocol': 'console-input-v1',
              'owner': owner,
              'generation': (call.arguments as Map)['generation'],
              'accepted': true,
              'lease': 1,
              'captured': call.method == 'capture',
            },
    );
    final keyboard = NativeConsoleKeyboard(channel: channel, owner: owner);
    addTearDown(keyboard.dispose);
    await keyboard.acquire(1);
    await keyboard.capture(true);
    Future<bool>? newer;
    var switched = false, lost = 0;
    keyboard.onCaptureLost = () => lost++;
    keyboard.onReset = () {
      if (!switched) {
        switched = true;
        newer = keyboard.acquire(2);
      }
    };
    await event(0, 0, false, type: 'input.reset');
    expect(await newer, isTrue);
    expect(lost, 0);
    keyboard.onReset = null;
    await keyboard.capture(true);
    await event(0, 0, false, type: 'capture.released', generation: 2);
    expect(lost, 1);
    expect(keyboard.captured, isFalse);
  });
}

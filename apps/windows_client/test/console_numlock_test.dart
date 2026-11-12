import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/console_keyboard.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('test/console-numlock');
  const owner = 'dddddddddddddddddddddddddddddddd';
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  // FreeRDP3.0.0 wf_event.c normalizes NumLock from the same Windows hook;
  // noVNC1.6.0 xtscancodes.js identifies NumLock as XT/qnum0x45.
  test(
    'NumLock normalizes the Windows extended flag without converting Pause',
    () {
      expect(consoleQemuKeyCode(0x90, 0x45, true), 0x45);
      expect(consoleQemuKeyCode(0x90, 0x45, false), 0x45);
      expect(consoleQemuKeyCode(0x13, 0x45, false), isNull);
      expect(consoleQemuKeyCode(0x13, 0x45, true), isNull);
      expect(consoleQemuKeyCode(0x2c, 0x37, true), 0xb7);
      expect(consoleQemuKeyCode(0xa3, 0x1d, true), 0x9d);
      expect(consoleQemuKeyCode(0x90, 0, true), isNull);
      expect(consoleQemuKeyCode(0x90, 0x145, true), isNull);
    },
  );

  for (final extended in [false, true]) {
    test(
      'native NumLock down/repeat/up retains normalized code (extended=$extended)',
      () async {
        messenger.setMockMethodCallHandler(channel, (call) async {
          if (call.method == 'release') return null;
          return {
            'protocol': 'console-input-v1',
            'owner': owner,
            'generation': (call.arguments as Map)['generation'],
            'accepted': true,
            'lease': 1,
            'captured': call.method == 'capture',
          };
        });
        final strokes = <ConsoleKeyStroke>[];
        final keyboard = NativeConsoleKeyboard(channel: channel, owner: owner)
          ..onPhysicalKey = strokes.add;
        addTearDown(keyboard.dispose);
        expect(await keyboard.acquire(1), isTrue);
        expect(await keyboard.capture(true), isTrue);
        for (final down in [true, true, false]) {
          final completed = Completer<void>();
          await messenger.handlePlatformMessage(
            channel.name,
            const StandardMethodCodec().encodeMethodCall(
              MethodCall('event', {
                'owner': owner,
                'generation': 1,
                'lease': 1,
                'type': 'input',
                'event': 'key',
                'vk': 0x90,
                'scan': 0x45,
                'down': down,
                'extended': extended,
              }),
            ),
            (_) => completed.complete(),
          );
          await completed.future;
        }
        expect(strokes.map((s) => (s.keysym, s.down, s.qemuKeyCode)), [
          (0xff7f, true, 0x45),
          (0xff7f, true, 0x45),
          (0xff7f, false, 0x45),
        ]);
        await keyboard.release();
        expect(keyboard.captured, isFalse);
      },
    );
  }
}

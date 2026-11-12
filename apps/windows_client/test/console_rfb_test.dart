import 'dart:convert';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:multitap_client/data/console_rfb.dart';
import 'package:multitap_client/ui/emergency_console_view.dart';

Uint8List initRfb({int width = 2, int height = 2}) {
  final init = Uint8List(24);
  ByteData.sublistView(init)
    ..setUint16(0, width)
    ..setUint16(2, height);
  init.setRange(4, 20, [
    32,
    24,
    0,
    1,
    0,
    255,
    0,
    255,
    0,
    255,
    16,
    8,
    0,
    0,
    0,
    0,
  ]);
  return Uint8List.fromList([
    ...ascii.encode('RFB 003.008\n'),
    1,
    1,
    0,
    0,
    0,
    0,
    ...init,
  ]);
}

Uint8List rfbRect(
  int x,
  int y,
  int width,
  int height,
  int encoding,
  List<int> payload,
) {
  final header = Uint8List(12);
  ByteData.sublistView(header)
    ..setUint16(0, x)
    ..setUint16(2, y)
    ..setUint16(4, width)
    ..setUint16(6, height)
    ..setInt32(8, encoding);
  return Uint8List.fromList([0, 0, 0, 1, ...header, ...payload]);
}

void main() {
  test('fragmented RFB 3.8 None initialization explicitly negotiates bounded encodings', () {
    final sent = <Uint8List>[];
    final rfb = ConsoleRfb(send: sent.add);
    addTearDown(rfb.dispose);
    for (final byte in initRfb()) {
      rfb.add([byte]);
    }
    expect(rfb.initialized, isTrue);
    expect(rfb.frameRevision, 0); // ServerInit alone is not a displayed frame.
    expect(ascii.decode(sent[0]), 'RFB 003.008\n');
    expect(sent[1], [1]);
    final encodings = ByteData.sublistView(sent[4]);
    expect(
      [for (var at = 4; at < 20; at += 4) encodings.getInt32(at)],
      [0, 1, -223, -239],
    );
    expect(sent[5][0], 3);
    expect(sent[5][1], 0);
  });

  test('raw pixels survive byte fragmentation with exact RGBA channels', () {
    final rfb = ConsoleRfb(send: (_) {});
    addTearDown(rfb.dispose);
    rfb.add(initRfb());
    final update = rfbRect(0, 0, 2, 2, 0, [
      3,
      2,
      1,
      0,
      6,
      5,
      4,
      0,
      9,
      8,
      7,
      0,
      12,
      11,
      10,
      0,
    ]);
    for (final byte in update) {
      rfb.add([byte]);
    }
    expect(rfb.pixels, [
      1,
      2,
      3,
      255,
      4,
      5,
      6,
      255,
      7,
      8,
      9,
      255,
      10,
      11,
      12,
      255,
    ]);
    expect(rfb.frameRevision, 1);
  });

  test('overlapping CopyRect preserves original source rows', () {
    final rfb = ConsoleRfb(send: (_) {});
    addTearDown(rfb.dispose);
    rfb.add(initRfb(width: 2, height: 3));
    rfb.add(
      rfbRect(0, 0, 2, 3, 0, [
        for (var n = 1; n <= 6; n++) ...[0, 0, n, 0],
      ]),
    );
    rfb.add(rfbRect(0, 1, 2, 2, 1, [0, 0, 0, 0]));
    expect([for (var i = 0; i < 6; i++) rfb.pixels[i * 4]], [1, 2, 1, 2, 3, 4]);
  });

  test('Cursor mask/hotspot and explicit hidden cursor are retained', () {
    final rfb = ConsoleRfb(send: (_) {});
    addTearDown(rfb.dispose);
    rfb.add(initRfb());
    rfb.add(rfbRect(1, 0, 2, 1, -239, [0, 0, 255, 0, 0, 255, 0, 0, 128]));
    expect(rfb.cursor!.hotX, 1);
    expect(
      rfb.frameValid,
      isFalse,
    ); // Cursor metadata is not a painted desktop.
    expect(rfb.cursor!.rgba, [255, 0, 0, 255, 0, 255, 0, 0]);
    rfb.add(rfbRect(0, 0, 0, 0, -239, []));
    expect(rfb.cursor!.hidden, isTrue);
  });

  test('copy from an uninitialized or resized framebuffer cannot invent first-frame readiness', () {
    final rfb = ConsoleRfb(send: (_) {});
    addTearDown(rfb.dispose);
    rfb.add(initRfb());
    expect(
      () => rfb.add(rfbRect(0, 0, 1, 1, 1, [0, 0, 0, 0])),
      throwsFormatException,
    );
    expect(rfb.frameValid, isFalse);
    expect(rfb.frameRevision, 0);
  });

  test('Raw followed by DesktopSize in one update cannot promote a zeroed replacement framebuffer', () {
    final rfb = ConsoleRfb(send: (_) {});
    addTearDown(rfb.dispose);
    rfb.add(initRfb());
    final raw = rfbRect(0, 0, 1, 1, 0, [0, 0, 255, 0]);
    final resize = rfbRect(0, 0, 3, 1, -223, []);
    rfb.add([0, 0, 0, 2, ...raw.sublist(4), ...resize.sublist(4)]);
    expect(rfb.width, 3);
    expect(rfb.height, 1);
    expect(rfb.pixels, everyElement(0));
    expect(rfb.frameValid, isFalse);
    expect(rfb.frameRevision, 0);
  });

  test(
    'DesktopSize clears old pixels and releases held input before resize',
    () {
      final sent = <Uint8List>[];
      final rfb = ConsoleRfb(send: sent.add);
      addTearDown(rfb.dispose);
      rfb.add(initRfb());
      rfb.key(0xffe3, true);
      rfb.pointer(1, 1, 1);
      sent.clear();
      rfb.add(rfbRect(0, 0, 3, 1, -223, []));
      expect([rfb.width, rfb.height], [3, 1]);
      expect(rfb.pixels, everyElement(0));
      expect(sent.where((m) => m[0] == 4).single[1], 0);
      expect(sent.where((m) => m[0] == 5).single[1], 0);
    },
  );

  test(
    'unsupported encodings, rectangle bounds and oversized framebuffer reject',
    () {
      for (final rect in [
        rfbRect(0, 0, 1, 1, 16, []),
        rfbRect(2, 0, 1, 1, 0, []),
      ]) {
        final rfb = ConsoleRfb(send: (_) {});
        addTearDown(rfb.dispose);
        rfb.add(initRfb());
        expect(() => rfb.add(rect), throwsFormatException);
      }
      final rfb = ConsoleRfb(send: (_) {});
      addTearDown(rfb.dispose);
      expect(
        () => rfb.add(initRfb(width: 8192, height: 8192)),
        throwsFormatException,
      );
    },
  );

  test(
    'only None auth and RFB 3.8 are accepted; transport chunk bound enforced',
    () {
      final rfb = ConsoleRfb(send: (_) {});
      addTearDown(rfb.dispose);
      rfb.add(ascii.encode('RFB 003.008\n'));
      expect(() => rfb.add([1, 2]), throwsFormatException);
      final old = ConsoleRfb(send: (_) {});
      addTearDown(old.dispose);
      expect(
        () => old.add(ascii.encode('RFB 003.003\n')),
        throwsFormatException,
      );
      expect(
        () => old.add(Uint8List(ConsoleRfb.maxChunk + 1)),
        throwsFormatException,
      );
    },
  );

  test('focus-loss release balances held keys/buttons exactly once and closed ignores input', () {
    final sent = <Uint8List>[];
    final rfb = ConsoleRfb(send: sent.add);
    addTearDown(rfb.dispose);
    rfb.add(initRfb());
    rfb.key(0xffe3, true);
    rfb.key(97, true);
    rfb.pointer(1, 1, 5);
    sent.clear();
    rfb.releaseInput();
    rfb.releaseInput();
    expect(sent.where((m) => m[0] == 4).length, 2);
    expect(sent.where((m) => m[0] == 5).length, 1);
    expect(sent.every((m) => m[1] == 0), isTrue);
    rfb.close();
    sent.clear();
    rfb.key(97, true);
    rfb.pointer(0, 0, 1);
    expect(sent, isEmpty);
  });

  test(
    'keysym mappings cover modifiers, navigation, function keys and Unicode',
    () {
      expect(consoleKeysym(LogicalKeyboardKey.controlLeft), 0xffe3);
      expect(consoleKeysym(LogicalKeyboardKey.delete), 0xffff);
      expect(consoleKeysym(LogicalKeyboardKey.f12), 0xffc9);
      expect(consoleKeysym(LogicalKeyboardKey.keyA), 97);
      expect(consoleKeysym(LogicalKeyboardKey.arrowLeft), 0xff51);
    },
  );
}

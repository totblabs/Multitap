import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/console_keyboard.dart';
import 'package:multitap_client/data/console_rfb.dart';

import 'console_rfb_test.dart' as wire;

// Wire fixtures from QEMU v10.0.0 ui/vnc.c (7c949c53e936aa3a658d84ab53bae5cadaa5d59c).
// These exercise the real byte parser/producer, not an actual guest keyboard.
void acknowledge(ConsoleRfb rfb, {int leds = 0}) {
  rfb.add(wire.rfbRect(0, 0, 2, 2, -258, []));
  rfb.add(wire.rfbRect(0, 0, 1, 1, -261, [leds]));
}

void main() {
  test('Windows, Alt/Tab, function and language keys retain exact physical down/up pairs', () {
    // noVNC v1.6.0 xtscancodes.js and QEMU's E0-to-qnum conversion.
    // This checks the negotiated byte contract, not guest shortcut/IME behavior.
    const cases = [
      (0x5b, 0x5b, true, 0xffeb, 0xdb),
      (0x5c, 0x5c, true, 0xffec, 0xdc),
      (0x5d, 0x5d, true, 0xff67, 0xdd),
      (0xa4, 0x38, false, 0xffe9, 0x38),
      (0xa5, 0x38, true, 0xffea, 0xb8),
      (0x09, 0x0f, false, 0xff09, 0x0f),
      (0x70, 0x3b, false, 0xffbe, 0x3b),
      (0x7b, 0x58, false, 0xffc9, 0x58),
      (0x2e, 0x53, true, 0xffff, 0xd3),
      (0x15, 0xf2, false, 0xff31, 0x72),
      (0x19, 0xf1, false, 0xff34, 0x71),
    ];
    final sent = <Uint8List>[];
    final rfb = ConsoleRfb(send: sent.add)..enableQemuKeyboard();
    addTearDown(rfb.dispose);
    rfb.add(wire.initRfb());
    acknowledge(rfb);
    sent.clear();
    for (final (vk, scan, extended, symbol, code) in cases) {
      expect(consoleVirtualKeysym(vk, scan, extended), symbol);
      expect(consoleQemuKeyCode(vk, scan, extended), code);
      final physical = scan * 2 + (extended ? 1 : 0);
      rfb.physicalKey(physical, symbol, true, code);
      // Layout/VK changes on key-up must not change the stored press.
      rfb.physicalKey(physical, 0x61, false, 0x1e);
    }
    expect(sent.length, cases.length * 2);
    for (final entry in cases.indexed) {
      final pair = sent.skip(entry.$1 * 2).take(2).toList();
      expect(pair.map((b) => b[0]), [255, 255]);
      expect(pair.map((b) => ByteData.sublistView(b).getUint16(2)), [1, 0]);
      expect(pair.map((b) => ByteData.sublistView(b).getUint32(4)), [
        entry.$2.$4,
        entry.$2.$4,
      ]);
      expect(pair.map((b) => ByteData.sublistView(b).getUint32(8)), [
        entry.$2.$5,
        entry.$2.$5,
      ]);
    }
    rfb.releaseInput();
    expect(sent.length, cases.length * 2);
  });

  test('QEMU default case-sync reproducer: negotiated LEDs preserve actual Shift/Caps without ASCII rewriting', () {
    for (final negotiate in [false, true]) {
      final guest = _QemuLockContract();
      final rfb = ConsoleRfb(send: guest.accept);
      addTearDown(rfb.dispose);
      if (negotiate) rfb.enableQemuKeyboard();
      rfb.add(wire.initRfb());
      if (negotiate) acknowledge(rfb);
      // Same lowercase symbol and physical Shift on both paths.
      rfb.physicalKey(84, 0xffe1, true, 0x2a);
      rfb.physicalKey(60, 0x61, true, 0x1e);
      rfb.physicalKey(60, 0x61, false, 0x1e);
      rfb.physicalKey(84, 0xffe1, false, 0x2a);
      expect(guest.text, negotiate ? 'A' : 'a');
      expect(guest.syntheticCapsChanges, negotiate ? 0 : 1);
      if (negotiate) {
        rfb.physicalKey(116, 0xffe5, true, 0x3a);
        rfb.physicalKey(116, 0xffe5, false, 0x3a);
        rfb.physicalKey(60, 0x61, true, 0x1e);
        rfb.physicalKey(60, 0x61, false, 0x1e);
        rfb.physicalKey(108, 0xffe2, true, 0x36);
        rfb.physicalKey(60, 0x61, true, 0x1e);
        rfb.releaseKeyboard();
        expect(guest.text, 'AAa');
        expect(guest.caps, isTrue);
        expect(guest.shift, isFalse);
        expect(guest.syntheticCapsChanges, 0);
      }
    }
  });

  test(
    'optional QEMU negotiation needs actual ACK and LED, neither is a picture',
    () {
      final sent = <Uint8List>[];
      final rfb = ConsoleRfb(send: sent.add);
      addTearDown(rfb.dispose);
      expect(rfb.enableQemuKeyboard(), isTrue);
      for (final byte in wire.initRfb()) {
        rfb.add([byte]);
      }
      final encodings = ByteData.sublistView(sent[4]);
      expect(encodings.getUint16(2), 6);
      expect(
        [for (var i = 4; i < sent[4].length; i += 4) encodings.getInt32(i)],
        [0, 1, -223, -239, -258, -261],
      );
      expect(rfb.qemuKeyboardReady, isFalse);
      rfb.add(wire.rfbRect(0, 0, 2, 2, -258, []));
      expect(rfb.qemuKeyboardReady, isFalse);
      for (final byte in wire.rfbRect(0, 0, 1, 1, -261, [4])) {
        rfb.add([byte]);
      }
      expect(rfb.qemuKeyboardReady, isTrue);
      expect(rfb.ledState, 4);
      expect(rfb.frameValid, isFalse);
      expect(rfb.frameRevision, 0);
      expect(rfb.enableQemuKeyboard(), isFalse);
      rfb.add(wire.rfbRect(0, 0, 1, 1, 0, [3, 2, 1, 0]));
      expect(rfb.frameValid, isTrue);
      expect(rfb.frameRevision, 1);
      expect(rfb.pixels.take(4), [1, 2, 3, 255]);
    },
  );

  test(
    'physical down before ACK keeps standard repeat/up; new press uses QEMU',
    () {
      final sent = <Uint8List>[];
      final rfb = ConsoleRfb(send: sent.add)..enableQemuKeyboard();
      addTearDown(rfb.dispose);
      rfb.add(wire.initRfb());
      sent.clear();
      rfb.physicalKey(60, 0x61, true, 0x1e);
      acknowledge(rfb);
      rfb.physicalKey(60, 0x71, true, 0x10);
      rfb.physicalKey(60, 0x71, false, 0x10);
      var keys = sent.where((b) => b[0] == 4 || b[0] == 255).toList();
      expect(keys.map((b) => b[0]), [4, 4, 4]);
      expect(keys.map((b) => ByteData.sublistView(b).getUint32(4)), [
        0x61,
        0x61,
        0x61,
      ]);
      sent.clear();
      rfb.physicalKey(60, 0x61, true, 0x1e);
      rfb.physicalKey(60, 0x71, false, 0x10);
      keys = sent.where((b) => b[0] == 255).toList();
      expect(keys.length, 2);
      expect(keys.map((b) => ByteData.sublistView(b).getUint16(2)), [1, 0]);
      expect(keys.map((b) => ByteData.sublistView(b).getUint32(8)), [
        0x1e,
        0x1e,
      ]);
      expect(keys.map((b) => ByteData.sublistView(b).getUint32(4)), [
        0x61,
        0x61,
      ]);
    },
  );

  test('sided modifiers and Korean language keys use reviewed physical codes; focus all-up balances once', () {
    expect(consoleQemuKeyCode(0x15, 0xf2, false), 0x72);
    expect(consoleQemuKeyCode(0x19, 0xf1, false), 0x71);
    expect(consoleQemuKeyCode(0xa5, 0x38, true), 0xb8);
    expect(consoleQemuKeyCode(0xa3, 0x1d, true), 0x9d);
    expect(consoleQemuKeyCode(0x13, 0x45, false), isNull);
    expect(consoleQemuKeyCode(0x41, 0, false), isNull);
    expect(consoleQemuKeyCode(0x41, 0x11e, false), isNull);
    final sent = <Uint8List>[];
    final rfb = ConsoleRfb(send: sent.add)..enableQemuKeyboard();
    addTearDown(rfb.dispose);
    rfb.add(wire.initRfb());
    acknowledge(rfb);
    sent.clear();
    final presses = [
      (84, 0xffe1, 0x2a),
      (60, 0x61, 0x1e),
      (115, 0xffea, 0xb8),
      (484, 0xff31, 0x72),
      (482, 0xff34, 0x71),
    ];
    for (final p in presses) {
      rfb.physicalKey(p.$1, p.$2, true, p.$3);
    }
    rfb.pointer(1, 1, 1);
    sent.clear();
    rfb.releaseKeyboard();
    rfb.releaseKeyboard();
    expect(sent.length, presses.length);
    expect(
      sent.every(
        (b) => b[0] == 255 && ByteData.sublistView(b).getUint16(2) == 0,
      ),
      isTrue,
    );
    expect(
      sent.map((b) => ByteData.sublistView(b).getUint32(8)),
      presses.map((p) => p.$3),
    );
    rfb.releaseInput();
    expect(sent.last[0], 5);
    expect(sent.last[1], 0);
    rfb.close();
    sent.clear();
    rfb.physicalKey(60, 0x61, true, 0x1e);
    expect(sent, isEmpty);
  });

  test('unrequested extensions, malformed ACK/LED and unknown keycode stay bounded', () {
    for (final qemu in [false, true]) {
      for (final rect in [
        wire.rfbRect(0, 0, 1, 1, -258, []),
        wire.rfbRect(0, 0, 2, 2, -261, [0]),
        wire.rfbRect(0, 0, 1, 1, -261, [8]),
      ]) {
        final rfb = ConsoleRfb(send: (_) {});
        addTearDown(rfb.dispose);
        if (qemu) rfb.enableQemuKeyboard();
        rfb.add(wire.initRfb());
        expect(() => rfb.add(rect), throwsFormatException);
      }
    }
    final sent = <Uint8List>[];
    final rfb = ConsoleRfb(send: sent.add)..enableQemuKeyboard();
    addTearDown(rfb.dispose);
    rfb.add(wire.initRfb());
    acknowledge(rfb);
    sent.clear();
    for (var i = 0; i < 200; i++) {
      rfb.physicalKey(i, 0x61, true, 0x10000);
    }
    expect(sent.length, 128);
    expect(sent.every((b) => b[0] == 4), isTrue);
    rfb.releaseKeyboard();
    expect(sent.length, 256);
  });
}

// Independent, deliberately small protocol model of QEMU do_key_event's
// lock-key-sync default when no LED extension was requested. It is not QEMU
// execution, OS injection, a guest IME, or a real uppercase acceptance result.
class _QemuLockContract {
  bool ledRequested = false, shift = false, caps = false;
  int syntheticCapsChanges = 0;
  String text = '';
  void accept(Uint8List message) {
    if (message[0] == 2) {
      final data = ByteData.sublistView(message);
      ledRequested = [
        for (var i = 0; i < data.getUint16(2); i++) data.getInt32(4 + i * 4),
      ].contains(-261);
      return;
    }
    if (message[0] != 4 && message[0] != 255) return;
    final data = ByteData.sublistView(message);
    final down = message[0] == 4 ? message[1] == 1 : data.getUint16(2) == 1;
    final sym = data.getUint32(4);
    final code = message[0] == 255 ? data.getUint32(8) : null;
    if (code == 0x2a || code == 0x36 || sym == 0xffe1 || sym == 0xffe2) {
      shift = down;
      return;
    }
    if (sym == 0xffe5) {
      if (down) caps = !caps;
      return;
    }
    if (!down || (sym != 0x61 && sym != 0x41)) return;
    if (!ledRequested && caps != ((sym == 0x41) != shift)) {
      caps = !caps;
      syntheticCapsChanges++;
    }
    text += shift != caps ? 'A' : 'a';
  }
}

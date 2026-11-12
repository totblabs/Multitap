import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_cursor.dart';
import 'package:multitap_client/android/android_media.dart';

void _u16(BytesBuilder out, int value) =>
    out.add([value & 0xff, (value >> 8) & 0xff]);
void _u32(BytesBuilder out, int value) => out.add([
  value & 0xff,
  (value >> 8) & 0xff,
  (value >> 16) & 0xff,
  (value >> 24) & 0xff,
]);

/// rows: one string per pixel row, '1' = bit set (MSB first, like Windows).
List<int> _mask(int width, List<String> rows) {
  final stride = ((width + 31) ~/ 32) * 4;
  final out = <int>[];
  for (final row in rows) {
    final bytes = List<int>.filled(stride, 0);
    for (var x = 0; x < row.length; x++) {
      if (row[x] == '1') bytes[x >> 3] |= 0x80 >> (x & 7);
    }
    out.addAll(bytes);
  }
  return out;
}

Uint8List shape({
  required int id,
  required int format,
  required int width,
  required int height,
  int hotX = 0,
  int hotY = 0,
  required List<int> mask,
  List<int> color = const [],
}) {
  final out = BytesBuilder()..add([1, format, 0, 0]);
  _u32(out, id);
  _u16(out, width);
  _u16(out, height);
  _u16(out, hotX);
  _u16(out, hotY);
  out.add(mask);
  out.add(color);
  return out.toBytes();
}

Uint8List state({required bool visible, int id = 0, int? x, int? y}) {
  final out = BytesBuilder()
    ..add([2, (visible ? 1 : 0) | (x == null ? 0 : 2), 0, 0]);
  _u32(out, visible ? id : 0);
  _u32(out, x ?? 0);
  _u32(out, y ?? 0);
  return out.toBytes();
}

List<int> _pixel(AndroidCursorShape s, int x, int y) {
  final i = (y * s.width + x) * 4;
  return s.rgba.sublist(i, i + 4);
}

Uint8List _tinyShape(int id) => shape(
  id: id,
  format: 1,
  width: 1,
  height: 1,
  mask: const [0, 0, 0, 0],
  color: [id & 255, 0, 0, 255],
);

void main() {
  test('initial unknown, explicit hidden and visible-but-missing shape stay '
      'distinct without inventing a cursor image', () {
    final cursor = AndroidCursorState()..source = const Size(100, 100);
    addTearDown(cursor.dispose);
    cursor.pointer(const Offset(.5, .5));
    expect(cursor.reportedVisible, isNull);
    expect(cursor.image, isNull);
    cursor.record(state(visible: false));
    expect(cursor.reportedVisible, isFalse);
    expect(cursor.visible, isFalse);
    expect(cursor.image, isNull);
    cursor.pointer(const Offset(.6, .6));
    expect(cursor.reportedVisible, isFalse);
    cursor.record(state(visible: true, id: 77));
    expect(cursor.reportedVisible, isTrue);
    expect(
      cursor.visible,
      isFalse,
      reason: 'the requested shape has not arrived',
    );
    expect(cursor.image, isNull);
    cursor.reset();
    expect(cursor.reportedVisible, isNull);
    expect(cursor.visible, isFalse);
  });
  TestWidgetsFlutterBinding.ensureInitialized();

  test('hot Host shape survives 64 newer shapes without retransmission', () {
    final cursor = AndroidCursorState();
    addTearDown(cursor.dispose);
    cursor.record(_tinyShape(1));
    cursor.record(state(visible: true, id: 1));
    // Channel::resolve keeps this repeatedly used shape in its 32-entry LRU;
    // it sends only the existing ID when the pointer returns to that shape.
    for (var id = 2; id <= 65; id++) {
      cursor.record(state(visible: true, id: 1));
      cursor.record(_tinyShape(id));
      cursor.record(state(visible: true, id: id));
      expect(cursor.shape?.id, id);
    }
    cursor.record(state(visible: true, id: 1));
    expect(cursor.reportedVisible, isTrue);
    expect(cursor.visible, isTrue, reason: 'Host still references hot shape 1');
    expect(cursor.shape?.id, 1);
  });

  test(
    '32-shape bound evicts a cold shape and keeps the current reference',
    () {
      final cursor = AndroidCursorState();
      addTearDown(cursor.dispose);
      for (var id = 1; id <= 32; id++) {
        cursor.record(_tinyShape(id));
      }
      cursor.record(state(visible: true, id: 1));
      cursor.record(_tinyShape(33));
      // Receiving another image must not switch or evict the current shape.
      expect(cursor.shape?.id, 1);
      cursor.record(state(visible: true, id: 33));
      expect(cursor.shape?.id, 33);
      cursor.record(state(visible: true, id: 2));
      expect(cursor.reportedVisible, isTrue);
      expect(cursor.visible, isFalse, reason: 'cold shape 2 was evicted');
      for (var id = 3; id <= 32; id++) {
        cursor.record(state(visible: true, id: id));
        expect(cursor.shape?.id, id);
      }
      cursor.record(state(visible: true, id: 1));
      expect(cursor.shape?.id, 1);
    },
  );

  test('unknown and hidden references do not consume any cache slots', () {
    final cursor = AndroidCursorState();
    addTearDown(cursor.dispose);
    for (var id = 1; id <= 32; id++) {
      cursor.record(_tinyShape(id));
    }
    for (var id = 33; id <= 65; id++) {
      cursor.record(state(visible: true, id: id));
      expect(cursor.reportedVisible, isTrue);
      expect(cursor.visible, isFalse);
      expect(cursor.shape, isNull);
    }
    cursor.record(state(visible: false));
    expect(cursor.reportedVisible, isFalse);
    for (var id = 1; id <= 32; id++) {
      cursor.record(state(visible: true, id: id));
      expect(cursor.shape?.id, id);
    }
  });

  test('duplicate replaces one slot and reset waits for a new state', () {
    final cursor = AndroidCursorState();
    addTearDown(cursor.dispose);
    for (var id = 1; id <= 32; id++) {
      cursor.record(_tinyShape(id));
    }
    cursor.record(state(visible: true, id: 1));
    cursor.record(
      shape(
        id: 1,
        format: 1,
        width: 1,
        height: 1,
        mask: const [0, 0, 0, 0],
        color: const [7, 8, 9, 255],
      ),
    );
    expect(_pixel(cursor.shape!, 0, 0), [9, 8, 7, 255]);
    cursor.record(state(visible: true, id: 2));
    expect(cursor.shape?.id, 2, reason: 'duplicate did not add a 33rd slot');
    cursor.reset();
    expect(cursor.reportedVisible, isNull);
    expect(cursor.visible, isFalse);
    cursor.record(state(visible: true, id: 1));
    expect(_pixel(cursor.shape!, 0, 0), [9, 8, 7, 255]);
    cursor.record(_tinyShape(33));
    expect(cursor.shape?.id, 1);
    cursor.record(state(visible: true, id: 3));
    expect(
      cursor.visible,
      isFalse,
      reason: 'reset retained the 32-shape bound',
    );
    cursor.record(state(visible: true, id: 2));
    expect(cursor.shape?.id, 2);
  });

  test('monochrome AND/XOR: black, white, transparent and outlined invert', () {
    // 5x3: row 1 has black, white, inverting, transparent, transparent.
    final record = shape(
      id: 1,
      format: 2,
      width: 5,
      height: 3,
      hotX: 2,
      hotY: 1,
      mask: [
        ..._mask(5, ['11111', '00111', '11111']), // AND
        ..._mask(5, ['00000', '01100', '00000']), // XOR
      ],
    );
    final decoded = decodeCursorShape(record)!;
    expect(
      (decoded.width, decoded.height, decoded.hotX, decoded.hotY),
      (5, 3, 2, 1),
    );
    expect(_pixel(decoded, 0, 1), [0, 0, 0, 255]);
    expect(_pixel(decoded, 1, 1), [255, 255, 255, 255]);
    // Inverting pixel: black, with a white outline on transparent neighbours.
    expect(_pixel(decoded, 2, 1), [0, 0, 0, 255]);
    expect(_pixel(decoded, 3, 1), [255, 255, 255, 255]);
    expect(_pixel(decoded, 2, 0), [255, 255, 255, 255]);
    expect(_pixel(decoded, 4, 1), [0, 0, 0, 0]);
    expect(_pixel(decoded, 4, 0), [0, 0, 0, 0]);
  });

  test('colour: alpha cursors keep alpha, others use the AND mask', () {
    final alpha = shape(
      id: 2,
      format: 1,
      width: 2,
      height: 1,
      mask: _mask(2, ['00']),
      color: [10, 20, 30, 128, 1, 2, 3, 0], // BGRA
    );
    final decoded = decodeCursorShape(alpha)!;
    expect(_pixel(decoded, 0, 0), [30, 20, 10, 128]);
    expect(_pixel(decoded, 1, 0), [3, 2, 1, 0]);
    final masked = shape(
      id: 3,
      format: 1,
      width: 2,
      height: 1,
      mask: _mask(2, ['01']),
      color: [10, 20, 30, 0, 0, 0, 0, 0],
    );
    final plain = decodeCursorShape(masked)!;
    expect(_pixel(plain, 0, 0), [30, 20, 10, 255]);
    expect(_pixel(plain, 1, 0), [0, 0, 0, 0]);
  });

  test('malformed shape records are refused', () {
    final good = shape(
      id: 4,
      format: 2,
      width: 1,
      height: 1,
      mask: [
        ..._mask(1, ['0']),
        ..._mask(1, ['0']),
      ],
    );
    expect(decodeCursorShape(good), isNotNull);
    expect(decodeCursorShape(Uint8List.sublistView(good, 0, 20)), isNull);
    final hotspot = shape(
      id: 4,
      format: 2,
      width: 1,
      height: 1,
      hotX: 1,
      mask: [
        ..._mask(1, ['0']),
        ..._mask(1, ['0']),
      ],
    );
    expect(decodeCursorShape(hotspot), isNull);
    expect(decodeCursorShape(state(visible: true, id: 4)), isNull);
  });

  test('state follows shapes, visibility, warps and local pointer', () {
    final cursor = AndroidCursorState()..source = const Size(1000, 500);
    addTearDown(cursor.dispose);
    final arrow = shape(
      id: 7,
      format: 2,
      width: 1,
      height: 1,
      mask: [
        ..._mask(1, ['0']),
        ..._mask(1, ['0']),
      ],
    );
    // A state naming a shape this client has not received stays hidden.
    cursor.record(state(visible: true, id: 7));
    expect(cursor.visible, false);
    cursor.record(arrow);
    cursor.record(state(visible: true, id: 7, x: 250, y: 125));
    expect(cursor.visible, true);
    expect(cursor.shape?.id, 7);
    expect(cursor.position, const Offset(.25, .25));
    cursor.pointer(const Offset(.5, .75));
    expect(cursor.position, const Offset(.5, .75));
    cursor.record(state(visible: false));
    expect(cursor.visible, false);
    cursor.record(state(visible: true, id: 7));
    cursor.reset();
    expect(cursor.visible, false);
  });

  test('Android media presents cursor records without a native call', () async {
    final media = AndroidRemoteMedia();
    addTearDown(media.dispose);
    final record = shape(
      id: 9,
      format: 2,
      width: 1,
      height: 1,
      mask: [
        ..._mask(1, ['0']),
        ..._mask(1, ['0']),
      ],
    );
    // The shared NativeRemoteMedia would invoke the unimplemented native
    // `cursor` method (MissingPluginException) and end the session.
    await media.cursor(1, record);
    await media.cursor(1, state(visible: true, id: 9));
    expect(media.cursorState.visible, true);
  });

  testWidgets('a decoded shape becomes an image drawn by the overlay', (
    tester,
  ) async {
    final cursor = AndroidCursorState()..source = const Size(100, 100);
    final record = shape(
      id: 1,
      format: 2,
      width: 2,
      height: 2,
      mask: [
        ..._mask(2, ['00', '00']),
        ..._mask(2, ['11', '11']),
      ],
    );
    await tester.runAsync(() async {
      cursor.record(record);
      cursor.record(state(visible: true, id: 1, x: 50, y: 50));
      for (var i = 0; i < 50 && cursor.image == null; i++) {
        await Future<void>.delayed(const Duration(milliseconds: 10));
      }
    });
    expect(cursor.image, isNotNull);
    await tester.pumpWidget(
      Directionality(
        textDirection: TextDirection.ltr,
        child: SizedBox(
          width: 200,
          height: 200,
          child: AndroidCursorOverlay(cursor: cursor, sourceWidth: 100),
        ),
      ),
    );
    expect(find.byKey(const Key('android-remote-cursor')), findsOneWidget);
    expect(tester.takeException(), isNull);
    cursor.dispose();
  });
}

import 'dart:collection';
import 'dart:convert';

import 'package:flutter/foundation.dart';

/// RFC 6143, RFB 3.8. Optional QEMU keyboard support requires broker opt-in
/// and actual upstream pseudo-encoding acknowledgements, never just ready.
/// This parser has no socket, OS input, clipboard or credential access.
class ConsoleRfb extends ChangeNotifier {
  ConsoleRfb({required this.send});
  final void Function(Uint8List) send;
  static const maxAxis = 8192, maxPixels = 16777216;
  static const maxPending = 8 * 1024 * 1024, maxChunk = 1024 * 1024;
  final _chunks = Queue<_RfbInput>();
  int _offset = 0, _pending = 0, _stage = 0, _rects = 0;
  int _rawRow = 0;
  _RfbRect? _rect;
  int width = 0, height = 0, frameRevision = 0, cursorRevision = 0;
  Uint8List pixels = Uint8List(0);
  ConsoleCursor? cursor;
  bool initialized = false, closed = false;
  bool frameValid = false, _frameDirty = false, _fullUpdate = false;
  final _keys = <int>{};
  final _physicalKeys = <int, _RfbKey>{};
  bool _qemuRequested = false, _qemuAcknowledged = false;
  int? _ledState;
  int? get ledState => _ledState;
  bool get qemuKeyboardReady => _qemuAcknowledged && _ledState != null;
  int _buttons = 0, _x = 0, _y = 0;

  /// Called only after the exact broker capability, before its RFB banner.
  bool enableQemuKeyboard() {
    if (closed || _stage != 0 || _pending != 0) return false;
    _qemuRequested = true;
    return true;
  }

  Never _invalid() => throw const FormatException('console_invalid_frame');
  void add(List<int> fragment) {
    if (closed) return;
    if (fragment.length > maxChunk || _pending + fragment.length > maxPending) {
      _invalid();
    }
    if (fragment.isNotEmpty) {
      var at = 0;
      while (at < fragment.length) {
        if (_chunks.isEmpty || _chunks.last.used == _chunks.last.bytes.length) {
          _chunks.add(_RfbInput());
        }
        final tail = _chunks.last;
        final take = (fragment.length - at).clamp(
          0,
          tail.bytes.length - tail.used,
        );
        tail.bytes.setRange(tail.used, tail.used + take, fragment, at);
        tail.used += take;
        at += take;
      }
      _pending += fragment.length;
    }
    while (!closed && _step()) {}
  }

  Uint8List _peek(int count) {
    if (count > _pending) _invalid();
    final out = Uint8List(count);
    var written = 0, first = true;
    for (final chunk in _chunks) {
      final begin = first ? _offset : 0;
      first = false;
      final take = (count - written).clamp(0, chunk.used - begin);
      out.setRange(written, written + take, chunk.bytes, begin);
      written += take;
      if (written == count) break;
    }
    return out;
  }

  Uint8List _take(int count) {
    final out = _peek(count);
    var left = count;
    while (left > 0) {
      final remaining = _chunks.first.used - _offset;
      if (left < remaining) {
        _offset += left;
        left = 0;
      } else {
        left -= remaining;
        _chunks.removeFirst();
        _offset = 0;
      }
    }
    _pending -= count;
    return out;
  }

  int _u16(Uint8List b, int at) => ByteData.sublistView(b).getUint16(at);
  int _u32(Uint8List b, int at) => ByteData.sublistView(b).getUint32(at);
  int _i32(Uint8List b, int at) => ByteData.sublistView(b).getInt32(at);
  void _size(int w, int h) {
    if (w < 1 || h < 1 || w > maxAxis || h > maxAxis || w * h > maxPixels) {
      _invalid();
    }
    width = w;
    height = h;
    pixels = Uint8List(w * h * 4);
    frameValid = false;
    _frameDirty = false;
    _fullUpdate = true;
  }

  bool _step() {
    if (_stage == 0) {
      if (_pending < 12) return false;
      if (ascii.decode(_take(12)) != 'RFB 003.008\n') _invalid();
      send(Uint8List.fromList(ascii.encode('RFB 003.008\n')));
      _stage = 1;
      return true;
    }
    if (_stage == 1) {
      if (_pending < 1) return false;
      final n = _peek(1)[0];
      if (n < 1 || n > 64) _invalid();
      if (_pending < n + 1) return false;
      final offered = _take(n + 1).sublist(1);
      if (!offered.contains(1)) _invalid();
      send(Uint8List.fromList([1]));
      _stage = 2;
      return true;
    }
    if (_stage == 2) {
      if (_pending < 4) return false;
      if (_u32(_take(4), 0) != 0) _invalid();
      send(Uint8List.fromList([1])); // shared; broker owns exclusive admission.
      _stage = 3;
      return true;
    }
    if (_stage == 3) {
      if (_pending < 24) return false;
      final header = _peek(24);
      final nameLength = _u32(header, 20);
      if (nameLength > 4096) _invalid();
      if (_pending < 24 + nameLength) return false;
      _take(24 + nameLength); // Never display/log the server-supplied name.
      _size(_u16(header, 0), _u16(header, 2));
      // 32-bit little endian true color, R16/G8/B0. Incoming B,G,R,X -> RGBA.
      send(
        Uint8List.fromList([
          0,
          0,
          0,
          0,
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
        ]),
      );
      final requested = [
        0,
        1,
        -223,
        -239,
        if (_qemuRequested) ...[-258, -261],
      ];
      final encodings = Uint8List(4 + requested.length * 4);
      encodings[0] = 2;
      final data = ByteData.sublistView(encodings)
        ..setUint16(2, requested.length);
      for (final entry in requested.indexed) {
        data.setInt32(4 + entry.$1 * 4, entry.$2);
      }
      send(encodings);
      initialized = true;
      _stage = 4;
      requestUpdate(incremental: false);
      _fullUpdate = false;
      notifyListeners();
      return true;
    }
    final rect = _rect;
    if (rect != null) return _rectData(rect);
    if (_rects > 0) {
      if (_pending < 12) return false;
      final h = _take(12);
      final r = _RfbRect(
        _u16(h, 0),
        _u16(h, 2),
        _u16(h, 4),
        _u16(h, 6),
        _i32(h, 8),
      );
      if (r.encoding == -258) {
        if (!_qemuRequested ||
            r.x != 0 ||
            r.y != 0 ||
            r.w != width ||
            r.h != height) {
          _invalid();
        }
        _qemuAcknowledged = true;
        _finishRect();
      } else if (r.encoding == -223) {
        if (r.x != 0 || r.y != 0 || _rects != 1) _invalid();
        releaseInput();
        _size(r.w, r.h);
        _finishRect();
      } else {
        if (r.encoding == -261) {
          if (!_qemuRequested || r.x != 0 || r.y != 0 || r.w != 1 || r.h != 1) {
            _invalid();
          }
        } else if (r.encoding == -239) {
          if (r.w > 256 ||
              r.h > 256 ||
              (r.w == 0) != (r.h == 0) ||
              (r.w > 0 && (r.x >= r.w || r.y >= r.h))) {
            _invalid();
          }
        } else if (![0, 1].contains(r.encoding) ||
            r.w == 0 ||
            r.h == 0 ||
            r.x + r.w > width ||
            r.y + r.h > height) {
          _invalid();
        }
        _rect = r;
        _rawRow = 0;
      }
      return true;
    }
    if (_pending < 1) return false;
    final type = _peek(1)[0];
    if (type == 0) {
      if (_pending < 4) return false;
      final header = _take(4);
      _rects = _u16(header, 2);
      if (_rects > 4096) _invalid();
      if (_rects == 0) _frameComplete();
      return true;
    }
    if (type == 2) {
      _take(1);
      return true;
    } // Bell, no local effect.
    if (type == 3) {
      // No clipboard capability: discard bounded ServerCutText, never publish it.
      if (_pending < 8) return false;
      final length = _u32(_peek(8), 4);
      if (length > 65536) _invalid();
      if (_pending < 8 + length) return false;
      _take(8 + length);
      return true;
    }
    _invalid(); // Indexed palette/compression/extensions were not requested.
  }

  bool _rectData(_RfbRect r) {
    if (r.encoding == -261) {
      if (_pending < 1) return false;
      final state = _take(1)[0];
      if (state > 7) _invalid();
      _ledState = state; // Actual guest LEDs; never a local guessed lock state.
      _finishRect();
      return true;
    }
    if (r.encoding == 0) {
      if (_pending < r.w * 4) return false;
      final row = _take(r.w * 4);
      final dest = ((r.y + _rawRow) * width + r.x) * 4;
      for (var x = 0; x < r.w; x++) {
        final s = x * 4, d = dest + s;
        pixels[d] = row[s + 2];
        pixels[d + 1] = row[s + 1];
        pixels[d + 2] = row[s];
        pixels[d + 3] = 255;
      }
      if (++_rawRow == r.h) {
        _frameDirty = true;
        _finishRect();
      }
      return true;
    }
    if (r.encoding == 1) {
      if (!frameValid) _invalid();
      if (_pending < 4) return false;
      final from = _take(4), x = _u16(from, 0), y = _u16(from, 2);
      if (x + r.w > width || y + r.h > height) _invalid();
      // Row order + setRange's overlap semantics preserve both overlap axes.
      final backwards = r.y > y;
      for (var row = 0; row < r.h; row++) {
        final dy = backwards ? r.h - 1 - row : row;
        final source = ((y + dy) * width + x) * 4;
        final target = ((r.y + dy) * width + r.x) * 4;
        final copy = Uint8List.fromList(
          pixels.sublist(source, source + r.w * 4),
        );
        pixels.setRange(target, target + copy.length, copy);
      }
      _frameDirty = true;
      _finishRect();
      return true;
    }
    final pixelBytes = r.w * r.h * 4;
    final rowMask = (r.w + 7) ~/ 8;
    final size = pixelBytes + rowMask * r.h;
    if (_pending < size) return false;
    final content = _take(size), rgba = Uint8List(pixelBytes);
    for (var y = 0; y < r.h; y++) {
      for (var x = 0; x < r.w; x++) {
        final at = (y * r.w + x) * 4;
        rgba[at] = content[at + 2];
        rgba[at + 1] = content[at + 1];
        rgba[at + 2] = content[at];
        rgba[at + 3] =
            (content[pixelBytes + y * rowMask + x ~/ 8] & (128 >> (x % 8))) != 0
            ? 255
            : 0;
      }
    }
    cursor = ConsoleCursor(r.w, r.h, r.x, r.y, rgba);
    cursorRevision++;
    _finishRect();
    return true;
  }

  void _finishRect() {
    _rect = null;
    if (--_rects == 0) _frameComplete();
  }

  void _frameComplete() {
    if (_frameDirty) {
      frameRevision++;
      frameValid = true;
    }
    _frameDirty = false;
    notifyListeners();
    requestUpdate(incremental: !_fullUpdate);
    _fullUpdate = false;
  }

  void requestUpdate({required bool incremental}) {
    if (!initialized || closed) return;
    final message = Uint8List(10);
    message[0] = 3;
    message[1] = incremental ? 1 : 0;
    ByteData.sublistView(message)
      ..setUint16(6, width)
      ..setUint16(8, height);
    send(message);
  }

  void key(int keysym, bool down) {
    if (!initialized || closed || keysym < 0 || keysym > 0xffffffff) return;
    if (down) {
      if (_keys.length + _physicalKeys.length >= 128 &&
          !_keys.contains(keysym)) {
        return;
      }
      _keys.add(keysym);
    } else {
      _keys.remove(keysym);
    }
    final message = Uint8List(8);
    message[0] = 4;
    message[1] = down ? 1 : 0;
    ByteData.sublistView(message).setUint32(4, keysym);
    send(message);
  }

  /// Keep each press's original wire protocol even if negotiation completes
  /// between its down, repeat and up. No ASCII case/IME text substitution.
  void physicalKey(int physical, int keysym, bool down, int? qemuKeyCode) {
    if (!initialized ||
        closed ||
        physical < 0 ||
        keysym < 0 ||
        keysym > 0xffffffff) {
      return;
    }
    var press = down ? _physicalKeys[physical] : _physicalKeys.remove(physical);
    if (down && press == null) {
      if (_keys.length + _physicalKeys.length >= 128) return;
      final validCode =
          qemuKeyCode != null &&
          qemuKeyCode >= 1 &&
          qemuKeyCode <= 0xfe &&
          qemuKeyCode != 0x7f &&
          qemuKeyCode != 0x80;
      press = _RfbKey(
        keysym,
        qemuKeyboardReady && validCode ? qemuKeyCode : null,
      );
      _physicalKeys[physical] = press;
    }
    if (press == null) return;
    final code = press.qemuKeyCode;
    final message = Uint8List(code == null ? 8 : 12);
    final data = ByteData.sublistView(message);
    if (code == null) {
      message[0] = 4;
      message[1] = down ? 1 : 0;
    } else {
      message[0] = 255;
      data.setUint16(2, down ? 1 : 0);
      data.setUint32(8, code);
    }
    data.setUint32(4, press.keysym);
    send(message);
  }

  void pointer(int x, int y, int mask) {
    if (!initialized || closed || mask < 0 || mask > 255) return;
    _x = x.clamp(0, width - 1);
    _y = y.clamp(0, height - 1);
    _buttons = mask;
    final message = Uint8List(6);
    message[0] = 5;
    message[1] = mask;
    ByteData.sublistView(message)
      ..setUint16(2, _x)
      ..setUint16(4, _y);
    send(message);
  }

  /// Keyboard ownership may change while the view still owns a pointer drag.
  void releaseKeyboard() {
    if (!closed) {
      for (final press in _physicalKeys.entries.toList()) {
        physicalKey(
          press.key,
          press.value.keysym,
          false,
          press.value.qemuKeyCode,
        );
      }
      for (final keycode in _keys.toList()) {
        key(keycode, false);
      }
    }
    _keys.clear();
    _physicalKeys.clear();
  }

  void releaseInput() {
    releaseKeyboard();
    if (!closed && _buttons != 0) pointer(_x, _y, 0);
    _buttons = 0;
  }

  void close() {
    if (closed) return;
    releaseInput();
    closed = true;
    _chunks.clear();
    _pending = 0;
  }

  @override
  void dispose() {
    close();
    super.dispose();
  }
}

class _RfbKey {
  const _RfbKey(this.keysym, this.qemuKeyCode);
  final int keysym;
  final int? qemuKeyCode;
}

class _RfbRect {
  const _RfbRect(this.x, this.y, this.w, this.h, this.encoding);
  final int x, y, w, h, encoding;
}

class _RfbInput {
  final bytes = Uint8List(65536);
  int used = 0;
}

class ConsoleCursor {
  const ConsoleCursor(this.width, this.height, this.hotX, this.hotY, this.rgba);
  final int width, height, hotX, hotY;
  final Uint8List rgba;
  bool get hidden => width == 0 && height == 0;
}

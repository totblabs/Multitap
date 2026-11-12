import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/widgets.dart';

import '../data/remote_cursor.dart';

/// One cursor-v1 shape as straight RGBA, with its hotspot in source pixels.
class AndroidCursorShape {
  const AndroidCursorShape({
    required this.id,
    required this.width,
    required this.height,
    required this.hotX,
    required this.hotY,
    required this.rgba,
  });
  final int id, width, height, hotX, hotY;
  final Uint8List rgba;
}

int _u16(Uint8List b, int at) => b[at] | (b[at + 1] << 8);
int _u32(Uint8List b, int at) =>
    b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24);
int _i32(Uint8List b, int at) => _u32(b, at).toSigned(32);

/// Decodes a validated cursor-v1 shape record (the payload after the media
/// header). Windows draws inverting pixels against the screen; without that
/// raster operation they become black with a one-pixel white outline, as the
/// protocol describes for non-Windows renderers. Returns null for anything
/// that is not a well-formed shape record.
AndroidCursorShape? decodeCursorShape(Uint8List record) {
  if (!validCursorRecord(record) || record[0] != cursorShapeRecord) {
    return null;
  }
  final format = record[1];
  final id = _u32(record, 4);
  final width = _u16(record, 8), height = _u16(record, 10);
  if (id < 1 || id > 0x7fffffff) return null;
  final stride = ((width + 31) ~/ 32) * 4;
  bool bit(int row, int x) =>
      (record[cursorRecordHeaderBytes + row * stride + (x >> 3)] &
          (0x80 >> (x & 7))) !=
      0;
  final rgba = Uint8List(width * height * 4);
  final invert = List<bool>.filled(width * height, false);
  void put(int i, int r, int g, int b, int a) {
    rgba[i * 4] = r;
    rgba[i * 4 + 1] = g;
    rgba[i * 4 + 2] = b;
    rgba[i * 4 + 3] = a;
  }

  if (format == 2) {
    for (var y = 0; y < height; y++) {
      for (var x = 0; x < width; x++) {
        final i = y * width + x;
        final and = bit(y, x), xor = bit(height + y, x);
        if (!and) {
          xor ? put(i, 255, 255, 255, 255) : put(i, 0, 0, 0, 255);
        } else if (xor) {
          invert[i] = true;
          put(i, 0, 0, 0, 255);
        }
      }
    }
  } else {
    final color = cursorRecordHeaderBytes + stride * height;
    var alpha = false;
    for (var i = 0; i < width * height && !alpha; i++) {
      alpha = record[color + i * 4 + 3] != 0;
    }
    for (var y = 0; y < height; y++) {
      for (var x = 0; x < width; x++) {
        final i = y * width + x, at = color + i * 4;
        final b = record[at], g = record[at + 1], r = record[at + 2];
        if (alpha) {
          put(i, r, g, b, record[at + 3]);
        } else if (!bit(y, x)) {
          put(i, r, g, b, 255);
        } else if (r | g | b != 0) {
          // AND 1 with colour: XORed onto the screen by Windows.
          invert[i] = true;
          put(i, 0, 0, 0, 255);
        }
      }
    }
  }
  for (var y = 0; y < height; y++) {
    for (var x = 0; x < width; x++) {
      if (!invert[y * width + x]) continue;
      for (var dy = -1; dy <= 1; dy++) {
        for (var dx = -1; dx <= 1; dx++) {
          final nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
          final n = ny * width + nx;
          if (rgba[n * 4 + 3] == 0) put(n, 255, 255, 255, 255);
        }
      }
    }
  }
  return AndroidCursorShape(
    id: id,
    width: width,
    height: height,
    hotX: _u16(record, 12),
    hotY: _u16(record, 14),
    rgba: rgba,
  );
}

/// The separated remote cursor of one Android session: shapes, visibility and
/// where it is (normalized 0..1 over the picture). Records come only from a
/// granted cursor-v1 connection; position follows this device's own pointer
/// input, and a host position record (an application warp) moves it.
class AndroidCursorState extends ChangeNotifier {
  static const _cacheLimit = 32;
  final _shapes = <int, AndroidCursorShape>{};
  final _images = <int, ui.Image>{};
  int _shown = 0;
  bool _visible = false, _disposed = false, _hasState = false;
  Offset? _position;

  /// Remote source size; position records are in these pixels.
  Size? source;

  bool get visible => _visible && _shapes.containsKey(_shown);

  /// null: no state for this decoder yet; false: Host explicitly hid it;
  /// true: Host wants it visible (the shape/image may still be unavailable).
  bool? get reportedVisible => _hasState ? _visible : null;
  Offset? get position => _position;
  AndroidCursorShape? get shape => visible ? _shapes[_shown] : null;
  ui.Image? get image => visible ? _images[_shown] : null;

  /// This device moved the remote pointer to [normalized].
  void pointer(Offset normalized) {
    if (_position == normalized) return;
    _position = normalized;
    if (visible) _notify();
  }

  /// Applies one validated record; malformed records are ignored here (the
  /// session already rejected anything [validCursorRecord] refuses).
  void record(Uint8List record) {
    if (_disposed || !validCursorRecord(record)) return;
    if (record[0] == cursorShapeRecord) {
      final shape = decodeCursorShape(record);
      if (shape == null) return;
      _shapes.remove(shape.id);
      _shapes[shape.id] = shape;
      while (_shapes.length > _cacheLimit) {
        final oldest = _shapes.keys.first;
        _shapes.remove(oldest);
        _images.remove(oldest)?.dispose();
      }
      ui.decodeImageFromPixels(
        shape.rgba,
        shape.width,
        shape.height,
        ui.PixelFormat.rgba8888,
        (image) {
          if (_disposed || !_shapes.containsKey(shape.id)) {
            image.dispose();
            return;
          }
          _images.remove(shape.id)?.dispose();
          _images[shape.id] = image;
          if (shape.id == _shown) _notify();
        },
      );
      return;
    }
    final flags = record[1];
    _hasState = true;
    _visible = flags & 1 != 0;
    _shown = _visible ? _u32(record, 4) : 0;
    // Match the Host's LRU: reused shapes arrive as state IDs, not new images.
    final shown = _shapes.remove(_shown);
    if (shown != null) _shapes[_shown] = shown;
    if (flags & 2 != 0) {
      final size = source;
      final x = _i32(record, 8), y = _i32(record, 12);
      if (size != null && size.width > 0 && size.height > 0) {
        _position = Offset(
          (x / size.width).clamp(0.0, 1.0),
          (y / size.height).clamp(0.0, 1.0),
        );
      }
    }
    _notify();
  }

  /// A new decoder (stop/resize) shows nothing until the host's next state.
  void reset() {
    final changed = _hasState || _visible;
    _hasState = false;
    _visible = false;
    if (changed) _notify();
  }

  void _notify() {
    if (!_disposed) notifyListeners();
  }

  @override
  void dispose() {
    _disposed = true;
    for (final image in _images.values) {
      image.dispose();
    }
    _images.clear();
    _shapes.clear();
    super.dispose();
  }
}

/// Draws the remote cursor over the picture (nearest-neighbour, scaled by
/// picture width / source width exactly like the Windows presenter, but at
/// least [minimumScale] logical pixels per cursor pixel: on a phone a
/// 32-pixel cursor at the device's own pixels is about 10 dp, too small to
/// steer in trackpad mode). The hotspot stays where the remote pointer is.
class AndroidCursorOverlay extends StatelessWidget {
  const AndroidCursorOverlay({
    super.key,
    required this.cursor,
    required this.sourceWidth,
    this.minimumScale = 0,
  });
  final AndroidCursorState cursor;
  final int sourceWidth;
  final double minimumScale;

  @override
  Widget build(BuildContext context) => IgnorePointer(
    child: RepaintBoundary(
      child: CustomPaint(
        key: const Key('android-remote-cursor'),
        painter: _CursorPainter(cursor, sourceWidth, minimumScale),
        size: Size.infinite,
      ),
    ),
  );
}

class _CursorPainter extends CustomPainter {
  _CursorPainter(this.cursor, this.sourceWidth, this.minimumScale)
    : super(repaint: cursor);
  final AndroidCursorState cursor;
  final int sourceWidth;
  final double minimumScale;

  @override
  void paint(Canvas canvas, Size size) {
    final image = cursor.image, shape = cursor.shape, at = cursor.position;
    if (image == null || shape == null || at == null || sourceWidth <= 0) {
      return;
    }
    final scale = size.width / sourceWidth < minimumScale
        ? minimumScale
        : size.width / sourceWidth;
    final left = at.dx * size.width - shape.hotX * scale;
    final top = at.dy * size.height - shape.hotY * scale;
    canvas.drawImageRect(
      image,
      Rect.fromLTWH(0, 0, shape.width.toDouble(), shape.height.toDouble()),
      Rect.fromLTWH(left, top, shape.width * scale, shape.height * scale),
      Paint()..filterQuality = FilterQuality.none,
    );
  }

  @override
  bool shouldRepaint(_CursorPainter old) =>
      old.cursor != cursor ||
      old.sourceWidth != sourceWidth ||
      old.minimumScale != minimumScale;
}

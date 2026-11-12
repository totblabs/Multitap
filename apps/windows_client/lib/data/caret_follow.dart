/// caret-v1 (docs/remote-session-protocol.md "Caret follow"): while a client's
/// soft keyboard is open it asks the Host (`{"type":"caret.watch","on":true}`)
/// to report where the text caret of the focused remote application is, as a
/// fixed-size `{"type":"caret",...}` message in the pixels of the streamed
/// output. The client keeps that rectangle in view above the keyboard. No text,
/// window title or application name is ever part of it. Only a connection
/// whose broker granted the protocol (a Host that advertised it) uses it;
/// older peers have no caret and the client falls back to following taps.
library;

const caretProtocol = 'caret-v1';

/// Largest coordinate / output size the relay and Host accept.
const caretMaxPixels = 65535;

/// The client request that starts (on) or stops (off) caret reports.
Map<String, Object?> caretWatchMessage(bool on) => {
  'type': 'caret.watch',
  'on': on,
};

/// One Host report: the caret rectangle in output pixels ([x], [y], [width],
/// [height]) inside an output of [sourceWidth] x [sourceHeight], or no caret
/// ([has] false) when the focused application has none.
class RemoteCaret {
  const RemoteCaret({
    required this.has,
    this.x = 0,
    this.y = 0,
    this.width = 0,
    this.height = 0,
    this.sourceWidth = 0,
    this.sourceHeight = 0,
    this.at,
  });

  final bool has;
  final int x, y, width, height;
  final int sourceWidth, sourceHeight;

  /// When the client received it (monotonic stopwatch microseconds, set by the
  /// session); null for a parsed-only value.
  final Duration? at;

  /// Caret rectangle as fractions (0..1) of the picture: left, top, right,
  /// bottom. Null without a caret or with an empty output.
  ({double left, double top, double right, double bottom})? get fraction {
    if (!has || sourceWidth <= 0 || sourceHeight <= 0) return null;
    return (
      left: x / sourceWidth,
      top: y / sourceHeight,
      right: (x + (width < 1 ? 1 : width)) / sourceWidth,
      bottom: (y + (height < 1 ? 1 : height)) / sourceHeight,
    );
  }

  RemoteCaret stamped(Duration now) => RemoteCaret(
    has: has,
    x: x,
    y: y,
    width: width,
    height: height,
    sourceWidth: sourceWidth,
    sourceHeight: sourceHeight,
    at: now,
  );

  /// Null for anything but the exact report shape.
  static RemoteCaret? parse(Map<String, dynamic> event) {
    if (event['type'] != 'caret') return null;
    final has = event['has'];
    if (has is! bool) return null;
    int? number(String key) {
      final value = event[key];
      return value is int && value >= 0 && value <= caretMaxPixels
          ? value
          : null;
    }

    if (!has) return const RemoteCaret(has: false);
    final x = number('x'), y = number('y');
    final w = number('w'), h = number('h');
    final sw = number('sw'), sh = number('sh');
    if (x == null ||
        y == null ||
        w == null ||
        h == null ||
        sw == null ||
        sh == null ||
        sw < 2 ||
        sh < 2) {
      return null;
    }
    return RemoteCaret(
      has: true,
      x: x,
      y: y,
      width: w,
      height: h,
      sourceWidth: sw,
      sourceHeight: sh,
    );
  }
}

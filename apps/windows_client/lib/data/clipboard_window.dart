import 'clipboard_limits.dart';

/// Raised when an acknowledgement names no sent, unacknowledged unit.
class ClipboardWindowException implements Exception {
  const ClipboardWindowException(this.code);
  final String code;
  @override
  String toString() => code;
}

/// The sender's side of the clipboard sliding window. A "unit" is one data
/// chunk or one item end; acknowledgements are cumulative: an ack names the end
/// of a unit the peer has received and releases that unit and every earlier
/// one. A pair without the formats profile uses a window of one unit (stop and
/// wait), so both modes run through the same sender.
class ClipboardSendWindow {
  ClipboardSendWindow({required this.maxUnits, required this.maxBytes});

  /// The window the limits of a pair allow.
  factory ClipboardSendWindow.forLimits(ClipboardLimits limits) =>
      ClipboardSendWindow(
        maxUnits: limits.windowUnits,
        maxBytes: limits.windowBytes,
      );

  final int maxUnits, maxBytes;
  final _units = <_Unit>[];
  int _bytes = 0;

  int get units => _units.length;
  int get bytes => _bytes;
  bool get isEmpty => _units.isEmpty;

  /// Whether a chunk of [dataBytes] may be sent now.
  bool canSendChunk(int dataBytes) =>
      _units.length < maxUnits && _bytes + dataBytes <= maxBytes;
  bool get canSendItemEnd => _units.length < maxUnits;

  void sentChunk(int index, int endOffset, int dataBytes) {
    if (!canSendChunk(dataBytes)) {
      throw const ClipboardWindowException('clipboard_window_full');
    }
    _units.add(_Unit(false, index, endOffset, dataBytes));
    _bytes += dataBytes;
  }

  void sentItemEnd(int index) {
    if (!canSendItemEnd) {
      throw const ClipboardWindowException('clipboard_window_full');
    }
    _units.add(_Unit(true, index, 0, 0));
  }

  /// Releases up to and including the chunk ending at [offset] in item
  /// [index]; returns the data bytes released. Throws for an ack that names no
  /// pending unit (repeated, out of order or never sent).
  int ackChunk(int index, int offset) => _release(false, index, offset);

  /// Releases up to and including the end of item [index].
  int ackItem(int index) => _release(true, index, 0);

  int _release(bool itemEnd, int index, int end) {
    final at = _units.indexWhere(
      (unit) => unit.itemEnd == itemEnd && unit.index == index && unit.end == end,
    );
    if (at < 0) {
      throw const ClipboardWindowException('clipboard_unexpected_ack');
    }
    var released = 0;
    for (final unit in _units.sublist(0, at + 1)) {
      released += unit.data;
    }
    _units.removeRange(0, at + 1);
    _bytes -= released;
    return released;
  }
}

class _Unit {
  const _Unit(this.itemEnd, this.index, this.end, this.data);
  final bool itemEnd;
  final int index, end, data;
}

/// The receiver's side: when to acknowledge. Stop and wait acknowledges every
/// chunk; a windowed pair after four chunks or 128 KiB, and always at an item
/// end (which the caller acknowledges separately).
class ClipboardAckPolicy {
  ClipboardAckPolicy({
    required this.windowed,
    this.everyChunks = 4,
    this.everyBytes = 128 * 1024,
  });
  final bool windowed;
  final int everyChunks, everyBytes;
  int _chunks = 0, _bytes = 0;

  /// A chunk of [bytes] was applied; true when an ack of it is due now.
  bool chunk(int bytes) {
    _chunks++;
    _bytes += bytes;
    if (!windowed || _chunks >= everyChunks || _bytes >= everyBytes) {
      reset();
      return true;
    }
    return false;
  }

  void reset() {
    _chunks = 0;
    _bytes = 0;
  }
}

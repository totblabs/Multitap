import 'dart:typed_data';

import 'video_geometry.dart';

/// Negotiated ToTBSeamless cursor separation (cursor-v1; see
/// docs/remote-session-protocol.md "Candidate cursor separation"). Only a
/// connection whose broker granted it receives cursor records (binary media
/// kind 3); the host then leaves the cursor out of the video and the native
/// runner shows the remote shape as the real local Windows cursor.
const cursorProtocol = 'cursor-v1';

/// Independent outputs (tracks-v1, 모든 모니터 사용): the same records in a
/// TVT1 envelope of their own (kind 3, connector 255, track epoch 0), never
/// with a position. Granted only together with cursor-v1.
const cursorTracksProtocol = 'cursor-tracks-v1';

/// cursor-echo-v1 (docs/remote-session-protocol.md "Cursor echo"): while
/// 게임 마우스 holds the mouse, the client asks the Host for the VM's real cursor
/// position as it changes (`{"type":"cursor.echo","on":true}`). The positions
/// arrive as ordinary cursor-v1 states with a position; the runner then pins
/// this PC's pointer there. Only together with cursor-v1, on one output.
const cursorEchoProtocol = 'cursor-echo-v1';

/// Released brokers reject this connect field and strip this capability.
bool cursorEchoAdvertised(Object? capabilities) =>
    capabilities is Map &&
    capabilities['cursorProtocol'] == cursorProtocol &&
    capabilities['cursorEchoProtocol'] == cursorEchoProtocol;

/// The client request that starts (on) or stops (off) the echo.
Map<String, Object?> cursorEchoMessage(bool on) => {
  'type': 'cursor.echo',
  'on': on,
};

/// cursor-compose-v1 (docs/remote-session-protocol.md "Cursor in the
/// video"): while 게임 마우스 holds the mouse, the client asks the Host to draw
/// the VM's real cursor into the video (`{"type":"cursor.compose","on":true}`)
/// and shows no cursor of its own, so cursor and game always arrive together
/// (as in Moonlight). Preferred over the echo where granted. Only together
/// with cursor-v1, on one output.
const cursorComposeProtocol = 'cursor-compose-v1';

/// Released brokers reject this connect field and strip this capability.
bool cursorComposeAdvertised(Object? capabilities) =>
    capabilities is Map &&
    capabilities['cursorProtocol'] == cursorProtocol &&
    capabilities['cursorComposeProtocol'] == cursorComposeProtocol;

/// The client request that starts (on) or stops (off) the composed cursor.
Map<String, Object?> cursorComposeMessage(bool on) => {
  'type': 'cursor.compose',
  'on': on,
};

/// Binary media kind of a cursor record (12-byte media header, then one record).
const cursorMediaKind = 3;

const cursorShapeRecord = 1, cursorStateRecord = 2;
const cursorRecordHeaderBytes = 16;
const cursorMaximumSide = 256;

int _u16(Uint8List bytes, int at) => bytes[at] | (bytes[at + 1] << 8);

/// Bounded first check of one cursor record (the payload after the 12-byte
/// media header). The native presenter validates every field again; this
/// rejects what can never be a cursor-v1 record before a platform call.
bool validCursorRecord(Uint8List record) {
  if (record.length < cursorRecordHeaderBytes) return false;
  final type = record[0];
  if (record[2] != 0 || record[3] != 0) return false;
  if (type == cursorStateRecord) {
    return record.length == cursorRecordHeaderBytes && (record[1] & ~3) == 0;
  }
  if (type != cursorShapeRecord) return false;
  final format = record[1];
  if (format != 1 && format != 2) return false;
  final width = _u16(record, 8), height = _u16(record, 10);
  if (width < 1 ||
      height < 1 ||
      width > cursorMaximumSide ||
      height > cursorMaximumSide ||
      _u16(record, 12) >= width ||
      _u16(record, 14) >= height) {
    return false;
  }
  final stride = ((width + 31) ~/ 32) * 4;
  final mask = stride * height * (format == 2 ? 2 : 1);
  final color = format == 1 ? width * height * 4 : 0;
  return record.length == cursorRecordHeaderBytes + mask + color;
}

/// Picture geometry handed to the native presenter: the picture rectangle in
/// logical pixels of the Flutter view and the remote source size.
class CursorView {
  const CursorView({
    required this.left,
    required this.top,
    required this.width,
    required this.height,
    required this.sourceWidth,
    required this.sourceHeight,
  });
  final double left, top, width, height;
  final int sourceWidth, sourceHeight;
  bool get valid =>
      left.isFinite &&
      top.isFinite &&
      width.isFinite &&
      height.isFinite &&
      width > 0 &&
      height > 0 &&
      sourceWidth > 0 &&
      sourceHeight > 0 &&
      sourceWidth <= maximumGeometrySide &&
      sourceHeight <= maximumGeometrySide;
  Map<String, Object> toJson() => {
    'left': left,
    'top': top,
    'width': width,
    'height': height,
    'sourceWidth': sourceWidth,
    'sourceHeight': sourceHeight,
  };
  @override
  bool operator ==(Object other) =>
      other is CursorView &&
      other.left == left &&
      other.top == top &&
      other.width == width &&
      other.height == height &&
      other.sourceWidth == sourceWidth &&
      other.sourceHeight == sourceHeight;
  @override
  int get hashCode =>
      Object.hash(left, top, width, height, sourceWidth, sourceHeight);
}

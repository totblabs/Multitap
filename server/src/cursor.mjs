import { HttpError } from './errors.mjs';
import { maximumGeometrySide } from './video-geometry.mjs';
import { relativeMouseTracksCapability } from './relative-mouse.mjs';

// Negotiated ToTBSeamless cursor separation (cursor-v1; see
// docs/remote-session-protocol.md "Candidate cursor separation"). The host
// leaves the cursor out of the video and sends its shape, visibility and rare
// position as binary media kind 3. Every field exists only in a session whose
// client requested the protocol, whose host advertised it and whose broker
// granted it in the connect response; released peers never see any of it.
export const cursorProtocol = 'cursor-v1';
// Independent outputs (tracks-v1): the same records on the session's track
// tunnel in a TVT1 envelope of their own. Granted only together with
// cursor-v1, to a client that asked for both and a Host that advertised both.
export const cursorTracksProtocol = 'cursor-tracks-v1';
export const cursorKind = 3;
export const cursorMaxShapeId = 2147483647;
export const cursorMaxSide = 256;
// Twice the host's own bound of 50 cursor messages per second; beyond it the
// host channel is dropped like any other rate violation.
export const cursorMessagesPerSecond = 100;
export const cursorBytesPerSecond = 4 * 1024 * 1024;
const header = 12, recordHeader = 16, trackHeader = 28, maxTrackPacket = 4 * 1024 * 1024;
// Cursor echo (cursor-echo-v1; docs/remote-session-protocol.md "Cursor
// echo"): while the client holds the mouse (게임 마우스) it asks the Host for the
// VM's real cursor position as it changes ({type:'cursor.echo', on}); the Host
// sends ordinary state records with a position, at most one per 1.5 ms. Only
// with cursor-v1, for an ordinary (single-output) stream.
export const cursorEchoProtocol = 'cursor-echo-v1';
// Twice the Host's own bound while echoing (50 records and 667 echoes a second).
export const cursorEchoMessagesPerSecond = 1500;

// Host capability: exactly the marker; anything else is omitted (the session
// keeps the composed cursor) and never invalidates the other capabilities.
export function cursorCapability(value) {
  return value?.cursorProtocol === cursorProtocol;
}

// Host capability for the echo: both markers exactly.
export function cursorEchoCapability(value) {
  return cursorCapability(value) && value?.cursorEchoProtocol === cursorEchoProtocol;
}

// Connect body: the echo marker only with cursor-v1 in a stream. Malformed
// requests are refused before any session; independent outputs simply leave
// it ungranted.
export function requestedCursorEcho(mode, protocol, echo) {
  if (echo === undefined) return false;
  if (mode !== 'stream' || protocol !== cursorProtocol || echo !== cursorEchoProtocol) throw new HttpError(400, 'invalid_cursor_protocol');
  return true;
}

// Never a reason to refuse a connection: granted only with cursor-v1 itself,
// for a single output, to a Host that advertised it.
export function negotiateCursorEcho(requested, cursorGranted, host, tracks) {
  return requested === true && cursorGranted === true && !tracks && cursorEchoCapability(host?.capabilities);
}

// Client request during the session: exactly {type:'cursor.echo', on:boolean}.
export function validCursorEcho(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const keys = Object.keys(value);
  return keys.length === 2 && keys.includes('type') && keys.includes('on') && value.type === 'cursor.echo' && typeof value.on === 'boolean';
}

// Cursor in the video (cursor-compose-v1; docs/remote-session-protocol.md
// "Cursor in the video"): while the client holds the mouse (게임 마우스) it asks
// the Host to draw the VM's real cursor into the video frames
// ({type:'cursor.compose', on}) and shows no cursor of its own, so the cursor
// and the game always arrive in the same frame. The Host reports the cursor
// hidden meanwhile (as for an unrepresentable shape); no new record and no
// higher bound. Same negotiation rules as cursor-echo-v1.
export const cursorComposeProtocol = 'cursor-compose-v1';

export function cursorComposeCapability(value) {
  return cursorCapability(value) && value?.cursorComposeProtocol === cursorComposeProtocol;
}

export function requestedCursorCompose(mode, protocol, compose) {
  if (compose === undefined) return false;
  if (mode !== 'stream' || protocol !== cursorProtocol || compose !== cursorComposeProtocol) throw new HttpError(400, 'invalid_cursor_protocol');
  return true;
}

// Independent outputs (with cursor-tracks-v1, which `cursorGranted` then is)
// only for a Host that takes 게임 마우스 there (relative-mouse-tracks-v1): the
// output the cursor is over composes it.
export function negotiateCursorCompose(requested, cursorGranted, host, tracks) {
  return requested === true && cursorGranted === true && cursorComposeCapability(host?.capabilities) && (!tracks || relativeMouseTracksCapability(host.capabilities));
}

// Exactly {type:'cursor.compose', on:boolean}.
export function validCursorCompose(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const keys = Object.keys(value);
  return keys.length === 2 && keys.includes('type') && keys.includes('on') && value.type === 'cursor.compose' && typeof value.on === 'boolean';
}

// Host capability for independent outputs: both markers exactly.
export function cursorTracksCapability(value) {
  return cursorCapability(value) && value?.cursorTracksProtocol === cursorTracksProtocol;
}

// Connect body field. Malformed requests are refused before any session.
export function requestedCursor(mode, protocol) {
  if (protocol === undefined) return false;
  if (mode !== 'stream' || protocol !== cursorProtocol) throw new HttpError(400, 'invalid_cursor_protocol');
  return true;
}

// Connect body: the independent-outputs marker only with cursor-v1 and
// tracks-v1. Returns whether it was requested.
export function requestedCursorTracks(mode, protocol, outputs, tracks) {
  if (outputs === undefined) return false;
  if (mode !== 'stream' || protocol !== cursorProtocol || outputs !== cursorTracksProtocol || !tracks) throw new HttpError(400, 'invalid_cursor_protocol');
  return true;
}

// Separation never blocks a connection: an incapable host, or independent
// outputs (tracks-v1) without cursor-tracks-v1 on both sides, leave it
// ungranted (the composed cursor).
export function negotiateCursor(requested, host, tracks, outputs = false) {
  return requested === true && (tracks ? outputs === true && cursorTracksCapability(host?.capabilities) : cursorCapability(host?.capabilities));
}

export function cursorState() {
  return { lastShapeId: 0, windowStart: 0, messages: 0, bytes: 0, sourceWidth: 0, sourceHeight: 0 };
}

const maskStride = width => ((width + 31) >>> 5) * 4;

// Stateless structure of one kind-3 media message: the 12-byte media header
// (kind 3, flags 0) and exactly one shape or state record. Returns null for
// anything malformed.
export function parseCursorRecord(data) {
  if (!Buffer.isBuffer(data) || data.length < header + recordHeader || data[0] !== cursorKind || data[1] !== 0) return null;
  return parseRecordAt(data, header);
}

// The one record that fills `data` from `at` on.
function parseRecordAt(data, at) {
  const type = data[at], length = data.length - at;
  if (data[at + 2] !== 0 || data[at + 3] !== 0) return null;
  const id = data.readUInt32LE(at + 4);
  if (type === 1) {
    const format = data[at + 1];
    const width = data.readUInt16LE(at + 8), height = data.readUInt16LE(at + 10);
    const hotspotX = data.readUInt16LE(at + 12), hotspotY = data.readUInt16LE(at + 14);
    if ((format !== 1 && format !== 2) || id < 1 || id > cursorMaxShapeId || width < 1 || width > cursorMaxSide ||
        height < 1 || height > cursorMaxSide || hotspotX >= width || hotspotY >= height) return null;
    const stride = maskStride(width), rows = format === 1 ? height : 2 * height;
    if (length !== recordHeader + stride * rows + (format === 1 ? width * height * 4 : 0)) return null;
    // Bits past `width` in every mask row are zero.
    const used = (width + 7) >>> 3, tail = width % 8 ? 0xff >>> (width % 8) : 0;
    for (let row = 0; row < rows; row++) {
      const start = at + recordHeader + row * stride;
      if (tail && (data[start + used - 1] & tail)) return null;
      for (let n = used; n < stride; n++) if (data[start + n]) return null;
    }
    return { type: 'shape', format, id, width, height, hotspotX, hotspotY };
  }
  if (type === 2) {
    const flags = data[at + 1], visible = (flags & 1) !== 0, position = (flags & 2) !== 0;
    const x = data.readInt32LE(at + 8), y = data.readInt32LE(at + 12);
    if (length !== recordHeader || (flags & ~3) || (visible ? id < 1 || id > cursorMaxShapeId : id !== 0) ||
        (position ? x < 0 || y < 0 || x >= maximumGeometrySide || y >= maximumGeometrySide : x !== 0 || y !== 0)) return null;
    return { type: 'state', id, visible, position, x, y };
  }
  return null;
}

// A kind-3 message from the host bound to `session`. Returns 'invalid' for a
// protocol violation (the caller drops the host as invalid_media), 'rate'
// beyond the per-session bound (dropped as host_rate_limit), 'discard' for a
// well-formed record that belongs to no current decoder (ungranted session or
// a retired worker's packet in transit, stale epoch, pending resize, before
// the epoch's first video key frame) and 'forward' for the exact bytes to send
// to this session's controller. Cursor records are never video.
export function cursorPacket(session, data, now = Date.now()) {
  const record = parseCursorRecord(data);
  if (!record) return 'invalid';
  const state = session?.cursor;
  if (!state || session.mode !== 'stream' || session.trackState) return 'discard';
  if (now - state.windowStart >= 1000) { state.windowStart = now; state.messages = 0; state.bytes = 0; }
  const limit = session.cursorEcho === true ? cursorEchoMessagesPerSecond : cursorMessagesPerSecond;
  if (++state.messages > limit || (state.bytes += data.length - header) > cursorBytesPerSecond) return 'rate';
  if (!session.streamReady || session.resize || session.awaitingKeyFrame || data.readUInt16LE(2) !== session.epoch) return 'discard';
  if (record.type === 'shape') {
    // Shape ids strictly increase within the session, across epochs.
    if (record.id <= state.lastShapeId) return 'invalid';
    state.lastShapeId = record.id;
    return 'forward';
  }
  if (record.visible && record.id > state.lastShapeId) return 'invalid';
  if (record.position && (record.x >= state.sourceWidth || record.y >= state.sourceHeight)) return 'invalid';
  return 'forward';
}

// cursor-tracks-v1: a TVT1 message of kind 3 from the track tunnel bound to
// `session` (track-media.mjs never accepts kind 3): connector 255, track epoch
// 0, flags 0, the session's topology and exactly one record, never with a
// position. Same results as cursorPacket. A session without the grant keeps
// refusing it as invalid track media; records of an older topology, during a
// resize or before every output's first key frame are discarded.
export function trackCursorPacket(session, data, now = Date.now()) {
  if (!Buffer.isBuffer(data) || data.length < trackHeader + recordHeader || data.length > maxTrackPacket ||
      data.toString('latin1', 0, 4) !== 'TVT1' || data[4] !== cursorKind || data[5] !== 0 || data[6] !== 255 || data[7] !== 0 ||
      data.readUInt32LE(24) !== data.length - trackHeader || data.readUInt32LE(12) !== 0 || !Number.isSafeInteger(Number(data.readBigUInt64LE(16)))) return 'invalid';
  const topology = data.readUInt32LE(8), record = parseRecordAt(data, trackHeader);
  if (!topology || !record || (record.type === 'state' && record.position)) return 'invalid';
  const state = session?.cursor, tracks = session?.trackState;
  if (!state || session.cursorTracks !== true || !tracks) return 'invalid';
  if (now - state.windowStart >= 1000) { state.windowStart = now; state.messages = 0; state.bytes = 0; }
  if (++state.messages > cursorMessagesPerSecond || (state.bytes += data.length - trackHeader) > cursorBytesPerSecond) return 'rate';
  const info = tracks.info;
  if (!info || tracks.pending || topology < info.topologyEpoch) return 'discard';
  if (topology !== info.topologyEpoch) return 'invalid';
  if ([...tracks.video.values()].some(output => !output.key)) return 'discard';
  if (record.type === 'shape') {
    if (record.id <= state.lastShapeId) return 'invalid';
    state.lastShapeId = record.id;
    return 'forward';
  }
  return record.visible && record.id > state.lastShapeId ? 'invalid' : 'forward';
}

// The accepted stream.info source size bounds later position records.
export function cursorSource(session, info) {
  if (!session.cursor) return;
  session.cursor.sourceWidth = info.sourceWidth;
  session.cursor.sourceHeight = info.sourceHeight;
}

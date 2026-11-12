import { HttpError } from './errors.mjs';

// Negotiated relative mouse input (relative-mouse-v1; see
// docs/remote-session-protocol.md). While a remote application hides
// the Windows cursor the client captures the mouse and sends movement, and the
// buttons and wheel at the current remote cursor (no position to send). Every
// field exists only in a session whose client requested it, whose Host
// advertised it and whose broker granted it; released peers keep positions.
export const relativeMouseProtocol = 'relative-mouse-v1';
export const relativeMouseMaxDelta = 16384;

// Host capability: exactly the marker; anything else is omitted.
export function relativeMouseCapability(value) {
  return value?.relativeMouseProtocol === relativeMouseProtocol;
}

// 게임 마우스 on independent outputs (모든 모니터): the Host takes movement and
// cursor-compose-v1 over its tracks connection. Released Hosts refuse both
// there (an unknown control type ends their track connection).
export const relativeMouseTracksProtocol = 'relative-mouse-tracks-v1';
export function relativeMouseTracksCapability(value) {
  return relativeMouseCapability(value) && value?.relativeMouseTracksProtocol === relativeMouseTracksProtocol;
}

// Connect body field. Malformed requests are refused before any session.
export function requestedRelativeMouse(mode, protocol) {
  if (protocol === undefined) return false;
  if (mode !== 'stream' || protocol !== relativeMouseProtocol) throw new HttpError(400, 'invalid_relative_mouse_protocol');
  return true;
}

// Never a reason to refuse a connection: an incapable Host, or one output of
// two monitors (movement could leave the streamed output) stay absolute.
// Independent outputs only when every monitor is a streamed track, the Host
// takes it there and cursor-compose-v1 was granted (`composed`): the VM's
// cursor is drawn into the output it is over, wherever movement takes it.
export function negotiateRelativeMouse(requested, host, tracks, composed = false) {
  if (requested !== true || !relativeMouseCapability(host?.capabilities)) return false;
  if (tracks) return composed === true && relativeMouseTracksCapability(host.capabilities) && (host.capabilities.monitors ?? []).every(monitor => tracks.some(track => track.id === monitor.id));
  return !(host.capabilities.monitors?.length > 1);
}

// The same input in a granted tracks-v1 session carries its topologyEpoch (and
// never `sample`: tracks are not measured by input-latency-v1).
export function validRelativeTrackMouse(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || Object.hasOwn(value, 'sample') || !Object.hasOwn(value, 'topologyEpoch')) return false;
  const { topologyEpoch: _topology, ...base } = value;
  return validRelativeMouse(base);
}

// Whether this client input needs the grant: a movement, or a button or wheel
// step without a position. Released sessions refuse these as invalid_input.
export function relativeMouseInput(value) {
  return value.type === 'input' && (value.event === 'moveRelative' ||
    ((value.event === 'button' || value.event === 'wheel') && !Object.hasOwn(value, 'x') && !Object.hasOwn(value, 'y')));
}

const delta = value => Number.isInteger(value) && Math.abs(value) <= relativeMouseMaxDelta;
const exactly = (value, names) => {
  const allowed = [...names, ...(Object.hasOwn(value, 'sample') ? ['sample'] : [])], keys = Object.keys(value);
  return keys.length === allowed.length && keys.every(key => allowed.includes(key));
};

// Exact fields and bounds. `sample` is admitted afterwards like on `move`.
export function validRelativeMouse(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || value.type !== 'input') return false;
  if (value.event === 'moveRelative') return exactly(value, ['type', 'event', 'dx', 'dy']) && delta(value.dx) && delta(value.dy) && (value.dx !== 0 || value.dy !== 0);
  if (value.event === 'button') return exactly(value, ['type', 'event', 'button', 'down']) && ['left', 'right', 'middle', 'x1', 'x2'].includes(value.button) && typeof value.down === 'boolean';
  if (value.event === 'wheel') return exactly(value, ['type', 'event', 'delta']) && Number.isInteger(value.delta) && Math.abs(value.delta) <= 12000;
  return false;
}

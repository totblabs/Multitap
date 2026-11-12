import { HttpError } from './errors.mjs';

// Negotiated text-caret reports (caret-v1; see docs/remote-session-protocol.md
// "Caret follow"). A client with a soft keyboard (the Android app) asks the
// Host, while the keyboard is open, to report where the text caret of the
// focused remote application is, so it can keep the field being typed in above
// the keyboard. The report is numbers only (pixels of the streamed output):
// never text, a window title or an application name.
// Every field exists only in a session whose client requested it, whose Host
// advertised it and whose broker granted it; released peers never see it.
export const caretProtocol = 'caret-v1';
const maxPixels = 65535;

// Host capability: exactly the marker; anything else is omitted.
export function caretCapability(value) {
  return value?.caretProtocol === caretProtocol;
}

// Connect body field. Malformed requests are refused before any session.
export function requestedCaret(mode, protocol) {
  if (protocol === undefined) return false;
  if (mode !== 'stream' || protocol !== caretProtocol) throw new HttpError(400, 'invalid_caret_protocol');
  return true;
}

// Never a reason to refuse a connection: an incapable Host, or independent
// outputs (tracks-v1), leave it ungranted.
export function negotiateCaret(requested, host, tracks) {
  return requested === true && !tracks && caretCapability(host?.capabilities);
}

// Client request during the session: exactly {type:'caret.watch', on:boolean}.
export function validCaretWatch(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const keys = Object.keys(value);
  return keys.length === 2 && keys.includes('type') && keys.includes('on') && value.type === 'caret.watch' && typeof value.on === 'boolean';
}

// At most this many caret reports per second per session are relayed (the Host sends about 15); the rest are dropped.
export const caretMaxPerSecond = 30;
export function caretAllowed(state, now = Date.now()) {
  if (now - state.at >= 1000) { state.at = now; state.count = 0; }
  return ++state.count <= caretMaxPerSecond;
}

const pixel = value => Number.isInteger(value) && value >= 0 && value <= maxPixels;

// The Host's report, reduced to its fixed fields; null when malformed (the
// Host is then dropped like any other invalid control).
export function caretReport(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || value.type !== 'caret') return null;
  const { sessionId, has } = value;
  if (typeof sessionId !== 'string' || sessionId.length < 1 || sessionId.length > 80 || typeof has !== 'boolean') return null;
  const keys = Object.keys(value);
  if (!has) return keys.length === 3 ? { type: 'caret', sessionId, has: false } : null;
  const fields = ['x', 'y', 'w', 'h', 'sw', 'sh'];
  if (keys.length !== 9 || !fields.every(key => pixel(value[key])) || value.sw < 2 || value.sh < 2) return null;
  // The rectangle lies inside the streamed output (the Host clips it); a report that does not is malformed, not clamped.
  if (value.w < 1 || value.h < 1 || value.x + value.w > value.sw || value.y + value.h > value.sh) return null;
  return { type: 'caret', sessionId, has: true, x: value.x, y: value.y, w: value.w, h: value.h, sw: value.sw, sh: value.sh };
}

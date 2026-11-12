import { HttpError } from './errors.mjs';

// Negotiated touch contacts (touch-input-v1; see
// docs/remote-session-protocol.md "Touch input"). A client with a touch
// screen (the Android app's direct touch mode) sends the contacts of each
// touch frame instead of emulating a mouse; the Host injects them as Windows
// touch input (InjectTouchInput), so Windows applications get real pans,
// pinch-zoom and press-and-hold. Every field exists only in a session whose
// client requested it, whose Host advertised it and whose broker granted it;
// released peers keep mouse/key input only.
export const touchInputProtocol = 'touch-input-v1';
// Simultaneous contacts per frame (the Host's injection context holds 10).
export const touchInputMaxContacts = 10;
const touchPhases = new Set(['down', 'move', 'up', 'cancel']);
const contactRequired = ['id', 'phase', 'x', 'y'];
const contactAllowed = new Set([...contactRequired, 'pressure', 'w', 'h']);

// Host capability: exactly the marker; anything else is omitted.
export function touchInputCapability(value) {
  return value?.touchInputProtocol === touchInputProtocol;
}

// Connect body field. Malformed requests are refused before any session.
export function requestedTouchInput(mode, protocol) {
  if (protocol === undefined) return false;
  if (mode !== 'stream' || protocol !== touchInputProtocol) throw new HttpError(400, 'invalid_touch_input_protocol');
  return true;
}

// Never a reason to refuse a connection: an incapable Host, or independent
// outputs (tracks-v1 keeps pointer/keys), leave it ungranted.
export function negotiateTouchInput(requested, host, tracks) {
  return requested === true && !tracks && touchInputCapability(host?.capabilities);
}

const unit = value => typeof value === 'number' && Number.isFinite(value) && value >= 0 && value <= 1;
const size = value => typeof value === 'number' && Number.isFinite(value) && value > 0 && value <= 1;

// One contact: id (integer 0..2^31-1, unique in its frame), phase, x/y
// normalized 0..1 over the streamed source rectangle (the mouse input space),
// optional pressure 0..1 and optional contact size w/h (both or neither,
// fractions of the source width/height, > 0 and <= 1). No other field.
export function validTouchContact(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const keys = Object.keys(value);
  if (keys.some(key => !contactAllowed.has(key)) || contactRequired.some(key => !Object.hasOwn(value, key))) return false;
  if (!Number.isInteger(value.id) || value.id < 0 || value.id > 0x7fffffff || !touchPhases.has(value.phase) || !unit(value.x) || !unit(value.y)) return false;
  if (Object.hasOwn(value, 'pressure') && !unit(value.pressure)) return false;
  const sized = Object.hasOwn(value, 'w');
  if (sized !== Object.hasOwn(value, 'h') || (sized && (!size(value.w) || !size(value.h)))) return false;
  return true;
}

// One client touch frame: exactly type, event and 1..10 valid contacts with
// distinct ids. Never sampled for input latency.
export function validTouchInput(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const keys = Object.keys(value);
  if (keys.length !== 3 || !keys.every(key => key === 'type' || key === 'event' || key === 'contacts')) return false;
  const { type, event, contacts } = value;
  if (type !== 'input' || event !== 'touch' || !Array.isArray(contacts) || contacts.length < 1 || contacts.length > touchInputMaxContacts) return false;
  if (!contacts.every(validTouchContact)) return false;
  return new Set(contacts.map(contact => contact.id)).size === contacts.length;
}

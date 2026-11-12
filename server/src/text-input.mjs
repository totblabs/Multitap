import { HttpError } from './errors.mjs';

// Negotiated committed-text input (text-input-v1; see
// docs/remote-session-protocol.md "Text input"). A client whose keyboard or
// IME produces composed text (e.g. Korean on a phone) sends
// {"type":"input","event":"text","text":...}; the Host injects it as Unicode
// key events, independent of the remote layout and IME mode. Every field
// exists only in a session whose client requested it, whose Host advertised
// it and whose broker granted it in the connect response; released peers
// keep sending and receiving ordinary key events only.
export const textInputProtocol = 'text-input-v1';
// UTF-16 code units per message (the Host's bound too); clients split longer
// commits. 256 units stay far below the 8 KiB control message limit.
export const textInputMaxUnits = 256;

// Host capability: exactly the marker; anything else is omitted.
export function textInputCapability(value) {
  return value?.textInputProtocol === textInputProtocol;
}

// Connect body field. Malformed requests are refused before any session.
export function requestedTextInput(mode, protocol) {
  if (protocol === undefined) return false;
  if (mode !== 'stream' || protocol !== textInputProtocol) throw new HttpError(400, 'invalid_text_input_protocol');
  return true;
}

// Never a reason to refuse a connection: an incapable Host, or independent
// outputs (tracks-v1 keeps keys only), leave it ungranted.
export function negotiateTextInput(requested, host, tracks) {
  return requested === true && !tracks && textInputCapability(host?.capabilities);
}

// One client text message: exactly type, event and a well-formed string of
// 1..256 UTF-16 units without control characters (C0, DEL, C1). Enter, Tab
// and Backspace are key events, never text.
export function validTextInput(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const keys = Object.keys(value);
  if (keys.length !== 3 || !keys.every(key => key === 'type' || key === 'event' || key === 'text')) return false;
  const { type, event, text } = value;
  return type === 'input' && event === 'text' && typeof text === 'string' && text.length >= 1 && text.length <= textInputMaxUnits &&
    text.isWellFormed() && !/[\u0000-\u001f\u007f-\u009f]/u.test(text);
}

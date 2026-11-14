import { HttpError } from './errors.mjs';

// Gamepads (gamepad-v1; docs/remote-session-protocol.md "Gamepads"): the client
// reads the gamepads of its PC or phone and sends each pad's whole state
// ({type:'gamepad.state', pad, buttons, lt, rt, lx, ly, rx, ry}, XInput's
// values) and its removal ({type:'gamepad.remove', pad}); the Host plugs in a
// virtual Xbox 360 controller per pad and sends the rumble the VM's game asks
// for ({type:'gamepad.rumble', sessionId, pad, low, high}). Ordinary stream
// sessions only, up to four pads.
export const gamepadProtocol = 'gamepad-v1';
const maxPads = 4;
const int = (value, low, high) => Number.isInteger(value) && value >= low && value <= high;
const exact = (value, keys) => !!value && typeof value === 'object' && !Array.isArray(value) && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));

export function gamepadCapability(value) {
  return value?.gamepadProtocol === gamepadProtocol;
}

// Connect body field. A malformed request is refused before any session.
export function requestedGamepad(mode, value) {
  if (value === undefined) return false;
  if (mode !== 'stream' || value !== gamepadProtocol) throw new HttpError(400, 'invalid_gamepad_protocol');
  return true;
}

export function negotiateGamepad(requested, host, tracks) {
  return requested === true && !tracks && gamepadCapability(host?.capabilities);
}

// Exactly one pad's whole state, XInput's ranges (buttons: XINPUT_GAMEPAD_* bits).
export function validGamepadState(value) {
  return exact(value, ['type', 'pad', 'buttons', 'lt', 'rt', 'lx', 'ly', 'rx', 'ry']) && value.type === 'gamepad.state' && int(value.pad, 0, maxPads - 1) &&
    int(value.buttons, 0, 0xFFFF) && (value.buttons & 0x0800) === 0 && int(value.lt, 0, 255) && int(value.rt, 0, 255) &&
    ['lx', 'ly', 'rx', 'ry'].every(key => int(value[key], -32768, 32767));
}

export function validGamepadRemove(value) {
  return exact(value, ['type', 'pad']) && value.type === 'gamepad.remove' && int(value.pad, 0, maxPads - 1);
}

// The Host's rumble report for the client: exact fields; the client gets it without the sessionId.
export function gamepadRumble(value) {
  if (!exact(value, ['type', 'sessionId', 'pad', 'low', 'high']) || value.type !== 'gamepad.rumble' || typeof value.sessionId !== 'string' || !int(value.pad, 0, maxPads - 1) || !int(value.low, 0, 255) || !int(value.high, 0, 255)) return null;
  return { type: 'gamepad.rumble', sessionId: value.sessionId, pad: value.pad, low: value.low, high: value.high };
}

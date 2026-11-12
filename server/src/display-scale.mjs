import { HttpError } from './errors.mjs';

// Negotiated remote display scale (display-scale-v1; see
// docs/remote-session-protocol.md "Display scale"). A client that requests the
// remote output at its own physical pixels (an Android phone) keeps text
// readable by asking the Host to set the Windows display scale (per-monitor
// DPI) of the owned virtual output it streams, the way Microsoft Remote
// Desktop sends a desktop scale factor. The Host applies it only to that
// positively identified owned output, clamps it to what Windows allows for the
// output size and restores the previous value when the session ends.
// Every field exists only in a session whose client requested it, whose Host
// advertised it and whose broker granted it; released peers never see it.
export const displayScaleProtocol = 'display-scale-v1';
// Windows' scale steps (percent).
export const displayScalePercents = Object.freeze([100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500]);
// Independent outputs (display-scale-outputs-v1): the same request for every
// owned output of a tracks-v1 session, each clamped by Windows on its own; the
// result adds each output's own values. Granted only together with
// display-scale-v1, to a client that asked for it and a Host that advertised it.
export const displayScaleOutputsProtocol = 'display-scale-outputs-v1';
// Host result codes: 0 applied, 1 output not found, 2 unreadable, 3 refused
// by Windows, 4 recovery record unavailable, 5 unsupported output.
const displayScaleCodes = 5;

export function validDisplayScale(value) {
  return Number.isInteger(value) && displayScalePercents.includes(value);
}

// Host capability: exactly the marker; anything else is omitted.
export function displayScaleCapability(value) {
  return value?.displayScaleProtocol === displayScaleProtocol;
}

// Host capability for independent outputs: both markers exactly.
export function displayScaleOutputsCapability(value) {
  return displayScaleCapability(value) && value?.displayScaleOutputsProtocol === displayScaleOutputsProtocol;
}

// Connect body: the outputs marker only with display-scale-v1 and tracks-v1.
// Returns whether it was requested.
export function requestedDisplayScaleOutputs(mode, protocol, outputs, tracks) {
  if (outputs === undefined) return false;
  if (mode !== 'stream' || protocol !== displayScaleProtocol || outputs !== displayScaleOutputsProtocol || !tracks) throw new HttpError(400, 'invalid_display_scale');
  return true;
}

// Connect body fields. Malformed requests are refused before any session.
// An omitted percent negotiates runtime control without changing the current
// Windows scale. null is not an omitted field and remains malformed.
// Returns the requested initial percent, or null when none was requested.
export function requestedDisplayScale(mode, protocol, scale) {
  if (protocol === undefined && scale === undefined) return null;
  if (mode !== 'stream' || protocol !== displayScaleProtocol || (scale !== undefined && !validDisplayScale(scale))) throw new HttpError(400, 'invalid_display_scale');
  return scale ?? null;
}

// Separate the runtime grant from the optional initial DPI request. The
// authenticated connect request and advertised Host capability are both
// required; independent outputs receive the grant only with
// display-scale-outputs-v1 requested ([outputs]) and advertised.
function scaleGranted(host, tracks, outputs) {
  return tracks ? outputs && displayScaleOutputsCapability(host?.capabilities) : displayScaleCapability(host?.capabilities);
}
export function negotiateDisplayScaleControl(protocol, host, tracks, outputs = false) {
  return protocol === displayScaleProtocol && scaleGranted(host, tracks, outputs);
}

// Never a reason to refuse a connection: an incapable Host, or independent
// outputs (tracks-v1) without display-scale-outputs-v1, leave it ungranted.
export function negotiateDisplayScale(requested, host, tracks, outputs = false) {
  return requested !== null && scaleGranted(host, tracks, outputs) ? requested : null;
}

// One client request during the session: exactly {type, scale}.
export function validDisplayScaleRequest(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const keys = Object.keys(value);
  return keys.length === 2 && keys.includes('type') && keys.includes('scale') && value.type === 'display.scale' && validDisplayScale(value.scale);
}

// One output's (or the summary's) values.
function validScaleValues({ ok, scale, maximum, code }) {
  return typeof ok === 'boolean' && (scale === 0 || validDisplayScale(scale)) && (maximum === 0 || validDisplayScale(maximum)) &&
    Number.isInteger(code) && code >= 0 && code <= displayScaleCodes && !(ok && (code !== 0 || scale === 0));
}
// display-scale-outputs-v1: one entry per output, exactly {id, ok, scale,
// maximum, code}, ids '0'/'1' once each.
function scaleOutputs(value) {
  if (!Array.isArray(value) || value.length < 1 || value.length > 2) return null;
  const outputs = [];
  for (const item of value) {
    if (!item || typeof item !== 'object' || Array.isArray(item) || Object.keys(item).sort().join(',') !== 'code,id,maximum,ok,scale' ||
        !['0', '1'].includes(item.id) || outputs.some(output => output.id === item.id) || !validScaleValues(item)) return null;
    outputs.push({ id: item.id, ok: item.ok, scale: item.scale, maximum: item.maximum, code: item.code });
  }
  return outputs;
}

// The Host's result for this session, reduced to its fixed fields. null when
// malformed (the Host is then dropped like any other invalid control). A
// session granted display-scale-outputs-v1 ([outputs]) needs `outputs`; any
// other session never has it.
export function displayScaleResult(value, outputs = false) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  const allowed = ['type', 'sessionId', 'protocol', 'ok', 'scale', 'maximum', 'code', ...(outputs ? ['outputs'] : [])];
  if (Object.keys(value).some(key => !allowed.includes(key))) return null;
  const { sessionId, protocol, ok, scale, maximum, code } = value;
  if (value.type !== 'display.scale.applied' || protocol !== displayScaleProtocol ||
      typeof sessionId !== 'string' || sessionId.length < 1 || sessionId.length > 80 || !validScaleValues(value)) return null;
  const list = outputs ? scaleOutputs(value.outputs) : null;
  if (outputs && !list) return null;
  return { type: 'display.scale.applied', sessionId, protocol, ok, scale, maximum, code, ...(list ? { outputs: list } : {}) };
}

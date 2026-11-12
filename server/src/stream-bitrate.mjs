import { HttpError } from './errors.mjs';

// Adaptive ToTBSeamless bitrate (bitrate-adaptive-v1, docs/adaptive-bitrate.md).
// Released hosts/clients never see any of these fields: a request beyond the
// released 4-20 Mbps fixed range is accepted only for a host that advertises
// the protocol, and only then does the client send arrival feedback.
export const bitrateProtocol = 'bitrate-adaptive-v1';
export const legacyMaxBitrate = 20000000, adaptiveMaxBitrate = 100000000, adaptiveStartBitrate = 20000000;
const integer = (n, min, max) => Number.isSafeInteger(n) && n >= min && n <= max;

// Host capability: both fields or neither. Malformed values are omitted (the
// desktop stays fixed-rate) and never invalidate the other capabilities.
export function bitrateCapability(value) {
  if (value?.bitrateProtocol !== bitrateProtocol || !integer(value.maxBitrate, legacyMaxBitrate, adaptiveMaxBitrate)) return null;
  return { bitrateProtocol, maxBitrate: value.maxBitrate };
}

// Connect-time rule once the host is known. Returns whether the session runs
// the adaptive controller; throws before any session state exists.
export function negotiateBitrate(options, host, tracks) {
  const capability = bitrateCapability(host?.capabilities);
  const adaptive = options.bitrateMode === 'auto';
  if (!adaptive && options.bitrate <= legacyMaxBitrate) return false;
  // Independent outputs (tracks-v1) keep a fixed total rate.
  if (adaptive && tracks) throw new HttpError(400, 'invalid_stream_options');
  if (!capability) {
    // Exactly the released answer for an out-of-range fixed bitrate.
    if (!adaptive) throw new HttpError(400, 'invalid_stream_options');
    throw new HttpError(409, 'stream_bitrate_unavailable');
  }
  if (options.bitrate > capability.maxBitrate) throw new HttpError(409, 'stream_bitrate_unavailable');
  return adaptive;
}

// session.start bitrate fields. Auto: start at min(20 Mbps, ceiling); the
// options' bitrate is the ceiling.
export function startBitrateFields(options, adaptive) {
  return adaptive ? { bitrate: Math.min(adaptiveStartBitrate, options.bitrate), bitrateMode: 'auto', bitrateMax: options.bitrate } : { bitrate: options.bitrate };
}

// Client arrival report: host PTS and client monotonic arrival per video packet.
export function validBitrateFeedback(value) {
  return !!value && typeof value === 'object' && !Array.isArray(value) &&
    Object.keys(value).every(key => ['type', 'epoch', 'frames', 'bytes', 'intervalUs'].includes(key)) &&
    value.type === 'stream.feedback' && integer(value.epoch, 0, 65535) && integer(value.bytes, 0, 1e9) && integer(value.intervalUs, 1, 10e6) &&
    Array.isArray(value.frames) && value.frames.length <= 64 &&
    value.frames.every(frame => Array.isArray(frame) && frame.length === 2 && integer(frame[0], 0, Number.MAX_SAFE_INTEGER) && integer(frame[1], 0, Number.MAX_SAFE_INTEGER));
}

// What the host's controller receives: the report plus the relay's own
// untransmitted bytes toward this client (its queue on the relay->client leg).
export function bitrateFeedbackForHost(session, value) {
  return { type: 'stream.feedback', sessionId: session.id, epoch: value.epoch, frames: value.frames, bytes: value.bytes, intervalUs: value.intervalUs, relayQueuedBytes: session.ws?.bufferedAmount ?? 0 };
}

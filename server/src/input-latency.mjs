import { HttpError } from './errors.mjs';

// Negotiated ToTBSeamless input latency measurement (input-latency-v1; see
// docs/remote-session-protocol.md "Input latency measurement"). Every field is
// additive and exists only in a session whose client requested the protocol,
// whose host advertised it and whose broker granted it in the connect response.
// Released clients/hosts never send or receive any of it. The relay forwards
// only sequence numbers, durations and a video PTS: no key code, coordinate or
// other input content is part of a report.
export const inputLatencyProtocol = 'input-latency-v1';
export const inputLatencyMaxSample = 2147483647;
// Twice the client's own bound of 10 sampled inputs per second. A client
// beyond it still has its input delivered, only without the sample field, so a
// misbehaving controller cannot multiply host acknowledgements.
export const inputLatencySamplesPerSecond = 20;
export const inputLatencyMaxUs = 10000000;
// Brokers that send this marker in session.start accept a tag carrying the
// host's per-input durations (host clock, us): injection -> capture tick of
// the changed frame (`captureUs`), that tick -> encoder output (`encodeUs`),
// encoder output -> this tag's write (`queueUs`). Hosts without it, or a
// released broker's session, keep the exact five-field tag.
export const inputFrameTimingProtocol = 'frame-timing-v1';
const outstandingLimit = 64;
const integer = (n, min, max) => Number.isSafeInteger(n) && n >= min && n <= max;

// Host capability: exactly the marker; anything else is omitted (the session
// stays unmeasured) and never invalidates the other capabilities.
export function inputLatencyCapability(value) {
  return value?.inputLatencyProtocol === inputLatencyProtocol;
}

// Connect body field. Malformed requests are refused before any session.
export function requestedInputLatency(mode, protocol) {
  if (protocol === undefined) return false;
  if (mode !== 'stream' || protocol !== inputLatencyProtocol) throw new HttpError(400, 'invalid_input_latency_protocol');
  return true;
}

// Measurement never blocks a connection: an incapable host, or independent
// outputs (tracks-v1, not measured by this protocol), leave it unmeasured.
export function negotiateInputLatency(requested, host, tracks) {
  return requested === true && !tracks && inputLatencyCapability(host?.capabilities);
}

export function inputLatencyState() {
  return { lastSample: 0, windowStart: 0, windowCount: 0, outstanding: new Set(), lastTagged: 0, forwardPts: -1, forward: [] };
}

// Relay share of a tagged video packet (diagnostics only): receipt of the
// host's complete message -> the controller socket accepted the write (ws send
// callback; a backed-up controller connection shows here). Only the packet the
// last forwarded tag names is timed; a later packet ends the wait.
const forwardWindowMs = 30000, forwardLimit = 64;
export function inputLatencyTimedPacket(state, packet) {
  if (!state || state.forwardPts < 0 || packet.length < 12 || packet[0] !== 1) return false;
  const pts = Number(packet.readBigUInt64LE(4));
  if (pts < state.forwardPts) return false;
  const named = pts === state.forwardPts;
  state.forwardPts = -1;
  return named;
}
export function recordInputLatencyForward(state, ms, now = Date.now()) {
  if (!state || !Number.isFinite(ms) || ms < 0) return;
  state.forward.push([Math.min(ms, 10000), now]);
  while (state.forward.length > forwardLimit) state.forward.shift();
}
// The stream.stats member, {samples, p50Ms, p95Ms} (nearest rank, last 30 s), or null.
export function inputLatencyForwardStats(state, now = Date.now()) {
  const values = (state?.forward ?? []).filter(([, at]) => now - at <= forwardWindowMs).map(([ms]) => ms).sort((a, b) => a - b);
  if (!values.length) return null;
  const rank = fraction => values[Math.min(values.length - 1, Math.max(0, Math.ceil(fraction * values.length) - 1))];
  const round = value => Math.round(value * 100) / 100;
  return { samples: values.length, p50Ms: round(rank(.5)), p95Ms: round(rank(.95)) };
}

// A client control value carrying `sample`. Returns the value to forward
// (possibly without `sample` above the rate bound) or null when it is invalid
// input for this session. Values without the field are returned unchanged.
export function admitInputSample(session, value, now = Date.now()) {
  if (!Object.hasOwn(value, 'sample')) return value;
  const state = session.inputLatency;
  if (!state || value.type !== 'input' || !integer(value.sample, 1, inputLatencyMaxSample) || value.sample <= state.lastSample) return null;
  state.lastSample = value.sample;
  if (now - state.windowStart >= 1000) { state.windowStart = now; state.windowCount = 0; }
  if (++state.windowCount > inputLatencySamplesPerSecond) {
    const { sample: _unmeasured, ...input } = value;
    return input;
  }
  state.outstanding.add(value.sample);
  if (state.outstanding.size > outstandingLimit) state.outstanding.delete(state.outstanding.values().next().value);
  return value;
}

const ackFields = ['type', 'sessionId', 'sample', 'ok', 'injectUs', 'holdUs'];
const frameFields = ['type', 'sessionId', 'epoch', 'sample', 'pts'];
const frameTimingFields = ['captureUs', 'encodeUs', 'queueUs'];

// Host `input.ack` / `input.frame` on the reusable host channel. Returns null
// for a protocol violation (the caller drops the host like any invalid
// control), false for a stale or unmatched report (ignored) and otherwise the
// exact object to forward to this session's controller.
export function inputLatencyHostMessage(session, value) {
  // A tag may carry the host's durations (frame-timing-v1, granted with every
  // measured session by this broker); otherwise the exact released fields.
  const timed = value.type === 'input.frame' && frameTimingFields.some(key => Object.hasOwn(value, key));
  const fields = value.type === 'input.ack' ? ackFields : timed ? [...frameFields, ...frameTimingFields] : frameFields;
  if (Object.keys(value).length !== fields.length || !fields.every(key => Object.hasOwn(value, key)) || typeof value.sessionId !== 'string') return null;
  if (timed && !frameTimingFields.every(key => integer(value[key], 0, inputLatencyMaxUs))) return null;
  // A retired worker's late report cannot reach its successor.
  if (value.sessionId !== session.id) return false;
  const state = session.inputLatency;
  if (!state || session.mode !== 'stream' || !integer(value.sample, 1, state.lastSample)) return null;
  if (value.type === 'input.ack') {
    if (typeof value.ok !== 'boolean' || !integer(value.injectUs, 0, inputLatencyMaxUs) || !integer(value.holdUs, value.injectUs, inputLatencyMaxUs)) return null;
    // Each forwarded sample is acknowledged at most once.
    if (!state.outstanding.delete(value.sample)) return false;
    return { type: 'input.ack', sessionId: session.id, sample: value.sample, ok: value.ok, injectUs: value.injectUs, holdUs: value.holdUs };
  }
  if (!integer(value.epoch, 0, 65535) || !integer(value.pts, 0, Number.MAX_SAFE_INTEGER)) return null;
  // Tags follow the media they describe: none during a resize or for an old epoch.
  if (value.sample <= state.lastTagged || !session.streamReady || session.resize || value.epoch !== session.epoch) return false;
  state.lastTagged = value.sample;
  state.forwardPts = value.pts;
  return { type: 'input.frame', sessionId: session.id, epoch: value.epoch, sample: value.sample, pts: value.pts, ...(timed ? { captureUs: value.captureUs, encodeUs: value.encodeUs, queueUs: value.queueUs } : {}) };
}

// A direct injection can beat its reliable WSS copy to the broker. Validate
// and briefly hold its real worker report instead of dropping the Host for a
// sample whose registration is still in flight. Never invent an injection.
export function holdEarlyDirectInputReport(session,value,now=Date.now()){
  const state=session.inputLatency;
  if(!session.udpDirect||!state||!integer(value.sample,state.lastSample+1,inputLatencyMaxSample))return undefined;
  const probe={...session,inputLatency:{...state,lastSample:value.sample,outstanding:new Set([value.sample])}};
  const report=inputLatencyHostMessage(probe,value);
  if(report===null)return null;
  if(report===false)return true;
  const held=state.earlyDirect??=new Map();
  for(const [key,row] of held)if(now-row.at>2000)held.delete(key);
  held.set(value.type+':'+value.sample,{value,at:now});
  while(held.size>64)held.delete(held.keys().next().value);
  return true;
}
export function takeEarlyDirectInputReports(session,sample,now=Date.now()){
  const held=session.inputLatency?.earlyDirect,output=[];if(!held)return output;
  for(const [key,row] of held){
    if(now-row.at>2000){held.delete(key);continue;}
    if(row.value.sample!==sample)continue;
    held.delete(key);const report=inputLatencyHostMessage(session,row.value);if(report)output.push(report);
  }
  return output;
}

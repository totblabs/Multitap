import { HttpError } from './errors.mjs';

// Passive, negotiated snapshots. Observations never authorize a session or
// influence media delivery, readiness, queue admission or rate control.
export const observabilityProtocol = 'stream-observability-v1';
export const hostObservabilityBounds = Object.freeze({
  hostQueuePackets: 4096, hostQueueBytes: 16777216,
  hostQueueOldestUs: 60000000, hostWriteUs: 60000000,
  readbackUs: 60000000, readbackMaxUs: 60000000,
  serviceCpuPct10: 1000, workerCpuPct10: 1000,
});
export const observabilityDiagnosticBounds = Object.freeze({
  ...hostObservabilityBounds,
  relayQueueBytes: 16777216, relayForwardUs: 60000000,
  relayVideoQueuePackets: 4096, relayVideoQueueOldestUs: 60000000,
  clientInboxPackets: 4096, clientInboxBytes: 16777216,
  clientInboxOldestUs: 60000000,
  inputGateWaitUs: 60000000, inputGateWaitMaxUs: 60000000,
  inputGateWaitCount: 0xffffffff, inputGateQueuePackets: 128,
});
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const bounded = (value, maximum) => Number.isSafeInteger(value) && value >= 0 && value <= maximum;

export function requestedObservability(mode, protocol) {
  if (protocol === undefined) return false;
  if (mode !== 'stream' || protocol !== observabilityProtocol) throw new HttpError(400, 'invalid_observability_protocol');
  return true;
}
export const observabilityCapability = value => value?.observabilityProtocol === observabilityProtocol;

export function safeObservabilityMetrics(value, bounds = hostObservabilityBounds) {
  if (!object(value)) return null;
  const result = {};
  for (const [key, maximum] of Object.entries(bounds)) if (Object.hasOwn(value, key) && bounded(value[key], maximum)) result[key] = value[key];
  return Object.keys(result).length ? result : null;
}

// Called only with the access context resolved by the broker. Forwarded headers
// by themselves cannot manufacture proxy evidence. Legacy contexts stay unknown.
export function observabilityTls(req, context) {
  if (context?.proxied === true) return 'proxy';
  if (req.socket?.encrypted === true) return 'direct';
  if (context?.origin?.startsWith('http:') && context?.proxied === false) return 'plain';
  return 'unknown';
}
export const safeObservabilityTls = value => ['direct', 'proxy', 'plain'].includes(value) ? value : 'unknown';

// Version evidence only: package metadata and an authenticated Host's report do
// not prove the installed executable's integrity or runtime acceptance.
export const safeObservabilityVersion = value => typeof value === 'string' && /^\d{1,6}\.\d{1,6}\.\d{1,6}$/.test(value) ? value : null;

export function withoutObservability(value) {
  if (!Object.hasOwn(value, 'observability')) return value;
  const { observability: _observation, ...base } = value;
  return base;
}

// Only local video writes not yet completed by ws. This never holds payloads,
// reorders sends or applies flow control. Overflow makes the observation unknown.
export function beginObservabilityForward(session, epoch, now = performance.now()) {
  if (!session.observability || !Number.isFinite(now)) return null;
  let state = session.observabilityPending;
  if (!state || state.epoch !== epoch) session.observabilityPending = state = { epoch, pending:new Map(), serial:0, overflow:false };
  const key = ++state.serial;
  if (state.pending.size >= 4096) state.overflow = true;
  else state.pending.set(key, now);
  return { state, key, started:now, epoch };
}

export function completeObservabilityForward(session, ticket, success, now = performance.now()) {
  if (!ticket) return;
  ticket.state.pending.delete(ticket.key);
  if (success && session.observabilityPending === ticket.state) recordObservabilityForward(session, ticket.started, ticket.epoch, now);
}

export function clearObservability(session) {
  session.observabilityPending?.pending.clear();
  session.observabilityPending = null;
  session.observabilityForward = null;
}

// Snapshot times are all local. A completed ws write is not peer receipt.
export function recordObservabilityForward(session, started, epoch, now = performance.now()) {
  const currentEpoch = session.trackState?.info?.topologyEpoch ?? session.epoch;
  if (!session.observability || epoch !== currentEpoch || !Number.isFinite(started) || now < started) return;
  const us = Math.round((now - started) * 1000);
  if (bounded(us, 60000000)) session.observabilityForward = { us, epoch, at: now };
}

export function observabilityStats(session, value, now = performance.now()) {
  const base = withoutObservability(value);
  if (!session.observability || value.sessionId !== session.id) return base;
  const epoch = session.trackState?.info?.topologyEpoch ?? session.epoch;
  if (session.trackState ? value.topologyEpoch !== epoch || session.trackState.pending : (value.epoch ?? 0) !== epoch || session.resize) return base;
  const raw = value.observability;
  const metrics = object(raw) && raw.protocol === observabilityProtocol && object(raw.metrics)
    ? safeObservabilityMetrics(raw.metrics) ?? {} : {};
  const queued = session.ws?.bufferedAmount;
  if (bounded(queued, 16777216)) metrics.relayQueueBytes = queued;
  const pending = session.observabilityPending;
  if (pending && !pending.overflow && pending.epoch === epoch) {
    metrics.relayVideoQueuePackets = pending.pending.size;
    const first = pending.pending.values().next();
    const ageUs = first.done ? 0 : Math.round((now - first.value) * 1000);
    if (bounded(ageUs, 60000000)) metrics.relayVideoQueueOldestUs = ageUs;
  }
  const sent = session.observabilityForward;
  if (sent && sent.epoch === epoch && now >= sent.at && now - sent.at <= 3000) metrics.relayForwardUs = sent.us;
  return Object.keys(metrics).length ? { ...base, observability: { protocol: observabilityProtocol, metrics } } : base;
}

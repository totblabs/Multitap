import { timingSafeEqual } from 'node:crypto';
import { newToken, tokenHash } from './security.mjs';
import { channelResumeProtocol } from './channel-resume.mjs';
import { neutralGamepadState } from './gamepad.mjs';

// Transparent resume (tuning backlog #32). A short loss of the client's or the
// Host's transport no longer ends a stream: the broker keeps the session (its
// grant, its Host worker and the prepared display) for a grace window and the
// returning side continues it. The picture restarts at an IDR; nothing about
// the display is prepared again.
//
// session-resume-v1, client <-> broker:
//   * The desktop list shows `sessionResumeProtocol` for a Host that codes an
//     IDR on request (keyframe-v1); a client sends the field back in its
//     connect body only then (released brokers refuse unknown fields) and the
//     grant is `sessionResumeProtocol` + `resumeGraceMs` in the response.
//   * On every attach the client gets {type:'session.resume.token', token,
//     graceMs}, on a resumed attach {type:'session.resumed', token, graceMs}.
//     The token is single-use: an attempt spends it, a resumed attach issues
//     the next one. Only its SHA-256 is kept, it is never logged or audited.
//   * A socket that ends without a close frame (or with a transport error,
//     the heartbeat or the send bound) leaves the session waiting. A close
//     frame is a deliberate end, as before.
//   * The client resumes by opening the same session URL with its bearer token
//     and `X-Multitap-Resume: <token>`. Bound to the account (bearer hash),
//     the desktop and the session (the URL); a socket the broker still holds
//     is replaced.
//   * While the Host is away the client hears {type:'session.resuming'} and,
//     when it is back, {type:'session.resumed'} without a token.
// host-resume-v1, Host <-> broker:
//   * A Host that keeps its worker through a lost channel advertises
//     `hostResumeProtocol`; the capabilities ACK carries it back when this
//     broker can resume. Its first word on the new channel is
//     {type:'host.session.resume', sessions:[id]} (none: []); the answer is
//     {type:'host.session.resumed', sessionId} or a plain session.stop.
// tracks-resume-v1, independent outputs (tracks-v1, "모든 모니터"):
//   * Only from a Host that advertises `trackResumeProtocol`: its track tunnel
//     takes {type:'stream.keyframe', sessionId} (an IDR on every output), and a
//     session.start carrying the marker keeps its worker through a lost track
//     tunnel and dials the tunnel again. The client's grant and messages are
//     the ones above; a client that did not ask gets neither.
//   * A resume of either socket, or of the Host's control channel, restarts
//     every output at an IDR (TrackStream.restart, asked again while it is
//     late); the outputs, their topology and media epochs stay as they were.
//   * A track tunnel that ends without a close frame (or with a transport
//     error, or silent past the heartbeat) leaves the session waiting for the
//     Host's next tunnel; a tunnel the broker still holds is replaced by it.
//     The client hears session.resuming / session.resumed as for the Host.
// Resizes in flight and streams without a first picture keep the released
// behaviour: they end.
export const sessionResumeProtocol = 'session-resume-v1';
export const hostResumeProtocol = 'host-resume-v1';
export const trackResumeProtocol = 'tracks-resume-v1';
export const resumeHeader = 'x-multitap-resume';
const tokenPattern = /^[A-Za-z0-9_-]{43}$/;
const sessionPattern = /^[0-9a-f-]{36}$/;
// Cursor shapes a resumed client never saw: the newest ones, bounded.
const maxShapes = 32, maxShapeBytes = 2 * 1024 * 1024;
// Transport losses only. Everything a peer said or did on purpose ends the session.
const hostLosses = new Set(['host_timeout', 'host_backpressure']);

// The client marker in the desktop list: the Host can restart the picture.
export function sessionResumeCapability(capabilities) {
  return capabilities?.keyFrameProtocol === 'keyframe-v1';
}
export function requestedSessionResume(mode, protocol) {
  return mode === 'stream' && protocol === sessionResumeProtocol;
}
export function negotiateSessionResume(requested, host, tracks) {
  return requested === true && sessionResumeCapability(host?.capabilities) && (!tracks || trackResumeCapability(host?.capabilities));
}
export function hostResumeCapability(value) {
  return value?.hostResumeProtocol === hostResumeProtocol;
}
export function trackResumeCapability(value) {
  return value?.trackResumeProtocol === trackResumeProtocol;
}
// A granted stream that already shows pictures and is not changing.
export function resumable(session) {
  return session.resume === true && session.mode === 'stream' && (session.trackState ? session.trackState.established : session.streamReady === true) &&
    !session.resize && !session.profilePreparing && !session.initialFrameDeadline;
}
export function issueResumeToken(session) {
  const token = newToken();
  session.resumeHash = tokenHash(token);
  return token;
}
// Spent by any attempt, matching or not.
export function spendResumeToken(session, token) {
  const expected = session.resumeHash;
  session.resumeHash = null;
  if (typeof expected !== 'string' || typeof token !== 'string' || !tokenPattern.test(token)) return false;
  return timingSafeEqual(Buffer.from(expected, 'hex'), Buffer.from(tokenHash(token), 'hex'));
}

export class SessionResume {
  constructor(relay, { clientGraceMs = 15000, hostGraceMs = 30000 } = {}) {
    Object.assign(this, { relay, clientGraceMs, hostGraceMs });
  }
  // Either side (or the Host's track tunnel) is away and the session waits for it.
  held(session) { return !!(session.detached || session.hostHeld || session.trackHeld); }
  // The client socket is gone (abrupt end, transport error, heartbeat or send
  // bound): keep the session if it can resume. False: end it as before.
  detachClient(session, reason) {
    const r = this.relay, ws = session.ws;
    if (!ws || session.detached || r.sessions.get(session.id) !== session || !resumable(session)) return false;
    const now = Date.now();
    session.ws = null;
    session.detached = { reason, at: now, until: now + this.clientGraceMs, info: null, shapes: [], shapeBytes: 0, state: null };
    try { ws.terminate(); } catch { /* already closed */ }
    this.resetInput(session);
    // channel-resume-v1: the side channels end with the socket and wait for it (the Host is told).
    if (r.sessions.get(session.id) === session) r.channels?.clientAway(session);
    return true;
  }
  resetInput(session) {
    const r = this.relay;
    // Nothing the client held stays pressed while nobody is there (independent
    // outputs take input on their track tunnel).
    const reset = { type: 'input.reset', sessionId: session.id };
    if (session.trackState) { if (session.trackTunnel) r.send(session.trackTunnel, reset); }
    else if (!session.hostHeld) {
      if (!r.send(session.host.ws, reset)) return r.dropHost(session.host, 'host_backpressure');
      // gamepad-v1: input.reset does not reach the Host's virtual pads (its own devices, not the worker's input):
      // each pad the client used is set neutral. The client sends its pads' states again once it is back.
      for (const pad of session.gamepad ? session.gamepadPads ?? [] : []) if (!r.send(session.host.ws, neutralGamepadState(session.id, pad))) return r.dropHost(session.host, 'host_backpressure');
    }
  }
  clientClosed(session, ws, code) { return session.ws === ws && code === 1006 && this.detachClient(session, 'client_disconnected'); }
  // ws names protocol violations WS_ERR_*; those end the session as before.
  clientError(session, ws, error) { return session.ws === ws && !String(error?.code ?? '').startsWith('WS_ERR_') && this.detachClient(session, 'client_error'); }
  // The bearer was already checked against the session by the caller.
  upgrade(req, socket, head, session, token, reject) {
    const r = this.relay;
    if (session.resume !== true || !spendResumeToken(session, token) || !resumable(session)) {
      // A client that cannot resume starts over: release the session now
      // instead of making its next connection wait for the grace window.
      r.stop(session, session.detached?.reason ?? 'client_disconnected');
      return reject(401, 'Unauthorized');
    }
    r.wss.handleUpgrade(req, socket, head, ws => r.attachClient(ws, session, true));
  }
  // attachClient(resumed): a socket the broker still had is replaced. Its close
  // callback no longer owns the session, so release its input here, exactly once.
  replace(session) {
    const old = session.ws;
    if (!old) return true; // detachClient already released this controller.
    session.ws = null;
    try { old.terminate(); } catch { /* already closed */ }
    this.resetInput(session);
    // Reset backpressure may synchronously drop the Host and end the session.
    return this.relay.sessions.get(session.id) === session;
  }
  // After attachClient bound the listeners.
  attached(session, resumed) {
    const r = this.relay, detached = session.detached;
    session.detached = null;
    r.send(session.ws, { type: resumed ? 'session.resumed' : 'session.resume.token', sessionId: session.id, token: issueResumeToken(session), graceMs: this.clientGraceMs, ...(session.channelResume === true ? { channelResumeProtocol } : {}) });
    if (!resumed) return;
    // channel-resume-v1: the Host may dial this session's side channels again.
    if (detached) r.channels?.clientBack(session);
    // What changed while the client was away: a rescaled encoder (stream-size-v1)
    // and the cursor shapes it will refer to.
    if (detached?.info) r.send(session.ws, detached.info);
    for (const shape of detached?.shapes ?? []) r.send(session.ws, shape, true);
    if (detached?.state) r.send(session.ws, detached.state, true);
    if (session.hostHeld || session.trackHeld) { r.send(session.ws, { type: 'session.resuming', sessionId: session.id }); return; }
    // gamepad-v1: each pad's latest rumble (one changed while the client was away never reached it).
    for (const [pad, level] of session.gamepad ? session.gamepadRumble ?? [] : []) r.send(session.ws, { type: 'gamepad.rumble', pad, low: level.low, high: level.high });
    this.restartVideo(session);
    this.record('session_resumed', session, 'client', detached);
  }
  // keyframe-v1's skip: no video until the IDR the Host is asked for (again while it is late).
  // Independent outputs: every output waits for its own IDR (tracks-resume-v1).
  restartVideo(session) {
    session.keyFrameRequestedAt = 0;
    if (session.trackState) { session.trackState.restart(); this.trackKeyFrame(session); return; }
    session.skippingVideo = true;
    this.relay.requestKeyFrame(session);
  }
  // tracks-resume-v1: an IDR on every output, on the track tunnel, at most every 500 ms.
  trackKeyFrame(session) {
    const now = Date.now();
    if (!session.trackTunnel || now - (session.keyFrameRequestedAt ?? 0) < 500) return;
    session.keyFrameRequestedAt = now;
    if (!this.relay.send(session.trackTunnel, { type: 'stream.keyframe', sessionId: session.id })) this.relay.stop(session, 'tracks_host_backpressure');
  }
  // stream.info while the client is away (only a rescale can come).
  info(session, info) { if (!session.ws && session.detached) session.detached.info = info; }
  // A cursor record while the client is away: shapes in order, the last state
  // (independent outputs: in the 28-byte TVT1 envelope).
  cursor(session, data) {
    const held = session.detached;
    if (!held) return;
    const copy = Buffer.from(data);
    if (data[session.trackState ? 28 : 12] !== 1) { held.state = copy; return; }
    held.shapes.push(copy); held.shapeBytes += copy.length;
    while (held.shapes.length > maxShapes || held.shapeBytes > maxShapeBytes) held.shapeBytes -= held.shapes.shift().length;
  }
  // While the Host is away only the client's own stop counts.
  heldClientMessage(session, data, binary) {
    if (!session.hostHeld || binary || data.length > 8192) return;
    try { if (JSON.parse(data.toString('utf8'))?.type === 'session.stop') this.relay.stop(session, 'client_stopped'); } catch { /* ignored */ }
  }
  // dropHost: keep the session for a Host that comes back. Deliberate closes,
  // protocol errors, revocation and VM power actions end it as before.
  hostLost(session, reason, detail) {
    const r = this.relay;
    if (session.hostHeld) return true; // ws reports an error and then its close

    const transport = hostLosses.has(reason) || (reason === 'host_disconnected' && detail === undefined) || (reason === 'host_error' && detail === 'host_transport_error');
    if (!transport || !resumable(session) || !hostResumeCapability(session.host.capabilities) || (session.trackState && !trackResumeCapability(session.host.capabilities)) || r.powerBusy(session.desktopId)) return false;
    const now = Date.now();
    session.hostHeld = { reason, detail, at: now, until: now + this.hostGraceMs };
    // gamepad-v1: a rumble the game changes while the Host is away never reaches the broker, so the levels kept
    // are no longer known to be current (the Host sends its own again once back; the client stops its motors).
    session.gamepadRumble?.clear();
    if (session.ws) r.send(session.ws, { type: 'session.resuming', sessionId: session.id });
    // channel-resume-v1: the side channels end with the Host's channel and wait for it.
    r.channels?.closePairs(session);
    return true;
  }
  // The Host's first word on a new channel: the sessions its worker kept.
  hostResume(host, value) {
    const r = this.relay;
    const ids = value.sessions;
    if (Object.keys(value).some(key => key !== 'type' && key !== 'sessions') || !Array.isArray(ids) || ids.length > 1 || ids.some(id => typeof id !== 'string' || !sessionPattern.test(id))) return r.dropHost(host, 'invalid_control');
    const resumed = new Set();
    for (const session of [...r.sessions.values()]) {
      const held = session.hostHeld;
      if (!held || session.desktopId !== host.identity.desktop_id) continue;
      if (!ids.includes(session.id) || session.host.identity.id !== host.identity.id || session.host.tokenHash !== host.tokenHash || !r.authorized(session)) {
        this.record('session_resume_expired', session, 'host', held);
        r.stop(session, held.reason, held.detail); continue;
      }
      session.hostHeld = null; session.host = host; host.attachedSession = undefined;
      resumed.add(session.id);
      // channel-resume-v1: first whether the client is there, so the Host's side channels wait or dial.
      r.channels?.tell(session, host);
      r.send(host.ws, { type: 'host.session.resumed', sessionId: session.id });
      // The client went away while the Host was away too: its input.reset (and the pads' neutral states) could not
      // be sent then, so they go now.
      if (!session.ws && session.detached) { this.resetInput(session); if (r.sessions.get(session.id) !== session) continue; }
      // A track tunnel still away restarts the picture when it is back.
      if (session.ws && !session.trackHeld) { r.send(session.ws, { type: 'session.resumed', sessionId: session.id }); this.restartVideo(session); }
      this.record('session_resumed', session, 'host', held);
    }
    // A stream the Host kept for a session this broker no longer has ends like
    // any other: through stop-v1 where the Host speaks it, so the next session
    // waits for its display to be restored.
    for (const id of ids) if (!resumed.has(id)) {
      const retirement = r.sessionStop.begin({ id, desktopId: host.identity.desktop_id, host, mode: 'stream', hostDispatched: true, stopProtocol: host.capabilities?.sessionStopProtocol });
      r.send(host.ws, { type: 'session.stop', sessionId: id, reason: 'resume_refused', ...r.sessionStop.fields(retirement) });
    }
  }
  // tracks-resume-v1: the session's track tunnel ended (`transport`: without a
  // close frame, with a transport error or silent). True: nothing more to do
  // (kept for the Host's next tunnel, or the session is already gone).
  trackLost(session, reason, detail, transport) {
    const r = this.relay, ws = session.trackTunnel;
    if (!ws || r.sessions.get(session.id) !== session) return true;
    if (!transport || !resumable(session) || !trackResumeCapability(session.host.capabilities) || r.powerBusy(session.desktopId)) return false;
    const now = Date.now();
    session.trackTunnel = null;
    session.trackHeld = { reason, detail, at: now, until: now + this.hostGraceMs };
    try { ws.terminate(); } catch { /* already closed */ }
    if (session.ws && !session.hostHeld) r.send(session.ws, { type: 'session.resuming', sessionId: session.id });
    return true;
  }
  // The Host dials the tunnel of a session that waits for it, or of one whose
  // tunnel the broker still holds (a half-open socket is replaced).
  trackReturning(session) {
    return !!session.trackHeld || (!!session.trackTunnel && resumable(session) && trackResumeCapability(session.host.capabilities));
  }
  // attachTrackTunnel(resumed), before it binds the new tunnel. The held
  // record (or null: the session cannot take it, the tunnel was closed).
  trackReturned(session, ws) {
    const r = this.relay;
    if (r.sessions.get(session.id) !== session || !r.current(session) || !r.authorized(session) || !this.trackReturning(session)) {
      try { ws.close(1008, 'resume_refused'); } catch { /* already closed */ }
      return null;
    }
    const held = session.trackHeld ?? { reason: 'replaced', at: Date.now() }, old = session.trackTunnel;
    session.trackHeld = null; session.trackTunnel = null;
    if (old) try { old.terminate(); } catch { /* already closed */ }
    return held;
  }
  // attachTrackTunnel(resumed), after it bound the new tunnel.
  trackResumed(session, held) {
    const r = this.relay;
    if (session.ws) r.send(session.ws, { type: 'session.resumed', sessionId: session.id });
    // A client away restarts the picture when it is back.
    if (!session.detached) this.restartVideo(session);
    this.record('session_resumed', session, 'tracks', held);
  }
  // revalidate: a side that did not come back in time ends the session with its own reason.
  expire(session, now = Date.now()) {
    const side = session.detached?.until <= now ? 'client' : session.hostHeld?.until <= now ? 'host' : session.trackHeld?.until <= now ? 'tracks' : null;
    if (!side) return false;
    const held = side === 'client' ? session.detached : side === 'host' ? session.hostHeld : session.trackHeld;
    this.record('session_resume_expired', session, side, held);
    this.relay.stop(session, held.reason, held.detail);
    return true;
  }
  // Measured, not claimed: each resume and each expiry with its gap. Never the token.
  record(event, session, side, held) {
    try { this.relay.store.audit(event, session.userId, JSON.stringify({ sessionId: session.id, desktopId: session.desktopId, side, reason: held?.reason ?? 'replaced', gapMs: held ? Date.now() - held.at : 0 })); } catch { /* optional audit */ }
  }
}

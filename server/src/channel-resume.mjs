// channel-resume-v1: a stream's side channels (clipboard, print-v1, drive-v1,
// usb-v1) come back by themselves after the stream resumes (session-resume-v1 /
// host-resume-v1, session-resume.mjs). Before it a resumed stream kept its
// picture and lost every side channel until the next connection: a pair that
// ended was final (print, drive, usb), a dial while the client was away was a
// refusal (401) both peers took as final, and the Host never heard that the
// client was back.
//
// Negotiation (every other mix keeps the released behaviour, field for field):
//   * A Host that redials advertises `hostChannelResumeProtocol`.
//   * A client that redials sends `X-Multitap-Channel-Resume: channel-resume-v1`
//     on its session socket (released brokers ignore the header).
//   * Granted where both did and the session resumes (session-resume-v1): the
//     client reads `channelResumeProtocol` in its session.resume.token /
//     session.resumed message, the Host in its session.start.
//
// What the broker keeps for a session that waits for a side: its grants (they
// already live on the session) and, per channel, whether it may pair again.
// Never the pair: its transfer, requests, shares and devices end with it and
// the peers that own them offer them again on the new pair, which passes every
// check of a first pair (the same session, the same bearer / Host token, the
// stored permissions as they are at that moment).
//   * While the client or the Host is away the pairs are closed with
//     `<channel>_stream_away` and a dial is answered 503 (not now), never 401.
//   * The Host hears {type:'session.channels', sessionId, state:'away'|'back'}
//     when the client leaves and returns; a returning Host is told the state
//     before its host.session.resumed. The client knows from session.resuming /
//     session.resumed.
//   * A pair that ended for a transport reason may be dialled again. One that
//     ended for a reason of its own (a violation, a rate limit, a revoked
//     permission) is final for the session, as every end was before; so is a
//     channel whose permission was taken away while it had no pair.
//   * A new dial of a role the broker still holds replaces that pair: the same
//     authenticated peer says its old socket is gone (a network change the
//     broker has not noticed yet).
//   * New pairs are bounded per channel and session (maxPairs per windowMs).
// Nothing survives the session: stop() closes every channel for good.
import { clipboardPolicyRestricted } from './clipboard-policy.mjs';

export const channelResumeProtocol = 'channel-resume-v1';
export const channelResumeHeader = 'x-multitap-channel-resume';
export const hostChannelResumeCapability = value => value?.hostChannelResumeProtocol === channelResumeProtocol;
const transient = ['_transport_closed', '_pair_timeout', '_stream_away', '_timeout'];

export class ChannelResume {
  constructor(relay, { windowMs = 60000, maxPairs = 6 } = {}) {
    Object.assign(this, { relay, windowMs, maxPairs });
  }
  // The client's first attach: its header, this Host and a session that resumes.
  negotiate(session, header) {
    session.channelResume = session.mode === 'stream' && session.resume === true && !session.signinPolicy &&
      header === channelResumeProtocol && hostChannelResumeCapability(session.host?.capabilities);
    return session.channelResume;
  }
  enabled(session) { return session?.channelResume === true; }
  // The stream itself waits for its client or its Host.
  away(session) { return this.enabled(session) && !!(session.detached || session.hostHeld); }
  state(session, kind) { const all = session.channelState ??= {}; return all[kind] ??= { final: false, pairs: [] }; }
  final(session, kind) { return this.enabled(session) && this.state(session, kind).final; }
  // A pair (or the whole channel) ended with `reason`. True: the same session
  // may pair again. The clipboard keeps its released rule (its peers decide
  // what they retry); only a narrower grant ends it for the session.
  closed(session, kind, reason) {
    if (!this.enabled(session)) return false;
    if (kind === 'clipboard' ? reason !== 'clipboard_policy_changed' && reason !== 'session_ended' : transient.some(suffix => reason === kind + suffix)) return true;
    this.state(session, kind).final = true;
    return false;
  }
  // The HTTP status that refuses a dial of this channel now, or 0.
  refusal(session, kind) {
    if (!this.enabled(session)) return 0;
    if (this.state(session, kind).final) return 409;
    return this.away(session) ? 503 : 0;
  }
  // A dial that starts a new pair. False: too many in this window.
  admit(session, kind, now = Date.now()) {
    if (!this.enabled(session)) return true;
    const state = this.state(session, kind);
    state.pairs = state.pairs.filter(at => now - at < this.windowMs);
    if (state.pairs.length >= this.maxPairs) return false;
    state.pairs.push(now);
    return true;
  }
  // The stream waits for a side: its pairs end now, with the reason both peers
  // wait on, instead of at the next sweep with a refusal.
  closePairs(session) {
    if (!this.enabled(session)) return;
    const r = this.relay;
    if (session.clipboardPair) r.clipboard.closePair(session.clipboardPair, 'clipboard_stream_away');
    if (session.printPair && !session.printPair.closed) r.print.closePair(session.printPair, 'print_stream_away');
    if (session.drivePair) r.drive.closePair(session.drivePair, 'drive_stream_away');
    if (session.usbPair && !session.usbPair.closed) r.usb.closePair(session.usbPair, 'usb_stream_away');
  }
  // The Host's word about the client (its own absence it knows itself).
  tell(session, host = session.host) {
    if (!this.enabled(session)) return;
    const r = this.relay;
    if (!r.send(host.ws, { type: 'session.channels', sessionId: session.id, state: session.detached ? 'away' : 'back' })) r.dropHost(host, 'host_backpressure');
  }
  // session-resume.mjs: the client's socket is gone / the client is back.
  clientAway(session) { if (!this.enabled(session)) return; this.closePairs(session); if (!session.hostHeld) this.tell(session); }
  clientBack(session) { if (this.enabled(session) && !session.hostHeld) this.tell(session); }
  // The administrator changed this user's grants: a channel without a pair
  // (the stream is away) whose permission is gone stays closed for the session,
  // as a pair that was open at that moment would have been.
  policyChanged(userId, desktopId) {
    const r = this.relay;
    for (const session of r.sessions.values()) {
      if (!this.enabled(session) || session.userId !== userId || (desktopId !== undefined && session.desktopId !== desktopId)) continue;
      const gone = (kind, granted, pair, permitted) => { if (granted && !pair && !this.state(session, kind).final && !permitted()) this.state(session, kind).final = true; };
      gone('print', session.print === true, session.printPair && !session.printPair.closed, () => r.print.permitted(session));
      gone('drive', session.driveEnabled === true, session.drivePair, () => r.drive.allowed(session));
      gone('usb', session.usb === true, session.usbPair && !session.usbPair.closed, () => r.usb.permitted(session));
      gone('clipboard', !!session.clipboardEnabled, session.clipboardPair, () => !clipboardPolicyRestricted(session.clipboardPolicy, r.store.clipboardPolicy(session.userId, session.desktopId)));
    }
  }
}

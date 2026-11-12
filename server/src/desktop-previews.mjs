import { randomBytes } from 'node:crypto';
import { HttpError } from './errors.mjs';
import { freshThumbnailPrivacy } from './thumbnail-privacy.mjs';

// desktop-preview-v1: a recent
// picture of each assigned desktop on the launcher without connecting first.
//
// On demand only. A client that shows its launcher asks, about once a minute,
// GET /api/v1/desktops/:id/preview for each assigned desktop whose list entry
// carries previewProtocol. This broker then asks that desktop's Host over its
// authenticated control channel (Host capability thumbnail-capture-v1) at most
// once per 30 s per desktop; nobody looking means nothing is captured.
//
// Privacy. The JPEG (at most 48 KiB) is kept in memory only, at most 10
// minutes, bound to the Host's thumbnail-privacy-v1 epoch of the moment it was
// taken: a lock, sign-out or user switch moves that epoch and the picture is
// no longer served. It is served only through the assignment-checked client
// route; it is never written to disk, the audit log, diagnostics or any
// administrator view. A Host that disconnects takes its picture with it. The
// Host itself refuses the lock/sign-in/secure desktop (answers locked/signin).
//
// Off switch: settings.desktop_previews = '0' (PUT /api/v1/admin/desktop-previews)
// forgets every picture at once and clients stop asking (their list no longer
// carries previewProtocol). Without a stored value previews are on. The
// administrator console has no page for this switch (previews are turned on
// and off in the client); the stored value and the route stay for an
// administrator who sets it through the API.
export const desktopPreviewProtocol = 'desktop-preview-v1';
export const thumbnailCaptureProtocol = 'thumbnail-capture-v1';
export const desktopPreviewSetting = 'desktop_previews';
export const previewMaxBytes = 48 * 1024;
const magic = Buffer.from('TVP1');
const hostStates = new Set(['captured', 'blank', 'locked', 'signin', 'failed', 'busy']);
const requestIdPattern = /^[0-9a-f]{32}$/, epochPattern = /^[0-9a-f]{32}$/;

export function thumbnailCaptureCapability(value) {
  return value?.thumbnailCaptureProtocol === thumbnailCaptureProtocol && value.service === true;
}

export class DesktopPreviews {
  constructor({ store, send, now = Date.now, intervalMs = 30000, retryMs = 10000, waitMs = 6000, answerMs = 15000, keepMs = 600000, allowed = () => true }) {
    Object.assign(this, { store, send, now, intervalMs, retryMs, waitMs, answerMs, keepMs });
    this.entries = new Map();
    this.allowed = host => { try { return allowed(host) === true; } catch { return false; } };
  }
  enabled() { return this.store.setting(desktopPreviewSetting) !== '0'; }
  setEnabled(enabled, actor) {
    if (typeof enabled !== 'boolean') throw new HttpError(400, 'invalid_desktop_previews');
    this.store.saveSetting(desktopPreviewSetting, enabled ? '1' : '0', actor);
    if (!enabled) this.clear();
    return { enabled: this.enabled() };
  }
  // The list entry flag: a client asks only where this is set.
  offered(host, connected) { return this.allowed(host) && connected && this.enabled() && thumbnailCaptureCapability(host?.capabilities) && !host.capabilitiesPending; }
  clear() { for (const [id] of this.entries) this.forget(id); }
  forget(desktopId) {
    const entry = this.entries.get(desktopId);
    if (!entry) return;
    this.entries.delete(desktopId);
    this.settle(entry, 'failed');
  }
  hostLost(host) {
    const entry = this.entries.get(host.identity.desktop_id);
    if (entry?.host === host) this.forget(host.identity.desktop_id);
  }
  sweep(now = this.now()) {
    for (const [id, entry] of this.entries) {
      if (entry.pending && now - entry.pending.at > this.answerMs) this.settle(entry, 'failed');
      if (entry.image && now - entry.takenAt > this.keepMs) { entry.image = null; entry.epoch = null; }
      if (!entry.image && !entry.pending && now - entry.askedAt > this.keepMs) this.entries.delete(id);
    }
  }
  settle(entry, state) {
    const pending = entry.pending;
    if (!pending) return;
    entry.pending = null;
    entry.state = state; entry.stateAt = this.now();
    for (const resolve of pending.waiters) resolve();
  }
  // Host → broker: "TVP1" + 32-char request id + JPEG, right before its result.
  // Returns true for every record of this kind (never forwarded anywhere).
  binary(host, data) {
    if (!Buffer.isBuffer(data) || data.length < 4 || !data.subarray(0, 4).equals(magic)) return false;
    if (!this.allowed(host)) { this.forget(host.identity.desktop_id); return true; }
    const entry = this.entries.get(host.identity.desktop_id), pending = entry?.host === host ? entry.pending : null;
    if (!pending || pending.bytes || data.length < 36 + 4 || data.length - 36 > previewMaxBytes) return true;
    if (data.toString('latin1', 4, 36) !== pending.id) return true;
    const jpeg = data.subarray(36);
    if (jpeg[0] !== 0xff || jpeg[1] !== 0xd8 || jpeg[jpeg.length - 2] !== 0xff || jpeg[jpeg.length - 1] !== 0xd9) return true;
    pending.bytes = Buffer.from(jpeg);
    return true;
  }
  // Host → broker: {type:'host.thumbnail.result', requestId, state, epoch?, bytes?, width?, height?}.
  result(host, value) {
    if (!this.allowed(host)) { this.forget(host.identity.desktop_id); return; }
    const entry = this.entries.get(host.identity.desktop_id), pending = entry?.host === host ? entry.pending : null;
    if (!pending || value.requestId !== pending.id) return;
    const state = hostStates.has(value.state) ? value.state : 'failed';
    if (state === 'captured') {
      const valid = pending.bytes && epochPattern.test(value.epoch ?? '') && value.bytes === pending.bytes.length &&
        Number.isInteger(value.width) && Number.isInteger(value.height) && value.width >= 1 && value.height >= 1 && value.width <= 480 && value.height <= 360;
      if (!valid) return this.settle(entry, 'failed');
      entry.image = pending.bytes; entry.takenAt = this.now(); entry.epoch = value.epoch;
    } else if (state === 'blank') {
      // The output is dark: the last picture stays, with its age, if it belongs to this epoch.
      if (!epochPattern.test(value.epoch ?? '') || value.epoch !== entry.epoch) { entry.image = null; entry.epoch = null; }
    } else if (state === 'locked' || state === 'signin') {
      entry.image = null; entry.epoch = null;
    }
    this.settle(entry, state);
  }
  // Client → broker (route already checked the assignment). Waits up to waitMs
  // for a capture it started; otherwise answers from what it has.
  // othersSince: when another account's live session on this desktop began
  // (relay.othersSince), or null. While one lasts nothing new is captured for
  // this viewer and only a picture taken before it began may be shown, so one
  // account never watches another's session through the launcher.
  async view(desktopId, host, connected, heartbeatMs, { othersSince = null } = {}) {
    if (!this.offered(host, connected)) { this.forget(desktopId); return { protocol: desktopPreviewProtocol, state: 'unavailable' }; }
    const now = this.now();
    let entry = this.entries.get(desktopId);
    if (!entry || entry.host !== host) {
      if (entry) this.forget(desktopId);
      entry = { host, image: null, takenAt: 0, epoch: null, state: null, stateAt: 0, askedAt: 0, requestedAt: 0, requestedEpoch: null, pending: null };
      this.entries.set(desktopId, entry);
    }
    entry.askedAt = now;
    const privacy = freshThumbnailPrivacy(host, connected, heartbeatMs * 3, now);
    // A lock/sign-out/switch moved the epoch since the last ask: ask again
    // sooner than the ordinary interval, so the card says why at once.
    const moved = privacy && privacy.epoch !== entry.requestedEpoch;
    if (othersSince !== null) return this.answer(desktopId, host, connected, heartbeatMs, { othersSince });
    if (!entry.pending && (!entry.requestedAt || now - entry.requestedAt >= this.intervalMs || (moved && now - entry.requestedAt >= this.retryMs))) {
      const id = randomBytes(16).toString('hex');
      entry.pending = { id, at: now, bytes: null, waiters: new Set() };
      entry.requestedAt = now; entry.requestedEpoch = privacy?.epoch ?? null;
      if (!this.send(host, { type: 'host.thumbnail.request', requestId: id })) this.settle(entry, 'failed');
    }
    if (entry.pending) {
      const pending = entry.pending;
      await new Promise(resolve => {
        const timer = setTimeout(done, this.waitMs); timer.unref?.();
        function done() { clearTimeout(timer); pending.waiters.delete(done); resolve(); }
        pending.waiters.add(done);
      });
    }
    return this.answer(desktopId, host, connected, heartbeatMs);
  }
  answer(desktopId, host, connected, heartbeatMs, { othersSince = null } = {}) {
    const entry = this.entries.get(desktopId), now = this.now();
    const plain = state => ({ protocol: desktopPreviewProtocol, state });
    if (!entry || entry.host !== host || !this.offered(host, connected)) return plain('unavailable');
    const privacy = freshThumbnailPrivacy(host, connected, heartbeatMs * 3, now);
    // The Host's newest word (it also covers a UAC prompt on an unlocked
    // session), unless the epoch moved to an unlocked one since it was asked:
    // then the next ask captures it.
    if (entry.state === 'locked' || entry.state === 'signin') return plain(privacy?.allowed && privacy.epoch !== entry.requestedEpoch ? 'unavailable' : entry.state);
    if (privacy && !privacy.allowed) return plain('locked');
    const image = entry.image && privacy?.allowed && privacy.epoch === entry.epoch && now - entry.takenAt <= this.keepMs && (othersSince === null || entry.takenAt < othersSince) ? entry.image : null;
    // ageMs: the client shows "N분 전" from this, not from its own clock.
    if (image) return { ...plain('captured'), takenAt: new Date(entry.takenAt).toISOString(), ageMs: Math.max(0, now - entry.takenAt), image: image.toString('base64') };
    return plain(entry.state === 'blank' ? 'asleep' : 'unavailable');
  }
}

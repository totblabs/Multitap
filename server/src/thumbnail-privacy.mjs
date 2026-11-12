export const thumbnailPrivacyProtocol = 'thumbnail-privacy-v1';

// Read-only invalidation metadata, never a frame/stream/input permission.
export function validThumbnailPrivacy(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) ||
      Object.keys(value).sort().join(',') !== 'allowed,epoch,protocol' ||
      value.protocol !== thumbnailPrivacyProtocol || typeof value.epoch !== 'string' ||
      !/^[0-9a-f]{32}$/.test(value.epoch) || /^0{16}$/.test(value.epoch.slice(16)) ||
      typeof value.allowed !== 'boolean') return null;
  return { protocol: thumbnailPrivacyProtocol, epoch: value.epoch, allowed: value.allowed };
}

// A rejected new report makes privacy unknown immediately, without changing
// the broker's existing streaming/capability admission or retry behavior.
export function observeThumbnailPrivacy(host, capabilities) {
  const value = validThumbnailPrivacy(capabilities?.thumbnailPrivacy);
  host.thumbnailPrivacyUncertain = true;
  if (!value || (value.allowed && capabilities.interactiveSession !== true)) {
    if (capabilities) delete capabilities.thumbnailPrivacy;
    return;
  }
  const prefix = value.epoch.slice(0, 16), sequence = BigInt('0x' + value.epoch.slice(16));
  const previous = host.thumbnailPrivacyEpochState;
  // One owner prefix per authenticated socket. No retired-prefix collection,
  // no replay eviction, and no stream/capability-admission side effect.
  if (previous && (previous.prefix !== prefix || sequence < previous.sequence ||
      (sequence === previous.sequence && previous.allowed === false && value.allowed))) {
    delete capabilities.thumbnailPrivacy;
    return;
  }
  host.thumbnailPrivacyEpochState = { prefix, sequence, allowed: value.allowed };
  host.thumbnailPrivacyUncertain = false;
}

export function freshThumbnailPrivacy(host, authenticated, maxAgeMs, now = Date.now()) {
  if (authenticated !== true || !host || host.capabilitiesPending || host.thumbnailPrivacyUncertain ||
      !Number.isFinite(host.capabilitiesAt) || !Number.isFinite(now) ||
      !Number.isFinite(maxAgeMs) || maxAgeMs < 0) return null;
  const age = now - host.capabilitiesAt;
  if (age < 0 || age > maxAgeMs) return null;
  const value = validThumbnailPrivacy(host.capabilities?.thumbnailPrivacy);
  const state = host.thumbnailPrivacyEpochState;
  if (!value || !state || value.epoch.slice(0, 16) !== state.prefix ||
      BigInt('0x' + value.epoch.slice(16)) !== state.sequence || value.allowed !== state.allowed ||
      (value.allowed && host.capabilities?.interactiveSession !== true)) return null;
  return value;
}

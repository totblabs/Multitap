// Shows only the authenticated broker's verified, newer server release. The
// caller owns requests/timers and the existing server-page navigation.
import { t } from './i18n.js';
const versionPattern = /^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$/;
function newer(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || !versionPattern.test(a) || !versionPattern.test(b)) return false;
  const x = a.split('.').map(Number), y = b.split('.').map(Number);
  for (let i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] > y[i];
  return false;
}
function offeredVersion(info) {
  const update = info?.update;
  if (info?.service !== 'multitap-broker' || !Number.isFinite(Date.parse(info.checkedAt)) || update?.feedConfigured !== true || update.state !== 'available' || !/^[a-f0-9]{64}$/.test(update.releaseId ?? '') || !newer(update.latest?.version, info.version)) return null;
  // An old updater result cannot badge a newly running server as out of date.
  if (update.currentVersion !== undefined && update.currentVersion !== info.version) return null;
  return update.latest.version;
}

export function createServerUpdateBadge({ root = globalThis.document } = {}) {
  let latest = null, ticket = null, observedAt = null;
  function render() {
    for (const node of root.querySelectorAll('[data-nav-badge="server"], [data-server-update-badge]')) {
      node.hidden = latest === null;
      // The header indicator keeps its icon; only its label names the version.
      const text = node.hasAttribute('data-nav-badge') ? node : node.querySelector('.update-indicator-label');
      if (text) text.textContent = latest ? (text === node ? '1' : t('server-update-badge.1', { latest })) : '';
      if (latest) {
        const label = t('server-update-badge.2', { latest });
        node.setAttribute('title', label); node.setAttribute('aria-label', label);
      } else { node.removeAttribute('title'); node.removeAttribute('aria-label'); }
    }
  }
  function begin() { ticket = Object.freeze({}); return ticket; }
  function update(info, current) {
    if (!current || current !== ticket) return false;
    const receivedAt = Date.parse(info?.checkedAt);
    if (Number.isFinite(receivedAt)) {
      if (observedAt !== null && receivedAt < observedAt) return false;
      observedAt = receivedAt;
    }
    latest = offeredVersion(info); render(); return true;
  }
  function fail(current) {
    if (!current || current !== ticket) return false;
    latest = null; render(); return true;
  }
  function clear() { ticket = null; latest = null; observedAt = null; render(); }
  clear();
  // The verified newer server version, or null (the sidebar's 업데이트 count adds it).
  return { begin, update, fail, clear, render, offered: () => latest };
}

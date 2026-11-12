import { selectVisibleText } from './js/dom.js';
import { t, lang as consoleLanguage } from './js/i18n.js';

const prefix = 'MULTITAP-CONNECTION-V1:';
function expiry(value) {
  if (typeof value !== 'string' || !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?Z$/.test(value)) throw new Error();
  const date = new Date(value);
  if (!Number.isFinite(date.getTime()) || date.toISOString().slice(0, 19) !== value.slice(0, 19)) throw new Error();
  return date.toISOString();
}
function decodeBlock(value, marker, endMarker) {
  if (typeof value !== 'string' || value.length > 32768) throw new Error();
  const start = value.indexOf(marker), end = value.indexOf(endMarker, start);
  if (start < 0 || end < 0 || value.indexOf(marker, start + marker.length) >= 0) throw new Error();
  const encoded = value.slice(start + marker.length, end).replace(/\s/g, '');
  if (!/^[A-Za-z0-9+/]+={0,2}$/.test(encoded)) throw new Error();
  const decoded = new TextDecoder('utf-8', { fatal: true }).decode(Uint8Array.from(atob(encoded), char => char.charCodeAt(0)));
  const data = JSON.parse(decoded);
  if (!data || Array.isArray(data) || typeof data !== 'object') throw new Error();
  return data;
}
export function decodeConnectionBlock(value) {
  if (typeof value !== 'string' || value.length > 32768) throw new Error(t('shell.1'));
  const start = value.indexOf(prefix), end = value.indexOf('END-MULTITAP-CONNECTION', start);
  if (start < 0 || end < 0 || value.indexOf(prefix, start + prefix.length) >= 0) throw new Error(t('shell.2'));
  try {
    const data = decodeBlock(value, prefix, 'END-MULTITAP-CONNECTION');
    // fingerprint: the script pinned the node's own certificate (then in ca).
    if (!['ca,endpoint,secret,tokenId', 'ca,endpoint,expiresAt,secret,tokenId', 'ca,endpoint,fingerprint,secret,tokenId', 'ca,endpoint,expiresAt,fingerprint,secret,tokenId'].includes(Object.keys(data).sort().join(',')) || Object.values(data).some(value => typeof value !== 'string')) throw new Error();
    if ('fingerprint' in data && (!/^[0-9A-F]{2}(?::[0-9A-F]{2}){31}$/.test(data.fingerprint) || !data.ca)) throw new Error();
    const url = new URL(data.endpoint);
    if (url.protocol !== 'https:' || url.username || url.password || url.search || url.hash || !['', '/'].includes(url.pathname) || data.endpoint.length > 200) throw new Error();
    if (!/^[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+![A-Za-z0-9_.-]+$/.test(data.tokenId) || !/^[\x21-\x7e]{8,256}$/.test(data.secret) || data.ca.length > 16000 || (data.ca && !data.ca.includes('-----BEGIN CERTIFICATE-----'))) throw new Error();
    return { ...data, endpoint: url.origin, ...('expiresAt' in data ? { expiresAt: expiry(data.expiresAt) } : {}) };
  } catch { throw new Error(t('shell.3')); }
}

export function decodeRenewalBlock(value, tokenId) {
  try {
    const data = decodeBlock(value, 'MULTITAP-RENEWAL-V1:', 'END-MULTITAP-RENEWAL');
    if (Object.keys(data).sort().join(',') !== 'expiresAt,tokenId' || data.tokenId !== tokenId || !/^[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+![A-Za-z0-9_.-]+$/.test(data.tokenId)) throw new Error();
    return { tokenId: data.tokenId, expiresAt: expiry(data.expiresAt) };
  } catch { throw new Error(t('shell.4')); }
}
const quote = value => "'" + value.replaceAll("'", "'\\''") + "'";

// The script speaks the console's language: --lang ko|en follows the page (docs/i18n.md).
// The node runs the script only when its SHA-256 matches the one the broker
// computed from the exact bytes it serves to this address (sha256, from the
// Proxmox page data): over plain HTTP, a changed script stops before running
// as root. A small sh wrapper keeps "add the address at the very end" working
// and removes the downloaded copy afterwards.
export function enrollmentCommand(origin, { allowInsecureHttp = false, lang = consoleLanguage, sha256 } = {}) {
  if (typeof sha256 !== 'string' || !/^[0-9a-f]{64}$/.test(sha256)) throw new Error(t('shell.38'));
  const url = new URL(origin);
  if (typeof origin !== 'string' || origin.length > 512 || /[\s\x00-\x20\x7f\\@?#%]/u.test(origin) || !/^(https?):\/\/(\[[0-9a-f:.]+\]|[^/:]+)(?::[0-9]{1,5})?\/?$/i.test(origin) || !['https:', ...(allowInsecureHttp === true ? ['http:'] : [])].includes(url.protocol) || !url.hostname || url.port === '0' || url.username || url.password || url.search || url.hash || !['', '/'].includes(url.pathname)) throw new Error(t('shell.5'));
  const rawAuthority = /^(?:https?):\/\/(\[[0-9a-f:.]+\]|[^/:]+)(?::[0-9]{1,5})?\/?$/i.exec(origin)[1];
  if (!url.hostname.startsWith('[')) {
    const host = url.hostname, labels = host.replace(/\.$/, '').split('.');
    if (host.length > 253 || labels.some(label => !/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/i.test(label)) ||
        (/^[0-9]+(?:\.[0-9]+){3}$/.test(host) && rawAuthority.replace(/\.$/, '') !== host)) throw new Error(t('shell.6'));
  }
  const scriptUrl = new URL('/setup/proxmox.py', url).href;
  return "sh -c " + quote(setupWrapper) + ' multitap-setup ' + quote(scriptUrl) + ' ' + sha256 + ' --lang ' + (lang === 'ko' ? 'ko' : 'en');
}
// $1 = script address, $2 = its SHA-256; the rest goes to the script.
const setupWrapper = 'f=$(mktemp) || exit; curl -q -fsS "$1" -o "$f" && echo "$2  $f" | sha256sum -c --quiet - && shift 2 && python3 "$f" "$@"; s=$?; rm -f "$f"; exit $s';

export function renewalCommand(origin, tokenId, endpoint, transportOptions) {
  if (!/^[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+![A-Za-z0-9_.-]+$/.test(tokenId)) throw new Error(t('shell.7'));
  const url = new URL(endpoint);
  if (url.protocol !== 'https:' || url.username || url.password || url.search || url.hash || !['', '/'].includes(url.pathname)) throw new Error(t('shell.8'));
  return enrollmentCommand(origin, transportOptions) + ' --renew ' + quote(tokenId) + ' --endpoint ' + quote(url.origin);
}

// The identity the setup script made: multitap-<name>@pve!<token>. The
// script proves ownership itself (comment, role, ACL) before it changes anything.
export const dedicatedIdentity = tokenId => typeof tokenId === 'string' && /^multitap-[A-Za-z0-9_.-]{2,40}@pve![A-Za-z][A-Za-z0-9_.-]{1,31}$/.test(tokenId);
export function enableConsoleCommand(origin, tokenId, transportOptions) {
  const match = dedicatedIdentity(tokenId) ? [tokenId] : null;
  if (!match || match[0] !== tokenId) throw new Error(t('shell.9'));
  return enrollmentCommand(origin, transportOptions) + ' --enable-console ' + quote(tokenId);
}

export function commandBox(el, button, command, label = t('common.27')) {
  let timer = null;
  const code = el('code', {}, command);
  const copy = button(label, async () => {
    try { if (typeof globalThis.navigator?.clipboard?.writeText !== 'function') throw Error('unavailable'); await globalThis.navigator.clipboard.writeText(command); copy.textContent = t('common.28'); copy.classList.add('is-done'); }
    catch {
      selectVisibleText(code);
      let copied = false;
      try { copied = globalThis.document?.execCommand?.('copy') === true; } catch { copied = false; }
      copy.textContent = t(copied ? 'common.28' : 'common.29');
      if (copied) copy.classList.add('is-done');
    }
    // The label returns, so a second copy is announced the same way.
    clearTimeout(timer); timer = setTimeout(() => { copy.textContent = label; copy.classList.remove('is-done'); }, 2400); timer.unref?.();
  }, 'quiet', null, { 'aria-live': 'polite' });
  return el('div', { class: 'shell-command' }, code, copy);
}

export function createShellSetup(el, button, apply, { sha256 } = {}) {
  const origin = globalThis.location.origin, plainHttp = globalThis.location.protocol === 'http:';
  let command = null, commandError = '';
  try { command = enrollmentCommand(origin, { allowInsecureHttp: plainHttp, sha256 }); } catch (error) { commandError = error.message; }
  const input = el('textarea', { class: 'connection-import', rows: '3', maxlength: '32768', placeholder: t('shell.10'), 'aria-label': t('shell.11'), autocomplete: 'off', spellcheck: 'false' });
  const status = el('p', { class: 'import-status', role: 'status' });
  let saving = false;
  const connect = button(t('shell.12'), async () => {
    if (saving) return;
    try {
      const data = decodeConnectionBlock(input.value); saving = true; connect.disabled = true; connect.textContent = t('shell.13');
      await apply(data); input.value = ''; status.classList.remove('error'); status.textContent = t('shell.14');
    } catch (error) { status.classList.add('error'); status.textContent = error.message; }
    finally { saving = false; connect.disabled = false; connect.textContent = t('shell.12'); }
  }, 'primary');
  const step = (number, title, text) => el('div', { class: 'setup-step' }, el('span', { class: 'setup-step-number', 'aria-hidden': 'true' }, String(number)), el('div', {}, el('strong', {}, title), text ? el('p', {}, text) : null));
  const panel = el('section', { class: 'panel shell-setup', id: 'shell-setup' },
    el('div', { class: 'panel-head' }, el('h2', {}, t('common.10')), el('span', { class: 'pill blue' }, t('shell.15'))),
    step(1, t('shell.16'), t('shell.17')),
    command ? commandBox(el, button, command, t('shell.18')) : el('p', { class: 'import-status error', role: 'alert' }, commandError),
    // What the command makes and allows, in full, so nobody reads more (or less) into it.
    el('details', { class: 'disclosure pve-scope', 'data-keep-open': 'pve-scope' }, el('summary', {}, t('shell.19')),
      el('div', { class: 'pve-scope-body' },
        el('p', {}, t('shell.20')),
        el('table', { class: 'scope-table' },
          el('thead', {}, el('tr', {}, el('th', {}, t('shell.21')), el('th', {}, t('shell.22')))),
          el('tbody', {},
            el('tr', {}, el('td', {}, el('code', {}, 'VM.Audit')), el('td', {}, t('shell.23'))),
            el('tr', {}, el('td', {}, el('code', {}, 'VM.PowerMgmt')), el('td', {}, t('shell.24'))),
            el('tr', {}, el('td', {}, el('code', {}, 'VM.Console')), el('td', {}, t('shell.25'))),
            el('tr', {}, el('td', {}, el('code', {}, 'VM.GuestAgent.Audit'), t('shell.26'), el('code', {}, 'VM.Monitor'), ')'), el('td', {}, t('shell.27'))))),
        el('p', {}, t('shell.28'), el('code', {}, '/vms'), t('shell.29')),
        el('p', {}, el('strong', {}, t('shell.30')), t('shell.31')),
        el('p', {}, t('shell.32')),
        el('p', {}, t('shell.33')))),    step(2, t('shell.34'), [el('code', { class: 'nowrap' }, 'MULTITAP-CONNECTION-V1:'), t('shell.35'), el('code', { class: 'nowrap' }, 'END-MULTITAP-CONNECTION'), t('shell.36')]),
    // Over plain HTTP the pasted block (with its secret) is not encrypted on the way.
    plainHttp ? el('p', { class: 'shell-http-warning' }, t('shell.39')) : null,
    el('div', { class: 'connection-import-field' }, input),
    status, el('div', { class: 'actions' }, connect, el('small', {}, t('shell.37'))));
  return panel;
}

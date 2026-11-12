// 다운로드: its own place in the sidebar. The same cards as the public pages
// (/ and /download), each saying what it is for, with the short links an
// administrator gives to users or types inside a fresh Windows.
// The catalog is the public /api/v1/downloads; only this server's own
// /downloads/<file> paths become links (download-choices.js).
import { el, icon, button, panel, loadingState, errorState, selectVisibleText } from './dom.js';
import { downloadChoices, formatSize, exactSize, releaseDate, fileLabel, validSha256 } from '../download-choices.js';
import { t, lang } from './i18n.js';

function copyButton(text, label, target) {
  let timer = null;
  const node = button(t('common.27'), async () => {
    try { if (typeof globalThis.navigator?.clipboard?.writeText !== 'function') throw Error('unavailable'); await globalThis.navigator.clipboard.writeText(text); node.textContent = t('common.28'); node.classList.add('is-done'); }
    catch { selectVisibleText(target); node.textContent = t('common.29'); }
    clearTimeout(timer); timer = setTimeout(() => { node.textContent = t('common.27'); node.classList.remove('is-done'); }, 2400); timer.unref?.();
  }, 'dl-copy', null, { 'aria-label': label });
  return node;
}
// An address box: the origin and an optional path may wrap between them.
const addressBox = (origin, path, label) => { const code = el('code', {}, origin, path ? el('wbr') : null, path || null); return el('div', { class: 'dl-short-box' }, code, copyButton(origin + path, label, code)); };

function card(choice) {
  const file = choice.file, origin = globalThis.location.origin;
  const info = el('div', { class: 'dl-info' },
    el('h2', { class: 'dl-title' }, choice.title, choice.badge ? el('span', { class: 'dl-badge' }, choice.badge) : null),
    el('p', { class: 'dl-text' }, choice.text));
  if (!file) return el('article', { class: 'dl-card is-empty', 'data-product': choice.key }, info,
    el('p', { class: 'dl-missing' }, icon('info'), el('span', {}, t('downloads.1'))));
  const size = formatSize(file.size), day = releaseDate(file.releasedAt, lang);
  return el('article', { class: 'dl-card', 'data-product': choice.key }, info,
    el('div', { class: 'dl-action' },
      el('a', { class: 'button dl-button secondary', href: file.url, download: true, 'aria-label': t('downloads.2', { title: choice.title, version: file.version, value: size ? ` (${size})` : '' }) }, icon('download'), el('span', {}, t('common.9')), size ? el('span', { class: 'dl-size' }, size) : null),
      el('ul', { class: 'dl-meta' }, el('li', {}, t('downloads.3', { version: file.version })), day ? el('li', {}, el('time', { datetime: file.releasedAt }, day)) : null, el('li', {}, fileLabel(file)))),
    el('div', { class: 'dl-more' },
      el('div', { class: 'dl-short' }, el('p', { class: 'dl-short-label' }, choice.shortLabel), addressBox(origin, choice.shortPath, t('downloads.4', { title: choice.title }))),
      choice.alternatives.length ? el('p', { class: 'dl-alt' }, `${choice.alternativesLabel}: `, choice.alternatives.flatMap((entry, index) => [index ? ' · ' : null, el('a', { href: entry.url, download: true, 'aria-label': t('downloads.5', { abi: entry.abi }) }, entry.abi)])) : null,
      choice.note ? el('p', { class: 'dl-note' }, icon('info'), el('span', {}, choice.note)) : null,
      el('details', { class: 'dl-verify', 'data-keep-open': 'dl-verify:' + choice.key }, el('summary', {}, t('downloads.6')),
        el('dl', {}, el('div', {}, el('dt', {}, t('downloads.fileF4')), el('dd', {}, el('code', {}, file.filename))),
          validSha256(file.sha256) ? el('div', {}, el('dt', {}, 'SHA-256'), el('dd', {}, el('code', {}, file.sha256))) : null,
          exactSize(file.size, lang) ? el('div', {}, el('dt', {}, t('downloads.7')), el('dd', {}, exactSize(file.size, lang))) : null))));
}

export function render(ctx) {
  const entry = ctx.resource('downloads'), origin = globalThis.location.origin;
  const header = ctx.pageHeader({ title: t('html.64'), description: t('downloads.8'),
    actions: [el('a', { class: 'button secondary', href: '/download', target: '_blank', rel: 'noopener' }, icon('link'), t('downloads.9'))] });
  if (!entry?.value) return [header, entry?.error ? errorState(t('downloads.10'), entry.error, () => ctx.load('downloads', { force: true })) : loadingState(t('downloads.11'))];
  const choices = downloadChoices(entry.value.releases, { lang });
  const address = (label, path, hint) => el('div', { class: 'dl-address' }, el('p', { class: 'dl-short-label' }, label), addressBox(origin, path, t('downloads.12', { label })), el('small', { class: 'field-hint' }, hint));
  return [header,
    el('div', { class: 'dl-wrap' }, el('div', { class: 'dl-grid cols-' + choices.length }, choices.map(choice => card(choice)))),
    panel({ title: t('downloads.13'), description: t('downloads.14'), className: 'dl-addresses' },
      el('div', { class: 'dl-address-grid' },
        address(t('common.103'), '', t('downloads.15')),
        address(t('downloads.16'), '/download', t('downloads.17'))))];
}

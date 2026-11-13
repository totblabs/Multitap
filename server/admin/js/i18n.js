// The console's language (docs/i18n.md). Korean is the source text and
// English its translation; both catalogs (../i18n/ko.js, ../i18n/en.js) use
// the same ids. The language is the one stored by the top-bar switch
// (localStorage, per browser), else the browser's first language: Korean when
// it starts with "ko", English otherwise. Outside a browser (DOM tests, tools)
// nothing is stored and the source text is used. The language is fixed for
// the life of the page: the switch stores the choice and reloads, so text a
// module builds once at load time is never left in the other language.
export const languageKey = 'multitap-lang';
export const languages = ['ko', 'en'];

export function chooseLanguage(stored, preferred = []) {
  if (languages.includes(stored)) return stored;
  const first = Array.isArray(preferred) ? preferred.find(value => typeof value === 'string' && value) : '';
  return first && first.toLowerCase().startsWith('ko') ? 'ko' : 'en';
}
const stored = () => { try { return globalThis.localStorage?.getItem(languageKey) ?? null; } catch { return null; } };
const browser = typeof globalThis.window !== 'undefined' && typeof globalThis.navigator !== 'undefined';
const preferred = () => { try { const list = globalThis.navigator.languages; return list?.length ? [...list] : [globalThis.navigator.language]; } catch { return []; } };

export const lang = chooseLanguage(stored(), browser ? preferred() : ['ko']);
// Dates and numbers follow the language (Intl).
export const locale = lang === 'ko' ? 'ko-KR' : 'en-US';
const table = (await (lang === 'ko' ? import('../i18n/ko.js') : import('../i18n/en.js'))).default;
const plurals = new Intl.PluralRules(locale);

// t(id) or t(id, { name: value }): {name} in the text takes the value. An
// English entry may be { one, other }, chosen by `count` (or the first number).
export function t(id, params) {
  let text = table[id];
  if (text === undefined) return id;
  if (text !== null && typeof text === 'object') {
    const values = params ? Object.values(params) : [];
    const n = typeof params?.count === 'number' ? params.count : values.find(value => typeof value === 'number');
    text = text[plurals.select(n ?? 0)] ?? text.other;
  }
  return params ? text.replace(/\{(\w+)\}/g, (match, name) => Object.hasOwn(params, name) ? String(params[name]) : match) : text;
}
export const has = id => table[id] !== undefined;

// Release notes are written as a title line, the Korean lines, then the same lines in English, each part after a
// blank line (.artifacts/s112-notes.txt). Only the part in the console's language is shown, under the title; notes
// that are not cleanly Korean-then-English are shown whole.
const hangul = /[ᄀ-ᇿ㄰-㆏가-힯]/, latin = /[A-Za-z]/;
export function localNotes(text, language = lang) {
  if (typeof text !== 'string' || !text.trim()) return text;
  const blocks = text.replace(/\r\n?/g, '\n').trim().split(/\n[ \t]*\n+/).map(block => block.replace(/\s+$/, ''));
  const head = blocks.length > 2 && !hangul.test(blocks[0]) && !blocks[0].includes('\n') && !/^\s*[-*•]/.test(blocks[0]) ? blocks.shift() : null;
  const korean = blocks.map(block => hangul.test(block)), split = korean.indexOf(false);
  if (split <= 0 || korean.slice(split).includes(true) || blocks.slice(split).some(block => !latin.test(block))) return text;
  return [head, ...(language === 'ko' ? blocks.slice(0, split) : blocks.slice(split))].filter(Boolean).join('\n\n');
}

// The static markup of a page (index.html): data-i18n replaces the element's
// one text node (or its whole text), data-i18n-html its inner markup (the
// catalog's own fixed markup, never data), data-i18n-attr="name:id ...".
export function translatePage(root = globalThis.document) {
  const doc = root.ownerDocument ?? root;
  if (lang !== 'ko') {
    for (const node of root.querySelectorAll('[data-i18n-attr]')) for (const pair of node.getAttribute('data-i18n-attr').split(/\s+/)) {
      const at = pair.indexOf(':'); if (at > 0 && has(pair.slice(at + 1))) node.setAttribute(pair.slice(0, at), t(pair.slice(at + 1)));
    }
    for (const node of root.querySelectorAll('[data-i18n-html]')) { const id = node.getAttribute('data-i18n-html'); if (has(id)) node.innerHTML = t(id); }
    for (const node of root.querySelectorAll('[data-i18n]')) {
      const id = node.getAttribute('data-i18n'); if (!has(id)) continue;
      const text = [...node.childNodes].find(child => child.nodeType === 3 && child.nodeValue.trim());
      if (!text) { node.textContent = t(id); continue; }
      const [, before, , after] = /^(\s*)([\s\S]*?)(\s*)$/.exec(text.nodeValue);
      text.nodeValue = before + t(id) + after;
    }
  }
  const html = doc.documentElement;
  if (html) { html.setAttribute('lang', lang); html.classList?.remove('i18n-pending'); }
  syncLanguageSwitch(root);
}

// The switch ([data-language-switch] holding a button per [data-language]):
// the pressed one is the language shown. A press stores the choice and reloads.
export function syncLanguageSwitch(root = globalThis.document) {
  for (const node of root.querySelectorAll('[data-language]')) node.setAttribute('aria-pressed', String(node.getAttribute('data-language') === lang));
}
export function chooseStoredLanguage(next) {
  if (!languages.includes(next)) return false;
  try { globalThis.localStorage?.setItem(languageKey, next); } catch { /* A browser that refuses storage keeps its own language. */ }
  globalThis.document?.documentElement?.setAttribute('lang', next);
  if (next !== lang) globalThis.location?.reload?.();
  return true;
}
export function wireLanguageSwitch(root = globalThis.document) {
  for (const node of root.querySelectorAll('[data-language]')) node.addEventListener('click', () => chooseStoredLanguage(node.getAttribute('data-language')));
  syncLanguageSwitch(root);
}

// The page language before the first paint (docs/i18n.md): the choice the
// language switch stored in this browser, else the browser's first language
// (Korean when it starts with "ko", English otherwise). It sets <html lang>;
// a page whose markup is Korean stays hidden until its module has put the
// English in (js/i18n.js translatePage, console/i18n.js), at most 1.5 s
// (base.css, console.css). Same rule as js/i18n.js chooseLanguage.
(() => {
  const root = globalThis.document.documentElement;
  let lang = null;
  try { lang = globalThis.localStorage?.getItem('multitap-lang') ?? null; } catch { lang = null; }
  if (lang !== 'ko' && lang !== 'en') {
    const list = globalThis.navigator?.languages?.length ? globalThis.navigator.languages : [globalThis.navigator?.language ?? ''];
    lang = String(list[0] ?? '').toLowerCase().startsWith('ko') ? 'ko' : 'en';
  }
  root.setAttribute('lang', lang);
  if (lang !== 'ko' && root.getAttribute('data-i18n-source') === 'ko') root.classList.add('i18n-pending');
})();

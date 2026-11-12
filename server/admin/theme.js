// Appearance: the viewer picks dark or light with a [data-theme-toggle]
// button; that choice is a per-browser convenience in localStorage. Until
// then the public pages and the sign-in screens follow the system
// (prefers-color-scheme) and the administrator console (#console) is light.
// Loaded as a classic script in <head> so the stored choice applies before
// the first paint.
(() => {
  const key = 'multitap-theme';
  const root = globalThis.document.documentElement;
  const stored = () => { try { const value = globalThis.localStorage?.getItem(key); return value === 'light' || value === 'dark' ? value : ''; } catch { return ''; } };
  const query = () => { try { return globalThis.matchMedia?.('(prefers-color-scheme: light)') ?? null; } catch { return null; } };
  // What a toggle's surroundings show now: the console has its own default.
  const effective = node => root.getAttribute('data-theme') || (node?.closest?.('#console') ? 'light' : query()?.matches ? 'light' : 'dark');
  function sync() {
    for (const node of globalThis.document.querySelectorAll('[data-theme-toggle]')) {
      // The page's language (<html lang>, set by lang.js or by the server for the public pages).
      const korean = (root.getAttribute('lang') ?? 'ko').startsWith('ko');
      const label = effective(node) === 'dark' ? (korean ? '밝은 화면으로 전환' : 'Switch to light mode') : (korean ? '어두운 화면으로 전환' : 'Switch to dark mode');
      node.setAttribute('aria-label', label); node.setAttribute('title', label);
    }
  }
  function apply(value) {
    if (value) root.setAttribute('data-theme', value); else root.removeAttribute('data-theme');
    sync();
  }
  apply(stored());
  globalThis.document.addEventListener('click', event => {
    const toggle = event.target?.closest?.('[data-theme-toggle]');
    if (!toggle) return;
    const next = effective(toggle) === 'dark' ? 'light' : 'dark';
    try { globalThis.localStorage?.setItem(key, next); } catch {}
    apply(next);
  });
  globalThis.document.addEventListener('DOMContentLoaded', sync);
  query()?.addEventListener?.('change', sync);
})();

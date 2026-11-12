// Public pages (/ and /download): the 복사 buttons beside the short download
// links and the server address. They stay hidden without JavaScript.
// If Clipboard API is missing, select the visible text for manual copying.
(() => {
  const clipboard = globalThis.navigator?.clipboard;
  const canCopy = globalThis.isSecureContext && typeof clipboard?.writeText === 'function';
  // The release day of each file in the visitor's own calendar day (the server wrote its own, landing.mjs).
  const localDates = () => {
    const korean = (globalThis.document.documentElement.getAttribute('lang') ?? 'ko').startsWith('ko');
    for (const node of globalThis.document.querySelectorAll('time[data-local-date]')) {
      try { const time = new Date(node.getAttribute('datetime')); if (Number.isFinite(time.getTime())) node.textContent = new Intl.DateTimeFormat(korean ? 'ko-KR' : 'en-US', { dateStyle: 'medium' }).format(time); } catch { /* the server's text stays */ }
    }
  };
  const show = () => { for (const node of globalThis.document.querySelectorAll('[data-copy]')) node.hidden = false; localDates(); };
  if (globalThis.document.readyState === 'loading') globalThis.document.addEventListener('DOMContentLoaded', show); else show();
  const timers = new WeakMap(), labels = new WeakMap();
  globalThis.document.addEventListener('click', async event => {
    const button = event.target?.closest?.('[data-copy]');
    if (!button) return;
    if (!labels.has(button)) labels.set(button, button.getAttribute('aria-label') ?? '');
    let copied = true;
    try { if (!canCopy) throw Error('unavailable'); await clipboard.writeText(button.getAttribute('data-copy')); } catch { copied = false;
      try { const node = button.parentElement?.querySelector('code'), selection = globalThis.getSelection?.(), range = document.createRange?.(); if (node && selection && range?.selectNodeContents) { range.selectNodeContents(node); selection.removeAllRanges(); selection.addRange(range); } } catch {}
    }
    // The page's language: the server writes <html lang> from the request's Accept-Language.
    const korean = (globalThis.document.documentElement.getAttribute('lang') ?? 'ko').startsWith('ko');
    button.textContent = copied ? (korean ? '복사됨' : 'Copied') : (korean ? '직접 선택해 복사' : 'Select and copy');
    button.classList.toggle('is-done', copied);
    button.setAttribute('aria-label', button.textContent);
    clearTimeout(timers.get(button));
    timers.set(button, setTimeout(() => { button.textContent = korean ? '복사' : 'Copy'; button.classList.remove('is-done'); button.setAttribute('aria-label', labels.get(button)); }, 2200));
  });
})();

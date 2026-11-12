// The language of the human text the server itself writes (docs/i18n.md):
// the request's Accept-Language, highest quality first. Korean when that
// language starts with "ko", English otherwise (none, unreadable, "*").
// Error responses keep their codes; only pages and sentences use this.
export function requestLanguage(header) {
  if (typeof header !== 'string' || !header || header.length > 1024) return 'en';
  let best = null;
  header.split(',').forEach((part, index) => {
    const [tag, ...parameters] = part.split(';').map(value => value.trim());
    if (!/^[A-Za-z]{1,8}(?:-[A-Za-z0-9]{1,8})*$/.test(tag)) return;
    let quality = 1;
    for (const parameter of parameters) {
      const [name, value] = parameter.split('=').map(item => item.trim());
      if (name === 'q') quality = /^(?:0(?:\.\d{0,3})?|1(?:\.0{0,3})?)$/.test(value ?? '') ? Number(value) : 0;
    }
    if (quality > 0 && (!best || quality > best.quality)) best = { tag, quality, index };
  });
  return best && best.tag.toLowerCase().startsWith('ko') ? 'ko' : 'en';
}

// The operator's language for log lines (docs/i18n.md, as install.sh and
// multitap): MULTITAP_LANG, else LC_ALL, LC_MESSAGES, LANG.
export function operatorLanguage(env = process.env) {
  const value = env.MULTITAP_LANG || env.LC_ALL || env.LC_MESSAGES || env.LANG || '';
  return /^ko/i.test(value) ? 'ko' : 'en';
}

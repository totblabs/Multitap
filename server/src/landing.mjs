import { iconMarkup as icon } from '../admin/icons.js';
import { downloadChoices, latestWindows, latestAndroid, latestFile, formatSize, exactSize, releaseDate, fileLabel, validSha256, productsFor } from '../admin/download-choices.js';

// GET / serves two audiences. Browsers navigating to the server ask for
// text/html explicitly and receive the landing page; every other caller (no
// Accept header, */*, application/json, curl, WinHTTP, Dart HttpClient, health
// probes) keeps the unchanged JSON health object. A tie prefers JSON.
export function prefersHtml(accept) {
  if (typeof accept !== 'string' || !accept || accept.length > 2048) return false;
  let html = 0, json = 0;
  for (const range of accept.split(',')) {
    const [media, ...parameters] = range.split(';').map(part => part.trim().toLowerCase());
    let quality = 1;
    for (const parameter of parameters) {
      const [name, value] = parameter.split('=').map(part => part.trim());
      if (name === 'q') { quality = Number(value); if (!Number.isFinite(quality) || quality < 0) quality = 0; if (quality > 1) quality = 1; }
    }
    if (media === 'text/html' || media === 'application/xhtml+xml') html = Math.max(html, quality);
    else if (media === 'application/json') json = Math.max(json, quality);
  }
  return html > 0 && html > json;
}

// The page text in Korean (the source) and English (docs/i18n.md). The
// server picks the language from the request's Accept-Language (src/i18n.mjs).
const words = {
  ko: {
    copy: '복사', missing: '이 서버에 아직 게시되지 않았어요.<br>서버 관리자에게 문의하세요.', version: v => `버전 ${v}`, apk: abi => `${abi} APK 다운로드`,
    file: '파일', size: '크기', download: '다운로드', downloadLabel: (title, version, size) => `${title} ${version} 다운로드${size ? ` (${size})` : ''}`,
    copyShort: title => `${title} 짧은 주소 복사`, verify: '검증 값',
    apple: 'Apple 기기(iPhone·iPad·Mac)용 앱은 없어요. Windows PC나 Android 기기에서 접속하세요.', other: '이 기기용 앱은 없어요. Windows PC나 Android 기기에서 접속하세요.',
    plain: 'HTTP · 암호화 없음', port: '포트', androidMissing: 'Android 앱은 아직 이 서버에 게시되지 않았어요.',
    serverAddress: '클라이언트에 입력할 서버 주소', copyAddress: '서버 주소 복사', firstPage: 'Multitap for Proxmox VE 첫 화면', theme: '화면 모드 전환',
    skip: '본문으로 건너뛰기', shortcuts: '바로 가기', admin: '관리자 페이지',
    getTitle: 'Multitap 앱 받기', getLead: '이 서버에 접속할 앱을 받으세요. 아이디와 비밀번호는 관리자에게 받으세요.',
    helpBefore: '설치할 때 Windows 경고가 뜨면: ', help: '설치 도움말', helpUrl: 'https://multitap.me/ko/install#edge',
    aboutBefore: 'Multitap 소개와 설치 안내: ', aboutUrl: 'https://multitap.me',
    downloadTitle: '다운로드 · Multitap', downloadDescription: '이 Multitap 서버에 게시된 Windows 클라이언트, Host와 Android 앱의 최신 설치 파일이에요.',
    missingTitle: '아직 게시된 파일이 없어요', missingLead: title => `이 서버에 ${title} 설치 파일이 아직 게시되지 않았어요.<br>서버 관리자에게 문의하세요.`,
    toDownloads: '다운로드 페이지로', missingPageTitle: '파일 없음 · Multitap', missingDescription: '아직 게시되지 않은 설치 파일이에요.',
  },
  en: {
    copy: 'Copy', missing: 'Not published on this server yet.<br>Ask your server administrator.', version: v => `Version ${v}`, apk: abi => `Download ${abi} APK`,
    file: 'File', size: 'Size', download: 'Download', downloadLabel: (title, version, size) => `Download ${title} ${version}${size ? ` (${size})` : ''}`,
    copyShort: title => `Copy the ${title} short link`, verify: 'Verification values',
    apple: 'There is no app for Apple devices (iPhone, iPad, Mac). Connect from a Windows PC or an Android device.', other: 'There is no app for this device. Connect from a Windows PC or an Android device.',
    plain: 'HTTP · not encrypted', port: 'port', androidMissing: 'The Android app is not published on this server yet.',
    serverAddress: 'Server address to enter in the client', copyAddress: 'Copy the server address', firstPage: 'Multitap for Proxmox VE home', theme: 'Switch light or dark mode',
    skip: 'Skip to content', shortcuts: 'Shortcuts', admin: 'Admin console',
    getTitle: 'Get the Multitap apps', getLead: 'Get the apps to connect to this server. Your administrator gives you a username and password.',
    helpBefore: 'Windows warning while installing? ', help: 'Install help', helpUrl: 'https://multitap.me/en/install#edge',
    aboutBefore: 'About Multitap and install guides: ', aboutUrl: 'https://multitap.me/en',
    downloadTitle: 'Download · Multitap', downloadDescription: 'The newest installers of the Windows client, Host and Android app published on this Multitap server.',
    missingTitle: 'No file published yet', missingLead: title => `The ${title} installer is not published on this server yet.<br>Ask your server administrator.`,
    toDownloads: 'Go to downloads', missingPageTitle: 'No file · Multitap', missingDescription: 'This installer is not published yet.',
  },
};
const say = lang => words[lang === 'ko' ? 'ko' : 'en'];

const escapeHtml = value => String(value).replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[char]);

// The newest release per product (the selection rules live in
// admin/download-choices.js, shared with the console's 다운로드 page).
export function latestWindowsReleases(releases) { return { client: latestWindows(releases, 'client'), host: latestWindows(releases, 'host') }; }
export { latestAndroid as latestAndroidRelease, latestFile };

// The visitor's platform from the page request's User-Agent. Only the order
// of the download cards and a one-line note depend on it.
export function platformOf(userAgent) {
  if (typeof userAgent !== 'string' || !userAgent || userAgent.length > 1024) return '';
  if (/Android/i.test(userAgent)) return 'android';
  if (/iPhone|iPad|iPod/i.test(userAgent)) return 'apple';
  if (/Windows/i.test(userAgent)) return 'windows';
  if (/Macintosh|Mac OS X/i.test(userAgent)) return 'apple';
  if (/Linux|X11|CrOS/i.test(userAgent)) return 'other';
  return '';
}

const copyButton = (text, label, w) => `<button type="button" class="dl-copy" data-copy="${escapeHtml(text)}" aria-label="${escapeHtml(label)}" hidden>${w.copy}</button>`;

// One download card: who installs it where, the primary download (newest
// version), a short typeable link, and the verification values on request.
function card(choice, origin, w) {
  const file = choice.file;
  const head = `<h2 class="dl-title">${escapeHtml(choice.title)}${choice.badge ? ` <span class="dl-badge">${escapeHtml(choice.badge)}</span>` : ''}</h2>`;
  const note = file && choice.note ? `<p class="dl-note">${icon('info')}<span>${escapeHtml(choice.note)}</span></p>` : '';
  const info = `<div class="dl-info">${head}<p class="dl-text">${escapeHtml(choice.text)}</p></div>`;
  if (!file) return `<article class="dl-card is-empty" data-product="${choice.key}">${info}<p class="dl-missing">${icon('info')}<span>${w.missing}</span></p></article>`;
  // The server's own day here; home.js writes the visitor's over it (data-local-date).
  const size = formatSize(file.size), day = releaseDate(file.releasedAt, w === words.ko ? 'ko' : 'en'), short = origin + choice.shortPath;
  const meta = [w.version(escapeHtml(file.version)), day ? `<time datetime="${escapeHtml(file.releasedAt)}" data-local-date>${escapeHtml(day)}</time>` : '', escapeHtml(fileLabel(file))].filter(Boolean).map(item => `<li>${item}</li>`).join('');
  const alternatives = choice.alternatives.length ? `<p class="dl-alt">${escapeHtml(choice.alternativesLabel)}: ${choice.alternatives.map(entry => `<a href="${escapeHtml(entry.url)}" download aria-label="${escapeHtml(w.apk(entry.abi))}">${escapeHtml(entry.abi)}</a>`).join(' · ')}</p>` : '';
  const verify = [[w.file, `<code>${escapeHtml(file.filename)}</code>`], validSha256(file.sha256) ? ['SHA-256', `<code>${file.sha256}</code>`] : null, exactSize(file.size, w === words.ko ? 'ko' : 'en') ? [w.size, escapeHtml(exactSize(file.size, w === words.ko ? 'ko' : 'en'))] : null]
    .filter(Boolean).map(([label, value]) => `<div><dt>${label}</dt><dd>${value}</dd></div>`).join('');
  return `<article class="dl-card" data-product="${choice.key}">
      ${info}
      <div class="dl-action">
        <a class="button secondary dl-button" href="${escapeHtml(file.url)}" download aria-label="${escapeHtml(w.downloadLabel(choice.title, file.version, size))}">${icon('download')}<span>${w.download}</span>${size ? `<span class="dl-size">${escapeHtml(size)}</span>` : ''}</a>
        <ul class="dl-meta">${meta}</ul>
      </div>
      <div class="dl-more">
        <div class="dl-short"><p class="dl-short-label">${escapeHtml(choice.shortLabel)}</p><div class="dl-short-box"><code>${escapeHtml(origin)}<wbr>${choice.shortPath}</code>${copyButton(short, w.copyShort(choice.title), w)}</div></div>
        ${alternatives}${note}<details class="dl-verify"><summary>${w.verify}</summary><dl>${verify}</dl></details>
      </div>
    </article>`;
}


function addressTransport(origin, w) {
  try {
    const url = new URL(origin);
    if (!['http:', 'https:'].includes(url.protocol) || url.origin !== origin) return '';
    const scheme = url.protocol === 'https:' ? 'HTTPS' : w.plain;
    return scheme + ' · ' + w.port + ' ' + (url.port || (url.protocol === 'https:' ? '443' : '80'));
  } catch { return ''; }
}

function downloadSection(releases, origin, platform, lang = 'ko') {
  const w = say(lang), choices = downloadChoices(releases, { platform, lang });
  const note = platform === 'android' && !choices.some(choice => choice.key === 'android') ? w.androidMissing : platform === 'apple' || platform === 'other' ? w[platform] : '';
  return `<section id="downloads" class="dl-section" aria-label="${w.download}">
    ${note ? `<p class="dl-platform">${icon('info')}<span>${escapeHtml(note)}</span></p>` : ''}
    <div class="dl-wrap"><div class="dl-grid cols-${choices.length}">
      ${choices.map(choice => card(choice, origin, w)).join('\n      ')}
    </div></div>
    <p class="dl-help">${icon('info')}<span>${w.helpBefore}<a href="${w.helpUrl}">${w.help}</a></span></p>
    <div class="dl-server">${icon('link')}<span class="dl-server-label">${w.serverAddress}</span><span class="dl-server-transport">${escapeHtml(addressTransport(origin, w))}</span><span class="dl-server-box"><code>${escapeHtml(origin)}</code>${copyButton(origin, w.copyAddress, w)}</span></div>
  </section>`;
}

const brand = w => `<a class="brand" href="/" aria-label="${w.firstPage}"><span class="brand-mark" aria-hidden="true"></span><span class="wordmark" aria-hidden="true"><span class="wordmark-name"><b>Multitap</b></span><span class="wordmark-for">for <b>Proxmox VE</b></span></span></a>`;
const theme = w => `<button type="button" class="icon-button theme-toggle" data-theme-toggle aria-label="${w.theme}"><span class="when-dark">${icon('sun')}</span><span class="when-light">${icon('moon')}</span></button>`;

// The frame shared by the public pages: strict CSP (only /admin/* scripts
// and styles, no inline code), the site header and the footer.
function page({ title, description, version, main, lang = 'ko' }) {
  const w = say(lang);
  return `<!doctype html>
<html lang="${lang === 'ko' ? 'ko' : 'en'}">
<head>
<link rel="icon" href="/admin/favicon.svg" type="image/svg+xml">
  <meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="color-scheme" content="dark light"><title>${escapeHtml(title)}</title>
  <meta name="description" content="${escapeHtml(description)}">
  <link rel="icon" href="/admin/favicon.svg" type="image/svg+xml">
  <script src="/admin/theme.js"></script><script src="/admin/home.js" defer></script>
  <link rel="stylesheet" href="/admin/base.css"><link rel="stylesheet" href="/admin/home.css"><link rel="stylesheet" href="/admin/downloads.css">
</head>
<body class="home">
  <a class="skip-link" href="#main">${w.skip}</a>
  <header class="site-top"><div class="site-top-inner">${brand(w)}<nav class="site-nav" aria-label="${w.shortcuts}">${theme(w)}<a class="button secondary site-admin" href="/admin/">${icon('lock')}<span>${w.admin}</span></a></nav></div></header>
  <main id="main" tabindex="-1">
${main}
  </main>
  <footer class="site-foot"><div class="site-foot-inner"><p class="site-foot-about">${w.aboutBefore}<a href="${w.aboutUrl}">multitap.me</a></p><span class="site-foot-version">Multitap Server <b class="mono-value">${escapeHtml(version)}</b></span></div></footer>
</body>
</html>
`;
}

const hero = ({ title, lead, compact = false }) => `    <section class="hero${compact ? ' is-compact' : ''}" aria-labelledby="hero-title">
      <div class="hero-inner"><h1 id="hero-title">${title}</h1><p class="hero-lead">${lead}</p></div>
    </section>`;
// GET / and GET /download for browsers: the apps for the people who use this
// server, newest published file per product, its short link and the server
// address to type. Public: product, version, this server's address and the
// published release catalog only. No account, VM, session, credential or
// deployment detail. What Multitap is lives on multitap.me.
function downloadPage({ title, version, publicOrigin, releases, userAgent, lang }) {
  const platform = platformOf(userAgent), w = say(lang);
  const main = `${hero({ title: w.getTitle, lead: w.getLead, compact: true })}
    ${downloadSection(releases, publicOrigin, platform, lang)}`;
  return page({ title: title(w), description: w.downloadDescription, version, main, lang });
}
export const renderLanding = ({ version, publicOrigin, releases = [], userAgent = '', lang = 'ko' }) => downloadPage({ title: () => 'Multitap', version, publicOrigin, releases, userAgent, lang });
// GET /download: the same page under the address to bookmark or type.
export const renderDownloadPage = ({ version, publicOrigin, releases = [], userAgent = '', lang = 'ko' }) => downloadPage({ title: w => w.downloadTitle, version, publicOrigin, releases, userAgent, lang });

// A short link (/get/<key>) whose product has no published file yet.
export function renderMissingRelease({ version, key, lang = 'ko' }) {
  const product = productsFor(lang)[key], w = say(lang);
  const main = `${hero({ title: w.missingTitle, lead: w.missingLead(escapeHtml(product.title)), compact: true })}
    <div class="missing-actions"><a class="button primary" href="/download">${icon('download')}<span>${w.toDownloads}</span></a></div>`;
  return page({ title: w.missingPageTitle, description: w.missingDescription, version, main, lang });
}

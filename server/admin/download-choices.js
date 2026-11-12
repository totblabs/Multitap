// Which file to download for which machine. Shared by the public pages the
// server renders (src/landing.mjs: GET /, GET /download and the /get/<name>
// short links) and the console's 다운로드 page (js/downloads.js), so both offer
// the same files in the same words. Input: release catalog entries as
// /api/v1/downloads publishes them; nothing here reads accounts or VMs.

export const androidAbis = ['arm64-v8a', 'armeabi-v7a', 'x86_64'];
const versionPattern = /^(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})(?:-preview(?:\.[1-9][0-9]{0,8})?)?$/;
// Only this server's own download paths ever become links.
const usable = entry => !!entry && typeof entry.version === 'string' && versionPattern.test(entry.version) &&
  typeof entry.filename === 'string' && /^[A-Za-z0-9._-]{1,120}\.(?:exe|zip|apk)$/.test(entry.filename) && entry.url === '/downloads/' + entry.filename;

// Release order of src/downloads.mjs (compareReleaseVersions): x.y.z
// numerically; a -preview(.N) build sorts before the release of its number.
const compareNumber = (a, b) => a.length === b.length ? (a === b ? 0 : a < b ? -1 : 1) : a.length < b.length ? -1 : 1;
export function compareVersions(a, b) {
  const [av, ap] = a.split('-'), [bv, bp] = b.split('-');
  const aa = av.split('.'), bb = bv.split('.');
  for (let i = 0; i < 3; i++) { const result = compareNumber(aa[i], bb[i]); if (result) return result; }
  if (ap === undefined || bp === undefined) return ap === bp ? 0 : ap === undefined ? 1 : -1;
  return compareNumber(ap.split('.')[1] ?? '0', bp.split('.')[1] ?? '0');
}

// The newest Windows release of a product; an installer is preferred over an
// archive of the same version.
export function latestWindows(releases, product) {
  return (Array.isArray(releases) ? releases : []).filter(entry => usable(entry) && entry.product === product && (entry.format === 'installer' || entry.format === 'archive'))
    .sort((a, b) => compareVersions(b.version, a.version) || Number(b.format === 'installer') - Number(a.format === 'installer'))[0] ?? null;
}

// The newest Android preview: its APKs, ARM64 (almost every phone) first.
export function latestAndroid(releases) {
  const apks = (Array.isArray(releases) ? releases : []).filter(entry => usable(entry) && entry.product === 'android-client' && entry.format === 'apk' && androidAbis.includes(entry.abi));
  if (!apks.length) return null;
  const version = apks.map(entry => entry.version).sort((a, b) => compareVersions(b, a))[0];
  return { version, files: apks.filter(entry => entry.version === version).sort((a, b) => androidAbis.indexOf(a.abi) - androidAbis.indexOf(b.abi)) };
}

// The file a short link (/get/<key>) stands for, or null.
export function latestFile(releases, key) {
  if (key === 'android') return latestAndroid(releases)?.files[0] ?? null;
  return key === 'client' || key === 'host' ? latestWindows(releases, key) : null;
}

// Fixed product text: the product's name and one plain line on where it goes. No performance claims.
export const products = {
  client: { key: 'client', shortPath: '/get/client', title: 'Windows 클라이언트', text: '데스크톱에 접속할 PC에 설치해요.', shortLabel: '짧은 주소' },
  host: { key: 'host', shortPath: '/get/host', title: 'Multitap Host', text: '데스크톱으로 쓸 Windows(VM이나 PC)마다 설치해요.', shortLabel: '짧은 주소' },
  android: { key: 'android', shortPath: '/get/android', title: 'Android 앱', text: '휴대폰과 태블릿에서 데스크톱에 접속해요.', shortLabel: '짧은 주소',
    note: '설치할 때 ‘출처를 알 수 없는 앱’ 설치를 허용해야 할 수 있어요.', alternativesLabel: 'ARM64가 맞지 않는 기기용' },
};
// The same text in English (docs/i18n.md). The public pages choose by the request's Accept-Language,
// the console's 다운로드 page by its own language.
const englishProducts = {
  client: { title: 'Windows client', text: 'Install it on the PC you connect from.', shortLabel: 'Short link' },
  host: { title: 'Multitap Host', text: 'Install it on each Windows (VM or PC) you use as a desktop.', shortLabel: 'Short link' },
  android: { title: 'Android app', text: 'Connect to your desktops from phones and tablets.', shortLabel: 'Short link',
    note: 'You may need to allow installing “unknown apps” when you install it.', alternativesLabel: 'For devices where ARM64 does not fit' },
};
export const productsFor = (lang = 'ko') => lang === 'ko' ? products
  : Object.fromEntries(Object.entries(products).map(([key, value]) => [key, { ...value, ...englishProducts[key] }]));

// The cards in display order. The Windows client and Host always appear (a
// product without a published file says so); the Android preview only when
// this server publishes it. An Android visitor sees it first.
export function downloadChoices(releases, { platform = '', lang = 'ko' } = {}) {
  const android = latestAndroid(releases), products = productsFor(lang);
  const choices = [
    { ...products.client, file: latestWindows(releases, 'client'), alternatives: [] },
    { ...products.host, file: latestWindows(releases, 'host'), alternatives: [] },
    ...(android ? [{ ...products.android, file: android.files[0], alternatives: android.files.slice(1) }] : []),
  ];
  if (platform === 'android' && android) choices.unshift(choices.pop());
  return choices;
}

export function formatSize(bytes) {
  if (!Number.isSafeInteger(bytes) || bytes <= 0) return '';
  if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
  return Math.max(1, Math.round(bytes / 1024)) + ' KB';
}
// "52,428,800 바이트" / "52,428,800 bytes": the exact size to compare with the downloaded file.
export const exactSize = (bytes, lang = 'ko') => Number.isSafeInteger(bytes) && bytes > 0 ? String(bytes).replace(/\B(?=(\d{3})+(?!\d))/g, ',') + (lang === 'ko' ? ' 바이트' : ' bytes') : '';
export const releaseDay = value => typeof value === 'string' && /^\d{4}-\d\d-\d\dT/.test(value) ? value.slice(0, 10) : '';
// The release day as the reader's calendar shows it, in the page's language (docs/i18n.md): "2026. 11. 12." /
// "Nov 12, 2026". In a browser that is the viewer's own day; on the server (the public pages) it is the server's,
// and home.js writes the viewer's over it once the page runs. The UTC day (releaseDay) was a day early in Korea
// for anything released before 9 in the morning.
export function releaseDate(value, lang = 'ko') {
  if (!releaseDay(value)) return '';
  const time = new Date(value);
  if (!Number.isFinite(time.getTime())) return '';
  try { return new Intl.DateTimeFormat(lang === 'ko' ? 'ko-KR' : 'en-US', { dateStyle: 'medium' }).format(time); } catch { return releaseDay(value); }
}
export function fileLabel(entry) {
  if (entry.format === 'apk') return entry.abi === 'arm64-v8a' ? 'ARM64 APK' : `${entry.abi} APK`;
  return entry.format === 'installer' ? 'Windows x64' : 'Windows x64 ZIP';
}
export const validSha256 = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);

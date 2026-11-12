import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, readFile, readdir, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { randomBytes, createHash } from 'node:crypto';
import { parseHTML } from 'linkedom';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer, VERSION } from '../src/http.mjs';
import { prefersHtml, renderLanding, renderDownloadPage, latestWindowsReleases, latestAndroidRelease, platformOf } from '../src/landing.mjs';
import { publishedRelease, compareReleaseVersions } from '../src/downloads.mjs';
import { compareVersions, downloadChoices, latestFile } from '../admin/download-choices.js';
import { packPackage } from '../update/package.mjs';

const origin = 'https://landing-tests.example';
const browser = 'text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8';
const agents = {
  windows: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 Edg/130.0.0.0',
  android: 'Mozilla/5.0 (Linux; Android 14; K) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Mobile Safari/537.36',
  iphone: 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1',
};
const strictPolicy = "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; font-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";
const sha = 'b'.repeat(64);
const release = (product, version, extension = 'exe', extra = {}) => ({ product, version, filename: `Multitap${product === 'host' ? '-Host' : ''}-${version}-windows-x64.${extension}`, size: 52428800, sha256: sha, ...extra });
const apk = (version, abi, extra = {}) => ({ product: 'android-client', format: 'apk', version, abi, filename: `Multitap-${version}-android-${abi}.apk`, size: 24923887, sha256: sha, ...extra });
// The catalog most tests publish: older and archive entries must never win.
const catalog = [release('client', '0.4.9'), release('client', '0.4.10', 'zip'), release('client', '0.4.10', 'exe', { releasedAt: '2026-10-25T09:00:00.000Z' }), release('host', '0.2.61', 'exe', { size: 18765432 }), release('host', '0.2.7'),
  apk('0.1.0-preview.4', 'x86_64'), apk('0.1.0-preview.4', 'arm64-v8a'), apk('0.1.0-preview.4', 'armeabi-v7a'), apk('0.1.0-preview.2', 'arm64-v8a')];

async function fixture(t, releases, files = {}) {
  const downloads = await mkdtemp(join(tmpdir(), 'multitap-landing-'));
  if (releases) await writeFile(join(downloads, 'releases.json'), JSON.stringify(releases));
  for (const [name, bytes] of Object.entries(files)) await writeFile(join(downloads, name), bytes);
  const store = new Store(':memory:');
  store.addUser({ name: 'private-admin-name', notes: 'secret-note', passwordHash: 'x', role: 'admin' });
  store.addDesktop({ id: 'private-vm', name: 'Private VM name', kind: 'vm' });
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: origin, downloadDir: downloads, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); await rm(downloads, { recursive: true, force: true }); });
  const base = `http://127.0.0.1:${server.address().port}`;
  // A Korean browser unless a test says otherwise: the pages follow Accept-Language (docs/i18n.md).
  const get = async (path, accept, { method = 'GET', agent, language = 'ko-KR,ko;q=0.9' } = {}) => {
    const response = await fetch(base + path, { method, redirect: 'manual', headers: { ...(accept === undefined ? {} : { Accept: accept }), ...(agent ? { 'User-Agent': agent } : {}), ...(language ? { 'Accept-Language': language } : {}) } });
    return { status: response.status, headers: response.headers, bytes: Buffer.from(await response.arrayBuffer()) };
  };
  return { get, base };
}
const page = result => parseHTML(result.bytes.toString('utf8')).document;
const cards = document => [...document.querySelectorAll('.dl-card')].map(node => node.getAttribute('data-product'));

test('Accept negotiation picks HTML only for an explicit text/html preference', () => {
  for (const accept of [browser, 'text/html', 'TEXT/HTML; charset=utf-8', 'application/xhtml+xml', 'text/html;q=0.5, */*;q=0.1', 'application/json;q=0.4, text/html'])
    assert.equal(prefersHtml(accept), true, accept);
  for (const accept of [undefined, '', '*/*', 'application/json', 'application/json, */*', 'text/*', 'text/plain', 'text/html;q=0', 'application/json, text/html', 'text/html;q=0.5, application/json', 'text/html;q=abc', 'x'.repeat(2049)])
    assert.equal(prefersHtml(accept), false, String(accept).slice(0, 40));
});

test('GET / keeps the exact JSON health bytes for non-browser callers and /healthz never negotiates', async t => {
  const f = await fixture(t);
  const expected = Buffer.from(JSON.stringify({ service: 'multitap-broker', version: VERSION, status: 'ok', authentication: 'id-password', media: 'relay-v1', transport: { protocol: 'broker-transport-v1', publicOrigin: origin, scheme: 'https', allowInsecureHttp: false, directTls: false } }));
  // No Accept (WinHTTP default, raw sockets), */* (curl, fetch), JSON clients (Windows client probe).
  for (const accept of [undefined, '*/*', 'application/json', 'application/json, text/plain, */*']) {
    for (const path of ['/', '/healthz']) {
      const result = await f.get(path, accept, { agent: agents.windows });
      assert.equal(result.status, 200);
      assert.deepEqual(result.bytes, expected, `${path} ${accept}`);
      assert.equal(result.headers.get('content-type'), 'application/json; charset=utf-8');
      assert.equal(result.headers.get('cache-control'), 'no-store');
      assert.equal(result.headers.get('x-content-type-options'), 'nosniff');
    }
  }
  // The page's card order depends on the User-Agent, so / varies on both.
  assert.equal((await f.get('/', '*/*')).headers.get('vary'), 'Accept, User-Agent, Accept-Language');
  assert.equal((await f.get('/healthz', browser)).headers.get('vary'), null);
  assert.deepEqual((await f.get('/healthz', browser)).bytes, expected, 'a browser asking /healthz still receives JSON');
  assert.equal((await fetch(f.base + '/', { method: 'POST', headers: { Accept: browser } })).status, 404);
});

test('a browser receives the download-first landing page: newest file per product, short links, no private data', async t => {
  const f = await fixture(t, catalog);
  const result = await f.get('/', browser, { agent: agents.windows }), html = result.bytes.toString('utf8');
  assert.equal(result.status, 200);
  assert.equal(result.headers.get('content-type'), 'text/html; charset=utf-8');
  assert.equal(result.headers.get('content-security-policy'), strictPolicy);
  assert.equal(result.headers.get('cache-control'), 'no-store');
  assert.equal(result.headers.get('x-content-type-options'), 'nosniff');
  assert.equal(result.headers.get('referrer-policy'), 'no-referrer');
  assert.equal(result.headers.get('vary'), 'Accept, User-Agent, Accept-Language');
  const document = page(result);
  // Which file for which machine: the PC client, the Host for VMs, then the Android app. Each card is the product's
  // name and one plain line (no amber eyebrow, no icon tile, no highlighted first card).
  assert.deepEqual(cards(document), ['client', 'host', 'android']);
  assert.deepEqual([...document.querySelectorAll('.dl-card')].map(node => [node.querySelector('.dl-title').textContent, node.querySelector('.dl-text').textContent]),
    [['Windows 클라이언트', '데스크톱에 접속할 PC에 설치해요.'], ['Multitap Host', '데스크톱으로 쓸 Windows(VM이나 PC)마다 설치해요.'], ['Android 앱', '휴대폰과 태블릿에서 데스크톱에 접속해요.']]);
  assert.equal(document.querySelector('.dl-where,.dl-icon,.dl-head,.hero-eyebrow,.dl-card.is-primary,a.dl-button.primary'), null);
  assert.ok([...document.querySelectorAll('a.dl-button')].every(link => link.classList.contains('secondary')), 'every download is the same button');
  assert.deepEqual([...document.querySelectorAll('a.dl-button')].map(link => link.getAttribute('href')),
    ['/downloads/Multitap-0.4.10-windows-x64.exe', '/downloads/Multitap-Host-0.2.61-windows-x64.exe', '/downloads/Multitap-0.1.0-preview.4-android-arm64-v8a.apk'], 'newest per product, installer preferred, ARM64 first');
  assert.ok([...document.querySelectorAll('a.dl-button')].every(link => link.hasAttribute('download')));
  assert.deepEqual([...document.querySelectorAll('.dl-card[data-product=android] .dl-alt a')].map(a => a.getAttribute('href')), ['/downloads/Multitap-0.1.0-preview.4-android-armeabi-v7a.apk', '/downloads/Multitap-0.1.0-preview.4-android-x86_64.apk']);
  // No preview badge: the Android card is an ordinary download.
  assert.equal(document.querySelector('.dl-card[data-product=android] .dl-badge'), null);
  assert.doesNotMatch(document.querySelector('.dl-card[data-product=android]').textContent, /미리보기/);
  // Version, size and date compactly; the SHA-256 behind 검증 값.
  assert.match(html, /50\.0 MB/); assert.match(html, /17\.9 MB/); assert.match(html, /2026-10-25/);
  assert.equal(html.includes('0.4.9'), false); assert.equal(html.includes('preview.2'), false);
  assert.ok([...document.querySelectorAll('details.dl-verify')].every(node => node.querySelector('summary').textContent === '검증 값' && !node.hasAttribute('open')));
  assert.ok(document.querySelector('.dl-card[data-product=client] details.dl-verify').textContent.includes(sha));
  assert.match(document.querySelector('.dl-card[data-product=client] details.dl-verify').textContent, /52,428,800 바이트/);
  // Short links that always lead to the newest file, with this installation's own origin.
  assert.deepEqual([...document.querySelectorAll('.dl-short-box code')].map(node => node.textContent), [`${origin}/get/client`, `${origin}/get/host`, `${origin}/get/android`]);
  assert.deepEqual([...document.querySelectorAll('.dl-short-label')].map(node => node.textContent), ['짧은 주소', '짧은 주소', '짧은 주소']);
  assert.deepEqual([...document.querySelectorAll('[data-copy]')].map(node => node.getAttribute('data-copy')), [`${origin}/get/client`, `${origin}/get/host`, `${origin}/get/android`, origin]);
  assert.ok([...document.querySelectorAll('[data-copy]')].every(node => node.hasAttribute('hidden')), 'copy buttons appear only with a working clipboard (home.js)');
  assert.ok(document.querySelector('.dl-server').textContent.includes(origin), 'the actual server address to type into the client');
  assert.ok(html.includes(VERSION));
  assert.ok([...document.querySelectorAll('a')].some(link => link.getAttribute('href') === '/admin/'), 'console link');
  // An independent tool for Proxmox VE: "for" and the amber name (base.css);
  // no trademark notice.
  assert.equal(document.querySelector('.wordmark-for').textContent, 'for Proxmox VE');
  assert.equal(html.includes('등록 상표'), false);
  assert.equal(/with Proxmox|Powered by Proxmox/i.test(html), false);
  // A plain download page for the people who use this server (what Multitap is lives on
  // multitap.me): one short heading and one line, the three cards, the server address. No slogan, no
  // device showcase, no section about running a server.
  assert.equal(document.querySelector('h1').textContent, 'Multitap 앱 받기');
  assert.equal(document.querySelector('.hero-lead').textContent, '이 서버에 접속할 앱을 받으세요. 아이디와 비밀번호는 관리자에게 받으세요.');
  assert.equal(document.querySelectorAll('[data-device],[data-brand-motion],[data-motion-stage],.home-hero,.home-next,.home-ownership,#start,#features,img,canvas,video,iframe').length, 0);
  assert.equal(/내 Windows|어디서든|직접 운영하세요/.test(html), false, 'no slogan or marketing section');
  assert.equal(/brand-motion|home-assets/.test(html), false, 'nothing of the showcase is loaded');
  assert.equal(/시안|fixture|데모/i.test(document.body.textContent), false);
  assert.equal(/\b\d+\s*fps\b|지연 없|무지연|Horizon|Blast/i.test(html), false, 'no performance or product-parity claims');
  assert.deepEqual([...document.querySelectorAll('.site-nav a')].map(link => link.getAttribute('href')), ['/admin/'], 'the top bar: logo, theme and 관리자 페이지');
  assert.ok(document.querySelector('.site-nav [data-theme-toggle]'));
  // Under the cards: where to read about a Windows warning; in the footer: where Multitap is explained.
  const help = document.querySelector('#downloads .dl-help');
  assert.equal(help.textContent, '설치할 때 Windows 경고가 뜨면: 설치 도움말'); assert.equal(help.querySelector('a').getAttribute('href'), 'https://multitap.me/ko/install#edge');
  assert.equal(help.previousElementSibling.getAttribute('class'), 'dl-wrap', 'the help line follows the cards');
  const about = document.querySelector('.site-foot .site-foot-about');
  assert.equal(about.textContent, 'Multitap 소개와 설치 안내: multitap.me'); assert.equal(about.querySelector('a').getAttribute('href'), 'https://multitap.me');
  for (const id of ['downloads', 'main']) assert.ok(document.getElementById(id), id);
  assert.equal(document.querySelectorAll('h1').length, 1);
  for (const secret of ['private-admin-name', 'secret-note', 'Private VM name', 'private-vm']) assert.equal(html.includes(secret), false, secret);
  // Strict CSP: no inline script or style anywhere in the page.
  assert.equal(document.querySelectorAll('style,[style]').length, 0);
  assert.ok([...document.querySelectorAll('script')].every(script => script.getAttribute('src') && !script.textContent.trim()));
  assert.equal(/\son[a-z]+=/i.test(html), false);
  // Every referenced asset is served by this broker with the same strict policy.
  const assets = new Set([...document.querySelectorAll('[src],[href]')].map(node => node.getAttribute('src') ?? node.getAttribute('href')).filter(path => path.startsWith('/admin/') && path !== '/admin/'));
  for (const path of ['/admin/home.js', '/admin/home.css', '/admin/downloads.css', '/admin/theme.js', '/admin/base.css']) assert.ok(assets.has(path), path);
  for (const path of assets) {
    const asset = await f.get(path);
    assert.equal(asset.status, 200, path); assert.equal(asset.headers.get('content-security-policy'), strictPolicy, path);
  }
});

test('the card order and a short note follow the visitor\'s platform (server-side User-Agent)', async t => {
  assert.deepEqual(['windows', 'android', 'iphone'].map(name => platformOf(agents[name])), ['windows', 'android', 'apple']);
  assert.equal(platformOf('Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15'), 'apple');
  assert.equal(platformOf('Mozilla/5.0 (X11; Linux x86_64)'), 'other');
  assert.equal(platformOf(undefined), ''); assert.equal(platformOf('x'.repeat(1025)), '');
  const f = await fixture(t, catalog);
  const phone = page(await f.get('/', browser, { agent: agents.android }));
  assert.deepEqual(cards(phone), ['android', 'client', 'host'], 'an Android visitor sees the app first');
  assert.equal(phone.querySelector('a.dl-button').getAttribute('href'), '/downloads/Multitap-0.1.0-preview.4-android-arm64-v8a.apk');
  assert.equal(phone.querySelector('h1').textContent, 'Multitap 앱 받기');
  assert.equal(phone.querySelector('.dl-platform'), null);
  const apple = page(await f.get('/download', browser, { agent: agents.iphone }));
  assert.deepEqual(cards(apple), ['client', 'host', 'android']);
  assert.match(apple.querySelector('.dl-platform').textContent, /Apple 기기\(iPhone·iPad·Mac\)용 앱은 없어요\. Windows PC나 Android 기기에서 접속하세요\./);
  assert.equal(page(await f.get('/', browser)).querySelector('.dl-platform'), null, 'no User-Agent: the default order without a note');
  // An Android visitor of a server without the preview is told so.
  const windowsOnly = await fixture(t, [release('client', '0.4.10')]);
  const without = page(await windowsOnly.get('/', browser, { agent: agents.android }));
  assert.deepEqual(cards(without), ['client', 'host']);
  assert.match(without.querySelector('.dl-platform').textContent, /Android 앱은 아직 이 서버에 게시되지 않았어요/);
});

test('short links redirect to the newest published file and answer "not published yet" per caller', async t => {
  const exe = Buffer.from('fixture installer bytes, not a program');
  const client = release('client', '0.4.10', 'exe', { size: exe.length, sha256: createHash('sha256').update(exe).digest('hex') });
  const f = await fixture(t, [release('client', '0.4.9'), client, release('client', '0.4.10', 'zip'), apk('0.1.0-preview.4', 'armeabi-v7a'), apk('0.1.0-preview.4', 'arm64-v8a')], { [client.filename]: exe });
  for (const [path, location] of [['/get/client', '/downloads/Multitap-0.4.10-windows-x64.exe'], ['/GET/Client/', '/downloads/Multitap-0.4.10-windows-x64.exe'], ['/get/android', '/downloads/Multitap-0.1.0-preview.4-android-arm64-v8a.apk']]) {
    for (const accept of [undefined, '*/*', browser]) for (const method of ['GET', 'HEAD']) {
      const result = await f.get(path, accept, { method });
      assert.equal(result.status, 302, `${method} ${path} ${accept}`);
      assert.equal(result.headers.get('location'), location);
      assert.equal(result.headers.get('cache-control'), 'no-store', 'always the newest: never cached');
      assert.equal(result.bytes.length, 0);
    }
  }
  // Following the redirect downloads the published bytes (what a browser in a fresh VM does).
  const followed = await fetch(f.base + '/get/client');
  assert.equal(followed.status, 200); assert.deepEqual(Buffer.from(await followed.arrayBuffer()), exe);
  assert.match(followed.headers.get('content-disposition'), /Multitap-0\.4\.10-windows-x64\.exe/);
  // No Host published: JSON 404 for tools, a page for a browser; unknown names are ordinary 404s.
  for (const accept of [undefined, '*/*', 'application/json']) {
    const missing = await f.get('/get/host', accept);
    assert.equal(missing.status, 404); assert.equal(missing.headers.get('content-type'), 'application/json; charset=utf-8');
    assert.equal(JSON.parse(missing.bytes).error.code, 'release_not_found');
  }
  const missing = await f.get('/get/host', browser), document = page(missing);
  assert.equal(missing.status, 404); assert.equal(missing.headers.get('content-type'), 'text/html; charset=utf-8');
  assert.equal(missing.headers.get('content-security-policy'), strictPolicy);
  assert.match(document.querySelector('h1').textContent, /아직 게시된 파일이 없어요/);
  assert.match(document.querySelector('.hero-lead').textContent, /Multitap Host 설치 파일/);
  assert.ok([...document.querySelectorAll('a')].some(link => link.getAttribute('href') === '/download'));
  for (const path of ['/get/', '/get/admin', '/get/client/x', '/get/' + 'c'.repeat(40)]) assert.equal(JSON.parse((await f.get(path, '*/*')).bytes).error.code, 'not_found', path);
  assert.equal((await fetch(f.base + '/get/client', { method: 'POST', redirect: 'manual' })).status, 404);
});

test('/download is the download page; /downloads and the JSON catalog stay unchanged for API callers', async t => {
  const f = await fixture(t, catalog);
  for (const path of ['/download', '/Download/']) {
    const result = await f.get(path, browser, { agent: agents.windows });
    assert.equal(result.status, 200, path);
    assert.equal(result.headers.get('content-security-policy'), strictPolicy);
    assert.equal(result.headers.get('vary'), 'User-Agent, Accept-Language');
    const document = page(result);
    assert.equal(document.querySelector('h1').textContent, 'Multitap 앱 받기', 'the same plain page as the root');
    assert.equal(document.querySelector('title').textContent, '다운로드 · Multitap');
    assert.deepEqual(cards(document), ['client', 'host', 'android']);
    assert.equal(document.querySelector('#features,.home-next'), null, 'only the downloads');
  }
  assert.equal((await f.get('/download', undefined)).status, 200, 'a human page for any caller');
  assert.equal((await f.get('/download', browser, { method: 'HEAD' })).bytes.length, 0);
  // A browser opening /downloads lands on the page; other callers keep the old answers.
  for (const path of ['/downloads', '/downloads/']) {
    const redirected = await f.get(path, browser);
    assert.equal(redirected.status, 302); assert.equal(redirected.headers.get('location'), '/download'); assert.equal(redirected.headers.get('vary'), 'Accept');
  }
  const bare = await f.get('/downloads', '*/*');
  assert.equal(bare.status, 404); assert.equal(JSON.parse(bare.bytes).error.code, 'not_found');
  const slash = await f.get('/downloads/', undefined);
  assert.equal(slash.status, 404); assert.equal(slash.bytes.length, 0);
  for (const accept of [undefined, '*/*', browser]) {
    const api = await f.get('/api/v1/downloads', accept);
    assert.equal(api.status, 200); assert.equal(api.headers.get('content-type'), 'application/json; charset=utf-8');
    assert.ok(Array.isArray(JSON.parse(api.bytes).releases), String(accept));
  }
});

test('the landing page lists only what the catalog has and escapes catalog text', async t => {
  const empty = await fixture(t);
  const pageText = (await empty.get('/', browser)).bytes.toString('utf8'), document = parseHTML(pageText).document;
  assert.deepEqual(cards(document), ['client', 'host']);
  assert.equal(document.querySelectorAll('.dl-card.is-empty .dl-missing').length, 2);
  assert.match(document.querySelector('.dl-missing').textContent, /아직 게시되지 않았어요/);
  assert.equal(pageText.includes('/downloads/'), false); assert.equal(pageText.includes('/get/'), false);
  assert.equal(document.querySelector('a.dl-button'), null);
  const hostOnly = parseHTML(renderLanding({ version: '1.2.3', publicOrigin: origin, releases: [publishedRelease(release('host', '0.2.61'))] })).document;
  assert.deepEqual([...hostOnly.querySelectorAll('.dl-card:not(.is-empty) .dl-title')].map(node => node.textContent), ['Multitap Host']);
  assert.equal(hostOnly.querySelector('a.dl-button').getAttribute('href'), '/downloads/Multitap-Host-0.2.61-windows-x64.exe');
  const hostile = renderLanding({ version: '<b>1</b>', publicOrigin: 'https://x.example"><script>', releases: [], userAgent: '<script>' });
  assert.equal(hostile.includes('<b>1</b>'), false); assert.equal(hostile.includes('"><script>'), false);
  assert.equal(renderDownloadPage({ version: '1', publicOrigin: 'https://x.example"><script>', releases: [] }).includes('"><script>'), false);
  // Only this server's own download paths become links, whatever a catalog entry says.
  const foreign = downloadChoices([{ product: 'client', version: '9.9.9', filename: 'x.exe', url: 'javascript:alert(1)', format: 'installer' }, { product: 'host', version: '1.0.0', filename: 'Multitap-Host-1.0.0-windows-x64.exe', url: 'https://elsewhere.example/x.exe', format: 'installer' }]);
  assert.ok(foreign.every(choice => choice.file === null));
  assert.deepEqual(latestWindowsReleases([]), { client: null, host: null });
});

test('The newest Android preview gets one card: ARM64 first, the other ABIs as links', () => {
  const releases = [apk('0.1.0-preview.2', 'arm64-v8a'), apk('0.1.0-preview.4', 'x86_64'), apk('0.1.0-preview.4', 'arm64-v8a'), apk('0.1.0-preview.4', 'armeabi-v7a'), release('client', '0.4.76')].map(publishedRelease);
  assert.ok(releases.every(Boolean));
  const android = latestAndroidRelease(releases);
  assert.equal(android.version, '0.1.0-preview.4');
  assert.deepEqual(android.files.map(file => file.abi), ['arm64-v8a', 'armeabi-v7a', 'x86_64']);
  assert.equal(latestFile(releases, 'android').filename, 'Multitap-0.1.0-preview.4-android-arm64-v8a.apk');
  assert.equal(latestFile(releases, 'host'), null); assert.equal(latestFile(releases, 'admin'), null);
  const document = parseHTML(renderLanding({ version: '1.2.3', publicOrigin: origin, releases })).document;
  assert.deepEqual(cards(document), ['client', 'host', 'android']);
  const card = document.querySelector('.dl-card[data-product=android]');
  assert.equal(card.querySelector('.dl-button').getAttribute('href'), '/downloads/Multitap-0.1.0-preview.4-android-arm64-v8a.apk');
  assert.deepEqual([...card.querySelectorAll('.dl-alt a')].map(a => a.getAttribute('href')), ['/downloads/Multitap-0.1.0-preview.4-android-armeabi-v7a.apk', '/downloads/Multitap-0.1.0-preview.4-android-x86_64.apk']);
  assert.match(card.querySelector('.dl-note').textContent, /출처를 알 수 없는 앱/);
  assert.equal(latestAndroidRelease([]), null);
});

test('the shared download selection orders versions exactly like the broker catalog', () => {
  const versions = ['0.1.0', '0.1.0-preview', '0.1.0-preview.2', '0.1.0-preview.10', '0.2.0', '0.10.0', '1.0.0', '0.4.9', '0.4.10', '0.4.100'];
  for (const a of versions) for (const b of versions) assert.equal(Math.sign(compareVersions(a, b)), Math.sign(compareReleaseVersions(a, b)), `${a} vs ${b}`);
});

test('the public pages\' copy buttons use a working clipboard or select exact visible text on HTTP', async t => {
  const html = renderDownloadPage({ version: '1.2.3', publicOrigin: origin, releases: catalog.map(publishedRelease).filter(Boolean) });
  const source = await readFile(new URL('../admin/home.js', import.meta.url), 'utf8');
  const previous = Object.fromEntries(['document', 'navigator', 'isSecureContext', 'getSelection'].map(key => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  t.after(() => { for (const [key, value] of Object.entries(previous)) { if (value) Object.defineProperty(globalThis, key, value); else delete globalThis[key]; } });
  const load = (secure, clipboard) => {
    const { document, Event } = parseHTML(html);
    let selected = null; document.createRange = () => ({ selectNodeContents: node => { selected = node; } });
    Object.defineProperty(globalThis, 'getSelection', { configurable: true, value: () => ({ removeAllRanges() {}, addRange() {} }) });
    Object.defineProperty(globalThis, 'document', { configurable: true, writable: true, value: document });
    Object.defineProperty(globalThis, 'navigator', { configurable: true, writable: true, value: { clipboard } });
    Object.defineProperty(globalThis, 'isSecureContext', { configurable: true, writable: true, value: secure });
    new Function(source)();
    return { document, Event, selected: () => selected };
  };
  const plain = load(false, { writeText: async () => assert.fail('insecure context must not call Clipboard API') });
  const plainButtons = [...plain.document.querySelectorAll('[data-copy]')];
  assert.ok(plainButtons.every(node => !node.hidden), 'HTTP retains explicit manual-copy action');
  plainButtons[0].dispatchEvent(new plain.Event('click', { bubbles: true }));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(plainButtons[0].textContent, '직접 선택해 복사');
  assert.equal(plain.selected(), plainButtons[0].parentElement.querySelector('code'));
  assert.equal(plain.selected().textContent, origin + '/get/client');
  const written = [];
  const secure = load(true, { writeText: async text => { written.push(text); } });
  const buttons = [...secure.document.querySelectorAll('[data-copy]')];
  assert.ok(buttons.length === 4 && buttons.every(node => !node.hidden));
  buttons[1].dispatchEvent(new secure.Event('click', { bubbles: true }));
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(written, [`${origin}/get/host`]);
  assert.equal(buttons[1].textContent, '복사됨');
});

test('every console and landing file is a served asset admissible in a signed server update package', async t => {
  const f = await fixture(t);
  const admin = new URL('../admin/', import.meta.url), files = new Map();
  const walk = async (directory, prefix) => { for (const entry of await readdir(directory, { withFileTypes: true })) entry.isDirectory() ? await walk(new URL(entry.name + '/', directory), prefix + entry.name + '/') : files.set(prefix + entry.name, await readFile(new URL(entry.name, directory))); };
  await walk(admin, 'admin/');
  assert.doesNotThrow(() => packPackage(files));
  // The standalone browser viewer is packaged under admin/console but has
  // its own manifest allowlist and /console/assets routes, not /admin aliases.
  const adminFiles = [...files].filter(([name]) => !name.startsWith('admin/console/'));
  const { document } = parseHTML(await readFile(new URL('index.html', admin), 'utf8'));
  const referenced = [...document.querySelectorAll('[src],[href]')].map(node => node.getAttribute('src') ?? node.getAttribute('href')).filter(path => path.startsWith('/admin/'));
  assert.ok(referenced.includes('/admin/theme.js') && referenced.includes('/admin/base.css') && referenced.includes('/admin/downloads.css'));
  for (const path of [...referenced, '/admin/icons.js', '/admin/home.css', '/admin/home.js', '/admin/download-choices.js', '/admin/scene-dusk.jpg', '/admin/scene-daylight.jpg']) {
    const asset = await f.get(path);
    assert.equal(asset.status, 200, path); assert.equal(asset.headers.get('content-security-policy'), strictPolicy, path);
  }
  assert.equal((await f.get('/admin/mark.svg')).headers.get('content-type'), 'image/svg+xml');
  assert.equal((await f.get('/admin/scene-dusk.jpg')).headers.get('content-type'), 'image/jpeg');
  assert.equal(document.querySelectorAll('style,[style]').length, 0, 'console markup has no inline styles');
  assert.equal(document.querySelectorAll('script:not([src])').length, 0, 'console markup has no inline scripts');
  // Every web file of the console (including modules only reached through
  // imports) is served with the strict policy and the right type.
  const types = { js: 'text/javascript; charset=utf-8', css: 'text/css; charset=utf-8', html: 'text/html; charset=utf-8', svg: 'image/svg+xml', jpg: 'image/jpeg', webp: 'image/webp', png: 'image/png', woff2: 'font/woff2' };
  for (const [name] of adminFiles) {
    const extension = name.split('.').pop();
    if (!types[extension]) continue;
    const asset = await f.get(name === 'admin/index.html' ? '/admin/' : '/' + name);
    assert.equal(asset.status, 200, name); assert.equal(asset.headers.get('content-type'), types[extension], name); assert.equal(asset.headers.get('content-security-policy'), strictPolicy, name);
  }
  // Module imports stay same-origin and resolve to served files.
  for (const [name, bytes] of adminFiles) {
    if (!name.endsWith('.js')) continue;
    for (const [, target] of bytes.toString('utf8').matchAll(/^\s*import\s[^'"]*['"]([^'"]+)['"]/gm)) {
      assert.match(target, /^\.\.?\//, `${name} imports only relative same-origin modules`);
      const resolved = new URL(target, 'https://x.invalid/' + name).pathname;
      assert.equal((await f.get(resolved)).status, 200, `${name} → ${resolved}`);
    }
  }
  assert.ok([...files.keys()].some(name => name.startsWith('admin/js/')), 'console modules are part of the package');
  assert.equal((await f.get('/admin/fonts/LICENSE.txt')).status, 200, 'bundled font license is served with its font');
  assert.equal((await f.get('/admin/console/console.js')).status, 404, 'standalone assets are not exposed through an admin alias');
  assert.equal((await f.get('/admin/console/assets-manifest.json')).status, 404, 'manifest is not a public admin asset');
  // The wordmark's "Proxmox VE" takes Multitap's own amber accent, never Proxmox's orange.
  const base = await readFile(new URL('base.css', admin), 'utf8');
  assert.match(base, /\.wordmark-for b\{[^}]*color:var\(--amber-ink\)/);
  assert.equal(/#E57000/i.test([...files].filter(([name]) => /\.(?:js|css|html|svg)$/.test(name)).map(([, bytes]) => bytes.toString('utf8')).join('\n')), false);
});

async function loadTheme(t, { stored = null, systemLight = false, throwing = false } = {}) {
  const window = parseHTML(await readFile(new URL('../admin/index.html', import.meta.url), 'utf8'));
  const previous = Object.fromEntries(['document', 'localStorage', 'matchMedia'].map(key => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  t.after(() => { for (const [key, value] of Object.entries(previous)) { if (value) Object.defineProperty(globalThis, key, value); else delete globalThis[key]; } });
  const values = new Map(stored ? [['multitap-theme', stored]] : []);
  const storage = throwing ? { getItem() { throw new Error('blocked'); }, setItem() { throw new Error('blocked'); } } : { getItem: key => values.get(key) ?? null, setItem: (key, value) => values.set(key, String(value)) };
  Object.defineProperty(globalThis, 'document', { configurable: true, writable: true, value: window.document });
  Object.defineProperty(globalThis, 'localStorage', { configurable: true, writable: true, value: storage });
  Object.defineProperty(globalThis, 'matchMedia', { configurable: true, writable: true, value: () => ({ matches: systemLight, addEventListener() {} }) });
  await import('../admin/theme.js?' + Math.random());
  const root = window.document.documentElement;
  // The toggle of the console unless another place is named (the sign-in screen: '#login').
  const click = (place = '#console') => window.document.querySelector(place + ' [data-theme-toggle]').dispatchEvent(new window.Event('click', { bubbles: true }));
  return { root, values, click, toggles: () => [...window.document.querySelectorAll('[data-theme-toggle]')] };
}

test('until the viewer picks an appearance the sign-in screens follow the system and the console is light, and the choice persists', async t => {
  const inConsole = node => node.closest('#console') !== null, label = node => node.getAttribute('aria-label');
  // A dark system and no stored choice.
  const system = await loadTheme(t);
  assert.equal(system.root.hasAttribute('data-theme'), false, 'no stored choice: the sign-in screens follow prefers-color-scheme in CSS, the console is light in CSS');
  assert.ok(system.toggles().length >= 3 && system.toggles().some(inConsole));
  assert.ok(system.toggles().filter(node => !inConsole(node)).every(node => label(node) === '밝은 화면으로 전환'), 'the sign-in screens are dark with the system');
  assert.ok(system.toggles().filter(inConsole).every(node => label(node) === '어두운 화면으로 전환'), 'the console is light whatever the system says');
  // In the console the first press goes to dark, the second back to light.
  system.click();
  assert.equal(system.root.getAttribute('data-theme'), 'dark'); assert.equal(system.values.get('multitap-theme'), 'dark');
  assert.ok(system.toggles().every(node => label(node) === '밝은 화면으로 전환'));
  system.click();
  assert.equal(system.root.getAttribute('data-theme'), 'light'); assert.equal(system.values.get('multitap-theme'), 'light');
  assert.ok(system.toggles().every(node => label(node) === '어두운 화면으로 전환'));
  // On a sign-in screen of a dark system the first press goes to light, as before.
  const signIn = await loadTheme(t);
  signIn.click('#login'); assert.equal(signIn.root.getAttribute('data-theme'), 'light'); assert.equal(signIn.values.get('multitap-theme'), 'light');
  const light = await loadTheme(t, { systemLight: true });
  assert.equal(light.toggles()[0].getAttribute('aria-label'), '어두운 화면으로 전환');
  light.click(); assert.equal(light.root.getAttribute('data-theme'), 'dark');
  const remembered = await loadTheme(t, { stored: 'light' });
  assert.equal(remembered.root.getAttribute('data-theme'), 'light');
  const invalid = await loadTheme(t, { stored: 'purple' });
  assert.equal(invalid.root.hasAttribute('data-theme'), false);
  const blocked = await loadTheme(t, { throwing: true });
  blocked.click('#login'); assert.equal(blocked.root.getAttribute('data-theme'), 'light', 'blocked storage still switches this page');
});


test('public address labels use the configured scheme and effective port without inventing an alias', () => {
  const cases = [
    ['https://broker.example', 'HTTPS · 포트 443'],
    ['https://broker.example:18443', 'HTTPS · 포트 18443'],
    ['http://192.0.2.10:18400', 'HTTP · 암호화 없음 · 포트 18400'],
    ['http://192.0.2.10', 'HTTP · 암호화 없음 · 포트 80'],
    ['http://[2001:db8::1]:18400', 'HTTP · 암호화 없음 · 포트 18400'],
  ];
  for (const [publicOrigin, label] of cases) {
    for (const render of [renderLanding, renderDownloadPage]) {
      const { document } = parseHTML(render({ version: '1.2.3', publicOrigin, releases: [] }));
      const address = document.querySelector('.dl-server');
      assert.equal(address.querySelector('.dl-server-transport').textContent, label);
      assert.equal(address.querySelector('code').textContent, publicOrigin);
      assert.deepEqual([...address.querySelectorAll('[data-copy]')].map(node => node.getAttribute('data-copy')), [publicOrigin]);
      assert.equal(document.querySelectorAll('.dl-server').length, 1);
      assert.equal(address.querySelector('a'), null, 'no guessed alternate HTTP or local-IP address');
    }
  }
});

// The cards must not show the UTC day of the ISO text in both languages (a day early in Korea
// before 9 in the morning), and the missing-release page must not have two buttons to the same page.
test('release days follow the page language and the reader\'s own day; the missing-release page has one way on', async () => {
  const { releaseDate } = await import('../admin/download-choices.js');
  const { renderMissingRelease } = await import('../src/landing.mjs');
  const at = '2026-11-11T16:30:00.000Z', local = lang => new Intl.DateTimeFormat(lang === 'ko' ? 'ko-KR' : 'en-US', { dateStyle: 'medium' }).format(new Date(at));
  assert.equal(releaseDate(at, 'ko'), local('ko')); assert.equal(releaseDate(at, 'en'), local('en'));
  assert.match(releaseDate(at, 'en'), /^Nov 1[12], 2026$/); assert.match(releaseDate(at, 'ko'), /^2026\. 11\. 1[12]\.$/);
  for (const value of ['yesterday', '', undefined, null, 20261111, '2026-13-45T99:99:99Z']) assert.equal(releaseDate(value, 'ko'), '', String(value));
  const client = { ...release('client', '0.4.10'), url: '/downloads/Multitap-0.4.10-windows-x64.exe', format: 'installer', releasedAt: at };
  for (const lang of ['ko', 'en']) {
    const { document } = parseHTML(renderLanding({ version: '1.2.3', publicOrigin: origin, releases: [client], lang }));
    const time = document.querySelector('.dl-card[data-product=client] time');
    // The ISO time stays for home.js, which writes the visitor's own day over the server's.
    assert.equal(time.getAttribute('datetime'), at); assert.ok(time.hasAttribute('data-local-date')); assert.equal(time.textContent, local(lang));
    const missing = parseHTML(renderMissingRelease({ version: '1.2.3', key: 'host', lang })).document;
    assert.deepEqual([...missing.querySelectorAll('.missing-actions a')].map(node => node.getAttribute('href')), ['/download']);
  }
  const script = await readFile(new URL('../admin/home.js', import.meta.url), 'utf8');
  assert.match(script, /time\[data-local-date\]/);
});

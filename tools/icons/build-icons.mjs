// Rebuilds every Multitap product icon from the SVG sources in tools/icons/src.
//
//   node tools/icons/build-icons.mjs [--frames <dir>]
//     [--products client,host,client-setup,host-setup,tray,android,mark] [--root <checkout>]
//
// The artwork is the power-strip mark (tools/icons/README.md).
//
// No npm packages: headless Microsoft Edge (or Chrome) rasterises each SVG on a
// canvas at the exact output size, and this script packs the pixels into
// multi-resolution .ico files (BMP frames below 256 px, PNG at 256), Android
// mipmaps and VectorDrawables. Set MULTITAP_ICON_BROWSER to use another
// Chromium executable. --frames also writes every rasterised frame as PNG.
import { readFileSync, writeFileSync, mkdirSync, mkdtempSync, rmSync, existsSync, realpathSync, lstatSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { join, dirname, basename, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { createHash } from 'node:crypto';

const here = import.meta.dirname;
const args = process.argv.slice(2);
const root = args.includes('--root') ? resolve(args[args.indexOf('--root') + 1]) : join(here, '..', '..');
const src = join(root, 'tools', 'icons', 'src');
const framesDir = args.includes('--frames') ? args[args.indexOf('--frames') + 1] : null;

// ---- what gets built --------------------------------------------------------
// Each .ico size uses the hand-tuned grid of that size where one exists (16, 20,
// 24, 32, 40, 48) and the 256 master above that. The four Windows products share
// one drawing, src/windows/tile-<grid>.svg: amber tile, dark mark.
const ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256];
const grid = size => size <= 48 ? size : 256;
const ICONS = [
  ['client', 'apps/windows_client/windows/runner/resources/app_icon.ico'], // Multitap.exe
  ['host', 'apps/windows_host/resources/host.ico'], // MultitapHost.exe, MultitapHostSettings.exe (patched at package time)
  ['client-setup', 'apps/windows_host/resources/client_setup.ico'], // MultitapClientSetup.exe
  ['host-setup', 'apps/windows_host/resources/host_setup.ico'], // Multitap-Host-<version>-windows-x64.exe (the Host installer)
];
// The Host's notification-area icon is the bare mark, src/windows/tray-<size>.svg:
// amber for a dark taskbar, and the same drawing in the dark ink for a light one.
const TRAY_SIZES = [16, 20, 24, 32, 40, 48];
const TRAY_AMBER = '#F5A623', TRAY_INK = '#1E1503';
const TRAYS = [
  ['tray', 'apps/windows_host/resources/host_tray.ico', svg => svg],
  ['tray-light', 'apps/windows_host/resources/host_tray_light.ico', svg => svg.replaceAll(TRAY_AMBER, TRAY_INK)],
];
// Byte copies. Headers of 28 to 34 px take the small drawing, larger places the
// large one; favicons are the 16 px tile (it doubles cleanly to 32).
const COPIES = [
  ['multitap-mark.svg', 'apps/windows_client/assets/brand/multitap.svg'],
  ['multitap-mark.svg', 'website/static/assets/mark.svg'],
  ['multitap-mark-small.svg', 'server/admin/mark.svg'],
  ['windows/tile-16.svg', 'server/admin/favicon.svg'],
  ['windows/tile-16.svg', 'website/static/assets/favicon.svg'],
];
const RES = 'apps/windows_client/android/app/src/main/res';
const MIPMAPS = [['mdpi', 48], ['hdpi', 72], ['xhdpi', 96], ['xxhdpi', 144], ['xxxhdpi', 192]];
const VECTORS = [['foreground', 'multitap_foreground'], ['background', 'multitap_background'], ['monochrome', 'multitap_monochrome'],
  ['symbol', 'multitap_symbol'], ['notification', 'multitap_notification'], ['tv-banner', 'multitap_tv_banner']];
const productNames = [...ICONS.map(([name]) => name), 'tray', 'android', 'mark'];
const requested = args.includes('--products') ? args[args.indexOf('--products') + 1]?.split(',') : productNames;
if (!requested?.length || requested.some(name => !productNames.includes(name))) throw new Error('Invalid --products selection.');
const products = new Set(requested);

// ---- rasterise --------------------------------------------------------------
function browser() {
  const candidates = [process.env.MULTITAP_ICON_BROWSER,
    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
    'C:/Program Files/Google/Chrome/Application/chrome.exe'].filter(Boolean);
  const found = candidates.find(p => existsSync(p));
  if (!found) throw new Error('No Chromium browser found; set MULTITAP_ICON_BROWSER.');
  return found;
}

const sized = (svg, size) => svg.replace(/<svg\b([^>]*?)\swidth="[^"]*"\s+height="[^"]*"/, `<svg$1 width="${size}" height="${size}"`);

function rasterise(jobs) {
  const workParent = realpathSync(tmpdir());
  const work = mkdtempSync(join(workParent, 'multitap-icons-'));
  try {
    const page = join(work, 'raster.html');
    const payload = jobs.map(j => ({ id: j.id, size: j.size, svg: Buffer.from(sized(j.svg, j.size)).toString('base64') }));
    writeFileSync(page, `<!doctype html><meta charset="utf-8"><pre id="out">pending</pre><script id="jobs" type="application/json">${JSON.stringify(payload)}</script><script>
const jobs = JSON.parse(document.getElementById('jobs').textContent), out = {};
const b64 = bytes => { let s = ''; for (let i = 0; i < bytes.length; i += 32768) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 32768)); return btoa(s); };
Promise.all(jobs.map(j => new Promise((done, fail) => {
  const img = new Image();
  img.onload = () => {
    const c = document.createElement('canvas'); c.width = c.height = j.size;
    const g = c.getContext('2d'); g.drawImage(img, 0, 0, j.size, j.size);
    out[j.id] = { png: c.toDataURL('image/png').slice(22), rgba: b64(g.getImageData(0, 0, j.size, j.size).data) };
    done();
  };
  img.onerror = () => fail(new Error('cannot load ' + j.id));
  img.src = 'data:image/svg+xml;base64,' + j.svg;
}))).then(() => { document.getElementById('out').textContent = 'OK' + JSON.stringify(out); }, e => { document.getElementById('out').textContent = 'ERROR ' + e.message; });
</script>`);
    const run = spawnSync(browser(), ['--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check', '--disable-extensions',
      `--user-data-dir=${join(work, 'profile')}`, '--virtual-time-budget=20000', '--dump-dom', 'file:///' + page.replace(/\\/g, '/')],
    { encoding: 'utf8', maxBuffer: 512 * 1024 * 1024, windowsHide: true });
    const text = (run.stdout ?? '').match(/<pre id="out">([\s\S]*?)<\/pre>/)?.[1] ?? '';
    if (!text.startsWith('OK')) throw new Error('Rasterising failed: ' + (text.slice(0, 200) || run.stderr || 'no output'));
    const result = JSON.parse(text.slice(2).replace(/&quot;/g, '"').replace(/&amp;/g, '&'));
    return Object.fromEntries(Object.entries(result).map(([id, r]) => [id, { png: Buffer.from(r.png, 'base64'), rgba: Buffer.from(r.rgba, 'base64') }]));
  } finally {
    // Delete only this invocation's verified temporary directory, never a
    // redirected or computed directory outside its resolved temporary parent.
    const target = realpathSync(work);
    if (dirname(target) !== workParent || !basename(target).startsWith('multitap-icons-') || lstatSync(work).isSymbolicLink()) {
      throw new Error('Refusing icon temporary cleanup outside its owned directory.');
    }
    try { rmSync(target, { recursive: true, force: true, maxRetries: 5, retryDelay: 200 }); } catch { /* the browser may still hold its profile briefly */ }
  }
}

// ---- .ico -------------------------------------------------------------------
// Frames below 256 px are 32-bit BGRA DIBs with an AND mask (what every Windows
// API and resource compiler reads); the 256 px frame is PNG.
function dib(size, rgba) {
  const maskStride = Math.ceil(size / 32) * 4;
  const out = Buffer.alloc(40 + size * size * 4 + maskStride * size);
  out.writeUInt32LE(40, 0); out.writeInt32LE(size, 4); out.writeInt32LE(size * 2, 8);
  out.writeUInt16LE(1, 12); out.writeUInt16LE(32, 14); out.writeUInt32LE(0, 16);
  out.writeUInt32LE(size * size * 4 + maskStride * size, 20);
  for (let y = 0; y < size; y++) {
    const row = size - 1 - y; // bottom-up
    for (let x = 0; x < size; x++) {
      const s = (y * size + x) * 4, d = 40 + (row * size + x) * 4;
      out[d] = rgba[s + 2]; out[d + 1] = rgba[s + 1]; out[d + 2] = rgba[s]; out[d + 3] = rgba[s + 3];
      if (rgba[s + 3] === 0) { const m = 40 + size * size * 4 + row * maskStride + (x >> 3); out[m] |= 0x80 >> (x & 7); }
    }
  }
  return out;
}

function ico(frames) {
  const header = Buffer.alloc(6 + frames.length * 16);
  header.writeUInt16LE(0, 0); header.writeUInt16LE(1, 2); header.writeUInt16LE(frames.length, 4);
  let offset = header.length;
  const blobs = frames.map(({ size, png, rgba }, i) => {
    const data = size >= 256 ? png : dib(size, rgba);
    const e = 6 + i * 16;
    header[e] = size >= 256 ? 0 : size; header[e + 1] = size >= 256 ? 0 : size;
    header.writeUInt16LE(1, e + 4); header.writeUInt16LE(32, e + 6);
    header.writeUInt32LE(data.length, e + 8); header.writeUInt32LE(offset, e + 12);
    offset += data.length;
    return data;
  });
  return Buffer.concat([header, ...blobs]);
}

// ---- SVG subset -> Android VectorDrawable -----------------------------------
function parseXml(text) {
  const rootNode = { name: '#root', attrs: {}, children: [] }, stack = [rootNode];
  const re = /<!--[\s\S]*?-->|<\?[\s\S]*?\?>|<\/([\w:-]+)\s*>|<([\w:-]+)((?:\s+[\w:-]+="[^"]*")*)\s*(\/?)>/g;
  for (let m; (m = re.exec(text));) {
    if (m[1]) { if (stack.pop().name !== m[1]) throw new Error('Mismatched </' + m[1] + '>'); continue; }
    if (!m[2]) continue;
    const node = { name: m[2], attrs: Object.fromEntries([...m[3].matchAll(/([\w:-]+)="([^"]*)"/g)].map(a => [a[1], a[2]])), children: [] };
    stack.at(-1).children.push(node);
    if (!m[4]) stack.push(node);
  }
  return rootNode.children[0];
}

const num = v => +(+v).toFixed(4);
const roundRect = (x, y, w, h, r) => r > 0
  ? `M${num(x + r)},${num(y)}H${num(x + w - r)}A${r},${r} 0 0,1 ${num(x + w)},${num(y + r)}V${num(y + h - r)}A${r},${r} 0 0,1 ${num(x + w - r)},${num(y + h)}H${num(x + r)}A${r},${r} 0 0,1 ${num(x)},${num(y + h - r)}V${num(y + r)}A${r},${r} 0 0,1 ${num(x + r)},${num(y)}Z`
  : `M${num(x)},${num(y)}H${num(x + w)}V${num(y + h)}H${num(x)}Z`;

function pathData(node) {
  const a = node.attrs;
  if (node.name === 'path') return a.d;
  if (node.name === 'rect') return roundRect(+(a.x ?? 0), +(a.y ?? 0), +a.width, +a.height, +(a.rx ?? 0));
  if (node.name === 'circle') { const [cx, cy, r] = [+a.cx, +a.cy, +a.r]; return `M${num(cx - r)},${num(cy)}A${r},${r} 0 1,0 ${num(cx + r)},${num(cy)}A${r},${r} 0 1,0 ${num(cx - r)},${num(cy)}Z`; }
  throw new Error('Unsupported shape <' + node.name + '>');
}

// translate()/scale() lists fold into one scale + translation, which a
// VectorDrawable <group> applies in the same order (scale, then translate).
function transform(text) {
  let sx = 1, sy = 1, tx = 0, ty = 0;
  const ops = [...(text ?? '').matchAll(/(translate|scale)\(([^)]*)\)/g)].reverse();
  for (const [, op, list] of ops) {
    const [p, q] = list.trim().split(/[\s,]+/).map(Number);
    if (op === 'scale') { const qy = q ?? p; sx *= p; sy *= qy; tx *= p; ty *= qy; } else { tx += p; ty += q ?? 0; }
  }
  if ((text ?? '').replace(/(translate|scale)\([^)]*\)/g, '').trim()) throw new Error('Unsupported transform: ' + text);
  return { sx, sy, tx, ty };
}

const argb = (hex, alpha = 1) => {
  const h = hex.replace('#', '');
  if (!/^[0-9a-fA-F]{6}$/.test(h)) throw new Error('Use #RRGGBB colours in Android sources: ' + hex);
  return '#' + Math.round(alpha * 255).toString(16).padStart(2, '0').toUpperCase() + h.toUpperCase();
};

function vectorDrawable(svgText) {
  const svg = parseXml(svgText);
  const [, , vw, vh] = svg.attrs.viewBox.split(/\s+/).map(Number);
  const ids = {};
  const index = node => { if (node.attrs.id) ids[node.attrs.id] = node; node.children.forEach(index); };
  index(svg);
  const ref = v => { const id = /^url\(#([\w-]+)\)$/.exec(v ?? '')?.[1]; if (id && !ids[id]) throw new Error('Unknown reference ' + v); return id ? ids[id] : null; };
  const lines = [];
  const emit = (node, depth, inheritedFill = svg.attrs.fill) => {
    const pad = '    '.repeat(depth);
    if (node.name === 'defs' || node.name === 'linearGradient' || node.name === 'clipPath') return;
    if (node.name === 'g') {
      const t = transform(node.attrs.transform);
      const groupAttrs = [];
      if (t.sx !== 1) groupAttrs.push(`android:scaleX="${num(t.sx)}"`); if (t.sy !== 1) groupAttrs.push(`android:scaleY="${num(t.sy)}"`);
      if (t.tx) groupAttrs.push(`android:translateX="${num(t.tx)}"`); if (t.ty) groupAttrs.push(`android:translateY="${num(t.ty)}"`);
      lines.push(`${pad}<group${groupAttrs.length ? ' ' + groupAttrs.join(' ') : ''}>`);
      const clip = ref(node.attrs['clip-path']);
      if (clip) lines.push(`${pad}    <clip-path android:pathData="${clip.children.map(pathData).join('')}" />`);
      node.children.forEach(c => emit(c, depth + 1, node.attrs.fill ?? inheritedFill));
      lines.push(`${pad}</group>`);
      return;
    }
    if (node.attrs.filter || node.attrs.mask || node.attrs.stroke || node.attrs.opacity) throw new Error('Android sources cannot use filter/mask/stroke/opacity');
    const a = node.attrs, alpha = a['fill-opacity'] !== undefined ? +a['fill-opacity'] : 1, fill = a.fill ?? inheritedFill;
    const attrs = [`android:pathData="${pathData(node)}"`];
    if (a['fill-rule'] === 'evenodd') attrs.push('android:fillType="evenOdd"');
    const gradient = ref(fill);
    if (!gradient) {
      attrs.push(`android:fillColor="${argb(fill)}"`);
      if (alpha !== 1) attrs.push(`android:fillAlpha="${num(alpha)}"`);
      lines.push(`${pad}<path ${attrs.join(' ')} />`);
      return;
    }
    if (gradient.name !== 'linearGradient' || gradient.attrs.gradientUnits !== 'userSpaceOnUse') throw new Error('Only userSpaceOnUse linear gradients convert');
    const g = gradient.attrs;
    lines.push(`${pad}<path ${attrs.join(' ')}>`);
    lines.push(`${pad}    <aapt:attr name="android:fillColor">`);
    lines.push(`${pad}        <gradient android:type="linear" android:startX="${g.x1}" android:startY="${g.y1}" android:endX="${g.x2}" android:endY="${g.y2}">`);
    for (const stop of gradient.children) lines.push(`${pad}            <item android:offset="${stop.attrs.offset}" android:color="${argb(stop.attrs['stop-color'], +(stop.attrs['stop-opacity'] ?? 1) * alpha)}" />`);
    lines.push(`${pad}        </gradient>`, `${pad}    </aapt:attr>`, `${pad}</path>`);
  };
  svg.children.forEach(c => emit(c, 1));
  return ['<?xml version="1.0" encoding="utf-8"?>',
    '<!-- Generated by tools/icons/build-icons.mjs from tools/icons/src/android. Do not edit by hand. -->',
    `<vector xmlns:android="http://schemas.android.com/apk/res/android" xmlns:aapt="http://schemas.android.com/aapt"`,
    `    android:width="${vw}dp" android:height="${vh}dp" android:viewportWidth="${vw}" android:viewportHeight="${vh}">`,
    ...lines, '</vector>', ''].join('\n');
}

// ---- build ------------------------------------------------------------------
const read = p => readFileSync(join(src, p), 'utf8');
const digest = bytes => createHash('sha256').update(bytes).digest('hex');
const outputs = [];
const write = (rel, data) => { const p = join(root, rel); mkdirSync(dirname(p), { recursive: true }); writeFileSync(p, data); outputs.push({path:rel,sha256:digest(data)}); console.log('wrote', rel); };
const selectedIcons = ICONS.filter(([variant]) => products.has(variant));
const selectedTrays = products.has('tray') ? TRAYS : [];

const jobs = [];
for (const [variant] of selectedIcons) for (const size of ICO_SIZES) jobs.push({ id: `${variant}-${size}`, size, svg: read(`windows/tile-${grid(size)}.svg`) });
for (const [variant, , colour] of selectedTrays) for (const size of TRAY_SIZES) jobs.push({ id: `${variant}-${size}`, size, svg: colour(read(`windows/tray-${size}.svg`)) });
if (products.has('android')) for (const [density, size] of MIPMAPS) jobs.push({ id: `android-${density}`, size, svg: read('android/legacy.svg') });
const frames = jobs.length ? rasterise(jobs) : {};

if (framesDir) {
  mkdirSync(framesDir, { recursive: true });
  for (const [id, f] of Object.entries(frames)) writeFileSync(join(framesDir, id + '.png'), f.png);
}
for (const [variant, out] of selectedIcons) write(out, ico(ICO_SIZES.map(size => ({ size, ...frames[`${variant}-${size}`] }))));
for (const [variant, out] of selectedTrays) write(out, ico(TRAY_SIZES.map(size => ({ size, ...frames[`${variant}-${size}`] }))));
if (products.has('android')) {
  for (const [density] of MIPMAPS) write(`${RES}/mipmap-${density}/ic_launcher.png`, frames[`android-${density}`].png);
  for (const [layer, name] of VECTORS) write(`${RES}/drawable/${name}.xml`, vectorDrawable(read(`android/${layer}.svg`)));
}
if (products.has('mark')) for (const [from, to] of COPIES) write(to, readFileSync(join(src, from)));
const manifest = { brand: 'Multitap', mark: 'power strip, 2026-11-11', masterSha256: digest(readFileSync(join(src, 'multitap-mark.svg'))),
  smallMasterSha256: digest(readFileSync(join(src, 'multitap-mark-small.svg'))), tile: { color: '#F5A623', ink: '#1E1503', radius: '22.5%' },
  products: [...products], icoSizes: ICO_SIZES, traySizes: TRAY_SIZES, outputs };
if (framesDir) writeFileSync(join(framesDir, 'export-manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
console.log(JSON.stringify(manifest));

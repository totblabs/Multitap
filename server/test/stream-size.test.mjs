// stream-size-v1: exact three-peer negotiation, portrait owned outputs up to
// 2160x3840, the client's decoder limits, mid-epoch picture changes and decoder
// reports; released peers keep exactly today's rules. Real broker HTTP and
// WebSockets with a fake Host/client; no Windows display or codec is involved.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { streamSizeProtocol, ownedOutputSize, encodedSize, validDecoderLimits, streamSizeCapability, requestedStreamSize, negotiateStreamSize, withinDecoderLimits, rescaledInfoValid, validDecoderReport } from '../src/stream-size.mjs';
import { safeClientDiagnosticReport } from '../src/client-diagnostics.mjs';

const origin = 'https://stream-size-fixture.example';
const limits = { maxWidth: 3840, maxHeight: 3840, maxPixels: 8294400, maxPixelRate: 0 };
const phone = { maxWidth: 3840, maxHeight: 3840, maxPixels: 8294400, maxPixelRate: 248832000 };
const released = () => ({ service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true,   dynamicResolution: true, viewportProvisioning: true, virtualMonitorControl: true, resizeProtocol: 'epoch-v1', initialViewportProtocol: 'epoch-v1', viewportEncoderLimitsProtocol: 'separate-v1',
  monitors: [{ id: '0', width: 1920, height: 1080, primary: true, modes: [{ width: 1920, height: 1080, refreshRate: 60 }] }] });
const capable = () => ({ ...released(), streamSizeProtocol });
const options = { maxWidth: 3840, maxHeight: 3840, fps: 60, bitrate: 8000000, audio: false, monitor: '0' };
const releasedOptions = { ...options, maxHeight: 2160 };
const body = (viewport, extra = {}) => ({ mode: 'stream', options, resizeProtocol: 'epoch-v1', initialViewportProtocol: 'epoch-v1', initialViewport: { monitor: '0', fps: 60, ...viewport }, viewportEncoderLimitsProtocol: 'separate-v1', streamSizeProtocol, decoderLimits: limits, ...extra });
const releasedBody = viewport => ({ mode: 'stream', options: releasedOptions, resizeProtocol: 'epoch-v1', initialViewportProtocol: 'epoch-v1', initialViewport: { monitor: '0', fps: 60, ...viewport }, viewportEncoderLimitsProtocol: 'separate-v1' });

async function socket(url, authorization, headers = {}) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization, ...headers }, perMessageDeflate: false });
    ws.once('error', reject); ws.once('open', () => {
      const queue = [], waiters = [];
      ws.on('message', (bytes, binary) => {
        const value = binary ? bytes : JSON.parse(bytes.toString('utf8'));
        const at = waiters.findIndex(w => w.predicate(value)); if (at < 0) queue.push(value); else { const w = waiters.splice(at, 1)[0]; clearTimeout(w.timer); w.resolve(value); }
      });
      ws.queue = queue;
      ws.take = (predicate = () => true) => {
        const at = queue.findIndex(predicate); if (at >= 0) return Promise.resolve(queue.splice(at, 1)[0]);
        return new Promise((done, fail) => { const w = { predicate, resolve: done }; w.timer = setTimeout(() => { waiters.splice(waiters.indexOf(w), 1); fail(new Error('fixture_message_timeout')); }, 2000); waiters.push(w); });
      };
      ws.json = value => ws.send(JSON.stringify(value)); resolve(ws);
    });
  });
}
const closed = ws => new Promise(resolve => { if (ws.readyState === WebSocket.CLOSED) resolve(); else ws.once('close', resolve); });

async function fixture(t, caps = capable()) {
  const store = new Store(':memory:'); store.addUser({ name: 'alice', passwordHash: 'isolated-stream-size-fixture' });
  store.syncProxmox([{ vmid: 171, node: 'fixture', name: 'VM 171', status: 'running' }], 'fixture'); store.assign('alice', 'pve-171');
  const token = newToken(), hostToken = newToken(), code = newToken();
  store.createSession(store.findUser('alice'), tokenHash(token), Date.now(), 3600000);
  store.issueEnrollment('pve-171', '12345678-1234-1234-1234-123456789abd', tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), '12345678-1234-1234-1234-123456789abd', { hostname: 'Stream size fixture', version: '0.2.72', tokenHash: tokenHash(hostToken) });
  const vault = new Vault(randomBytes(32)); store.saveSetting('proxmox', vault.seal({ endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture-only' }), 'fixture');
  const server = await buildServer({ store, vault, proxmox: { consoleIdentity: async () => ({}) }, publicOrigin: origin, relayOptions: { authorizationMs: 10, heartbeatMs: 10000 }, logger: { error: v => assert.fail(v) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve)); t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = 'http://127.0.0.1:' + server.address().port, local = path => base.replace('http:', 'ws:') + new URL(path, origin).pathname;
  const host = await socket(local('/api/v1/hosts/channel'), 'Host ' + hostToken, { 'X-Multitap-Host-Protocol': 'capabilities-v1' });
  async function advertise(value) { host.json({ type: 'host.capabilities', capabilities: value }); return host.take(v => v.type === 'host.capabilities.ack'); }
  assert.equal((await advertise(caps)).accepted, true);
  async function call(data) { const response = await fetch(base + '/api/v1/desktops/pve-171/connect', { method: 'POST', headers: { Authorization: 'Bearer ' + token, Origin: origin, 'Content-Type': 'application/json' }, body: JSON.stringify(data) }); return { status: response.status, body: await response.json() }; }
  async function inventory() { const response = await fetch(base + '/api/v1/desktops', { headers: { Authorization: 'Bearer ' + token } }); return (await response.json()).desktops[0]; }
  async function connect(data) { const response = await call(data); assert.equal(response.status, 200, JSON.stringify(response.body)); const client = await socket(local(response.body.webSocketUrl), 'Bearer ' + token); const start = await host.take(v => v.type === 'session.start'); return { session: response.body, client, start }; }
  return { host, advertise, call, connect, inventory };
}
const info = (c, changes = {}) => ({ type: 'stream.info', codec: 'h264', ...c.session.initialViewport, width: c.session.initialViewport.width, height: c.session.initialViewport.height,
  sourceWidth: c.session.initialViewport.width, sourceHeight: c.session.initialViewport.height, fpsTarget: 60, audio: { sampleRate: 48000, channels: 2, format: 's16le', available: false }, ...changes });
function frame({ epoch = 1, key = true } = {}) { const b = Buffer.alloc(16); b[0] = 1; b[1] = key ? 1 : 0; b.writeUInt16LE(epoch, 2); return b; }
async function ready(f, c, changes) { f.host.json(info(c, changes)); const applied = await c.client.take(v => v.type === 'stream.info'); f.host.send(frame()); await c.client.take(Buffer.isBuffer); return applied; }
const code = fn => { try { fn(); return 'accepted'; } catch (error) { return error.code; } };

test('Size, limit, capability, request and report rules are exact', () => {
  assert.equal(ownedOutputSize(1080, 2400), false, 'released bounds: at most 2160 rows');
  assert.equal(ownedOutputSize(1080, 2400, true), true);
  assert.equal(ownedOutputSize(2160, 3840, true), true);
  for (const [w, h] of [[2162, 3840], [2160, 3842], [3842, 2160], [1081, 2400], [318, 2400], [3840, 3840], [2400, 3458]]) assert.equal(ownedOutputSize(w, h, true), false, `${w}x${h}`);
  assert.equal(ownedOutputSize(3840, 2160), true); assert.equal(ownedOutputSize(3840, 2160, true), true);
  assert.equal(encodedSize(1080, 2400), false); assert.equal(encodedSize(1080, 2400, true), true); assert.equal(encodedSize(16, 16, true), true); assert.equal(encodedSize(3840, 2162, true), false);
  assert.equal(validDecoderLimits(limits), true); assert.equal(validDecoderLimits(phone), true);
  for (const bad of [{ ...limits, maxWidth: 4096 }, { ...limits, maxHeight: 100 }, { ...limits, maxPixels: 8294401 }, { ...limits, maxPixelRate: 1 }, { ...limits, maxPixelRate: 497664001 }, { ...limits, extra: 1 }, { maxWidth: 3840, maxHeight: 3840, maxPixels: 8294400 }, { ...limits, maxWidth: 3840.5 }, null, [], 'limits'])
    assert.equal(validDecoderLimits(bad), false, JSON.stringify(bad));
  assert.equal(streamSizeCapability(capable()), true);
  for (const value of [released(), { ...capable(), streamSizeProtocol: 'stream-size-unknown' }, { ...capable(), viewportEncoderLimitsProtocol: undefined }, undefined, null]) assert.equal(streamSizeCapability(value), false);
  assert.equal(requestedStreamSize('stream', undefined, undefined, true, null), null);
  assert.deepEqual(requestedStreamSize('stream', streamSizeProtocol, limits, true, null), limits);
  for (const args of [['rdp', streamSizeProtocol, limits, true, null], ['stream', 'stream-size-unknown', limits, true, null], ['stream', streamSizeProtocol, undefined, true, null], ['stream', undefined, limits, true, null],
    ['stream', streamSizeProtocol, limits, false, null], ['stream', streamSizeProtocol, limits, true, [{ id: '0' }]], ['stream', streamSizeProtocol, { ...limits, maxWidth: 100 }, true, null]])
    assert.equal(code(() => requestedStreamSize(...args)), 'invalid_stream_size', JSON.stringify(args.slice(0, 2)));
  const viewport = { width: 1080, height: 2400 }, landscape = { width: 2560, height: 1440 };
  assert.equal(negotiateStreamSize(limits, { capabilities: capable() }, null, viewport, options), limits);
  assert.equal(negotiateStreamSize(null, { capabilities: capable() }, null, landscape, releasedOptions), null, 'never granted unrequested');
  assert.equal(negotiateStreamSize(limits, { capabilities: released() }, null, landscape, releasedOptions), null, 'released Host and released geometry: not granted, not refused');
  assert.equal(code(() => negotiateStreamSize(limits, { capabilities: released() }, null, viewport, releasedOptions)), 'stream_size_unavailable', 'portrait needs the grant');
  assert.equal(code(() => negotiateStreamSize(limits, { capabilities: released() }, null, landscape, options)), 'stream_size_unavailable', 'a portrait box needs the grant');
  // Decoder limits count whole macroblocks per frame and per second.
  assert.equal(withinDecoderLimits({ width: 3840, height: 2160 }, limits, 60), true);
  assert.equal(withinDecoderLimits({ width: 2398, height: 3458 }, limits, 60), false, '2400x3472 coded exceeds 8,294,400');
  assert.equal(withinDecoderLimits({ width: 3840, height: 2160 }, phone, 60), false, '4K60 above a 4K30 decoder');
  assert.equal(withinDecoderLimits({ width: 3840, height: 2160 }, phone, 30), true);
  assert.equal(withinDecoderLimits({ width: 1080, height: 2400 }, { ...limits, maxHeight: 2304 }, 60), false);
  assert.equal(withinDecoderLimits({ width: 9999, height: 9999 }, null, 60), true, 'ungranted sessions are not checked here');
  assert.equal(validDecoderReport({ type: 'stream.decoder', busy: 750, backlogMs: 12 }), true);
  assert.equal(validDecoderReport({ type: 'stream.decoder', busy: -1, backlogMs: 0 }), true);
  for (const value of [{ type: 'stream.decoder', busy: 1001, backlogMs: 0 }, { type: 'stream.decoder', busy: -2, backlogMs: 0 }, { type: 'stream.decoder', busy: 5.5, backlogMs: 0 }, { type: 'stream.decoder', busy: 5, backlogMs: 60001 },
    { type: 'stream.decoder', busy: 5 }, { type: 'stream.decoder', busy: 5, backlogMs: 0, sessionId: 'x' }, { type: 'stream.decoders', busy: 5, backlogMs: 0 }, null, []])
    assert.equal(validDecoderReport(value), false, JSON.stringify(value));
  const session = { id: 's', epoch: 3, source: { width: 1080, height: 2400 }, options, streamSize: limits };
  const rescaled = { sessionId: 's', epoch: 3, sourceWidth: 1080, sourceHeight: 2400, fpsTarget: 60, width: 864, height: 1920 };
  assert.equal(rescaledInfoValid(rescaled, session), true);
  for (const delta of [{ epoch: 2 }, { sessionId: 't' }, { sourceWidth: 1078 }, { fpsTarget: 30 }, { width: 1082 }, { height: 2402 }]) assert.equal(rescaledInfoValid({ ...rescaled, ...delta }, session), false, JSON.stringify(delta));
  assert.equal(rescaledInfoValid(rescaled, { ...session, source: null }), false);
});

test('A capable Host advertises it; a portrait viewport is granted end to end with the decoder limits', async t => {
  const f = await fixture(t);
  assert.equal((await f.inventory()).host.capabilities.streamSizeProtocol, streamSizeProtocol);
  const c = await f.connect(body({ width: 1080, height: 2400 }));
  assert.equal(c.session.streamSizeProtocol, streamSizeProtocol);
  assert.deepEqual(c.session.options, options, 'the per-side box up to 3840x3840 is accepted');
  assert.equal(c.start.streamSizeProtocol, streamSizeProtocol);
  assert.deepEqual(c.start.decoderLimits, limits);
  assert.equal(c.start.initialViewport.width, 1080); assert.equal(c.start.initialViewport.height, 2400); assert.equal(c.start.height, 3840);
  // The native portrait picture (1:1) reaches the client.
  const applied = await ready(f, c);
  assert.equal(applied.width, 1080); assert.equal(applied.height, 2400);
  // The Host restarts its encoder at a smaller scale of the same source.
  const sessionId = c.session.sessionId;
  f.host.json(info(c, { width: 864, height: 1920, encodeScale: 800, encodeLimit: 'host_cpu' }));
  const rescaled = await c.client.take(v => v.type === 'stream.info');
  assert.equal(rescaled.width, 864); assert.equal(rescaled.encodeLimit, 'host_cpu'); assert.equal(rescaled.sessionId, sessionId);
  f.host.send(frame()); await c.client.take(Buffer.isBuffer);
  // Decoder reports reach the Host with the session id; extra ones within the
  // same ~half second are dropped; malformed ones end the session.
  c.client.json({ type: 'stream.decoder', busy: 930, backlogMs: 40 });
  assert.deepEqual(await f.host.take(v => v.type === 'stream.decoder'), { type: 'stream.decoder', sessionId, busy: 930, backlogMs: 40 });
  c.client.json({ type: 'stream.decoder', busy: 931, backlogMs: 40 });
  c.client.json({ type: 'ping', id: 'barrier' }); await f.host.take(v => v.type === 'ping' && v.id === 'barrier');
  assert.equal(f.host.queue.some(v => v.type === 'stream.decoder'), false, 'rate limited');
  // A portrait resize of a granted session is dispatched.
  c.client.json({ type: 'stream.resize', requestId: 'portrait', width: 1440, height: 3200, fps: 60, monitor: '0' });
  const resize = await f.host.take(v => v.type === 'stream.resize');
  assert.equal(resize.width, 1440); assert.equal(resize.height, 3200);
  const pending = await c.client.take(v => v.type === 'stream.resizing');
  f.host.json({ ...info(c), ...pending, type: 'stream.info', sessionId, width: 1440, height: 3200, sourceWidth: 1440, sourceHeight: 3200 });
  assert.equal((await c.client.take(v => v.type === 'stream.info')).height, 3200);
  c.client.json({ type: 'stream.decoder', busy: 1001, backlogMs: 0 });
  assert.equal((await c.client.take(v => v.type === 'session.ended')).reason, 'invalid_input');
});

test('A Host picture above the declared decoder limits is refused', async t => {
  const f = await fixture(t);
  const c = await f.connect(body({ width: 3840, height: 2160 }, { decoderLimits: phone }));
  // 4K at 60 fps exceeds a 4K30 decoder: the initial metadata fails the session.
  f.host.json(info(c));
  assert.equal((await c.client.take(v => v.type === 'stream.resize.failed')).reason, 'invalid_resize_info');
  const next = await f.connect(body({ width: 3840, height: 2160 }, { decoderLimits: phone }));
  await ready(f, next, { width: 2560, height: 1440 });
  // A mid-epoch picture beyond the limits drops the Host.
  f.host.json(info(next, { width: 3840, height: 2160 }));
  await closed(f.host);
});

test('A Fold connected folded gets its whole unfolded picture', async t => {
  // A foldable phone: connected folded at 1248x1868, unfolded to
  // a 2448x1848 output. The box is the per-side ceiling, not the folded size,
  // so the unfolded picture within a 4K60 decoder is relayed whole.
  const f = await fixture(t);
  const fold = { maxWidth: 3840, maxHeight: 3840, maxPixels: 8294400, maxPixelRate: 497664000 };
  const c = await f.connect(body({ width: 1248, height: 1868 }, { decoderLimits: fold }));
  assert.deepEqual(c.session.options, options);
  assert.equal(c.start.width, 3840); assert.equal(c.start.height, 3840);
  await ready(f, c);
  const sessionId = c.session.sessionId;
  c.client.json({ type: 'stream.resize', requestId: 'unfold', width: 2448, height: 1848, fps: 60, monitor: '0' });
  const resize = await f.host.take(v => v.type === 'stream.resize');
  assert.equal(resize.width, 2448); assert.equal(resize.height, 1848);
  const pending = await c.client.take(v => v.type === 'stream.resizing');
  f.host.json({ ...info(c), ...pending, type: 'stream.info', sessionId, width: 2448, height: 1848, sourceWidth: 2448, sourceHeight: 1848 });
  const unfolded = await c.client.take(v => v.type === 'stream.info');
  assert.equal(unfolded.width, 2448); assert.equal(unfolded.height, 1848);
});

test('Released Hosts and malformed requests: never granted, portrait refused, today\'s rules kept', async t => {
  const f = await fixture(t, released());
  assert.equal((await f.inventory()).host.capabilities.streamSizeProtocol, undefined);
  assert.equal((await f.call(body({ width: 1080, height: 2400 }))).body.error.code, 'stream_size_unavailable');
  assert.equal((await f.call(body({ width: 2560, height: 1440 }))).body.error.code, 'stream_size_unavailable', 'a 3840x3840 box needs the grant');
  // Released geometry with the request: not granted, not refused, today's session.
  const c = await f.connect(body({ width: 2560, height: 1440 }, { options: releasedOptions }));
  assert.equal(c.session.streamSizeProtocol, undefined); assert.equal(c.start.streamSizeProtocol, undefined); assert.equal(c.start.decoderLimits, undefined);
  await ready(f, c, { width: 2560, height: 1440 });
  c.client.json({ type: 'stream.decoder', busy: 10, backlogMs: 0 });
  assert.equal((await c.client.take(v => v.type === 'session.ended')).reason, 'invalid_input');
  await f.host.take(v => v.type === 'session.stop');
  for (const delta of [{ decoderLimits: undefined }, { streamSizeProtocol: 'stream-size-unknown' }, { decoderLimits: { ...limits, maxPixels: 1 } }, { viewportEncoderLimitsProtocol: undefined }, { initialViewportProtocol: undefined, initialViewport: undefined }])
    assert.equal((await f.call(body({ width: 2560, height: 1440 }, { options: releasedOptions, ...delta }))).status, 400, JSON.stringify(delta));
  // RDP is not offered: refused as such before its fields are read.
  const rdp = await f.call({ mode: 'rdp', streamSizeProtocol, decoderLimits: limits });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported');
  // Without the request a portrait viewport or box is the released 400.
  assert.equal((await f.call(releasedBody({ width: 1080, height: 2400 }))).body.error.code, 'invalid_initial_viewport');
  assert.equal((await f.call({ ...releasedBody({ width: 2560, height: 1440 }), options })).body.error.code, 'invalid_stream_options');
});

test('A capable Host with a released client keeps today\'s sizes and session.start', async t => {
  const f = await fixture(t);
  const c = await f.connect(releasedBody({ width: 2560, height: 1440 }));
  assert.equal(c.session.streamSizeProtocol, undefined); assert.equal(c.start.streamSizeProtocol, undefined); assert.equal(c.start.decoderLimits, undefined);
  await ready(f, c, { width: 2560, height: 1440 });
  // A portrait resize is invalid input for an ungranted session.
  c.client.json({ type: 'stream.resize', requestId: 'portrait', width: 1080, height: 2400, fps: 60, monitor: '0' });
  assert.equal((await c.client.take(v => v.type === 'session.ended')).reason, 'invalid_input');
  await f.host.take(v => v.type === 'session.stop');
  // A portrait picture of an ungranted session is invalid stream metadata.
  const next = await f.connect(releasedBody({ width: 2560, height: 1440 }));
  f.host.json(info(next, { width: 1080, height: 2400, sourceWidth: 2560, sourceHeight: 1440 }));
  await closed(f.host);
});

test('Capabilities: an incoherent marker is omitted, portrait modes are valid', async t => {
  const f = await fixture(t, { ...released(), streamSizeProtocol, viewportEncoderLimitsProtocol: undefined, initialViewportProtocol: 'epoch-v1' });
  assert.equal((await f.inventory()).host.capabilities.streamSizeProtocol, undefined);
  assert.equal((await f.advertise({ ...capable(), monitors: [{ id: '0', width: 1080, height: 2400, primary: true, modes: [{ width: 1080, height: 2400, refreshRate: 60 }] }] })).accepted, true);
  assert.equal((await f.inventory()).host.capabilities.monitors[0].height, 2400);
  assert.equal((await f.advertise({ ...capable(), monitors: [{ id: '0', width: 2160, height: 3842, primary: true, modes: [{ width: 2160, height: 3842, refreshRate: 60 }] }] })).accepted, false);
});

test('Stream size diagnostics pass the client diagnostics projection', () => {
  const report = safeClientDiagnosticReport({ protocol: 'client-diagnostics-v1', runId: 'c'.repeat(32), sequence: 1, version: null,
    events: [{ event: 'streamStats', time: '2026-10-26T12:00:00.000Z', values: { samples: 10, encodeScale: 830, encodeLimit: 4, decoderBusy: 640, futureField: 7 } }] });
  assert.deepEqual(report.events[0].values, { samples: 10, encodeScale: 830, encodeLimit: 4, decoderBusy: 640 });
});

test('v2 additive capability keeps a 5K workspace while encoded pictures and later limits change independently',async t=>{
  const v2='stream-size-v2';
  const f=await fixture(t,{...capable(),streamSizeProtocols:[streamSizeProtocol,v2]});
  assert.deepEqual((await f.inventory()).host.capabilities.streamSizeProtocols,[streamSizeProtocol,v2]);
  const large={maxWidth:8192,maxHeight:8192,maxPixels:8192*8192,maxPixelRate:0};
  const c=await f.connect(body({width:5120,height:2880},{streamSizeProtocol:v2,decoderLimits:large,options:{...options,maxWidth:8192,maxHeight:8192}}));
  assert.equal(c.start.initialViewport.width,5120);assert.equal(c.start.initialViewport.height,2880);
  assert.equal(c.start.streamSizeProtocol,v2);assert.equal(c.session.streamSizeProtocol,v2);
  await ready(f,c,{width:2560,height:1440});
  const smaller={maxWidth:2560,maxHeight:1440,maxPixels:2560*1440,maxPixelRate:0};
  c.client.json({type:'stream.resize',requestId:'new-capacity',width:5120,height:1440,fps:60,monitor:'0',decoderLimits:smaller});
  const resize=await f.host.take(v=>v.type==='stream.resize');
  assert.deepEqual(resize.decoderLimits,smaller);
  const pending=await c.client.take(v=>v.type==='stream.resizing');
  assert.equal(pending.decoderLimits,undefined,'client ACK keeps the existing bounded shape');
  f.host.json({...info(c),...pending,type:'stream.info',sessionId:c.session.sessionId,width:2560,height:720,sourceWidth:5120,sourceHeight:1440});
  const applied=await c.client.take(v=>v.type==='stream.info');
  assert.equal(applied.sourceWidth,5120);assert.equal(applied.width,2560);
});

test('v2 two-output requests do not inherit the old aggregate 4K pixel ceiling',async t=>{
  const v2='stream-size-v2';
  const caps={...released(),viewportEncoderLimitsProtocol:undefined,streamSizeProtocol,
    streamSizeProtocols:[streamSizeProtocol,v2],videoTrackProtocol:'tracks-v1',trackViewportProvisioning:true,
    monitors:[{id:'0',width:3840,height:2160,primary:true,modes:[{width:3840,height:2160,refreshRate:60}]},
      {id:'1',width:3840,height:2160,primary:false,modes:[{width:3840,height:2160,refreshRate:60}]}]};
  const f=await fixture(t,caps);
  const tracks=[{id:'0',width:3840,height:2160,x:0,y:0},{id:'1',width:3840,height:2160,x:3840,y:0}];
  const c=await f.connect({mode:'stream',mediaProtocol:'tracks-v1',tracks,options:{...options,maxHeight:2160,monitor:'all'},streamSizeProtocol:v2,decoderLimits:limits});
  assert.equal(c.session.streamSizeProtocol,v2);assert.equal(c.start.streamSizeProtocol,v2);
  assert.deepEqual(c.start.decoderLimits,limits);assert.equal(c.start.tracks.length,2);
  assert.equal(c.start.tracks.reduce((sum,t)=>sum+t.width*t.height,0),2*3840*2160);
});

// Real loopback HTTP/WebSocket, memory SQLite and synthetic PVE transports.
// No guest, device, foreground input, runtime account or external network.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer, request } from 'node:http';
import { EventEmitter, once } from 'node:events';
import { randomBytes, createHash } from 'node:crypto';
import { mkdtemp, mkdir, writeFile, readFile, rm } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import WebSocket from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { BrowserConsole, consoleCookieName, consolePagePolicy } from '../src/browser-console.mjs';
import { hashPassword, newToken, tokenHash } from '../src/security.mjs';
import { normalizeServerAccess } from '../update/server-access-policy.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

const origin = 'https://console-tests.invalid', password = 'Synthetic-console-tests-only-42!';
const machine = id => '12345678-1234-1234-1234-' + String(id).padStart(12, '0');
let passwordHash;
const hashed = () => passwordHash ??= hashPassword(password);
const confirmation = reply => {
  assert.equal(reply.status,409); assert.equal(reply.body.error.code,'login_confirmation_required');
  assert.equal(reply.headers['set-cookie'],undefined);
  const details=reply.body.error.details; assert.equal(details.protocol,'session-access-v1');
  assert.match(details.challengeToken,/^[A-Za-z0-9_-]{43}$/); assert.ok(details.expiresInMs>0&&details.expiresInMs<=120000);
  return {challengeToken:details.challengeToken,confirm:true};
};
const consoleAuth = reply => {
  assert.equal(reply.status,200);return {reply,cookie:reply.headers['set-cookie'][0].split(';')[0],csrf:reply.body.csrfToken};
};
const pause = () => new Promise(resolve => setImmediate(resolve));
async function until(predicate) { for (let i = 0; i < 100; ++i) { if (predicate()) return; await new Promise(resolve => setTimeout(resolve, 5)); } assert.fail('bounded fixture wait'); }
class Wire extends EventEmitter {
  readyState = 1; sent = [];
  send(bytes, _options, done) { this.sent.push(Buffer.from(bytes)); done?.(); }
  terminate() { if (this.readyState === 3) return; this.readyState = 3; this.emit('close'); }
  close() { this.terminate(); }
  packet(value) { this.emit('message', Buffer.from(JSON.stringify(value)), false); }
  pause() {} resume() {} ping() {}
}
async function assets(directory, extra = {}) {
  const files = { 'index.html':'<!doctype html><title>Fixture viewer</title>', 'console.js':'export const viewer = true;', ...extra };
  const manifest = { schema:1, product:'multitap-browser-console', files:[] };
  for (const [path, text] of Object.entries(files)) {
    const bytes = Buffer.from(text); await mkdir(join(directory, path, '..'), { recursive:true }); await writeFile(join(directory, path), bytes);
    manifest.files.push({ path, bytes:bytes.length, sha256:createHash('sha256').update(bytes).digest('hex') });
  }
  await writeFile(join(directory, 'assets-manifest.json'), JSON.stringify(manifest));
  return manifest;
}
async function fixture(t, options = {}) {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-console-http-')); await assets(directory);
  const reservation = createServer(); await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port; await new Promise(resolve => reservation.close(resolve));
  const config = normalizeServerAccess({ protocol:'server-access-v1', primaryOrigin:origin,
    https:{ enabled:true, origin, mode:'proxy', listenHost:'127.0.0.1', listenPort:port },
    http:{ enabled:false, origin:null, listenHost:'127.0.0.1', listenPort:port }, trustedProxies:['127.0.0.1'] });
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  for (const name of ['alice','bobby','admin']) store.addUser({ name, passwordHash:await hashed(), role:name === 'admin' ? 'admin':'user' });
  store.syncProxmox([149,150].map(vmid => ({ vmid, node:'fixture', name:'Memory VM ' + vmid, status:'running', machineUuid:machine(vmid) })), 'fixture');
  for (const name of ['alice','bobby']) for (const id of ['pve-149','pve-150']) store.assign(name, id);
  store.saveSetting('proxmox', vault.seal({ endpoint:'https://pve.invalid', tokenId:'fixture@pve!fixture', secret:'PRIVATE_PVE_SECRET' }), 'fixture');
  const upstream = [], clock = { time:Date.now() }, proxmox = {
    consoleSnapshot:async (_config, vmid) => ({ node:'fixture', vmid, machineUuid:machine(vmid) }),
    openConsole:async () => { const socket = new Wire(); upstream.push(socket); return { socket, password:'PRIVATE_VNC_SECRET' }; },
  };
  const server = await buildServer({ store, vault, proxmox, publicOrigin:origin, accessConfig:config, trustedProxies:config.trustedProxies,
    initialSetupOptions:{ announce:() => {} }, browserConsoleOptions:{ assetDirectory:directory },
    relayOptions:{ authorizationMs:3600000, heartbeatMs:3600000, consoleOptions:{ now:() => clock.time }, qemuConsoleOptions:{ now:() => clock.time }, imeOptions:{ timeoutMs:200 } },
    logger:{ error:event => assert.fail(JSON.stringify(event)) }, ...options });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); store.close(); await rm(directory, { recursive:true }); });
  function call(path, { method = 'GET', data, headers = {}, cookie, csrf, browserOrigin = origin, proxy = true } = {}) {
    return new Promise((resolve, reject) => {
      const bytes = data === undefined ? null : Buffer.from(JSON.stringify(data));
      const req = request({ hostname:'127.0.0.1', port, path, method, headers:{ Host:new URL(origin).host,
        ...(proxy ? { 'X-Forwarded-Proto':'https' } : {}), ...(browserOrigin ? { Origin:browserOrigin } : {}),
        ...(bytes ? { 'Content-Type':'application/json', 'Content-Length':bytes.length } : {}), ...(cookie ? { Cookie:cookie } : {}),
        ...(csrf ? { 'X-Multitap-Console-CSRF':csrf } : {}), ...headers } }, res => {
        const chunks=[]; res.on('data', value => chunks.push(value)); res.on('end', () => {
          const bytes=Buffer.concat(chunks),text=bytes.toString(); resolve({ status:res.statusCode, headers:res.headers, bytes, text, body:text && res.headers['content-type']?.startsWith('application/json') ? JSON.parse(text) : null });
        });
      }); req.on('error', reject); req.end(bytes);
    });
  }
  async function login(name = 'alice') {
    const result = await call('/console/api/login', { method:'POST', data:{ username:name, password } });
    return consoleAuth(result);
  }
  const connect = auth => call('/console/api/desktops/pve-149/connect', { ...auth, method:'POST', data:{} });
  function socket(id, auth = {}, headers = {}) {
    return new WebSocket(`ws://127.0.0.1:${port}/console/ws/${id}`, { headers:{ Host:new URL(origin).host, 'X-Forwarded-Proto':'https', Origin:origin, ...(auth.cookie ? { Cookie:auth.cookie } : {}), ...headers } });
  }
  async function rejectedSocket(id, auth, headers = {}) {
    const ws = socket(id, auth, headers);
    return new Promise((resolve, reject) => { ws.once('open', () => { ws.terminate(); reject(Error('unexpected upgrade')); }); ws.once('unexpected-response', (_req, res) => { res.resume(); ws.terminate(); resolve(res.statusCode); }); ws.on('error', () => {}); });
  }
  return { directory, store, vault, server, proxmox, upstream, clock, port, call, login, connect, socket, rejectedSocket };
}

test('HTTPS-resolved real HTTP login uses a separate session cookie and projects no bearer', async t => {
  const f=await fixture(t), auth=await f.login();
  assert.deepEqual(Object.keys(auth.reply.body).sort(), ['csrfToken','user']); assert.deepEqual(auth.reply.body.user,{ username:'alice' });
  assert.match(auth.reply.headers['set-cookie'][0], new RegExp('^' + consoleCookieName(origin) + '=[A-Za-z0-9_-]{43}; Path=/console; HttpOnly; Secure; SameSite=Strict$'));
  assert.equal(auth.reply.headers['cache-control'],'no-store'); assert.equal(auth.reply.headers['referrer-policy'],'no-referrer');
  const row=f.store.db.prepare('SELECT * FROM sessions').get(); assert.equal(row.purpose,'client'); assert.notEqual(row.token_hash,auth.cookie.split('=')[1]);
  assert.deepEqual((await f.call('/console/api/session',auth)).body,auth.reply.body);
  assert.equal((await f.call('/console/api/session',{ cookie:'__Secure-multitap-admin='+auth.cookie.split('=')[1] })).status,401);
  assert.equal((await f.call('/console/api/session',{ cookie:auth.cookie+'; '+auth.cookie })).status,401);
  assert.equal((await f.call('/console/api/session',{ headers:{ Authorization:'Bearer '+auth.cookie.split('=')[1] } })).status,401);
  const native=await f.call('/api/v1/auth/login',{ method:'POST',data:{ username:'alice',password } });
  const accepted=await f.call('/api/v1/auth/login/confirm',{method:'POST',data:confirmation(native)});
  assert.equal(accepted.status,200);assert.deepEqual(Object.keys(accepted.body).sort(),['expiresAt','sessionAccess','sessionToken','user']); assert.equal(accepted.headers['set-cookie'],undefined);
  assert.equal((await f.call('/console/api/session',auth)).status,401,'explicit native confirmation retires the browser principal');
});

test('browser duplicate confirmation issues only a scoped cookie after explicit acceptance and cancellation preserves the old owner',async t=>{
  const f=await fixture(t),native=await f.call('/api/v1/auth/login',{method:'POST',data:{username:'alice',password}});
  assert.equal(native.status,200);const hash=tokenHash(native.body.sessionToken);
  const pending=confirmation(await f.call('/console/api/login',{method:'POST',data:{username:'alice',password}}));
  assert.ok(f.store.authenticate(hash));
  const cancel=await f.call('/console/api/login/confirm',{method:'POST',data:{...pending,confirm:false}});
  assert.equal(cancel.status,400);assert.equal(cancel.headers['set-cookie'],undefined);assert.ok(f.store.authenticate(hash));
  const accepted=await f.call('/console/api/login/confirm',{method:'POST',data:pending});
  assert.equal(accepted.status,200);assert.deepEqual(Object.keys(accepted.body).sort(),['csrfToken','user']);
  assert.match(accepted.headers['set-cookie'][0],/; Path=\/console; HttpOnly; Secure; SameSite=Strict$/);
  assert.equal(/sessionToken|expiresAt|sessionAccess|tokenHash/.test(accepted.text),false);
  assert.equal(Boolean(f.store.authenticate(hash)),false);
  const cookie=accepted.headers['set-cookie'][0].split(';')[0];assert.equal((await f.call('/console/api/session',{cookie})).status,200);
  const replay=await f.call('/console/api/login/confirm',{method:'POST',data:pending});assert.equal(replay.status,409);assert.equal(replay.headers['set-cookie'],undefined);
});

test('browser and native confirmation challenges cannot cross issuer purposes or consume the correct flow',async t=>{
  const f=await fixture(t);await f.login();
  const native=confirmation(await f.call('/api/v1/auth/login',{method:'POST',data:{username:'alice',password}}));
  const crossed=await f.call('/console/api/login/confirm',{method:'POST',data:native});
  assert.equal(crossed.status,409);assert.equal(crossed.headers['set-cookie'],undefined);
  const accepted=await f.call('/api/v1/auth/login/confirm',{method:'POST',data:native});assert.equal(accepted.status,200);assert.equal(accepted.headers['set-cookie'],undefined);
  const browser=confirmation(await f.call('/console/api/login',{method:'POST',data:{username:'alice',password}}));
  const nativeCross=await f.call('/api/v1/auth/login/confirm',{method:'POST',data:browser});
  assert.equal(nativeCross.status,409);assert.equal(nativeCross.body.sessionToken,undefined);assert.ok(f.store.authenticate(tokenHash(accepted.body.sessionToken)));
  const browserAccepted=await f.call('/console/api/login/confirm',{method:'POST',data:browser});assert.equal(browserAccepted.status,200);
});

test('browser confirmation requires TLS and the exact Origin without spending its challenge',async t=>{
  const f=await fixture(t);const old=await f.login();
  const pending=confirmation(await f.call('/console/api/login',{method:'POST',data:{username:'alice',password}}));
  for(const extra of[{browserOrigin:null},{browserOrigin:'https://foreign.invalid'},{proxy:false}]){
    const result=await f.call('/console/api/login/confirm',{method:'POST',data:pending,...extra});assert.equal(result.status,403);assert.equal(result.headers['set-cookie'],undefined);
    assert.equal((await f.call('/console/api/session',old)).status,200);
  }
  assert.equal((await f.call('/console/api/login/confirm',{method:'POST',data:pending})).status,200);
});

test('expired browser confirmation cannot issue a cookie or retire the existing session',async t=>{
  let now=Date.now();const f=await fixture(t,{sessionAccessOptions:{now:()=>now}}),old=await f.login();
  const pending=confirmation(await f.call('/console/api/login',{method:'POST',data:{username:'alice',password}}));
  now+=120001;const result=await f.call('/console/api/login/confirm',{method:'POST',data:pending});
  assert.equal(result.status,410);assert.equal(result.body.error.code,'login_confirmation_expired');assert.equal(result.headers['set-cookie'],undefined);
  assert.equal((await f.call('/console/api/session',old)).status,200);
});

test('duplicate confirmation advertises remaining TTL from a fresh clock and never more than120000ms',async t=>{
  let now=Date.now();const f=await fixture(t,{sessionAccessOptions:{now:()=>++now}});
  const old=await f.login();
  const result=await f.call('/console/api/login',{method:'POST',data:{username:'alice',password}});
  assert.ok(result.body.error.details.expiresInMs<=120000,'observedRemainingTTL='+result.body.error.details.expiresInMs);
  const pending=confirmation(result);
  assert.equal((await f.call('/console/api/session',old)).status,200,'the metadata read never retires the old principal');
  assert.equal((await f.call('/console/api/login/confirm',{method:'POST',data:pending})).status,200);
});

test('browser confirmation revalidates account credentials and malformed bodies cannot issue authority',async t=>{
  const f=await fixture(t);await f.login();
  const pending=confirmation(await f.call('/console/api/login',{method:'POST',data:{username:'alice',password}}));
  for(const data of[null,[],{}, {...pending,extra:true},{...pending,challengeToken:'x'}]){
    const result=await f.call('/console/api/login/confirm',{method:'POST',data});assert.equal(result.status,400);assert.equal(result.headers['set-cookie'],undefined);
  }
  const sessions=f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all();
  f.store.db.prepare('UPDATE users SET password_hash=? WHERE username=?').run('synthetic-changed-password-fingerprint','alice');
  const result=await f.call('/console/api/login/confirm',{method:'POST',data:pending});
  assert.equal(result.status,409);assert.equal(result.body.error.code,'login_confirmation_changed');assert.equal(result.headers['set-cookie'],undefined);
  assert.deepEqual(f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all(),sessions);
});

test('assigned VM list and detail expose browser capability through resolved HTTPS',async t=>{
  const f=await fixture(t);
  enrollFixtureHosts(f.store,['pve-149','pve-150']);
  const login=await f.call('/api/v1/auth/login',{method:'POST',data:{username:'alice',password}});
  assert.equal(login.status,200);
  const headers={Authorization:'Bearer '+login.body.sessionToken};
  const list=await f.call('/api/v1/desktops',{headers}),detail=await f.call('/api/v1/desktops/pve-149',{headers});
  assert.equal(list.status,200);assert.equal(detail.status,200);
  for(const row of [...list.body.desktops,detail.body.desktop])assert.equal(row.consoleBrowserProtocol,'console-browser-v1');
  assert.equal((await f.call('/api/v1/desktops/pve-149')).status,401);
});

test('console writes require exact Origin and session-bound CSRF; HTTP/proxy spoofing cannot mint cookies', async t => {
  const f=await fixture(t), auth=await f.login();
  for(const [overrides,status] of [[{browserOrigin:null},403],[{browserOrigin:'https://foreign.invalid'},403],[{csrf:undefined},403],[{csrf:'x'.repeat(43)},403],[{proxy:false},403]]) {
    const result=await f.connect({...auth,...overrides}); assert.equal(result.status,status); assert.equal(result.headers['set-cookie'],undefined);
  }
  assert.equal((await f.call('/console/api/login',{method:'POST',data:{username:'alice',password},browserOrigin:null})).status,403);
  assert.equal((await f.call('/console/api/login',{method:'POST',data:{username:'alice',password},proxy:false})).status,403);
  assert.equal((await f.call('/console/api/session?token=forbidden',auth)).status,400);
  const other=await f.login('bobby'); assert.equal((await f.connect({...auth,csrf:other.csrf})).status,403);
  assert.equal((await f.connect(auth)).status,200);
});

test('native and browser password attempts share the same existing account budget', async t => {
  const f=await fixture(t,{loginLimit:1});
  assert.equal((await f.call('/api/v1/auth/login',{method:'POST',data:{username:'alice',password:'incorrect-fixture'}})).status,401);
  const blocked=await f.call('/console/api/login',{method:'POST',data:{username:'alice',password}});
  assert.equal(blocked.status,429); assert.equal(blocked.body.error.code,'too_many_attempts'); assert.equal(f.store.db.prepare('SELECT count(*) n FROM sessions').get().n,0);
});

test('스트리밍 끊고 연결 ends the streams on the assigned VM and then connects', async t => {
  // relay.takeover authenticates an app token; through it the console
  // session would get desktop_not_found ('이 VM에 연결할 수 없어요').
  const f=await fixture(t), auth=await f.login(), relay=f.server.relay, path='/console/api/desktops/pve-149/connect';
  const alice=f.store.findUser('alice'), bobby=f.store.findUser('bobby');
  // Stand-in streams carry real app sessions, so the relay's own authorization
  // sweep keeps them (and never stops a half-built session object).
  const appHash=user=>{const value=tokenHash(newToken());f.store.createSession(user,value,Date.now(),3600000,'client');return value;};
  if(!f.store.assignedDesktop(bobby.id,'pve-149'))f.store.assign('bobby','pve-149');
  // Another assigned account's stream ends too.
  relay.sessions.set('other-stream',{id:'other-stream',desktopId:'pve-149',userId:bobby.id,tokenHash:appHash(bobby),expiresAt:Date.now()+60000});
  let r=await f.call(path,{...auth,method:'POST',data:{takeover:true}});
  assert.equal(r.status,202);assert.equal(relay.sessions.has('other-stream'),false);
  relay.sessions.set('own-stream',{id:'own-stream',desktopId:'pve-149',userId:alice.id,tokenHash:appHash(alice),expiresAt:Date.now()+60000});
  assert.equal((await f.connect(auth)).body.error.code,'console_busy');
  r=await f.call(path,{...auth,method:'POST',data:{takeover:true}});
  assert.equal(r.status,202);assert.equal(r.body.code,'console_cleanup_pending');assert.equal(relay.sessions.has('own-stream'),false);
  assert.ok(f.store.db.prepare("SELECT subject FROM audit_events WHERE event='session_taken_over'").all().length>=1);
  assert.equal((await f.connect(auth)).status,200);
});
test('Console 끊고 연결: an assigned app session ends the VM console', async t => {
  // Otherwise a connected console shows the stream tile as '가상 모니터를
  // 구성하고 있어요' with no way in; the list flags it and offers release.
  const f=await fixture(t), auth=await f.login(), alice=f.store.findUser('alice');
  const token=newToken();f.store.createSession(alice,tokenHash(token),Date.now(),3600000,'client');
  const app=(path,method='GET')=>f.call('/api/v1'+path,{method,browserOrigin:null,headers:{Authorization:'Bearer '+token}});
  assert.equal((await f.connect(auth)).status,200);
  assert.equal(f.server.relay.console.blocked('pve-149'),true,'the list flags consoleInUse from this');
  const released=await app('/desktops/pve-149/console/release','POST');assert.equal(released.status,200);assert.equal(released.body.released,1);
  assert.equal(f.store.db.prepare("SELECT COUNT(*) n FROM audit_events WHERE event='console_taken_over'").get().n,1);
  await new Promise(resolve=>setTimeout(resolve,50));assert.equal(f.server.relay.console.blocked('pve-149'),false);
  assert.equal((await app('/desktops/pve-999/console/release','POST')).status,404,'only an assigned VM');
});
test('browser grants remain assigned VM only, hide private tickets and scope both cancellation IDs', async t => {
  const f=await fixture(t), auth=await f.login(), admin=await f.login('admin'), bobby=await f.login('bobby');
  assert.equal((await f.connect(admin)).status,404,'administrator does not bypass assignment');
  assert.equal((await f.call('/console/api/desktops/pve-149/connect',{...auth,method:'POST',data:{ticket:'invented'}})).status,400);
  // takeover is true or absent; with no stream running it is an ordinary connect.
  assert.equal((await f.call('/console/api/desktops/pve-149/connect',{...auth,method:'POST',data:{takeover:'yes'}})).status,400);
  const result=await f.call('/console/api/desktops/pve-149/connect',{...auth,method:'POST',data:{takeover:true}}); assert.equal(result.status,200);
  assert.deepEqual(Object.keys(result.body).sort(),['clipboardPolicy','desktop','expiresAt','id','qemuConsolePreparation','wsPath']);
  assert.deepEqual(result.body.qemuConsolePreparation,{status:'unknown'});
  assert.deepEqual(result.body.clipboardPolicy,{textToRemote:false,textToLocal:false}); assert.deepEqual(result.body.desktop,{id:'pve-149',name:'Memory VM 149'});
  assert.equal(result.body.wsPath,'/console/ws/'+result.body.id); assert.equal(/PRIVATE_|ticket|password|tokenHash|relayPath/.test(result.text),false);
  const grant=f.server.relay.console.grants.get(result.body.id); assert.equal(grant.profile,'novnc'); assert.match(grant.ticketHash,/^[a-f0-9]{64}$/);
  for(const [who,id] of [[bobby,'pve-149'],[auth,'pve-150']]) assert.equal((await f.call(`/console/api/desktops/${id}/sessions/${result.body.id}`,{...who,method:'DELETE'})).status,404);
  assert.equal(f.server.relay.console.grants.has(result.body.id),true);
  assert.equal((await f.call(`/console/api/desktops/pve-149/sessions/${result.body.id}`,{...auth,method:'DELETE'})).status,204);
  assert.equal(f.server.relay.console.grants.has(result.body.id),false);
});

test('Read-only console preparation status requires the exact browser principal, VM, origin and live browser grant', async t => {
  const f=await fixture(t),other=await f.login();
  const prompt=confirmation(await f.call('/console/api/login',{method:'POST',data:{username:'alice',password}}));
  const auth=consoleAuth(await f.call('/console/api/login/confirm',{method:'POST',data:prompt})),bobby=await f.login('bobby'),admin=await f.login('admin');
  const issued=await f.connect(auth),id=issued.body.id,path=`/console/api/desktops/pve-149/sessions/${id}`;
  f.store.addUser({name:'native-only',passwordHash:await hashed()});
  const native=await f.call('/api/v1/auth/login',{method:'POST',data:{username:'native-only',password}});assert.equal(native.status,200);
  const sessions=f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all(),audit=f.store.auditLog();
  f.proxmox.consoleSnapshot=async()=>assert.fail('a preparation status read cannot query PVE');
  const result=await f.call(path,{cookie:auth.cookie});
  assert.equal(result.status,200);assert.deepEqual(result.body,{qemuConsolePreparation:{status:'unknown'}});
  assert.equal(result.headers['cache-control'],'no-store');assert.equal(result.headers['set-cookie'],undefined);
  assert.equal(/ticket|nonce|operationId|tokenHash|password|leaseMs/.test(result.text),false);
  assert.equal((await f.call(path,{cookie:auth.cookie,browserOrigin:null})).status,200,'ordinary same-origin GET needs no write CSRF');
  for(const who of [bobby,admin])assert.equal((await f.call(path,who)).status,404);
  assert.equal((await f.call(path,other)).status,401,'the explicitly replaced same-account cookie cannot authorize any read');
  assert.equal((await f.call(path)).status,401);
  assert.equal((await f.call(path,{headers:{Authorization:'Bearer '+native.body.sessionToken}})).status,401);
  assert.equal((await f.call(path,{...auth,browserOrigin:'https://foreign.invalid'})).status,403);
  assert.equal((await f.call(path,{...auth,proxy:false})).status,403);
  assert.equal((await f.call(path.replace('pve-149','pve-150'),auth)).status,404);
  assert.equal((await f.call(path.replace(id,'11111111-1111-1111-1111-111111111111'),auth)).status,404);
  assert.equal((await f.call(path+'?ticket=forbidden',auth)).status,400);
  assert.equal((await f.call(path,{...auth,data:{}})).status,400);
  const grant=f.server.relay.console.grants.get(id);
  grant.origin='https://foreign.invalid';assert.equal((await f.call(path,auth)).status,404);grant.origin=origin;
  grant.profile='native';assert.equal((await f.call(path,auth)).status,404);grant.profile='novnc';
  assert.deepEqual(f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all(),sessions,'status never renews browser or native logins');
  assert.deepEqual(f.store.auditLog(),audit);assert.equal(f.upstream.length,0);
  f.clock.time+=30001;assert.equal((await f.call(path,auth)).status,404);
  assert.equal(f.server.relay.console.grants.get(id),grant,'an expired read does not retire or mutate the grant');
});

test('Console preparation status observes v2 lease freshness and Host loss without commands, renewal or PVE calls', async t => {
  const f=await fixture(t),auth=await f.login(),relay=f.server.relay,enrollment=newToken(),secret=newToken();
  f.store.issueEnrollment('pve-149',machine(149),tokenHash(enrollment),'fixture');
  f.store.consumeEnrollment(tokenHash(enrollment),machine(149),{hostname:'MemoryHost',version:'9.0.0',tokenHash:tokenHash(secret)});
  const wire=new Wire();relay.attachHost(wire,f.store.authenticateHost(tokenHash(secret)),tokenHash(secret),true);
  wire.packet({type:'host.capabilities',capabilities:{service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,qemuConsoleProtocol:'qemu-console-v2'}});
  let snapshots=0;f.proxmox.consoleSnapshot=async()=>{snapshots++;return{node:'fixture',vmid:149,machineUuid:machine(149),model:'std'};};
  const pending=f.connect(auth);let request;
  await until(()=>{request=wire.sent.map(bytes=>JSON.parse(bytes)).find(value=>value.type==='host.qemu-console.prepare');return !!request;});
  wire.packet({type:'host.qemu-console.result',protocol:'qemu-console-v2',operationId:request.operationId,nonce:request.nonce,ok:true,attempted:false,workerQuiescent:true,reason:'qemu_console_active'});
  const issued=await pending;assert.equal(issued.body.qemuConsolePreparation.status,'isolated');
  const grant=relay.console.grants.get(issued.body.id),op=grant.qemuOperation,path=`/console/api/desktops/pve-149/sessions/${grant.id}`;
  const sessions=f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all(),packets=wire.sent.length;
  assert.deepEqual((await f.call(path,auth)).body,{qemuConsolePreparation:{status:'isolated'}});
  f.clock.time+=10000;relay.qemuConsole.revalidate();const afterRenew=wire.sent.length;
  f.clock.time+=5000;
  assert.deepEqual((await f.call(path,auth)).body,{qemuConsolePreparation:{status:'unknown'}});
  assert.equal(op.status,'held');assert.equal(wire.sent.length,afterRenew,'read is not a status command or renew');
  assert.equal(snapshots,2);assert.equal(f.upstream.length,0);assert.ok(afterRenew>packets);
  assert.deepEqual(f.store.db.prepare('SELECT * FROM sessions ORDER BY token_hash').all(),sessions);
  relay.dropHost(relay.hosts.get('pve-149'),'host_disconnected');
  assert.deepEqual((await f.call(path,auth)).body,{qemuConsolePreparation:{status:'unknown'}});
  assert.equal(grant.closed,false,'Host loss does not end an authorized PVE emergency grant');
  assert.equal(snapshots,2);assert.equal(f.upstream.length,0);
});

// vm-config-sweep item 5: signed out or locked, with a PCI device in Proxmox
// (a GPU or vGPU keeps its own display through the Host's park): the grant
// tells the page the sign-in screen may be on a display the Console cannot show.
test('Console grant carries the sign-in hint only for a parked console on a VM with a passed-through device', async t => {
  const f=await fixture(t),auth=await f.login(),relay=f.server.relay,enrollment=newToken(),secret=newToken();
  f.store.issueEnrollment('pve-149',machine(149),tokenHash(enrollment),'fixture');
  f.store.consumeEnrollment(tokenHash(enrollment),machine(149),{hostname:'MemoryHost',version:'9.0.0',tokenHash:tokenHash(secret)});
  const wire=new Wire();relay.attachHost(wire,f.store.authenticateHost(tokenHash(secret)),tokenHash(secret),true);
  let passthrough=true;f.proxmox.consoleSnapshot=async()=>({node:'fixture',vmid:149,machineUuid:machine(149),model:'std',...(passthrough?{passthrough:true}:{})});
  const open=async interactiveSession=>{
    wire.packet({type:'host.capabilities',capabilities:{service:true,interactiveSession,streaming:interactiveSession,virtualDisplayReady:true,qemuConsoleProtocol:'qemu-console-v2'}});
    const before=wire.sent.length,pending=f.connect(auth);let request;
    await until(()=>{request=wire.sent.slice(before).map(bytes=>JSON.parse(bytes)).find(value=>value.type==='host.qemu-console.prepare');return !!request;});
    // The park answers with the protocol's existing words (Host qemu_console_route.hpp).
    wire.packet({type:'host.qemu-console.result',protocol:'qemu-console-v2',operationId:request.operationId,nonce:request.nonce,ok:true,attempted:true,workerQuiescent:true,reason:'qemu_console_restored'});
    const issued=await pending;assert.equal(issued.status,200);
    relay.console.retire(relay.console.grants.get(issued.body.id),'console_cancelled');
    for(const operation of [...relay.qemuConsole.operations.values()])relay.qemuConsole.finish(operation);
    return issued.body;
  };
  const parked=await open(false);
  assert.equal(parked.consoleHint,'signin_other_display');
  const audit=JSON.parse(f.store.db.prepare("SELECT subject FROM audit_events WHERE event='console_granted' ORDER BY id DESC LIMIT 1").get().subject);
  assert.equal(audit.hint,'signin_other_display','the administrator can see it was said');
  assert.equal(Object.hasOwn(await open(true),'consoleHint'),false,'signed in and unlocked: the worker turns the other display off');
  passthrough=false;
  assert.equal(Object.hasOwn(await open(false),'consoleHint'),false,'no PCI device in Proxmox');
});

test('Preparation status respects revoked VM, clipboard policy and parent session authority without cleanup side effects', async t => {
  for(const revocation of ['assignment','clipboard','session']){
    const f=await fixture(t),auth=await f.login();
    if(revocation==='clipboard')f.store.updateAssignmentPolicy('alice','pve-149',{clipboardPolicy:{textToRemote:true,textToLocal:false,filesToRemote:false,filesToLocal:false}});
    const issued=await f.connect(auth),grant=f.server.relay.console.grants.get(issued.body.id),path=`/console/api/desktops/pve-149/sessions/${grant.id}`;
    if(revocation==='assignment')f.store.revoke('alice','pve-149');
    if(revocation==='clipboard')f.store.updateAssignmentPolicy('alice','pve-149',{clipboardPolicy:{textToRemote:false,textToLocal:false,filesToRemote:false,filesToLocal:false}});
    if(revocation==='session')f.store.logout(grant.tokenHash);
    assert.equal((await f.call(path,auth)).status,revocation==='session'?401:revocation==='clipboard'?403:404);
    assert.equal(grant.closed,false);assert.equal(f.upstream.length,0);
  }
});

test('native console grant shape stays exact and cannot be upgraded through the browser profile', async t => {
  const f=await fixture(t),native=await f.call('/api/v1/auth/login',{method:'POST',data:{username:'alice',password}}),token=native.body.sessionToken;
  const result=await f.call('/api/v1/desktops/pve-149/console-sessions',{method:'POST',data:{},headers:{Authorization:'Bearer '+token}});
  assert.equal(result.status,201);assert.deepEqual(Object.keys(result.body.consoleSession).sort(),['expiresAt','id','relayPath','ticket']);
  const cookie=consoleCookieName(origin)+'='+token;
  assert.equal(await f.rejectedSocket(result.body.consoleSession.id,{cookie}),401);
  assert.equal(f.server.relay.console.grants.get(result.body.consoleSession.id).profile,'native');
  assert.equal((await f.call('/api/v1/desktops/pve-149/console-sessions/'+result.body.consoleSession.id,{method:'DELETE',headers:{Authorization:'Bearer '+token}})).status,200);
});

test('only a real retained stream stop barrier yields pending cleanup; active console contention remains terminal',async t=>{
  const f=await fixture(t),auth=await f.login(),secret=newToken(),enrollment=newToken();
  f.store.issueEnrollment('pve-149',machine(149),tokenHash(enrollment),'fixture');
  f.store.consumeEnrollment(tokenHash(enrollment),machine(149),{hostname:'MemoryHost',version:'9.0.0',tokenHash:tokenHash(secret)});
  const relay=f.server.relay;relay.attachHost(new Wire(),f.store.authenticateHost(tokenHash(secret)),tokenHash(secret),true);
  const host=relay.hosts.get('pve-149'),record=relay.sessionStop.begin({id:'11111111-1111-1111-1111-111111111111',desktopId:'pve-149',host,mode:'stream',hostDispatched:true,stopProtocol:'stop-v1'});
  assert.ok(record);const pending=await f.connect(auth);assert.equal(pending.status,202);assert.deepEqual(pending.body,{code:'console_cleanup_pending'});assert.equal(relay.sessionStop.pending.get('pve-149'),record);assert.equal(f.upstream.length,0);
  relay.sessionStop.pending.delete('pve-149');const owner=await f.login('bobby');const grant=await f.connect(owner);assert.equal(grant.status,200);
  const busy=await f.connect(auth);assert.equal(busy.status,409);assert.equal(busy.body.error.code,'console_busy');assert.equal(relay.console.grants.has(grant.body.id),true);
});

test('body and upstream awaits revalidate assignment, and cancellation retains real pending capacity', async t => {
  const f=await fixture(t),auth=await f.login(); let release;
  f.proxmox.consoleSnapshot=()=>new Promise(resolve=>{release=resolve;});
  const pending=f.connect(auth); await until(()=>typeof release==='function');
  assert.equal(f.server.relay.console.grants.size,1); f.store.revoke('alice','pve-149');
  release({node:'fixture',vmid:149,machineUuid:machine(149)}); const refused=await pending;
  assert.equal(refused.status,409); assert.equal(refused.body.error.code,'authorization_revoked'); assert.equal(f.server.relay.console.grants.size,0); assert.equal(f.upstream.length,0);
  f.store.assign('alice','pve-149'); release=null;
  const req=request({hostname:'127.0.0.1',port:f.port,path:'/console/api/desktops/pve-149/connect',method:'POST',headers:{Host:new URL(origin).host,'X-Forwarded-Proto':'https',Origin:origin,Cookie:auth.cookie,'X-Multitap-Console-CSRF':auth.csrf,'Content-Type':'application/json'} });
  req.on('error',()=>{});req.end('{}');await until(()=>typeof release==='function');req.destroy();await pause();await pause();
  assert.equal(f.server.relay.console.grants.size,1,'aborted PVE work still owns admission');
  release({node:'fixture',vmid:149,machineUuid:machine(149)});await until(()=>f.server.relay.console.grants.size===0);
});

test('a request paused while reading its body loses VM authority before creation', async t => {
  const f=await fixture(t),auth=await f.login();let incoming,req;
  const observed=value=>{if(value.url==='/console/api/desktops/pve-149/connect')incoming=value;};f.server.on('request',observed);
  const result=new Promise((resolve,reject)=>{
    req=request({hostname:'127.0.0.1',port:f.port,path:'/console/api/desktops/pve-149/connect',method:'POST',headers:{Host:new URL(origin).host,'X-Forwarded-Proto':'https',Origin:origin,Cookie:auth.cookie,'X-Multitap-Console-CSRF':auth.csrf,'Content-Type':'application/json'}},res=>{res.resume();res.once('end',()=>resolve(res.statusCode));});req.on('error',reject);req.flushHeaders();
  });
  await until(()=>incoming?.listenerCount('data')>0);const read=once(incoming,'data');req.write('{');await read;f.store.revoke('alice','pve-149');req.end('}');
  assert.equal(await result,404);assert.equal(f.server.relay.console.grants.size,0);assert.equal(f.upstream.length,0);f.server.off('request',observed);
});

test('browser WebSocket requires cookie plus Origin, is single-use and carries RFB without private JSON preamble', async t => {
  const f=await fixture(t),auth=await f.login(),other=await f.login('bobby'),result=await f.connect(auth),id=result.body.id;
  assert.equal(await f.rejectedSocket(id,{}),401);assert.equal(await f.rejectedSocket(id,other),401);
  assert.equal(await f.rejectedSocket(id,auth,{Origin:'https://foreign.invalid'}),403);
  assert.equal(await f.rejectedSocket(id+'?ticket=forbidden',auth),400);
  const ws=f.socket(id,auth),frames=[];ws.on('message',(data,binary)=>frames.push({data:Buffer.from(data),binary}));await once(ws,'open');
  t.after(()=>ws.terminate());await until(()=>f.upstream.length===1&&f.server.relay.console.grants.get(id)?.rfb);
  assert.equal(f.server.relay.console.grants.get(id).ticketHash,null);
  assert.equal(await f.rejectedSocket(id,auth),401);assert.equal(ws.readyState,1,'replay cannot retire original owner');
  const init=Buffer.alloc(24);init.writeUInt16BE(2,0);init.writeUInt16BE(2,2);Buffer.from([32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0]).copy(init,4);
  f.upstream[0].emit('message',Buffer.concat([Buffer.from('RFB 003.008\n'),Buffer.from([1,1]),Buffer.alloc(4),init]),true);
  await until(()=>frames.length>0);assert.equal(frames[0].data.toString(),'RFB 003.008\n');assert.equal(frames.every(frame=>frame.binary),true);
  ws.send(Buffer.concat([Buffer.from('RFB 003.008\n'),Buffer.from([1,1])]));await until(()=>f.server.relay.console.grants.get(id)?.state==='active');
  assert.equal(Buffer.concat(frames.map(frame=>frame.data)).includes(Buffer.from('console-ready')),false);
  const closed=once(ws,'close');assert.equal((await f.call('/console/api/logout',{...auth,method:'POST',data:{}})).status,204);await closed;
  assert.equal(f.upstream[0].readyState,3);assert.equal((await f.call('/console/api/session',auth)).status,401);
});

test('expired grants and actual session expiry fail closed before dialing upstream', async t => {
  const f=await fixture(t),auth=await f.login(),grant=await f.connect(auth);f.clock.time+=30001;
  assert.equal(await f.rejectedSocket(grant.body.id,auth),401);assert.equal(f.upstream.length,0);
  f.store.db.prepare('UPDATE sessions SET expires_at=?').run(Date.now()-1);
  assert.equal((await f.call('/console/api/session',auth)).status,401);assert.equal(await f.rejectedSocket(grant.body.id,auth),401);
});

test('explicit session deletion clears only the console cookie and revokes outstanding grants', async t => {
  const f=await fixture(t),auth=await f.login(),result=await f.connect(auth);
  const deleted=await f.call('/console/api/session',{...auth,method:'DELETE'});assert.equal(deleted.status,204);assert.equal(deleted.text,'');
  assert.match(deleted.headers['set-cookie'][0],/Path=\/console; HttpOnly; Secure; SameSite=Strict; Max-Age=0$/);
  assert.equal(f.server.relay.console.grants.has(result.body.id),false);assert.equal(await f.rejectedSocket(result.body.id,auth),401);
});

test('only manifest-verified local assets are served, with query/traversal and partial packages rejected', async t => {
  const f=await fixture(t);
  const shell=await f.call('/console/pve-149',{browserOrigin:null});assert.equal(shell.status,200);assert.match(shell.text,/Fixture viewer/);
  for(const id of ['api','assets','ws','lab_149','Office-PC'])assert.equal((await f.call('/console/'+id,{browserOrigin:null})).status,200,'valid one-segment desktop ID '+id);
  assert.equal((await f.call('/console/api/unknown',{browserOrigin:null})).status,404,'generic shell does not admit unknown API paths');
  assert.equal(shell.headers['referrer-policy'],'no-referrer');assert.match(shell.headers['content-security-policy'],/frame-ancestors 'none'/);assert.match(shell.headers['content-security-policy'],/img-src 'self' data: blob:/);
  assert.equal((await f.call('/console/assets/console.js',{browserOrigin:null})).status,200);
  await writeFile(join(f.directory,'vault.key'),'SYNTHETIC_NOT_PUBLIC');assert.equal((await f.call('/console/assets/vault.key')).status,404);
  for(const path of ['/console/assets/assets-manifest.json','/console/assets/missing.js','/console/assets/../index.html','/console/assets/%2e%2e/index.html','/console/assets/console.js?token=x']) {
    const result=await f.call(path);assert.ok([400,404].includes(result.status),path);assert.equal(result.headers['cache-control'],'no-store');
  }
  const broken=await fixture(t);await writeFile(join(broken.directory,'console.js'),'tampered-before-first-request');
  assert.equal((await broken.call('/console/pve-149')).status,503);assert.equal((await broken.call('/healthz')).status,200,'static damage cannot take down ordinary broker API');
});

test('every shipped browser manifest asset and relative module import uses the verified standalone route',async t=>{
  const f=await fixture(t,{browserConsoleOptions:{}}),manifest=JSON.parse(await readFile(new URL('../admin/console/assets-manifest.json',import.meta.url),'utf8'));
  assert.equal(manifest.schema,1);assert.equal(manifest.product,'multitap-browser-console');assert.ok(manifest.files.length>60);
  for(const entry of manifest.files){
    const path='/console/assets/'+entry.path,result=await f.call(path,{browserOrigin:null});assert.equal(result.status,200,path);
    assert.equal(result.bytes.length,entry.bytes,path);assert.equal(createHash('sha256').update(result.bytes).digest('hex'),entry.sha256,path);
    assert.equal(result.headers['content-security-policy'],consolePagePolicy,path);assert.equal(result.headers['referrer-policy'],'no-referrer',path);
    if(!entry.path.endsWith('.js'))continue;
    assert.equal(result.headers['content-type'],'text/javascript; charset=utf-8');
    for(const[,target]of result.text.matchAll(/^\s*import\s[^'"]*['"]([^'"]+)['"]/gm)){
      const shared=['/admin/js/account-security-controller.js','/admin/js/account-security-view.js'];
      assert.ok(/^\.\.?\//.test(target)||shared.includes(target),entry.path+' -> '+target);
      const resolved=new URL(target,origin+path).pathname,loaded=await f.call(resolved,{browserOrigin:null});
      assert.equal(loaded.status,200,entry.path+' -> '+resolved);
      if(shared.includes(target)){
        assert.equal(loaded.headers['content-type'],'text/javascript; charset=utf-8');
        assert.deepEqual(loaded.bytes,await readFile(new URL('..'+target,import.meta.url)));
      }
    }
  }
  for(const path of ['/console/assets/assets-manifest.json','/console/assets/broker.sqlite','/console/assets/vault.key','/admin/console/console.js'])assert.equal((await f.call(path,{browserOrigin:null})).status,404,path);
});

test('shipped notice and approved CSS serve verified bytes while a tampered CSS disables only isolated viewer assets',async t=>{
  const shipped=await fixture(t,{browserConsoleOptions:{}}),manifest=JSON.parse(await readFile(new URL('../admin/console/assets-manifest.json',import.meta.url),'utf8'));
  for(const path of ['THIRD-PARTY-NOTICES.txt','console.css']){
    const expected=manifest.files.find(entry=>entry.path===path),reply=await shipped.call('/console/assets/'+path,{browserOrigin:null});
    assert.equal(reply.status,200,path);assert.equal(reply.bytes.length,expected.bytes,path);assert.equal(createHash('sha256').update(reply.bytes).digest('hex'),expected.sha256,path);
  }
  const broken=await fixture(t);await assets(broken.directory,{'THIRD-PARTY-NOTICES.txt':'Synthetic isolated notice only.','console.css':':root{color:red}'});
  await writeFile(join(broken.directory,'console.css'),':root{color:tan}');
  assert.equal((await broken.call('/console/assets/THIRD-PARTY-NOTICES.txt',{browserOrigin:null})).status,503);
  assert.equal((await broken.call('/console/assets/console.css',{browserOrigin:null})).status,503);
  assert.equal((await broken.call('/healthz',{browserOrigin:null})).status,200);
});

test('legacy unresolved HTTP context cannot claim browser HTTPS from a public-origin label', async t => {
  const f=await fixture(t,{accessConfig:undefined});
  const result=await f.call('/console/api/login',{method:'POST',data:{username:'alice',password}});
  assert.equal(result.status,403);assert.equal(result.body.error.code,'console_https_required');assert.equal(result.headers['set-cookie'],undefined);
  assert.equal((await f.call('/api/v1/auth/login',{method:'POST',data:{username:'alice',password}})).status,200,'native legacy behavior is retained');
});

test('a browser that opens the Console over plain HTTP gets a page that says so; the API and everything else keep the JSON refusal', async t => {
  const f=await fixture(t,{accessConfig:undefined}),html={Accept:'text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8','Accept-Language':'ko-KR,ko;q=0.9,en;q=0.8'};
  for(const path of ['/console','/console/','/console/pve-149']){
    const page=await f.call(path,{browserOrigin:null,headers:html});
    assert.equal(page.status,403,path);assert.match(page.headers['content-type'],/^text\/html; charset=utf-8$/,path);
    assert.match(page.text,/HTTPS 주소로 열어 주세요/);assert.match(page.text,/https:\/\/로 시작하는 주소/);
    assert.doesNotMatch(page.text,/console_https_required|\{"error"/,'no API text on the page');
    assert.doesNotMatch(page.text,/<script|<img|<link|src=|href=|url\(/i,'the page loads nothing');
    assert.equal(page.headers['content-security-policy'],"default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    assert.equal(page.headers['cache-control'],'no-store');assert.equal(page.headers['x-content-type-options'],'nosniff');assert.equal(page.headers['referrer-policy'],'no-referrer');
    assert.equal(page.headers['set-cookie'],undefined);
  }
  // The same page in English for a browser that does not ask for Korean first (docs/i18n.md).
  for(const language of ['en-US,en;q=0.9,ko;q=0.5',undefined]){
    const page=await f.call('/console',{browserOrigin:null,headers:{Accept:html.Accept,...(language?{'Accept-Language':language}:{})}});
    assert.equal(page.status,403);assert.match(page.text,/<html lang="en">/);assert.match(page.text,/Open it with an HTTPS address/);assert.doesNotMatch(page.text,/[가-힯]/);
    assert.match(page.headers.vary,/Accept-Language/);
  }
  const head=await f.call('/console',{method:'HEAD',browserOrigin:null,headers:html});
  assert.equal(head.status,403);assert.equal(head.bytes.length,0);assert.match(head.headers['content-type'],/^text\/html/);
  // Not a page a person opened: the same JSON as before.
  for(const [path,options] of [
    ['/console',{browserOrigin:null}],
    ['/console',{browserOrigin:null,headers:{Accept:'application/json'}}],
    ['/console/api/session',{browserOrigin:null,headers:html}],
    ['/console/assets/console.js',{browserOrigin:null,headers:html}],
    ['/console/api/login',{method:'POST',data:{username:'alice',password},headers:html}],
  ]){
    const refused=await f.call(path,options);
    assert.equal(refused.status,403,path);assert.equal(refused.body.error.code,'console_https_required',path);
  }
});

test('console MFA challenge CSRF is preauth-only, audience-bound and never issues bearer JSON', async t => {
  const f=await fixture(t),controller=new BrowserConsole({store:f.store,vault:f.vault,relay:f.server.relay,sessionTtlMs:3600000,sessionMaxLifetimeMs:7200000});
  const challenge=newToken(),headers={},sent=[];const req={url:'/console/api/login',method:'POST',headers:{origin},brokerTls:'proxy'},res={setHeader:(key,value)=>{headers[key]=value;}};
  const send=(status,value)=>sent.push({status,value});
  await controller.handle(req,res,req.url,{origin,body:async()=>({username:'alice',password}),send,login:async(_input,admin,purpose)=>{assert.equal(admin,false);assert.equal(purpose,'console');return {mfa:{protocol:'mfa-v1',challengeToken:challenge,expiresAt:new Date(Date.now()+120000).toISOString()}};}});
  assert.equal(sent[0].status,202);assert.equal(headers['Set-Cookie'],undefined);assert.equal(JSON.stringify(sent).includes('sessionToken'),false);
  const csrf=sent[0].value.csrfToken;assert.equal(f.vault.verifyCsrf('console-v1:'+origin+':'+challenge,csrf),false);
  req.url='/console/api/mfa/verify';req.headers['x-multitap-console-csrf']=csrf;
  let verified=0;const mfaRoutes={guard(){},verify:(_input,admin,purpose)=>{++verified;assert.equal(admin,false);assert.equal(purpose,'console');const token=newToken(),user=f.store.findUser('alice');f.store.createSession(user,tokenHash(token),Date.now(),3600000);return {token,user};}};
  await controller.handle(req,res,req.url,{origin,body:async()=>({challengeToken:challenge,code:'123456'}),send,mfaRoutes});
  assert.equal(verified,1);assert.match(headers['Set-Cookie'],/__Secure-multitap-console-/);assert.deepEqual(Object.keys(sent.at(-1).value).sort(),['csrfToken','user']);
  req.headers['x-multitap-console-csrf']=f.vault.csrf('console-mfa:https://foreign.invalid:'+challenge);
  await assert.rejects(controller.handle(req,res,req.url,{origin,body:async()=>({challengeToken:challenge,code:'123456'}),send,mfaRoutes}),e=>e.code==='csrf_required');assert.equal(verified,1);
  // The real provider is now installed. A controller-only synthetic challenge
  // is correctly unknown there, rather than indicating an absent provider.
  const expired=await f.call('/console/api/mfa/verify',{method:'POST',data:{challengeToken:challenge,code:'123456'},csrf});assert.equal(expired.status,410);assert.equal(expired.body.error.code,'mfa_challenge_expired');assert.equal(expired.headers['set-cookie'],undefined);
});

// Explicit new static closure; no wildcard admin alias is admitted.
test('new administrator MFA modules and stylesheet serve exact shipped bytes with correct MIME',async t=>{
  const f=await fixture(t);
  for(const [asset,mime]of [['js/mfa-auth.js','text/javascript'],['js/account-security-controller.js','text/javascript'],['js/account-security-view.js','text/javascript'],['account-security.css','text/css']]){
    const reply=await f.call('/admin/'+asset,{browserOrigin:null});
    assert.equal(reply.status,200,asset);assert.equal(reply.headers['content-type'],mime+'; charset=utf-8',asset);
    assert.deepEqual(reply.bytes,await readFile(new URL('../admin/'+asset,import.meta.url)),asset);
    assert.equal(reply.headers['x-content-type-options'],'nosniff',asset);
  }
  for(const asset of ['js/private-key.js','js/../mfa-auth.js','js/account-security-controller.js/extra'])assert.equal((await f.call('/admin/'+asset,{browserOrigin:null})).status,404,asset);
});

// ime-state-v1: every read asks
// the Host afresh; an older Host, no answer or a lost Host is unknown ({}).
test('Console IME reads ask the Host each time and give only its answer to the exact live browser grant', async t => {
  const f=await fixture(t),auth=await f.login(),bobby=await f.login('bobby'),relay=f.server.relay,enrollment=newToken(),secret=newToken();
  f.store.issueEnrollment('pve-149',machine(149),tokenHash(enrollment),'fixture');
  f.store.consumeEnrollment(tokenHash(enrollment),machine(149),{hostname:'MemoryHost',version:'9.0.0',tokenHash:tokenHash(secret)});
  const wire=new Wire();relay.attachHost(wire,f.store.authenticateHost(tokenHash(secret)),tokenHash(secret),true);
  const caps={service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true};
  wire.packet({type:'host.capabilities',capabilities:caps});
  const issued=await f.connect(auth),id=issued.body.id,path=`/console/api/desktops/pve-149/sessions/${id}/ime`;
  const queries=()=>wire.sent.map(bytes=>JSON.parse(bytes)).filter(value=>value.type==='host.ime.query');
  // An older Host (no imeStateProtocol) is never asked: unknown.
  assert.deepEqual((await f.call(path,auth)).body,{});assert.equal(queries().length,0);
  wire.packet({type:'host.capabilities',capabilities:{...caps,imeStateProtocol:'ime-state-v1'}});
  assert.equal(relay.hosts.get('pve-149').capabilities.imeStateProtocol,'ime-state-v1');
  const answer=async(fields,expected)=>{
    const before=queries().length,pending=f.call(path,auth);
    await until(()=>queries().length===before+1);const query=queries().at(-1);
    assert.deepEqual(Object.keys(query).sort(),['id','protocol','type']);assert.equal(query.protocol,'ime-state-v1');
    wire.packet({type:'host.ime.state',protocol:'ime-state-v1',id:query.id,...fields});
    const result=await pending;assert.equal(result.status,200);assert.deepEqual(result.body,expected);
    assert.equal(result.headers['cache-control'],'no-store');return query;
  };
  await answer({imeKorean:true},{imeKorean:true});
  await answer({imeKorean:false},{imeKorean:false});
  await answer({},{});
  // Nothing is kept: with no answer the next read is unknown, not the last state.
  await answer({imeKorean:true},{imeKorean:true});
  const before=queries().length;
  assert.deepEqual((await f.call(path,auth)).body,{},'a Host that does not answer is unknown');
  assert.equal(queries().length,before+1);
  // A late answer to that question is not a new state and does not drop the Host.
  wire.packet({type:'host.ime.state',protocol:'ime-state-v1',id:queries().at(-1).id,imeKorean:true});
  assert.ok(relay.hostConnected('pve-149'));
  // Only the exact browser principal, VM and grant may read it.
  for(const who of [bobby])assert.equal((await f.call(path,who)).status,404);
  assert.equal((await f.call(path)).status,401);
  assert.equal((await f.call(path.replace('pve-149','pve-150'),auth)).status,404);
  assert.equal((await f.call(path.replace(id,'11111111-1111-1111-1111-111111111111'),auth)).status,404);
  assert.equal((await f.call(path,{...auth,browserOrigin:'https://foreign.invalid'})).status,403);
  assert.equal((await f.call(path+'?fresh=1',auth)).status,400);
  assert.equal((await f.call(path,{...auth,method:'DELETE'})).status,405,'the IME path never cancels the grant');
  assert.equal(relay.console.grants.get(id).closed,false);
  assert.equal(queries().length,before+1,'refused reads never ask the Host');
  // A malformed answer drops the Host; a lost Host is unknown.
  const pending=f.call(path,auth);await until(()=>queries().length===before+2);
  wire.packet({type:'host.ime.state',protocol:'ime-state-v1',id:queries().at(-1).id,imeKorean:true,window:'Notepad'});
  assert.deepEqual((await pending).body,{});assert.equal(relay.hostConnected('pve-149'),false);
  assert.deepEqual((await f.call(path,auth)).body,{});
});

test('a Host IME answer needs its own question and its declared protocol', async () => {
  const { ImeState, imeStateReply, imeStateCapability } = await import('../src/ime-state.mjs');
  const id='0f8fad5b-d9cb-469f-a165-70867728950e';
  assert.deepEqual(imeStateReply({type:'host.ime.state',protocol:'ime-state-v1',id,imeKorean:false}),{id,imeKorean:false});
  assert.deepEqual(imeStateReply({type:'host.ime.state',protocol:'ime-state-v1',id}),{id,imeKorean:null});
  for(const bad of [{type:'host.ime.state',protocol:'ime-state-v2',id},{type:'host.ime.state',protocol:'ime-state-v1',id:'x'},{type:'host.ime.state',protocol:'ime-state-v1',id,imeKorean:'yes'},{type:'host.ime.state',protocol:'ime-state-v1',id,title:'x'},null,[]])assert.equal(imeStateReply(bad),null);
  assert.equal(imeStateCapability({imeStateProtocol:'ime-state-v1',service:true}),true);
  assert.equal(imeStateCapability({imeStateProtocol:'ime-state-v1',service:false}),false);
  const sent=[],host={ws:{},capabilities:{service:true,imeStateProtocol:'ime-state-v1'}};
  const relay={hosts:new Map([['pve-1',host]]),hostAuthenticated:value=>value===host,send:(_ws,value)=>{sent.push(value);return true;}};
  const state=new ImeState(relay,{timeoutMs:50});
  const first=state.query('pve-1'),second=state.query('pve-1');
  assert.equal(sent.length,1,'overlapping reads share one question');
  assert.equal(state.result(host,{type:'host.ime.state',protocol:'ime-state-v1',id:sent[0].id,imeKorean:true}),true);
  assert.deepEqual([await first,await second],[true,true]);
  assert.equal(await state.query('pve-2'),null,'no Host, no question');
  host.capabilities={service:true};assert.equal(await state.query('pve-1'),null);assert.equal(sent.length,1);
  assert.equal(state.result(host,{type:'host.ime.state',protocol:'ime-state-v1',id:sent[0].id,imeKorean:true}),false,'an undeclared answer is refused');
  host.capabilities={service:true,imeStateProtocol:'ime-state-v1'};
  const lost=state.query('pve-1');state.hostLost(host);assert.equal(await lost,null);
  assert.equal(await state.query('pve-1'),null,'an unanswered question times out as unknown');
});

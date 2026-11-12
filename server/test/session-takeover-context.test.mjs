import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword, tokenHash, newToken } from '../src/security.mjs';

const origin = 'https://takeover-fixture.example';
const password = 'Takeover-fixture-password-only!';
const machineUuid = '12345678-1234-1234-1234-123456789abc';

async function fixture(t, { aliceAdmin = false } = {}) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  const hash = await hashPassword(password);
  store.addUser({ name: 'admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', passwordHash: hash, role: aliceAdmin ? 'admin' : 'user' });
  store.addUser({ name: 'bobby', passwordHash: hash });
  store.syncProxmox([170, 149].map(vmid => ({ vmid, node: 'pve', name: `VM ${vmid}`, status: 'running' })), 'fixture');
  store.assign('alice', 'pve-170'); store.assign('bobby', 'pve-170');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve-fixture.example', tokenId: 'fixture@pve!broker', secret: 'only-fixture-secret' }), 'fixture');
  let clock = Date.now();
  const proxmox = { machineIdentity: async () => ({ machineUuid }) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, relayOptions: { authorizationMs: 20, heartbeatMs: 200, takeoverContextOptions: { now: () => clock } }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = `http://127.0.0.1:${server.address().port}`;
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  async function call(path, { method = 'GET', data, token, cookie, csrf, context, loginContext, intent } = {}) {
    const response = await fetch(base + '/api/v1' + path, { method,
      headers: { ...(context ? { 'X-Multitap-Takeover-Context': context } : {}), ...(loginContext ? { 'X-Multitap-Login-Context': loginContext } : {}), ...(intent ? { 'X-Multitap-Session-Policy': 'windows-pc-v1', 'X-Multitap-Login-Intent': intent } : {}), ...(token ? { Authorization: 'Bearer ' + token } : {}), ...(cookie ? { Cookie: cookie } : {}), ...(method !== 'GET' ? { 'Content-Type': 'application/json', Origin: origin, ...(csrf ? { 'X-CSRF-Token': csrf } : {}) } : {}) },
      body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, body: await response.json() };
  }
  const login = async name => (await call('/auth/login', { method: 'POST', data: { username: name, password } })).body.sessionToken;
  const adminLogin = await fetch(base + '/api/v1/admin/login', { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: origin }, body: JSON.stringify({ username: 'admin', password }) });
  const admin = { cookie: adminLogin.headers.get('set-cookie').split(';')[0], csrf: (await adminLogin.json()).csrfToken };
  const code = (await call('/admin/desktops/pve-170/enrollment', { ...admin, method: 'POST', data: {} })).body.code;
  const enrolled = await call('/hosts/enroll', { method: 'POST', data: { code, hostname: 'Fixture VM', machineUuid, version: '0.1.0' } });
  assert.equal(enrolled.status, 201);
  const wsUrl = path => base.replace('http:', 'ws:') + path;
  const host = await socket(wsUrl('/api/v1/hosts/channel'), 'Host ' + enrolled.body.hostToken);
  return { store, call, login, wsUrl, host, relay: server.relay, advance: ms => { clock += ms; } };
}

async function socket(url, authorization) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url, { headers: { Authorization: authorization }, perMessageDeflate: false });
    ws.once('error', reject);
    ws.once('unexpected-response', (_req, response) => { response.resume(); reject(Object.assign(new Error('upgrade_rejected'), { status: response.statusCode })); });
    ws.once('open', () => {
      const queue = [], waiters = [];
      ws.on('message', (data, binary) => {
        const value = binary ? data : JSON.parse(data.toString('utf8'));
        const index = waiters.findIndex(waiter => waiter.predicate(value));
        if (index >= 0) { const waiter = waiters.splice(index, 1)[0]; clearTimeout(waiter.timer); waiter.resolve(value); }
        else queue.push(value);
      });
      ws.take = (predicate = () => true) => {
        const index = queue.findIndex(predicate);
        if (index >= 0) return Promise.resolve(queue.splice(index, 1)[0]);
        return new Promise((resolve, reject) => {
          const waiter = { predicate, resolve, timer: setTimeout(() => reject(new Error('message_timeout')), 2000) }; waiters.push(waiter);
        });
      };
      resolve(ws);
    });
  });
}

async function attach(f, token) {
  const connection = await f.call('/desktops/pve-170/connect', { method: 'POST', token });
  assert.equal(connection.status, 200);
  const client = await socket(f.wsUrl('/api/v1/sessions/' + connection.body.sessionId), 'Bearer ' + token);
  assert.equal((await f.host.take(value => value.type === 'session.start')).sessionId, connection.body.sessionId);
  return { client, sessionId: connection.body.sessionId };
}

const endpoint='/desktops/pve-170';
async function offer(f, token) {
  const busy=await f.call(endpoint+'/connect',{method:'POST',token,context:'v1'});
  assert.equal(busy.status,409);assert.equal(busy.body.error.code,'desktop_busy');
  const value=busy.body.error.details.takeoverOffer;
  assert.ok(value,'opted-in eligible busy response carries an actual reviewed occupant');
  return value;
}
const accept=(f,token,context)=>f.call(endpoint+'/takeover',{method:'POST',token,context:'v1',data:{context}});
const assertLive=async(f,token,id)=>{assert.equal((await f.call('/auth/me',{token})).status,200);assert.ok(f.relay.sessions.has(id));};

test('legacy busy fields and empty takeover retain their exact existing contract',async t=>{
  const f=await fixture(t),token=await f.login('alice'),old=await attach(f,token);
  assert.deepEqual((await f.call(endpoint+'/connect',{method:'POST',token})).body.error.details,{reason:'controller_in_use',takeover:true});
  assert.equal((await f.call(endpoint+'/takeover',{method:'POST',token})).body.released,true);
  const ended=await old.client.take(v=>v.type==='session.ended');
  assert.equal(ended.reason,'taken_over');assert.equal(ended.detail,'same_account');assert.equal('by' in ended,false,'the own account is not named');
});

test('negotiated offer contains only actual bounded user and target metadata and is one-use',async t=>{
  const f=await fixture(t),token=await f.login('alice'),old=await attach(f,token),value=await offer(f,token);
  assert.deepEqual(Object.keys(value).sort(),['context','desktop','expiresInSeconds','protocol','sessions']);
  assert.equal(value.protocol,'takeover-context-v1');assert.match(value.context,/^[A-Za-z0-9_-]{43}$/);assert.equal(value.expiresInSeconds,120);
  assert.deepEqual(value.desktop,{id:'pve-170',name:'VM 170'});assert.equal(value.sessions.length,1);
  assert.deepEqual(Object.keys(value.sessions[0]).sort(),['createdAt','deviceLabel','username']);assert.equal(value.sessions[0].username,'alice');assert.equal(value.sessions[0].deviceLabel,null);
  assert.equal(value.sessions[0].createdAt,new Date(f.relay.sessions.get(old.sessionId).createdAt).toISOString());
  assert.ok(!JSON.stringify(value).includes(token));assert.ok(!JSON.stringify(value).includes(old.sessionId));assert.ok(!JSON.stringify(value).includes(tokenHash(token)));
  assert.equal((await accept(f,token,value.context)).status,200);
  assert.equal((await old.client.take(v=>v.type==='session.ended')).reason,'taken_over');
  assert.equal((await accept(f,token,value.context)).body.error.code,'takeover_context_stale');
});

test('a reviewed occupant replaced before confirmation never retires its successor',async t=>{
  const f=await fixture(t),token=await f.login('alice'),old=await attach(f,token),value=await offer(f,token);
  old.client.close();await f.host.take(v=>v.type==='session.stop');
  const successor=await attach(f,token),attempt=await accept(f,token,value.context);
  assert.equal(attempt.status,409);assert.equal(attempt.body.error.code,'takeover_context_stale');
  await assertLive(f,token,successor.sessionId);assert.equal(successor.client.readyState,WebSocket.OPEN);
  assert.equal((await accept(f,token,value.context)).body.error.code,'takeover_context_stale');
});

test('context is bound to current account token and exact desktop, not just account identity',async t=>{
  const f=await fixture(t),token=await f.login('alice'),old=await attach(f,token),value=await offer(f,token);
  const other=newToken();f.store.createSession(f.store.findUser('alice'),tokenHash(other),Date.now(),3600000,'client');
  assert.equal((await accept(f,other,value.context)).body.error.code,'takeover_context_stale');await assertLive(f,token,old.sessionId);
  const fresh=await offer(f,token);f.store.assign('alice','pve-149');
  const wrong=await f.call('/desktops/pve-149/takeover',{method:'POST',token,context:'v1',data:{context:fresh.context}});
  assert.equal(wrong.body.error.code,'takeover_context_stale');await assertLive(f,token,old.sessionId);
});

test('expiry and invalid or missing context never fall back to legacy destructive release',async t=>{
  const f=await fixture(t),token=await f.login('alice'),old=await attach(f,token),value=await offer(f,token);
  f.advance(120000);assert.equal((await accept(f,token,value.context)).body.error.code,'takeover_context_expired');await assertLive(f,token,old.sessionId);
  for(const data of [{},{context:'bad'},{context:'A'.repeat(43),extra:true}]){
    assert.equal((await f.call(endpoint+'/takeover',{method:'POST',token,context:'v1',data})).status,400);await assertLive(f,token,old.sessionId);
  }
});

// Any assigned account is offered the takeover (with
// the occupant's name for its confirmation); losing the assignment still
// prevents the release.
test('another assigned user sees the occupant and only assignment loss prevents release',async t=>{
  const f2=await fixture(t),a=await f2.login('alice'),b=await f2.login('bobby'),occupied=await attach(f2,a);
  const busy=await f2.call(endpoint+'/connect',{method:'POST',token:b,context:'v1'});assert.equal(busy.body.error.details.takeover,true);assert.equal(busy.body.error.details.takeoverOffer.sessions[0].username,'alice');
  const own=await offer(f2,a),stops=[],originalStop=f2.relay.stop.bind(f2.relay);
  f2.relay.stop=(session,reason,...rest)=>{stops.push({id:session.id,reason});return originalStop(session,reason,...rest);};
  f2.store.db.prepare('DELETE FROM assignments WHERE user_id=?').run(f2.store.findUser('alice').id);
  assert.equal((await accept(f2,a,own.context)).status,404);
  assert.equal(stops.filter(s=>s.id===occupied.sessionId&&s.reason==='taken_over').length,0,'refused takeover never stops the controller; independent assignment revocation may retire it');
});

test('unknown stream timestamp remains null and does not obstruct a permitted contextual release',async t=>{
  const f=await fixture(t),token=await f.login('alice'),old=await attach(f,token);delete f.relay.sessions.get(old.sessionId).createdAt;
  const value=await offer(f,token);assert.equal(value.sessions[0].createdAt,null);
  assert.equal((await accept(f,token,value.context)).status,200);
});

test('manual negotiated login adds exact real context while legacy details and invalid-password privacy stay intact',async t=>{
  const f=await fixture(t),token=await f.login('alice'),old=await attach(f,token);
  const pending=await f.call('/auth/login',{method:'POST',data:{username:'alice',password},intent:'confirmable-v1',loginContext:'v1'});
  assert.equal(pending.status,409);const d=pending.body.error.details;
  assert.deepEqual(Object.keys(d).sort(),['challengeToken','existingLogin','expiresInMs','protocol']);
  assert.equal(d.existingLogin.username,'alice');assert.equal(d.existingLogin.sessionCount,1);assert.equal(d.existingLogin.sessions.length,1);
  assert.equal(d.existingLogin.sessions[0].issuer,'legacy');assert.ok(Number.isFinite(Date.parse(d.existingLogin.sessions[0].createdAt)));
  assert.deepEqual(d.existingLogin.sessions[0].desktops,[{id:'pve-170',name:'VM 170'}]);
  assert.ok(!JSON.stringify(d.existingLogin).includes(tokenHash(token)));assert.ok(!JSON.stringify(d.existingLogin).includes(token));
  const legacy=await f.call('/auth/login',{method:'POST',data:{username:'alice',password},intent:'confirmable-v1'});
  assert.deepEqual(Object.keys(legacy.body.error.details).sort(),['challengeToken','expiresInMs','protocol']);
  const bad=await f.call('/auth/login',{method:'POST',data:{username:'alice',password:'invalid-password'},intent:'confirmable-v1',loginContext:'v1'});
  assert.equal(bad.status,401);assert.ok(!JSON.stringify(bad.body).includes('existingLogin'));await assertLive(f,token,old.sessionId);
  const confirmed=await f.call('/auth/login/confirm',{method:'POST',data:{challengeToken:d.challengeToken,confirm:true}});assert.equal(confirmed.status,200);
  assert.equal((await old.client.take(v=>v.type==='session.ended')).reason,'pc_login_replaced');
});

test('negotiation rejects unsupported headers and does not enable automatic recovery context',async t=>{
  const f=await fixture(t),token=await f.login('alice'),old=await attach(f,token);
  assert.equal((await f.call(endpoint+'/connect',{method:'POST',token,context:'v2'})).status,400);
  assert.equal((await f.call('/auth/login',{method:'POST',data:{username:'alice',password},intent:'recovery-v1',loginContext:'v1'})).status,400);
  assert.equal((await f.call('/auth/login',{method:'POST',data:{username:'alice',password},intent:'confirmable-v1',loginContext:'v2'})).status,400);
  await assertLive(f,token,old.sessionId);
});

const preflight=(f,token,extra={})=>f.call(endpoint+'/takeover-context',{method:'POST',token,context:'v1',data:{},...extra});

test('occupied stop-v1 display preflight exposes safe takeover before any display mutation',async t=>{
  const f=await fixture(t),token=await f.login('alice');
  f.host.send(JSON.stringify({type:'host.capabilities',capabilities:{service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,virtualMonitorControl:true,sessionStopProtocol:'stop-v1',monitors:[0,1].map(id=>({id:String(id),width:1920,height:1080,primary:id===0}))}}));
  assert.equal((await f.host.take(v=>v.type==='host.capabilities.ack')).accepted,true);
  const old=await attach(f,token),session=f.relay.sessions.get(old.sessionId);
  assert.equal(session.stopProtocol,'stop-v1');assert.equal(session.hostDispatched,true);
  const display=await f.call(endpoint+'/display',{method:'POST',token,data:{count:1}});
  assert.equal(display.status,409);assert.equal(display.body.error.code,'desktop_busy');assert.equal(display.body.error.details,undefined);
  const mutationCalls=[],originalStop=f.relay.stop.bind(f.relay),originalConfigure=f.relay.configureDisplay,originalConnect=f.relay.connect;
  f.relay.stop=(...args)=>{mutationCalls.push('stop');return originalStop(...args);};
  f.relay.configureDisplay=(...args)=>{mutationCalls.push('display');return originalConfigure.apply(f.relay,args);};
  f.relay.connect=(...args)=>{mutationCalls.push('connect');return originalConnect.apply(f.relay,args);};
  const before={sessions:f.relay.sessions.size,operations:f.relay.displayOperations.size,busy:f.relay.displayBusy.size};
  const response=await preflight(f,token);assert.equal(response.status,409);assert.equal(response.body.error.code,'desktop_busy');
  const details=response.body.error.details;assert.equal(details.reason,'controller_in_use');assert.equal(details.takeover,true);
  assert.equal(details.takeoverOffer.sessions[0].username,'alice');assert.deepEqual(details.takeoverOffer.desktop,{id:'pve-170',name:'VM 170'});
  assert.deepEqual(mutationCalls,[]);assert.deepEqual({sessions:f.relay.sessions.size,operations:f.relay.displayOperations.size,busy:f.relay.displayBusy.size},before);
  assert.equal(f.relay.sessions.get(old.sessionId),session);assert.equal(old.client.readyState,WebSocket.OPEN);
  assert.equal(JSON.stringify(details).includes(token),false);assert.equal(JSON.stringify(details).includes(tokenHash(token)),false);assert.equal(JSON.stringify(details).includes(old.sessionId),false);
  assert.equal((await accept(f,token,details.takeoverOffer.context)).body.released,true);
  assert.equal((await old.client.take(v=>v.type==='session.ended')).reason,'taken_over');
  assert.deepEqual(mutationCalls,['stop']);
});

test('unoccupied takeover preflight is exact read-only busy false, including offline approved Hosts',async t=>{
  const f=await fixture(t),token=await f.login('alice');
  const called=[];for(const method of ['connect','configureDisplay','stop','assertDisplayIdle','assertPowerAllowed'])f.relay[method]=()=>{called.push(method);assert.fail('read-only preflight called '+method);};
  const first=await preflight(f,token);assert.equal(first.status,200);assert.deepEqual(first.body,{busy:false});
  f.host.close();
  const second=await preflight(f,token);assert.equal(second.status,200);assert.deepEqual(second.body,{busy:false});
  assert.deepEqual(called,[]);assert.equal(f.relay.sessions.size,0);assert.equal(f.relay.displayOperations.size,0);
});

test('takeover preflight requires opt-in and empty object, rejects methods and leaks no identity on invalid input',async t=>{
  const f=await fixture(t),token=await f.login('alice'),old=await attach(f,token);
  for(const extra of [{context:undefined},{context:'v2'},{context:['v1','v1']},{data:{context:'A'.repeat(43)}},{data:{count:1}},{data:[]},{data:null}]){
    const response=await preflight(f,token,extra);assert.equal(response.status,400);assert.equal(JSON.stringify(response.body).includes('takeoverOffer'),false);
    assert.equal(f.relay.sessions.has(old.sessionId),true);
  }
  assert.equal((await f.call(endpoint+'/takeover-context',{token,context:'v1'})).status,405);
  assert.equal((await preflight(f,undefined)).status,401);
  await assertLive(f,token,old.sessionId);
});

test('preflight applies current assignment and approved registration fences without VM discovery',async t=>{
  const f=await fixture(t),token=await f.login('alice');
  f.store.assign('alice','pve-149');
  const unregistered=await f.call('/desktops/pve-149/takeover-context',{method:'POST',token,context:'v1',data:{}});
  assert.equal(unregistered.status,404);assert.equal(unregistered.body.error.code,'desktop_not_found');
  f.store.db.prepare('DELETE FROM assignments WHERE user_id=? AND desktop_id=?').run(f.store.findUser('alice').id,'pve-170');
  assert.equal((await preflight(f,token)).status,404);assert.equal(f.relay.sessions.size,0);assert.equal(f.relay.displayOperations.size,0);
});

test('preflight repeats auth after request body and never uses the earlier authenticated snapshot',async t=>{
  const f=await fixture(t),token=await f.login('alice'),authenticate=f.store.authenticate.bind(f.store);let calls=0;
  f.store.authenticate=(hash,...args)=>{if(hash===tokenHash(token)&&++calls>1)return null;return authenticate(hash,...args);};
  const response=await preflight(f,token);assert.equal(response.status,401);assert.equal(response.body.error.code,'session_expired');
  assert.ok(calls>=2);assert.equal(f.relay.sessions.size,0);assert.equal(f.relay.displayOperations.size,0);
});

test('preflight offers the occupant to another assigned user and to an administrator alike',async t=>{
  const f=await fixture(t,{aliceAdmin:true}),admin=await f.login('alice'),bob=await f.login('bobby'),old=await attach(f,admin);
  const offered=await preflight(f,bob);assert.equal(offered.status,409);assert.equal(offered.body.error.details.takeover,true);assert.equal(offered.body.error.details.takeoverOffer.sessions[0].username,'alice');
  old.client.close();await f.host.take(v=>v.type==='session.stop');
  const occupied=await attach(f,bob),allowed=await preflight(f,admin);assert.equal(allowed.status,409);
  assert.equal(allowed.body.error.details.takeoverOffer.sessions[0].username,'bobby');assert.equal(f.relay.sessions.has(occupied.sessionId),true);
});

test('a preflight review cannot release a replaced occupant or transfer to a different token',async t=>{
  const f=await fixture(t),token=await f.login('alice'),old=await attach(f,token),review=await preflight(f,token);
  assert.equal(review.status,409);const value=review.body.error.details.takeoverOffer;assert.ok(value);
  const other=newToken();f.store.createSession(f.store.findUser('alice'),tokenHash(other),Date.now(),3600000,'client');
  assert.equal((await accept(f,other,value.context)).body.error.code,'takeover_context_stale');await assertLive(f,token,old.sessionId);
  const second=await preflight(f,token);old.client.close();await f.host.take(v=>v.type==='session.stop');const successor=await attach(f,token);
  const stale=await accept(f,token,second.body.error.details.takeoverOffer.context);assert.equal(stale.body.error.code,'takeover_context_stale');
  await assertLive(f,token,successor.sessionId);assert.equal(successor.client.readyState,WebSocket.OPEN);
});


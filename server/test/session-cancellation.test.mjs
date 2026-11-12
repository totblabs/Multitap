// Real loopback HTTP/WebSocket routing, synthetic Host/VM identity. This does
// not exercise Windows session unlock, display restoration or physical capture.
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {randomBytes} from 'node:crypto';
import {WebSocket} from 'ws';
import {Store} from '../src/store.mjs';
import {Vault} from '../src/vault.mjs';
import {buildServer} from '../src/http.mjs';
import {newToken,tokenHash} from '../src/security.mjs';

const origin='https://session-cancel-fixture.example';
const capabilities={service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true};
async function socket(url,authorization){
  const ws=new WebSocket(url,{headers:{Authorization:authorization},perMessageDeflate:false});
  const messages=[];ws.on('message',value=>messages.push(JSON.parse(value.toString('utf8'))));
  ws.on('error',()=>{});
  await new Promise((resolve,reject)=>{ws.once('open',resolve);ws.once('error',reject);ws.once('unexpected-response',(_req,response)=>{response.resume();reject(Object.assign(new Error('upgrade_rejected'),{status:response.statusCode}));});});
  ws.messages=messages;return ws;
}
async function until(predicate){const end=Date.now()+2500;while(!predicate()){assert.ok(Date.now()<end,'fixture_timeout');await new Promise(resolve=>setTimeout(resolve,5));}}
async function fixture(t){
  const store=new Store(':memory:'),vault=new Vault(randomBytes(32)),sockets=[];
  for(const name of ['alice','bobby','admin'])store.addUser({name,role:name==='admin'?'admin':'user',passwordHash:'fixture-unused'});
  store.syncProxmox([{vmid:149,node:'fixture',name:'VM149',status:'running'}],'fixture');
  for(const name of ['alice','bobby','admin'])store.assign(name,'pve-149');
  const tokens={alice:newToken(),aliceOther:newToken(),bobby:newToken(),admin:newToken(),host:newToken()},code=newToken();
  for(const name of ['alice','aliceOther','bobby','admin'])store.createSession(store.findUser(name==='aliceOther'?'alice':name),tokenHash(tokens[name]),Date.now(),3600000,'client');
  store.issueEnrollment('pve-149','12345678-1234-1234-1234-123456789abc',tokenHash(code),'fixture');
  store.consumeEnrollment(tokenHash(code),'12345678-1234-1234-1234-123456789abc',{hostname:'Fixture',version:'9.0.0',tokenHash:tokenHash(tokens.host)});
  const server=await buildServer({store,vault,publicOrigin:origin,relayOptions:{heartbeatMs:10000,authorizationMs:10000},logger:{error:value=>assert.fail(value)}});
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  t.after(async()=>{for(const ws of sockets)ws.terminate();await new Promise(resolve=>server.close(resolve));store.close();});
  const base='http://127.0.0.1:'+server.address().port;
  async function open(path,as='alice'){const ws=await socket(base.replace('http:','ws:')+path,(as==='host'?'Host ':'Bearer ')+tokens[as]);sockets.push(ws);return ws;}
  const host=await open('/api/v1/hosts/channel','host');host.send(JSON.stringify({type:'host.capabilities',capabilities}));
  async function call(path,{method='GET',as='alice',data,requestOrigin}={}){
    const response=await fetch(base+'/api/v1'+path,{method,headers:{...(as?{Authorization:'Bearer '+tokens[as]}:{}),...(requestOrigin?{Origin:requestOrigin}:{}),...(data===undefined?{}:{'Content-Type':'application/json'})},body:data===undefined?undefined:JSON.stringify(data)});
    return {status:response.status,body:await response.json()};
  }
  await until(()=>host.messages.some(v=>v.type==='host.capabilities.ack'));
  const create=async()=>{const r=await call('/desktops/pve-149/connect',{method:'POST',data:{mode:'stream'}});assert.equal(r.status,200);return r.body;};
  const cancel=(id,options={})=>call('/sessions/'+id,{method:'DELETE',...options});
  return {store,tokens,host,call,create,cancel,open};
}

test('Cancel before WebSocket attach releases the ticket immediately without starting the Host',async t=>{
  const f=await fixture(t),ticket=await f.create();
  assert.equal((await f.call('/desktops/pve-149/connect',{method:'POST',data:{}})).body.error.code,'desktop_busy');
  const cancelled=await f.cancel(ticket.sessionId);
  assert.equal(cancelled.status,200);
  assert.deepEqual(cancelled.body,{sessionId:ticket.sessionId,released:true});
  const next=await f.create();assert.notEqual(next.sessionId,ticket.sessionId);
  assert.equal(f.host.messages.some(v=>v.type==='session.start'||v.type==='session.stop'),false);
  await assert.rejects(f.open('/api/v1/sessions/'+ticket.sessionId),error=>error.status===401);
});

test('An active stream cancellation uses ordinary session stop and never claims Windows restoration',async t=>{
  const f=await fixture(t),ticket=await f.create(),client=await f.open('/api/v1/sessions/'+ticket.sessionId);
  await until(()=>f.host.messages.some(v=>v.type==='session.start'));
  assert.equal((await f.cancel(ticket.sessionId)).status,200);
  await until(()=>f.host.messages.some(v=>v.type==='session.stop'&&v.sessionId===ticket.sessionId));
  await until(()=>client.messages.some(v=>v.type==='session.ended'));
  assert.equal(f.host.messages.filter(v=>v.type==='session.stop').length,1);
  assert.equal((await f.cancel(ticket.sessionId)).status,404);
  await f.create();
});

test('Only the creating login token can cancel, including accounts assigned to the same VM',async t=>{
  const f=await fixture(t),ticket=await f.create();
  for(const as of ['bobby','admin','aliceOther'])assert.equal((await f.cancel(ticket.sessionId,{as})).status,404);
  assert.equal((await f.call('/desktops/pve-149/connect',{method:'POST',data:{}})).status,409);
  assert.equal((await f.cancel(ticket.sessionId)).status,200);
});

test('Expired login, revoked assignment, foreign origin, body and wrong method never cancel',async t=>{
  const f=await fixture(t),ticket=await f.create();
  assert.equal((await f.cancel(ticket.sessionId,{as:null})).status,401);
  assert.equal((await f.cancel(ticket.sessionId,{requestOrigin:'https://foreign.invalid'})).status,403);
  assert.equal((await f.cancel(ticket.sessionId,{data:{desktopId:'pve-149'}})).status,400);
  assert.equal((await f.call('/sessions/'+ticket.sessionId)).status,405);
  f.store.revoke('alice','pve-149');assert.equal((await f.cancel(ticket.sessionId)).status,404);
  f.store.assign('alice','pve-149');f.store.logout(tokenHash(f.tokens.alice));assert.equal((await f.cancel(ticket.sessionId)).status,401);
});

test('A late cancellation for an ended session cannot retire its replacement',async t=>{
  const f=await fixture(t),old=await f.create();assert.equal((await f.cancel(old.sessionId)).status,200);
  const next=await f.create();assert.equal((await f.cancel(old.sessionId)).status,404);
  assert.equal((await f.call('/desktops/pve-149/connect',{method:'POST',data:{}})).body.error.code,'desktop_busy');
  assert.equal((await f.cancel(next.sessionId)).status,200);
});

test('The connection reply explicitly advertises the supported cancellation protocol',async t=>{
  const f=await fixture(t),ticket=await f.create();assert.equal(ticket.sessionCancellationProtocol,'session-cancel-v1');
});

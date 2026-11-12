import {test} from 'node:test';
import assert from 'node:assert/strict';
import {parseHTML} from 'linkedom';
import {normalizeMfaRecoveryCode,createMfaDeadline,readMfaStatus,readMfaSetup,readMfaChanged,requestMfaAuthentication,retireMfaCookie,MfaClientError} from '../admin/js/mfa-auth.js';
import {createAccountSecurityController} from '../admin/js/account-security-controller.js';

const secret='A'.repeat(32),proofToken='t'.repeat(43),csrf='c'.repeat(43);
const codes=Array.from({length:10},(_,i)=>i.toString(16).padStart(32,'0'));
const grouped=value=>value.match(/.{4}/g).join('-');
const deferred=()=>{let resolve,reject;const promise=new Promise((a,b)=>{resolve=a;reject=b;});return{promise,resolve,reject};};
const response=(body,status=200)=>({status,ok:status>=200&&status<300,json:async()=>body});
const setup=()=>({protocol:'mfa-v1',setupToken:proofToken,expiresAt:new Date(Date.now()+60000).toISOString(),secret,uri:`otpauth://totp/Multitap%3Afixture?secret=${secret}&issuer=Multitap&algorithm=SHA1&digits=6&period=30`});
function fixture(t,{enabled=false,timeoutMs,now,monotonic}={}){
  const {document}=parseHTML('<html><body></body></html>'),oldDocument=globalThis.document;globalThis.document=document;
  let auth=true,revision=1,scope='https://admin.fixture.invalid',views=[],reply=null;
  const requests=[],retirements=[];
  const owner=()=>({revision}),isCurrent=value=>auth&&value?.revision===revision;
  const controller=createAccountSecurityController({document,owner,isCurrent,origin:()=>scope,accountName:()=> 'Fixture administrator',timeoutMs,now,monotonic,
    createView:options=>{const view={element:document.createElement('dialog'),models:[],opened:false,destroyed:false,closeExternal:()=>options.onClose(),open(){this.opened=true;},render(model){this.models.push(structuredClone(model));},destroy(){this.destroyed=true;this.element.replaceChildren();this.element.remove();}};views.push(view);return view;},
    request:async(path,method,data,{accept})=>{requests.push({path,method,data:data===undefined?undefined:structuredClone(data)});const result=reply?await reply(path,method,data):path==='/mfa'?{status:200,body:{protocol:'mfa-v1',enabled,recoveryCodesRemaining:enabled?10:0}}:path==='/mfa/setup'?{status:201,body:setup()}:path==='/mfa/reauth'?{status:200,body:{ok:true}}:{status:200,body:path==='/mfa/disable'?{enabled:false,requiresLogin:true}:{enabled:true,recoveryCodes:[...codes],requiresLogin:true}};return accept(result);},
    retireAuthentication:ticket=>{retirements.push(ticket);auth=false;revision++;controller.retire({preserveResult:ticket});},
  });
  t.after(()=>{controller.destroy();globalThis.document=oldDocument;});
  return{controller,requests,retirements,view:()=>views.at(-1),model:()=>views.at(-1)?.models.at(-1),authenticated:()=>auth,setReply:value=>reply=value,retire(){auth=false;revision++;controller.retire();},newOwner(){revision++;auth=true;controller.retire();},changeOrigin(){scope='https://other.fixture.invalid';}};
}

test('client normalization accepts only raw32 or exactly eight hexadecimal groups, never trims',()=>{
  const raw='ABCDEF0123456789ABCDEF0123456789';
  assert.equal(normalizeMfaRecoveryCode(raw),raw.toLowerCase());assert.equal(normalizeMfaRecoveryCode(grouped(raw)),raw.toLowerCase());
  for(const value of [null,42,' '+raw,raw+' ',raw+'\n','-'+raw,raw+'-',raw.slice(0,4)+'--'+raw.slice(4),grouped(raw).replace('-','—'),'１２３４'.repeat(8),raw.slice(1),raw+'0'])assert.equal(normalizeMfaRecoveryCode(value),null);
});
test('actual DTOs remain raw status, setup201 and exact once-only result without fabricated fields',()=>{
  assert.deepEqual(readMfaStatus({protocol:'mfa-v1',enabled:true,recoveryCodesRemaining:3},200),{enabled:true,recoveryCodesRemaining:3});
  assert.throws(()=>readMfaStatus({protocol:'mfa-v1',enabled:true,recoveryCodesRemaining:3,available:true},200),e=>e.code==='mfa_response_invalid');
  assert.equal(readMfaSetup(setup(),201).setupToken,proofToken);assert.throws(()=>readMfaSetup(setup(),200),e=>e.uncertain);
  assert.equal(readMfaChanged({enabled:true,recoveryCodes:codes,requiresLogin:true},200,'setup').codes.length,10);
  assert.throws(()=>readMfaChanged({enabled:true,recoveryCodes:[...codes.slice(0,9),codes[0]],requiresLogin:true},200,'recovery'),e=>e.uncertain);
});
test('first enrollment maps statusGET/setup201 and validated ACK retires real auth before response-only codes',async t=>{
  const f=fixture(t);await f.controller.open();assert.equal(f.model().stage,'summary');assert.equal(f.requests[0].method,'GET');assert.equal(f.requests[0].data,undefined);
  await f.controller.act({kind:'begin',intent:'setup'});await f.controller.act({kind:'submitProof',currentPassword:'Synthetic-controller-password!'});
  assert.equal(f.model().stage,'setup');assert.equal(f.model().secret,secret);assert.deepEqual(f.requests.map(x=>x.path),['/mfa','/mfa/setup']);
  await f.controller.act({kind:'submitSetup',code:'123456'});
  assert.equal(f.authenticated(),false);assert.equal(f.retirements.length,1);assert.equal(f.model().stage,'recovery');assert.equal(f.model().requiresLogin,true);assert.deepEqual(f.model().codes,codes);assert.equal(f.controller.hasCompletedResult(),true);
  await f.controller.act({kind:'finish'});assert.equal(f.controller.hasCompletedResult(),false);assert.equal(f.view().destroyed,true);
});
for(const intent of ['disable','recovery','setup'])test(`enabled ${intent} performs fresh factor reauth before its explicit second password call`,async t=>{
  const f=fixture(t,{enabled:true});await f.controller.open();await f.controller.act({kind:'begin',intent});
  const raw='ABCDEF0123456789ABCDEF0123456789',action={kind:'submitProof',currentPassword:'Synthetic-controller-password!',recoveryCode:grouped(raw)};
  await f.controller.act(action);assert.deepEqual(f.requests.slice(1).map(x=>x.path),['/mfa/reauth','/mfa/'+intent]);
  assert.deepEqual(f.requests[1].data,{currentPassword:'Synthetic-controller-password!',recoveryCode:raw.toLowerCase()});
  assert.deepEqual(f.requests[2].data,{currentPassword:'Synthetic-controller-password!'});assert.equal(action.currentPassword,'');assert.equal(action.recoveryCode,'');
  if(intent==='setup'){assert.equal(f.authenticated(),true);assert.equal(f.model().stage,'setup');}
  else{assert.equal(f.authenticated(),false);assert.equal(f.model().requiresLogin,true);assert.equal(f.model().codes.length,intent==='disable'?0:10);}
});
test('retiring during held reauth cancels the second request without resurrecting proof or setup secret',async t=>{
  const f=fixture(t,{enabled:true}),held=deferred();await f.controller.open();f.setReply(path=>path==='/mfa/reauth'?held.promise:null);
  await f.controller.act({kind:'begin',intent:'recovery'});const pending=f.controller.act({kind:'submitProof',currentPassword:'Synthetic-controller-password!',code:'123456'});
  await new Promise(r=>setImmediate(r));f.retire();held.resolve({status:200,body:{ok:true}});await pending;
  assert.equal(f.requests.some(x=>x.path==='/mfa/recovery'),false);assert.equal(f.controller.hasCompletedResult(),false);assert.equal(f.view().destroyed,true);
});
test('external401/manual retirement before an enable ACK never displays late once-only codes',async t=>{
  const f=fixture(t),held=deferred();await f.controller.open();await f.controller.act({kind:'begin',intent:'setup'});await f.controller.act({kind:'submitProof',currentPassword:'Synthetic-controller-password!'});
  f.setReply(()=>held.promise);const pending=f.controller.act({kind:'submitSetup',code:'123456'});f.retire();held.resolve({status:200,body:{enabled:true,recoveryCodes:codes,requiresLogin:true}});await pending;
  assert.equal(f.controller.hasCompletedResult(),false);assert.equal(f.retirements.length,0);assert.equal(f.view().destroyed,true);assert.equal(f.view().models.some(x=>x.codes?.length),false);
});
test('same-account fresh generation and origin changes reject held result admission',async t=>{
  const f=fixture(t),held=deferred();await f.controller.open();await f.controller.act({kind:'begin',intent:'setup'});await f.controller.act({kind:'submitProof',currentPassword:'Synthetic-controller-password!'});
  f.setReply(()=>held.promise);const pending=f.controller.act({kind:'submitSetup',code:'123456'});f.newOwner();f.changeOrigin();held.resolve({status:200,body:{enabled:true,recoveryCodes:codes,requiresLogin:true}});await pending;
  assert.equal(f.controller.hasCompletedResult(),false);assert.equal(f.retirements.length,0);
});
test('malformed mutation ACK retires to nonsecret uncertain state instead of fake codes/authentication',async t=>{
  const f=fixture(t);await f.controller.open();await f.controller.act({kind:'begin',intent:'setup'});await f.controller.act({kind:'submitProof',currentPassword:'Synthetic-controller-password!'});
  f.setReply(()=>({status:200,body:{enabled:true,recoveryCodes:['malformed'],requiresLogin:true}}));await f.controller.act({kind:'submitSetup',code:'123456'});
  assert.equal(f.authenticated(),false);assert.equal(f.model().stage,'uncertain');assert.equal(f.model().requiresLogin,true);assert.deepEqual(f.model().codes,[]);assert.equal(f.model().secret,undefined);
});
test('definite invalid candidate code preserves setup for explicit retry and does not retire auth',async t=>{
  const f=fixture(t);await f.controller.open();await f.controller.act({kind:'begin',intent:'setup'});await f.controller.act({kind:'submitProof',currentPassword:'Synthetic-controller-password!'});
  f.setReply(()=>{throw new MfaClientError(400,'mfa_invalid_code');});await f.controller.act({kind:'submitSetup',code:'123456'});
  assert.equal(f.model().stage,'setup');assert.equal(f.model().secret,secret);assert.equal(f.model().busy,false);assert.equal(f.authenticated(),true);
});
test('late canceled auth200 cleanup uses its response CSRF;202/409 never trigger logout',async()=>{
  for(const status of [200,202,409]){
    let current=true;const held=deferred(),requests=[];
    const pending=requestMfaAuthentication({path:'/api/v1/admin/mfa/verify',data:{challengeToken:proofToken,code:'123456'},owner:{},isCurrent:()=>current,origin:()=> 'https://admin.fixture.invalid',fetcher:async(path,options)=>{requests.push({path,options});return path.endsWith('/logout')?response({ok:true}):held.promise;}}).catch(e=>e);
    current=false;held.resolve(response(status===200?{user:{id:'fixture',username:'fixture',role:'admin'},csrfToken:csrf}:{mfa:{}},status));const result=await pending;
    assert.equal(result.code,'admin_session_changed');assert.equal(requests.length,status===200?2:1);
    if(status===200)assert.deepEqual(requests[1].options.headers,{'X-CSRF-Token':csrf});
    assert.equal(requests[0].options.headers['X-Multitap-Auth-Protocol'],'mfa-v1');
  }
});
test('current202 is anonymous challenge and malformed typed error does not expose server text',async()=>{
  const challenge={mfa:{protocol:'mfa-v1',challengeToken:proofToken,expiresAt:new Date(Date.now()+60000).toISOString(),methods:['totp','recovery']}};
  const result=await requestMfaAuthentication({path:'/api/v1/admin/login',data:{},owner:{},isCurrent:()=>true,origin:()=> 'https://admin.fixture.invalid',fetcher:async()=>response(challenge,202)});
  assert.equal(result.kind,'challenge');assert.equal(result.session,undefined);
  await assert.rejects(requestMfaAuthentication({path:'/api/v1/admin/login',data:{},owner:{},isCurrent:()=>true,origin:()=> 'https://admin.fixture.invalid',fetcher:async()=>response({error:{code:'mfa_invalid_code',message:'Synthetic-private-server-text'}},400)}),e=>e.status===400&&e.code==='mfa_invalid_code'&&!e.message.includes('Synthetic-private'));
});
test('bounded authentication timeout remains uncertain and later200 uses only its own cleanup credential',async()=>{
  const held=deferred(),requests=[];
  const pending=requestMfaAuthentication({path:'/api/v1/admin/login',data:{},owner:{},isCurrent:()=>true,origin:()=> 'https://admin.fixture.invalid',timeoutMs:5,fetcher:async(path,options)=>{requests.push({path,options});return path.endsWith('/logout')?response({ok:true}):held.promise;}});
  await assert.rejects(pending,e=>e.code==='mfa_network_unknown'&&e.uncertain);
  held.resolve(response({user:{id:'fixture',username:'fixture',role:'admin'},csrfToken:csrf}));await new Promise(r=>setImmediate(r));
  assert.equal(requests.length,2);assert.equal(requests[1].options.headers['X-CSRF-Token'],csrf);
});
test('bounded mutation timeout keeps only retired nonsecret uncertainty and refuses later code resurrection',async t=>{
  const f=fixture(t,{timeoutMs:5}),held=deferred();await f.controller.open();await f.controller.act({kind:'begin',intent:'setup'});await f.controller.act({kind:'submitProof',currentPassword:'Synthetic-controller-password!'});f.setReply(()=>held.promise);
  await f.controller.act({kind:'submitSetup',code:'123456'});assert.equal(f.authenticated(),false);assert.equal(f.model().stage,'uncertain');assert.deepEqual(f.model().codes,[]);
  held.resolve({status:200,body:{enabled:true,recoveryCodes:codes,requiresLogin:true}});await new Promise(r=>setImmediate(r));assert.deepEqual(f.model().codes,[]);
});
test('best-effort cookie cleanup is bounded, origin-bound and configurable for console scoped CSRF',async()=>{
  let calls=0;const common={path:'/console/api/logout',csrfToken:csrf,origin:()=> 'https://console.fixture.invalid',admittedOrigin:'https://console.fixture.invalid'};
  assert.equal(await retireMfaCookie({...common,csrfHeader:'X-Multitap-Console-CSRF',fetcher:async(_,options)=>{calls++;assert.deepEqual(options.headers,{'X-Multitap-Console-CSRF':csrf});return response({ok:true});}}),true);
  assert.equal(await retireMfaCookie({...common,admittedOrigin:'https://other.fixture.invalid',fetcher:async()=>{calls++;}}),false);assert.equal(calls,1);
  assert.equal(await retireMfaCookie({...common,timeoutMs:5,fetcher:()=>new Promise(()=>{})}),false);
});
test('forced native close discards result locally without claiming saved completion or restoring authentication',async t=>{
  const f=fixture(t);await f.controller.open();await f.controller.act({kind:'begin',intent:'setup'});await f.controller.act({kind:'submitProof',currentPassword:'Synthetic-controller-password!'});await f.controller.act({kind:'submitSetup',code:'123456'});
  f.view().closeExternal();assert.equal(f.controller.hasCompletedResult(),false);assert.equal(f.view().destroyed,true);assert.equal(f.authenticated(),false);
});
test('malformed exclusive factor proof is rejected locally without an authorization request',async t=>{
  const f=fixture(t,{enabled:true});await f.controller.open();await f.controller.act({kind:'begin',intent:'disable'});
  await f.controller.act({kind:'submitProof',currentPassword:'Synthetic-controller-password!',code:'123456',recoveryCode:codes[0]});
  assert.equal(f.requests.length,1);assert.equal(f.model().stage,'proof');assert.equal(f.authenticated(),true);
});

test('local deadline uses both clocks, exact expiry, rollback rejection and a bounded lifetime',()=>{
  let wall=1000000,mono=200;const options={now:()=>wall,monotonic:()=>mono};
  const deadline=createMfaDeadline(new Date(wall+60000).toISOString(),options);
  assert.equal(deadline.remaining(),60000);mono+=59999;assert.equal(deadline.valid(),true);mono++;assert.equal(deadline.valid(),false);
  wall=1000000;mono=200;const rollback=createMfaDeadline(new Date(wall+60000).toISOString(),options);wall--;assert.equal(rollback.valid(),false);
  wall=1000000;mono=200;const backwardMono=createMfaDeadline(new Date(wall+60000).toISOString(),options);mono--;assert.equal(backwardMono.valid(),false);
  wall=1000000;mono=200;assert.equal(createMfaDeadline(new Date(wall+999999).toISOString(),options).remaining(),120000);
  assert.equal(createMfaDeadline('malformed',options).valid(),false);
});
test('expired setup clears provisional secret and token instead of making an enable request',async t=>{
  let wall=Date.now(),mono=200;const f=fixture(t,{now:()=>wall,monotonic:()=>mono});
  await f.controller.open();await f.controller.act({kind:'begin',intent:'setup'});await f.controller.act({kind:'submitProof',currentPassword:'Synthetic-controller-password!'});
  assert.equal(f.model().secret,secret);mono+=120000;wall--;
  await f.controller.act({kind:'submitSetup',code:'123456'});
  assert.equal(f.requests.some(row=>row.path==='/mfa/enable'),false);assert.equal(f.model().secret,'');assert.equal(f.model().expiresAt,'');assert.equal(f.model().stage,'proof');assert.equal(f.authenticated(),true);
});
// Over http every 2단계 인증 request is refused (mfa_requires_https). The model says so, so
// the view shows one sentence and 닫기 instead of a failed read with 다시 확인; any other failed read does not.
test('a status read refused over http is marked unavailable; another failed read is not',async t=>{
  const f=fixture(t);
  f.setReply(async()=>{throw Object.assign(new Error('refused'),{status:403,code:'mfa_requires_https'});});
  await f.controller.open();
  assert.equal(f.model().stage,'uncertain');assert.equal(f.model().unavailable,true);assert.equal(f.model().requiresLogin,false);
  f.setReply(async()=>{throw Object.assign(new Error('down'),{status:503,code:'mfa_unavailable'});});
  await f.controller.open();
  assert.equal(f.model().stage,'uncertain');assert.equal(f.model().unavailable,false);
});
// The ID and password form has no recovery code field; the real server answers it with
// 429 too_many_attempts (ninth wrong password) and 400 invalid_credentials_format (an ID with a space).
test('sign-in refusals on the password step: no recovery code hint, an unreadable ID asks for the ID and password',async()=>{
  const ask=(data,body,status)=>requestMfaAuthentication({path:'/api/v1/admin/login',data,owner:{},isCurrent:()=>true,origin:()=> 'https://admin.fixture.invalid',fetcher:async()=>response(body,status)});
  const many={error:{code:'too_many_attempts',message:'too_many_attempts'}};
  await assert.rejects(ask({username:'owner',password:'x'},many,429),e=>e.status===429&&e.code==='too_many_attempts'&&e.message==='시도가 많아요. 잠시 기다린 뒤 다시 로그인하세요.'&&!e.message.includes('복구 코드'));
  await assert.rejects(ask({username:'has space',password:'x'},{error:{code:'invalid_credentials_format',message:'invalid_credentials_format'}},400),e=>e.status===400&&e.message.startsWith('아이디 또는 비밀번호를 확인해 주세요.'));
  // The code step keeps the recovery code hint.
  await assert.rejects(ask({challengeToken:proofToken,code:'123456'},many,429),e=>e.message.includes('복구 코드'));
});

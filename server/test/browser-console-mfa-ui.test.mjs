// console.js text comes from console/i18n.js (docs/i18n.md); its imports are removed below, so the context gets the same functions.
import { t, translatePage } from '../admin/console/i18n.js';
import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {runInNewContext} from 'node:vm';
import {parseHTML} from 'linkedom';
import {createConsoleAuthentication,normalizeConsoleRecoveryCode} from '../admin/console/console-auth.js';
const html=await readFile(new URL('../admin/console/index.html',import.meta.url),'utf8');
const script=(await readFile(new URL('../admin/console/console.js',import.meta.url),'utf8')).replace(/^import .+;\r?\n/gm,'').replace(/\bboot\(\);\s*$/,'');
const tick=()=>new Promise(resolve=>setImmediate(resolve));
const challengeToken='M'.repeat(43),mfaCsrf='P'.repeat(43),sessionCsrf='S'.repeat(43),confirmationToken='C'.repeat(43);
const session={status:200,body:{csrfToken:sessionCsrf,user:{username:'fixture-user'}}};
const prepare={status:202,body:{code:'console_preparing'}};
const question=(details={})=>({status:409,body:{error:{code:'login_confirmation_required',details:{protocol:'session-access-v1',challengeToken:confirmationToken,expiresInMs:120000,...details}}}});
function fixture(responses){
  const {document,window:dom}=parseHTML(html),window=new EventTarget();
  const requests=[],timers=new Map();let sequence=0,wall=Date.now(),mono=0;
  for(const node of document.querySelectorAll('dialog')){node.open=false;node.showModal=()=>{node.open=true;};node.close=()=>{node.open=false;node.dispatchEvent(new dom.Event('close'));};}
  const schedule=fn=>{const id=++sequence;timers.set(id,fn);return id;},cancel=id=>timers.delete(id);
  const fetch=async(path,options)=>{
    requests.push({path,...options,body:options.body?JSON.parse(options.body):undefined});
    const next=responses.shift();assert.ok(next,'Unexpected request: '+path);
    const response=typeof next==='function'?await next():next;
    return {status:response.status,ok:response.status>=200&&response.status<300,json:async()=>response.body};
  };
  const context={document,window,location:{pathname:'/console/pve-170',origin:'https://fixture.invalid',protocol:'https:',hostname:'fixture.invalid'},navigator:{userAgent:'Fixture browser'},
    matchMedia:()=>({matches:false,addEventListener(){}}),fetch,AbortSignal,URL,initLogging(){},releaseCapture(){},createHangulKeyAdapter:()=>({reset(){}}),t,translatePage,setTimeout:schedule,clearTimeout:cancel,
    createConsoleAuthentication:options=>createConsoleAuthentication({...options,now:()=>wall,monotonic:()=>mono,schedule,cancel})};
  runInNewContext(script,context);
  const $=id=>document.getElementById(id);
  return {$,requests,timers,window,dom,
    password(){ $('username').value='fixture-user';$('password').value='synthetic-password';$('login-form').dispatchEvent(new dom.Event('submit',{cancelable:true})); },
    submit(code,{recovery=false}={}){ $('mfa-recovery').checked=recovery;$('mfa-code').value=code;$('mfa-form').dispatchEvent(new dom.Event('submit',{cancelable:true})); },
    click(id){$(id).dispatchEvent(new dom.Event('click',{cancelable:true}));},
    challenge(extra={}){return {status:202,body:{mfa:{protocol:'mfa-v1',challengeToken,expiresAt:new Date(wall+120000).toISOString(),methods:['totp','recovery'],...extra},csrfToken:mfaCsrf}};},
    advance(ms){wall+=ms;mono+=ms;},
    cleanup(){window.dispatchEvent(new Event('pagehide'));}
  };
}
async function begin(t,next=[]){const responses=[],f=fixture(responses);t.after(()=>f.cleanup());responses.push(f.challenge(),...next);f.password();await tick();return f;}

test('password login negotiates MFA and202 never starts transport or exposes authenticated logout',async t=>{
  const f=await begin(t);
  assert.equal(f.requests[0].headers['X-Multitap-Auth-Protocol'],'mfa-v1');
  assert.equal(f.$('mfa-form').hidden,false);assert.equal(f.$('login-form').hidden,true);
  assert.equal(f.$('password').value,'');assert.equal(f.$('logout').hidden,true);assert.equal(f.requests.length,1);
});
test('MFA verification uses only the challenge-scoped CSRF then adopts final session before connect',async t=>{
  const f=await begin(t,[session,prepare]);f.submit('123456');await tick();
  assert.deepEqual(f.requests.map(r=>r.path),['/console/api/login','/console/api/mfa/verify','/console/api/desktops/pve-170/connect']);
  assert.deepEqual(f.requests[1].body,{challengeToken,code:'123456'});assert.equal(f.requests[1].headers['X-Multitap-Console-CSRF'],mfaCsrf);
  assert.equal(f.requests[2].headers['X-Multitap-Console-CSRF'],sessionCsrf);assert.equal(f.$('mfa-code').value,'');
});
test('exact grouped uppercase recovery is normalized to raw lowercase32 before transport',async t=>{
  const f=await begin(t,[session,prepare]);f.submit('ABCD-ABCD-ABCD-ABCD-ABCD-ABCD-ABCD-ABCD',{recovery:true});await tick();
  assert.equal(f.requests[1].body.recoveryCode,'abcd'.repeat(8));assert.equal(f.requests[1].body.code,undefined);
});
test('unsupported recovery separators and lengths never make a proof request',async t=>{
  const f=await begin(t);for(const value of ['-'.repeat(32),'a'.repeat(31),'a'.repeat(33),'abcd_abcd_abcd_abcd_abcd_abcd_abcd_abcd','a'.repeat(32)+'-',' '+('a'.repeat(32)),('a'.repeat(32))+' ',('a'.repeat(32))+'\n']){f.submit(value,{recovery:true});await tick();}
  assert.equal(f.requests.length,1);assert.equal(f.$('mfa-form').hidden,false);
});
test('wrong code remains a manual pending step and never replays password automatically',async t=>{
  const f=await begin(t,[{status:400,body:{error:{code:'mfa_invalid_code'}}},session,prepare]);
  f.submit('123456');await tick();assert.equal(f.$('mfa-form').hidden,false);assert.equal(f.requests.length,2);assert.equal(f.$('mfa-code').value,'');
  f.submit('234567');await tick();assert.equal(f.requests.filter(r=>r.path==='/console/api/login').length,1);assert.equal(f.requests[2].body.code,'234567');
});
test('cancel retires pending MFA and a later submit sends no verification',async t=>{
  const f=await begin(t);f.click('mfa-cancel');f.submit('123456');await tick();assert.equal(f.requests.length,1);assert.equal(f.$('mfa-form').hidden,true);assert.equal(f.timers.size,0);
});
test('recovery success followed by consent Cancel explains consumption and sends no confirm',async t=>{
  const f=await begin(t,[question()]);f.submit('a'.repeat(32),{recovery:true});await tick();
  assert.equal(f.$('login-confirm-dialog').open,true);assert.equal(f.$('login-confirm-recovery').hidden,false);assert.match(f.$('login-confirm-recovery').textContent,/ 취소해도 다시 쓸 수 없어요/);
  f.click('login-confirm-cancel');await tick();assert.equal(f.requests.length,2);assert.match(f.$('login-message').textContent,/복구 코드는 사용됐어요/);assert.equal(f.timers.size,0);
});
test('MFA consent Accept sends the distinct confirmation token once without password or proof replay',async t=>{
  const f=await begin(t,[question(),session,prepare]);f.submit('123456');await tick();assert.equal(f.$('login-confirm-recovery').hidden,true);
  f.click('login-confirm-accept');f.click('login-confirm-accept');await tick();
  assert.deepEqual(f.requests.map(r=>r.path),['/console/api/login','/console/api/mfa/verify','/console/api/login/confirm','/console/api/desktops/pve-170/connect']);
  assert.deepEqual(f.requests[2].body,{challengeToken:confirmationToken,confirm:true});assert.equal(f.requests[2].headers['X-Multitap-Console-CSRF'],undefined);
});
test('malformed definite consent retires MFA and cannot manually reuse its original challenge',async t=>{
  const f=await begin(t,[question({expiresInMs:0})]);f.submit('123456');await tick();f.submit('234567');await tick();
  assert.equal(f.requests.length,2);assert.equal(f.$('mfa-form').hidden,true);assert.equal(f.$('login-confirm-dialog').open,false);
});
test('late MFA session after pagehide cannot connect and cleanup uses only late response CSRF',async t=>{
  let finish;const f=await begin(t,[()=>new Promise(resolve=>finish=resolve),{status:204}]);f.submit('123456');await tick();f.window.dispatchEvent(new Event('pagehide'));finish(session);await tick();
  assert.deepEqual(f.requests.map(r=>r.path),['/console/api/login','/console/api/mfa/verify','/console/api/logout']);
  assert.equal(f.requests[2].headers['X-Multitap-Console-CSRF'],sessionCsrf);assert.equal(f.$('logout').hidden,true);
});
test('expired pending challenge returns to fresh login without sending proof',async t=>{
  const f=await begin(t);f.advance(120001);for(const run of [...f.timers.values()])run();f.submit('123456');await tick();assert.equal(f.requests.length,1);assert.equal(f.$('mfa-form').hidden,true);assert.match(f.$('login-message').textContent,/인증 시간이 끝났어요/);
});
test('strict recovery helper accepts only the two negotiated presentation shapes',()=>{
  assert.equal(normalizeConsoleRecoveryCode('A'.repeat(32)),'a'.repeat(32));assert.equal(normalizeConsoleRecoveryCode('ABCD-ABCD-ABCD-ABCD-ABCD-ABCD-ABCD-ABCD'),'abcd'.repeat(8));
  for(const value of [null,32,' a'.repeat(16),'-'.repeat(32),'a'.repeat(32)+'-'])assert.equal(normalizeConsoleRecoveryCode(value),null);
});
test('local clock rollback retires pending proof instead of extending its lifetime',async t=>{
  const f=await begin(t);f.advance(-1);f.submit('123456');await tick();assert.equal(f.requests.length,1);assert.equal(f.$('mfa-form').hidden,true);
});

import test from 'node:test';
import assert from 'node:assert/strict';
import {fixture,tick,session,prepare} from './console-mfa-fixture.mjs';
const csrf=session.body.csrfToken;
const status={status:200,body:{protocol:'mfa-v1',enabled:false,recoveryCodesRemaining:0}};
const codes=Array.from({length:10},(_,i)=>i.toString(16).padStart(32,'0'));
const enabled={status:200,body:{enabled:true,recoveryCodes:codes,requiresLogin:true}};
const candidate=()=>({status:201,body:{protocol:'mfa-v1',setupToken:'E'.repeat(43),expiresAt:new Date(Date.now()+120000).toISOString(),secret:'A'.repeat(32),uri:'otpauth://totp/Multitap:fixture-user?secret='+('A'.repeat(32))+'&issuer=Multitap&algorithm=SHA1&digits=6&period=30'}});
function app(t,responses){
  const f=fixture(responses),previous=globalThis.document;
  globalThis.document=f.document;
  t.after(()=>{f.cleanup();if(previous===undefined)delete globalThis.document;else globalThis.document=previous;});
  const query=selector=>f.document.querySelector('.account-security-dialog '+selector);
  const click=selector=>{const node=query(selector);assert.ok(node,'Expected actual security control '+selector);node.dispatchEvent(new f.dom.Event('click',{cancelable:true}));};
  return {...f,query,clickSecurity:click,
    async open(){f.password();await tick();assert.equal(f.$('account-security').hidden,false);f.click('account-security');await tick();},
    async setup(){click('[data-action="begin"][data-intent="setup"]');await tick();const input=query('[data-field="currentPassword"]');input.value='synthetic-current-password';input.closest('form').dispatchEvent(new f.dom.Event('submit',{cancelable:true}));await tick();},
    async enable(){const input=query('[data-field="code"]');assert.ok(input);input.value='123456';input.closest('form').dispatchEvent(new f.dom.Event('submit',{cancelable:true}));await tick();},
    displayedCodes:()=>[...f.document.querySelectorAll('[data-recovery-code]')].map(node=>node.textContent)
  };
}
test('actual manual console management reads raw status through the console route and owner',async t=>{
  const f=app(t,[session,prepare,status]);await f.open();
  assert.equal(f.requests[2].path,'/console/api/mfa');assert.equal(f.requests[2].method,'GET');
  assert.ok(f.query('[data-action="begin"][data-intent="setup"]'));assert.equal(f.query('[data-action="begin"][data-intent="disable"]'),null);
  assert.deepEqual(f.displayedCodes(),[]);
});
test('acknowledged enable resets auth while exact once-only result remains until saved acknowledgement',async t=>{
  const f=app(t,[session,prepare,status,candidate(),enabled]);await f.open();await f.setup();await f.enable();
  assert.deepEqual(f.requests.map(row=>row.path),['/console/api/login','/console/api/desktops/pve-170/connect','/console/api/mfa','/console/api/mfa/setup','/console/api/mfa/enable']);
  assert.equal(f.requests[3].headers['X-Multitap-Console-CSRF'],csrf);assert.equal(f.requests[4].headers['X-Multitap-Console-CSRF'],csrf);
  assert.deepEqual(f.requests[4].body,{setupToken:'E'.repeat(43),code:'123456'});
  assert.equal(f.$('logout').hidden,true);assert.equal(f.$('account-security').hidden,true);assert.equal(f.$('login-panel').hidden,false);
  assert.deepEqual(f.displayedCodes(),codes);
  const saved=f.query('[data-saved-codes]');assert.ok(saved);assert.equal(f.query('[data-finish-codes]').disabled,true);
  saved.checked=true;saved.dispatchEvent(new f.dom.Event('change'));f.clickSecurity('[data-finish-codes]');await tick();
  assert.deepEqual(f.displayedCodes(),[]);assert.equal(f.$('login-panel').hidden,false);assert.equal(f.requests.length,5);
});
test('actual manual sign-out intent before enable ACK destroys the owner and never revives late codes',async t=>{
  let finish;
  const f=app(t,[session,prepare,status,candidate(),()=>new Promise(resolve=>finish=resolve),{status:204}]);
  await f.open();await f.setup();await f.enable();assert.equal(typeof finish,'function');
  // Dispatches the actual public sign-out handler. This is DOM lifecycle
  // coverage, not a claim that pointer hit-testing crosses a modal backdrop.
  f.click('logout');await tick();finish(enabled);await tick();
  assert.deepEqual(f.displayedCodes(),[]);assert.equal(f.$('login-panel').hidden,false);
  assert.equal(f.requests.filter(row=>row.path==='/console/api/mfa/enable').length,1);
  assert.equal(f.requests.at(-1).path,'/console/api/logout');
});
test('pagehide before ACK clears setup/result scope and drops a late successful code list',async t=>{
  let finish;const f=app(t,[session,prepare,status,candidate(),()=>new Promise(resolve=>finish=resolve)]);
  await f.open();await f.setup();await f.enable();f.window.dispatchEvent(new Event('pagehide'));finish(enabled);await tick();
  assert.deepEqual(f.displayedCodes(),[]);assert.equal(f.document.querySelector('.account-security-dialog'),null);
  assert.equal(f.requests.length,5);
});
test('uncertain mutation retires locally and cleanup uses captured console CSRF without retrying mutation',async t=>{
  const f=app(t,[session,prepare,status,candidate(),()=>{throw Error('synthetic dropped reply');},{status:403,body:{error:{code:'csrf_required'}}}]);
  await f.open();await f.setup();await f.enable();await tick();
  assert.equal(f.requests.filter(row=>row.path==='/console/api/mfa/enable').length,1);
  assert.equal(f.requests.at(-1).path,'/console/api/logout');assert.equal(f.requests.at(-1).headers['X-Multitap-Console-CSRF'],csrf);
  assert.deepEqual(f.displayedCodes(),[]);assert.match(f.document.querySelector('.account-security-dialog').textContent,/변경 결과를 받지 못했어요/);
  assert.equal(f.$('logout').hidden,true);assert.equal(f.$('login-panel').hidden,false);
});
test('derived console WebView session never gains the self-management entry',async t=>{
  const derived={status:200,body:{...session.body,consoleWebView:{protocol:'console-webview-v1',desktopId:'pve-170',expiresAt:new Date(Date.now()+120000).toISOString()}}};
  const f=app(t,[derived,prepare]);f.password();await tick();assert.equal(f.$('account-security').hidden,true);f.click('account-security');await tick();assert.equal(f.requests.length,2);
});

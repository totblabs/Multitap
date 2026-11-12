import test from 'node:test';
import assert from 'node:assert/strict';
import {fixture,tick,session,prepare} from './console-mfa-fixture.mjs';

test('failed MFA status is explicitly unknown and only manual refresh admits a later raw status',async t=>{
  const f=fixture([session,prepare,{status:503,body:{error:{code:'mfa_unavailable'}}},{status:200,body:{protocol:'mfa-v1',enabled:true,recoveryCodesRemaining:7}}]),priorDocument=globalThis.document;
  globalThis.document=f.document;t.after(()=>{f.cleanup();if(priorDocument===undefined)delete globalThis.document;else globalThis.document=priorDocument;});
  f.password();await tick();assert.equal(f.$('account-security').hidden,false);f.click('account-security');await tick();await tick();
  const panel=f.document.querySelector('.account-security-dialog');assert.ok(panel);
  assert.deepEqual(f.requests.map(row=>row.path),['/console/api/login','/console/api/desktops/pve-170/connect','/console/api/mfa']);
  assert.equal(panel.querySelector('[data-action="begin"]'),null,'unknown status cannot authorize an OFF/on management assumption');
  assert.equal(panel.querySelector('[data-secret-value]'),null);assert.equal(panel.querySelector('[data-recovery-code]'),null);
  assert.doesNotMatch(panel.textContent,/2단계 인증을 사용하지 않고 있어요|상태를 확인하고 있어요/);
  const refresh=panel.querySelector('[data-action="refresh"]');assert.ok(refresh,'an explicitly unknown status must offer manual retry');assert.equal(refresh.disabled,false);
  await tick();assert.equal(f.requests.length,3,'there is no automatic status or mutation retry');
  refresh.dispatchEvent(new f.dom.Event('click',{cancelable:true}));await tick();await tick();
  const current=f.document.querySelector('.account-security-dialog');assert.match(current.textContent,/2단계 인증을 사용하고 있어요/);assert.match(current.textContent,/남은 복구 코드 7개/);
  assert.ok(current.querySelector('[data-action="begin"][data-intent="recovery"]'));
  assert.deepEqual(f.requests.filter(row=>row.path.includes('/mfa')).map(row=>({path:row.path,method:row.method})),[{path:'/console/api/mfa',method:'GET'},{path:'/console/api/mfa',method:'GET'}]);
});

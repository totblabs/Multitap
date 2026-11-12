import test from 'node:test';
import assert from 'node:assert/strict';
import {fixture,tick,session} from './console-mfa-fixture.mjs';

test('persisted page restore resets to usable anonymous login without automatic requests or old-owner effects',async t=>{
  const held=Promise.withResolvers(),f=fixture([()=>held.promise,{status:204}]);t.after(()=>f.cleanup());
  // The reusable fixture omits boot(); expose the ordinary pre-login panel.
  f.$('login-panel').hidden=false;f.password();await tick();assert.equal(f.$('login-submit').disabled,true);
  f.window.dispatchEvent(new Event('pagehide'));
  const restored=new Event('pageshow');Object.defineProperty(restored,'persisted',{value:true});f.window.dispatchEvent(restored);
  const state={loginVisible:!f.$('login-panel').hidden,loginEnabled:!f.$('login-submit').disabled,securityHidden:f.$('account-security').hidden,mfaHidden:f.$('mfa-form').hidden,passwordEmpty:f.$('password').value==='',requestCount:f.requests.length};
  f.$('password').value='synthetic-restored-input';held.resolve(session);await tick();await tick();
  assert.deepEqual(state,{loginVisible:true,loginEnabled:true,securityHidden:true,mfaHidden:true,passwordEmpty:true,requestCount:1});
  assert.equal(f.$('password').value,'synthetic-restored-input');assert.equal(f.$('logout').hidden,true);
  const cleanups=f.requests.filter(row=>row.path==='/console/api/logout');assert.equal(cleanups.length,1);assert.equal(cleanups[0].headers['X-Multitap-Console-CSRF'],session.body.csrfToken);
  assert.equal(f.requests.filter(row=>row.path.endsWith('/connect')).length,0);
});

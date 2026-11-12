import test from 'node:test';
import assert from 'node:assert/strict';
import {fixture,tick,session} from './console-mfa-fixture.mjs';

function assertNoSessionResurrection(f) {
  assert.equal(f.$('logout').hidden,true);
  assert.equal(f.$('account-security').hidden,true);
  assert.equal(f.requests.filter(row=>row.path.endsWith('/connect')).length,0);
}
function assertScopedCleanup(f) {
  const cleanup=f.requests.filter(row=>row.path==='/console/api/logout');
  assert.equal(cleanup.length,1,'a retired successful password-stage response needs its own scoped cookie cleanup');
  assert.equal(cleanup[0].headers['X-Multitap-Console-CSRF'],session.body.csrfToken);
  assert.equal(cleanup[0].credentials,'same-origin');
  assert.equal(cleanup[0].method,'POST');
  assert.deepEqual(cleanup[0].body,{});
}

test('retired password200 cannot revive console UI and cleans only its response-CSRF cookie',async t=>{
  const held=Promise.withResolvers();
  const f=fixture([()=>held.promise,{status:204}]);t.after(()=>f.cleanup());
  f.password();await tick();f.cleanup();held.resolve(session);await tick();await tick();
  assertNoSessionResurrection(f);assertScopedCleanup(f);
});

test('retired password-confirm200 cannot revive console UI and cleans only its response-CSRF cookie',async t=>{
  const held=Promise.withResolvers(),question={status:409,body:{error:{code:'login_confirmation_required',details:{protocol:'session-access-v1',challengeToken:'C'.repeat(43),expiresInMs:120000}}}};
  const f=fixture([question,()=>held.promise,{status:204}]);t.after(()=>f.cleanup());
  f.password();await tick();assert.equal(f.$('login-confirm-dialog').open,true);
  f.click('login-confirm-accept');await tick();assert.equal(f.requests.at(-1).path,'/console/api/login/confirm');
  f.cleanup();held.resolve(session);await tick();await tick();
  assertNoSessionResurrection(f);assertScopedCleanup(f);
});

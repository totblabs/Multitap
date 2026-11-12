import test from 'node:test';
import assert from 'node:assert/strict';
import {fixture,tick,session} from './console-mfa-fixture.mjs';

test('retired password request cannot erase a later edit in the restored document',async t=>{
  const held=Promise.withResolvers(),f=fixture([()=>held.promise,{status:204}]);t.after(()=>f.cleanup());
  f.password();await tick();
  f.window.dispatchEvent(new Event('pagehide'));
  f.window.dispatchEvent(new Event('pageshow'));
  // Public DOM lifecycle/edit coverage; not actual Edge BFCache admission,
  // pointer hit-testing, or an assertion that disabled submit can be clicked.
  f.$('password').value='synthetic-restored-document-edit';
  held.resolve(session);await tick();await tick();
  assert.equal(f.$('password').value,'synthetic-restored-document-edit','old request completion must not erase a later document edit');
  assert.equal(f.$('logout').hidden,true);assert.equal(f.$('account-security').hidden,true);
});

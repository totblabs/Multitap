import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { loadAdmin } from './admin-ui-harness.mjs';

// Codes that come from outside (a page address, server, Host or client
// values) are looked up only among a table's own entries; "constructor" or
// "__proto__" never reach Object.prototype, and an on* attribute is a function.
test('label lookups ignore Object.prototype names', async () => {
  const { own, eventLabel, displayReason, activationReason, usbErrnoLabel } = await import('../admin/js/labels.js');
  for (const name of ['constructor', '__proto__', 'toString', 'hasOwnProperty']) {
    assert.equal(own({ a: 1 }, name), undefined, name);
    assert.equal(eventLabel(name), name); assert.equal(displayReason(name), name);
    assert.equal(typeof activationReason(name), 'string'); assert.equal(usbErrnoLabel(name), 'errno ' + name);
  }
  assert.equal(own({ a: 1 }, 'a'), 1); assert.equal(own({ 2: 'two' }, 2), 'two'); assert.equal(own({ a: 1 }, { toString: () => 'a' }), undefined);
  assert.equal(eventLabel('login_failed'), '로그인 실패');
});

test('el() refuses an on* attribute that is not a function', async t => {
  const { document, Node } = parseHTML('<html><body></body></html>');
  const previous = { document: globalThis.document, Node: globalThis.Node };
  Object.assign(globalThis, { document, Node }); t.after(() => Object.assign(globalThis, previous));
  const { el } = await import('../admin/js/dom.js');
  assert.throws(() => el('button', { onclick: 'alert(1)' }), TypeError);
  assert.throws(() => el('a', { onmouseover: 1 }), TypeError);
  let clicks = 0; const button = el('button', { onclick: () => clicks++, onfocus: null, onblur: undefined });
  button.dispatchEvent(new (parseHTML('').Event)('click')); assert.equal(clicks, 1);
  assert.equal(button.getAttribute('onclick'), null);
});

test('#/constructor and #/__proto__ open the home page instead of breaking the console', async t => {
  const user = { id: 'owner', username: 'owner', role: 'admin', isPrimaryAdmin: true, mustChangeCredentials: false };
  const inventory = { users: [{ id: user.id, username: user.username, notes: '', role: 'admin', disabled: 0, is_primary_admin: 1 }], desktops: [], assignments: [], audit: [], proxmox: { configured: true } };
  for (const hash of ['#/constructor', '#/__proto__', '#/toString']) {
    const f = await loadAdmin(t, 'prototype-route' + hash.slice(2), user, inventory, { hash });
    await f.settle();
    assert.ok(f.content.querySelector('h1'), hash + ' renders a page'); assert.equal(f.crumb(), '홈', hash);
  }
});

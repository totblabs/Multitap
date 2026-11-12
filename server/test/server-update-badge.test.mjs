import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { createServerUpdateBadge } from '../admin/js/server-update-badge.js';

const indicator = '<button type="button" data-server-update-badge hidden><span data-icon="download"><svg></svg></span><span class="update-indicator-label"></span></button>';
function fixture() {
  const window = parseHTML(`<html><body><a href="#/server"><span data-nav-badge="server" hidden></span></a>${indicator}<span data-nav-badge="hosts">2</span></body></html>`);
  const root = window.document, badge = createServerUpdateBadge({ root });
  return { root, badge, nav: () => root.querySelector('[data-nav-badge="server"]'), header: () => root.querySelector('[data-server-update-badge]') };
}
const info = () => ({ service: 'multitap-broker', version: '0.5.70', checkedAt: '2026-10-28T01:00:00.000Z', update: { state: 'available', feedConfigured: true, applySupported: true, currentVersion: '0.5.70', latest: { version: '0.5.71' }, releaseId: 'a'.repeat(64) } });

test('verified newer metadata badges both existing server links without changing navigation or Host badges', () => {
  const f = fixture(); assert.equal(f.nav().hidden, true);
  assert.equal(f.badge.update(info(), f.badge.begin()), true);
  assert.equal(f.nav().textContent, '1'); assert.equal(f.header().textContent, '업데이트 0.5.71');
  assert.ok(f.header().querySelector('[data-icon] svg'), 'the header indicator keeps its icon (all a narrow bar shows)');
  for (const node of [f.nav(), f.header()]) { assert.equal(node.hidden, false); assert.equal(node.getAttribute('aria-label'), '서버 0.5.71 업데이트 가능'); assert.equal(node.getAttribute('title'), '서버 0.5.71 업데이트 가능'); }
  assert.equal(f.nav().parentElement.getAttribute('href'), '#/server');
  assert.equal(f.root.querySelector('[data-nav-badge="hosts"]').textContent, '2');
  f.root.body.innerHTML = '<span data-nav-badge="server" hidden></span>' + indicator;
  f.badge.render(); assert.equal(f.nav().hidden, false); assert.equal(f.header().textContent, '업데이트 0.5.71');
});

test('unavailable, unverified, current, older and malformed metadata cannot claim an update', () => {
  for (const mutate of [v => v.update.state = 'current', v => v.update.state = 'checking', v => v.update.state = 'failed', v => v.update.state = 'unavailable', v => v.update.state = 'recovery_required', v => v.update.feedConfigured = false, v => delete v.update.releaseId, v => v.update.releaseId = 'unverified', v => v.update.latest.version = '0.5.70', v => v.update.latest.version = '0.5.9', v => v.update.latest.version = '0.05.71', v => v.update.latest.version = 'unknown', v => v.update.latest.version = null, v => v.version = 'unknown', v => v.update.currentVersion = '0.5.69', v => v.service = 'foreign', v => v.checkedAt = 'unknown']) {
    const f = fixture(), value = info();
    f.badge.update(info(), f.badge.begin()); mutate(value); f.badge.update(value, f.badge.begin());
    assert.equal(f.nav().hidden, true); assert.equal(f.header().hidden, true); assert.equal(f.header().getAttribute('title'), null);
  }
  const f = fixture(); f.badge.update({ ...info(), version: '0.9.99', update: { ...info().update, currentVersion: '0.9.99', latest: { version: '0.10.0' } } }, f.badge.begin()); assert.equal(f.header().hidden, false, 'version comparison is numeric');
});

test('late responses, stale errors and logout responses cannot replace the current badge', () => {
  const f = fixture(), old = f.badge.begin(), current = f.badge.begin();
  const value = info(); value.update.latest.version = '0.5.72';
  f.badge.update(value, current); assert.equal(f.badge.update(info(), old), false); assert.equal(f.badge.fail(old), false);
  assert.equal(f.header().getAttribute('aria-label'), '서버 0.5.72 업데이트 가능');
  f.badge.clear(); assert.equal(f.badge.update(value, current), false); assert.equal(f.header().hidden, true);
  const nextUser = f.badge.begin(); assert.equal(f.badge.update(info(), current), false); f.badge.update(info(), nextUser); assert.equal(f.nav().hidden, false);
  assert.equal(f.badge.fail(nextUser), true); assert.equal(f.nav().hidden, true); assert.equal(f.header().textContent, '');
});

test('an older response snapshot is ignored while a newly reported rollback and a new login remain valid', () => {
  const f = fixture(), current = info(); current.checkedAt = '2026-10-28T01:10:00.000Z'; current.update.latest.version = '0.5.72';
  f.badge.update(current, f.badge.begin());
  assert.equal(f.badge.update(info(), f.badge.begin()), false, 'a stale snapshot cannot regress an already reported release');
  assert.equal(f.header().getAttribute('title'), '서버 0.5.72 업데이트 가능');
  const rollback = info(); rollback.checkedAt = '2026-10-28T01:11:00.000Z'; rollback.version = '0.5.69'; rollback.update.state = 'rolled_back'; rollback.update.currentVersion = '0.5.69';
  assert.equal(f.badge.update(rollback, f.badge.begin()), true, 'a real rollback has a new observation time'); assert.equal(f.header().hidden, true);
  f.badge.clear(); assert.equal(f.badge.update(info(), f.badge.begin()), true, 'logout also clears the observation boundary'); assert.equal(f.header().hidden, false);
});

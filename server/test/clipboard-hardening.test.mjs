import { test } from 'node:test';
import assert from 'node:assert/strict';
import { safeName, FairByteBudget } from '../src/clipboard.mjs';

test('safeName mirrors the native safeLeaf: reserved device names with trailing dots/spaces and superscript digits', () => {
  for (const name of ['CON', 'con', 'Con.txt', 'CON .txt', 'CON. .txt', 'NUL  .a.b', 'aux.', 'PRN ', 'COM1', 'com1.txt', 'COM1 .txt', 'LPT9.x', 'COM¹', 'LPT²', 'com³.txt', 'LPT³ .txt', 'CONIN$', 'CONOUT$ .x', '', '.', '..', 'a:b', 'a/b', 'a\\b', 'a*b', 'a?', 'a|b', 'a"b', 'a<b', 'tab\tname', 'x'.repeat(241), 'ends.', 'ends ']) {
    assert.equal(safeName(name), false, JSON.stringify(name));
  }
  for (const name of ['COM0', 'COM10', 'COMX', 'LPT0', 'CONSOLE', 'CON-1', 'a.CON', 'readme.txt', '한글.txt', 'x'.repeat(240), 'a b', '.hidden']) {
    assert.equal(safeName(name), true, JSON.stringify(name));
  }
  assert.equal(safeName('lone\ud800'), false);
});

test('The shared clipboard byte budget ends the heaviest pair, not an innocent one that crossed the line', () => {
  const budget = new FairByteBudget(100, 1000);
  assert.deepEqual(budget.charge('heavy', 90, 0), []);
  assert.deepEqual(budget.charge('light', 5, 1), []);
  // The light pair's next message pushes the total over: the heavy pair is the one closed.
  assert.deepEqual(budget.charge('light', 10, 2), ['heavy']);
  assert.deepEqual(budget.charge('light', 10, 3), [], 'the heavy pair\'s bytes left the account');
  // The sender that is itself the heaviest is the one closed.
  const alone = new FairByteBudget(100, 1000);
  assert.deepEqual(alone.charge('a', 60, 0), []);
  assert.deepEqual(alone.charge('b', 30, 0), []);
  assert.deepEqual(alone.charge('a', 20, 0), ['a']);
  // Several may have to go until the total fits.
  const many = new FairByteBudget(100, 1000);
  many.charge('a', 50, 0); many.charge('b', 40, 0);
  assert.deepEqual(many.charge('c', 200, 0), ['c']);
  const two = new FairByteBudget(100, 1000);
  two.charge('a', 40, 0); two.charge('b', 30, 0);
  assert.deepEqual(two.charge('c', 40, 0), ['a'], 'one closure brings the total back within the budget; the newcomer stays');
  // A new window forgets the previous one.
  const windows = new FairByteBudget(100, 1000);
  windows.charge('a', 99, 0);
  assert.deepEqual(windows.charge('b', 99, 1000), []);
});

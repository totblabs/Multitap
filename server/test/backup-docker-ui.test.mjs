// 백업·복원 on a Docker install (restore.state 'docker-command', server/src/backup.mjs): the console checks
// the file and names the command that restores it on the server; it never restores itself. The other
// unsupported state keeps its old note. Scripted server answers (admin-ui-harness.mjs).
import test from 'node:test';
import assert from 'node:assert/strict';
import { loadAdmin, ownerFixture } from './admin-ui-harness.mjs';
import { messages } from '../admin/js/labels.js';

const answer = (value, status = 200) => ({ ok: status < 400, status, json: async () => value });
const status = ({ restore, last = null, setup = false }) => ({ protocol: 'backup-v1', export: { available: true, passphraseMinimum: 12, maxDatabaseBytes: 268435456, ...(setup ? {} : { last: null }) },
  restore, upload: null, ...(setup ? {} : { last }), reauthentication: setup ? { setupCode: true, mfa: false } : { setupCode: false, mfa: false }, ...(setup ? {} : { connections: { people: 0, sessions: 0, consoles: 0 } }) });
const docker = { supported: false, state: 'docker-command', command: 'sudo multitap restore', chunkBytes: 1000, maxFileBytes: 300000000 };
const respond = value => async url => url === '/api/v1/admin/backup' ? answer(value) : undefined;
const open = async (t, name, value, { user, hash = '#/backup' } = {}) => {
  const fixture = ownerFixture(), f = await loadAdmin(t, 'backup-docker-' + name, user ?? fixture.user, fixture.inventory, { hash, respond: respond(value) });
  await f.settle(); return f;
};

test('a Docker install names the server command with a copy button and the two-line help', async t => {
  const f = await open(t, 'page', status({ restore: docker })), restore = f.content.querySelector('.backup-restore');
  assert.equal(restore.querySelector('.backup-docker .note').textContent, '이 서버(Docker 설치)는 서버에서 명령으로 복원해요. 여기서는 파일 확인까지 할 수 있어요.');
  assert.doesNotMatch(restore.textContent, /이 설치 방식에서는 관리자 페이지에서 복원할 수 없어요/);
  assert.deepEqual([...restore.querySelectorAll('.backup-docker .shell-command code')].map(node => node.textContent), ['sudo multitap restore <백업 파일>']);
  assert.ok([...restore.querySelectorAll('.backup-docker button')].some(node => node.textContent === '복사'), 'a copy button beside it');
  assert.equal(restore.querySelector('.backup-docker .field-hint').textContent, '백업 파일을 서버로 옮긴 뒤 위 명령을 실행하세요. 백업 암호를 묻고, 내용을 보여 준 뒤 복원할지 물어요.\n복원 전 데이터는 /opt/multitap/backups에 안전 백업으로 남아요.');
  // The file can still be checked here.
  assert.ok(restore.querySelector('form.backup-choose input[type=file]'));
});

test('the restore button of a checked file says where a Docker install restores', async t => {
  const preview = { createdAt: '2026-11-09T03:10:00.000Z', serverVersion: '1.0.43', origin: 'https://cloud.fixture.example', counts: { accounts: 1, admins: 1, desktops: 0, hosts: 0, assignments: 0, adminNames: ['chief'] },
    proxmox: { configured: false, readable: true }, unreadableSecrets: 0, schemaVersion: 19, migrated: false, ignoredObjects: 0, access: { available: false, differs: false } };
  const value = { ...status({ restore: docker }), upload: { uploadId: 'u'.repeat(32), state: 'inspected', preview } };
  const f = await open(t, 'apply', value);
  const apply = [...f.content.querySelectorAll('button')].find(node => node.textContent === '이 백업으로 복원');
  assert.equal(apply.disabled, true); assert.equal(apply.title, 'Docker 설치는 서버에서 sudo multitap restore로 복원해요.');
  const other = await open(t, 'apply-other', { ...status({ restore: { supported: false, state: 'unavailable', chunkBytes: 1000, maxFileBytes: 300000000 } }), upload: value.upload });
  const plain = [...other.content.querySelectorAll('button')].find(node => node.textContent === '이 백업으로 복원');
  assert.equal(plain.title, messages.restore_unavailable, 'another install keeps the general reason');
  assert.match(other.content.textContent, /이 설치 방식에서는 관리자 페이지에서 복원할 수 없어요\. 파일 확인까지는 할 수 있어요\./);
  assert.equal(other.content.querySelector('.backup-docker'), null);
});

test('a restore made by the server command is labelled as such', async t => {
  const last = { operationId: '0a0a0a0a-1234-4234-8234-123456789abc', restoredAt: '2026-11-10T08:00:00.000Z', backup: { createdAt: '2026-11-09T03:10:00.000Z', serverVersion: '1.0.43', origin: 'https://old.fixture.example' },
    actor: 'multitap restore', proxmox: { state: 'ok' }, access: { state: 'kept' } };
  const f = await open(t, 'last', status({ restore: docker, last }));
  const rows = Object.fromEntries([...f.content.querySelectorAll('.backup-last .facts > div')].map(row => [row.querySelector('dt').textContent, row.querySelector('dd').textContent]));
  assert.match(rows['복원한 때'], /서버 명령\(multitap restore\)/); assert.doesNotMatch(rows['복원한 때'], /계정이 복원했어요/);
});

test('the first setup restore of a fresh Docker server shows the same note and command', async t => {
  const user = { id: 'fixture-initial', username: 'admin', role: 'admin', mustChangeCredentials: true };
  const f = await open(t, 'setup', status({ restore: docker, setup: true }), { user, hash: '' });
  document.querySelector('#setup-restore-open').dispatchEvent(new f.window.Event('click')); await f.settle();
  const card = document.querySelector('#setup-restore');
  assert.equal(card.querySelector('.backup-docker .note').textContent, '이 서버(Docker 설치)는 서버에서 명령으로 복원해요. 여기서는 파일 확인까지 할 수 있어요.');
  assert.deepEqual([...card.querySelectorAll('.backup-docker .shell-command code')].map(node => node.textContent), ['sudo multitap restore <백업 파일>']);
  assert.ok(card.querySelector('form.backup-choose'), 'the file can still be checked');
});

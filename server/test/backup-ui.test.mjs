// 백업·복원 in the administrator console: the page states and the first setup
// screen. DOM behaviour against a scripted server (admin-ui-harness.mjs); not
// a browser layout test and not the real broker (backup-http.test.mjs is).
import test from 'node:test';
import assert from 'node:assert/strict';
import { loadAdmin, ownerFixture } from './admin-ui-harness.mjs';
import { messages, eventNames } from '../admin/js/labels.js';
import { restoreErrors, blockerText } from '../admin/js/backup.js';

const answer = (value, status = 200) => ({ ok: status < 400, status, json: async () => value });
const refuse = (code, status = 422) => answer({ error: { code } }, status);
const preview = (change = {}) => ({ createdAt: '2026-11-09T03:10:00.000Z', serverVersion: '1.0.43', origin: 'https://cloud.fixture.example', counts: { accounts: 12, admins: 2, desktops: 9, hosts: 8, assignments: 21, adminNames: ['chief', '부관리자'] },
  proxmox: { configured: true, readable: true, endpoint: 'https://pve.fixture.example:8006' }, unreadableSecrets: 0, schemaVersion: 19, migrated: false, ignoredObjects: 0, access: { available: true, differs: false, primaryOrigin: 'https://cloud.fixture.example' }, expiresAt: '2026-11-10T09:00:00.000Z', ...change });
// The scripted server: what GET /backup says, and what each write answers.
function server(options = {}) {
  const s = { supported: true, mfa: false, setup: false, last: null, lastExport: null, restore: { state: 'idle' }, preview: preview(), passphrase: '맞는 백업 암호 열두 글자', gone: false, uploads: 0, chunks: [], ...options };
  s.status = () => ({ protocol: 'backup-v1', export: { available: true, passphraseMinimum: 12, maxDatabaseBytes: 268435456, ...(s.setup ? {} : { last: s.lastExport }) },
    restore: { supported: s.supported, chunkBytes: 1000, maxFileBytes: 300000000, ...s.restore, ...(s.supported ? {} : { state: 'unavailable' }) }, upload: null, ...(s.setup ? {} : { last: s.last }),
    reauthentication: s.setup ? { setupCode: true, mfa: false } : { setupCode: false, mfa: s.mfa }, ...(s.setup ? {} : { connections: s.connections ?? { people: 0, sessions: 0, consoles: 0 } }) });
  s.respond = async (url, request = {}) => {
    const path = url.replace('/api/v1/admin', ''), method = request.method ?? 'GET', body = request.body ? JSON.parse(request.body) : null;
    if (!path.startsWith('/backup')) return undefined;
    if (s.gone) return refuse('authentication_required', 401);
    if (path === '/backup' && method === 'GET') return answer(s.status());
    if (path === '/backup/export') return s.exportError ? refuse(s.exportError, 403) : answer({ download: { id: 'd'.repeat(32), size: 2411724, filename: 'multitap-backup-20261110-0730.mtbk', createdAt: '2026-11-10T07:30:00.000Z', expiresAt: new Date(Date.now() + 600000).toISOString(), counts: s.preview.counts } }, 201);
    if (path === '/backup/restore/upload') { s.uploads++; return s.uploadError ? refuse(s.uploadError, 403) : answer({ uploadId: 'u'.repeat(32), chunkBytes: 1000, size: body.size }, 201); }
    if (path === '/backup/restore/upload/' + 'u'.repeat(32) && method === 'PUT') { s.chunks.push(body); const received = body.offset + Buffer.from(body.data, 'base64').length; return answer({ received, complete: false }); }
    if (path === '/backup/restore/upload/' + 'u'.repeat(32) && method === 'DELETE') return answer({ ok: true });
    if (path === '/backup/restore/inspect') return body.passphrase === s.passphrase ? answer({ uploadId: body.uploadId, preview: s.preview }) : refuse('backup_unreadable');
    if (path === '/backup/restore/apply') { if (s.applyError) return refuse(s.applyError, 403); s.restore = { state: 'waiting_idle', operationId: '0a0a0a0a-1234-4234-8234-123456789abc', busy: true }; return answer({ accepted: true, operationId: s.restore.operationId, state: 'waiting_idle' }, 202); }
    return refuse('not_found', 404);
  };
  return s;
}
async function open(t, name, s, { user, inventory, hash = '#/backup' } = {}) {
  const fixture = ownerFixture(), f = await loadAdmin(t, 'backup-' + name, user ?? fixture.user, inventory ?? fixture.inventory, { hash, respond: s.respond });
  await f.settle();
  const text = () => f.content.textContent, panel = cls => f.content.querySelector('.' + cls);
  const submitModal = async () => { await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle(); };
  const field = name => [...document.querySelectorAll('#modal-fields [name]')].find(node => node.name === name);
  const noticeText = () => document.querySelector('#notice .notice-text')?.textContent ?? '';
  const backupWrites = () => f.writes.filter(write => write.path.includes('/backup'));
  const pick = (scope, bytes = Buffer.alloc(2500, 3), fileName = '백업.mtbk') => {
    const input = scope.querySelector('input[type=file]'), file = new File([bytes], fileName, { lastModified: 1794300000000 });
    Object.defineProperty(input, 'files', { configurable: true, value: [file] }); input.dispatchEvent(new f.window.Event('change')); return file;
  };
  const check = async (scope, passphrase) => {
    scope.querySelector('input[name=passphrase]').value = passphrase;
    const form = scope.querySelector('form.backup-choose'), event = new f.window.Event('submit', { cancelable: true });
    form.dispatchEvent(event); for (let i = 0; i < 8; i++) await f.settle();
  };
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  // Typed into a field: the value property, which a form reset may clear again.
  const type = (node, value) => { node.value = value; };
  return { ...f, text, panel, submitModal, field, type, noticeText, backupWrites, pick, check, wait };
}

test('백업·복원 is a page of the 설정 group and says what a backup holds and what a restore replaces', async t => {
  const s = server({ lastExport: { at: '2026-11-08T01:00:00.000Z', size: 2411724, actor: 'chief' } }), f = await open(t, 'page', s);
  assert.equal(document.querySelector('#sidebar [data-page=backup] span:not([data-icon])').textContent, '백업·복원');
  assert.equal(document.querySelector('#sidebar [data-page=backup]').closest('.nav-group').querySelector('.nav-heading').textContent, '설정');
  assert.equal(f.crumb(), '백업·복원'); assert.equal(document.querySelector('#crumbs .crumb-group').textContent, '설정');
  assert.equal(f.content.querySelector('h1').textContent, '백업·복원');
  const made = f.panel('backup-export'), restore = f.panel('backup-restore');
  assert.equal(made.querySelector('h2').textContent, '백업 파일 만들기');
  assert.match(made.textContent, /계정과 비밀번호, 배정과 공유 설정, Host 등록, Proxmox 연결, 사용 기록이 들어가요\./);
  assert.match(made.textContent, /암호를 잊으면 복원할 수 없어요\./);
  assert.match(made.textContent, /마지막으로 만든 때/); assert.match(made.textContent, /chief · 2\.3 MB/);
  // The warning about what a restore does is on the page itself, before any file is chosen.
  const warning = restore.querySelector('.inline-alert.is-warn');
  assert.equal(warning.querySelector('strong').textContent, '복원하면 지금 데이터가 백업의 것으로 바뀌어요');
  assert.match(warning.textContent, /접속 중인 사람은 끊겨요/);
  assert.equal(restore.querySelector('input[type=file]').getAttribute('accept'), '.mtbk');
  // The browser's own file button would speak the browser's language; the console draws its own.
  assert.ok(restore.querySelector('input[type=file]').classList.contains('visually-hidden'));
  assert.equal(restore.querySelector('.backup-file-pick .button').textContent, '파일 고르기'); assert.equal(restore.querySelector('.backup-file-name').textContent, '고른 파일 없음');
  assert.equal(restore.querySelector('input[name=passphrase]').getAttribute('type'), 'password');
  assert.equal(f.panel('backup-last'), null, 'no restore has happened on this server');
  // Notices are short: no paragraph of the page runs past three lines.
  for (const node of f.content.querySelectorAll('.field-hint,.inline-alert span,.note span')) assert.ok(node.textContent.split('\n').length <= 3, node.textContent);
  assert.doesNotMatch(f.text(), /이 PC|VM/);
});

test('making a file asks for the backup passphrase twice and the account password, then offers the download', async t => {
  const s = server(), f = await open(t, 'export', s);
  f.click(f.content, '백업 파일 만들기');
  assert.equal(document.querySelector('#modal-title').textContent, '백업 파일 만들기'); assert.equal(f.dialog.open, true);
  assert.deepEqual([...document.querySelectorAll('#modal-fields label')].map(node => node.firstChild.textContent), ['백업 암호', '백업 암호 확인', '내 계정 비밀번호']);
  assert.match(document.querySelector('#modal-fields').textContent, /12자 이상이에요\. 이 암호가 없으면 복원할 수 없어요\./);
  for (const name of ['passphrase', 'passphraseConfirmation', 'password']) assert.equal(f.field(name).getAttribute('type'), 'password');
  assert.equal(f.field('passphrase').getAttribute('minlength'), '12');
  f.type(f.field('passphrase'), '열두 글자가 넘는 백업 암호'); f.type(f.field('passphraseConfirmation'), '다르게 친 백업 암호 열두자'); f.type(f.field('password'), 'account-password');
  await f.submitModal();
  assert.equal(document.querySelector('#modal-error').textContent, '백업 암호와 확인이 달라요.'); assert.deepEqual(f.backupWrites(), []);
  f.type(f.field('passphraseConfirmation'), '열두 글자가 넘는 백업 암호');
  await f.submitModal();
  assert.deepEqual(f.backupWrites(), [{ path: '/api/v1/admin/backup/export', data: { passphrase: '열두 글자가 넘는 백업 암호', passphraseConfirmation: '열두 글자가 넘는 백업 암호', password: 'account-password' } }]);
  assert.equal(f.dialog.open, false, document.querySelector('#modal-error').textContent);
  assert.equal(f.noticeText(), '백업 파일을 만들었어요.\n내려받기가 시작돼요.'); assert.equal(document.querySelector('#notice').className, 'is-success');
  const link = f.panel('backup-export').querySelector('.backup-download a');
  assert.equal(link.getAttribute('href'), '/api/v1/admin/backup/export/' + 'd'.repeat(32)); assert.equal(link.getAttribute('download'), 'multitap-backup-20261110-0730.mtbk');
  assert.match(f.panel('backup-export').textContent, /2\.3 MB · 10분 동안 내려받을 수 있어요\./);
  // A refusal stays in the dialog, in words.
  s.exportError = 'current_password_incorrect';
  f.click(f.content, '백업 파일 만들기');
  f.type(f.field('passphrase'), '열두 글자가 넘는 백업 암호'); f.type(f.field('passphraseConfirmation'), '열두 글자가 넘는 백업 암호'); f.type(f.field('password'), 'wrong');
  await f.submitModal();
  assert.equal(document.querySelector('#modal-error').textContent, '내 계정 비밀번호가 맞지 않아요.'); assert.equal(f.dialog.open, true);
});

test('an account with two-step sign-in is asked for its code as well', async t => {
  const s = server({ mfa: true }), f = await open(t, 'export-mfa', s);
  f.click(f.content, '백업 파일 만들기');
  assert.deepEqual([...document.querySelectorAll('#modal-fields label')].map(node => node.firstChild.textContent), ['백업 암호', '백업 암호 확인', '내 계정 비밀번호', '인증 앱 코드']);
  f.type(f.field('passphrase'), '열두 글자가 넘는 백업 암호'); f.type(f.field('passphraseConfirmation'), '열두 글자가 넘는 백업 암호'); f.type(f.field('password'), 'account-password'); f.type(f.field('code'), '123456');
  await f.submitModal();
  assert.equal(f.backupWrites()[0].data.code, '123456');
});

test('checking a file uploads it in pieces, shows what is inside, and changes nothing yet', async t => {
  const s = server({ connections: { people: 3, sessions: 3, consoles: 1 } }), f = await open(t, 'inspect', s), restore = () => f.panel('backup-restore');
  const bytes = Buffer.from(Array.from({ length: 2500 }, (_, index) => index % 251));
  f.pick(restore(), bytes);
  // A wrong passphrase: said on the page, and the upload is kept for the next try.
  await f.check(restore(), '틀린 백업 암호 열두 글자쯤');
  assert.equal(restore().querySelector('.error').textContent, '백업 암호가 다르거나 파일이 손상됐어요.');
  // The file stays chosen (said, since the browser's own field is drawn anew), and the passphrase has the focus.
  assert.equal(restore().querySelector('[data-backup-file-kept]').textContent, '고른 파일은 그대로예요. 백업 암호만 다시 넣으세요.');
  assert.equal(restore().querySelector('.backup-file-name').textContent, '백업.mtbk · 2 KB');
  assert.equal(f.focused(), restore().querySelector('input[name=passphrase]'));
  assert.deepEqual(s.chunks.map(chunk => chunk.offset), [0, 1000, 2000]);
  assert.deepEqual(Buffer.concat(s.chunks.map(chunk => Buffer.from(chunk.data, 'base64'))), bytes, 'the pieces are the file');
  assert.equal(restore().querySelector('.backup-preview'), null);
  await f.check(restore(), s.passphrase);
  assert.equal(s.uploads, 1, 'the same file is not uploaded again'); assert.equal(s.chunks.length, 3);
  const shown = restore().querySelector('.backup-preview');
  assert.equal(shown.querySelector('h3').textContent, '이 백업에 들어 있는 것');
  const rows = Object.fromEntries([...shown.querySelectorAll('.facts > div')].map(row => [row.querySelector('dt').textContent, row.querySelector('dd').textContent]));
  assert.deepEqual(Object.keys(rows), ['만든 때', '만든 서버', '계정', '데스크톱', 'Host 등록', '배정', 'Proxmox 연결']);
  assert.equal(rows['만든 서버'], 'https://cloud.fixture.example버전 1.0.43'); assert.equal(rows['계정'], '12개관리자: chief, 부관리자');
  assert.equal(rows['데스크톱'], '9대'); assert.equal(rows['Host 등록'], '8개'); assert.equal(rows['배정'], '21개'); assert.equal(rows['Proxmox 연결'], 'https://pve.fixture.example:8006');
  // Same address as this console: Hosts come back by themselves; the access settings stay.
  assert.match(shown.textContent, /등록된 Host는 서버 주소가 그대로면 스스로 다시 연결돼요\.\n주소를 바꾸면 Host마다 새 주소로 바꿔야 해요\./);
  assert.match(shown.textContent, /서버 접속 설정은 지금 것을 그대로 써요\./); assert.equal(shown.querySelector('input[type=checkbox]'), null);
  assert.deepEqual(f.backupWrites().map(write => write.path.replace('/api/v1/admin/backup', '')).filter(path => !path.includes('/upload')), ['/restore/inspect', '/restore/inspect'], 'nothing but the upload and the check was sent');
  // The confirmation names what is replaced, who is cut off, and which accounts sign in afterwards.
  f.click(shown, '이 백업으로 복원');
  assert.equal(document.querySelector('#modal-title').textContent, '백업으로 복원');
  assert.equal(document.querySelector('.modal-description').textContent, '지금 서버의 계정·배정·Host 등록·Proxmox 연결이 백업의 것으로 바뀌어요.\n접속 중인 3명은 모두 끊기고 서버가 다시 시작돼요.\n복원 뒤에는 백업에 있던 계정과 비밀번호로 로그인해요 (관리자: chief, 부관리자).');
  assert.equal(document.querySelector('#modal-save').textContent, '복원'); assert.equal(document.querySelector('#modal-save').className, 'danger');
  assert.match(document.querySelector('#modal-fields').textContent, /지금 상태는 자동으로 보관해요\. 서버가 정상으로 뜨지 않으면 되돌려요\./);
  document.querySelector('#modal-cancel').dispatchEvent(new f.window.Event('click'));
  assert.equal(f.backupWrites().some(write => write.path.endsWith('/apply')), false);
  // Another file: the checked one is forgotten on the server too.
  f.click(restore(), '다른 파일 고르기'); await f.settle();
  assert.deepEqual(f.backupWrites().at(-1), { path: '/api/v1/admin/backup/restore/upload/' + 'u'.repeat(32), data: null });
  assert.ok(restore().querySelector('input[type=file]'));
});

test('a backup from another address warns about Hosts and offers its access settings, unchecked', async t => {
  const s = server({ preview: preview({ origin: 'https://old.fixture.example', access: { available: true, differs: true, primaryOrigin: 'https://old.fixture.example' }, migrated: true, proxmox: { configured: true, readable: false, endpoint: null }, unreadableSecrets: 2 }) });
  const f = await open(t, 'elsewhere', s), restore = () => f.panel('backup-restore');
  f.pick(restore()); await f.check(restore(), s.passphrase);
  const shown = restore().querySelector('.backup-preview'), alerts = [...shown.querySelectorAll('.inline-alert.is-warn')].map(node => node.textContent);
  assert.ok(alerts.some(text => text.startsWith('다른 주소에서 만든 백업이에요') && text.includes('등록된 Host는 https://old.fixture.example 로 다시 연결해요.') && text.includes('Host마다 새 주소로 바꿔야 해요.')));
  assert.ok(alerts.some(text => text.startsWith('암호화된 정보 일부를 읽을 수 없어요')));
  assert.match(shown.textContent, /이전 버전에서 만든 백업이에요\. 이 서버 버전에 맞게 바꿔서 복원해요\./);
  const option = shown.querySelector('.backup-option'), box = option.querySelector('input[type=checkbox]');
  assert.equal(option.querySelector('strong').textContent, '서버 접속 설정도 복원'); assert.notEqual(box.checked, true, 'off unless the administrator turns it on'); assert.equal(box.hasAttribute('checked'), false);
  assert.equal(option.querySelector('small').textContent, '백업한 서버의 주소(https://old.fixture.example)와 인증서 설정으로 바꿔요. 그 주소로 접속되지 않으면 지금 설정으로 되돌려요.');
  f.click(shown, '이 백업으로 복원'); f.type(f.field('password'), 'account-password'); await f.submitModal();
  assert.deepEqual(f.backupWrites().at(-1), { path: '/api/v1/admin/backup/restore/apply', data: { uploadId: 'u'.repeat(32), confirm: true, restoreAccess: false, password: 'account-password' } });
  // The same with the option turned on, on a console of its own.
  const s2 = server({ preview: s.preview }), g = await open(t, 'elsewhere-access', s2), again = () => g.panel('backup-restore');
  g.pick(again()); await g.check(again(), s2.passphrase);
  g.change(again().querySelector('.backup-option input'), true);
  g.click(again(), '이 백업으로 복원'); g.type(g.field('password'), 'account-password'); await g.submitModal();
  assert.equal(g.backupWrites().at(-1).data.restoreAccess, true);
});

test('a restore that worked ends this sign-in: the sign-in page says to use the backup\'s accounts', async t => {
  const s = server(), f = await open(t, 'restored', s), restore = () => f.panel('backup-restore');
  f.pick(restore()); await f.check(restore(), s.passphrase);
  f.click(restore(), '이 백업으로 복원'); f.type(f.field('password'), 'account-password'); await f.submitModal();
  // While it runs the page shows only the progress.
  assert.equal(f.dialog.open, false, document.querySelector('#modal-error').textContent);
  const progress = f.content.querySelector('.backup-progress');
  assert.equal(progress.querySelector('strong').textContent, '복원하고 있어요'); assert.match(progress.textContent, /진행 중인 작업이 끝나기를 기다리고 있어요\./);
  assert.equal(f.panel('backup-export'), null); assert.equal(f.content.querySelector('input[type=file]'), null);
  s.restore = { ...s.restore, state: 'waiting_idle', blockers: [{ code: 'restore_host_updates_busy', count: 1 }] };
  await f.wait(1150); await f.settle();
  assert.match(f.content.querySelector('.backup-progress').textContent, /기다리는 작업: Host 업데이트/);
  s.restore = { ...s.restore, state: 'applying', blockers: [] }; await f.wait(1550); await f.settle();
  assert.match(f.content.querySelector('.backup-progress').textContent, /지금 상태를 보관하고 백업으로 바꾸고 있어요\./);
  // The restored server no longer knows this sign-in.
  s.gone = true; await f.wait(1550); await f.settle();
  assert.equal(document.querySelector('#login').hidden, false); assert.equal(document.querySelector('#console').hidden, true);
  const notice = document.querySelector('#login-notice');
  assert.equal(notice.hidden, false); assert.equal(notice.textContent, '복원했어요.\n백업에 있던 관리자 계정으로 로그인해 주세요.');
});

test('a restore that was rolled back says so in a notice and leaves the page as it was', async t => {
  const s = server(), f = await open(t, 'rolled-back', s), restore = () => f.panel('backup-restore');
  f.pick(restore()); await f.check(restore(), s.passphrase);
  f.click(restore(), '이 백업으로 복원'); f.type(f.field('password'), 'account-password'); await f.submitModal();
  s.restore = { state: 'rolled_back', operationId: s.restore.operationId, errorCode: 'restore_health_failed', busy: false };
  await f.wait(1150); await f.settle();
  assert.equal(f.noticeText(), '복원한 뒤 서버가 정상으로 뜨지 않았어요.\n원래 상태로 되돌렸어요.'); assert.equal(document.querySelector('#notice').className, 'is-error');
  assert.equal(document.querySelector('#console').hidden, false); assert.equal(document.querySelector('#login-notice').hidden, true);
  assert.equal(f.content.querySelector('.backup-progress'), null); assert.ok(f.panel('backup-export')); assert.ok(restore().querySelector('input[type=file]'));
  // A refusal at the last step stays in the confirmation dialog.
  const s2 = server({ applyError: 'current_password_incorrect' }), g = await open(t, 'apply-refused', s2), again = () => g.panel('backup-restore');
  g.pick(again()); await g.check(again(), s2.passphrase);
  g.click(again(), '이 백업으로 복원'); g.type(g.field('password'), 'wrong'); await g.submitModal();
  assert.equal(document.querySelector('#modal-error').textContent, '내 계정 비밀번호가 맞지 않아요.'); assert.equal(g.content.querySelector('.backup-progress'), null);
});

test('a server without the updater can make and check files but not restore from the console', async t => {
  const s = server({ supported: false }), f = await open(t, 'unsupported', s), restore = () => f.panel('backup-restore');
  assert.match(restore().textContent, /이 설치 방식에서는 관리자 페이지에서 복원할 수 없어요\. 파일 확인까지는 할 수 있어요\./);
  assert.equal([...f.content.querySelectorAll('button')].find(node => node.textContent === '백업 파일 만들기').disabled, false);
  f.pick(restore()); await f.check(restore(), s.passphrase);
  const apply = [...restore().querySelectorAll('button')].find(node => node.textContent === '이 백업으로 복원');
  assert.equal(apply.disabled, true); assert.equal(apply.title, messages.restore_unavailable);
});

test('after a restore the page shows what happened: the Proxmox check and the access settings', async t => {
  const last = { operationId: '0a0a0a0a-1234-4234-8234-123456789abc', restoredAt: '2026-11-10T08:00:00.000Z', backup: { createdAt: '2026-11-09T03:10:00.000Z', serverVersion: '1.0.43', origin: 'https://old.fixture.example' }, actor: 'chief',
    proxmox: { state: 'error', errorCode: 'proxmox_permission_denied', checkedAt: '2026-11-10T08:00:05.000Z' }, access: { state: 'kept' } };
  const s = server({ last }), f = await open(t, 'last', s), panel = () => f.panel('backup-last');
  assert.equal(panel().querySelector('h2').textContent, '마지막 복원');
  const alert = panel().querySelector('.inline-alert.is-error');
  assert.equal(alert.querySelector('strong').textContent, 'Proxmox가 복원한 연결 정보를 받지 않아요');
  assert.equal(alert.querySelector('span').textContent, 'Proxmox 토큰 또는 접근 권한을 확인하세요.\n토큰이 바뀌었다면 Proxmox 연결에서 다시 넣어 주세요.');
  assert.equal(alert.querySelector('a').getAttribute('href'), '#/proxmox');
  const rows = () => Object.fromEntries([...panel().querySelectorAll('.facts > div')].map(row => [row.querySelector('dt').textContent, row.querySelector('dd').textContent]));
  assert.match(rows()['복원한 때'], /chief 계정이 복원했어요/); assert.match(rows()['쓴 백업'], /https:\/\/old\.fixture\.example · 버전 1\.0\.43/);
  assert.equal(rows()['Proxmox 연결'], '연결하지 못했어요'); assert.equal(rows()['서버 접속 설정'], '지금 설정을 그대로 썼어요');
  for (const [state, text] of [['requested', '백업의 설정을 적용했어요서버 접속 설정에서 결과 보기'], ['same', '백업과 같아서 그대로예요'], ['pending', '적용을 준비하고 있어요']]) {
    s.last = { ...last, proxmox: { state: 'ok' }, access: { state } };
    document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle(); f.navigate('overview'); f.navigate('backup'); await f.settle();
    await f.wait(20); await f.settle();
    if (rows()['서버 접속 설정'] !== text) { await f.wait(4100); f.navigate('overview'); f.navigate('backup'); await f.settle(); await f.settle(); }
    assert.equal(rows()['서버 접속 설정'], text, state); assert.equal(rows()['Proxmox 연결'], '정상이에요'); assert.equal(panel().querySelector('.inline-alert'), null);
  }
});

test('the first setup screen offers a restore with the setup code in place of a password', async t => {
  const s = server({ setup: true }), user = { id: 'fixture-initial', username: 'admin', role: 'admin', mustChangeCredentials: true };
  const f = await open(t, 'setup', s, { user, inventory: ownerFixture().inventory, hash: '' });
  assert.equal(document.querySelector('#setup').hidden, false); assert.equal(document.querySelector('#console').hidden, true);
  const form = document.querySelector('#setup-form'), card = document.querySelector('#setup-restore');
  assert.equal(card.hidden, true); assert.equal(form.hidden, false);
  assert.equal(document.querySelector('#setup-restore-open').textContent, '백업에서 복원');
  document.querySelector('#setup-restore-open').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.equal(card.hidden, false); assert.equal(form.hidden, true);
  assert.equal(card.querySelector('h2').textContent, '백업에서 복원');
  // Only its own parts: a part that is not shown (the Docker note here) must not leave the text "null".
  assert.equal([...card.childNodes].every(node => node.nodeType === 1), true); assert.doesNotMatch(card.textContent, /null/);
  assert.match(card.querySelector('.auth-lead').textContent, /복원한 뒤에는 백업에 있던 관리자 계정으로 로그인해요\./);
  assert.deepEqual([...card.querySelectorAll('form.backup-choose .backup-file-title, form.backup-choose > label')].map(node => node.firstChild.textContent), ['백업 파일', '백업 암호', '설정 코드']);
  const code = card.querySelector('input[name=setupCode]');
  code.value = 'ABCD-EFGH-JKMN'; code.dispatchEvent(new f.window.Event('input'));
  f.pick(card); await f.check(card, s.passphrase);
  const sent = f.backupWrites();
  assert.deepEqual(sent[0], { path: '/api/v1/admin/backup/restore/upload', data: { size: 2500, setupCode: 'ABCD-EFGH-JKMN' } });
  assert.deepEqual(sent.at(-1), { path: '/api/v1/admin/backup/restore/inspect', data: { uploadId: 'u'.repeat(32), passphrase: s.passphrase, setupCode: 'ABCD-EFGH-JKMN' } });
  assert.equal(card.querySelector('.backup-preview h3').textContent, '이 백업에 들어 있는 것');
  f.click(card, '이 백업으로 복원');
  assert.equal(document.querySelector('#modal-fields').querySelector('input'), null, 'the setup code already proved it; no password exists yet');
  await f.submitModal();
  assert.deepEqual(f.backupWrites().at(-1), { path: '/api/v1/admin/backup/restore/apply', data: { uploadId: 'u'.repeat(32), confirm: true, restoreAccess: false, setupCode: 'ABCD-EFGH-JKMN' } });
  assert.equal(card.querySelector('.backup-progress strong').textContent, '복원하고 있어요');
  assert.equal([...card.childNodes].every(node => node.nodeType === 1), true, 'no "null" around the progress'); assert.doesNotMatch(card.textContent, /null/);
  s.gone = true; await f.wait(1150); await f.settle();
  assert.equal(document.querySelector('#login').hidden, false);
  assert.equal(document.querySelector('#login-notice').textContent, '복원했어요.\n백업에 있던 관리자 계정으로 로그인해 주세요.');
  // Going back shows the ordinary first setup again.
  const s2 = server({ setup: true, uploadError: 'setup_code_invalid' }), g = await open(t, 'setup-back', s2, { user, inventory: ownerFixture().inventory, hash: '' });
  const card2 = document.querySelector('#setup-restore'); document.querySelector('#setup-restore-open').dispatchEvent(new g.window.Event('click')); await g.settle();
  g.pick(card2); await g.check(card2, s2.passphrase);
  assert.equal(card2.querySelector('.error').textContent, messages.setup_code_invalid);
  g.click(card2, '새 관리자 계정 만들기로 돌아가기');
  assert.equal(card2.hidden, true); assert.equal(document.querySelector('#setup-form').hidden, false); assert.equal(card2.textContent, '');
});

test('a reloaded page picks up the file this sign-in had checked, or the restore it had started', async t => {
  const s = server(); s.status = (base => () => ({ ...base(), upload: { uploadId: 'u'.repeat(32), size: 2500, received: 2500, state: 'inspected', preview: s.preview, operationId: '0a0a0a0a-1234-4234-8234-123456789abc' } }))(s.status);
  const f = await open(t, 'reload-preview', s);
  assert.equal(f.panel('backup-restore').querySelector('.backup-preview h3').textContent, '이 백업에 들어 있는 것'); assert.equal(s.uploads, 0);
  f.click(f.panel('backup-restore'), '이 백업으로 복원'); f.type(f.field('password'), 'account-password'); await f.submitModal();
  assert.equal(f.backupWrites().at(-1).data.uploadId, 'u'.repeat(32));
  const r = server({ restore: { state: 'applying', operationId: '0a0a0a0a-1234-4234-8234-123456789abc', busy: true } }); r.status = (base => () => ({ ...base(), upload: { uploadId: 'u'.repeat(32), size: 2500, received: 2500, state: 'applying', preview: r.preview, operationId: '0a0a0a0a-1234-4234-8234-123456789abc' } }))(r.status);
  const g = await open(t, 'reload-progress', r);
  assert.match(g.content.querySelector('.backup-progress').textContent, /복원하고 있어요지금 상태를 보관하고 백업으로 바꾸고 있어요\./);
  assert.equal(g.panel('backup-export'), null); assert.deepEqual(g.backupWrites(), []);
});

test('every code this feature can answer has words, and none of them is long', () => {
  const codes = ['backup_unavailable', 'backup_busy', 'backup_rate_limited', 'backup_disk_space', 'backup_passphrase_too_short', 'backup_passphrase_invalid', 'backup_passphrase_mismatch', 'backup_too_large', 'backup_download_expired', 'backup_file_invalid', 'backup_unreadable',
    'backup_format_newer', 'backup_from_newer_server', 'backup_schema_newer', 'backup_database_invalid', 'backup_database_unsupported', 'backup_no_administrator', 'backup_helper_failed', 'backup_helper_timeout', 'backup_upload_not_found', 'backup_upload_offset', 'backup_upload_state',
    'backup_upload_incomplete', 'backup_inspection_required', 'restore_unavailable', 'restore_busy', 'restore_recovery_required', 'restore_other_recovery_required', 'restore_confirmation_required', 'restore_result_unknown', 'restore_operation_reused', 'restore_command_invalid',
    'restore_fence_invalid', 'current_password_required', 'current_password_incorrect', 'mfa_code_required', 'mfa_invalid_code'];
  for (const code of codes) { assert.match(messages[code] ?? '', /[가-힣].*요\.$/s, code); assert.ok(messages[code].split('\n').length <= 3, code); }
  for (const code of ['restore_idle_timeout', 'restore_fence_lost', 'restore_activity_changed', 'restore_connections_busy', 'restore_disk_space', 'restore_staging_changed', 'restore_staging_invalid', 'restore_staged_database_invalid', 'restore_installation_changed', 'restore_health_failed', 'restore_interrupted', 'restore_recovery_required'])
    assert.match(restoreErrors[code] ?? '', /요\.$/, code);
  for (const code of ['restore_host_updates_busy', 'restore_desktop_settings_busy', 'restore_power_busy', 'restore_activity_unverified']) assert.match(blockerText[code] ?? '', /[가-힣]/, code);
  assert.equal(eventNames.backup_restore_connections_ended, '복원으로 연결 종료');
  for (const text of [...codes.map(code => messages[code]), ...Object.values(restoreErrors)]) assert.doesNotMatch(text, /이 PC|내 PC|VM/);
});

// A populated server for the backup tests: accounts, desktops, assignments
// with permissions, Host registrations, a sealed Proxmox connection, a sealed
// two-step factor, a Host request with sealed credentials, usage history, and
// the things that must not travel (sign-ins, enrolment codes, power tasks,
// diagnostics, the first-setup code).
import { randomBytes, createHash } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';
import { mkdtempSync, writeFileSync, rmSync, readdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { backupIncludedTables } from '../src/backup-database.mjs';

// The pin (a node certificate pinned by the setup script) is part of the
// sealed connection, so backup and restore carry it as they carry `ca`.
export const fixtureProxmox = { endpoint: 'https://pve.fixture.example:8006', tokenId: 'multitap@pve!broker', secret: '0f6c2a8e-5b1d-4c3a-9e7f-112233445566', ca: '', fingerprint: '5E:'.repeat(31) + 'A7', expiresAt: null };
export const fixtureTotp = 'a1'.repeat(20);
const sha = text => createHash('sha256').update(text).digest('hex');
const fakeHash = seed => 'scrypt-v1$' + sha('salt' + seed).slice(0, 32) + '$' + sha('key' + seed);
export const ids = { owner: '11111111-1111-4111-8111-111111111111', second: '22222222-2222-4222-8222-222222222222', member: '33333333-3333-4333-8333-333333333333', guest: '44444444-4444-4444-8444-444444444444' };

export function temporaryDirectory(t, prefix = 'multitap-backup-') {
  const directory = mkdtempSync(join(tmpdir(), prefix));
  t.after(() => rmSync(directory, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }));
  return directory;
}

// `seed` varies the content, so two servers made here differ in every table.
export function seedServer(directory, { seed = 'a', passwordHash = null, withSecrets = true } = {}) {
  const path = join(directory, 'broker.sqlite'), keyPath = join(directory, 'vault.key');
  const store = new Store(path), vault = Vault.open(keyPath), now = 1794300000000, db = store.db;
  const user = (id, name, role, primary = 0) => db.prepare('INSERT INTO users(id,username,display_name,notes,password_hash,role,created_at,is_primary_admin,must_change_credentials,disabled) VALUES(?,?,?,?,?,?,?,?,0,0)').run(id, name, name, '메모 ' + seed + name, passwordHash && primary ? passwordHash : fakeHash(seed + name), role, now, primary);
  user(ids.owner, 'owner' + seed, 'admin', 1); user(ids.second, 'second' + seed, 'admin'); user(ids.member, 'member' + seed, 'user'); user(ids.guest, '손님' + seed, 'user');
  db.prepare('UPDATE users SET disabled=1 WHERE id=?').run(ids.guest);
  for (const vmid of [101, 102, 170]) {
    db.prepare("INSERT INTO desktops(id,name,kind,description,os,enabled,created_at,pve_vmid,pve_available,pve_node,pve_power_status,pve_seen_at,pve_os_family,pve_os_source,pve_machine_uuid,pve_tags,pve_pool) VALUES(?,?,'vm',?,?,1,?,?,1,'pve','running',?,'windows','proxmox-config',?,?,?)")
      .run('pve-' + vmid, '업무 ' + seed + vmid, 'Proxmox VM ' + vmid, 'Windows 11', now, vmid, now, `0000${vmid}0-0000-4000-8000-00000000${seed === 'a' ? 'aaaa' : 'bbbb'}`, JSON.stringify(['office', seed]), 'pool-' + seed);
    db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at,last_seen_at,revoked) VALUES(?,?,?,?,?,?,?,?,0)')
      .run(`aaaaaaaa-0000-4000-8000-000000000${vmid}`, 'pve-' + vmid, sha('host-token-' + seed + vmid), 'DESKTOP-' + seed + vmid, `0000${vmid}0-0000-4000-8000-00000000${seed === 'a' ? 'aaaa' : 'bbbb'}`, '1.0.49', now, now + 5000);
  }
  db.prepare("INSERT INTO desktops(id,name,kind,description,os,enabled,created_at) VALUES('pc-" + seed + "','회의실 PC','pc','로컬 PC','Windows 11',1,?)").run(now);
  db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at,revoked) VALUES(?,?,?,?,?,?,?,0)').run('bbbbbbbb-0000-4000-8000-000000000001', 'pc-' + seed, sha('pc-token-' + seed), 'MEETING-' + seed, 'cccccccc-0000-4000-8000-00000000000' + (seed === 'a' ? '1' : '2'), '1.0.49', now);
  db.prepare('INSERT INTO local_pc_approvals(desktop_id,host_id,machine_uuid,approved_at,actor) VALUES(?,?,?,?,?)').run('pc-' + seed, 'bbbbbbbb-0000-4000-8000-000000000001', 'cccccccc-0000-4000-8000-00000000000' + (seed === 'a' ? '1' : '2'), now, ids.owner);
  db.prepare('INSERT INTO pc_wake(desktop_id,host_id,adapters,reported_at) VALUES(?,?,?,?)').run('pc-' + seed, 'bbbbbbbb-0000-4000-8000-000000000001', JSON.stringify([{ mac: '02:00:00:00:00:0' + (seed === 'a' ? '1' : '2'), broadcast: '192.0.2.255' }]), now);
  const assign = (userId, desktop, clipboard, redirections, sound, favorite) => db.prepare('INSERT INTO assignments(user_id,desktop_id,clipboard_permissions,rdp_redirections,sound_card_add,favorite,last_used_at) VALUES(?,?,?,?,?,?,?)').run(userId, desktop, clipboard, redirections, sound, favorite, now + clipboard);
  assign(ids.member, 'pve-101', 5, 3, 1, 1); assign(ids.member, 'pve-170', 15, 127, 0, 0); assign(ids.second, 'pve-102', 0, 0, 0, 0); assign(ids.guest, 'pc-' + seed, 1, 0, 0, 1);
  const setting = (key, value) => db.prepare('INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value').run(key, value);
  if (withSecrets) {
    setting('proxmox', vault.seal({ ...fixtureProxmox, secret: fixtureProxmox.secret + seed }));
    setting('proxmox_sync', JSON.stringify({ state: 'ok', checkedAt: new Date(now).toISOString(), count: 3, added: 0, updated: 3, unavailable: 0 }));
    setting('proxmox_initialized', '1');
    db.prepare("INSERT INTO account_mfa(user_id,generation,enabled,factor_ciphertext,last_accepted_step,changed_at) VALUES(?,?,1,?,-1,?)").run(ids.second, 2, vault.seal({ kind: 'totp-v1', userId: ids.second, generation: 2, secret: fixtureTotp }), now);
    db.prepare('INSERT INTO mfa_recovery_codes(user_id,generation,code_hash) VALUES(?,?,?)').run(ids.second, 2, sha('recovery' + seed));
    db.prepare("INSERT INTO host_requests(id,key_hash,hostname,machine_uuid,version,status,created_at,expires_at,decided_at,desktop_id,host_id,credentials_sealed,decision_actor) VALUES(?,?,?,?,?,'approved',?,?,?,?,?,?,?)")
      .run('dddddddd-0000-4000-8000-000000000001', sha('request-key' + seed), 'DESKTOP-' + seed + '101', '00001010-0000-4000-8000-00000000' + (seed === 'a' ? 'aaaa' : 'bbbb'), '1.0.49', now, now + 600000, now + 1000, 'pve-101', 'aaaaaaaa-0000-4000-8000-000000000101', vault.seal({ kind: 'host-request-v1', requestId: 'dddddddd-0000-4000-8000-000000000001', credentials: { hostToken: 'token-' + seed } }), ids.owner);
  }
  setting('desktop_previews', JSON.stringify({ enabled: seed === 'a' }));
  setting('initial_setup_code', sha('setup-code'));
  for (let index = 0; index < 40; index++) store.audit(index % 2 ? 'login_succeeded' : 'desktop_assigned', index % 3 ? ids.member : ids.owner, seed + ':' + index, now + index * 1000);
  // Usage history: ended sign-ins and connections, and one of each still open.
  for (let index = 0; index < 12; index++) {
    db.prepare("INSERT INTO usage_logins(user_id,token_hash,started_at,ended_at,end_reason,estimated,issuer,platform,client_version,address,source) VALUES(?,?,?,?,?,0,'windows-pc-v1','windows','1.0.43','192.0.2." + index + "','server')").run(ids.member, null, now + index * 60000, now + index * 60000 + 30000, 'logout');
    db.prepare("INSERT INTO usage_connections(session_id,user_id,desktop_id,desktop_name,mode,started_at,last_seen_at,ended_at,end_reason,estimated,platform,client_version,address,source) VALUES(?,?,?,?,'stream',?,?,?,'ended',0,'windows','1.0.43','192.0.2.9','server')")
      .run(`eeeeeeee-0000-4000-8000-0000000000${String(index).padStart(2, '0')}`, ids.member, 'pve-101', '업무 ' + seed + '101', now + index * 60000, now + index * 60000 + 20000, now + index * 60000 + 20000);
  }
  // What must stay behind.
  const liveToken = sha('live-session-' + seed), adminToken = sha('admin-session-' + seed);
  db.prepare("INSERT INTO sessions(token_hash,user_id,created_at,expires_at,purpose,issuer) VALUES(?,?,?,?,'client','windows-pc-v1')").run(liveToken, ids.member, now, now + 365 * 86400000 * 10);
  db.prepare("INSERT INTO sessions(token_hash,user_id,created_at,expires_at,purpose,issuer) VALUES(?,?,?,?,'admin','admin')").run(adminToken, ids.owner, now, now + 365 * 86400000 * 10);
  // The last input is the real clock's: a server started on this file sweeps a session idle for 30 minutes,
  // and with the fixed time above that closed the open sign-in once the day had moved on.
  db.prepare('INSERT INTO session_activity(token_hash,tracking_started_at,idle_enabled,last_input_at) VALUES(?,?,1,?)').run(liveToken, now, Math.max(now + 1, Date.now()));
  db.prepare("INSERT INTO session_end_reasons(token_hash,user_id,reason,ended_at,retain_until) VALUES(?,?,'session_idle_timeout',?,?)").run(sha('ended-session'), ids.member, now, now + 86400000);
  db.prepare("INSERT INTO usage_connections(session_id,user_id,desktop_id,desktop_name,mode,started_at,last_seen_at,estimated,source) VALUES('ffffffff-0000-4000-8000-000000000001',?,'pve-170','열린 연결','stream',?,?,0,'server')").run(ids.member, now + 900000, now + 905000);
  db.prepare('INSERT INTO host_enrollments(code_hash,desktop_id,machine_uuid,created_at,expires_at) VALUES(?,?,?,?,?)').run(sha('enroll'), 'pve-102', '00001020-0000-4000-8000-00000000aaaa', now, now + 600000);
  db.prepare("INSERT INTO power_tasks(id,desktop_id,user_id,action,status,active,submitted_at,guard_until,config_stamp) VALUES('task-1','pve-102',?,'start','submitted',1,?,?,'stamp')").run(ids.owner, now, now + 60000);
  db.prepare('INSERT INTO mfa_failure_budgets(user_id,window_started_at,failures,blocked_until,last_observed_at) VALUES(?,?,2,0,?)').run(ids.second, now, now);
  db.prepare("INSERT INTO client_diagnostics(user_id,received_at,run_id,sequence,report,evidence) VALUES(?,?,'run',1,?,0)").run(ids.member, now, JSON.stringify({ events: [], note: 'x'.repeat(2000) }));
  db.prepare('INSERT INTO host_diagnostics(host_id,received_at,report) VALUES(?,?,?)').run('aaaaaaaa-0000-4000-8000-000000000101', now, '{"report":true}');
  db.prepare("INSERT INTO host_diagnostic_events(host_id,received_at,version,row) VALUES(?,?,'1.0.49','{\"e\":1}')").run('aaaaaaaa-0000-4000-8000-000000000101', now);
  return { store, vault, path, keyPath, directory, tokens: { live: liveToken, admin: adminToken } };
}

// Every row of every table that travels, in a comparable form.
export function travellingRows(path, { skipSettings = [] } = {}) {
  const db = new DatabaseSync(path, { readOnly: true });
  try {
    const result = {};
    for (const name of backupIncludedTables) {
      const rows = db.prepare(`SELECT rowid AS "#rowid",* FROM "${name}" ORDER BY rowid`).all().map(row => ({ ...row }));
      result[name] = name === 'settings' ? rows.filter(row => !skipSettings.includes(row.key)).map(({ '#rowid': _rowid, ...row }) => row).sort((a, b) => a.key.localeCompare(b.key)) : rows;
    }
    result.sequence = db.prepare('SELECT name,seq FROM sqlite_sequence ORDER BY name').all().map(row => ({ ...row })).filter(row => backupIncludedTables.includes(row.name));
    return result;
  } finally { db.close(); }
}
export const tableCount = (path, name) => { const db = new DatabaseSync(path, { readOnly: true }); try { return db.prepare(`SELECT count(*) AS n FROM "${name}"`).get().n; } finally { db.close(); } };
export const freshKey = () => randomBytes(32);
export const leftovers = directory => readdirSync(directory);
export const writeBytes = (path, bytes) => writeFileSync(path, bytes);

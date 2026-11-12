// The database side of 백업·복원: what travels, how the snapshot is taken and
// cleaned, and how an uploaded database is read.
//
// An uploaded database is never opened by the broker and never becomes the
// live database. A short-lived helper process (backup-helper.mjs) reads it
// with SQLite's defences on and copies rows, table by table and column by
// column, into a new database that this server's own schema code created.
// Nothing executable in the file (triggers, views, index or column
// expressions, virtual tables) is installed, and none of it is run while rows
// are copied: the copy statements may only read ordinary columns of the
// tables named below (an authorizer refuses everything else, functions too).
import { DatabaseSync, backup, constants as sqlite } from 'node:sqlite';
import { openSync, readSync, closeSync, fstatSync, copyFileSync, statSync } from 'node:fs';
import { Store } from './store.mjs';
import { Vault } from './vault.mjs';
import { BackupError, backupMaxDatabaseBytes } from './backup-format.mjs';

const fail = code => { throw new BackupError(code); };

// Rows that travel. Accounts and their password hashes, desktops, who is
// assigned what with which permissions, Host registrations (token hashes),
// Host requests, approved PCs and their wake addresses, settings (the sealed
// Proxmox connection among them), the administration record, usage history
// and two-step sign-in factors.
export const backupIncludedTables = Object.freeze(['users', 'desktops', 'assignments', 'hosts', 'host_requests', 'local_pc_approvals', 'pc_wake', 'settings', 'audit_events', 'usage_logins', 'usage_connections', 'account_mfa', 'mfa_recovery_codes']);
// Rows that stay: live sign-ins and their tokens, one-time enrolment codes,
// power operations in flight, sign-in lockout counters and the short-lived
// diagnostic reports of clients and Hosts (7 days, up to tens of megabytes).
// Notices (공지, notices.mjs) stay too: they were said to the people signed in
// to that server at that time, and a restored server must not show them again.
export const backupExcludedTables = Object.freeze(['sessions', 'session_activity', 'session_end_reasons', 'host_enrollments', 'power_tasks', 'mfa_failure_budgets', 'client_diagnostics', 'host_diagnostics', 'host_diagnostic_events', 'notices', 'notice_recipients', 'notice_deliveries']);
// Settings that belong to one server's own run: the first-setup code and the
// records of this feature itself.
export const backupExcludedSettings = Object.freeze(['initial_setup_code', 'restore_pending', 'restore_last', 'backup_last_export']);
export const restorePendingSetting = 'restore_pending';

const quote = name => '"' + String(name).replaceAll('"', '""') + '"';
const single = (db, sql, ...values) => { const row = db.prepare(sql).get(...values); return row ? Object.values(row)[0] : undefined; };
function assertHealthy(db, code) {
  const rows = db.prepare('PRAGMA integrity_check(1)').all();
  if (rows.length !== 1 || Object.values(rows[0])[0] !== 'ok') fail(code);
}
const tableNames = db => new Set(db.prepare("SELECT name FROM sqlite_schema WHERE type='table'").all().map(row => row.name));

// A sign-in or connection that was open when the snapshot was taken is not
// open on the server that restores it.
function closeOpenUsage(db, now) {
  const tables = tableNames(db);
  if (tables.has('usage_logins')) {
    db.prepare("UPDATE usage_logins SET ended_at=max(started_at,?),end_reason=COALESCE(end_reason,'ended'),estimated=1 WHERE ended_at IS NULL").run(now);
    db.prepare('UPDATE usage_logins SET token_hash=NULL WHERE token_hash IS NOT NULL').run();
  }
  if (tables.has('usage_connections')) db.prepare("UPDATE usage_connections SET ended_at=max(started_at,last_seen_at),end_reason='server_restart',estimated=1 WHERE ended_at IS NULL").run();
}

export function backupCounts(db) {
  const admins = db.prepare("SELECT username FROM users WHERE role='admin' AND disabled=0 ORDER BY is_primary_admin DESC,username LIMIT 5").all().map(row => String(row.username));
  return {
    accounts: Number(single(db, 'SELECT count(*) FROM users')), admins: Number(single(db, "SELECT count(*) FROM users WHERE role='admin' AND disabled=0")),
    desktops: Number(single(db, 'SELECT count(*) FROM desktops')), hosts: Number(single(db, 'SELECT count(*) FROM hosts WHERE revoked=0')),
    assignments: Number(single(db, 'SELECT count(*) FROM assignments')), adminNames: admins,
  };
}

// SQLite's own online backup of the live database (no copy of a file that is
// being written). Runs in steps, so the broker keeps answering meanwhile.
export async function snapshotDatabase(db, target) { await backup(db, target); }

// The snapshot is this server's own data. Remove what does not travel, close
// what was open, check it, and rebuild the file so that the removed rows are
// not left behind in free pages. Returns what the file now holds.
export function sanitizeSnapshot(path, { now = Date.now() } = {}) {
  const db = new DatabaseSync(path);
  try {
    db.exec('PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA secure_delete=ON;');
    const tables = tableNames(db);
    db.exec('BEGIN IMMEDIATE');
    try {
      // Deleting the sign-ins first lets the usage triggers close their rows.
      for (const name of backupExcludedTables) if (tables.has(name)) db.exec('DELETE FROM ' + quote(name));
      for (const key of backupExcludedSettings) db.prepare('DELETE FROM settings WHERE key=?').run(key);
      closeOpenUsage(db, now);
      db.exec('COMMIT');
    } catch (error) { db.exec('ROLLBACK'); throw error; }
    db.exec('VACUUM');
    assertHealthy(db, 'backup_database_invalid');
    if (db.prepare('PRAGMA foreign_key_check').all().length) fail('backup_database_invalid');
    const schemaVersion = Number(single(db, 'PRAGMA user_version'));
    return { schemaVersion, counts: backupCounts(db) };
  } finally { db.close(); }
}

// ---- reading an uploaded database -------------------------------------

function assertSqliteFile(path) {
  const size = statSync(path).size;
  if (size < 512 || size > backupMaxDatabaseBytes) fail(size > backupMaxDatabaseBytes ? 'backup_too_large' : 'backup_database_invalid');
  const fd = openSync(path, 'r');
  try {
    if (!fstatSync(fd).isFile()) fail('backup_database_invalid');
    const head = Buffer.alloc(100);
    if (readSync(fd, head, 0, 100, 0) !== 100 || head.toString('latin1', 0, 16) !== 'SQLite format 3\0') fail('backup_database_invalid');
    const pageSize = head.readUInt16BE(16) === 1 ? 65536 : head.readUInt16BE(16);
    // A snapshot made here is a single file in rollback-journal mode; a file
    // that asks for a write-ahead log next to it is not one of ours.
    if (pageSize < 512 || (pageSize & (pageSize - 1)) || size % pageSize || head[18] !== 1 || head[19] !== 1) fail('backup_database_invalid');
  } finally { closeSync(fd); }
}
function openUntrusted(path) {
  const db = new DatabaseSync(path, { readOnly: true, allowExtension: false, enableForeignKeyConstraints: false,
    limits: { length: 16 * 1024 * 1024, sqlLength: 1024 * 1024, column: 200, exprDepth: 100, compoundSelect: 10, functionArg: 16, attach: 0, likePatternLength: 1000, variableNumber: 100, triggerDepth: 1 } });
  try { db.enableDefensive(true); db.exec('PRAGMA trusted_schema=OFF; PRAGMA cell_size_check=ON; PRAGMA query_only=ON;'); }
  catch (error) { db.close(); throw error; }
  return db;
}
const schemaRows = db => {
  const rows = db.prepare('SELECT type,name,tbl_name,sql FROM sqlite_schema LIMIT 2001').all();
  if (rows.length > 2000) fail('backup_database_invalid');
  return rows.map(row => ({ type: String(row.type), name: String(row.name), table: String(row.tbl_name), sql: row.sql === null ? null : String(row.sql) }));
};
const columnsOf = (db, name) => db.prepare('PRAGMA table_xinfo(' + quote(name) + ')').all().map(row => ({ name: String(row.name), type: String(row.type ?? '').toUpperCase(), pk: Number(row.pk), hidden: Number(row.hidden) }));
const rowidAlias = columns => { const keys = columns.filter(column => column.pk > 0); return keys.length === 1 && keys[0].type === 'INTEGER' ? keys[0].name : null; };

// What the file may contain. Views and virtual tables are never made by any
// Multitap server, so a file with one is refused. The tables this server
// knows must be ordinary tables with ordinary (not generated, not hidden)
// columns. Anything else, such as a table or index an old release left, is
// neither read nor copied.
function inspectSchema(db, reference) {
  const rows = schemaRows(db), tables = new Map();
  let ignored = 0;
  for (const row of rows) {
    if (row.type === 'view') fail('backup_database_unsupported');
    if (row.type === 'table') {
      if (row.sql !== null && /^\s*CREATE\s+VIRTUAL\b/i.test(row.sql)) fail('backup_database_unsupported');
      if (!reference.tables.has(row.name)) { if (!row.name.startsWith('sqlite_')) ignored++; continue; }
      if (row.sql === null || /\bWITHOUT\s+ROWID\b/i.test(row.sql)) fail('backup_database_unsupported');
      const columns = columnsOf(db, row.name);
      if (!columns.length || columns.some(column => column.hidden !== 0)) fail('backup_database_unsupported');
      tables.set(row.name, columns);
    } else if (row.type === 'index') { if (!reference.indexes.has(row.name) && !row.name.startsWith('sqlite_autoindex_')) ignored++; }
    else if (row.type === 'trigger') { if (reference.triggers.get(row.name) !== row.sql) ignored++; }
    else fail('backup_database_unsupported');
  }
  for (const name of ['users', 'settings']) if (!tables.has(name)) fail('backup_database_invalid');
  return { tables, ignored, sequence: rows.some(row => row.type === 'table' && row.name === 'sqlite_sequence') };
}
function referenceOf(db) {
  const rows = schemaRows(db);
  return {
    tables: new Map(rows.filter(row => row.type === 'table' && !row.name.startsWith('sqlite_')).map(row => [row.name, columnsOf(db, row.name)])),
    indexes: new Set(rows.filter(row => row.type === 'index').map(row => row.name)),
    triggers: new Map(rows.filter(row => row.type === 'trigger').map(row => [row.name, row.sql])),
  };
}
// Every table of this server is either copied or deliberately left out; a new
// table must be put in one of the two lists before a backup can be made.
export function assertTablesClassified(reference) {
  const known = new Set([...backupIncludedTables, ...backupExcludedTables]);
  for (const name of reference.tables.keys()) if (!known.has(name)) fail('backup_table_unclassified');
}
const lacksSomething = (found, reference) => backupIncludedTables.some(name => {
  const columns = found.tables.get(name), wanted = reference.tables.get(name);
  return wanted && (!columns || wanted.some(column => !columns.some(row => row.name === column.name)));
});

function copyRows(source, found, out, reference) {
  const allowed = new Set([...backupIncludedTables.filter(name => found.tables.has(name)), ...(found.sequence ? ['sqlite_sequence'] : [])]);
  // From here on the uploaded file can only be asked for stored column values
  // of those tables: no function, no pragma, nothing that writes.
  source.setAuthorizer((action, first) => action === sqlite.SQLITE_SELECT || (action === sqlite.SQLITE_READ && allowed.has(first)) ? sqlite.SQLITE_OK : sqlite.SQLITE_DENY);
  for (const name of backupIncludedTables) {
    const columns = found.tables.get(name), wanted = reference.tables.get(name);
    if (!columns || !wanted) continue;
    const shared = wanted.filter(column => columns.some(row => row.name === column.name)).map(column => column.name);
    const alias = rowidAlias(wanted), names = alias ? shared : ['rowid', ...shared];
    if (alias && rowidAlias(columns) !== alias) fail('backup_database_unsupported');
    const read = source.prepare('SELECT ' + names.map(column => column === 'rowid' ? 'rowid' : quote(column)).join(',') + ' FROM ' + quote(name) + ' ORDER BY rowid');
    read.setReadBigInts(true);
    const write = out.prepare('INSERT INTO ' + quote(name) + '(' + names.map(column => column === 'rowid' ? 'rowid' : quote(column)).join(',') + ') VALUES(' + names.map(() => '?').join(',') + ')');
    for (const row of read.iterate()) {
      if (name === 'settings' && backupExcludedSettings.includes(String(row.key))) continue;
      write.run(...names.map(column => row[column]));
    }
  }
  if (found.sequence) {
    // Ids keep growing from where the backup stopped. SQLite has already
    // noted the largest copied id of each table; a higher counter replaces it.
    const rows = source.prepare('SELECT name,seq FROM sqlite_sequence LIMIT 1000').all(), seen = new Set();
    for (const row of rows) {
      const name = String(row.name), seq = Number(row.seq);
      if (seen.has(name) || !backupIncludedTables.includes(name) || !reference.tables.has(name) || !Number.isSafeInteger(seq) || seq < 0) continue;
      seen.add(name);
      if (!out.prepare('UPDATE sqlite_sequence SET seq=max(seq,?) WHERE name=?').run(seq, name).changes) out.prepare('INSERT INTO sqlite_sequence(name,seq) VALUES(?,?)').run(name, seq);
    }
  }
}

// Whether what the vault sealed opens with the key that came in the same file.
// Only whether it opened is reported, and the Proxmox address the console
// shows anyway; no secret leaves this function.
function sealedSummary(db, vaultKey) {
  const vault = new Vault(Buffer.from(vaultKey)), result = { proxmox: { configured: false, readable: false, endpoint: null }, unreadable: 0 };
  const opens = value => { try { return vault.open(String(value)); } catch { result.unreadable++; return null; } };
  const proxmox = single(db, "SELECT value FROM settings WHERE key='proxmox'");
  if (proxmox !== undefined) {
    const value = opens(proxmox);
    result.proxmox = { configured: true, readable: value !== null, endpoint: typeof value?.endpoint === 'string' && value.endpoint.length <= 300 && /^https:\/\/[^\s\x00-\x20\x7f\\@?#%]+$/.test(value.endpoint) ? value.endpoint : null };
  }
  for (const row of db.prepare('SELECT credentials_sealed AS value FROM host_requests WHERE credentials_sealed IS NOT NULL LIMIT 5000').all()) opens(row.value);
  for (const row of db.prepare('SELECT factor_ciphertext AS value FROM account_mfa WHERE factor_ciphertext IS NOT NULL LIMIT 100000').all()) opens(row.value);
  return result;
}

// Runs inside the helper process. `source` is the decrypted, untrusted
// database; `output` is created here and is what a restore would install;
// `work` is a scratch path used only when the file is older than this server.
export function rebuildFromBackup({ source, work, output, vaultKey, marker = null, now = Date.now() }) {
  assertSqliteFile(source);
  // This server's schema, made by its own code, is both the destination and
  // the list of what is known.
  new Store(output).close();
  const out = new DatabaseSync(output);
  let from = null;
  try {
    const reference = referenceOf(out), current = Number(single(out, 'PRAGMA user_version'));
    assertTablesClassified(reference);
    let found, version, migrated = false, ignored;
    try {
      from = openUntrusted(source);
      found = inspectSchema(from, reference); ignored = found.ignored;
      assertHealthy(from, 'backup_database_invalid');
      version = Number(single(from, 'PRAGMA user_version'));
    } catch (error) { if (error instanceof BackupError) throw error; fail('backup_database_invalid'); }
    if (!Number.isSafeInteger(version) || version < 1) fail('backup_database_invalid');
    if (version > current) fail('backup_schema_newer');
    if (version < current || lacksSomething(found, reference)) {
      // An older database is brought up to date by the normal migration code,
      // on a scratch copy inside this helper. Trigger text that is not this
      // server's own is removed from that copy first, so the migration runs
      // no code from the file.
      from.close(); from = null;
      copyFileSync(source, work);
      const scratch = new DatabaseSync(work, { allowExtension: false });
      try {
        scratch.exec('PRAGMA trusted_schema=OFF;');
        for (const row of schemaRows(scratch)) if (row.type === 'trigger' && reference.triggers.get(row.name) !== row.sql) scratch.exec('DROP TRIGGER ' + quote(row.name));
      } finally { scratch.close(); }
      try { new Store(work).close(); } catch { fail('backup_database_invalid'); }
      const settled = new DatabaseSync(work); try { settled.exec('PRAGMA journal_mode=DELETE;'); } finally { settled.close(); }
      try {
        from = openUntrusted(work); found = inspectSchema(from, reference); assertHealthy(from, 'backup_database_invalid');
        if (Number(single(from, 'PRAGMA user_version')) !== current || lacksSomething(found, reference)) fail('backup_database_invalid');
      } catch (error) { if (error instanceof BackupError) throw error; fail('backup_database_invalid'); }
      migrated = true;
    }
    out.exec('PRAGMA foreign_keys=OFF;');
    const triggers = [...reference.triggers.entries()];
    out.exec('BEGIN IMMEDIATE');
    try {
      // Rows are copied as they are: this server's triggers must not react to
      // them, so they are set aside and put back, text unchanged, afterwards.
      for (const [name] of triggers) out.exec('DROP TRIGGER ' + quote(name));
      for (const name of reference.tables.keys()) out.exec('DELETE FROM ' + quote(name));
      if (tableNames(out).has('sqlite_sequence')) out.exec('DELETE FROM sqlite_sequence');
      try { copyRows(from, found, out, reference); }
      catch (error) { if (error instanceof BackupError) throw error; fail('backup_database_invalid'); }
      closeOpenUsage(out, now);
      if (marker !== null) {
        if (typeof marker !== 'string' || marker.length > 16384) fail('backup_unavailable');
        JSON.parse(marker);
        out.prepare('INSERT INTO settings(key,value) VALUES(?,?)').run(restorePendingSetting, marker);
      }
      for (const [, sql] of triggers) out.exec(sql);
      out.exec('COMMIT');
    } catch (error) { out.exec('ROLLBACK'); throw error; }
    if (out.prepare('PRAGMA foreign_key_check').all().length) fail('backup_database_invalid');
    assertHealthy(out, 'backup_database_invalid');
    if (Number(single(out, 'PRAGMA user_version')) !== current) fail('backup_database_invalid');
    if (!single(out, "SELECT 1 FROM users WHERE role='admin' AND disabled=0 LIMIT 1")) fail('backup_no_administrator');
    const sealed = sealedSummary(out, vaultKey), counts = backupCounts(out);
    out.exec('PRAGMA journal_mode=DELETE;');
    return { schemaVersion: version, currentSchemaVersion: current, migrated, ignoredObjects: ignored, counts, proxmox: sealed.proxmox, unreadableSecrets: sealed.unreadable };
  } finally { from?.close(); out.close(); }
}

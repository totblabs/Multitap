import { DatabaseSync } from 'node:sqlite';
import { mkdirSync, chmodSync } from 'node:fs';
import { dirname } from 'node:path';
import { randomUUID } from 'node:crypto';
import { username, text, notes as accountNotes, desktopId } from './security.mjs';
import { clipboardPolicyFromMask, clipboardPolicyMask } from './clipboard-policy.mjs';
import { devicePolicyBits, devicePolicyFromMask, devicePolicyMask } from './device-policy.mjs';
import { normalizeTags, normalizePool, storedTags } from './desktop-organize.mjs';
import { approvedPcSql, availableDesktopSql } from './local-pc.mjs';
import { wakeAdapters } from './pc-power.mjs';
import { pcSessionPolicy, pcLoginReplaced, replacedSessionsPerUser, replacedSessionsTotal } from './client-session-policy.mjs';
import { clientDiagnosticEvidenceSql } from './client-diagnostics.mjs';
import { sessionAccessSchema } from './session-access-store.mjs';
import { installUsage, noteLoginEnd } from './usage.mjs';
import { installNotices } from './notices.mjs';
import { readFileSync } from 'node:fs';

// Registered is not the same as online. Keep approved offline VM records, but
// never revive a revoked token or an unapproved physical PC.
const registeredDesktopSql = `EXISTS (SELECT 1 FROM hosts rh WHERE rh.desktop_id=d.id AND rh.revoked=0 AND (
  (d.kind='vm' AND d.pve_vmid IS NOT NULL AND (d.pve_machine_uuid IS NULL OR lower(rh.machine_uuid)=lower(d.pve_machine_uuid))) OR (${approvedPcSql})))`;

// The relay re-checks the bearer session, assignment and host token on every
// forwarded input/track message and in its 250 ms authorization sweep.
// Compiling those statements dominated each check (~50 of ~60 us measured
// locally). Reuse the compiled statement for the same database handle only;
// the SQL text, parameters and returned rows are unchanged, so every caller
// still observes the live database state on each call.
const reusableStatements = new WeakMap();
function reusable(db, sql) {
  let statements = reusableStatements.get(db);
  if (!statements) reusableStatements.set(db, statements = new Map());
  let statement = statements.get(sql);
  if (!statement) statements.set(sql, statement = db.prepare(sql));
  return statement;
}

// Batch assignment input: a legacy desktop id string, or one entry carrying
// that VM's own permissions. An omitted policy keeps an existing assignment's
// value and leaves a new assignment denied. Everything is validated before any
// row changes so one invalid entry changes nothing.
const assignmentEntryKeys = ['desktopId', 'clipboardPolicy', 'devicePolicy', 'soundCardAdd'];
const desktopPermissionKeys = ['username', 'expectedAccountId', 'clipboardPolicy', 'devicePolicy', 'soundCardAdd'];
function soundCardAddValue(value) {
  if (value !== undefined && typeof value !== 'boolean') throw new Error('invalid_sound_card_add');
  return value;
}
function assignmentEntries(entries) {
  if (!Array.isArray(entries) || entries.length > 500) throw new Error('invalid_desktop_ids');
  const result = entries.map(entry => {
    if (typeof entry === 'string') return { id: entry };
    if (!entry || typeof entry !== 'object' || Array.isArray(entry) || Object.keys(entry).some(key => !assignmentEntryKeys.includes(key))) throw new Error('invalid_assignment_entry');
    return { id: entry.desktopId, clipboard: entry.clipboardPolicy === undefined ? undefined : clipboardPolicyMask(entry.clipboardPolicy), redirections: entry.devicePolicy === undefined ? undefined : devicePolicyMask(entry.devicePolicy), soundCardAdd: soundCardAddValue(entry.soundCardAdd) };
  });
  if (result.some(({ id }) => typeof id !== 'string' || !/^[A-Za-z0-9_-]{1,80}$/.test(id)) || new Set(result.map(({ id }) => id)).size !== result.length) throw new Error('invalid_desktop_ids');
  return result;
}
const assignmentPolicy = row => ({ clipboardPolicy: clipboardPolicyFromMask(row?.clipboard_permissions), devicePolicy: devicePolicyFromMask(row?.rdp_redirections), soundCardAdd: row?.sound_card_add === 1 });
// A PC's last wake report for the administrator console; null before one.
function storedWake(adapters, reportedAt) {
  if (adapters == null) return null;
  let list = null; try { list = wakeAdapters(JSON.parse(adapters)); } catch { /* Damaged rows read as no report. */ }
  return list ? { adapters: list, reportedAt: new Date(reportedAt).toISOString() } : null;
}
const verifiedMachineUuid = value => typeof value === 'string' &&
  /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(value) &&
  !/^0{8}(?:-0{4}){3}-0{12}$/.test(value) && !/^f{8}(?:-f{4}){3}-f{12}$/i.test(value)
  ? value.toLowerCase() : null;

export class Store {
  constructor(path) {
    if (path !== ':memory:') mkdirSync(dirname(path), { recursive: true, mode: 0o700 });
    this.db = new DatabaseSync(path);
    // Whether each registered VM's Proxmox config enables the QEMU guest
    // agent (desktop id -> boolean), from the last registered-VM refresh.
    // Memory only: every refresh reads it again, so no schema change.
    this.qemuAgentOption = new Map();
    this.db.exec('PRAGMA foreign_keys=ON; PRAGMA busy_timeout=5000; PRAGMA journal_mode=WAL;');
    const version = this.db.prepare('PRAGMA user_version').get().user_version;
    if (version > 19) { this.db.close(); throw new Error('database_version_too_new'); }
    this.db.exec(`
      CREATE TABLE IF NOT EXISTS users (
        id TEXT PRIMARY KEY, username TEXT NOT NULL UNIQUE COLLATE NOCASE,
        display_name TEXT NOT NULL, password_hash TEXT NOT NULL,
        role TEXT NOT NULL CHECK(role IN ('admin','user')),
        disabled INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS desktops (
        id TEXT PRIMARY KEY, name TEXT NOT NULL, kind TEXT NOT NULL CHECK(kind IN ('pc','vm')),
        description TEXT NOT NULL, os TEXT NOT NULL, enabled INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL
      );
      CREATE TABLE IF NOT EXISTS assignments (
        user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        desktop_id TEXT NOT NULL REFERENCES desktops(id) ON DELETE CASCADE,
        PRIMARY KEY(user_id, desktop_id)
      );
      CREATE TABLE IF NOT EXISTS sessions (
        token_hash TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
        created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id);
      CREATE INDEX IF NOT EXISTS sessions_expiry ON sessions(expires_at);
      CREATE TABLE IF NOT EXISTS audit_events (
        id INTEGER PRIMARY KEY, created_at INTEGER NOT NULL, event TEXT NOT NULL,
        actor TEXT NOT NULL, subject TEXT NOT NULL
      );
      CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
    `);
    if (version < 2) this.transaction(() => {
      this.db.exec("ALTER TABLE desktops ADD COLUMN pve_vmid INTEGER; ALTER TABLE sessions ADD COLUMN purpose TEXT NOT NULL DEFAULT 'client'; PRAGMA user_version=2;");
    });
    if (version < 3) this.transaction(() => {
      this.db.exec('ALTER TABLE users ADD COLUMN must_change_credentials INTEGER NOT NULL DEFAULT 0; PRAGMA user_version=3;');
    });
    if (version < 4) this.transaction(() => {
      this.db.exec(`
        ALTER TABLE users ADD COLUMN notes TEXT NOT NULL DEFAULT '';
        ALTER TABLE users ADD COLUMN is_primary_admin INTEGER NOT NULL DEFAULT 0 CHECK(is_primary_admin IN (0,1));
        UPDATE users SET notes=CASE WHEN display_name<>username THEN display_name ELSE '' END,display_name=username;
        CREATE UNIQUE INDEX users_single_primary_admin ON users(is_primary_admin) WHERE is_primary_admin=1;
      `);
      // Bootstrap setup records reference the immutable user id, even after its name changes.
      // Older installations recorded bootstrap as the earliest server-admin creation instead.
      const primary = this.db.prepare(`
        SELECT u.id FROM audit_events a JOIN users u ON
          u.id=CASE WHEN a.event='initial_setup_completed' THEN a.actor ELSE a.subject END
        WHERE a.event IN ('initial_setup_completed','initial_setup_requested') ORDER BY a.id LIMIT 1
      `).get() ?? this.db.prepare(`
        SELECT u.id FROM audit_events a JOIN users u ON u.id=a.subject
        WHERE a.event='user_created' AND a.actor='server-admin' ORDER BY a.id LIMIT 1
      `).get() ?? this.db.prepare("SELECT id FROM users WHERE role='admin' ORDER BY created_at,id LIMIT 1").get();
      if (primary) this.db.prepare("UPDATE users SET is_primary_admin=1,role='admin',disabled=0 WHERE id=?").run(primary.id);
      this.db.exec('PRAGMA user_version=4;');
    });
    if (version < 5) this.transaction(() => {
      this.db.exec(`
        ALTER TABLE desktops ADD COLUMN pve_available INTEGER NOT NULL DEFAULT 1 CHECK(pve_available IN (0,1));
        ALTER TABLE desktops ADD COLUMN pve_node TEXT;
        ALTER TABLE desktops ADD COLUMN pve_power_status TEXT NOT NULL DEFAULT 'unknown';
        ALTER TABLE desktops ADD COLUMN pve_seen_at INTEGER;
        PRAGMA user_version=5;
      `);
    });
    if (version < 6) this.transaction(() => {
      this.db.exec(`
        ALTER TABLE desktops ADD COLUMN pve_os_type TEXT;
        ALTER TABLE desktops ADD COLUMN pve_os_family TEXT NOT NULL DEFAULT 'unknown';
        ALTER TABLE desktops ADD COLUMN pve_os_source TEXT NOT NULL DEFAULT 'unknown';
        CREATE TABLE hosts (
          id TEXT PRIMARY KEY, desktop_id TEXT NOT NULL UNIQUE REFERENCES desktops(id) ON DELETE CASCADE,
          token_hash TEXT NOT NULL UNIQUE, hostname TEXT NOT NULL, machine_uuid TEXT NOT NULL, version TEXT NOT NULL,
          created_at INTEGER NOT NULL, last_seen_at INTEGER, revoked INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE host_enrollments (
          code_hash TEXT PRIMARY KEY, desktop_id TEXT NOT NULL UNIQUE REFERENCES desktops(id) ON DELETE CASCADE,
          machine_uuid TEXT NOT NULL, created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL
        );
        PRAGMA user_version=6;
      `);
    });
    if (version < 7) this.transaction(() => {
      this.db.exec(`
        ALTER TABLE desktops ADD COLUMN pve_power_checked_at INTEGER;
        ALTER TABLE desktops ADD COLUMN pve_power_error TEXT;
        UPDATE desktops SET pve_power_checked_at=pve_seen_at WHERE pve_vmid IS NOT NULL;
        CREATE TABLE power_tasks (
          id TEXT PRIMARY KEY, desktop_id TEXT NOT NULL REFERENCES desktops(id) ON DELETE CASCADE,
          user_id TEXT NOT NULL,
          action TEXT NOT NULL CHECK(action IN ('start','shutdown','reboot')),
          status TEXT NOT NULL CHECK(status IN ('submitted','running','stopped','unknown')),
          active INTEGER NOT NULL DEFAULT 1, submitted_at INTEGER NOT NULL, checked_at INTEGER,
          guard_until INTEGER NOT NULL, config_stamp TEXT NOT NULL, pve_node TEXT, pve_task TEXT, exit_status TEXT
        );
        CREATE UNIQUE INDEX power_tasks_active_vm ON power_tasks(desktop_id) WHERE active=1;
        CREATE INDEX power_tasks_owner ON power_tasks(user_id,desktop_id,submitted_at);
        PRAGMA user_version=7;
      `);
    });
    if (version < 8) {
      try { this.transaction(() => {
      // SQLite CHECK constraints require rebuilding the table. Preserve rowid
      // ordering, task guards, existing results, owner binding and FK cascades.
      this.db.exec(`
        CREATE TABLE power_tasks_next (
          id TEXT PRIMARY KEY, desktop_id TEXT NOT NULL REFERENCES desktops(id) ON DELETE CASCADE,
          user_id TEXT NOT NULL,
          action TEXT NOT NULL CHECK(action IN ('start','shutdown','reboot','stop')),
          status TEXT NOT NULL CHECK(status IN ('submitted','running','stopped','unknown','superseded')),
          active INTEGER NOT NULL DEFAULT 1, submitted_at INTEGER NOT NULL, checked_at INTEGER,
          guard_until INTEGER NOT NULL, config_stamp TEXT NOT NULL, pve_node TEXT, pve_task TEXT, exit_status TEXT
        );
        INSERT INTO power_tasks_next(rowid,id,desktop_id,user_id,action,status,active,submitted_at,checked_at,guard_until,config_stamp,pve_node,pve_task,exit_status)
          SELECT rowid,id,desktop_id,user_id,action,status,active,submitted_at,checked_at,guard_until,config_stamp,pve_node,pve_task,exit_status FROM power_tasks;
        DROP TABLE power_tasks;
        ALTER TABLE power_tasks_next RENAME TO power_tasks;
        CREATE UNIQUE INDEX power_tasks_active_vm ON power_tasks(desktop_id) WHERE active=1;
        CREATE INDEX power_tasks_owner ON power_tasks(user_id,desktop_id,submitted_at);
        PRAGMA user_version=8;
      `);
      }); } catch (error) { this.db.close(); throw error; }
    }
    if (version < 9) {
      try { this.transaction(() => {
        this.db.exec(`
          ALTER TABLE desktops ADD COLUMN pve_machine_uuid TEXT;
          ALTER TABLE desktops ADD COLUMN pve_machine_checked_at INTEGER;
          CREATE TABLE host_requests (
            id TEXT PRIMARY KEY, key_hash TEXT NOT NULL UNIQUE,
            hostname TEXT NOT NULL, machine_uuid TEXT NOT NULL, version TEXT NOT NULL,
            status TEXT NOT NULL CHECK(status IN ('pending','approved','rejected','expired')),
            created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL, decided_at INTEGER,
            desktop_id TEXT REFERENCES desktops(id) ON DELETE SET NULL,
            host_id TEXT, credentials_sealed TEXT, decision_actor TEXT,
            replace_existing INTEGER NOT NULL DEFAULT 0 CHECK(replace_existing IN (0,1))
          );
          CREATE UNIQUE INDEX host_requests_pending_machine ON host_requests(machine_uuid) WHERE status='pending';
          CREATE INDEX host_requests_expiry ON host_requests(expires_at);
          PRAGMA user_version=9;
        `);
      }); } catch (error) { this.db.close(); throw error; }
    }
    if (version < 10) {
      try { this.transaction(() => {
        // Existing identities, passwords, roles, notes and assignments are untouched.
        this.db.exec('ALTER TABLE users ADD COLUMN clipboard_permissions INTEGER NOT NULL DEFAULT 0 CHECK(clipboard_permissions BETWEEN 0 AND 15); PRAGMA user_version=10;');
      }); } catch (error) { this.db.close(); throw error; }
    }
    if (version < 11) {
      try { this.transaction(() => {
        this.db.exec(`CREATE TABLE host_diagnostics (
          host_id TEXT PRIMARY KEY REFERENCES hosts(id) ON DELETE CASCADE,
          received_at INTEGER NOT NULL, report TEXT NOT NULL CHECK(length(report)<=6144)
        ); PRAGMA user_version=11;`);
      }); } catch (error) { this.db.close(); throw error; }
    }
    if (version < 12) {
      try { this.transaction(() => {
        this.db.exec(`CREATE TABLE client_diagnostics (
          id INTEGER PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
          received_at INTEGER NOT NULL, run_id TEXT NOT NULL, sequence INTEGER NOT NULL,
          report TEXT NOT NULL CHECK(length(report)<=16384), UNIQUE(user_id,run_id,sequence)
        ); CREATE INDEX client_diagnostics_received ON client_diagnostics(received_at); PRAGMA user_version=12;`);
      }); } catch (error) { this.db.close(); throw error; }
    }
    if (version < 13) {
      try { this.transaction(() => {
        // Every retained account starts with all device permissions denied.
        this.db.exec('ALTER TABLE users ADD COLUMN rdp_redirections INTEGER NOT NULL DEFAULT 0 CHECK(rdp_redirections BETWEEN 0 AND 127); PRAGMA user_version=13;');
      }); } catch (error) { this.db.close(); throw error; }
    }
    if (version < 14) {
      try { this.transaction(() => {
        // Permissions move from the account to each assignment (user x VM).
        // Every existing assignment keeps its account's current masks, so an
        // upgrade changes nothing; new assignments start denied. The users
        // columns stay for backups/older readers but are no longer enforced.
        this.db.exec(`
          ALTER TABLE assignments ADD COLUMN clipboard_permissions INTEGER NOT NULL DEFAULT 0 CHECK(clipboard_permissions BETWEEN 0 AND 15);
          ALTER TABLE assignments ADD COLUMN rdp_redirections INTEGER NOT NULL DEFAULT 0 CHECK(rdp_redirections BETWEEN 0 AND 127);
          UPDATE assignments SET
            clipboard_permissions=COALESCE((SELECT u.clipboard_permissions FROM users u WHERE u.id=assignments.user_id),0),
            rdp_redirections=COALESCE((SELECT u.rdp_redirections FROM users u WHERE u.id=assignments.user_id),0);
          PRAGMA user_version=14;
        `);
      }); } catch (error) { this.db.close(); throw error; }
    }
    if (version < 15) {
      try { this.transaction(() => {
        // Desktop organization: Proxmox tags/pool per VM (replaced by every
        // successful inventory) and per-assignment favorite / last use, which
        // live on the assignment row so removing it removes them too.
        this.db.exec(`
          ALTER TABLE desktops ADD COLUMN pve_tags TEXT NOT NULL DEFAULT '[]';
          ALTER TABLE desktops ADD COLUMN pve_pool TEXT;
          ALTER TABLE assignments ADD COLUMN favorite INTEGER NOT NULL DEFAULT 0 CHECK(favorite IN (0,1));
          ALTER TABLE assignments ADD COLUMN last_used_at INTEGER;
          PRAGMA user_version=15;
        `);
      }); } catch (error) { this.db.close(); throw error; }
    }
    if (version < 16) {
      try { this.transaction(() => {
        // VM hardware changes are separate from clipboard and the device
        // permissions. Existing and new assignments start denied.
        this.db.exec('ALTER TABLE assignments ADD COLUMN sound_card_add INTEGER NOT NULL DEFAULT 0 CHECK(sound_card_add IN (0,1)); PRAGMA user_version=16;');
      }); } catch (error) { this.db.close(); throw error; }
    }
    if (version < 17) {
      try { this.transaction(() => {
        // Historical tokens cannot reliably distinguish Windows from Android.
        this.db.exec(`ALTER TABLE sessions ADD COLUMN issuer TEXT NOT NULL DEFAULT 'legacy' CHECK(issuer IN ('legacy','windows-pc-v1','browser-console','admin'));
          ALTER TABLE sessions ADD COLUMN replaced_at INTEGER CHECK(replaced_at IS NULL OR (issuer='windows-pc-v1' AND replaced_at>=0));
          CREATE UNIQUE INDEX sessions_windows_owner ON sessions(user_id) WHERE issuer='windows-pc-v1' AND replaced_at IS NULL;
          CREATE INDEX sessions_replaced ON sessions(replaced_at) WHERE replaced_at IS NOT NULL;
          PRAGMA user_version=17;`);
      }); } catch (error) { this.db.close(); throw error; }
    }
    if (version < 18) {
      try { this.transaction(() => { this.db.exec(sessionAccessSchema); this.db.exec('PRAGMA user_version=18;'); }); }
      catch (error) { this.db.close(); throw error; }
    }
    if (version < 19) {
      try { this.transaction(() => {
        this.db.exec(readFileSync(new URL('./mfa-schema.sql', import.meta.url), 'utf8'));
        this.db.exec('PRAGMA user_version=19;');
      }); } catch (error) { this.db.close(); throw error; }
    }
    // Growing MFA blocks and the daily cap (mfa.mjs): three additive columns of
    // the lockout counters, which never travel in a backup. Older brokers
    // write the table without them and keep their defaults; no version change.
    for (const [name, sql] of [['block_streak', 'block_streak INTEGER NOT NULL DEFAULT 0 CHECK(block_streak>=0)'], ['day_started_at', 'day_started_at INTEGER NOT NULL DEFAULT 0 CHECK(day_started_at>=0)'], ['day_failures', 'day_failures INTEGER NOT NULL DEFAULT 0 CHECK(day_failures>=0)']]) {
      if (!this.db.prepare("SELECT 1 FROM pragma_table_info('mfa_failure_budgets') WHERE name=?").get(name)) this.db.exec('ALTER TABLE mfa_failure_budgets ADD COLUMN ' + sql);
    }
    // Several live requests may claim one machine; they are shown together and
    // approval is guarded (host-requests.mjs) instead of letting the first
    // claimant hold the only visible request. Idempotent and compatible both
    // ways (released brokers check for a pending claim before inserting), so
    // no schema version change.
    this.db.exec('DROP INDEX IF EXISTS host_requests_pending_machine; CREATE INDEX IF NOT EXISTS host_requests_machine ON host_requests(machine_uuid,status);');
    // Additive and fail-closed on older brokers: an explicit code-verified
    // approval binds one physical PC to its current Host, never a bare pc row.
    this.db.exec(`CREATE TABLE IF NOT EXISTS local_pc_approvals (
      desktop_id TEXT PRIMARY KEY REFERENCES desktops(id) ON DELETE CASCADE,
      host_id TEXT NOT NULL UNIQUE, machine_uuid TEXT NOT NULL UNIQUE,
      approved_at INTEGER NOT NULL, actor TEXT NOT NULL)`);
    // Additive too (pc-power.mjs): the wake addresses a PC's Host last
    // reported, kept while the PC is off. Server and administrator data only.
    this.db.exec(`CREATE TABLE IF NOT EXISTS pc_wake (
      desktop_id TEXT PRIMARY KEY REFERENCES desktops(id) ON DELETE CASCADE,
      host_id TEXT NOT NULL, adapters TEXT NOT NULL, reported_at INTEGER NOT NULL)`);
    // Rolling Host event history (host-diagnostics.mjs recordHostEventHistory).
    // Additive like the table above: older brokers ignore it, no version change.
    this.db.exec(`CREATE TABLE IF NOT EXISTS host_diagnostic_events (
      id INTEGER PRIMARY KEY, host_id TEXT NOT NULL REFERENCES hosts(id) ON DELETE CASCADE,
      received_at INTEGER NOT NULL, version TEXT NOT NULL, row TEXT NOT NULL CHECK(length(row)<=2048),
      UNIQUE(host_id,row));
      CREATE INDEX IF NOT EXISTS host_diagnostic_events_received ON host_diagnostic_events(received_at);`);
    // Retention (retention.mjs, client-diagnostics.mjs): each batch's kind is
    // stored once instead of ranked through json_each on every upload. Additive:
    // older brokers insert without it, and the next start of this one fills
    // their rows in before anything reads them. No version change. A damaged
    // row stays unranked (deleted first) instead of stopping the start.
    if (!this.db.prepare("SELECT 1 FROM pragma_table_info('client_diagnostics') WHERE name='evidence'").get()) this.db.exec('ALTER TABLE client_diagnostics ADD COLUMN evidence INTEGER');
    this.db.exec(`UPDATE client_diagnostics SET evidence=${clientDiagnosticEvidenceSql} WHERE evidence IS NULL AND json_valid(report);
      CREATE INDEX IF NOT EXISTS client_diagnostics_user_evidence ON client_diagnostics(user_id,evidence);
      CREATE INDEX IF NOT EXISTS client_diagnostics_evidence ON client_diagnostics(evidence);
      CREATE INDEX IF NOT EXISTS audit_events_created ON audit_events(created_at);`);
    // Usage history of the 모니터링 page (usage.mjs): two tables and three triggers
    // on the sign-in tables, additive like the tables above (no version change),
    // and once what audit_events and client reports already hold.
    installUsage(this);
    // 공지 보내기 (notices.mjs): three tables, additive like the ones above (no version change).
    installNotices(this);
    // Old releases could already have cached UUID B beside a Host for UUID A.
    // Repair only known mismatches; do not manufacture a fresh Proxmox check.
    this.transaction(() => {
      for (const desktop of this.db.prepare("SELECT id,pve_machine_uuid FROM desktops WHERE kind='vm' AND pve_machine_uuid IS NOT NULL").all()) {
        const uuid = verifiedMachineUuid(desktop.pve_machine_uuid);
        if (uuid) this.revokeMismatchedHosts(desktop.id, uuid, 'server-start');
        else this.db.prepare('UPDATE desktops SET pve_machine_uuid=NULL,pve_machine_checked_at=NULL WHERE id=?').run(desktop.id);
      }
    });
    // One time: an older database can hold the assignments a removed or
    // replaced Host left behind, and they would come back with the
    // next Host registered under the same VM number. Assignments of desktops
    // without a current (not revoked) Host go; accounts, desktops and the
    // assignments of registered Hosts stay. The settings mark makes every
    // later start a no-op; no schema version change, older brokers ignore it.
    this.transaction(() => {
      if (this.setting('hostless_assignments_cleanup') !== undefined) return;
      const removed = this.db.prepare('DELETE FROM assignments WHERE NOT EXISTS (SELECT 1 FROM hosts h WHERE h.desktop_id=assignments.desktop_id AND h.revoked=0)').run().changes;
      this.db.prepare("INSERT INTO settings(key,value) VALUES('hostless_assignments_cleanup',?)").run(String(removed));
      if (removed) this.audit('hostless_assignments_removed', 'server-start', String(removed));
    });
    // Preserve successful commissioning across upgrades and later outages.
    // A stored token alone is not proof that the broker reached Proxmox.
    if (this.setting('proxmox') && this.setting('proxmox_initialized') !== '1') {
      let sync = {};
      try { sync = JSON.parse(this.setting('proxmox_sync') ?? '{}'); } catch { /* Unverified configuration stays gated. */ }
      if ((sync.state === 'ok' || (Number.isInteger(sync.count) && Number.isFinite(Date.parse(sync.checkedAt)))) || this.db.prepare("SELECT 1 FROM audit_events WHERE event='proxmox_synchronized' LIMIT 1").get()) {
        this.db.prepare("INSERT INTO settings(key,value) VALUES('proxmox_initialized','1') ON CONFLICT(key) DO UPDATE SET value=excluded.value").run();
      }
    }
    if (path !== ':memory:') chmodSync(path, 0o600);
  }
  close() { this.db.close(); }
  transaction(action) {
    if (this.db.isTransaction) {
      const value = action();
      if (value && typeof value.then === 'function') throw Error('transaction_must_be_synchronous');
      return value;
    }
    this.db.exec('BEGIN IMMEDIATE');
    try { const result = action(); if (result && typeof result.then === 'function') throw Error('transaction_must_be_synchronous'); this.db.exec('COMMIT'); return result; }
    catch (error) { this.db.exec('ROLLBACK'); throw error; }
  }
  audit(event, actor, subject, now = Date.now()) {
    this.db.prepare('INSERT INTO audit_events(created_at,event,actor,subject) VALUES(?,?,?,?)').run(now, event, actor, subject);
  }
  // Compatibility: an account-wide clipboardPolicy/devicePolicy is
  // validated and applies to the account's current assignments, of which a new
  // account has none. Its later assignments therefore start denied.
  addUser({ name, notes, displayName, passwordHash, role = 'user', clipboardPolicy, devicePolicy }, actor = 'server-admin') {
    const canonical = username(name);
    if (!['admin', 'user'].includes(role)) throw new Error('invalid_role');
    const memo = accountNotes(notes === undefined ? (displayName !== undefined && displayName !== canonical ? displayName : '') : notes);
    if (clipboardPolicy !== undefined) clipboardPolicyMask(clipboardPolicy);
    if (devicePolicy !== undefined) devicePolicyMask(devicePolicy);
    const id = randomUUID();
    this.transaction(() => {
      const primary = role === 'admin' && !this.db.prepare('SELECT 1 FROM users WHERE is_primary_admin=1').get();
      this.db.prepare('INSERT INTO users(id,username,display_name,notes,password_hash,role,created_at,is_primary_admin) VALUES(?,?,?,?,?,?,?,?)').run(id, canonical, canonical, memo, passwordHash, role, Date.now(), primary ? 1 : 0);
      this.audit('user_created', actor, id);
    });
    return id;
  }
  findUser(name) { return this.db.prepare('SELECT * FROM users WHERE username=?').get(username(name)); }
  findUserById(id) { return this.db.prepare('SELECT * FROM users WHERE id=?').get(id); }
  // Enforcement reads the assignment (user x VM) of an enabled account. A
  // missing assignment, disabled account or absent VM id grants nothing.
  clipboardPolicy(userId, id) { return clipboardPolicyFromMask(typeof id === 'string' ? reusable(this.db, 'SELECT a.clipboard_permissions FROM assignments a JOIN users u ON u.id=a.user_id WHERE a.user_id=? AND a.desktop_id=? AND u.disabled=0').get(userId, id)?.clipboard_permissions : undefined); }
  devicePolicy(userId, id) { return devicePolicyFromMask(typeof id === 'string' ? reusable(this.db, 'SELECT a.rdp_redirections FROM assignments a JOIN users u ON u.id=a.user_id WHERE a.user_id=? AND a.desktop_id=? AND u.disabled=0').get(userId, id)?.rdp_redirections : undefined); }
  // The action layer owns any administrator bypass and still checks the VM.
  soundCardAddAllowed(userId, id) {
    if (typeof userId !== 'string' || typeof id !== 'string') return false;
    return reusable(this.db, `SELECT a.sound_card_add FROM assignments a JOIN users u ON u.id=a.user_id JOIN desktops d ON d.id=a.desktop_id WHERE a.user_id=? AND a.desktop_id=? AND u.disabled=0 AND d.kind='vm' AND ${availableDesktopSql}`).get(userId, id)?.sound_card_add === 1;
  }
  userAssignments(userId) {
    return this.db.prepare('SELECT a.desktop_id,a.clipboard_permissions,a.rdp_redirections,a.sound_card_add FROM assignments a JOIN desktops d ON d.id=a.desktop_id WHERE a.user_id=? ORDER BY d.pve_vmid IS NULL,d.pve_vmid,a.desktop_id').all(userId)
      .map(row => ({ desktopId: row.desktop_id, ...assignmentPolicy(row) }));
  }
  // Inside a transaction: set one existing assignment's masks (undefined keeps
  // a mask) and audit each actual change. Returns whether anything changed.
  writeAssignmentPolicy(userId, id, clipboard, redirections, actor, soundCardAdd) {
    soundCardAddValue(soundCardAdd);
    const row = this.db.prepare('SELECT clipboard_permissions,rdp_redirections,sound_card_add FROM assignments WHERE user_id=? AND desktop_id=?').get(userId, id);
    if (!row) return false;
    // A saved device policy leaves the bits of no permission as the row has them.
    const nextClipboard = clipboard ?? row.clipboard_permissions, nextRedirections = redirections === undefined ? row.rdp_redirections : redirections | (row.rdp_redirections & ~devicePolicyBits);
    const nextSoundCardAdd = soundCardAdd === undefined ? row.sound_card_add : (soundCardAdd ? 1 : 0);
    if (nextClipboard === row.clipboard_permissions && nextRedirections === row.rdp_redirections && nextSoundCardAdd === row.sound_card_add) return false;
    this.db.prepare('UPDATE assignments SET clipboard_permissions=?,rdp_redirections=?,sound_card_add=? WHERE user_id=? AND desktop_id=?').run(nextClipboard, nextRedirections, nextSoundCardAdd, userId, id);
    if (nextClipboard !== row.clipboard_permissions) this.audit('clipboard_policy_changed', actor, `${userId}:${id}`);
    if (nextRedirections !== row.rdp_redirections) this.audit('device_policy_changed', actor, `${userId}:${id}`);
    if (nextSoundCardAdd !== row.sound_card_add) this.audit('sound_card_add_permission_changed', actor, `${userId}:${id}`);
    return true;
  }
  updateAssignmentPolicy(name, id, { clipboardPolicy, devicePolicy, soundCardAdd } = {}, actor = 'server-admin') {
    const clipboard = clipboardPolicy === undefined ? undefined : clipboardPolicyMask(clipboardPolicy);
    const redirections = devicePolicy === undefined ? undefined : devicePolicyMask(devicePolicy);
    soundCardAddValue(soundCardAdd);
    return this.transaction(() => {
      const user = this.findUser(name);
      if (!user) throw new Error('user_not_found');
      if (!this.db.prepare('SELECT 1 FROM assignments WHERE user_id=? AND desktop_id=?').get(user.id, desktopId(id))) throw new Error('assignment_not_found');
      if (soundCardAdd === true && this.db.prepare('SELECT kind FROM desktops WHERE id=?').get(id)?.kind !== 'vm') throw new Error('sound_card_vm_only');
      return this.writeAssignmentPolicy(user.id, id, clipboard, redirections, actor, soundCardAdd);
    });
  }
  completeInitialSetup(userId, name, passwordHash) {
    const canonical = username(name);
    this.transaction(() => {
      const current = this.db.prepare('SELECT * FROM users WHERE id=? AND disabled=0 AND must_change_credentials=1').get(userId);
      if (!current || current.role !== 'admin') throw new Error('setup_not_required');
      if (canonical === current.username) throw new Error('username_must_change');
      this.db.prepare('UPDATE users SET username=?,display_name=?,password_hash=?,must_change_credentials=0 WHERE id=?').run(canonical, canonical, passwordHash, userId);
      this.db.prepare('DELETE FROM sessions WHERE user_id=?').run(userId);
      // The one-time setup code (initial-setup.mjs) is spent with the setup.
      this.db.prepare("DELETE FROM settings WHERE key='initial_setup_code'").run();
      this.audit('initial_setup_completed', userId, canonical);
    });
  }
  expectedUser(name, expectedId) {
    if (expectedId !== undefined && (typeof expectedId !== 'string' || !/^[A-Za-z0-9_-]{1,128}$/.test(expectedId))) throw new Error('invalid_account_identity');
    const user = this.findUser(name);
    if (expectedId !== undefined && (!user || user.id !== expectedId)) throw new Error('account_identity_changed');
    if (!user) throw new Error('user_not_found');
    return user;
  }
  // The primary administrator's password is changed only by that account
  // itself or on this server (the CLI and setup act as 'server-admin'), never
  // by another administrator.
  changePassword(name, passwordHash, actor = 'server-admin', expectedId) {
    this.transaction(() => {
      const user = this.expectedUser(name, expectedId);
      if (user.is_primary_admin && actor !== 'server-admin' && actor !== user.id) throw new Error('primary_admin_protected');
      this.db.prepare('UPDATE users SET password_hash=? WHERE id=?').run(passwordHash, user.id);
      this.db.prepare('DELETE FROM sessions WHERE user_id=?').run(user.id);
      this.audit('password_changed', actor, user.id);
    });
  }
  changeOwnPassword(userId, expectedHash, passwordHash, sessionHash) {
    this.transaction(() => {
      // HTTP callers must retain the exact client session through commit. Keep
      // the trusted three-argument internal path and password-hash CAS intact.
      if (sessionHash !== undefined) {
        const session = this.authenticate(sessionHash);
        if (!session || session.id !== userId || session.must_change_credentials) throw new Error('session_expired');
      }
      const current = this.db.prepare('SELECT password_hash,disabled FROM users WHERE id=?').get(userId);
      if (!current || current.disabled || current.password_hash !== expectedHash) throw new Error('credentials_changed');
      this.db.prepare('UPDATE users SET password_hash=? WHERE id=?').run(passwordHash, userId);
      this.db.prepare('DELETE FROM sessions WHERE user_id=?').run(userId);
      this.audit('password_changed', userId, userId);
    });
  }
  issueEnrollment(id, machineUuid, codeHash, actor, now = Date.now()) {
    const desktop = this.db.prepare("SELECT id FROM desktops WHERE id=? AND kind='vm' AND pve_vmid IS NOT NULL AND pve_available=1").get(id);
    if (!desktop) throw new Error('desktop_not_found');
    const expiresAt = now + 10 * 60 * 1000;
    this.transaction(() => {
      machineUuid = this.bindMachineIdentity(id, machineUuid, actor, now);
      this.db.prepare('DELETE FROM host_enrollments WHERE desktop_id=? OR expires_at<=?').run(id, now);
      this.db.prepare('INSERT INTO host_enrollments(code_hash,desktop_id,machine_uuid,created_at,expires_at) VALUES(?,?,?,?,?)').run(codeHash, id, machineUuid, now, expiresAt);
      this.audit('host_enrollment_created', actor, id);
    });
    return expiresAt;
  }
  pendingEnrollment(codeHash, now = Date.now()) {
    return this.db.prepare('SELECT e.*,d.pve_vmid,d.pve_node FROM host_enrollments e JOIN desktops d ON d.id=e.desktop_id WHERE e.code_hash=? AND e.expires_at>? AND d.pve_available=1').get(codeHash, now);
  }
  consumeEnrollment(codeHash, machineUuid, { hostname, version, tokenHash }, now = Date.now()) {
    return this.transaction(() => {
      machineUuid = verifiedMachineUuid(machineUuid);
      const enrollment = this.pendingEnrollment(codeHash, now);
      if (!enrollment || enrollment.machine_uuid !== machineUuid) throw new Error('invalid_enrollment');
      const id = randomUUID();
      this.db.prepare('DELETE FROM hosts WHERE desktop_id=?').run(enrollment.desktop_id);
      this.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run(id, enrollment.desktop_id, tokenHash, hostname, machineUuid, version, now);
      this.bindMachineIdentity(enrollment.desktop_id, machineUuid, id, now);
      this.db.prepare('DELETE FROM host_enrollments WHERE code_hash=?').run(codeHash);
      this.audit('host_enrolled', id, enrollment.desktop_id, now);
      return { hostId: id, desktopId: enrollment.desktop_id };
    });
  }
  authenticateHost(hash) {
    return reusable(this.db, `SELECT h.id,h.desktop_id,h.hostname,h.version FROM hosts h JOIN desktops d ON d.id=h.desktop_id WHERE h.token_hash=? AND h.revoked=0 AND ((d.kind='vm' AND d.pve_available=1 AND (d.pve_machine_uuid IS NULL OR lower(h.machine_uuid)=lower(d.pve_machine_uuid))) OR (${approvedPcSql} AND EXISTS(SELECT 1 FROM local_pc_approvals p WHERE p.desktop_id=d.id AND p.host_id=h.id)))`).get(hash);
  }
  // host-machine-v1: the SMBIOS UUID the Host was approved on (VM or local PC), lowercase.
  hostMachineUuid(hostId) {
    const value = this.db.prepare('SELECT machine_uuid FROM hosts WHERE id=? AND revoked=0').get(hostId)?.machine_uuid;
    return typeof value === 'string' && value ? value.toLowerCase() : null;
  }
  registeredHostIds() {
    return new Set(this.db.prepare(`SELECT d.id FROM desktops d WHERE ${registeredDesktopSql}`).all().map(row => row.id));
  }
  registeredVmTargets() {
    return this.db.prepare(`SELECT d.id AS desktopId,h.id AS hostId,d.pve_node AS node,d.pve_vmid AS vmid,lower(COALESCE(d.pve_machine_uuid,h.machine_uuid)) AS expectedUuid
      FROM desktops d JOIN hosts h ON h.desktop_id=d.id AND h.revoked=0
      WHERE d.kind='vm' AND d.pve_vmid IS NOT NULL AND ${registeredDesktopSql} ORDER BY d.id`).all().map(row => ({ ...row }));
  }
  // Only a definitive result from the exact registered config can quarantine
  // or restore availability. Unknown/status-only samples never assert identity.
  // The SQL CAS also protects direct callers from a replaced registration.
  observeRegisteredVmAvailability(target, observation) {
    if (!target || !observation || typeof target.desktopId !== 'string' || typeof target.hostId !== 'string' ||
        typeof target.node !== 'string' || !Number.isSafeInteger(target.vmid) || !verifiedMachineUuid(target.expectedUuid) ||
        observation.node !== target.node || observation.vmid !== target.vmid) throw new Error('invalid_proxmox_inventory');
    let available;
    if (observation.powerError === 'proxmox_machine_uuid_mismatch') {
      if (observation.status !== 'unknown' || observation.expectedUuid !== target.expectedUuid || Object.hasOwn(observation, 'machineUuid')) throw new Error('invalid_proxmox_inventory');
      available = 0;
    } else if (observation.powerError !== undefined || observation.machineUuid === undefined) return null;
    else {
      if (verifiedMachineUuid(observation.machineUuid) !== target.expectedUuid) throw new Error('invalid_proxmox_inventory');
      available = 1;
    }
    return this.db.prepare(`UPDATE desktops SET pve_available=? WHERE id=? AND kind='vm' AND pve_node=? AND pve_vmid=?
      AND EXISTS(SELECT 1 FROM hosts h WHERE h.id=? AND h.desktop_id=desktops.id AND h.revoked=0
        AND lower(h.machine_uuid)=? AND lower(COALESCE(desktops.pve_machine_uuid,h.machine_uuid))=?)`)
      .run(available, target.desktopId, target.node, target.vmid, target.hostId, target.expectedUuid, target.expectedUuid).changes === 1;
  }
  // A registered VM that Proxmox now keeps on another node (migration, HA
  // recovery) or under another VM ID (a backup restored to a new number): the
  // desktop row follows it and keeps its id, assignments, permissions and Host
  // registration. Only vm-relocate.mjs calls this, after Proxmox showed the
  // recorded SMBIOS UUID on exactly that one VM. The row must still be the
  // registration that failed (CAS on node, VM ID, UUID and Host); a VM ID that
  // another desktop row already has is refused. Returns 'moved', 'changed' or
  // 'conflict'. Audited as desktopId:oldNode:oldVmid:newNode:newVmid.
  relocateRegisteredVm(target, to, actor = 'server', now = Date.now()) {
    const node = value => typeof value === 'string' && /^[A-Za-z0-9_.-]{1,128}$/.test(value) && value !== '.' && value !== '..';
    const vmid = value => Number.isSafeInteger(value) && value >= 100 && value <= 999999999;
    if (!target || typeof target.desktopId !== 'string' || typeof target.hostId !== 'string' || !node(target.node) || !vmid(target.vmid) || !verifiedMachineUuid(target.expectedUuid) ||
        !to || !node(to.node) || !vmid(to.vmid) || (to.node === target.node && to.vmid === target.vmid)) throw new Error('invalid_proxmox_relocation');
    return this.transaction(() => {
      const row = this.db.prepare("SELECT id,name,description,pve_node,pve_vmid,pve_machine_uuid FROM desktops WHERE id=? AND kind='vm'").get(target.desktopId);
      if (!row || row.pve_node !== target.node || row.pve_vmid !== target.vmid || verifiedMachineUuid(row.pve_machine_uuid) !== target.expectedUuid ||
          !this.db.prepare('SELECT 1 FROM hosts WHERE id=? AND desktop_id=? AND revoked=0 AND lower(machine_uuid)=?').get(target.hostId, row.id, target.expectedUuid)) return 'changed';
      if (to.vmid !== target.vmid && this.db.prepare('SELECT 1 FROM desktops WHERE pve_vmid=? AND id<>?').get(to.vmid, row.id)) return 'conflict';
      // Only the texts this server wrote itself carry the VM ID.
      const name = row.name === `VM ${target.vmid}` ? `VM ${to.vmid}` : row.name;
      const description = row.description === `Proxmox VM ${target.vmid}` ? `Proxmox VM ${to.vmid}` : row.description;
      this.db.prepare("UPDATE desktops SET pve_node=?,pve_vmid=?,name=?,description=? WHERE id=? AND kind='vm'").run(to.node, to.vmid, name, description, row.id);
      this.qemuAgentOption.delete(row.id);
      this.audit('desktop_vm_relocated', actor, `${row.id}:${target.node}:${target.vmid}:${to.node}:${to.vmid}`, now);
      return 'moved';
    });
  }
  refreshRegisteredVms(targets, observations, actor, authorize = () => {}, now = Date.now()) {
    if (!Array.isArray(targets) || !Array.isArray(observations) || targets.length !== observations.length) throw new Error('invalid_proxmox_inventory');
    const errors = new Set(['proxmox_unreachable','proxmox_permission_denied','proxmox_request_failed','proxmox_invalid_response','proxmox_certificate_error','proxmox_vm_not_found','proxmox_machine_uuid_mismatch','proxmox_machine_uuid_missing']);
    const rows = new Map();
    for (const row of observations) {
      if (!row || typeof row !== 'object' || Array.isArray(row) || typeof row.node !== 'string' || !Number.isSafeInteger(row.vmid) || rows.has(row.node + ':' + row.vmid) || !['running','stopped','unknown'].includes(row.status)) throw new Error('invalid_proxmox_inventory');
      rows.set(row.node + ':' + row.vmid, row);
    }
    for (const target of targets) {
      const row = rows.get(target.node + ':' + target.vmid);
      if (!row || !verifiedMachineUuid(target.expectedUuid) || (row.powerError !== undefined
        ? row.status !== 'unknown' || row.expectedUuid !== target.expectedUuid || !errors.has(row.powerError) || row.machineUuid !== undefined
        : verifiedMachineUuid(row.machineUuid) !== target.expectedUuid)) throw new Error('invalid_proxmox_inventory');
    }
    return this.transaction(() => {
      authorize();
      if (JSON.stringify(this.registeredVmTargets()) !== JSON.stringify(targets)) throw new Error('host_registration_changed');
      let unavailable = 0;
      for (const target of targets) {
        const row = rows.get(target.node + ':' + target.vmid);
        if (this.observeRegisteredVmAvailability(target, row) === false) throw new Error('host_registration_changed');
        if (row.powerError !== undefined) {
          this.db.prepare('UPDATE desktops SET pve_power_status=\'unknown\',pve_power_checked_at=?,pve_power_error=? WHERE id=?').run(now, row.powerError, target.desktopId);unavailable++;
          this.qemuAgentOption.delete(target.desktopId);
        } else {
          if (typeof row.qemuAgent === 'boolean') this.qemuAgentOption.set(target.desktopId, row.qemuAgent); else this.qemuAgentOption.delete(target.desktopId);
          this.db.prepare('UPDATE desktops SET pve_power_status=?,pve_power_checked_at=?,pve_power_error=NULL,pve_seen_at=?,pve_machine_uuid=?,pve_machine_checked_at=?,pve_os_type=?,pve_os_family=?,pve_os_source=? WHERE id=?').run(row.status,now,now,target.expectedUuid,now,row.pveOsType??null,row.osFamily??'unknown',row.osSource??'unknown',target.desktopId);
          const current = this.db.prepare('SELECT pve_tags,pve_pool FROM desktops WHERE id=?').get(target.desktopId);
          const tags = row.tags === undefined ? current.pve_tags : JSON.stringify(normalizeTags(row.tags));
          const pool = row.pool === undefined ? current.pve_pool : normalizePool(row.pool);
          this.db.prepare('UPDATE desktops SET pve_tags=?,pve_pool=? WHERE id=?').run(tags,pool,target.desktopId);
        }
      }
      const result = { state: 'ok', checkedAt: new Date(now).toISOString(), count: targets.length, added: 0, updated: targets.length, unavailable };
      this.db.prepare("INSERT INTO settings(key,value) VALUES('proxmox_sync',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value").run(JSON.stringify(result));
      this.db.prepare("INSERT INTO settings(key,value) VALUES('proxmox_initialized','1') ON CONFLICT(key) DO UPDATE SET value=excluded.value").run();
      this.audit('registered_hosts_refreshed',actor,String(targets.length),now);
      return result;
    });
  }
  removeHostRegistration(id, expectedHostId, actor, guard) {
    if (typeof expectedHostId !== 'string' || !/^[A-Za-z0-9_-]{1,128}$/.test(expectedHostId)) throw new Error('invalid_host_identity');
    if (typeof guard !== 'function') throw new Error('host_removal_guard_required');
    return this.transaction(() => {
      const host = this.db.prepare('SELECT id FROM hosts WHERE desktop_id=? AND revoked=0').get(desktopId(id));
      if (!host || host.id !== expectedHostId || !this.registeredHostIds().has(id)) throw new Error('host_registration_changed');
      // Synchronous final authorization + real scoped activity, inside this
      // same write transaction. No await may separate the guard and revoke.
      guard();
      const kind = this.db.prepare('SELECT kind FROM desktops WHERE id=?').get(id)?.kind;
      this.db.prepare("UPDATE host_requests SET status='rejected',credentials_sealed=NULL WHERE desktop_id=? AND status='approved'").run(id);
      // A removed Host takes its assignments (and
      // with them each user's permissions and favourite) along, so a later
      // Host under the same VM number starts with nobody assigned. The VM
      // itself, its files and the accounts stay.
      const removedAssignments = this.db.prepare('DELETE FROM assignments WHERE desktop_id=?').run(id).changes;
      // Everything else bound to the desktop goes too, so adding it again
      // starts clean: the Host row itself (its token stops
      // working and its reports go by cascade), a pending enrollment, a local
      // PC's approval and wake addresses. A local PC row exists only because
      // it was approved, so it is deleted; a VM row is Proxmox inventory and
      // stays, with the display text an administrator typed reset. History
      // (audit, sign-in and connection records) is kept.
      const changed = this.db.prepare('DELETE FROM hosts WHERE id=? AND desktop_id=? AND revoked=0').run(expectedHostId, id).changes;
      if (changed !== 1) throw new Error('host_registration_changed');
      this.db.prepare('DELETE FROM hosts WHERE desktop_id=?').run(id);
      this.db.prepare('DELETE FROM host_enrollments WHERE desktop_id=?').run(id);
      this.db.prepare('DELETE FROM local_pc_approvals WHERE desktop_id=?').run(id);
      this.db.prepare('DELETE FROM pc_wake WHERE desktop_id=?').run(id);
      if (kind === 'pc') this.db.prepare("DELETE FROM desktops WHERE id=? AND kind='pc'").run(id);
      else this.db.prepare("UPDATE desktops SET description='Proxmox VM ' || pve_vmid WHERE id=? AND kind='vm' AND pve_vmid IS NOT NULL").run(id);
      this.audit('host_registration_removed', actor, `${expectedHostId}:${id}:${removedAssignments}`);
      return { desktopId: id, hostId: expectedHostId, removedAssignments };
    });
  }
  // Inside a transaction: nobody stays assigned to this desktop. Audited only
  // when something was removed (subject desktopId:count).
  dropDesktopAssignments(id, actor, now = Date.now()) {
    const removed = this.db.prepare('DELETE FROM assignments WHERE desktop_id=?').run(id).changes;
    if (removed) this.audit('desktop_assignments_removed', actor, `${id}:${removed}`, now);
    return removed;
  }
  hostConsoleBinding(hash) {
    return this.db.prepare(`SELECT h.id,h.desktop_id,h.machine_uuid FROM hosts h JOIN desktops d ON d.id=h.desktop_id WHERE h.token_hash=? AND h.revoked=0 AND ((d.kind='vm' AND d.pve_available=1 AND (d.pve_machine_uuid IS NULL OR lower(h.machine_uuid)=lower(d.pve_machine_uuid))) OR (${approvedPcSql} AND EXISTS(SELECT 1 FROM local_pc_approvals p WHERE p.desktop_id=d.id AND p.host_id=h.id)))`).get(hash);
  }
  revokeMismatchedHosts(id, uuid, actor, now = Date.now()) {
    const changed = this.db.prepare('UPDATE hosts SET revoked=1 WHERE desktop_id=? AND revoked=0 AND lower(machine_uuid)<>?').run(id, uuid).changes;
    this.db.prepare('DELETE FROM host_enrollments WHERE desktop_id=? AND lower(machine_uuid)<>?').run(id, uuid);
    this.db.prepare("UPDATE host_requests SET status='rejected',credentials_sealed=NULL WHERE desktop_id=? AND status='approved' AND host_id IN (SELECT id FROM hosts WHERE desktop_id=? AND revoked=1)").run(id, id);
    if (changed) {
      this.audit('host_identity_changed', actor, id, now);
      // A different machine under this VM number must not inherit the users
      // of the one that was registered before.
      this.dropDesktopAssignments(id, actor, now);
    }
    return changed;
  }
  // Called only with a fresh authoritative Proxmox identity (inventory or
  // administrator enrollment). Unknown/failed lookups must never call this.
  bindMachineIdentity(id, value, actor, now = Date.now()) {
    const uuid = verifiedMachineUuid(value);
    if (!uuid) throw new Error('invalid_machine_uuid');
    const previous = verifiedMachineUuid(this.db.prepare('SELECT pve_machine_uuid FROM desktops WHERE id=?').get(id)?.pve_machine_uuid);
    this.revokeMismatchedHosts(id, uuid, actor, now);
    // The VM number now belongs to another machine even when no Host of the
    // old one was left to revoke.
    if (previous && previous !== uuid) this.dropDesktopAssignments(id, actor, now);
    this.db.prepare('UPDATE desktops SET pve_machine_uuid=?,pve_machine_checked_at=? WHERE id=?').run(uuid, now, id);
    return uuid;
  }
  // Inside the transaction that replaces a desktop's Host row, before the old
  // row is deleted: a Host of the same machine (a reinstall) keeps the
  // assignments, a Host of another machine starts with none.
  replacingHost(id, value, actor, now = Date.now()) {
    const uuid = verifiedMachineUuid(value);
    if (!uuid) throw new Error('invalid_machine_uuid');
    const previous = this.db.prepare('SELECT machine_uuid FROM hosts WHERE desktop_id=?').get(id);
    if (previous && String(previous.machine_uuid).toLowerCase() !== uuid) this.dropDesktopAssignments(id, actor, now);
  }
  hostSeen(id, now = Date.now(), version) {
    if (typeof version === 'string' && version.length <= 32 && /^\d+\.\d+\.\d+$/.test(version)) {
      reusable(this.db, 'UPDATE hosts SET last_seen_at=?,version=? WHERE id=? AND revoked=0').run(now, version, id);
    } else reusable(this.db, 'UPDATE hosts SET last_seen_at=? WHERE id=? AND revoked=0').run(now, id);
  }
  // When the desktop's current Host was last heard from; null without one.
  hostLastSeen(desktopId) {
    const row = reusable(this.db, 'SELECT last_seen_at FROM hosts WHERE desktop_id=? AND revoked=0').get(desktopId);
    return Number.isSafeInteger(row?.last_seen_at) ? row.last_seen_at : null;
  }
  // The desktop's display name (Proxmox / administrator), for its Host's settings title (host.capabilities.ack desktopName).
  desktopName(id) {
    const row = this.db.prepare('SELECT name FROM desktops WHERE id=?').get(id);
    return typeof row?.name === 'string' && row.name.trim() ? row.name.slice(0, 80) : null;
  }
  assignedDesktop(userId, id) {
    return reusable(this.db, `SELECT d.* FROM desktops d JOIN assignments a ON a.desktop_id=d.id JOIN users u ON u.id=a.user_id WHERE a.user_id=? AND d.id=? AND u.disabled=0 AND ${availableDesktopSql}`).get(userId, id);
  }
  guardAdmin(user) {
    if (user.is_primary_admin) throw new Error('primary_admin_required');
    if (user.role === 'admin' && !user.disabled && this.db.prepare("SELECT count(*) AS n FROM users WHERE role='admin' AND disabled=0").get().n <= 1) throw new Error('last_admin_required');
  }
  setUserEnabled(name, enabled, actor = 'server-admin', expectedId) {
    this.transaction(() => {
      const user = this.expectedUser(name, expectedId);
      if (!enabled) this.guardAdmin(user);
      this.db.prepare('UPDATE users SET disabled=? WHERE id=?').run(enabled ? 0 : 1, user.id);
      this.db.prepare('DELETE FROM sessions WHERE user_id=?').run(user.id);
      this.audit(enabled ? 'user_enabled' : 'user_disabled', actor, user.id);
    });
  }
  updateUser(name, { notes, displayName, role, clipboardPolicy, devicePolicy }, actor, expectedId) {
    const clipboard = clipboardPolicy === undefined ? undefined : clipboardPolicyMask(clipboardPolicy);
    const redirections = devicePolicy === undefined ? undefined : devicePolicyMask(devicePolicy);
    this.transaction(() => {
      const user = this.expectedUser(name, expectedId);
      if (role === undefined) role = user.role;
      if (!['admin', 'user'].includes(role)) throw new Error('invalid_role');
      const memo = accountNotes(notes === undefined ? (displayName === undefined ? user.notes : displayName) : notes);
      if (user.is_primary_admin && role !== 'admin') throw new Error('primary_admin_required');
      if (user.role === 'admin' && role !== 'admin') this.guardAdmin(user);
      this.db.prepare('UPDATE users SET notes=?,role=? WHERE id=?').run(memo, role, user.id);
      if (role !== user.role) this.db.prepare('DELETE FROM sessions WHERE user_id=?').run(user.id);
      this.audit('user_updated', actor, user.id);
      // Compatibility: an account-wide value applies to every current assignment.
      if (clipboard !== undefined || redirections !== undefined) {
        for (const { desktop_id: id } of this.db.prepare('SELECT desktop_id FROM assignments WHERE user_id=?').all(user.id)) this.writeAssignmentPolicy(user.id, id, clipboard, redirections, actor);
      }
    });
  }
  deleteUser(name, actor, expectedId) {
    this.transaction(() => {
      const user = this.expectedUser(name, expectedId);
      if (user.is_primary_admin) throw new Error('primary_admin_required');
      if (user.id === actor) throw new Error('cannot_delete_self');
      this.guardAdmin(user); this.db.prepare('DELETE FROM users WHERE id=?').run(user.id); this.audit('user_deleted', actor, user.id);
    });
  }
  addDesktop({ id, name, kind = 'vm', description = '관리자가 할당한 데스크톱', os = 'Windows 11', vmid = null }, actor = 'server-admin') {
    if (!['pc', 'vm'].includes(kind)) throw new Error('invalid_desktop_kind');
    if (vmid !== null && (kind !== 'vm' || !Number.isSafeInteger(vmid) || vmid < 100)) throw new Error('invalid_vmid');
    this.transaction(() => {
      this.db.prepare('INSERT INTO desktops(id,name,kind,description,os,created_at,pve_vmid) VALUES(?,?,?,?,?,?,?)').run(desktopId(id), text(name, 80), kind, text(description, 200), text(os, 80), Date.now(), vmid);
      this.audit('desktop_created', actor, id);
    });
  }
  updateDesktop(id, { name, description, os }, actor = 'server-admin') {
    const old = this.db.prepare('SELECT * FROM desktops WHERE id=?').get(desktopId(id));
    if (!old) throw new Error('desktop_not_found');
    this.transaction(() => {
      this.db.prepare('UPDATE desktops SET name=?,description=?,os=? WHERE id=?').run(text(name ?? old.name, 80), text(description ?? old.description, 200), text(os ?? old.os, 80), id);
      this.audit('desktop_updated', actor, id);
    });
  }
  setDesktopEnabled(id, enabled, actor = 'server-admin') {
    const result = this.db.prepare('UPDATE desktops SET enabled=? WHERE id=?').run(enabled ? 1 : 0, desktopId(id));
    if (!result.changes) throw new Error('desktop_not_found');
    this.audit(enabled ? 'desktop_enabled' : 'desktop_disabled', actor, id);
  }
  deleteDesktop(id, actor) {
    this.transaction(() => {
      if (!this.db.prepare('DELETE FROM desktops WHERE id=?').run(desktopId(id)).changes) throw new Error('desktop_not_found');
      this.audit('desktop_deleted', actor, id);
    });
  }
  assign(name, id, actor = 'server-admin', expectedId) {
    this.transaction(() => {
      const user = expectedId === undefined ? this.findUser(name) : this.expectedUser(name, expectedId);
      const desktop = this.db.prepare(`SELECT d.id FROM desktops d WHERE d.id=? AND ${availableDesktopSql}`).get(desktopId(id));
      if (!user || user.disabled || !desktop) throw new Error('user_or_desktop_unavailable');
      this.db.prepare('INSERT OR IGNORE INTO assignments(user_id,desktop_id) VALUES(?,?)').run(user.id, id);
      this.audit('desktop_assigned', actor, `${user.id}:${id}`);
    });
  }
  revoke(name, id, actor = 'server-admin', expectedId) {
    this.transaction(() => {
      const user = this.expectedUser(name, expectedId);
      this.db.prepare('DELETE FROM assignments WHERE user_id=? AND desktop_id=?').run(user.id, desktopId(id));
      this.audit('assignment_revoked', actor, `${user.id}:${id}`);
    });
  }
  // One atomic per-user update of VM assignments and each VM's permissions.
  // Entries are desktop id strings (legacy) or { desktopId, clipboardPolicy?,
  // devicePolicy?, soundCardAdd? }; omitted policies are retained.
  replaceVmAssignments(name, entries, actor = 'server-admin', expectedId, { registeredOnly = false } = {}) {
    const selected = new Map(assignmentEntries(entries).map(entry => [entry.id, entry]));
    return this.transaction(() => {
      const user = expectedId === undefined ? this.findUser(name) : this.expectedUser(name, expectedId);
      if (!user || user.disabled) throw new Error('user_or_desktop_unavailable');
      const availableRows = this.db.prepare(`SELECT d.id,d.kind FROM desktops d WHERE ${availableDesktopSql}`).all();
      const available = new Set(availableRows.map(row => row.id));
      if ([...selected.keys()].some(id => !available.has(id))) throw new Error('user_or_desktop_unavailable');
      const registered = registeredOnly ? this.registeredHostIds() : null;
      if (registered && [...selected.keys()].some(id => !registered.has(id))) throw new Error('host_not_registered');
      if (availableRows.some(row => row.kind !== 'vm' && selected.get(row.id)?.soundCardAdd === true)) throw new Error('sound_card_vm_only');
      const current = new Set(this.db.prepare('SELECT desktop_id FROM assignments WHERE user_id=?').all(user.id).map(row => row.desktop_id));
      let added = 0, removed = 0, policiesChanged = 0;
      for (const id of current) {
        if (available.has(id) && (!registered || registered.has(id)) && !selected.has(id)) {
          this.db.prepare('DELETE FROM assignments WHERE user_id=? AND desktop_id=?').run(user.id, id); removed++;
        }
      }
      for (const [id, entry] of selected) {
        if (!current.has(id)) {
          this.db.prepare('INSERT INTO assignments(user_id,desktop_id,clipboard_permissions,rdp_redirections,sound_card_add) VALUES(?,?,?,?,?)').run(user.id, id, entry.clipboard ?? 0, entry.redirections ?? 0, entry.soundCardAdd === true ? 1 : 0); added++;
          if (entry.clipboard) this.audit('clipboard_policy_changed', actor, `${user.id}:${id}`);
          if (entry.redirections) this.audit('device_policy_changed', actor, `${user.id}:${id}`);
          if (entry.soundCardAdd === true) this.audit('sound_card_add_permission_changed', actor, `${user.id}:${id}`);
        } else if (this.writeAssignmentPolicy(user.id, id, entry.clipboard, entry.redirections, actor, entry.soundCardAdd)) policiesChanged++;
      }
      const preservedUnavailable = this.db.prepare(registeredOnly
        ? `SELECT count(*) AS n FROM assignments a JOIN desktops d ON d.id=a.desktop_id WHERE a.user_id=? AND NOT ((${availableDesktopSql}) AND (${registeredDesktopSql}))`
        : "SELECT count(*) AS n FROM assignments a JOIN desktops d ON d.id=a.desktop_id WHERE a.user_id=? AND d.kind='vm' AND d.pve_vmid IS NOT NULL AND d.pve_available=0").get(user.id).n;
      this.audit('assignments_replaced', actor, `${user.id}:+${added}:-${removed}`);
      return { added, removed, count: selected.size, preservedUnavailable, policiesChanged };
    });
  }
  // One atomic update of several accounts' permissions on one desktop. Each
  // entry is { username, clipboardPolicy?, devicePolicy?, soundCardAdd? };
  // omitted policies are retained. Every account must be enabled and already
  // have this desktop, or nothing changes.
  replaceDesktopPermissions(id, entries, actor = 'server-admin') {
    if (!Array.isArray(entries) || !entries.length || entries.length > 500) throw new Error('invalid_permission_entries');
    const parsed = entries.map(entry => {
      if (!entry || typeof entry !== 'object' || Array.isArray(entry) || typeof entry.username !== 'string' || Object.keys(entry).some(key => !desktopPermissionKeys.includes(key))) throw new Error('invalid_assignment_entry');
      return { name: entry.username, expectedId: entry.expectedAccountId, clipboard: entry.clipboardPolicy === undefined ? undefined : clipboardPolicyMask(entry.clipboardPolicy), redirections: entry.devicePolicy === undefined ? undefined : devicePolicyMask(entry.devicePolicy), soundCardAdd: soundCardAddValue(entry.soundCardAdd) };
    });
    return this.transaction(() => {
      const desktop = this.db.prepare(`SELECT d.id,d.kind FROM desktops d WHERE d.id=? AND ${availableDesktopSql}`).get(desktopId(id));
      if (!desktop) throw new Error('user_or_desktop_unavailable');
      if (desktop.kind !== 'vm' && parsed.some(entry => entry.soundCardAdd === true)) throw new Error('sound_card_vm_only');
      const users = parsed.map(entry => entry.expectedId === undefined ? this.findUser(entry.name) : this.expectedUser(entry.name, entry.expectedId));
      if (users.some(user => !user || user.disabled)) throw new Error('user_or_desktop_unavailable');
      if (new Set(users.map(user => user.id)).size !== users.length) throw new Error('invalid_assignment_entry');
      if (users.some(user => !this.db.prepare('SELECT 1 FROM assignments WHERE user_id=? AND desktop_id=?').get(user.id, desktop.id))) throw new Error('assignment_changed');
      const changedUserIds = users.filter((user, index) => this.writeAssignmentPolicy(user.id, desktop.id, parsed[index].clipboard, parsed[index].redirections, actor, parsed[index].soundCardAdd)).map(user => user.id);
      this.audit('desktop_permissions_replaced', actor, `${desktop.id}:${changedUserIds.length}`);
      return { count: parsed.length, changedUserIds };
    });
  }
  listDesktops(userId) {
    // Only a current approved Host is a client desktop (removing a Host also
    // removes its assignments). Offline registration remains valid authority.
    return this.db.prepare(`SELECT d.id,d.name,d.kind,d.description,d.os,d.pve_os_type,d.pve_os_family,d.pve_os_source,d.pve_tags,d.pve_pool,a.favorite,a.last_used_at FROM desktops d JOIN assignments a ON a.desktop_id=d.id JOIN users u ON u.id=a.user_id WHERE a.user_id=? AND u.disabled=0 AND ${availableDesktopSql} AND ${registeredDesktopSql} ORDER BY d.pve_vmid IS NULL,d.pve_vmid,d.name,d.id`).all(userId)
      .map(({ pve_tags, pve_pool, favorite, last_used_at, ...row }) => ({ ...row, ...(row.kind === 'pc' ? { localPcApproved: true } : {}), tags: storedTags(pve_tags), pool: pve_pool ?? null, favorite: favorite === 1, lastUsedAt: Number.isFinite(last_used_at) ? new Date(last_used_at).toISOString() : null, status: 'host_not_registered', canConnect: false }));
  }
  // The account's own favorite mark of one assigned VM. False (nothing
  // written) when the VM is not assigned to an enabled account; the caller
  // answers that with the ordinary 404. Last write wins.
  setFavorite(userId, id, favorite) {
    if (typeof favorite !== 'boolean') throw new Error('invalid_favorite');
    return this.db.prepare(`UPDATE assignments SET favorite=? WHERE user_id=? AND desktop_id=? AND EXISTS (SELECT 1 FROM users u WHERE u.id=assignments.user_id AND u.disabled=0) AND EXISTS (SELECT 1 FROM desktops d WHERE d.id=assignments.desktop_id AND ${availableDesktopSql} AND ${registeredDesktopSql})`).run(favorite ? 1 : 0, userId, id).changes > 0;
  }
  // A session grant for this assignment was issued.
  markUsed(userId, id, now = Date.now()) {
    return reusable(this.db, 'UPDATE assignments SET last_used_at=? WHERE user_id=? AND desktop_id=?').run(now, userId, id).changes > 0;
  }
  // Proxmox tags/pool from one successful whole-inventory read. Only VMs the
  // read contains are touched (a missing VM keeps its last values); a VM whose
  // entry carries no tags/pool field at all keeps its values too. Returns the
  // number of VMs whose organization changed. Call inside a transaction.
  applyOrganization(vms) {
    const update = this.db.prepare("UPDATE desktops SET pve_tags=?,pve_pool=? WHERE kind='vm' AND pve_vmid=? AND (pve_tags<>? OR pve_pool IS NOT ?)");
    let changed = 0;
    for (const vm of vms) {
      if (vm.tags === undefined && vm.pool === undefined) continue;
      const row = this.db.prepare("SELECT pve_tags,pve_pool FROM desktops WHERE kind='vm' AND pve_vmid=?").get(vm.vmid);
      if (!row) continue;
      const tags = JSON.stringify(vm.tags === undefined ? storedTags(row.pve_tags) : normalizeTags(vm.tags));
      const pool = vm.pool === undefined ? row.pve_pool : normalizePool(vm.pool);
      changed += update.run(tags, pool, vm.vmid, tags, pool).changes;
    }
    return changed;
  }
  syncProxmox(vms, actor, now = Date.now()) {
    // Validate the complete response before any availability flag changes.
    if (!Array.isArray(vms)) throw new Error('invalid_proxmox_inventory');
    const seen = new Set();
    for (const vm of vms) {
      if (!vm || typeof vm !== 'object' || !Number.isSafeInteger(vm.vmid) || vm.vmid < 100 || seen.has(vm.vmid) || typeof vm.name !== 'string' || !vm.name.trim() || typeof vm.node !== 'string' || !/^[A-Za-z0-9_.-]+$/.test(vm.node) || !['running','stopped','unknown'].includes(vm.status)) throw new Error('invalid_proxmox_inventory');
      if (vm.machineUuid !== undefined && vm.machineUuid !== null && (typeof vm.machineUuid !== 'string' || !/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(vm.machineUuid))) throw new Error('invalid_proxmox_inventory');
      seen.add(vm.vmid);
    }
    return this.transaction(() => {
      let added = 0, updated = 0;
      this.db.prepare("UPDATE desktops SET pve_available=0 WHERE kind='vm' AND pve_vmid IS NOT NULL").run();
      for (const vm of vms) {
        const existing = this.db.prepare("SELECT id FROM desktops WHERE kind='vm' AND pve_vmid=?").get(vm.vmid);
        if (existing) {
          this.db.prepare("UPDATE desktops SET pve_available=1,pve_node=?,pve_power_status=?,pve_seen_at=?,pve_os_type=?,pve_os_family=?,pve_os_source=? WHERE kind='vm' AND pve_vmid=?").run(vm.node, vm.status, now, vm.pveOsType ?? null, vm.osFamily ?? 'unknown', vm.osSource ?? 'unknown', vm.vmid);
          updated++;
        } else {
          this.db.prepare("INSERT INTO desktops(id,name,kind,description,os,enabled,created_at,pve_vmid,pve_available,pve_node,pve_power_status,pve_seen_at) VALUES(?,?,'vm',?,'미확인',1,?,?,1,?,?,?)").run(`pve-${vm.vmid}`, text(vm.name.trim().slice(0, 80), 80), `Proxmox VM ${vm.vmid}`, now, vm.vmid, vm.node, vm.status, now);
          this.db.prepare('UPDATE desktops SET pve_os_type=?,pve_os_family=?,pve_os_source=? WHERE id=?').run(vm.pveOsType ?? null, vm.osFamily ?? 'unknown', vm.osSource ?? 'unknown', `pve-${vm.vmid}`);
          added++;
        }
        const uuid = verifiedMachineUuid(vm.machineUuid), id = existing?.id ?? `pve-${vm.vmid}`;
        if (uuid) this.bindMachineIdentity(id, uuid, actor, now);
        else this.db.prepare('UPDATE desktops SET pve_machine_checked_at=NULL WHERE id=?').run(id);
      }
      this.applyOrganization(vms);
      const unavailable = this.db.prepare("SELECT count(*) AS n FROM desktops WHERE kind='vm' AND pve_vmid IS NOT NULL AND pve_available=0").get().n;
      const result = { state: 'ok', checkedAt: new Date(now).toISOString(), count: vms.length, added, updated, unavailable };
      this.db.prepare("INSERT INTO settings(key,value) VALUES('proxmox_sync',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value").run(JSON.stringify(result));
      this.db.prepare("INSERT INTO settings(key,value) VALUES('proxmox_initialized','1') ON CONFLICT(key) DO UPDATE SET value=excluded.value").run();
      this.audit('proxmox_synchronized', actor, String(vms.length), now);
      return result;
    });
  }
  createSession(user, hash, now, ttl, purpose = 'client', { protectedHashes = new Set(), issuer = 'legacy', loginIntent } = {}) {
    // Server-owned live bindings only. This option is never a login-body field.
    // Retain the existing total cap5 across client AND administrator purposes.
    if (!(protectedHashes instanceof Set) || protectedHashes.size > 5 || [...protectedHashes].some(value => typeof value !== 'string' || !/^[a-f0-9]{64}$/.test(value))) throw new Error('invalid_session_protection');
    if (!['legacy', pcSessionPolicy, 'browser-console', 'admin'].includes(issuer) ||
        (issuer === pcSessionPolicy && (purpose !== 'client' || !['interactive-v1', 'recovery-v1'].includes(loginIntent))) ||
        (issuer !== pcSessionPolicy && loginIntent !== undefined)) throw new Error('invalid_session_policy');
    return this.transaction(() => {
      const current = this.db.prepare('SELECT * FROM users WHERE id=? AND disabled=0').get(user.id);
      if (!current || current.password_hash !== user.password_hash || (purpose === 'client' && current.must_change_credentials)) return false;
      this.db.prepare('DELETE FROM sessions WHERE expires_at<=?').run(now);
      if (issuer === pcSessionPolicy) {
        const previous = this.db.prepare("SELECT 1 FROM sessions WHERE user_id=? AND issuer='windows-pc-v1' AND replaced_at IS NULL").get(user.id);
        if (previous && loginIntent !== 'interactive-v1') throw Object.assign(new Error('pc_login_requires_interaction'), { code: 'pc_login_requires_interaction' });
        this.db.prepare("UPDATE sessions SET replaced_at=? WHERE user_id=? AND issuer='windows-pc-v1' AND replaced_at IS NULL").run(now, user.id);
      }
      const existing = this.db.prepare('SELECT token_hash FROM sessions WHERE user_id=? AND replaced_at IS NULL ORDER BY created_at ASC,rowid ASC LIMIT 6').all(user.id);
      const victim = existing.find(row => !protectedHashes.has(row.token_hash));
      if (existing.length > 5 || (existing.length === 5 && (!victim || issuer === pcSessionPolicy))) throw Object.assign(new Error('active_session_limit'), { code: 'active_session_limit' });
      if (existing.length === 5) this.db.prepare('DELETE FROM sessions WHERE token_hash=? AND user_id=?').run(victim.token_hash, user.id);
      this.db.prepare('INSERT INTO sessions(token_hash,user_id,created_at,expires_at,purpose,issuer) VALUES(?,?,?,?,?,?)').run(hash, user.id, now, now + ttl, purpose, issuer);
      if (issuer === pcSessionPolicy) {
        this.db.prepare('DELETE FROM sessions WHERE token_hash IN (SELECT token_hash FROM sessions WHERE user_id=? AND replaced_at IS NOT NULL ORDER BY replaced_at DESC,rowid DESC LIMIT -1 OFFSET ?)').run(user.id, replacedSessionsPerUser);
        this.pruneReplacedSessions(now);
      }
      this.audit('login_succeeded', user.id, '', now);
      return true;
    });
  }
  // Sliding renewal of a still-valid client session: the SAME token lives on
  // (relay sessions, event sockets and the other channels are bound to its
  // hash), its expiry moves to now+ttl but never past created_at+maxLifetime.
  // Returns the new expiry; null for an unknown/expired/disabled session.
  renewSession(hash, now, ttl, maxLifetime) {
    return this.transaction(() => {
      const row = this.db.prepare("SELECT s.created_at,s.expires_at FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.token_hash=? AND s.expires_at>? AND s.replaced_at IS NULL AND u.disabled=0 AND s.purpose='client'").get(hash, now);
      if (!row) return null;
      const expiresAt = Math.max(row.expires_at, Math.min(now + ttl, row.created_at + maxLifetime));
      if (expiresAt !== row.expires_at) this.db.prepare('UPDATE sessions SET expires_at=? WHERE token_hash=?').run(expiresAt, hash);
      return expiresAt;
    });
  }
  authenticate(hash, now = Date.now(), purpose = 'client') {
    const user = reusable(this.db, 'SELECT u.id,u.username,u.display_name,u.notes,u.is_primary_admin,u.role,u.must_change_credentials,s.expires_at FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.token_hash=? AND s.expires_at>? AND s.replaced_at IS NULL AND u.disabled=0 AND s.purpose=?').get(hash, now, purpose);
    if (user && !user.must_change_credentials && purpose === 'client' && typeof this.sessionAccessGuard === 'function' && !this.sessionAccessGuard(hash, now)) return undefined;
    return user;
  }
  sessionRevocationReason(hash, now = Date.now(), userId) {
    const ended = this.db.prepare('SELECT user_id,reason FROM session_end_reasons WHERE token_hash=? AND retain_until>?').get(hash, now);
    if (ended && (userId === undefined || ended.user_id === userId)) return ended.reason;
    const row = this.db.prepare("SELECT user_id FROM sessions WHERE token_hash=? AND issuer='windows-pc-v1' AND replaced_at IS NOT NULL AND expires_at>?").get(hash, now);
    return row && (userId === undefined || row.user_id === userId) ? pcLoginReplaced : null;
  }
  pruneReplacedSessions(now = Date.now(), batch = 500) {
    const expired = this.db.prepare('DELETE FROM sessions WHERE token_hash IN (SELECT token_hash FROM sessions WHERE expires_at<=? LIMIT ?)').run(now, batch).changes;
    const overflow = this.db.prepare('DELETE FROM sessions WHERE token_hash IN (SELECT token_hash FROM sessions WHERE replaced_at IS NOT NULL ORDER BY replaced_at DESC,rowid DESC LIMIT ? OFFSET ?)').run(batch, replacedSessionsTotal).changes;
    return expired + overflow;
  }
  setting(key) { return this.db.prepare('SELECT value FROM settings WHERE key=?').get(key)?.value; }
  onboarding(user) {
    const credentialsRequired = !!user.must_change_credentials;
    const proxmoxRequired = this.setting('proxmox_initialized') !== '1';
    return { credentialsRequired, proxmoxRequired, complete: !credentialsRequired && !proxmoxRequired, stage: credentialsRequired ? 'credentials' : proxmoxRequired ? 'proxmox' : 'complete' };
  }
  saveSetting(key, value, actor) {
    this.transaction(() => { this.db.prepare('INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value').run(key, value); this.audit('integration_updated', actor, key); });
  }
  auditLog() { return this.db.prepare('SELECT * FROM audit_events ORDER BY id DESC LIMIT 100').all(); }
  logout(hash) { this.db.prepare('DELETE FROM sessions WHERE token_hash=?').run(hash); noteLoginEnd(this, hash, 'logout'); }
  inventory() {
    return {
      users: this.db.prepare(`SELECT u.id,u.username,u.username AS display_name,u.notes,u.is_primary_admin,u.role,u.disabled,(SELECT count(*) FROM assignments a JOIN desktops d ON d.id=a.desktop_id WHERE a.user_id=u.id AND NOT ((${availableDesktopSql}) AND (${registeredDesktopSql}))) AS unavailableAssignmentCount FROM users u ORDER BY u.username`).all(),
      desktops: this.db.prepare(`SELECT d.*,CASE WHEN ${approvedPcSql} THEN 1 ELSE 0 END AS local_pc_approved,w.adapters AS wake_adapters,w.reported_at AS wake_reported_at FROM desktops d LEFT JOIN pc_wake w ON w.desktop_id=d.id ORDER BY d.pve_vmid IS NULL,d.pve_vmid,d.name,d.id`).all().map(({local_pc_approved,wake_adapters,wake_reported_at,...row}) => ({ ...row, pve_tags: storedTags(row.pve_tags), ...(row.kind==='pc'?{localPcApproved:local_pc_approved===1,wake:storedWake(wake_adapters,wake_reported_at)}:{}) })),
      // Permissions belong to each assignment (user x VM), not to the account.
      assignments: this.db.prepare('SELECT u.username,a.desktop_id,a.clipboard_permissions,a.rdp_redirections,a.sound_card_add FROM assignments a JOIN users u ON u.id=a.user_id JOIN desktops d ON d.id=a.desktop_id ORDER BY u.username,d.pve_vmid IS NULL,d.pve_vmid,a.desktop_id').all()
        .map(({ clipboard_permissions, rdp_redirections, sound_card_add, ...row }) => ({ ...row, ...assignmentPolicy({ clipboard_permissions, rdp_redirections, sound_card_add }) })),
    };
  }
}

import { join } from 'node:path';
import { writeFileSync, existsSync, readFileSync } from 'node:fs';
import { configuration } from './config.mjs';
import { Store } from './store.mjs';
import { initialPasswordHash, verifyPassword } from './security.mjs';

process.umask(0o077);
const config = configuration();
const store = new Store(join(config.dataDir, 'broker.sqlite'));
try {
  const count = store.db.prepare('SELECT COUNT(*) AS count FROM users').get().count;
  const path = join(config.dataDir, 'initial-account.json');
  const initialRecord = { username: 'admin', password: 'admin', mustChangeCredentials: true, serverUrl: config.publicOrigin };
  if (count !== 0 && process.argv[2] === 'reset-initial') {
    const previous = JSON.parse(readFileSync(path, 'utf8'));
    const user = store.findUser(previous.username);
    if (count === 1 && user?.username === 'admin' && user.must_change_credentials) {
      console.log('Initial setup is already pending.');
    } else {
      if (count !== 1 || !user || user.role !== 'admin' || user.disabled || store.inventory().desktops.length || store.setting('proxmox') || !await verifyPassword(previous.password, user.password_hash)) throw new Error('existing_configuration_must_not_be_reset');
      const hash = await initialPasswordHash();
      store.transaction(() => {
        store.db.prepare('UPDATE users SET username=?,password_hash=?,must_change_credentials=1 WHERE id=?').run('admin', hash, user.id);
        store.db.prepare('DELETE FROM sessions').run();
        store.audit('initial_setup_requested', 'server-admin', user.id);
      });
      writeFileSync(path, JSON.stringify(initialRecord, null, 2), { mode: 0o600 });
      console.log('Initial account changed to the requested first-login setup flow.');
    }
  } else if (count !== 0) {
    console.log('Existing users preserved.');
  } else {
    if (existsSync(path)) throw new Error('initial_account_file_exists');
    const id = store.addUser({ name: 'admin', role: 'admin', passwordHash: await initialPasswordHash() });
    store.db.prepare('UPDATE users SET must_change_credentials=1 WHERE id=?').run(id);
    writeFileSync(path, JSON.stringify(initialRecord, null, 2), { mode: 0o600, flag: 'wx' });
    console.log('Initial account created; username/password change is mandatory before administration. The broker prints a one-time setup code (event initial_setup_code) to its log at startup; the first setup requires it.');
  }
} finally { store.close(); }

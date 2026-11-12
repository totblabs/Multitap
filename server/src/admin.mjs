import { join } from 'node:path';
import { configuration } from './config.mjs';
import { Store } from './store.mjs';
import { hashPassword } from './security.mjs';
import { MfaProvider } from './mfa.mjs';

process.umask(0o077);
const [command, ...args] = process.argv.slice(2);
const flags = {};
const values = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--password-stdin') flags.passwordStdin = true;
  else if (args[i].startsWith('--')) { if (!args[i + 1] || args[i + 1].startsWith('--')) throw new Error('missing_flag_value'); flags[args[i].slice(2)] = args[++i]; }
  else values.push(args[i]);
}
async function password() {
  if (!flags.passwordStdin) throw new Error('use_--password-stdin');
  let input = '';
  for await (const chunk of process.stdin) { input += chunk; if (input.length > 2048) throw new Error('password_input_too_long'); }
  return hashPassword(input.replace(/\r?\n$/, ''));
}
const config = configuration();
const store = new Store(join(config.dataDir, 'broker.sqlite'));
try {
  switch (command) {
    case 'mfa-reset':
      if (typeof process.getuid !== 'function' || process.getuid() !== 0) throw Error('mfa_reset_local_root_required');
      new MfaProvider({ store }).resetLocal(values[0], flags['confirm-user-id']); break;
    case 'user-add': store.addUser({ name: values[0], notes: flags.notes ?? '', passwordHash: await password(), role: flags.role ?? 'user' }); break;
    case 'password-set': store.changePassword(values[0], await password()); break;
    case 'user-disable': store.setUserEnabled(values[0], false); break;
    case 'user-enable': store.setUserEnabled(values[0], true); break;
    case 'desktop-add': throw new Error('Use Proxmox VM import in the admin console. Manual PC/desktop registration is not supported.');
    case 'desktop-update': store.updateDesktop(values[0], { name: flags.name, os: flags.os, description: flags.description }); break;
    case 'desktop-disable': store.setDesktopEnabled(values[0], false); break;
    case 'desktop-enable': store.setDesktopEnabled(values[0], true); break;
    case 'assign': store.assign(values[0], values[1]); break;
    case 'revoke': store.revoke(values[0], values[1]); break;
    case 'list': console.log(JSON.stringify(store.inventory(), null, 2)); break;
    default: throw new Error('commands: user-add password-set user-disable user-enable desktop-add desktop-update desktop-disable desktop-enable assign revoke list mfa-reset');
  }
  if (command !== 'list') console.log(JSON.stringify({ ok: true, command }));
} catch (error) { console.error(JSON.stringify({ error: error.code ?? error.message })); process.exitCode = 1; }
finally { store.close(); }

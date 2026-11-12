// The server access settings of a Docker Compose install without an updater,
// from the command line (self-access.mjs keeps them). It runs in a container
// of the server image with the service's environment and data volume, while
// the server runs or while it is stopped.
//
//   show                         the addresses in use and what the container fixes
//   set <url>... [--primary <url>] [--trusted-proxies <ip,ip|none>] [--follow on|off]
//                                these addresses instead of the current ones
//   public-ip status             whether the public IP is followed
//   public-ip on [--address <ipv4>]   follow it; --address names the public IP
//                                whose HTTP address is already registered
//   public-ip off                stop following; the registered address stays
//   revert                       put back what the last set / public-ip replaced
//   reset                        remove the saved policy: the .env lines apply again.
//                                It removes what cannot be used, whoever wrote it,
//                                as far as this account may remove files in the
//                                folder; where it may not: server_access_reset_denied
// --force goes on although a save from the console is still in flight (only
// with the server stopped).
//
// A running server looks at the volume every few seconds (self-access.mjs
// reconcile): it takes over `public-ip on|off` and a first policy of the
// addresses it already serves without disconnecting anybody, and for other
// addresses it ends the connections and starts again by itself. Starting it
// again right after a change only makes that sooner.
//
// Output: key=value lines. The first is ok=1, or ok=0 followed by code=<reason>.
// Exit code: 0 done, 2 refused, 64 wrong arguments.
import { pathToFileURL } from 'node:url';
import { lstat } from 'node:fs/promises';
import { configuration, selfAccessStorage } from './config.mjs';
import { normalizeBrokerOrigin } from './transport-policy.mjs';
import { endpointOrigins, legacyServerAccess } from '../update/server-access-policy.mjs';
import { selfAccessCandidate, selfAccessPolicy, selfPolicyText, putSelfPolicy, ensureSelfIdentity, readSelfJournal, clearSelfJournal, selfAccessActiveStates,
  writeSelfBeforeCli, readSelfBeforeCli, clearSelfBeforeCli, removeSelfIdentity, publicIpOffered, followedOrigin, publicIpv4 } from './self-access.mjs';

const usage = 'usage: access-cli.mjs show\n       access-cli.mjs set <url>... [--primary <url>] [--trusted-proxies <ip,ip|none>] [--follow on|off] [--force]\n       access-cli.mjs public-ip status | on [--address <ipv4>] [--force] | off [--force]\n       access-cli.mjs revert [--force]\n       access-cli.mjs reset\n';
const fail = code => { throw Error('server_access_' + code); };
// A reason is a fixed code: the server's own, or the one its settings fail to load with.
const codeOf = error => /^[a-z][a-z_]{1,80}$/.test(error?.message ?? '') ? error.message : 'server_access_failed';
const origins = row => row?.enabled ? endpointOrigins(row) : [];
const line = value => String(value ?? '').replace(/[\r\n]/g, '');

function describe(config) {
  const self = config.selfAccess, access = config.accessConfig;
  let pending = '';
  try { const journal = readSelfJournal(self.paths, self.read); if (journal && selfAccessActiveStates.has(journal.state)) pending = journal.state; } catch { pending = 'unreadable'; }
  return [['source', self.saved ? 'saved' : access ? 'env' : 'none'], ['revision', self.revision], ['primary', access?.primaryOrigin], ['https', origins(access?.https).join(',')], ['http', origins(access?.http).join(',')],
    ['trustedProxies', (access?.trustedProxies ?? []).join(',')], ['follow', self.publicIp.follow ? 1 : 0], ['followed', followedOrigin(access, self.publicIp)], ['followOffered', publicIpOffered(access, self.fixed) ? 1 : 0],
    ['tlsMode', self.fixed.httpsMode], ['publishedHost', self.fixed.published?.host], ['publishedPort', self.fixed.published?.port], ['serverAddress', self.fixed.serverAddress],
    ['pending', pending], ['envAddressesIgnored', self.envAddressesIgnored ? 1 : 0]];
}
const format = rows => rows.map(([key, value]) => key + '=' + line(value)).join('\n') + '\n';

// A change waits while a save from the console is in flight: that save's own
// restart and check would meet another policy than the one it wrote.
function guard(self, force) {
  if (force) return;
  let journal;
  try { journal = readSelfJournal(self.paths, self.read); } catch { fail('busy'); }
  if (journal && selfAccessActiveStates.has(journal.state)) fail('busy');
}
async function write(self, next) {
  await writeSelfBeforeCli(self.paths, self.read(self.paths.policy));
  await ensureSelfIdentity(self.paths, self.read);
  // Saved beside the .env address lines of now (config.mjs: only lines edited after a save are "ignored").
  await putSelfPolicy(self.paths, selfPolicyText(next.config, next.publicIp, Date.now, self.envSeed));
  await clearSelfJournal(self.paths);
}

function address(value) {
  try { return normalizeBrokerOrigin(value, { allowInsecureHttp: true }); } catch { fail('origin_invalid'); }
}
function requested(config, urls, flags, env) {
  const self = config.selfAccess, access = config.accessConfig, list = urls.map(address);
  const primaryOrigin = flags.primary === undefined ? list.find(value => value.startsWith('https://')) ?? list[0] : address(flags.primary);
  if (!list.includes(primaryOrigin)) fail('primary_origin_invalid');
  // Without a current policy the proxies are the ones .env would give.
  const trustedProxies = flags.trustedProxies === undefined ? access?.trustedProxies ?? legacyServerAccess({ MULTITAP_PUBLIC_ORIGIN: 'https://localhost', MULTITAP_TRUSTED_PROXIES: env.MULTITAP_TRUSTED_PROXIES }).trustedProxies
    : flags.trustedProxies === 'none' ? [] : flags.trustedProxies.split(',').map(value => value.trim());
  const policy = selfAccessPolicy({ primaryOrigin, https: list.filter(value => value.startsWith('https://')), http: list.filter(value => value.startsWith('http://')), trustedProxies }, self.fixed);
  return selfAccessCandidate(policy, flags.follow === undefined ? self.publicIp.follow : flags.follow === 'on', { fixed: self.fixed, current: { config: access, publicIp: self.publicIp } });
}
function following(config, on, flags) {
  const self = config.selfAccess, access = config.accessConfig;
  if (!access) { if (on) fail('public_ip_conflict'); return null; }
  let publicIp = self.publicIp;
  if (on && flags.address !== undefined) {
    const ip = publicIpv4(flags.address);
    if (!ip || !self.fixed.published) fail('public_ip_invalid');
    const registered = 'http://' + ip + ':' + self.fixed.published.port;
    if (origins(access.http).includes(registered)) publicIp = { follow: true, origin: registered };
  }
  return selfAccessCandidate(access, on, { fixed: self.fixed, current: { config: access, publicIp } });
}

export async function main(argv, { stdout = process.stdout, env = process.env, configure = configuration, storage = selfAccessStorage } = {}) {
  const [command, ...rest] = argv, flags = {}, values = [];
  for (let index = 0; index < rest.length; index++) {
    const name = rest[index];
    if (name === '--force') flags.force = true;
    else if (['--primary', '--trusted-proxies', '--follow', '--address'].includes(name) && index + 1 < rest.length) flags[name === '--trusted-proxies' ? 'trustedProxies' : name.slice(2)] = rest[++index];
    else if (name.startsWith('--')) { stdout.write(usage); return 64; }
    else values.push(name);
  }
  const only = allowed => Object.keys(flags).every(key => allowed.includes(key));
  const valid = command === 'show' ? !values.length && only([])
    : command === 'set' ? values.length > 0 && only(['primary', 'trustedProxies', 'follow', 'force']) && [undefined, 'on', 'off'].includes(flags.follow)
    : command === 'public-ip' ? values.length === 1 && (values[0] === 'status' ? only([]) : values[0] === 'on' ? only(['address', 'force']) : values[0] === 'off' && only(['force']))
    : command === 'revert' ? !values.length && only(['force']) : command === 'reset' && !values.length && only([]);
  if (!valid) { stdout.write(usage); return 64; }
  const load = () => {
    const config = configure(env);
    if (!config.selfAccess) fail('unavailable');
    // A saved policy that could not be used: said with its reason, as a start says it. `reset` removes it.
    if (config.selfAccess.unreadable) throw Error(config.selfAccess.unreadable);
    return config;
  };
  try {
    let changed = null;
    if (command === 'reset') {
      // Works on a policy that does not load and on one this account may not
      // even read (another owner or mode): nothing is read, the files are
      // removed. The installation ID stays unless it cannot be read either:
      // with such a file no server starts.
      const files = storage(env);
      if (!files) fail('unavailable');
      try {
        let had = true;
        try { await lstat(files.paths.policy); } catch (error) { if (error.code !== 'ENOENT') throw error; had = false; }
        await putSelfPolicy(files.paths, null); await clearSelfJournal(files.paths); await clearSelfBeforeCli(files.paths);
        let identity = true;
        try { files.read(files.paths.identity, 128); } catch { identity = false; }
        if (!identity) await removeSelfIdentity(files.paths);
        changed = had;
      // The folder belongs to another account that gave this one no right to remove files in it.
      } catch { fail('reset_denied'); }
    } else if (command === 'revert') {
      const config = load(), self = config.selfAccess;
      guard(self, flags.force);
      const before = readSelfBeforeCli(self.paths, self.read);
      if (!before) fail('revert_unavailable');
      await putSelfPolicy(self.paths, before.previous); await clearSelfJournal(self.paths); await clearSelfBeforeCli(self.paths);
      changed = true;
    } else if (command === 'set' || (command === 'public-ip' && values[0] !== 'status')) {
      const config = load(), self = config.selfAccess;
      guard(self, flags.force);
      const next = command === 'set' ? requested(config, values, flags, env) : following(config, values[0] === 'on', flags);
      changed = next !== null && next.revision !== self.revision;
      // `set` also makes the .env addresses the saved policy when they are the same.
      if (changed || (command === 'set' && !self.saved)) await write(self, next);
    }
    const config = load();
    stdout.write(format([['ok', 1], ...(changed === null ? [] : [['changed', changed ? 1 : 0]]), ...describe(config)]));
    return 0;
  } catch (error) { stdout.write('ok=0\ncode=' + codeOf(error) + '\n'); return 2; }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  process.umask(0o077);
  process.exitCode = await main(process.argv.slice(2));
}

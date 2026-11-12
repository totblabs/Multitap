import { join } from 'node:path';
import { configuration, revertUnloadableSelfAccess } from './config.mjs';
import { Store } from './store.mjs';
import { buildServer, VERSION } from './http.mjs';
import { Vault } from './vault.mjs';
import { selectServerUpdate } from './docker-update-check.mjs';
import { loadTlsListener } from './tls-listener.mjs';
import { ServerAccessClient } from './server-access-client.mjs';
import { ServerRestoreClient } from './server-restore-client.mjs';
import { SelfAccess, selfAccessRestartCode } from './self-access.mjs';
import { serverAccessListeners } from '../update/server-access-policy.mjs';
import { startupSecurityWarnings } from './startup-security.mjs';
import { setupGuide, containerRun } from './initial-setup.mjs';
import { CertificateMonitor, pemCertificate } from './certificate-expiry.mjs';

process.umask(0o077);
let config;
try { config = configuration(); }
catch (error) {
  // A self-managed Docker install whose just-saved access policy does not
  // load goes back to the previous one and starts again.
  if (await revertUnloadableSelfAccess().catch(() => false)) process.exit(selfAccessRestartCode);
  throw error;
}
// A saved access policy that could not be used (config.mjs): one a save of this
// server has just written goes back to the previous one; any other leaves the
// server running without addresses, and the log says so at every start.
if (config.selfAccess?.unreadable) {
  if (await revertUnloadableSelfAccess().catch(() => false)) process.exit(selfAccessRestartCode);
  console.error(JSON.stringify({ service: 'multitap-broker', event: 'access_policy_unreadable', code: config.selfAccess.unreadable, urls: config.setupOrigin ? [config.setupOrigin] : [],
    message: 'The saved server access settings could not be used. The server runs without addresses: only the administration console answers, at the server address. Sign in there and save the addresses again, or run: multitap address --reset <url>' }));
}
const listeners = config.accessConfig ? serverAccessListeners(config.accessConfig) : [{ protocol: 'http', host: config.bootstrapHost, port: config.bootstrapPort, tlsFiles: null }];
// The updater's primary local health address remains the configured primary
// listener, even when HTTP and direct HTTPS use separate ports.
listeners.sort((a, b) => Number(b.host === config.host && b.port === config.port) - Number(a.host === config.host && a.port === config.port));
// The native install's updater socket keeps the full check/apply path. A Docker
// Compose install without one only checks the signed public feed (no apply).
const serverUpdate = selectServerUpdate(config, VERSION);
let store, server;
let closing = false;
function shutdown(code = 0) {
  if (closing) return;
  closing = true;
  server.close(() => { store.close(); process.exit(code); });
  setTimeout(() => process.exit(code || 1), 10000).unref();
}
// A Docker Compose install without an updater applies its own access settings
// (self-access.mjs): the entrypoint starts the server again on this exit code.
const selfAccess = config.selfAccess ? SelfAccess.from(config, { load: () => configuration(), exit: () => { if (server?.listening) shutdown(selfAccessRestartCode); else process.exit(selfAccessRestartCode); } }) : null;
await selfAccess?.initialize();
try {
  store = new Store(join(config.dataDir, 'broker.sqlite'));
  const vault = Vault.open(join(config.dataDir, 'vault.key'));
  // tls-expiry-v1: the listeners' own certificates, filled in as they load below.
  const presented = [], certificates = new CertificateMonitor({ store, listeners: presented, origin: config.publicOrigin });
  for (let index = 0; index < listeners.length; index++) {
    const listener = listeners[index], tls = await loadTlsListener(listener.tlsFiles);
    if (tls) presented.push(pemCertificate(tls.cert));
    let current;
    try {
      if (index === 0) {
        server = await buildServer({ store, vault, publicOrigin: config.publicOrigin, allowInsecureHttp: config.allowInsecureHttp, tls, certificateMonitor: certificates,
          accessConfig: config.accessConfig, accessRevision: config.accessRevision, accessInstallationId: config.accessInstallationId, accessIdentityFile: config.accessIdentityFile, accessIdentityRead: selfAccess ? () => config.selfAccess.read(config.accessIdentityFile, 128) : null, accessOperationId: selfAccess?.startupOperationId ?? config.accessOperationId ?? null, legacyAdminOrigin: config.legacyPublicOrigin,
          bootstrap: config.bootstrapOnly, setupOrigin: config.bootstrapOnly ? config.setupOrigin : null, initialSetupOptions: { guide: setupGuide(config), run: config.deploymentKind === 'docker' ? containerRun() : null },
          accessClient: config.updateSocket ? new ServerAccessClient(config.updateSocket) : selfAccess,
          // 서버 다시 시작 in the console: the entrypoint starts the server again on this exit code.
          restartServer: selfAccess ? () => shutdown(selfAccessRestartCode) : null,
          // 백업·복원: its working directory, and the updater that applies a restore.
          backupDir: join(config.dataDir, 'backup-work'), restoreClient: config.updateSocket ? new ServerRestoreClient(config.updateSocket) : null,
          deploymentKind: config.deploymentKind, trustedProxies: config.trustedProxies, dataDir: config.dataDir, downloadDir: join(config.dataDir, 'downloads'), serverUpdate, relayOptions: { udp: config.udp, hostRoute: config.hostRoute, signinOptions: { enabled: false, qualifiedService: true } /* signed out -> Console, no in-stream sign-in */ } });
        current = server;
      } else current = server.createAdditionalListener(tls);
    } finally { tls?.key.fill(0); }
    await new Promise((resolve, reject) => { current.once('error', reject); current.listen(listener.port, listener.host, resolve); });
  }
  certificates.start();
  for (const warning of startupSecurityWarnings(config)) console.warn(JSON.stringify(warning));
  // The saved access policy is in use; the address lines in .env name other addresses.
  if (config.selfAccess?.envAddressesIgnored) console.warn(JSON.stringify({ service: 'multitap-broker', event: 'env_addresses_ignored', message: 'The saved server access settings are in use. MULTITAP_PUBLIC_ORIGIN and MULTITAP_HTTP_ORIGIN in .env name other addresses and are ignored.' }));
  console.log(JSON.stringify({ service: 'multitap-broker', version: VERSION, listeners: listeners.map(({ protocol, host, port }) => ({ protocol, host, port })), bootstrapOnly: config.bootstrapOnly }));
} catch (error) { if (server) await new Promise(resolve => server.close(resolve)); store?.close(); throw error; }
process.on('SIGTERM', () => shutdown());
process.on('SIGINT', () => shutdown());
// A restart made by a save reaches its new addresses now; then the rounds
// begin: the look at the volume, and the public IP while it is followed.
if (selfAccess) void selfAccess.resume().finally(() => selfAccess.startRounds());

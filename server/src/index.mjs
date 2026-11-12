import { join } from 'node:path';
import { configuration } from './config.mjs';
import { Store } from './store.mjs';
import { buildServer, VERSION } from './http.mjs';
import { Vault } from './vault.mjs';
import { selectServerUpdate } from './docker-update-check.mjs';
import { loadTlsListener } from './tls-listener.mjs';
import { ServerAccessClient } from './server-access-client.mjs';
import { ServerRestoreClient } from './server-restore-client.mjs';
import { serverAccessListeners } from '../update/server-access-policy.mjs';
import { startupSecurityWarnings } from './startup-security.mjs';
import { setupGuide } from './initial-setup.mjs';
import { CertificateMonitor, pemCertificate } from './certificate-expiry.mjs';

process.umask(0o077);
const config = configuration();
const listeners = config.accessConfig ? serverAccessListeners(config.accessConfig) : [{ protocol: 'http', host: config.bootstrapHost, port: config.bootstrapPort, tlsFiles: null }];
// The updater's primary local health address remains the configured primary
// listener, even when HTTP and direct HTTPS use separate ports.
listeners.sort((a, b) => Number(b.host === config.host && b.port === config.port) - Number(a.host === config.host && a.port === config.port));
// The native install's updater socket keeps the full check/apply path. A Docker
// Compose install without one only checks the signed public feed (no apply).
const serverUpdate = selectServerUpdate(config, VERSION);
let store, server;
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
          accessConfig: config.accessConfig, accessRevision: config.accessRevision, accessInstallationId: config.accessInstallationId, accessIdentityFile: config.accessIdentityFile, accessOperationId: config.accessOperationId ?? null, legacyAdminOrigin: config.legacyPublicOrigin,
          bootstrap: config.bootstrapOnly, setupOrigin: config.bootstrapOnly ? config.setupOrigin : null, initialSetupOptions: { guide: setupGuide(config) },
          accessClient: config.updateSocket ? new ServerAccessClient(config.updateSocket) : null,
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
  console.log(JSON.stringify({ service: 'multitap-broker', version: VERSION, listeners: listeners.map(({ protocol, host, port }) => ({ protocol, host, port })), bootstrapOnly: config.bootstrapOnly }));
} catch (error) { if (server) await new Promise(resolve => server.close(resolve)); store?.close(); throw error; }
let closing = false;
function shutdown() {
  if (closing) return;
  closing = true;
  server.close(() => { store.close(); process.exit(0); });
  setTimeout(() => process.exit(1), 10000).unref();
}
process.on('SIGTERM', shutdown);
process.on('SIGINT', shutdown);

import { request as httpRequest } from 'node:http';
import { request as httpsRequest } from 'node:https';
import { localHealthOptions, bootstrapHealthOrigin, validBootstrapLocalHealth } from './deployment-health.mjs';
import { normalizeServerAccess, allowedServerOrigins, serverAccessProtocol, serverAccessHash, serverAccessUnconfiguredRevision } from './server-access-policy.mjs';
import { serverAccessTransport } from './server-access-policy.mjs';
import { pathToFileURL } from 'node:url';

function boundedJson(requester, options) {
  return new Promise(resolve => {
    let settled = false, timer, request;
    const finish = value => { if (settled) return; settled = true; clearTimeout(timer); resolve(value); };
    try {
      request = requester(options, response => {
        if (response.statusCode !== 200) { response.destroy(); finish(null); return; }
        let size = 0; const chunks = [];
        response.on('data', chunk => { size += chunk.length; if (size > 16384) { response.destroy(); finish(null); } else chunks.push(chunk); });
        response.once('error', () => finish(null)); response.once('aborted', () => finish(null));
        response.once('end', () => { try { finish(JSON.parse(Buffer.concat(chunks).toString('utf8'))); } catch { finish(null); } });
      });
      timer = setTimeout(() => { request.destroy(); finish(null); }, 4000); timer.unref?.();
      request.once('error', () => finish(null)); request.once('timeout', () => { request.destroy(); finish(null); }); request.end();
    } catch { finish(null); }
  });
}

export function localAccessRequest(transport, { activityToken, endConnections = false, http = httpRequest, https = httpsRequest } = {}) {
  const { tls, options } = localHealthOptions(transport, { internalAuthority: true });
  // Ending the broker's connections is a fenced request or nothing at all.
  if (endConnections !== false && (endConnections !== true || activityToken === undefined)) return Promise.resolve(null);
  if (activityToken !== undefined) {
    if (!/^[a-f0-9]{64}$/.test(activityToken)) return Promise.resolve(null);
    options.path = endConnections ? '/internal/server-access/end-connections' : '/internal/server-access/activity';
    const host = options.hostname.includes(':') ? '[' + options.hostname + ']' : options.hostname;
    options.headers = { Host: host + ':' + options.port, Accept: 'application/json', 'x-multitap-access-fence': activityToken };
    if (endConnections) { options.method = 'POST'; options.headers['Content-Length'] = 0; }
  }
  return boundedJson(tls ? https : http, options);
}

export function publicAccessRequest(config, origin, { http = httpRequest, https = httpsRequest } = {}) {
  if (!allowedServerOrigins(config).includes(origin)) return Promise.resolve(null);
  const target = new URL('/healthz', origin);
  // A single fixed GET, no redirect, cookies, authorization, supplied headers or
  // certificate exceptions. HTTPS uses Node's normal CA and hostname checks.
  return boundedJson(target.protocol === 'https:' ? https : http, { protocol: target.protocol, hostname: target.hostname.replace(/^\[|\]$/g, ''), port: Number(target.port || (target.protocol === 'https:' ? 443 : 80)), path: '/healthz', method: 'GET', timeout: 3500, headers: { Host: target.host, Accept: 'application/json', 'User-Agent': 'Multitap-Access-Verification/1' }, ...(target.protocol === 'https:' ? { rejectUnauthorized: true } : {}) });
}

export function validAccessHealth(value, { config, revision, installationId, origin, localTransport = null, allowHealthProbeOnly = false }) {
  let normalized, allowed, expectedRevision;
  try {
    normalized = config === null ? null : normalizeServerAccess(config);
    allowed = normalized ? allowedServerOrigins(normalized) : [];
    expectedRevision = normalized ? serverAccessHash(normalized) : serverAccessUnconfiguredRevision;
  } catch { return false; }
  // Root owns the expected policy and probes each of its own allowed origins.
  // Never accept a separately labelled revision or a response-selected URL list.
  if (revision !== expectedRevision || (normalized ? !allowed.includes(origin) :
      !allowHealthProbeOnly || origin === null || origin !== bootstrapHealthOrigin(localTransport) || !validBootstrapLocalHealth(value, localTransport))) return false;
  const access = value?.access;
  if (!(value?.service === 'multitap-broker' && value.status === 'ok' && value.authentication === 'id-password' && /^\d+\.\d+\.\d+$/.test(value.version ?? '') &&
    access?.protocol === serverAccessProtocol && (allowHealthProbeOnly || access.healthProbeOnly !== true) &&
    (access.healthProbeOnly === undefined || typeof access.healthProbeOnly === 'boolean') && access.revision === revision &&
    /^[a-f0-9]{64}$/.test(installationId ?? '') && access.installationId === installationId &&
    access.bootstrapOnly === (config === null) && access.origin === origin)) return false;
  // Accept old full responses only when any optional raw fields still agree.
  // New public responses omit these fields; hash/current-origin/install checks
  // above retain the same self-reported health trust model, not a new MAC proof.
  if (Object.hasOwn(access, 'primaryOrigin') && access.primaryOrigin !== (normalized?.primaryOrigin ?? null)) return false;
  if (Object.hasOwn(access, 'allowedOrigins') && (!Array.isArray(access.allowedOrigins) ||
    JSON.stringify([...access.allowedOrigins].sort()) !== JSON.stringify([...allowed].sort()))) return false;
  return true;
}

// Docker invokes only this installed helper in the already-owned container.
// The nonce travels on stdin, never the process command line or public output.
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href && process.argv[2] === '--local-probe') {
  try {
    let size = 0; const chunks = [];
    for await (const chunk of process.stdin) { size += chunk.length; if (size > 8192) throw Error(); chunks.push(chunk); }
    const input = JSON.parse(Buffer.concat(chunks).toString('utf8'));
    if (!input || Object.keys(input).some(key => !['config', 'port', 'activityToken', 'endConnections'].includes(key)) || !Number.isInteger(input.port) || input.port < 1 || input.port > 65535 ||
      (Object.hasOwn(input, 'endConnections') && input.endConnections !== true)) throw Error();
    const config = input.config === null ? null : normalizeServerAccess(input.config);
    const transport = config ? serverAccessTransport(config) : { publicOrigin: null, tlsMode: 'proxy', tlsFiles: null, bootstrapOnly: true };
    const result = await localAccessRequest({ ...transport, host: '127.0.0.1', port: input.port }, { activityToken: input.activityToken, ...(input.endConnections ? { endConnections: true } : {}) });
    process.stdout.write(JSON.stringify(result) + '\n');
  } catch { process.stdout.write('null\n'); process.exitCode = 1; }
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href && process.argv[2] === '--container-health') {
  try {
    const { configuration } = await import('../src/config.mjs'), { readFile } = await import('node:fs/promises');
    const current = configuration(), version = JSON.parse(await readFile(new URL('../package.json', import.meta.url), 'utf8')).version;
    const transport = current.accessConfig ? serverAccessTransport(current.accessConfig) : { publicOrigin: null, tlsMode: 'proxy', tlsFiles: null, bootstrapOnly: true, port: current.bootstrapPort };
    const localTransport = { ...transport, host: '127.0.0.1' };
    const value = await localAccessRequest(localTransport);
    if (value?.service !== 'multitap-broker' || value.status !== 'ok' || value.authentication !== 'id-password' || value.version !== version || value.access?.protocol !== serverAccessProtocol || value.access.revision !== current.accessRevision) throw Error();
    if (!current.accessConfig && !validBootstrapLocalHealth(value, localTransport)) throw Error();
    if (current.accessInstallationId && !validAccessHealth(value, { config: current.accessConfig, revision: current.accessRevision, installationId: current.accessInstallationId, origin: current.accessConfig?.primaryOrigin ?? bootstrapHealthOrigin(localTransport), localTransport, allowHealthProbeOnly: true })) throw Error();
  } catch { process.exitCode = 1; }
}

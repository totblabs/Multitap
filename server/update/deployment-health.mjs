import { request as httpRequest } from 'node:http';
import { request as httpsRequest } from 'node:https';
import { checkServerIdentity } from 'node:tls';
import { isIP } from 'node:net';
import { readFile, lstat } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';
import { deploymentTransport, deploymentPort } from './transport-policy.mjs';
import { serverAccessUnconfiguredRevision, legacyServerAccess } from './server-access-policy.mjs';

const keys = new Set(['MULTITAP_PUBLIC_ORIGIN', 'MULTITAP_ALLOW_INSECURE_HTTP', 'MULTITAP_HOST', 'MULTITAP_PORT', 'MULTITAP_TLS_MODE', 'MULTITAP_TLS_CERT_FILE', 'MULTITAP_TLS_KEY_FILE', 'MULTITAP_TRUSTED_PROXIES', 'MULTITAP_SERVER_ACCESS_FILE', 'MULTITAP_ACCESS_OPERATION_ID']);

// Parse data, never source a root-owned environment file as shell code.
export function deploymentEnvironmentValues(text) {
  if (typeof text !== 'string' || Buffer.byteLength(text) > 65536 || /\x00/.test(text)) throw Error('update_installation_invalid');
  const env = {};
  for (const line of text.split(/\r?\n/)) {
    if (!line || /^\s*#/.test(line)) continue;
    const pair = /^([A-Z][A-Z0-9_]*)=(.*)$/.exec(line);
    if (!pair || !keys.has(pair[1])) {
      if ([...keys].some(key => new RegExp('^\\s*(?:export\\s+)?' + key + '(?:\\s|=|$)').test(line))) throw Error('update_installation_invalid');
      continue;
    }
    if (Object.hasOwn(env, pair[1])) throw Error('update_installation_invalid');
    let value = pair[2];
    if (value.startsWith('"') && value.endsWith('"')) value = value.slice(1, -1);
    if (/[\x00-\x1f\x7f"'`$\\]/.test(value)) throw Error('update_installation_invalid');
    env[pair[1]] = value;
  }
  return env;
}

export function deploymentEnvironment(text, { portOverride } = {}) {
  const env = deploymentEnvironmentValues(text);
  let config;
  try {
    if (env.MULTITAP_PUBLIC_ORIGIN) config = deploymentTransport(env);
    else {
      const host = env.MULTITAP_HOST ?? '0.0.0.0';
      if (host !== 'localhost' && (!isIP(host) || host.includes('%'))) throw Error();
      config = { publicOrigin: null, allowInsecureHttp: false, tlsMode: 'proxy', tlsFiles: null, host: host === 'localhost' ? '127.0.0.1' : host, port: deploymentPort(env.MULTITAP_PORT), bootstrapOnly: true };
    }
  } catch { throw Error('update_installation_invalid'); }
  if (portOverride !== undefined) {
    if (!Number.isInteger(portOverride) || portOverride < 1 || portOverride > 65535) throw Error('update_installation_invalid');
    config = { ...config, port: portOverride };
  }
  return Object.freeze(config);
}

// Released 83 provisioners import readDeploymentEnvironment from the extracted
// candidate before stopping/switching the old broker. Fence this transition at
// that existing call, without guessing a proxy peer or enabling plain HTTP.
export function deploymentEnvironmentForUpdate(text) {
  const config = deploymentEnvironment(text);
  if (config.publicOrigin?.startsWith('https://') && config.tlsMode === 'proxy') {
    const env = deploymentEnvironmentValues(text);
    if (!Object.hasOwn(env, 'MULTITAP_TRUSTED_PROXIES') || !env.MULTITAP_TRUSTED_PROXIES.trim()) throw Error('update_proxy_configuration_required');
    let policy;
    try { policy = legacyServerAccess(env); } catch { throw Error('update_proxy_configuration_required'); }
    // 'none' is explicit but cannot admit any HTTPS proxy request. A policy
    // already applied by the access agent writes its approved exact IPs here.
    if (policy.trustedProxies.length === 0) throw Error('update_proxy_configuration_required');
  }
  return config;
}

export async function readDeploymentEnvironment(path) {
  const stat = await lstat(path);
  if (!stat.isFile() || stat.isSymbolicLink() || stat.size > 65536 || (process.platform !== 'win32' && (stat.uid !== 0 || (stat.mode & 0o022)))) throw Error('update_installation_invalid');
  return deploymentEnvironmentForUpdate(await readFile(path, 'utf8'));
}

export function localHealthOptions(config, { internalAuthority = false } = {}) {
  const host = config.host === '0.0.0.0' || config.host === 'localhost' ? '127.0.0.1' : config.host === '::' ? '::1' : config.host;
  const localAuthority = (host.includes(':') ? '[' + host + ']' : host) + ':' + config.port;
  const origin = config.publicOrigin ? new URL(config.publicOrigin) : new URL('http://' + localAuthority), name = origin.hostname.replace(/^\[|\]$/g, '');
  const tls = config.tlsMode === 'direct';
  return {
    tls,
    options: { hostname: host, port: config.port, path: '/healthz', method: 'GET', timeout: 2500,
      headers: { Host: internalAuthority || config.bootstrapOnly ? localAuthority : origin.host, Accept: 'application/json' },
      ...(tls ? { rejectUnauthorized: true, ...(isIP(name) ? {} : { servername: name }), checkServerIdentity: (_hostname, cert) => checkServerIdentity(name, cert) } : {}) },
  };
}


// Root-owned receiver metadata only. This is derived from the same host/port
// used by localHealthOptions; a public response cannot select its own authority.
export function bootstrapHealthOrigin(config) {
  if (!config || config.bootstrapOnly !== true || config.publicOrigin !== null || config.tlsMode !== 'proxy' || config.tlsFiles != null ||
      typeof config.host !== 'string' || !Number.isInteger(config.port) || config.port < 1 || config.port > 65535) return null;
  try {
    const { tls, options } = localHealthOptions(config, { internalAuthority: true });
    if (tls || !isIP(options.hostname)) return null;
    return new URL('http://' + options.headers.Host).origin;
  } catch { return null; }
}
export function validBootstrapLocalHealth(value, config) {
  const origin = bootstrapHealthOrigin(config), access = value?.access, transport = value?.transport;
  if (origin === null || value?.service !== 'multitap-broker' || value.status !== 'ok' || value.authentication !== 'id-password' ||
      !/^\d+\.\d+\.\d+$/.test(value.version ?? '') || access?.protocol !== 'server-access-v1' || access.bootstrapOnly !== true ||
      access.revision !== serverAccessUnconfiguredRevision || access.origin !== origin ||
      (access.healthProbeOnly !== undefined && typeof access.healthProbeOnly !== 'boolean')) return false;
  if (!transport || typeof transport !== 'object' || Array.isArray(transport) || Object.keys(transport).length !== 5 ||
      transport.protocol !== 'broker-transport-v1' || transport.publicOrigin !== origin || transport.scheme !== 'http' ||
      transport.allowInsecureHttp !== false || transport.directTls !== false) return false;
  // Empty raw fields are the exact prior full bootstrap profile, never an
  // alternate address source. Current public responses omit both fields.
  return (!Object.hasOwn(access, 'primaryOrigin') || access.primaryOrigin === null) &&
    (!Object.hasOwn(access, 'allowedOrigins') || (Array.isArray(access.allowedOrigins) && access.allowedOrigins.length === 0));
}

function brokerHealthRequest(config, { http = httpRequest, https = httpsRequest, internalAuthority = true, publicRoute = false, includeAccess = false } = {}) {
  let { tls, options } = localHealthOptions(config, { internalAuthority });
  if (publicRoute) {
    const origin = new URL(config.publicOrigin), name = origin.hostname.replace(/^\[|\]$/g, '');
    if (origin.protocol !== 'https:') return Promise.resolve(null);
    tls = true;
    options = { hostname: name, port: Number(origin.port || 443), path: '/healthz', method: 'GET', timeout: 2500, agent: false,
      headers: { Host: origin.host, Accept: 'application/json' }, rejectUnauthorized: true,
      ...(isIP(name) ? {} : { servername: name }), checkServerIdentity: (_hostname, cert) => checkServerIdentity(name, cert) };
  }
  return new Promise(resolve => {
    let done = false, timer, request;
    const finish = value => { if (done) return; done = true; clearTimeout(timer); resolve(value); };
    try {
      request = (tls ? https : http)(options, response => {
        let bytes = 0; const chunks = [];
        if (response.statusCode !== 200) { response.destroy(); finish(null); return; }
        response.on('data', chunk => {
          bytes += chunk.length;
          if (bytes > 65536) { response.destroy(); finish(null); return; }
          chunks.push(chunk);
        });
        response.once('error', () => finish(null));
        response.once('aborted', () => finish(null));
        response.once('end', () => {
          try {
            const value = JSON.parse(Buffer.concat(chunks).toString('utf8'));
            if (value?.service !== 'multitap-broker' || value.status !== 'ok' || typeof value.version !== 'string' || !/^\d+\.\d+\.\d+$/.test(value.version)) { finish(null); return; }
            // HTTPS keeps its established health/TLS contract. Only explicit
            // HTTP requires the added support advertisement; optional metadata
            // cannot make a previously compatible HTTPS broker unavailable.
            if (config.bootstrapOnly) {
              if (!validBootstrapLocalHealth(value, config)) { finish(null); return; }
            } else if (config.publicOrigin.startsWith('http://')) {
              const t = value.transport;
              if (!t || typeof t !== 'object' || Array.isArray(t) || Object.keys(t).length !== 5 || t.protocol !== 'broker-transport-v1' || t.publicOrigin !== config.publicOrigin || t.scheme !== originScheme(config) || t.allowInsecureHttp !== config.publicOrigin.startsWith('http://') || t.directTls !== tls) { finish(null); return; }
            }
            if (publicRoute) {
              const t = value.transport, a = value.access;
              if (!t || Object.keys(t).length !== 5 || t.protocol !== 'broker-transport-v1' || t.publicOrigin !== config.publicOrigin ||
                  t.scheme !== 'https' || t.allowInsecureHttp !== false || t.directTls !== false ||
                  a?.protocol !== 'server-access-v1' || a.origin !== config.publicOrigin || a.bootstrapOnly !== false || a.healthProbeOnly === true ||
                  (a.healthProbeOnly !== undefined && typeof a.healthProbeOnly !== 'boolean') || !/^[a-f0-9]{64}$/.test(a.revision ?? '') ||
                  !(a.installationId === null || /^[a-f0-9]{64}$/.test(a.installationId ?? ''))) { finish(null); return; }
            }
            finish({ service: value.service, status: value.status, version: value.version,
              ...(includeAccess ? { access: value.access } : {}) });
          } catch { finish(null); }
        });
      });
      if (done) { request.destroy(); return; }
      request.once('error', () => finish(null));
      request.once('timeout', () => { request.destroy(); finish(null); });
      timer = setTimeout(() => { request.destroy(); finish(null); }, 3000);
      request.end();
    } catch { finish(null); }
  });
}
// A local healthProbeOnly response is intentionally a liveness check; keep its
// existing API for recovery/control. It cannot alone prove the public route.
export function localBrokerHealth(config, { http = httpRequest, https = httpsRequest, internalAuthority = true } = {}) {
  return brokerHealthRequest(config, { http, https, internalAuthority });
}
export async function deploymentBrokerHealth(config, { http = httpRequest, https = httpsRequest } = {}) {
  const local = await brokerHealthRequest(config, { http, https, includeAccess: true });
  if (!local) return null;
  if (config.publicOrigin?.startsWith('https://') && config.tlsMode === 'proxy') {
    const remote = await brokerHealthRequest(config, { http, https, publicRoute: true, includeAccess: true });
    if (!remote || remote.version !== local.version || local.access?.protocol !== 'server-access-v1' ||
        remote.access.revision !== local.access.revision || remote.access.installationId !== local.access.installationId) return null;
  }
  return { service: local.service, status: local.status, version: local.version };
}
const originScheme = config => new URL(config.publicOrigin).protocol.slice(0, -1);

// Fixed deployment helper only; no redirects, arbitrary-origin health probes,
// certificate bypass, arbitrary request path or returned private environment.
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    if (process.argv.length < 3 || process.argv.length > 4 || (process.argv[3] !== undefined && !/^\d+\.\d+\.\d+$/.test(process.argv[3]))) throw Error('update_installation_invalid');
    const config = await readDeploymentEnvironment(process.argv[2]);
    const result = await deploymentBrokerHealth(config);
    if (!result || (process.argv[3] !== undefined && result.version !== process.argv[3])) throw Error('update_health_failed');
    process.stdout.write(JSON.stringify(result) + '\n');
  } catch { process.stderr.write('update_health_failed\n'); process.exitCode = 1; }
}

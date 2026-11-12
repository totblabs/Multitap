import { isIP } from 'node:net';
import { HttpError } from './errors.mjs';
import { normalizeBrokerOrigin } from './transport-policy.mjs';
import { endpointOrigins } from '../update/server-access-policy.mjs';

const loopback = new Set(['127.0.0.1', '::1']);
const plainIp = value => typeof value === 'string' && value.startsWith('::ffff:') && isIP(value.slice(7)) === 4 ? value.slice(7) : value;
function hostOrigin(scheme, authority) {
  if (typeof authority !== 'string' || authority.length > 512) throw new HttpError(403, 'origin_not_allowed');
  try { return normalizeBrokerOrigin(scheme + '://' + authority, { allowInsecureHttp: true }); }
  catch { throw new HttpError(403, 'origin_not_allowed'); }
}
function localBootstrap(req, origin) {
  const url = new URL(origin), hostname = url.hostname.replace(/^\[|\]$/g, '');
  if (url.protocol !== 'http:' || Number(url.port || 80) !== req.socket.localPort) return false;
  const peer = plainIp(req.socket.remoteAddress);
  const local = plainIp(req.socket.localAddress)?.split('%')[0];
  if (hostname === 'localhost') return loopback.has(peer) && loopback.has(local);
  if (!isIP(hostname)) return false;
  // Use the destination of this accepted socket. Interface enumeration opens
  // AF_NETLINK on Linux, which the managed service deliberately cannot use.
  // Another local interface or a forged Host is not this management path.
  return hostname === local && (!loopback.has(hostname) || loopback.has(peer));
}

// The request's actual TLS socket, or one explicitly configured proxy hop,
// selects the scheme. Neither an arbitrary Origin nor a forwarded header from
// an untrusted peer can add an allowed address or turn HTTP into HTTPS.
export function resolveRequestAccess(req, config, { bootstrap = false, setupOrigin = null } = {}) {
  const hostCount = (req.rawHeaders ?? []).filter((name, index) => index % 2 === 0 && name.toLowerCase() === 'host').length;
  if (hostCount > 1) throw new HttpError(403, 'origin_not_allowed');
  let scheme = req.socket.encrypted === true ? 'https' : 'http', authority = req.headers.host, proxied = false;
  const peer = plainIp(req.socket.remoteAddress);
  if (config?.trustedProxies.includes(peer) && req.headers['x-forwarded-proto'] !== undefined) {
    const forwarded = req.headers['x-forwarded-proto'];
    if (!['http', 'https'].includes(forwarded)) throw new HttpError(403, 'origin_not_allowed');
    scheme = forwarded; proxied = true;
    if (req.headers['x-forwarded-host'] !== undefined) authority = req.headers['x-forwarded-host'];
  }
  const origin = hostOrigin(scheme, authority);
  if (req.headers.origin !== undefined && req.headers.origin !== origin) throw new HttpError(403, 'origin_not_allowed');
  if (bootstrap && !proxied && localBootstrap(req, origin)) return { origin, bootstrapOnly: true, proxied: false };
  // Docker: the operator's configured server address and published port
  // (config.mjs), one exact plain-HTTP origin; never TLS or a proxy hop, and
  // the same bootstrap-only paths as the native check above.
  if (bootstrap && !config && !proxied && setupOrigin !== null && origin === setupOrigin && req.socket.encrypted !== true) return { origin, bootstrapOnly: true, proxied: false };
  const endpoint = config?.[scheme];
  if (!endpoint?.enabled) {
    const knownHost = config && [config.https, config.http].flatMap(endpointOrigins).some(value => new URL(value).hostname === new URL(origin).hostname);
    throw new HttpError(403, scheme === 'http' && knownHost ? 'http_disabled' : 'origin_not_allowed');
  }
  // The main address or one of the endpoint's explicit extra addresses.
  if (!endpointOrigins(endpoint).includes(origin) || req.socket.localPort !== endpoint.listenPort) throw new HttpError(403, 'origin_not_allowed');
  if (scheme === 'https' && (endpoint.mode === 'proxy' ? !proxied : req.socket.encrypted !== true)) throw new HttpError(403, 'origin_not_allowed');
  if (scheme === 'http' && req.socket.encrypted === true) throw new HttpError(403, 'origin_not_allowed');
  return { origin, bootstrapOnly: false, proxied };
}

// While bootstrapping only the first-setup console answers, plus the Proxmox
// setup script its second step tells the operator to run on the Proxmox host
// (a read-only GET of a public script; were it refused, the
// step shown could not be done before 서버 접속 설정).
export function bootstrapPath(path) {
  return path === '/healthz' || path === '/admin' || path.startsWith('/admin/') || path.startsWith('/api/v1/admin/') || path === '/setup/proxmox.py';
}

// Released Docker updaters used a plain published backend health socket with
// the configured primary authority. Preserve only this unauthenticated read;
// it cannot admit a login, enrollment, WebSocket or another Host/Origin.
export function legacyProxyHealthProbe(req, config) {
  if (!config?.https.enabled || config.https.mode !== 'proxy' || config.primaryOrigin !== config.https.origin || req.method !== 'GET' || req.url !== '/healthz' || req.socket.encrypted === true || req.socket.localPort !== config.https.listenPort) return false;
  if (['origin', 'authorization', 'cookie', 'user-agent', 'x-forwarded-for', 'x-forwarded-host', 'x-forwarded-proto', 'x-real-ip', 'forwarded', 'via'].some(name => req.headers[name] !== undefined)) return false;
  try { return hostOrigin('https', req.headers.host) === config.primaryOrigin; } catch { return false; }
}

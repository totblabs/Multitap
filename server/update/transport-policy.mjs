import { isIP } from 'node:net';
import { isAbsolute } from 'node:path';

// This is installation policy, never an inference from Host/X-Forwarded-*.
export const brokerTransportProtocol = 'broker-transport-v1';

export function normalizeBrokerOrigin(value, { allowInsecureHttp = false } = {}) {
  if (typeof value !== 'string' || value.length > 512 || /[\s\x00-\x20\x7f\\@?#%]/u.test(value)) throw Error('invalid_public_origin');
  const raw = /^(https?):\/\/(\[[0-9a-f:.]+\]|[^/:]+)(?::([0-9]{1,5}))?\/?$/i.exec(value);
  if (!raw || (raw[3] !== undefined && (Number(raw[3]) < 1 || Number(raw[3]) > 65535))) throw Error('invalid_public_origin');
  let url;
  try { url = new URL(value); } catch { throw Error('invalid_public_origin'); }
  const hostname = url.hostname.replace(/^\[|\]$/g, '');
  if (!isIP(hostname)) {
    const labels = hostname.replace(/\.$/, '').split('.');
    if (hostname.length > 253 || labels.some(label => !/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/i.test(label))) throw Error('invalid_public_origin');
  }
  // WHATWG URLs otherwise turn 127.1, octal and hexadecimal authorities into
  // a different-looking IPv4 origin. All clients use canonical dotted decimal.
  if (isIP(url.hostname) === 4 && raw[2].replace(/\.$/, '') !== url.hostname) throw Error('invalid_public_origin');
  if (!['http:', 'https:'].includes(url.protocol) || !url.hostname || url.username || url.password || url.port === '0' || !['', '/'].includes(url.pathname) || url.search || url.hash ||
      (url.protocol === 'http:' && allowInsecureHttp !== true)) throw Error('invalid_public_origin');
  return url.origin;
}

export function brokerWebSocketOrigin(origin) {
  // Only a previously configured origin may reach this URL constructor. HTTP
  // opt-in is checked at configuration/admission, not granted by a response URL.
  const value = normalizeBrokerOrigin(origin, { allowInsecureHttp: true });
  const url = new URL(value);
  url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
  return url.origin;
}

export function deploymentPort(value = '18480') {
  if ((typeof value !== 'string' && typeof value !== 'number') || !/^[0-9]{1,5}$/.test(String(value))) throw Error('invalid_port');
  const port = Number(value);
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw Error('invalid_port');
  return port;
}

export function explicitHttpOptIn(value) {
  if (value === undefined || value === '' || value === '0') return false;
  if (value === '1') return true;
  throw Error('invalid_http_opt_in');
}

export function deploymentTransport(env = {}) {
  const allowInsecureHttp = explicitHttpOptIn(env.MULTITAP_ALLOW_INSECURE_HTTP);
  const publicOrigin = normalizeBrokerOrigin(env.MULTITAP_PUBLIC_ORIGIN ?? 'https://localhost', { allowInsecureHttp });
  const tlsMode = env.MULTITAP_TLS_MODE ?? 'proxy';
  if (!['proxy', 'direct'].includes(tlsMode)) throw Error('invalid_tls_mode');
  const certFile = env.MULTITAP_TLS_CERT_FILE ?? null, keyFile = env.MULTITAP_TLS_KEY_FILE ?? null;
  if (tlsMode === 'direct') {
    if (!publicOrigin.startsWith('https://') || typeof certFile !== 'string' || typeof keyFile !== 'string' || !isAbsolute(certFile) || !isAbsolute(keyFile) || certFile === keyFile || /[\x00-\x1f\x7f]/.test(certFile + keyFile)) throw Error('invalid_tls_configuration');
  } else if (certFile !== null || keyFile !== null) throw Error('invalid_tls_configuration');
  const host = env.MULTITAP_HOST ?? '127.0.0.1';
  if (typeof host !== 'string' || host.includes('%') || (host !== 'localhost' && !isIP(host))) throw Error('invalid_bind_host');
  // Bind and local health use the same numeric address, without a second DNS
  // choice of ::1 versus 127.0.0.1 for an explicit localhost bind.
  return { publicOrigin, allowInsecureHttp, tlsMode, tlsFiles: tlsMode === 'direct' ? { certFile, keyFile } : null, host: host === 'localhost' ? '127.0.0.1' : host, port: deploymentPort(env.MULTITAP_PORT) };
}

export function describeTransport(publicOrigin, { allowInsecureHttp = false, directTls = false } = {}) {
  const origin = normalizeBrokerOrigin(publicOrigin, { allowInsecureHttp });
  const scheme = new URL(origin).protocol.slice(0, -1);
  if (directTls && scheme !== 'https') throw Error('invalid_tls_configuration');
  return Object.freeze({ protocol: brokerTransportProtocol, publicOrigin: origin, scheme, allowInsecureHttp: scheme === 'http' && allowInsecureHttp === true, directTls: directTls === true });
}

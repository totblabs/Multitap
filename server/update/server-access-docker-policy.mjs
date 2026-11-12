import { isIP } from 'node:net';
import { normalizeServerAccess, serverAccessHash, serverAccessTransport } from './server-access-policy.mjs';

const fail = () => { throw Error('server_access_docker_mapping_invalid'); };
const integer = value => Number.isInteger(value) && value >= 1 && value <= 65535;
export function normalizeDockerBindings(values) {
  if (!Array.isArray(values) || values.length > 128) fail();
  const rows = values.map(row => {
    if (!row || Object.keys(row).sort().join(',') !== 'containerPort,hostIp,hostPort,protocol' || !integer(row.hostPort) || !integer(row.containerPort) || !['tcp', 'udp'].includes(row.protocol) || typeof row.hostIp !== 'string' || (row.hostIp !== '' && (!isIP(row.hostIp) || row.hostIp.includes('%')))) fail();
    return { hostPort: row.hostPort, containerPort: row.containerPort, protocol: row.protocol, hostIp: row.hostIp };
  });
  const key = row => [row.containerPort, row.protocol, row.hostPort, row.hostIp].join('/');
  if (new Set(rows.map(key)).size !== rows.length) fail();
  return rows.sort((a, b) => key(a).localeCompare(key(b)));
}
export function inspectDockerBindings(container) {
  const rows = [];
  for (const [target, values] of Object.entries(container?.HostConfig?.PortBindings ?? {})) {
    const match = /^([1-9][0-9]{0,4})\/(tcp|udp)$/.exec(target);
    if (!match || !Array.isArray(values)) fail();
    for (const value of values) rows.push({ containerPort: Number(match[1]), protocol: match[2], hostPort: Number(value.HostPort), hostIp: value.HostIp ?? '' });
  }
  return normalizeDockerBindings(rows);
}
export function planDockerAccess(config, { currentConfig, bindings, proxyPort }) {
  config = normalizeServerAccess(config); bindings = normalizeDockerBindings(bindings);
  const oldTargets = new Set(currentConfig ? [currentConfig.https, currentConfig.http].filter(value => value.enabled).map(value => value.listenPort) : [18480]);
  const preserved = bindings.filter(row => row.protocol !== 'tcp' || !oldTargets.has(row.containerPort));
  const dockerPorts = [], next = [...preserved];
  for (const [scheme, row] of [['https', config.https], ['http', config.http]]) if (row.enabled) {
    if (!['0.0.0.0', '::'].includes(row.listenHost)) throw Error('server_access_docker_bind_unreachable');
    const existing = bindings.find(value => value.protocol === 'tcp' && value.containerPort === row.listenPort);
    // An external origin/NAT port is NEVER substituted for a Docker host port.
    const hostPort = existing?.hostPort ?? (scheme === 'https' && row.mode === 'proxy' ? proxyPort : row.listenPort);
    const hostIp = existing?.hostIp ?? '0.0.0.0';
    dockerPorts.push({ hostPort, containerPort: row.listenPort, protocol: 'tcp', scheme });
    if (!next.some(value => value.protocol === 'tcp' && value.containerPort === row.listenPort && value.hostPort === hostPort && value.hostIp === hostIp)) next.push({ hostPort, containerPort: row.listenPort, protocol: 'tcp', hostIp });
  }
  for (const row of next) if (next.some(other => other !== row && other.protocol === row.protocol && other.hostPort === row.hostPort && other.containerPort !== row.containerPort && (other.hostIp === row.hostIp || !other.hostIp || !row.hostIp || ['0.0.0.0', '::'].includes(other.hostIp) || ['0.0.0.0', '::'].includes(row.hostIp)))) throw Error('server_access_docker_port_conflict');
  return { dockerPorts, bindings: normalizeDockerBindings(next) };
}
export function decodeDockerAccessManifest(value) {
  if (!value || Object.keys(value).sort().join(',') !== 'bindings,config,dockerPorts,revision,schema' || value.schema !== 1) fail();
  const config = normalizeServerAccess(value.config), revision = serverAccessHash(config), bindings = normalizeDockerBindings(value.bindings);
  if (value.revision !== revision || !Array.isArray(value.dockerPorts) || value.dockerPorts.length < 1 || value.dockerPorts.length > 2) fail();
  for (const row of value.dockerPorts) if (!row || Object.keys(row).sort().join(',') !== 'containerPort,hostPort,protocol,scheme' || !['http', 'https'].includes(row.scheme) || !config[row.scheme].enabled || row.protocol !== 'tcp' || row.containerPort !== config[row.scheme].listenPort || !bindings.some(value => value.protocol === 'tcp' && value.containerPort === row.containerPort && value.hostPort === row.hostPort)) fail();
  if (new Set(value.dockerPorts.map(row => row.scheme)).size !== value.dockerPorts.length || value.dockerPorts.length !== [config.https, config.http].filter(row => row.enabled).length) fail();
  return { schema: 1, config, revision, dockerPorts: value.dockerPorts, bindings };
}
export function renderDockerAccessOverlay(input) {
  const value = decodeDockerAccessManifest(input), transport = serverAccessTransport(value.config);
  const env = { MULTITAP_SERVER_ACCESS_FILE: '/var/lib/multitap/server-access.json', MULTITAP_PUBLIC_ORIGIN: transport.publicOrigin, MULTITAP_ALLOW_INSECURE_HTTP: transport.allowInsecureHttp ? '1' : '0', MULTITAP_HOST: transport.host, MULTITAP_PORT: String(transport.port), MULTITAP_TLS_MODE: transport.tlsMode, MULTITAP_TLS_CERT_FILE: transport.tlsFiles?.certFile ?? '', MULTITAP_TLS_KEY_FILE: transport.tlsFiles?.keyFile ?? '', MULTITAP_TRUSTED_PROXIES: value.config.trustedProxies.join(',') || 'none' };
  let text = '# Multitap managed server access\nservices:\n  broker:\n    environment:\n';
  for (const [key, entry] of Object.entries(env)) text += '      ' + key + ': ' + JSON.stringify(entry) + '\n';
  text += '      MULTITAP_ACCESS_OPERATION_ID: "${MULTITAP_ACCESS_OPERATION_ID:-}"\n    ports: !override\n';
  for (const row of value.bindings) text += '      - target: ' + row.containerPort + '\n        published: ' + JSON.stringify(String(row.hostPort)) + '\n        protocol: ' + row.protocol + '\n' + (row.hostIp ? '        host_ip: ' + JSON.stringify(row.hostIp) + '\n' : '');
  return text;
}

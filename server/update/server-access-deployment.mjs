import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { createServer, isIP } from 'node:net';
import { networkInterfaces } from 'node:os';
import { lookup } from 'node:dns/promises';
import { constants } from 'node:fs';
import { lstat, readFile, mkdir, open, rename, unlink, realpath } from 'node:fs/promises';
import { join, dirname } from 'node:path';
import { randomBytes, createHash, X509Certificate, createPrivateKey } from 'node:crypto';
import { deploymentEnvironmentValues, deploymentEnvironment, bootstrapHealthOrigin } from './deployment-health.mjs';
import { normalizeServerAccess, legacyServerAccess, serverAccessHash, serverAccessTransport, serverAccessListeners, serverAccessPortMismatches, allowedServerOrigins, probedServerOrigins, decodeServerAccessState, serverAccessUnconfiguredRevision } from './server-access-policy.mjs';
import { localAccessRequest, publicAccessRequest, validAccessHealth } from './server-access-health.mjs';
import { inspectDockerBindings, planDockerAccess, decodeDockerAccessManifest, renderDockerAccessOverlay } from './server-access-docker-policy.mjs';
import { feedUrl } from './protocol.mjs';

const execute = promisify(execFile), digest = bytes => createHash('sha256').update(bytes).digest('hex');
const uuid = /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/;
const fail = code => { throw Error('server_access_' + code); };
const defaultRun = async (program, args) => (await execute(program, args, { maxBuffer: 1048576, timeout: 30000, env: { PATH: '/usr/sbin:/usr/bin:/sbin:/bin', LANG: 'C.UTF-8' } })).stdout;
async function ownedDirectory(path, { data = false } = {}) {
  const stat = await lstat(path);
  if (!stat.isDirectory() || stat.isSymbolicLink() || await realpath(path) !== path || (!data && (stat.uid !== 0 || (stat.mode & 0o022)))) fail('storage_invalid');
  return stat;
}
async function protectedBytes(path, { optional = false, maximum = 65536 } = {}) {
  let stat;
  try { stat = await lstat(path); } catch (error) { if (optional && error.code === 'ENOENT') return null; throw Error('server_access_storage_invalid'); }
  if (!stat.isFile() || stat.isSymbolicLink() || stat.uid !== 0 || (stat.mode & 0o022) || stat.size > maximum) fail('storage_invalid');
  const file = await open(path, constants.O_RDONLY | constants.O_NOFOLLOW);
  try { const actual = await file.stat(); if (actual.dev !== stat.dev || actual.ino !== stat.ino) fail('storage_invalid'); const bytes = await file.readFile(); if (bytes.length > maximum) fail('storage_invalid'); return bytes; } finally { await file.close(); }
}
async function durableFile(path, bytes, { gid = 0, exclusive = false, mode = 0o640 } = {}) {
  const parent = dirname(path); await ownedDirectory(parent, { data: true });
  const temporary = exclusive ? path : path + '.' + randomBytes(16).toString('hex') + '.tmp';
  const file = await open(temporary, 'wx', mode);
  try { await file.chown(0, gid); await file.chmod(mode); await file.writeFile(bytes); await file.sync(); } finally { await file.close(); }
  if (!exclusive) await rename(temporary, path);
  const directory = await open(parent, 'r'); try { await directory.sync(); } finally { await directory.close(); }
}
export function accessIdentityDecision({ identity, witness, hasPolicy = false, hasJournal = false }) {
  const decode = bytes => { if (bytes === null) return null; const value = bytes.toString('utf8').trim(); if (!/^[a-f0-9]{64}$/.test(value)) fail('identity_invalid'); return value; };
  const current = decode(identity), known = decode(witness);
  if (current === null && (known !== null || hasPolicy || hasJournal)) fail('identity_missing');
  if (known !== null && current !== known) fail('identity_changed');
  return current; // null is allowed only for a genuinely unobserved legacy/bootstrap installation.
}

export function renderAccessEnvironment(text, config, { accessFile = '/var/lib/multitap/server-access.json', operationId = null } = {}) {
  deploymentEnvironmentValues(text); // data-only syntax/duplicate validation
  const transport = serverAccessTransport(config);
  const values = { MULTITAP_PUBLIC_ORIGIN: transport.publicOrigin, MULTITAP_ALLOW_INSECURE_HTTP: transport.allowInsecureHttp ? '1' : '0', MULTITAP_HOST: transport.host, MULTITAP_PORT: String(transport.port), MULTITAP_TLS_MODE: transport.tlsMode,
    MULTITAP_TLS_CERT_FILE: transport.tlsFiles?.certFile ?? null, MULTITAP_TLS_KEY_FILE: transport.tlsFiles?.keyFile ?? null, MULTITAP_TRUSTED_PROXIES: config.trustedProxies.join(',') || 'none', MULTITAP_SERVER_ACCESS_FILE: accessFile, MULTITAP_ACCESS_OPERATION_ID: operationId };
  const seen = new Set(), lines = [];
  for (const line of text.split(/\r?\n/)) {
    const match = /^([A-Z][A-Z0-9_]*)=/.exec(line), key = match?.[1];
    if (Object.hasOwn(values, key)) { seen.add(key); if (values[key] !== null) lines.push(key + '=' + values[key]); }
    else lines.push(line);
  }
  while (lines.at(-1) === '') lines.pop();
  for (const [key, value] of Object.entries(values)) if (!seen.has(key) && value !== null) lines.push(key + '=' + value);
  return lines.join('\n') + '\n';
}
export function admissionEnvironment(text, operationId) {
  if (operationId !== null && !uuid.test(operationId)) fail('operation_invalid');
  const clean = text.replace(/^MULTITAP_ACCESS_OPERATION_ID=[^\r\n]*(?:\r?\n|$)/gm, '');
  if (operationId === null) return clean;
  return 'MULTITAP_ACCESS_OPERATION_ID=' + operationId + (text.includes('\r\n') ? '\r\n' : '\n') + clean;
}
export function rebaseInstallationFeed(bytes, previousConfig, nextConfig) {
  const config = JSON.parse(bytes), original = feedUrl(config.feed, { allowInsecureHttp: config.allowInsecureHttp === true });
  if (!previousConfig || !allowedServerOrigins(previousConfig).includes(original.origin) || original.pathname !== '/downloads/server-updates.json') return bytes;
  const next = new URL('/downloads/server-updates.json', normalizeServerAccess(nextConfig).primaryOrigin);
  config.feed = next.href; config.allowInsecureHttp = next.protocol === 'http:';
  feedUrl(config.feed, { allowInsecureHttp: config.allowInsecureHttp });
  return Buffer.from(JSON.stringify(config));
}
async function freePort(host, port) {
  await new Promise((resolve, reject) => {
    const server = createServer(socket => socket.destroy()), timer = setTimeout(() => { server.close(); reject(Error('server_access_listener_unverified')); }, 2000);
    server.once('error', error => { clearTimeout(timer); reject(Error(error.code === 'EADDRINUSE' ? 'server_access_listener_busy' : error.code === 'EADDRNOTAVAIL' ? 'server_access_listener_host_unavailable' : 'server_access_listener_unverified')); });
    server.listen({ host, port }, () => server.close(error => { clearTimeout(timer); error ? reject(Error('server_access_listener_unverified')) : resolve(); }));
  });
}
// This machine's own address (or a name for it): no router can map ports there.
async function ownHost(hostname) {
  const local = new Set(Object.values(networkInterfaces()).flat().map(row => row.address.split('%')[0].toLowerCase()));
  const own = address => local.has(address.toLowerCase()) || address.startsWith('127.') || address === '::1';
  if (isIP(hostname)) return own(hostname);
  try { return (await Promise.race([lookup(hostname, { all: true }), new Promise((_, reject) => setTimeout(reject, 2000).unref())])).some(row => own(row.address)); }
  catch { return false; } // Unknown: the post-apply check of the real address decides.
}
// External port ≠ listener on this machine's own address, or HTTP on the UDP
// media port (a new collision only), is refused before anything is written.
export async function portPreflight(config, { dockerPorts = null, udpPort = null, appliedConfig = null, isOwnHost = ownHost } = {}) {
  for (const row of serverAccessPortMismatches(config, { dockerPorts })) if (await isOwnHost(row.hostname)) fail('external_port_conflict');
  if (udpPort !== null && config.http.enabled && config.http.listenPort === udpPort && !(appliedConfig?.http.enabled && appliedConfig.http.listenPort === udpPort)) fail('udp_port_conflict');
}
function sameLocal(a, b) { return a.protocol === b.protocol && a.host === b.host && a.port === b.port; }
export function accessProbeTransport(config, environmentText) {
  return config ? serverAccessTransport(config) : deploymentEnvironment(environmentText);
}

export class ServerAccessDeployment {
  constructor({ adapter, directory, fixture = null, updaterConfigPath = null, run = defaultRun, localRequest = localAccessRequest, publicRequest = publicAccessRequest, sleep = ms => new Promise(resolve => setTimeout(resolve, ms)) }) {
    this.adapter = adapter; this.directory = directory; this.run = run; this.localRequest = localRequest; this.publicRequest = publicRequest; this.sleep = sleep;
    this.kind = adapter.platform === 'linux-x64-docker' ? 'docker' : adapter.platform === 'linux-x64-systemd' ? 'systemd' : 'unsupported';
    if (fixture !== null && (this.kind !== 'systemd' || !fixture || Object.keys(fixture).join(',') !== 'id' || !/^[a-f0-9]{8,32}$/.test(fixture.id))) fail('fixture_invalid');
    this.serviceName = fixture ? 'multitap-access-fixture-' + fixture.id : 'multitap-broker';
    this.unitPath = '/etc/systemd/system/' + this.serviceName + '.service';
    this.systemData = fixture ? '/var/lib/' + this.serviceName : '/var/lib/multitap';
    this.systemEnv = fixture ? '/etc/' + this.serviceName + '/broker.env' : '/etc/multitap/broker.env';
    this.updaterConfigPath = updaterConfigPath ?? (fixture ? '/etc/' + this.serviceName + '/update.json' : this.kind === 'docker' ? '/etc/multitap/docker-update.json' : '/etc/multitap/update.json');
  }
  async initialize() {
    if (!['systemd', 'docker'].includes(this.kind) || process.platform !== 'linux' || process.getuid() !== 0) fail('deployment_unsupported');
    await mkdir(this.directory, { recursive: true, mode: 0o700 }); await ownedDirectory(this.directory);
    if (this.kind === 'docker') {
      // A crash may leave access manifest/overlay or the running container at
      // different transaction stages. Validate the immutable project first;
      // let the engine restore its durable plan before strict active checks.
      await this.adapter.trust(this.adapter.config.projectDirectory, 'directory');
      for (const path of Object.values(this.adapter.paths())) await this.adapter.trust(path);
      const volume = await this.adapter.volume(); this.data = volume.Mountpoint;
      this.envPath = this.adapter.paths().env; this.metaPath = join(this.adapter.config.projectDirectory, 'multitap-server-access.json'); this.overlayPath = join(this.adapter.config.projectDirectory, 'multitap-server-access.yaml');
      if ([this.adapter.config.composeFile, this.adapter.config.envFile, this.adapter.config.overrideFile].some(name => ['multitap-server-access.json', 'multitap-server-access.yaml'].includes(name))) fail('docker_path_conflict');
    } else {
      await this.adapter.version();
      const unit = await protectedBytes(this.unitPath);
      if (!unit.toString('utf8').includes('# Multitap managed broker')) fail('deployment_unsupported');
      this.data = this.systemData; this.envPath = this.systemEnv;
    }
    const data = await ownedDirectory(this.data, { data: true }); this.gid = data.gid; this.uid = data.uid;
    this.accessFile = join(this.data, 'server-access.json'); this.identityFile = join(this.data, 'server-access.identity');
    this.identityWitness = join(this.directory, 'installation.identity');
    this.runtimeAccessFile = this.kind === 'docker' ? '/var/lib/multitap/server-access.json' : this.accessFile;
    const env = deploymentEnvironmentValues((await protectedBytes(this.envPath)).toString('utf8'));
    const policy = await protectedBytes(this.accessFile, { optional: true, maximum: 32768 });
    if (env.MULTITAP_SERVER_ACCESS_FILE && policy === null) fail('state_missing');
    let identity = await protectedBytes(this.identityFile, { optional: true, maximum: 128 });
    const witness = await protectedBytes(this.identityWitness, { optional: true, maximum: 128 });
    // Adopt an existing valid identity without moving/chowning the normal
    // broker-writable data directory. Old feature journals establish that an
    // identity already existed, even before this witness was introduced.
    const history = identity === null && witness === null ? await protectedBytes(join(dirname(this.directory), 'server-access-state', 'status.json'), { optional: true, maximum: 262144 }) : null;
    accessIdentityDecision({ identity, witness, hasPolicy: policy !== null, hasJournal: history !== null });
    if (identity === null) { try { await durableFile(this.identityFile, randomBytes(32).toString('hex') + '\n', { gid: this.gid, exclusive: true }); } catch (error) { if (error.code !== 'EEXIST') throw error; } identity = await protectedBytes(this.identityFile, { maximum: 128 }); }
    this.installationId = identity.toString('utf8').trim(); if (!/^[a-f0-9]{64}$/.test(this.installationId)) fail('identity_invalid');
    if (witness === null) {
      try { await durableFile(this.identityWitness, this.installationId + '\n', { exclusive: true, mode: 0o600 }); }
      catch (error) { if (error.code !== 'EEXIST') throw error; }
    }
    accessIdentityDecision({ identity, witness: await protectedBytes(this.identityWitness, { maximum: 128 }) });
  }
  async current() {
    const envBytes = await protectedBytes(this.envPath), env = deploymentEnvironmentValues(envBytes.toString('utf8'));
    if (env.MULTITAP_SERVER_ACCESS_FILE && env.MULTITAP_SERVER_ACCESS_FILE !== this.runtimeAccessFile) fail('storage_unsupported');
    const bytes = await protectedBytes(this.accessFile, { optional: true, maximum: 32768 });
    if (env.MULTITAP_SERVER_ACCESS_FILE && bytes === null) fail('state_missing');
    const identity = await protectedBytes(this.identityFile, { optional: true, maximum: 128 }), witness = await protectedBytes(this.identityWitness, { maximum: 128 });
    if (accessIdentityDecision({ identity, witness, hasPolicy: bytes !== null }) !== this.installationId) fail('identity_changed');
    const saved = bytes === null ? null : decodeServerAccessState(JSON.parse(bytes));
    const config = saved?.config ?? (env.MULTITAP_PUBLIC_ORIGIN ? legacyServerAccess(env) : null), revision = saved?.revision ?? (config ? serverAccessHash(config) : serverAccessUnconfiguredRevision);
    let transport = accessProbeTransport(config, envBytes.toString('utf8'));
    let dockerPorts = [], container = null, bindings = [];
    if (this.kind === 'docker') {
      await this.adapter.files(); container = await this.adapter.container(); this.adapter.validateContainer(container, { running: true }); bindings = inspectDockerBindings(container);
      if (this.adapter.accessManifest) dockerPorts = this.adapter.accessManifest.dockerPorts;
      else if (config) for (const [scheme, row] of [['https', config.https], ['http', config.http]]) if (row.enabled) { const binding = bindings.find(value => value.protocol === 'tcp' && value.containerPort === row.listenPort); if (!binding) fail('docker_mapping_invalid'); dockerPorts.push({ hostPort: binding.hostPort, containerPort: row.listenPort, protocol: 'tcp', scheme }); }
      const primary = config ? dockerPorts.find(row => row.scheme === (config.primaryOrigin === config.https.origin ? 'https' : 'http')) : null;
      transport = { ...transport, host: '127.0.0.1', port: primary?.hostPort ?? this.adapter.config.healthPort };
    }
    return { config, revision, legacy: !saved, installationId: this.installationId, deploymentKind: this.kind, managed: true, applySupported: true, transport, dockerPorts, bindings, internalPort: config ? serverAccessTransport(config).port : Number(env.MULTITAP_PORT ?? 18480), containerId: container?.Id ?? null, image: container?.Image ?? null,
      envSha256: digest(envBytes), accessSha256: bytes === null ? null : digest(bytes), requiresProxyConfiguration: !saved && config?.https.enabled === true && config.https.mode === 'proxy' && !env.MULTITAP_TRUSTED_PROXIES };
  }
  async probeLocal(current, token, { endConnections = false } = {}) {
    if(this.kind!=='docker')return this.localRequest(current.transport,{activityToken:token,...(endConnections?{endConnections:true}:{})});
    try{return JSON.parse(await this.adapter.runInput(['exec','--interactive',current.containerId,'/usr/local/bin/node','/app/update/server-access-health.mjs','--local-probe'],Buffer.from(JSON.stringify({config:current.config,port:current.internalPort,...(token===undefined?{}:{activityToken:token}),...(endConnections?{endConnections:true}:{})}))));}catch{return null;}
  }
  activity(current, token) { return this.probeLocal(current, token); }
  // The broker ends its own remote connections; only the fence holder may ask.
  endConnections(current, token) { return typeof token === 'string' ? this.probeLocal(current, token, { endConnections: true }) : Promise.resolve(null); }
  async unchanged(previous, token) {
    const current = await this.current();
    if (current.revision !== previous.revision || current.envSha256 !== previous.envSha256 || current.accessSha256 !== previous.accessSha256) return false;
    const report = await this.activity(current, token); return report?.protocol === 'server-access-activity-v1' && report.fence === true;
  }
  async unchangedAfterRestart(previous) {
    const current = await this.current(); if (current.revision !== previous.revision) return false;
    // Docker probes inside the owned container, not its host-side mapped port.
    const localTransport = this.kind === 'docker' ? { ...current.transport, host: '127.0.0.1', port: current.internalPort } : current.transport;
    return validAccessHealth(await this.probeLocal(current), { config: current.config, revision: current.revision, installationId: this.installationId, origin: current.config?.primaryOrigin ?? bootstrapHealthOrigin(localTransport), localTransport, allowHealthProbeOnly: true });
  }
  async tlsPreflight(config) {
    if (!config.https.enabled || config.https.mode !== 'direct') return;
    let certPath = config.https.certFile, keyPath = config.https.keyFile;
    if (this.kind === 'docker') {
      if (!this.adapter.config.tlsDirectory || certPath !== '/etc/multitap/tls/cert.pem' || keyPath !== '/etc/multitap/tls/key.pem') fail('docker_tls_mount_required');
      certPath = join(this.adapter.config.tlsDirectory, 'cert.pem'); keyPath = join(this.adapter.config.tlsDirectory, 'key.pem');
    }
    const cert = await protectedBytes(certPath, { maximum: 262144 }), key = await protectedBytes(keyPath, { maximum: 65536 });
    try {
      const certificate = new X509Certificate(cert), privateKey = createPrivateKey(key), hostname = new URL(config.https.origin).hostname.replace(/^\[|\]$/g, '');
      const isAddress = isIP(hostname) !== 0;
      if (!certificate.checkPrivateKey(privateKey) || !(isAddress ? certificate.checkIP(hostname) : certificate.checkHost(hostname, { subject: 'never' })) || Date.parse(certificate.validFrom) > Date.now() || Date.parse(certificate.validTo) <= Date.now()) fail('tls_identity_invalid');
    } catch { fail('tls_identity_invalid'); }
    // The broker remains unprivileged. Supplied files must already be readable
    // by its owned account; do not chmod somebody else's key or run as root.
    for (const path of [certPath, keyPath]) {
      const stat = await lstat(path);
      if (!(stat.mode & 0o004) && !(stat.gid === this.gid && (stat.mode & 0o040)) && !(stat.uid === this.uid && (stat.mode & 0o400))) fail('tls_not_readable');
      for (let parent = dirname(path); ; parent = dirname(parent)) {
        const directory = await lstat(parent);
        if (!directory.isDirectory() || directory.isSymbolicLink() || (!(directory.mode & 0o001) && !(directory.gid === this.gid && (directory.mode & 0o010)) && !(directory.uid === this.uid && (directory.mode & 0o100)))) fail('tls_not_readable');
        if (dirname(parent) === parent) break;
      }
    }
  }
  async preflight(input, current) {
    const config = normalizeServerAccess(input); await this.tlsPreflight(config);
    let plannedDockerPorts = [];
    if (this.kind === 'docker') {
      const version = String(await this.adapter.run(['compose', 'version', '--short'])).trim().replace(/^v/, ''), parts = /^(\d+)\.(\d+)\.(\d+)/.exec(version);
      if (!parts || Number(parts[1]) < 2 || (Number(parts[1]) === 2 && (Number(parts[2]) < 24 || (Number(parts[2]) === 24 && Number(parts[3]) < 4)))) fail('docker_compose_unsupported');
      const plan = planDockerAccess(config, { currentConfig: current.config, bindings: current.bindings, proxyPort: this.adapter.config.healthPort }); plannedDockerPorts = plan.dockerPorts;
      for (const row of plan.bindings) if (!current.bindings.some(value => value.hostPort === row.hostPort && value.protocol === row.protocol && value.hostIp === row.hostIp)) await freePort(row.hostIp || '0.0.0.0', row.hostPort);
      for (const listener of serverAccessListeners(config)) if (!current.config || !serverAccessListeners(current.config).some(value => sameLocal(value, listener))) {
        const program = 'const n=require("node:net"),s=n.createServer(c=>c.destroy()),t=setTimeout(()=>process.exit(2),2000);s.on("error",()=>process.exit(1));s.listen({host:process.argv[1],port:Number(process.argv[2])},()=>s.close(()=>{clearTimeout(t);process.exit(0)}));';
        try { await this.adapter.run(['exec', current.containerId, '/usr/local/bin/node', '-e', program, listener.host, String(listener.port)]); } catch { fail('listener_unverified'); }
      }
    } else {
      for (const listener of serverAccessListeners(config)) if (!current.config || !serverAccessListeners(current.config).some(value => sameLocal(value, listener))) await freePort(listener.host, listener.port);
      const threshold = Number((await readFile('/proc/sys/net/ipv4/ip_unprivileged_port_start', 'utf8')).trim());
      if (!Number.isInteger(threshold) || threshold < 0 || threshold > 65535) fail('listener_privilege_unverified');
      if (serverAccessListeners(config).some(row => row.port < threshold)) {
        const capabilities = String(await this.run('/bin/systemctl', ['show', this.serviceName, '--property=AmbientCapabilities', '--value'])).trim().toLowerCase().split(/\s+/);
        if (!capabilities.includes('cap_net_bind_service')) fail('privileged_listener_not_configured');
      }
    }
    // Compose publishes UDP media on 18481 unless .env says otherwise.
    const env = deploymentEnvironmentValues((await protectedBytes(this.envPath)).toString('utf8')), udp = env.MULTITAP_UDP_PORT ?? (this.kind === 'docker' ? '18481' : '');
    await portPreflight(config, { dockerPorts: this.kind === 'docker' ? plannedDockerPorts : null, udpPort: /^\d{1,5}$/.test(udp) ? Number(udp) : null, appliedConfig: current.config });
    return { passed: true, plannedDockerPorts, checks: [{ name: 'configuration', status: 'passed' }, { name: 'managed_installation', status: 'passed' }, { name: 'listeners', status: 'passed' },
      ...(config.https.enabled && config.https.mode === 'direct' ? [{ name: 'certificate_identity_and_key', status: 'passed' }] : []), { name: 'external_origin_tls_and_identity', status: 'pending', code: 'server_access_pending_until_apply' }] };
  }
  targets() { return [{ name: 'access', path: this.accessFile }, { name: 'environment', path: this.envPath }, { name: 'updater', path: this.updaterConfigPath }, ...(this.kind === 'docker' ? [{ name: 'dockerManifest', path: this.metaPath }, { name: 'dockerOverlay', path: this.overlayPath }] : [])]; }
  async prepare(input, { previous, operationId }) {
    if (!uuid.test(operationId)) fail('operation_invalid');
    const current = await this.current(); if (current.revision !== previous.revision || current.envSha256 !== previous.envSha256 || current.accessSha256 !== previous.accessSha256) fail('revision_changed');
    const config = normalizeServerAccess(input); await this.preflight(config, current);
    const operation = join(this.directory, operationId); await mkdir(operation, { mode: 0o700 }); await ownedDirectory(operation);
    const revision = serverAccessHash(config), state = { schema: 1, protocol: 'server-access-v1', revision, config, updatedAt: new Date().toISOString() };
    const env = (await protectedBytes(this.envPath)).toString('utf8'), planned = { access: JSON.stringify(state), environment: renderAccessEnvironment(env, config, { operationId, accessFile: this.runtimeAccessFile }) };
    planned.updater = rebaseInstallationFeed(await protectedBytes(this.updaterConfigPath), current.config, config).toString('utf8');
    let dockerManifest = null;
    if (this.kind === 'docker') {
      const plan = planDockerAccess(config, { currentConfig: current.config, bindings: current.bindings, proxyPort: this.adapter.config.healthPort });
      dockerManifest = decodeDockerAccessManifest({ schema: 1, config, revision, ...plan });
      planned.dockerManifest = JSON.stringify(dockerManifest); planned.dockerOverlay = renderDockerAccessOverlay(dockerManifest);
    }
    const snapshots = [];
    for (const target of this.targets()) {
      const before = await protectedBytes(target.path, { optional: !['environment', 'updater'].includes(target.name), maximum: 65536 });
      if (before !== null) await durableFile(join(operation, target.name + '.before'), before, { mode: 0o600, exclusive: true });
      const next = Buffer.from(planned[target.name]); await durableFile(join(operation, target.name + '.after'), next, { mode: 0o600, exclusive: true });
      snapshots.push({ ...target, beforeSha256: before === null ? null : digest(before), afterSha256: digest(next) });
    }
    const corePins = this.kind === 'docker' ? await this.dockerCorePins() : [];
    const job = { schema: 1, operationId, operation, config, revision, previous, snapshots, dockerManifest, corePins, installationId: this.installationId };
    await durableFile(join(operation, 'plan.json'), JSON.stringify(job), { mode: 0o600, exclusive: true });
    return { ...job, planSha256: digest(Buffer.from(JSON.stringify(job))) };
  }
  async validateJob(job) {
    if (!job || job.schema !== 1 || !uuid.test(job.operationId) || job.operation !== join(this.directory, job.operationId) || job.revision !== serverAccessHash(job.config) || job.installationId !== this.installationId) fail('recovery_required');
    await ownedDirectory(job.operation);
    const bytes = await protectedBytes(join(job.operation, 'plan.json'), { maximum: 262144 }); if (digest(bytes) !== job.planSha256) fail('recovery_required');
    const saved = JSON.parse(bytes), { planSha256, ...expected } = job;
    if (JSON.stringify(saved) !== JSON.stringify(expected)) fail('recovery_required');
    const targets = this.targets();
    if (job.snapshots.length !== targets.length || job.snapshots.some((row, index) => row.name !== targets[index].name || row.path !== targets[index].path)) fail('recovery_required');
    for (const row of job.snapshots) for (const [suffix, sha] of [['before', row.beforeSha256], ['after', row.afterSha256]]) if (sha !== null && digest(await protectedBytes(join(job.operation, row.name + '.' + suffix))) !== sha) fail('recovery_required');
    if (this.kind === 'docker' && JSON.stringify(await this.dockerCorePins()) !== JSON.stringify(job.corePins)) fail('installation_changed');
  }
  async dockerCorePins() {
    const paths = this.adapter.paths(), files = [paths.compose, paths.channel, paths.override];
    if (this.adapter.config.tlsDirectory) files.push(join(this.adapter.config.tlsDirectory, 'cert.pem'), join(this.adapter.config.tlsDirectory, 'key.pem'));
    return Promise.all(files.map(async path => ({ path, sha256: digest(await protectedBytes(path, { maximum: 1048576 })) })));
  }
  async replaceFrom(job, side) {
    for (const row of job.snapshots) {
      const current = await protectedBytes(row.path, { optional: true }); const actual = current === null ? null : digest(current);
      const allowed = [row.beforeSha256, row.afterSha256];
      if (row.name === 'environment' && row.beforeSha256 !== null) allowed.push(digest(Buffer.from(admissionEnvironment((await protectedBytes(join(job.operation, row.name + '.before'))).toString('utf8'), job.operationId))));
      if (!allowed.includes(actual)) fail('installation_changed');
      const wanted = side === 'before' ? row.beforeSha256 : row.afterSha256;
      if (wanted === null) { if (current !== null) { await unlink(row.path); const directory = await open(dirname(row.path), 'r'); try { await directory.sync(); } finally { await directory.close(); } } }
      else {
        let bytes = await protectedBytes(join(job.operation, row.name + '.' + side));
        if (row.name === 'environment' && side === 'before') bytes = Buffer.from(admissionEnvironment(bytes.toString('utf8'), job.operationId));
        await durableFile(row.path, bytes, { gid: row.name === 'access' ? this.gid : 0, mode: row.name === 'access' ? 0o640 : 0o600 });
      }
    }
  }
  async restart() {
    if (this.kind === 'systemd') await this.run('/bin/systemctl', ['restart', this.serviceName]);
    else { await this.adapter.loadAccessOverlay(); await this.adapter.run(this.adapter.compose(['up', '--detach', '--no-deps', '--no-build', '--pull', 'never', '--force-recreate', 'broker'])); }
  }
  async apply(job) {
    await this.validateJob(job);
    const current = await this.current();
    if (current.revision !== job.previous.revision || current.envSha256 !== job.previous.envSha256 || current.accessSha256 !== job.previous.accessSha256 || (this.kind === 'docker' && (current.containerId !== job.previous.containerId || current.image !== job.previous.image))) fail('installation_changed');
    await this.replaceFrom(job, 'after'); await this.restart();
  }
  async verify(job) {
    for (let attempt = 0; attempt < 8; attempt++) {
      let all = true;
      for (const origin of probedServerOrigins(job.config)) if (!validAccessHealth(await this.publicRequest(job.config, origin), { config: job.config, revision: job.revision, installationId: this.installationId, origin })) { all = false; break; }
      if (all) { const current = await this.current(); if (current.revision === job.revision) return true; }
      await this.sleep(500);
    }
    return false;
  }
  async rollback(job) {
    await this.validateJob(job);
    if (this.kind === 'docker') {
      const ids = String(await this.adapter.run(this.adapter.compose(['ps', '--all', '--quiet', 'broker']))).trim().split(/\s+/).filter(Boolean);
      if (ids.length > 1 || ids.some(value => !/^[a-f0-9]{64}$/.test(value))) fail('installation_changed');
      if (ids.length) {
        const values = JSON.parse(await this.adapter.run(['container', 'inspect', ids[0]]));
        if (!Array.isArray(values) || values.length !== 1 || values[0].Id !== ids[0]) fail('installation_changed');
        const actual = inspectDockerBindings(values[0]), text = JSON.stringify(actual);
        if (![JSON.stringify(job.previous.bindings), JSON.stringify(job.dockerManifest.bindings)].includes(text)) fail('installation_changed');
        this.adapter.validateContainer(values[0], { images: [job.previous.image], portBindings: actual });
      }
    }
    await this.replaceFrom(job, 'before'); await this.restart();
    for (let attempt = 0; attempt < 12; attempt++) { if (await this.unchangedAfterRestart(job.previous)) return true; await this.sleep(500); }
    return false;
  }
  async clearAdmissionMarker(job) {
    await this.validateJob(job);
    const bytes = await protectedBytes(this.envPath), env = deploymentEnvironmentValues(bytes.toString('utf8'));
    if (env.MULTITAP_ACCESS_OPERATION_ID === undefined) return true;
    if (env.MULTITAP_ACCESS_OPERATION_ID !== job.operationId) fail('installation_changed');
    await durableFile(this.envPath, admissionEnvironment(bytes.toString('utf8'), null), { mode: 0o600 }); return true;
  }
}

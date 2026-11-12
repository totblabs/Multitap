import { request } from 'node:https';
import { X509Certificate } from 'node:crypto';
import { WebSocket } from 'ws';
import { HttpError } from './errors.mjs';
import { standardConsoleModel, consoleDisplayMissing, consoleModelIssue } from './stream-console-profile.mjs';
import { normalizeTags, normalizePool } from './desktop-organize.mjs';
import { ProxmoxHealth, certificateFailure } from './proxmox-health.mjs';

const validNode = node => typeof node === 'string' && node.length <= 128 && /^[A-Za-z0-9_.-]+$/.test(node) && node !== '.' && node !== '..';
const validVmid = vmid => Number.isSafeInteger(vmid) && vmid >= 100 && vmid <= 999999999;
const validMachineUuid = value => typeof value === 'string' && /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(value) && !/^(?:0{32}|f{32})$/i.test(value.replaceAll('-', ''));
function registeredTarget(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || Object.keys(value).length !== 3 ||
      !['node', 'vmid', 'expectedUuid'].every(key => Object.hasOwn(value, key)) ||
      Object.keys(value).some(key => !['node', 'vmid', 'expectedUuid'].includes(key)) ||
      !validNode(value.node) || !validVmid(value.vmid) || !validMachineUuid(value.expectedUuid)) throw new HttpError(400, 'invalid_proxmox_target');
  return { node: value.node, vmid: value.vmid, expectedUuid: value.expectedUuid.toLowerCase() };
}
function measuredStatus(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new HttpError(502, 'proxmox_invalid_response');
  const whole = (n, max = Number.MAX_SAFE_INTEGER) => Number.isSafeInteger(n) && n > 0 && n <= max ? n : null;
  const memoryBytes = whole(value.maxmem);
  return {
    status: ['running', 'stopped'].includes(value.status) ? value.status : 'unknown',
    paused: value.qmpstatus === 'paused' || value.qmpstatus === 'suspended',
    uptimeSeconds: Number.isSafeInteger(value.uptime) && value.uptime >= 0 ? value.uptime : null,
    vcpus: whole(value.cpus, 4096), memoryBytes,
    memoryUsedBytes: Number.isSafeInteger(value.mem) && value.mem >= 0 && (memoryBytes === null || value.mem <= memoryBytes) ? value.mem : null,
    cpuUsage: typeof value.cpu === 'number' && Number.isFinite(value.cpu) && value.cpu >= 0 && value.cpu <= 1 ? value.cpu : null,
  };
}
const targetReadErrors = new Set(['proxmox_machine_uuid_mismatch', 'proxmox_machine_uuid_missing', 'proxmox_vm_not_found', 'proxmox_permission_denied', 'proxmox_certificate_error', 'proxmox_unreachable', 'proxmox_invalid_response', 'proxmox_request_failed']);

export function classifyOs(ostype) {
  if (typeof ostype !== 'string' || !ostype) return { pveOsType: null, osFamily: 'unknown', osSource: 'unknown' };
  const family = /^(?:wxp|w2k|w2k3|w2k8|wvista|win7|win8|win10|win11)$/.test(ostype) ? 'windows' : /^l2[46]$/.test(ostype) ? 'linux' : ['solaris', 'other'].includes(ostype) ? 'other' : 'unknown';
  return { pveOsType: ostype.slice(0, 40), osFamily: family, osSource: 'proxmox-config' };
}

// The VM config's `agent` option: "1" or "enabled=1,..." turns the QEMU guest
// agent on; absent, "0" or "enabled=0" leaves it off.
export function qemuAgentOptionEnabled(value) {
  return /^(?:enabled=)?1(?:,|$)/.test(String(value ?? '0'));
}
// hostpci0..hostpciN: a PCI device of the node given to this VM (passthrough,
// a vGPU through mdev=, a mapped device). Which kind is not readable here.
export const pciPassthrough = value => !!value && typeof value === 'object' && Object.keys(value).some(key => /^hostpci\d{1,2}$/.test(key) && typeof value[key] === 'string' && value[key].length > 0);
export function smbiosUuid(value) {
  if (typeof value !== 'string') return null;
  const match = /(?:^|,)uuid=([0-9a-f-]{36})(?:,|$)/i.exec(value);
  return match && /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i.test(match[1]) ? match[1].toLowerCase() : null;
}

export function validateProxmox(input, previous) {
  let url;
  try { url = new URL(input.endpoint); } catch { throw new HttpError(400, 'invalid_proxmox_endpoint'); }
  if (url.protocol !== 'https:' || url.username || url.password || url.search || url.hash || !['', '/'].includes(url.pathname)) throw new HttpError(400, 'invalid_proxmox_endpoint');
  if (typeof input.tokenId !== 'string' || !/^[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+![A-Za-z0-9_.-]+$/.test(input.tokenId)) throw new HttpError(400, 'invalid_proxmox_token_id');
  const endpoint = url.origin;
  const unchanged = endpoint === previous?.endpoint && input.tokenId === previous?.tokenId;
  const secret = input.secret || (unchanged ? previous?.secret : undefined);
  if (typeof secret !== 'string' || !/^[\x21-\x7e]{8,256}$/.test(secret)) throw new HttpError(400, 'proxmox_secret_required');
  const ca = input.ca === undefined ? (unchanged ? previous?.ca ?? '' : '') : input.ca;
  if (typeof ca !== 'string' || ca.length > 16000) throw new HttpError(400, 'invalid_proxmox_ca');
  if (ca) {
    try { new X509Certificate(ca); } catch { throw new HttpError(400, 'invalid_proxmox_ca'); }
  }
  // A pin: this exact certificate, whatever names it carries (the setup script
  // pins a node's own certificate when it lacks the address). It travels with
  // that certificate in `ca`, which then is the only trust anchor.
  const fingerprint = input.fingerprint === undefined ? (unchanged && ca === (previous?.ca ?? '') ? previous?.fingerprint ?? '' : '') : input.fingerprint;
  if (typeof fingerprint !== 'string' || (fingerprint && !validFingerprint(fingerprint))) throw new HttpError(400, 'invalid_proxmox_fingerprint');
  if (fingerprint) {
    let pinned = null;
    try { pinned = new X509Certificate(ca).fingerprint256; } catch { /* below */ }
    if (pinned !== fingerprint) throw new HttpError(400, 'invalid_proxmox_fingerprint');
  }
  let expiresAt = input.expiresAt === undefined ? (unchanged ? previous?.expiresAt ?? null : null) : input.expiresAt;
  if (expiresAt !== null) {
    if (typeof expiresAt !== 'string' || !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,3})?Z$/.test(expiresAt) || !Number.isFinite(Date.parse(expiresAt))) throw new HttpError(400, 'invalid_proxmox_expiry');
    const normalized = new Date(expiresAt).toISOString();
    if (normalized.slice(0, 19) !== expiresAt.slice(0, 19)) throw new HttpError(400, 'invalid_proxmox_expiry');
    expiresAt = normalized;
  }
  return { endpoint, tokenId: input.tokenId, secret, ca, ...(fingerprint ? { fingerprint } : {}), expiresAt };
}

// SHA-256 of the certificate (DER), uppercase hex with colons: how Proxmox
// and Node's fingerprint256 write it.
export const validFingerprint = value => typeof value === 'string' && /^[0-9A-F]{2}(?::[0-9A-F]{2}){31}$/.test(value);

// The TLS options of every connection to the configured Proxmox endpoint (API
// calls and the console WebSocket). rejectUnauthorized stays true in both
// forms. Pinned: the pinned certificate is the only trust anchor (a CA-issued
// one counts as an anchor by itself) and the peer's leaf must have exactly the
// pinned fingerprint instead of carrying the address as a name. Anything else
// fails with a certificate error; there is no unverified fallback.
export function proxmoxTls(config) {
  if (!config?.fingerprint) return { ca: config?.ca || undefined, rejectUnauthorized: true };
  const pinned = config.fingerprint;
  if (!validFingerprint(pinned) || typeof config.ca !== 'string' || !config.ca) {
    // A malformed stored pin never connects at all.
    return { ca: config.ca || undefined, rejectUnauthorized: true, checkServerIdentity: () => pinError('Stored Proxmox certificate pin is malformed') };
  }
  return { ca: config.ca, rejectUnauthorized: true, allowPartialTrustChain: true,
    checkServerIdentity: (_host, cert) => cert?.fingerprint256 === pinned ? undefined : pinError('Proxmox certificate does not match the pinned fingerprint') };
}
const pinError = message => Object.assign(new Error(message), { code: 'ERR_TLS_CERT_PIN_MISMATCH' });

export class Proxmox {
  // health: the last certificate refusal, for the admin console (proxmox-health.mjs).
  constructor({ consoleSocket = (url, protocols, options) => new WebSocket(url, protocols, options), health = new ProxmoxHealth() } = {}) { this.consoleSocket = consoleSocket; this.health = health; }
  async call(config, path, method = 'GET', body = '', { signal } = {}) {
    return new Promise((resolve, reject) => {
      const req = request(new URL('/api2/json' + path, config.endpoint), {
        // A pinned connection never shares the pooled sockets or resumed
        // sessions of other settings: every request makes its own handshake.
        method, ...proxmoxTls(config), ...(config.fingerprint ? { agent: false } : {}), signal: signal ? AbortSignal.any([signal, AbortSignal.timeout(8000)]) : AbortSignal.timeout(8000),
        headers: { Authorization: `PVEAPIToken=${config.tokenId}=${config.secret}`, Accept: 'application/json', ...(['POST', 'PUT'].includes(method) ? { 'Content-Type': 'application/x-www-form-urlencoded', 'Content-Length': String(Buffer.byteLength(body)) } : {}) },
      }, res => {
        this.health?.reached(config);
        let length = 0; const chunks = [];
        res.on('data', chunk => { length += chunk.length; if (length > 1048576) req.destroy(new Error('response_too_large')); else chunks.push(chunk); });
        res.on('error', () => reject(new HttpError(502, 'proxmox_unreachable')));
        res.on('end', () => {
          if (res.statusCode !== 200) { reject(new HttpError(502, res.statusCode === 401 || res.statusCode === 403 ? 'proxmox_permission_denied' : 'proxmox_request_failed')); return; }
          try { resolve(JSON.parse(Buffer.concat(chunks).toString('utf8')).data); }
          catch { reject(new HttpError(502, 'proxmox_invalid_response')); }
        });
      });
      req.setTimeout(8000, () => req.destroy(new Error('timeout')));
      req.on('error', error => { this.health?.observe(config, error); reject(new HttpError(502, certificateFailure(error) ? 'proxmox_certificate_error' : 'proxmox_unreachable')); });
      req.end(body);
    });
  }
  // Host registrations supply these exact bindings. Missing targets NEVER means
  // discover the cluster. Per-target failures preserve requested identity only.
  async inventory(config, { targets, enrich = true, signal } = {}) {
    if (targets === undefined) throw new HttpError(400, 'proxmox_registered_targets_required');
    if (!Array.isArray(targets) || targets.length > 10000 || typeof enrich !== 'boolean') throw new HttpError(400, 'invalid_proxmox_target');
    const scoped = Array.from(targets, registeredTarget), seen = new Set();
    for (const target of scoped) { if (seen.has(target.vmid)) throw new HttpError(400, 'invalid_proxmox_target'); seen.add(target.vmid); }
    const results = new Array(scoped.length); let next = 0;
    await Promise.all(Array.from({ length: Math.min(4, scoped.length) }, async () => {
      while (next < scoped.length) {
        const index = next++, target = scoped[index];
        try {
          const { value, status, machineUuid } = await this._registeredTarget(config, target, { signal });
          results[index] = { node: target.node, vmid: target.vmid,
            name: typeof value.name === 'string' && value.name.length > 0 && value.name.length <= 256 ? value.name : null,
            machineUuid, ...status,
            tags: normalizeTags(value.tags), pool: normalizePool(value.pool),
            qemuAgent: qemuAgentOptionEnabled(value.agent),
            ...(enrich ? { ...classifyOs(value.ostype), audioConfig: Object.fromEntries(['audio0', 'lock', 'digest'].filter(key => Object.hasOwn(value, key)).map(key => [key, value[key]])) } : {}) };
        } catch (error) {
          const powerError = error instanceof HttpError && targetReadErrors.has(error.code) ? error.code : 'proxmox_unreachable';
          results[index] = { ...target, status: 'unknown', powerError };
        }
      }
    }));
    return results;
  }
  async _registeredTarget(config, rawTarget, { signal, withStatus = true } = {}) {
    const target = registeredTarget(rawTarget);
    signal?.throwIfAborted();
    const value = await this.vmConfig(config, target.node, target.vmid, { current: true, signal });
    if (value.template === 1 || value.template === true) throw new HttpError(404, 'proxmox_vm_not_found');
    const machineUuid = smbiosUuid(value.smbios1);
    if (!validMachineUuid(machineUuid)) throw new HttpError(409, 'proxmox_machine_uuid_missing');
    if (machineUuid !== target.expectedUuid) throw new HttpError(409, 'proxmox_machine_uuid_mismatch');
    let status;
    if (withStatus) status = signal
      ? measuredStatus(await this.call(config, '/nodes/' + encodeURIComponent(target.node) + '/qemu/' + target.vmid + '/status/current', 'GET', '', { signal }))
      : await this.vmStatus(config, target.node, target.vmid);
    signal?.throwIfAborted();
    return { target, value, machineUuid, status };
  }
  async vmConfig(config, node, vmid, { current = false, signal } = {}) {
    if (!validNode(node) || !validVmid(vmid)) throw new HttpError(400, 'invalid_proxmox_vm');
    const path = `/nodes/${encodeURIComponent(node)}/qemu/${vmid}/config${current ? '?current=1' : ''}`;
    const result = signal ? await this.call(config, path, 'GET', '', { signal }) : await this.call(config, path);
    if (!result || typeof result !== 'object' || Array.isArray(result)) throw new HttpError(502, 'proxmox_invalid_response');
    return result;
  }
  async consoleSnapshot(config, vmid, { node, expectedUuid, signal } = {}) {
    const target = registeredTarget({ node, vmid, expectedUuid });
    const path = '/vms/' + vmid;
    const permissions = await this.call(config, '/access/permissions?path=' + encodeURIComponent(path), 'GET', '', { signal });
    const own = permissions?.[path];
    // Zero is a granted non-propagating permission, not denial.
    if (!own || Array.isArray(own) || !Object.hasOwn(own, 'VM.Console') || ![0, 1, false, true].includes(own['VM.Console'])) throw new HttpError(403, 'console_permission_denied');
    const { value, status, machineUuid } = await this._registeredTarget(config, target, { signal });
    if (status.status !== 'running') throw new HttpError(409, 'console_vm_unavailable');
    if (consoleDisplayMissing(value)) throw new HttpError(409, 'console_no_display');
    const model = standardConsoleModel(value);
    // A PCI device passed through (a GPU or vGPU among them keeps its own
    // display head beside the QEMU one): the browser Console names what that
    // can hide (console-relay.mjs signinElsewhere).
    return { node, vmid, machineUuid, ...(model ? { model } : {}), ...(pciPassthrough(value) ? { passthrough: true } : {}) };
  }
  async openConsole(config, snapshot, { signal } = {}) {
    if (signal?.aborted) throw new HttpError(409, 'console_cancelled');
    const value = await this.call(config, `/nodes/${encodeURIComponent(snapshot.node)}/qemu/${snapshot.vmid}/vncproxy`, 'POST', 'websocket=1', { signal });
    if (signal?.aborted) throw new HttpError(409, 'console_cancelled');
    // PVE's JSON response may serialize this integer as four decimal digits.
    // Accept that exact wire form without coercing whitespace, booleans or URLs.
    const port = typeof value?.port === 'string' && value.port.length === 4 && /^[0-9]{4}$/.test(value.port) ? Number(value.port) : value?.port;
    if (!value || !Number.isInteger(port) || port < 5900 || port > 5999 ||
        typeof value.ticket !== 'string' || !/^[\x21-\x7e]{1,512}$/.test(value.ticket) ||
        (value.password !== undefined && (typeof value.password !== 'string' || !/^[\x21-\x7e]{1,256}$/.test(value.password)))) {
      throw new HttpError(502, 'console_invalid_upstream');
    }
    const url = new URL(`/api2/json/nodes/${encodeURIComponent(snapshot.node)}/qemu/${snapshot.vmid}/vncwebsocket`, config.endpoint);
    if (url.protocol !== 'https:') throw new HttpError(502, 'console_certificate_error');
    url.protocol = 'wss:'; url.searchParams.set('port', String(port)); url.searchParams.set('vncticket', value.ticket);
    const socket = this.consoleSocket(url, ['binary'], {
      headers: { Authorization: `PVEAPIToken=${config.tokenId}=${config.secret}` },
      ...proxmoxTls(config), handshakeTimeout: 8000,
      maxPayload: 16 * 1024 * 1024, perMessageDeflate: false, followRedirects: false,
    });
    // Cancellation may terminate a CONNECTING ws before the awaiting manager
    // can attach its own listener. Never let that owned error escape unhandled.
    // A refused certificate here is recorded like an API call's (proxmox-health.mjs).
    socket.on('error', error => this.health?.observe(config, error));
    socket.once?.('upgrade', () => this.health?.reached(config));
    const abort = () => socket.terminate();
    signal?.addEventListener('abort', abort, { once: true });
    socket.once('close', () => signal?.removeEventListener('abort', abort));
    if (signal?.aborted) abort();
    return { socket, password: value.password ?? value.ticket };
  }
  async consoleIdentity(config, vmid, { node, expectedUuid } = {}) {
    const { value, status, machineUuid } = await this._registeredTarget(config, { node, vmid, expectedUuid });
    if (status.status !== 'running') throw new HttpError(409, 'stream_profile_vm_unavailable');
    const model = standardConsoleModel(value);
    // Any other display streams without a console proof; `issue` is the fixed
    // reason the broker records for the administrator (stream-console-profile.mjs).
    if (!model) return { machineUuid, model: null, issue: consoleModelIssue(value) };
    return { machineUuid, model };
  }
  async machineIdentity(config, vmid, { node, expectedUuid } = {}) {
    const { value, machineUuid } = await this._registeredTarget(config, { node, vmid, expectedUuid }, { withStatus: false });
    return { machineUuid, ...classifyOs(value.ostype) };
  }
  // The one read that looks at the whole cluster: which VM carries which
  // SMBIOS UUID, so a Host request from a VM that never had a Host can be
  // shown with its VM instead of asking the administrator for node and VM ID
  // (host-vm-lookup.mjs). Read-only, bounded (maxVms, four reads at a time,
  // the caller's deadline) and never used for a registered Host: those keep
  // their exact targets above. It decides nothing; approval re-reads the one
  // VM through machineIdentity.
  async machineIndex(config, { signal, maxVms = 1000 } = {}) {
    const resources = await this.call(config, '/cluster/resources?type=vm', 'GET', '', { signal });
    if (!Array.isArray(resources)) throw new HttpError(502, 'proxmox_invalid_response');
    const seen = new Set(), vms = [];
    for (const row of resources) {
      if (!row || typeof row !== 'object' || row.type !== 'qemu' || row.template === 1 || row.template === true || !validNode(row.node) || !validVmid(row.vmid) || seen.has(row.vmid)) continue;
      seen.add(row.vmid); vms.push(row);
    }
    const listed = vms.slice(0, maxVms), machines = new Map();
    let unread = vms.length - listed.length, next = 0;
    await Promise.all(Array.from({ length: Math.min(4, listed.length) }, async () => {
      while (next < listed.length) {
        const row = listed[next++];
        try {
          signal?.throwIfAborted();
          const value = await this.vmConfig(config, row.node, row.vmid, { current: true, signal });
          if (value.template === 1 || value.template === true) continue;
          const machineUuid = smbiosUuid(value.smbios1);
          if (!validMachineUuid(machineUuid)) continue;
          const name = [value.name, row.name].find(text => typeof text === 'string' && text.length > 0 && text.length <= 256) ?? null;
          if (!machines.has(machineUuid)) machines.set(machineUuid, []);
          machines.get(machineUuid).push({ node: row.node, vmid: row.vmid, name });
        } catch { unread++; }
      }
    }));
    for (const list of machines.values()) list.sort((a, b) => a.vmid - b.vmid);
    return { machines, total: vms.length, unread };
  }
  // Where are these registered VMs now? Asked only for registered targets
  // whose exact config read failed as if the VM was not there (vm-relocate.mjs:
  // a migration or HA recovery moves it to another node, a backup restore
  // gives it another VM ID). One resource listing, then:
  // - the same VM ID on another node: that one config is read, and it is the
  //   VM only when it carries the recorded SMBIOS UUID (moved); another UUID
  //   there means the number now belongs to another VM (replaced);
  // - the VM ID gone from the cluster: the configs are read (bounded like
  //   machineIndex, templates included) and the UUID must be on exactly one
  //   VM that is no template (moved); several, or a template, is ambiguous.
  // The listing still showing the VM on its node means the failure was
  // something else (present). Nothing here changes anything.
  async locateMachines(config, wanted, { signal, maxVms = 1000 } = {}) {
    if (!Array.isArray(wanted) || wanted.length > 10000) throw new HttpError(400, 'invalid_proxmox_target');
    const scoped = Array.from(wanted, registeredTarget);
    const resources = await this.call(config, '/cluster/resources?type=vm', 'GET', '', { signal });
    if (!Array.isArray(resources)) throw new HttpError(502, 'proxmox_invalid_response');
    // vmid -> listed VM, or null when the listing names one VM ID twice.
    const listed = new Map();
    for (const row of resources) {
      if (!row || typeof row !== 'object' || row.type !== 'qemu' || !validNode(row.node) || !validVmid(row.vmid)) continue;
      listed.set(row.vmid, listed.has(row.vmid) ? null : { node: row.node, vmid: row.vmid, template: row.template === 1 || row.template === true });
    }
    const reads = new Map();
    const read = row => {
      if (!reads.has(row.vmid)) reads.set(row.vmid, (async () => {
        signal?.throwIfAborted();
        const value = await this.vmConfig(config, row.node, row.vmid, { current: true, signal });
        return { uuid: smbiosUuid(value.smbios1), template: row.template || value.template === 1 || value.template === true };
      })());
      return reads.get(row.vmid);
    };
    const results = new Array(scoped.length), gone = [];
    for (const [index, target] of scoped.entries()) {
      const row = listed.get(target.vmid);
      if (row === null) results[index] = { state: 'unavailable' };
      else if (!row) gone.push(index);
      else if (row.node === target.node) results[index] = { state: 'present' };
      else {
        try {
          const found = await read(row);
          results[index] = found.uuid !== target.expectedUuid ? { state: 'replaced' } : found.template ? { state: 'ambiguous' } : { state: 'moved', node: row.node, vmid: row.vmid };
        } catch (error) {
          if (signal?.aborted) throw error;
          results[index] = { state: 'unavailable' };
        }
      }
    }
    if (gone.length) {
      const rows = [...listed.values()].filter(Boolean), scanned = rows.slice(0, maxVms), carriers = new Map();
      let unread = rows.length - scanned.length, next = 0;
      await Promise.all(Array.from({ length: Math.min(4, scanned.length) }, async () => {
        while (next < scanned.length) {
          const row = scanned[next++];
          try {
            const found = await read(row);
            if (!validMachineUuid(found.uuid)) continue;
            if (!carriers.has(found.uuid)) carriers.set(found.uuid, []);
            carriers.get(found.uuid).push({ node: row.node, vmid: row.vmid, template: found.template });
          } catch { unread++; }
        }
      }));
      signal?.throwIfAborted();
      for (const index of gone) {
        const found = carriers.get(scoped[index].expectedUuid) ?? [];
        results[index] = found.length > 1 || found.some(vm => vm.template) ? { state: 'ambiguous', count: found.length }
          : unread ? { state: 'unavailable' }
          : found.length === 1 ? { state: 'moved', node: found[0].node, vmid: found[0].vmid }
          : { state: 'not_found' };
      }
    }
    return results;
  }
  async tokenStatus(config) {
    const [userId, tokenName] = config.tokenId.split('!');
    const token = await this.call(config, `/access/users/${encodeURIComponent(userId)}/token/${encodeURIComponent(tokenName)}`);
    if (!token || typeof token !== 'object' || !Number.isSafeInteger(token.expire) || token.expire < 0) throw new HttpError(502, 'proxmox_invalid_response');
    const tokenExpiresAt = token.expire ? new Date(token.expire * 1000).toISOString() : null;
    // /access/users always includes the authenticated owner, without extra
    // User.Modify rights. Older PVE versions may still refuse token metadata.
    let accountExpiresAt = null, accountExpiryKnown = false;
    try {
      const users = await this.call(config, '/access/users');
      const owner = Array.isArray(users) ? users.find(value => value.userid === userId) : null;
      if (owner && Number.isSafeInteger(owner.expire ?? 0) && (owner.expire ?? 0) >= 0) {
        accountExpiryKnown = true;
        accountExpiresAt = owner.expire ? new Date(owner.expire * 1000).toISOString() : null;
      }
    } catch { /* Token metadata remains useful when user-list access is denied. */ }
    const dates = [tokenExpiresAt, accountExpiresAt].filter(Boolean);
    return { tokenExpiresAt, accountExpiresAt, accountExpiryKnown, expiresAt: dates.length ? dates.sort()[0] : null };
  }
  // One VM, one call (the live status, not the cluster list that pvestatd
  // refreshes about every 10 s): used only while a power change is watched.
  async vmStatus(config, node, vmid) {
    if (!validNode(node) || !validVmid(vmid)) throw new HttpError(400, 'invalid_proxmox_vm');
    return measuredStatus(await this.call(config, '/nodes/' + encodeURIComponent(node) + '/qemu/' + vmid + '/status/current'));
  }
  // Whether the VM's Proxmox config enables the QEMU guest agent
  // ("1" or "enabled=1,..."); the agent is only pinged for such VMs.
  async agentEnabled(config, node, vmid) {
    const value = await this.vmConfig(config, node, vmid, { current: true });
    return qemuAgentOptionEnabled(value.agent);
  }
  // 'up' when the agent answers, 'down' when Proxmox reports it not running,
  // 'denied' when the dedicated identity lacks the guest-agent privilege
  // (VM.Monitor before PVE 9, VM.GuestAgent.Audit after): callers then stop
  // asking instead of repeating a refused call.
  async agentPing(config, node, vmid) {
    if (typeof node !== 'string' || !/^[A-Za-z0-9_.-]+$/.test(node) || !Number.isSafeInteger(vmid) || vmid < 100) throw new HttpError(400, 'invalid_proxmox_vm');
    try { await this.call(config, `/nodes/${encodeURIComponent(node)}/qemu/${vmid}/agent/ping`, 'POST'); return 'up'; }
    catch (error) {
      if (error?.code === 'proxmox_permission_denied') return 'denied';
      if (error?.code === 'proxmox_request_failed') return 'down';
      throw error;
    }
  }  async power(config, vmid, action, { node, expectedUuid, beforeSubmit, overruleShutdown = false } = {}) {
    if (!['start', 'shutdown', 'reboot', 'stop'].includes(action)) throw new HttpError(400, 'invalid_power_action');
    if (!Number.isSafeInteger(vmid) || vmid < 100 || vmid > 999999999 || typeof overruleShutdown !== 'boolean' || (overruleShutdown && action !== 'stop')) throw new HttpError(400, 'invalid_power_action');
    const observed = await this._registeredTarget(config, { node, vmid, expectedUuid });
    const vm = { node, status: observed.status.status };
    if (vm.status !== (action === 'start' ? 'stopped' : 'running')) throw new HttpError(409, 'invalid_power_state');
    // The status read may have waited across an external VM replacement.
    // Re-read exact identity immediately before the caller's final authority
    // check. PVE's power API has no UUID CAS; this is not an atomic identity lock.
    await this._registeredTarget(config, { node, vmid, expectedUuid }, { withStatus: false });
    beforeSubmit?.();
    // This documented option aborts qmshutdown work for this VM. It is used
    // only after the broker verified its exact confirmed shutdown task. Never
    // send skiplock/reset/keepActive or accept arbitrary client parameters.
    const task = await this.call(config, `/nodes/${encodeURIComponent(vm.node)}/qemu/${vmid}/status/${action}`, 'POST', overruleShutdown ? 'overrule-shutdown=1' : '');
    if (typeof task !== 'string' || !task.startsWith('UPID:')) throw new HttpError(502, 'proxmox_invalid_response');
    return { task, node: vm.node, vmid, action, status: 'submitted' };
  }
  async task(config, node, task) {
    if (!/^[A-Za-z0-9_.-]+$/.test(node) || !/^UPID:[A-Za-z0-9_.:@!-]+:$/.test(task) || task.length > 400) throw new HttpError(400, 'invalid_task');
    const result = await this.call(config, `/nodes/${encodeURIComponent(node)}/tasks/${encodeURIComponent(task)}/status`);
    if (!result || typeof result !== 'object' || !['running', 'stopped'].includes(result.status)) throw new HttpError(502, 'proxmox_invalid_response');
    return { status: result.status === 'stopped' ? 'stopped' : 'running', exitStatus: result.status === 'stopped' ? String(result.exitstatus ?? 'unknown').slice(0, 200) : null };
  }
}

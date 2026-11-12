import { randomUUID } from 'node:crypto';
import dgram from 'node:dgram';
import { networkInterfaces } from 'node:os';
import { HttpError } from './errors.mjs';

// Power for approved physical PCs (pc-power-v1).
// 종료 and 다시 시작 go to the PC's Host over its authenticated
// channel, which asks Windows for an ordinary (never forced) shutdown. 켜기
// is a Wake-on-LAN magic packet this broker broadcasts on its own networks,
// so it reaches only a PC in the broker's broadcast domain. No 정지: a PC has
// no hypervisor to cut its power, and a forced Windows shutdown closes apps
// without asking. Authorization is the VM rule: the account's assignment.
export const pcPowerProtocol = 'pc-power-v1';
const hostActions = ['shutdown', 'reboot'];
// The Host's fixed refusal codes (apps/windows_host/src/host_power.hpp).
export const pcPowerRefusals = new Set(['invalid_request', 'shutdown_in_progress', 'privilege_unavailable', 'other_users_signed_in', 'machine_locked', 'shutdown_failed']);
const uuid = /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/;
const iso = value => new Date(value).toISOString();

// A Host's wake report: at most 8 wired adapters, each a unicast MAC and
// Windows' read-only wake settings (true, false or null for unknown).
export function wakeAdapters(value) {
  if (!Array.isArray(value) || value.length > 8) return null;
  const seen = new Set(), adapters = [];
  for (const entry of value) {
    if (!entry || typeof entry !== 'object' || Array.isArray(entry) || Object.keys(entry).some(key => !['mac', 'wakeEnabled', 'magicPacket'].includes(key))) return null;
    const mac = typeof entry.mac === 'string' && /^[0-9A-Fa-f]{2}(?:[:-][0-9A-Fa-f]{2}){5}$/.test(entry.mac) ? entry.mac.toUpperCase().replaceAll('-', ':') : null;
    const flag = key => entry[key] === undefined || entry[key] === null ? null : typeof entry[key] === 'boolean' ? entry[key] : undefined;
    const wakeEnabled = flag('wakeEnabled'), magicPacket = flag('magicPacket');
    // Group (multicast) and all-zero addresses never name one adapter.
    if (!mac || (parseInt(mac.slice(0, 2), 16) & 1) || mac === '00:00:00:00:00:00' || wakeEnabled === undefined || magicPacket === undefined) return null;
    if (!seen.has(mac)) { seen.add(mac); adapters.push({ mac, wakeEnabled, magicPacket }); }
  }
  return adapters;
}

export function magicPacket(mac) {
  const address = Buffer.from(mac.replaceAll(':', ''), 'hex');
  return Buffer.concat([Buffer.alloc(6, 0xff), ...Array.from({ length: 16 }, () => address)]);
}

// The limited broadcast plus each of this broker's own IPv4 networks'
// broadcast address. Never an address a Host or client supplied.
export function broadcastTargets(interfaces = networkInterfaces()) {
  const targets = new Set(['255.255.255.255']);
  for (const list of Object.values(interfaces)) for (const entry of list ?? []) {
    if ((entry.family !== 'IPv4' && entry.family !== 4) || entry.internal || entry.netmask === '255.255.255.255') continue;
    const address = entry.address.split('.').map(Number), mask = entry.netmask.split('.').map(Number);
    if (address.length !== 4 || mask.length !== 4 || [...address, ...mask].some(part => !Number.isInteger(part) || part < 0 || part > 255)) continue;
    targets.add(address.map((part, index) => part | (~mask[index] & 255)).join('.'));
  }
  return [...targets];
}

// UDP port 9 (discard), three rounds. Resolves with the packets handed to the
// network; throws when not one could be sent.
export async function sendWakePackets(macs, { targets = broadcastTargets(), port = 9, rounds = 3 } = {}) {
  const socket = dgram.createSocket('udp4');
  try {
    await new Promise((resolve, reject) => { socket.once('error', reject); socket.bind(0, () => { socket.off('error', reject); resolve(); }); });
    socket.setBroadcast(true);
    let sent = 0;
    for (let round = 0; round < rounds; round++) for (const mac of macs) for (const target of targets) {
      try { await new Promise((resolve, reject) => socket.send(magicPacket(mac), port, target, error => error ? reject(error) : resolve())); sent++; }
      catch { /* Another network may still reach the PC. */ }
    }
    if (!sent) throw new Error('wake_send_failed');
    return sent;
  } finally { socket.close(); }
}

// Operation status in the VM power task vocabulary, so a client follows a PC
// action like a Proxmox task: submitted (sent to the Host / packets going
// out), running (packets sent, waiting for the Host), stopped with exitStatus
// 'OK' or a fixed code, unknown (no answer).
const active = operation => !!operation && ['submitted', 'running'].includes(operation.status);

export class PcPower {
  constructor(relay, { now = Date.now, tickMs = 1000, replyMs = 15000, wakeMs = 240000, rebootMs = 300000, sendWake = sendWakePackets, onChange = () => {} } = {}) {
    Object.assign(this, { relay, now, replyMs, wakeMs, rebootMs, sendWake, onChange });
    this.operations = new Map();
    this.timer = setInterval(() => this.tick(), tickMs); this.timer.unref();
  }
  close() { clearInterval(this.timer); }
  get store() { return this.relay.store; }
  changed() { try { this.onChange(); } catch { /* A watcher cannot fail power bookkeeping. */ } }
  audit(event, actor, detail) { try { this.store.audit(event, actor, detail); } catch { /* Audit I/O cannot fail a power state change. */ } }
  busy(desktopId) { return active(this.operations.get(desktopId)); }
  // Whether this broker's own record says the PC is off or restarting since
  // `since` (hostMissingAfterUpdate): an action under way, a 종료 its Host
  // accepted (or whose answer the closing channel cut off), or a 다시 시작
  // still within rebootMs. A wake that timed out says nothing.
  explainsAbsence(desktopId, since) {
    const operation = this.operations.get(desktopId);
    if (!operation) return false;
    if (active(operation)) return true;
    if (operation.action === 'start' || operation.checkedAt === null || operation.checkedAt < since) return false;
    const accepted = operation.exitStatus === 'OK' || (operation.status === 'unknown' && operation.exitStatus === 'host_disconnected');
    return accepted && (operation.action === 'shutdown' || this.now() - operation.checkedAt < this.rebootMs);
  }
  // The last reported adapters of this PC (server and administrator data).
  adapters(desktopId) {
    try { const row = this.store.db.prepare('SELECT adapters FROM pc_wake WHERE desktop_id=?').get(desktopId); return row ? wakeAdapters(JSON.parse(row.adapters)) ?? [] : []; }
    catch { return []; }
  }
  message(host, value) {
    try {
      if (this.relay.hosts.get(host.identity.desktop_id) !== host) return;
      if (value.type === 'host.power.hello') this.hello(host, value);
      else if (value.type === 'host.power.result') this.result(host, value);
    } catch { /* Optional power reporting cannot break the control channel. */ }
  }
  hello(host, value) {
    if (value.protocol !== pcPowerProtocol || !Array.isArray(value.actions) || value.actions.length > 2 || value.actions.some(action => !hostActions.includes(action))) return;
    // No list: the Host could not read its adapters; the last report stays.
    const adapters = value.wake === undefined ? undefined : wakeAdapters(value.wake);
    if (adapters === null) return;
    host.pcPower = new Set(value.actions);
    const id = host.identity.desktop_id;
    // Only an approved PC keeps wake data; a VM is started through Proxmox.
    if (adapters && this.store.db.prepare("SELECT 1 FROM desktops WHERE id=? AND kind='pc'").get(id)) {
      this.store.db.prepare('INSERT INTO pc_wake(desktop_id,host_id,adapters,reported_at) VALUES(?,?,?,?) ON CONFLICT(desktop_id) DO UPDATE SET host_id=excluded.host_id,adapters=excluded.adapters,reported_at=excluded.reported_at')
        .run(id, host.identity.id, JSON.stringify(adapters), this.now());
    }
    this.changed();
  }
  result(host, value) {
    const operation = this.operations.get(host.identity.desktop_id);
    if (!operation || operation.id !== value.requestId || operation.status !== 'submitted' || operation.action === 'start') return;
    if (value.state === 'accepted' && (value.code ?? '') === '') {
      this.finish(operation, 'stopped', 'OK');
      this.audit('pc_power_accepted', operation.actor, `${operation.desktopId}:${operation.action}`);
    } else if (value.state === 'refused' && pcPowerRefusals.has(value.code)) {
      this.finish(operation, 'stopped', value.code);
      this.audit('pc_power_refused', operation.actor, `${operation.desktopId}:${operation.action}:${value.code}`);
    }
  }
  // Host channel events from the relay: a Host that comes back ends a wake.
  hostEvent(desktopId, kind) {
    const operation = this.operations.get(desktopId);
    if (!operation) return;
    if (kind === 'connected' && operation.action === 'start' && active(operation)) {
      this.finish(operation, 'stopped', 'OK');
      this.audit('pc_wake_online', operation.actor, desktopId);
    } else if (kind === 'disconnected' && operation.action !== 'start' && operation.status === 'submitted') {
      // Windows may have accepted and closed the channel before the answer left.
      this.finish(operation, 'unknown', 'host_disconnected');
    }
  }
  finish(operation, status, exitStatus) {
    operation.status = status; operation.exitStatus = exitStatus; operation.checkedAt = this.now();
    this.changed();
  }
  tick() {
    const now = this.now();
    for (const operation of this.operations.values()) {
      if (operation.action !== 'start' && operation.status === 'submitted' && now - operation.submittedAt > this.replyMs) this.finish(operation, 'unknown', 'host_no_response');
      else if (operation.action === 'start' && active(operation) && now - operation.submittedAt > this.wakeMs) {
        this.finish(operation, 'stopped', 'wake_timeout');
        this.audit('pc_wake_timeout', operation.actor, operation.desktopId);
      }
    }
  }
  taskPublic(operation) {
    return operation ? { id: operation.id, desktopId: operation.desktopId, action: operation.action, status: operation.status, exitStatus: operation.exitStatus,
      submittedAt: iso(operation.submittedAt), checkedAt: operation.checkedAt === null ? null : iso(operation.checkedAt) } : null;
  }
  // What a client may do now. Online: the actions the Host announced (none
  // for a Host before pc-power-v1). Offline: 켜기 when a wake address is known.
  // The address itself never leaves the server and the administrator console.
  listView(desktopId, actor) {
    const online = this.relay.hostConnected(desktopId), host = this.relay.hosts.get(desktopId), operation = this.operations.get(desktopId);
    const wake = this.adapters(desktopId).length > 0;
    const actions = online ? hostActions.filter(action => host.pcPower?.has(action)) : wake ? ['start'] : [];
    return { protocol: pcPowerProtocol, actions, wake, busy: active(operation), ...(operation && operation.actor === actor ? { task: this.taskPublic(operation) } : {}) };
  }
  // The VM power answer's shape (power, powerTask, powerBusy, canForceStop).
  view(desktopId, actor, operation = this.operations.get(desktopId)) {
    const online = this.relay.hostConnected(desktopId);
    return { power: { state: online ? 'running' : 'unknown', checkedAt: iso(this.now()), stale: false, error: null },
      powerTask: operation && operation.actor === actor ? this.taskPublic(operation) : null, powerBusy: this.busy(desktopId), canForceStop: false, pcPower: this.listView(desktopId, actor) };
  }
  async status(authorize, actor, taskId) {
    const desktop = authorize();
    if (taskId === undefined) return this.view(desktop.id, actor);
    const operation = this.operations.get(desktop.id);
    if (!uuid.test(taskId) || !operation || operation.id !== taskId || operation.actor !== actor) throw new HttpError(404, 'power_task_not_found');
    return this.view(desktop.id, actor, operation);
  }
  async submit(authorize, actor, action) {
    if (!['start', ...hostActions].includes(action)) throw new HttpError(400, 'invalid_power_action');
    const desktop = authorize(), id = desktop.id;
    if (desktop.kind !== 'pc') throw new HttpError(400, 'invalid_power_action');
    this.relay.assertPowerAllowed(id);
    if (this.busy(id)) throw new HttpError(409, 'power_in_progress');
    const online = this.relay.hostConnected(id), host = this.relay.hosts.get(id);
    const adapters = action === 'start' ? this.adapters(id) : [];
    if ((action === 'start') === online) throw new HttpError(409, 'invalid_power_state');
    if (action === 'start' && !adapters.length) throw new HttpError(409, 'pc_wake_unavailable');
    if (action !== 'start' && !host.pcPower?.has(action)) throw new HttpError(409, 'pc_power_unsupported');
    if (this.operations.size >= 10000 && !this.operations.has(id)) throw new HttpError(503, 'power_busy');
    const operation = { id: randomUUID(), desktopId: id, actor, action, status: 'submitted', exitStatus: null, submittedAt: this.now(), checkedAt: null };
    this.operations.set(id, operation);
    this.audit('power_submitted', actor, `${id}:${action}`);
    if (action === 'start') {
      try { await this.sendWake(adapters.map(adapter => adapter.mac)); }
      catch {
        this.finish(operation, 'stopped', 'wake_send_failed');
        this.audit('power_failed', actor, `${id}:start`);
        return this.view(id, actor, operation);
      }
      // The Host may already be back (a PC that was only offline).
      if (active(operation)) {
        if (this.relay.hostConnected(id)) this.finish(operation, 'stopped', 'OK');
        else { operation.status = 'running'; operation.checkedAt = this.now(); }
        this.audit('pc_wake_sent', actor, id);
      }
    } else if (!this.relay.send(host.ws, { type: 'host.power.request', requestId: operation.id, action })) {
      this.relay.dropHost(host, 'host_backpressure');
      this.finish(operation, 'unknown', 'host_disconnected');
    }
    this.changed();
    return this.view(id, actor, operation);
  }
}

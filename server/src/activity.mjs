import { WebSocket } from 'ws';

// Local-only activity view for the installation's own update tooling
// (GET /internal/activity). A root-side script that wants to apply a broker
// release without cutting a remote session needs the broker's own idea of
// "active", not a guess from sockets: a reverse proxy, a tunnel or a second
// Host connection changes socket counts but never changes what a session is.
//
// The reply is integers only: no account, desktop, token, address or free
// text. It is served to a loopback peer that sent no forwarding header and
// asked for a loopback Host name; anything else gets the ordinary 404.
export const activityProtocol = 'activity-v1';
const loopbackPeers = new Set(['127.0.0.1', '::1', '::ffff:127.0.0.1']);
const forwarding = ['x-forwarded-for', 'x-forwarded-host', 'x-forwarded-proto', 'x-real-ip', 'forwarded', 'via'];
export function localActivityRequest(req) {
  if (!loopbackPeers.has(req.socket?.remoteAddress)) return false;
  if (forwarding.some(name => req.headers?.[name] !== undefined)) return false;
  return /^(?:127\.0\.0\.1|localhost|\[::1\])(?::\d{1,5})?$/i.test(req.headers?.host ?? '');
}

// A sibling operator-only view. The global activity-v1 shape and guard stay
// unchanged; browsers and ambiguous targets never get the scoped report.
const activityDesktopId = value => typeof value === 'string' && /^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$/.test(value);
export function localDesktopActivityTarget(req, url) {
  if (req.method !== 'GET' || url.pathname !== '/internal/activity/desktop' || !localActivityRequest(req) ||
      Object.keys(req.headers ?? {}).some(key => key.toLowerCase() === 'origin')) return null;
  const fields = [...url.searchParams.entries()];
  return fields.length === 1 && fields[0][0] === 'desktopId' && activityDesktopId(fields[0][1]) ? fields[0][1] : null;
}

const count = (iterable, test) => { let n = 0; for (const item of iterable) if (test(item)) n++; return n; };
const sum = (iterable, pick) => { let n = 0; for (const item of iterable) n += pick(item); return n; };
const open = ws => ws?.readyState === WebSocket.OPEN;

// Everything that is in progress right now, as counts. `sessions` holds
// every pending ticket and attached stream session; the clipboard,
// drive, USB and print channels hang off a session and are listed so a
// report can say what kind of work was running.
export function activitySnapshot({ relay, desktopEvents, store, now = Date.now(), startedAt = now }) {
  const sessions = [...relay.sessions.values()];
  const power = (() => { try { return store.db.prepare('SELECT count(*) AS n FROM power_tasks WHERE active=1').get().n; } catch { return null; } })();
  const hostUpdates = count(relay.hostUpdate?.operations?.values() ?? [], operation => ['dispatched', 'accepted', 'downloading', 'verifying', 'installing'].includes(operation.status));
  return {
    protocol: activityProtocol,
    sessions: {
      total: sessions.length,
      stream: count(sessions, session => session.mode === 'stream'),
      attached: count(sessions, session => !!session.ws),
      tickets: count(sessions, session => !session.ws),
    },
    channels: {
      clipboard: relay.clipboard?.pairs?.size ?? 0,
      clipboardTransfers: relay.clipboard?.activeTransfers ?? 0,
      drive: relay.drive?.pairs?.size ?? 0,
      driveShares: sum(relay.drive?.pairs ?? [], pair => pair.shares?.size ?? 0),
      driveRequests: sum(relay.drive?.pairs ?? [], pair => pair.outstanding?.size ?? 0),
      usb: relay.usb?.pairs?.size ?? 0,
      usbDevices: sum(relay.usb?.pairs ?? [], pair => pair.devices?.size ?? 0),
      print: relay.print?.pairs?.size ?? 0,
      printTransfers: count(relay.print?.pairs ?? [], pair => !!pair.transfer),
    },
    operations: {
      // Keep activity-v1's exact schema: emergency console grants are display
      // ownership activity, not streaming sessions or display-mode changes.
      // Retired grants remain here until actual open/query/transport completion.
      display: (relay.displayBusy?.size ?? 0) + (relay.console?.grants?.size ?? 0) + (relay.vmConfigOperations?.size ?? 0),
      sessionCleanup: relay.sessionStop?.pending?.size ?? 0,
      hostUpdates,
      power,
    },
    hosts: { connected: count(relay.hosts.values(), host => open(host.ws)) },
    // Client live (events) channels: open whenever a signed-in client runs,
    // including hidden in the tray. They are not a remote session.
    live: { channels: count(desktopEvents?.clients?.values() ?? [], entry => open(entry.ws)) },
    // Time since a session was created or ended, or a display operation began.
    // A broker that has just started reports its own uptime.
    sinceActivityMs: Math.max(0, now - Math.max(relay.activityAt ?? 0, startedAt)),
    uptimeMs: Math.max(0, now - startedAt),
  };
}

// Who a change of the server access settings cuts off: every remote session
// (attached or still a ticket) and every open Console. Counts
// only. `consoles` is the same number activity-v1 folds into
// operations.display, so a reader can tell a Console from a display change.
export function connectionCounts(relay) {
  const sessions = [...relay.sessions.values()], consoles = [...(relay.console?.grants?.values() ?? [])];
  const people = new Set([...sessions, ...consoles.filter(grant => !grant.closed)].map(row => row.userId).filter(id => typeof id === 'string' && id));
  return { people: people.size, sessions: sessions.length, consoles: consoles.length };
}
// Ends them the way a stopping server does (relay.close): each client is told
// server_stopping and waits for the server to come back. Side channels close
// with their session. Returns what was ended.
export function endConnections(relay) {
  const sessions = [...relay.sessions.values()], consoles = [...(relay.console?.grants?.values() ?? [])].filter(grant => !grant.closed);
  const people = new Set([...sessions, ...consoles].map(row => row.userId).filter(id => typeof id === 'string' && id)).size;
  for (const session of sessions) relay.stop(session, 'server_stopping');
  for (const grant of consoles) relay.console.retire(grant, 'server_stopping');
  return { people, sessions: sessions.length, consoles: consoles.length };
}

// A current count snapshot, not a reservation or proof that Windows is idle.
// Unknown ownership/components produce null counters, never invented zeros.
export function desktopActivitySnapshot({ relay, vmAudio, store, now = Date.now() }, desktopId) {
  if (!activityDesktopId(desktopId)) return null;
  try { if (!store.db.prepare('SELECT 1 FROM desktops WHERE id=?').get(desktopId)) return null; }
  catch { return null; }
  let complete = true;
  const measure = fn => { try { return fn(); } catch { complete = false; return null; } };
  const integer = value => { if (!Number.isSafeInteger(value) || value < 0 || value > 1000000) throw Error('invalid_activity_counter'); return value; };
  const boolean = value => { if (typeof value !== 'boolean') throw Error('invalid_activity_boolean'); return value; };
  const scoped = (values, owner) => {
    const rows = [...values];
    if (rows.some(value => !activityDesktopId(owner(value)))) throw Error('unknown_activity_owner');
    return rows.filter(value => owner(value) === desktopId);
  };
  const map = value => { if (!(value instanceof Map) || [...value.keys()].some(key => !activityDesktopId(key))) throw Error('unknown_activity_map'); return value; };
  const pairs = component => {
    if (!(component?.pairs instanceof Set)) throw Error('unknown_activity_pairs');
    return scoped(component.pairs, pair => pair?.session?.desktopId);
  };
  const sessions = measure(() => {
    if (!(relay.sessions instanceof Map)) throw Error('unknown_activity_sessions');
    return scoped(relay.sessions.values(), session => session?.desktopId);
  });
  const clip = measure(() => pairs(relay.clipboard)), drive = measure(() => pairs(relay.drive));
  const usb = measure(() => pairs(relay.usb)), print = measure(() => pairs(relay.print));
  const size = value => { if (!(value instanceof Map)) throw Error('unknown_activity_collection'); return integer(value.size); };
  const transfers = pair => {
    if (!Object.hasOwn(pair, 'transfer') || (pair.transfer !== null && typeof pair.transfer !== 'object') || !Array.isArray(pair.pulls)) throw Error('unknown_clipboard_transfer');
    return (pair.transfer ? 1 : 0) + pair.pulls.length;
  };
  const result = {
    protocol: 'activity-desktop-v1', observedAtMs: measure(() => { if (!Number.isSafeInteger(now) || now < 0) throw Error('invalid_activity_time'); return now; }),
    host: { connected: measure(() => { map(relay.hosts); if (typeof relay.hostAuthenticated !== 'function') throw Error('unknown_activity_host'); return boolean(relay.hostAuthenticated(relay.hosts.get(desktopId))); }) },
    sessions: {
      total: sessions?.length ?? null, stream: sessions ? count(sessions, session => session.mode === 'stream') : null,
      attached: sessions ? count(sessions, session => !!session.ws) : null, tickets: sessions ? count(sessions, session => !session.ws) : null,
    },
    channels: {
      clipboard: clip?.length ?? null,
      clipboardTransfers: measure(() => {
        if (!clip) throw Error('unknown_clipboard_pairs');
        const all = [...relay.clipboard.pairs];
        if (integer(relay.clipboard.activeTransfers) !== sum(all, transfers)) throw Error('unattributed_clipboard_transfer');
        return integer(sum(clip, transfers));
      }),
      drive: drive?.length ?? null, driveShares: measure(() => { if (!drive) throw Error('unknown_drive_pairs'); return integer(sum(drive, pair => size(pair.shares))); }),
      driveRequests: measure(() => { if (!drive) throw Error('unknown_drive_pairs'); return integer(sum(drive, pair => size(pair.outstanding))); }),
      usb: usb?.length ?? null, usbDevices: measure(() => { if (!usb) throw Error('unknown_usb_pairs'); return integer(sum(usb, pair => size(pair.devices))); }),
      print: print?.length ?? null, printTransfers: measure(() => {
        if (!print || print.some(pair => !Object.hasOwn(pair, 'transfer') || (pair.transfer !== null && typeof pair.transfer !== 'object'))) throw Error('unknown_print_transfer');
        return count(print, pair => !!pair.transfer);
      }),
    },
    operations: {
      display: measure(() => Number(map(relay.displayBusy).has(desktopId))),
      console: measure(() => { if (!(relay.console?.grants instanceof Map)) throw Error('unknown_console_grants'); return scoped(relay.console.grants.values(), grant => grant?.desktopId).length; }),
      vmConfig: measure(() => Number(map(vmAudio?.operations).has(desktopId))),
      vmConfigUncertain: measure(() => { if (!(vmAudio?.uncertain instanceof Set) || [...vmAudio.uncertain].some(id => !activityDesktopId(id))) throw Error('unknown_vm_configuration'); return Number(vmAudio.uncertain.has(desktopId)); }),
      sessionCleanup: measure(() => Number(map(relay.sessionStop?.pending).has(desktopId))),
      hostUpdates: measure(() => {
        const operation = map(relay.hostUpdate?.operations).get(desktopId);
        if (!operation) return 0;
        if (!['waiting', 'dispatched', 'accepted', 'downloading', 'verifying', 'installing', 'unknown', 'completed', 'failed', 'deferred', 'rejected', 'expired'].includes(operation.status)) throw Error('unknown_host_update_state');
        return Number(['waiting', 'dispatched', 'accepted', 'downloading', 'verifying', 'installing', 'unknown'].includes(operation.status));
      }),
      power: measure(() => {
        const persisted = integer(store.db.prepare('SELECT count(*) AS n FROM power_tasks WHERE active=1 AND desktop_id=?').get(desktopId).n);
        if (typeof relay.pcPower?.busy !== 'function') throw Error('unknown_pc_power');
        return Math.max(persisted, Number(boolean(relay.pcPower.busy(desktopId))));
      }),
      serverAccess: measure(() => { if (typeof relay.accessPending !== 'function') throw Error('unknown_server_access'); return Number(boolean(relay.accessPending())); }),
    },
  };
  return { ...result, complete };
}

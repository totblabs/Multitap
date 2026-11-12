// 홈 자원: what the server and the running desktops use (GET /resources,
// server/src/admin-resources.mjs). Two small tables, the server (the machine
// it runs on) and the Multitap container or service (resources.css), then a
// board of the running desktops (memory first, at most ten), and the sums per
// Proxmox node. Node capacity is not known (the token reads VMs only), so it
// is never shown. An unknown desktop number is "—", never 0.
import { el, svg, panel, loadingState, emptyState, board, fixed, integer } from './dom.js';
import { t } from './i18n.js';

export const resourceRows = 10;
const warnAt = 90;
const GB = 1024 ** 3, MB = 1024 ** 2;
const known = value => typeof value === 'number' && Number.isFinite(value);
export const gb = bytes => known(bytes) ? fixed(bytes / GB, 1) : '—';
export const cpuText = value => known(value) ? `${integer(value)}%` : '—';
// "3일 4시간", "5시간 12분", "12분", "1분 미만".
export function upFor(seconds) {
  if (!known(seconds) || seconds < 0) return '—';
  const minutes = Math.floor(seconds / 60), hours = Math.floor(minutes / 60), days = Math.floor(hours / 24);
  if (days) return t('resources.1', { days, value: hours % 24 });
  if (hours) return t('resources.2', { hours, value: minutes % 60 });
  return minutes ? t('common.101', { minutes }) : t('common.145');
}
const share = (used, total) => known(used) && known(total) && total > 0 ? Math.min(100, Math.max(0, used / total * 100)) : null;
// A thin bar (SVG: the page's policy has no inline styles); warn above 90 %.
function bar(fraction, warn) {
  if (fraction === null) return null;
  return svg('svg', { class: 'res-bar', viewBox: '0 0 100 4', preserveAspectRatio: 'none', 'aria-hidden': 'true', focusable: 'false' },
    svg('rect', { class: 'res-track', x: 0, y: 0, width: 100, height: 4 }), svg('rect', { class: 'res-fill' + (warn ? ' is-warn' : ''), x: 0, y: 0, width: fraction.toFixed(1), height: 4 }));
}
// "6.1 / 8.0 GB" over its bar.
function meter(used, total, { warn = null, unit = 'GB', format = gb } = {}) {
  const fraction = share(used, total), hot = warn ?? (fraction !== null && fraction >= warnAt);
  const text = !known(used) ? '—' : [format(used), known(total) ? '/ ' + format(total) : null, unit].filter(Boolean).join(' ');
  return el('span', { class: 'res-meter' + (hot ? ' is-hot' : '') }, el('span', { class: 'res-value' + (hot ? ' is-warn' : '') }, text), bar(fraction, hot));
}
const cpuCell = value => el('span', { class: known(value) && value >= warnAt ? 'is-warn' : null }, cpuText(value));

// Memory used first, then CPU; unknown numbers last, then by name.
export function sortRows(rows, nameOf) {
  const key = value => known(value) ? value : -1;
  return [...rows].sort((a, b) => key(b.memoryUsedBytes) - key(a.memoryUsedBytes) || key(b.cpuPercent) - key(a.cpuPercent) || String(nameOf(a)).localeCompare(String(nameOf(b)), 'ko', { numeric: true }));
}

// ---- 서버 and Multitap 컨테이너 / 서비스: two small tables of
// 항목 | 값, numbers as text with their units. A share has one thin bar in a
// neutral colour, amber above 80 % and red above 95 %. A number the server
// could not read says so in words ("확인할 수 없어요"), never 0 or a dash.
export const highAt = 80, fullAt = 95;
export const level = value => !known(value) ? '' : value > fullAt ? 'is-full' : value > highAt ? 'is-high' : '';
const TB = 1024 ** 4, KB = 1024;
// 0 KB, 512 KB, 212 MB, 3.1 GB, 1.2 TB.
export function amount(value) {
  if (!known(value) || value < 0) return null;
  if (value < MB) return `${integer(value / KB)} KB`;
  if (value < GB) return `${integer(value / MB)} MB`;
  return value < TB ? `${fixed(value / GB, 1)} GB` : `${fixed(value / TB, 1)} TB`;
}
// "3.1 / 7.8 GB" when both are gigabytes, else each with its unit.
export function pair(used, total) {
  if (!known(used) || !known(total)) return null;
  if (used >= GB && total >= GB && total < TB) return `${fixed(used / GB, 1)} / ${fixed(total / GB, 1)} GB`;
  return `${amount(used)} / ${amount(total)}`;
}
const unknown = () => el('span', { class: 'res-unknown' }, t('resources.30'));
const hint = text => text ? el('small', { class: 'res-hint' }, text) : null;
function gauge(value) {
  if (!known(value)) return null;
  const width = Math.min(100, Math.max(0, value));
  return svg('svg', { class: 'res-gauge', viewBox: '0 0 100 4', preserveAspectRatio: 'none', 'aria-hidden': 'true', focusable: 'false' },
    svg('rect', { class: 'res-track', x: 0, y: 0, width: 100, height: 4 }), svg('rect', { class: 'res-level ' + (level(value) || 'is-calm'), x: 0, y: 0, width: width.toFixed(1), height: 4 }));
}
// A value: its text (or 확인할 수 없어요), then its bar and a short note.
function fact(text, { share: value = null, note = null } = {}) {
  if (text === null || text === undefined) return el('div', { class: 'res-fact' }, unknown());
  const tone = level(value);
  return el('div', { class: 'res-fact' }, el('span', { class: 'res-text' + (tone ? ' ' + tone : '') }, text), gauge(value), hint(note));
}
const percentText = value => known(value) ? `${fixed(value, value < 10 ? 1 : 0)}%` : null;
const shareNote = value => known(value) ? `${integer(value)}%` : null;
function factTable(title, rows, { className = '', note = null } = {}) {
  return el('table', { class: 'res-facts' + (className ? ' ' + className : '') },
    el('colgroup', {}, el('col', { class: 'res-key' }), el('col', {})),
    el('thead', {}, el('tr', {}, el('th', { scope: 'colgroup', colspan: '2', title: note }, title))),
    el('tbody', {}, rows.filter(Boolean).map(([key, label, value, title = null]) => el('tr', { 'data-fact': key, title }, el('th', { scope: 'row' }, label), el('td', {}, value)))));
}
const network = value => value && known(value.receivedBytes) && known(value.sentBytes) ? t('resources.24', { received: amount(value.receivedBytes), sent: amount(value.sentBytes) }) : null;

// The machine (CT, VM or Docker host). On Docker its name and network are the
// container's own, so they are under the container.
export function machineTable(server) {
  const machine = server?.machine ?? {}, docker = server?.deploymentKind === 'docker', disk = machine.disk ?? server?.disk ?? null;
  const memoryShare = share(machine.memoryUsedBytes, machine.memoryTotalBytes);
  const diskUsed = known(disk?.usedBytes) ? disk.usedBytes : known(disk?.totalBytes) && known(disk?.freeBytes) ? disk.totalBytes - disk.freeBytes : null;
  const diskShare = share(diskUsed, disk?.totalBytes);
  return factTable(t('common.146'), [
    docker ? null : ['name', t('resources.21'), fact(machine.name ?? null)],
    ['kernel', t('resources.22'), fact(machine.kernel ?? null)],
    // A Docker install inside a Proxmox CT cannot read its own machine's
    // CPU, memory and uptime (the container sees the node's): no row then.
    docker && !known(machine.uptimeSeconds) ? null : ['uptime', t('resources.10'), fact(known(machine.uptimeSeconds) ? upFor(machine.uptimeSeconds) : null)],
    docker && !known(machine.cpuPercent) ? null : ['cpu', 'CPU', fact(percentText(machine.cpuPercent), { share: machine.cpuPercent, note: known(machine.cpuCount) ? t('resources.35', { cores: machine.cpuCount }) : null })],
    docker && !known(machine.memoryTotalBytes) ? null : ['memory', t('resources.4'), fact(pair(machine.memoryUsedBytes, machine.memoryTotalBytes), { share: memoryShare, note: shareNote(memoryShare) })],
    ['disk', t('resources.7'), fact(pair(diskUsed, disk?.totalBytes), { share: diskShare, note: shareNote(diskShare) }), t('resources.9')],
    docker ? null : ['network', t('resources.23'), fact(network(machine.network)), t('resources.36')],
  ], { className: 'res-machine', note: docker ? t('resources.42') : t('resources.41') });
}

// The Multitap container (Docker) or service (systemd and the rest).
export function serviceTable(server) {
  const service = server?.service ?? {}, docker = server?.deploymentKind === 'docker' || service.kind === 'container';
  const cgroup = service.measuredBy === 'cgroup';
  const cpuNote = service.cpuBasis === 'quota' && known(service.cpuLimitCores) ? t('resources.33', { cores: fixed(service.cpuLimitCores, Number.isInteger(service.cpuLimitCores) ? 0 : 1) })
    : service.cpuBasis === 'cores' && known(server?.machine?.cpuCount) ? t('resources.32', { cores: server.machine.cpuCount })
    : service.cpuBasis === 'process' || !cgroup ? t('resources.34') : null;
  const limited = known(service.memoryLimitBytes), memoryShare = limited ? share(service.memoryUsedBytes, service.memoryLimitBytes) : null;
  const memory = !known(service.memoryUsedBytes) ? null : limited ? pair(service.memoryUsedBytes, service.memoryLimitBytes) : amount(service.memoryUsedBytes);
  const memoryNote = limited ? shareNote(memoryShare) : service.memoryUnlimited === true ? t('resources.31') : null;
  const memoryTitle = !cgroup ? t('resources.6') : docker ? t('resources.39') : t('resources.40');
  const io = service.blockIo && known(service.blockIo.readBytes) && known(service.blockIo.writtenBytes) ? t('resources.28', { read: amount(service.blockIo.readBytes), written: amount(service.blockIo.writtenBytes) }) : null;
  return factTable(docker ? t('resources.19') : t('resources.20'), [
    docker ? ['name', t('resources.21'), fact(service.name ?? null)] : null,
    ['state', t('resources.25'), fact(service.state === 'running' ? t('resources.26') : null)],
    ['cpu', 'CPU', fact(percentText(service.cpuPercent), { share: service.cpuPercent, note: cpuNote })],
    ['memory', t('resources.4'), fact(memory, { share: memoryShare, note: memoryNote }), memoryTitle],
    docker ? ['network', t('resources.23'), fact(network(service.network)), t('resources.37')] : null,
    ['io', t('resources.27'), fact(io)],
    ['data', t('resources.29'), fact(amount(service.dataBytes)), t('resources.38')],
    ['uptime', t('resources.10'), fact(known(service.uptimeSeconds) ? upFor(service.uptimeSeconds) : null)],
  ], { className: 'res-service' });
}
const serverPart = server => el('div', { class: 'res-sections' }, machineTable(server), serviceTable(server));

export function resourcePanel(ctx) {
  const entry = ctx.resource('resources'), value = ctx.value('resources');
  const all = [ctx.link(t('common.142'), 'desktops', null, { class: 'button quiet small' })];
  if (!value) return panel({ title: t('resources.11'), className: 'resource-panel' },
    entry?.error ? emptyState(t('resources.12'), entry.error, { iconName: 'alert', compact: true, tone: 'is-error' }) : loadingState());
  const proxmox = value.proxmox ?? {}, nameOf = row => ctx.desktopById(row.id)?.name ?? row.id;
  const rows = sortRows((value.desktops ?? []).filter(row => ctx.desktopById(row.id)), nameOf);
  const parts = [serverPart(value.server)];
  if (!rows.length) parts.push(el('p', { class: 'res-empty' }, proxmox.configured ? t('resources.13') : t('resources.14')));
  else {
    parts.push(board([{ label: t('common.5'), cls: 'col-name is-name' }, { label: 'CPU', cls: 'col-cpu' }, { label: t('resources.4'), cls: 'col-memory is-state' }, { label: t('resources.10'), cls: 'col-up' }],
      rows.slice(0, resourceRows).map(row => {
        const open = () => ctx.navigate('desktop', row.id);
        return { attrs: { class: 'is-pick', 'data-desktop': row.id, onclick: event => { if (!event.target?.closest?.('a,button')) open(); } },
          cells: [ctx.link(el('strong', {}, nameOf(row)), 'desktop', row.id), cpuCell(row.cpuPercent), meter(row.memoryUsedBytes, row.memoryTotalBytes, { warn: (share(row.memoryUsedBytes, row.memoryTotalBytes) ?? 0) >= warnAt }), upFor(row.uptimeSeconds)] };
      }), { className: 'res-table' }));
    if (rows.length > resourceRows) parts.push(el('p', { class: 'res-more' }, t('resources.15', { rowsCount: rows.length, resourceRows })));
  }
  // Per node only when it says something the rows above do not: two or more
  // running desktops (with one desktop it would repeat that row).
  const nodes = (value.nodes ?? []).filter(node => node.running > 0);
  const running = nodes.reduce((sum, node) => sum + node.running, 0);
  // CPU as the share of the vCPUs set on its running desktops, like a desktop's own CPU
  // ("0.0 / 4 vCPU" beside a desktop at 1% would read as a contradiction).
  const nodeCpu = node => known(node.vcpusUsed) && known(node.vcpusAssigned) && node.vcpusAssigned > 0
    ? el('span', {}, cpuCell(node.vcpusUsed / node.vcpusAssigned * 100), el('small', { class: 'res-sub' }, ` (${integer(node.vcpusAssigned)} vCPU)`)) : '—';
  if (nodes.length && running >= 2) parts.push(el('div', { class: 'panel-head' }, el('div', { class: 'panel-title' }, el('h3', {}, t('resources.16')))),
    board([{ label: t('common.63'), cls: 'col-node is-name' }, { label: t('resources.17'), cls: 'col-count' }, { label: 'CPU', cls: 'col-vcpu' }, { label: t('resources.4'), cls: 'col-memory is-state' }],
      nodes.map(node => ({ cells: [node.node, t('resources.18', { running: node.running }), nodeCpu(node), meter(node.memoryUsedBytes, node.memoryTotalBytes, { warn: false })] })), { className: 'res-nodes' }));
  return panel({ title: t('resources.11'), className: 'resource-panel', actions: rows.length ? all : [] }, parts);
}

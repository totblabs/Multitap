import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Proxmox, classifyOs, smbiosUuid } from '../src/proxmox.mjs';
import { Store } from '../src/store.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

test('Configured OS types identify a family without asserting a Windows client/server version', () => {
  for (const type of ['wxp', 'w2k', 'w2k3', 'w2k8', 'wvista', 'win7', 'win8', 'win10', 'win11']) assert.equal(classifyOs(type).osFamily, 'windows');
  for (const type of ['l24', 'l26']) assert.equal(classifyOs(type).osFamily, 'linux');
  assert.equal(classifyOs('solaris').osFamily, 'other');
  assert.equal(classifyOs('unrecognized').osFamily, 'unknown');
  assert.equal(classifyOs(undefined).osFamily, 'unknown');
  assert.equal(classifyOs('win11').pveOsType, 'win11');
  assert.equal(Object.hasOwn(classifyOs('win11'), 'version'), false);
  assert.equal(smbiosUuid('base64=1,uuid=12345678-1234-ABCD-1234-123456789ABC,serial=xx'), '12345678-1234-abcd-1234-123456789abc');
  assert.equal(smbiosUuid('serial=not-a-uuid'), null);
});

test('Registered target inventory uses at most four concurrent reads and keeps unreadable registered VMs without discovery', async () => {
  const adapter = new Proxmox(); let active = 0, maximum = 0;
  const uuid = vmid => '12345678-1234-4234-8234-' + String(vmid).padStart(12, '0');
  const targets = Array.from({ length: 9 }, (_, i) => ({ node: 'pve-a', vmid: 170 + i, expectedUuid: uuid(170 + i) }));
  const calls = [];
  adapter.call = async (_config, path, method = 'GET') => {
    assert.equal(method, 'GET');
    calls.push(path);
    assert.match(path, /^\/nodes\/pve-a\/qemu\/17[0-8]\/(?:config\?current=1|status\/current)$/);
    active++; maximum = Math.max(active, maximum);
    await new Promise(resolve => setTimeout(resolve, 5)); active--;
    if (path.includes('/171/')) throw new Error('Fixture config unavailable');
    if (path.endsWith('/status/current')) return { status: 'running' };
    const vmid = Number(path.split('/')[4]);
    return { name: `VM ${vmid}`, smbios1: 'uuid=' + uuid(vmid), ostype: vmid === 170 ? 'win11' : 'l26' };
  };
  const vms = await adapter.inventory({}, { targets });
  assert.equal(vms.length, 9); assert.equal(maximum, 4);
  assert.equal(vms.find(vm => vm.vmid === 170).osFamily, 'windows');
  assert.deepEqual(vms.find(vm => vm.vmid === 171), { ...targets[1], status: 'unknown', powerError: 'proxmox_unreachable' });
  assert.equal(calls.some(path => path.startsWith('/cluster/')), false);
  assert.equal(calls.some(path => path.includes('/200/')), false);
  const store = new Store(':memory:');
  try {
    store.syncProxmox(targets.map(target => ({ node: target.node, vmid: target.vmid, name: 'Existing VM ' + target.vmid, status: 'running', machineUuid: target.expectedUuid })), 'fixture');
    enrollFixtureHosts(store, targets.map(target => 'pve-' + target.vmid));
    store.refreshRegisteredVms(store.registeredVmTargets(), vms, 'fixture');
    store.updateDesktop('pve-170', { os: '관리자가 지정한 표시' }, 'fixture');
    store.refreshRegisteredVms(store.registeredVmTargets(), vms, 'fixture');
    const row = store.inventory().desktops.find(vm => vm.pve_vmid === 170);
    assert.equal(row.os, '관리자가 지정한 표시');
    assert.equal(row.pve_os_family, 'windows'); assert.equal(row.pve_os_source, 'proxmox-config');
    assert.equal(store.inventory().desktops.find(vm => vm.pve_vmid === 171).pve_available, 1);
    assert.equal(store.inventory().desktops.find(vm => vm.pve_vmid === 171).pve_power_status, 'unknown');
    assert.equal(store.inventory().desktops.length, 9);
  } finally { store.close(); }
});

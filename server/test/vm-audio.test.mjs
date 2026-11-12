import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { randomBytes } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { VmAudioControl, vmAudioProtocol } from '../src/vm-audio.mjs';
import { Proxmox } from '../src/proxmox.mjs';

// Multitap does not add a VM sound card: the
// administrator adds an audio device in Proxmox. The control only observes
// each VM's state; what released clients still ask is refused the same clean
// way for everyone, and nothing here can reach Proxmox with a change.
function fixture() {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), admin = store.addUser({ name: 'audio-admin', role: 'admin', passwordHash: 'fixture-no-login' });
  const user = store.addUser({ name: 'audio-user', role: 'user', passwordHash: 'fixture-no-login' });
  store.syncProxmox([{ vmid: 149, node: 'fixture', name: 'VM', status: 'running' }], admin);
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://fixture.invalid', tokenId: 'fixture@pve!audio', secret: 'fixture-not-real' }), admin);
  store.replaceVmAssignments('audio-user', ['pve-149'], admin);
  const calls = [];
  // Any use of the Proxmox adapter by a refusal would be recorded here.
  const proxmox = new Proxy({}, { get: (_target, name) => (...args) => { calls.push([String(name), ...args.slice(1)]); throw new Error('Proxmox must not be asked'); } });
  const control = new VmAudioControl({ store, vault, proxmox, relay: { sessions: new Map() } });
  const desktop = store.db.prepare("SELECT * FROM desktops WHERE id='pve-149'").get();
  return { store, control, calls, user, desktop };
}
const refused = code => error => error?.code === code && error.status === (code === 'vm_audio_vm_only' ? 409 : code === 'vm_audio_permission_denied' ? 403 : error.status);

test('a status or add request of a released client is refused as "this account cannot add a sound card", without asking Proxmox', async () => {
  const f = fixture(), authorize = () => ({ user: { id: f.user }, desktop: f.desktop });
  await assert.rejects(f.control.status(authorize), refused('vm_audio_permission_denied'));
  const input = { protocol: vmAudioProtocol, action: 'add', requestId: '12345678-1234-4234-8234-123456789abc', revision: 'a'.repeat(64), confirm: true };
  await assert.rejects(f.control.add(authorize, input), refused('vm_audio_permission_denied'));
  await assert.rejects(f.control.add(authorize, { ...input, confirm: false }), refused('vm_audio_permission_denied'), 'no input makes it do more');
  assert.deepEqual(f.calls, [], 'nothing was read from or sent to Proxmox');
  assert.equal(f.control.operations.size, 0); assert.equal(f.control.uncertain.size, 0);
  assert.equal(f.store.db.prepare("SELECT count(*) n FROM audit_events WHERE event LIKE 'vm_audio%'").get().n, 0, 'nothing was added, so nothing is recorded as added');
  f.store.close();
});

test('who may not see the desktop, and a desktop that is not a VM, get the answers they got before', async () => {
  const f = fixture();
  const gone = () => { throw Object.assign(new Error('desktop_not_found'), { code: 'desktop_not_found', status: 404 }); };
  await assert.rejects(f.control.status(gone), error => error.code === 'desktop_not_found');
  await assert.rejects(f.control.add(gone, {}), error => error.code === 'desktop_not_found');
  for (const desktop of [{ ...f.desktop, kind: 'pc', pve_vmid: null }, { ...f.desktop, pve_vmid: 99 }, { ...f.desktop, pve_vmid: '149' }]) {
    await assert.rejects(f.control.status(() => ({ user: { id: f.user }, desktop })), refused('vm_audio_vm_only'));
    await assert.rejects(f.control.add(() => ({ user: { id: f.user }, desktop }), {}), refused('vm_audio_vm_only'));
  }
  assert.deepEqual(f.calls, []);
  f.store.close();
});

test('no code path of the broker can change VM hardware or asks for the hardware privilege', async () => {
  // The control and the Proxmox adapter have no audio write, and neither names VM.Config.HWType as something to use.
  for (const name of ['snapshot', 'view', 'idle', 'current']) assert.equal(name in VmAudioControl.prototype, false, name);
  for (const name of ['audioSnapshot', 'addAudio']) assert.equal(name in Proxmox.prototype, false, name);
  const adapter = await readFile(new URL('../src/proxmox.mjs', import.meta.url), 'utf8');
  assert.doesNotMatch(adapter, /audio0=|audio0:|HWType/);
  assert.doesNotMatch(adapter, /qemu\/\$\{[^}]+\}\/config`, 'PUT'/, 'no VM config is ever written');
  for (const file of ['admin-http.mjs', 'http.mjs', 'vm-audio.mjs']) {
    const source = await readFile(new URL('../src/' + file, import.meta.url), 'utf8');
    assert.doesNotMatch(source.replace(/^\s*\/\/.*$/gm, ''), /HWType/, file + ' uses no hardware privilege');
  }
});

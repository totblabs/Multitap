import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:https';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname, resolve, basename } from 'node:path';
import { spawnSync } from 'node:child_process';
import { Proxmox } from '../src/proxmox.mjs';

test('Proxmox adapter uses actual verified HTTPS and the API token header against an isolated TLS fixture', async t => {
  const available = spawnSync('openssl', ['version'], { encoding: 'utf8' });
  if (available.error?.code === 'ENOENT') { t.skip('OpenSSL unavailable; run this transport test on the Linux deployment QA stage.'); return; }
  assert.equal(available.status, 0);
  const directory = await mkdtemp(join(tmpdir(), 'multitap-transport-'));
  t.after(async () => { assert.equal(dirname(resolve(directory)), resolve(tmpdir())); assert.ok(basename(directory).startsWith('multitap-transport-')); await rm(directory, { recursive: true }); });
  const certificate = spawnSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', join(directory, 'key.pem'), '-out', join(directory, 'cert.pem'), '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], { encoding: 'utf8' });
  assert.equal(certificate.status, 0, 'Could not generate an ephemeral TLS fixture.');
  const cert = await readFile(join(directory, 'cert.pem')); const key = await readFile(join(directory, 'key.pem'));
  const requests = [];
  const task = 'UPID:pve-a:0001:0002:0003:qmstart:710:test@pve:';
  let powerState = 'stopped';
  const fixture = createServer({ cert, key }, async (req, res) => {
    let body = ''; for await (const chunk of req) body += chunk.toString('utf8');
    requests.push({ url: req.url, method: req.method, auth: req.headers.authorization, body, contentLength: req.headers['content-length'] });
    let data;
    if (req.url === '/api2/json/nodes/pve-a/qemu/710/config?current=1' && req.method === 'GET') data = { name:'Fixture VM',ostype: 'win11', smbios1: 'uuid=12345678-1234-1234-1234-123456789abc' };
    else if(req.url === '/api2/json/nodes/pve-a/qemu/710/status/current') data={status:powerState,cpus:2,maxmem:4294967296};
    else if(req.url === '/api2/json/nodes/pve-a/qemu/712/config?current=1') data={template:1};
    else if (req.url === '/api2/json/access/users/test%40pve/token/fixture' && req.method === 'GET') data = { expire: 2000000000 };
    else if (req.url === '/api2/json/access/users' && req.method === 'GET') data = [{ userid: 'test@pve', expire: 1900000000 }];
    else if (req.url === '/api2/json/nodes/pve-a/qemu/710/status/start' && req.method === 'POST') data = task;
    else if (req.url === '/api2/json/nodes/pve-a/qemu/710/status/stop' && req.method === 'POST') data = 'UPID:pve-a:0001:0002:0003:qmstop:710:test@pve:';
    else if (req.url.endsWith('/status') && req.url.includes('/tasks/')) data = { status: 'stopped', exitstatus: 'OK' };
    else { res.writeHead(403); res.end('denied'); return; }
    res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify({ data }));
  });
  await new Promise(resolve => fixture.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => fixture.close(resolve)));
  const config = { endpoint: `https://127.0.0.1:${fixture.address().port}`, tokenId: 'test@pve!fixture', secret: 'transport-fixture-only', ca: cert.toString() };
  const adapter = new Proxmox();
  const target={node:'pve-a',expectedUuid:'12345678-1234-1234-1234-123456789abc'};
  await assert.rejects(adapter.machineIdentity({ ...config, ca: '' },710,target), error => error.code === 'proxmox_certificate_error');
  const vms = await adapter.inventory(config,{targets:[{...target,vmid:710}]}); assert.deepEqual(vms.map(vm => vm.vmid), [710]); assert.equal(vms[0].memoryBytes, 4294967296);
  assert.equal(vms[0].osFamily, 'windows'); assert.equal(vms[0].pveOsType, 'win11');
  assert.equal((await adapter.machineIdentity(config, 710,target)).machineUuid, '12345678-1234-1234-1234-123456789abc');
  const expiry = await adapter.tokenStatus(config); assert.equal(expiry.expiresAt, new Date(1900000000 * 1000).toISOString()); assert.equal(expiry.accountExpiryKnown, true);
  const submitted = await adapter.power(config, 710, 'start',target); assert.equal(submitted.status, 'submitted');
  assert.deepEqual(await adapter.task(config, submitted.node, submitted.task), { status: 'stopped', exitStatus: 'OK' });
  powerState = 'running';
  await adapter.power(config, 710, 'stop',target);
  await adapter.power(config, 710, 'stop', { ...target,overruleShutdown: true });
  const stopRequests = requests.filter(req => req.url.endsWith('/status/stop'));
  assert.deepEqual(stopRequests.map(req => req.body), ['', 'overrule-shutdown=1']);
  assert.deepEqual(stopRequests.map(req => req.contentLength), ['0', '19']);
  assert.ok(stopRequests.every(req => !/skiplock|reset|keepActive/.test(req.url + req.body)));
  await assert.rejects(adapter.power(config, 710, 'reboot', { overruleShutdown: true }), error => error.code === 'invalid_power_action');
  await assert.rejects(adapter.power(config, 712, 'start',target), error => error.code === 'proxmox_vm_not_found');
  await assert.rejects(adapter.power(config, 999, 'start',target), error => error.code === 'proxmox_permission_denied');
  assert.equal(requests.some(req=>req.url.includes('/cluster/')||req.method==='POST'&&!req.url.includes('/710/')),false);
  assert.ok(requests.every(req => req.auth === 'PVEAPIToken=test@pve!fixture=transport-fixture-only'));
  assert.equal(requests.filter(req => req.method === 'POST').length, 3);
});

test('Stop adapter constructs only the documented exact endpoint and optional overrule form', async () => {
  const adapter=new Proxmox(),calls=[];
  const expectedUuid='12345678-1234-1234-1234-123456789abc',scope={node:'pve-fixture',expectedUuid};
  const reads=[];adapter.call=async(_config,path,method='GET',body='')=>{if(method==='GET'){reads.push(path);if(path.endsWith('/config?current=1'))return{smbios1:'uuid='+expectedUuid};assert.equal(path,'/nodes/pve-fixture/qemu/710/status/current');return{status:'running'};}calls.push([path,method,body]);return 'UPID:pve-fixture:1:2:3:qmstop:710:test@pve:';};
  let authorityChecks=0;
  await adapter.power({},710,'stop',{...scope,beforeSubmit:()=>authorityChecks++});await adapter.power({},710,'stop',{...scope,overruleShutdown:true,beforeSubmit:()=>authorityChecks++});
  assert.deepEqual(calls,[['/nodes/pve-fixture/qemu/710/status/stop','POST',''],['/nodes/pve-fixture/qemu/710/status/stop','POST','overrule-shutdown=1']]);
  for(const action of ['reset','destroy','stop?skiplock=1'])await assert.rejects(adapter.power({},710,action),error=>error.code==='invalid_power_action');
  await assert.rejects(adapter.power({},'710/status/reset','stop'),error=>error.code==='invalid_power_action');
  await assert.rejects(adapter.power({},710,'stop',{overruleShutdown:'true'}),error=>error.code==='invalid_power_action');
  assert.equal(calls.length,2);
  assert.equal(authorityChecks,2);assert.equal(reads.length,6);assert.ok(reads.every(path=>path.startsWith('/nodes/pve-fixture/qemu/710/')));
});

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { configuration, validatePublicOrigin, validateDeploymentKind } from '../src/config.mjs';
import { renderProxmoxSetup } from '../src/setup-script.mjs';
import { buildServer } from '../src/http.mjs';
import { Store } from '../src/store.mjs';

test('Fresh production exposes explicit admin bootstrap without an invented origin; configured HTTPS and development remain compatible', () => {
  const fresh = configuration({ NODE_ENV: 'production', MULTITAP_DATA_DIR: '/tmp/multitap-config-test' });
  assert.equal(fresh.publicOrigin, null);
  assert.equal(fresh.accessConfig, null);
  assert.equal(fresh.bootstrapOnly, true);
  assert.deepEqual(fresh.allowedOrigins, []);
  assert.equal(fresh.bootstrapHost, '0.0.0.0');
  assert.equal(fresh.bootstrapPort, 18480);
  assert.equal(configuration({}).publicOrigin, 'https://localhost');
  const config = configuration({ NODE_ENV: 'production', MULTITAP_DATA_DIR: '/tmp/multitap-config-test', MULTITAP_PUBLIC_ORIGIN: 'https://ALPHA.example:443/' });
  assert.equal(config.publicOrigin, 'https://alpha.example');
  assert.equal(validatePublicOrigin('https://beta.example:8443'), 'https://beta.example:8443');
  assert.equal(validatePublicOrigin('https://[::1]:8443/'), 'https://[::1]:8443');
});

test('deployment type is explicitly configured and never inferred from runtime environment',()=>{
  assert.equal(configuration({}).deploymentKind,'unknown');
  for(const kind of ['systemd','docker','external','unknown'])assert.equal(configuration({MULTITAP_DEPLOYMENT_KIND:kind}).deploymentKind,kind);
  for(const kind of ['',null,[],{},'Docker',' docker','docker\n','/var/run/docker.sock'])assert.throws(()=>validateDeploymentKind(kind),/invalid_deployment_kind/);
  assert.equal(configuration({NODE_ENV:'development',container:'docker',INVOCATION_ID:'not-detection'}).deploymentKind,'unknown');
});

test('Malformed, credential-bearing and non-origin public addresses fail closed', async () => {
  for (const origin of [undefined, null, '', 'http://alpha.example', 'https:alpha.example', ' https://alpha.example', 'https://alpha.example\n', 'https://alpha.example/path', 'https://alpha.example/.', 'https://alpha.example//', 'https://alpha.example?query=1', 'https://alpha.example?', 'https://alpha.example#fragment', 'https://alpha.example#', 'https://user:secret@alpha.example', 'https://@alpha.example', 'https://alpha.example:0', 'https://alpha.example:65536', 'https://alpha.example\\evil']) {
    assert.throws(() => validatePublicOrigin(origin), /invalid_public_origin/);
  }
  await assert.rejects(buildServer({ store: {} }), /invalid_public_origin/);
  assert.throws(() => configuration({ MULTITAP_PUBLIC_ORIGIN: 'http://alpha.example' }), /invalid_public_origin/);
});

test('Proxmox script rendering requires exactly one unconfigured source marker', () => {
  const template = "# setup\nBROKER_ORIGIN = ''\nprint(BROKER_ORIGIN)\n";
  assert.match(renderProxmoxSetup(template, 'https://alpha.example/'), /^BROKER_ORIGIN = "https:\/\/alpha\.example"$/m);
  for (const broken of ['', "BROKER_ORIGIN = 'already-configured'", template + "BROKER_ORIGIN = ''\n", "# BROKER_ORIGIN = ''\n"]) {
    assert.throws(() => renderProxmoxSetup(broken, 'https://alpha.example'), /invalid_proxmox_setup_template/);
  }
  assert.throws(() => renderProxmoxSetup(template, "https://alpha.example/'\nprint('injected')"), /invalid_public_origin/);
});

test('Independent HTTP brokers serve enrollment scripts for their own origins without modifying source', async t => {
  const source = await readFile(new URL('../setup/proxmox.py', import.meta.url), 'utf8');
  const origins = ['https://alpha.example', 'https://beta.example:8443'];
  const instances = [];
  t.after(async () => {
    for (const { server, store } of instances) {
      await new Promise(resolve => server.close(resolve));
      store.close();
    }
  });
  for (const publicOrigin of origins) {
    const store = new Store(':memory:');
    const server = await buildServer({ store, publicOrigin, logger: { error: value => assert.fail(value) } });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
    instances.push({ server, store });
  }
  for (const [index, { server }] of instances.entries()) {
    const response = await fetch(`http://127.0.0.1:${server.address().port}/setup/proxmox.py`);
    assert.equal(response.status, 200);
    assert.equal(response.headers.get('cache-control'), 'no-store');
    const rendered = await response.text();
    const embedded = /^BROKER_ORIGIN = (.+)$/m.exec(rendered);
    assert.equal(JSON.parse(embedded[1]), origins[index]);
    assert.equal(rendered.includes(origins[1 - index]), false);
    assert.equal(rendered.match(/^BROKER_ORIGIN = /gm).length, 1);
  }
  assert.equal(await readFile(new URL('../setup/proxmox.py', import.meta.url), 'utf8'), source);
  assert.match(source, /^BROKER_ORIGIN = ''$/m);
});

import test from 'node:test';
import assert from 'node:assert/strict';
import { normalizeServerAccess, serverAccessHash, serverAccessListeners, serverAccessPortMismatches } from '../update/server-access-policy.mjs';
import { resolveRequestAccess } from '../src/request-origin.mjs';
import { admissionEnvironment, renderAccessEnvironment, rebaseInstallationFeed, ServerAccessDeployment, accessProbeTransport, accessIdentityDecision, portPreflight } from '../update/server-access-deployment.mjs';
import { planDockerAccess, renderDockerAccessOverlay, decodeDockerAccessManifest } from '../update/server-access-docker-policy.mjs';

const initial = () => normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: 'https://alpha.example:8443', https: { enabled: true, origin: 'https://alpha.example:8443', mode: 'proxy', listenHost: '0.0.0.0', listenPort: 18480 }, http: { enabled: false, origin: null, listenHost: '0.0.0.0', listenPort: 18481 }, trustedProxies: ['192.0.2.10'] });
test('bootstrap private probe uses its observed configured receiver port without an invented public origin', () => {
  const probe = accessProbeTransport(null, 'MULTITAP_HOST=0.0.0.0\nMULTITAP_PORT=19327\n');
  assert.equal(probe.port, 19327); assert.equal(probe.host, '0.0.0.0'); assert.equal(probe.publicOrigin, null); assert.equal(probe.bootstrapOnly, true);
  assert.throws(() => accessProbeTransport(null, 'MULTITAP_PORT=not-a-port\n'), /installation_invalid/);
});
test('admission marker removal preserves all unrelated original bytes, including CRLF and no final newline', () => {
  const before = '# preserved\r\nOTHER_TOKEN=opaque\r\nMULTITAP_PUBLIC_ORIGIN=https://alpha.example';
  const marked = admissionEnvironment(before, '12345678-1234-4234-8234-123456789abc');
  assert.equal(admissionEnvironment(marked, null), before);
  const rendered = renderAccessEnvironment(before, initial());
  assert.match(rendered, /OTHER_TOKEN=opaque/); assert.match(rendered, /MULTITAP_PORT=18480/); assert.match(rendered, /MULTITAP_TRUSTED_PROXIES=192\.0\.2\.10/);
});
test('only fixed self-hosted feed follows an explicitly enabled primary HTTP origin; external operator bytes stay exact', () => {
  const previous = initial(), next = normalizeServerAccess({ ...previous, primaryOrigin: 'http://alpha.example:8080', http: { ...previous.http, enabled: true, origin: 'http://alpha.example:8080' } });
  const local = Buffer.from(JSON.stringify({ publicKey: 'pinned-unchanged', feed: previous.primaryOrigin + '/downloads/server-updates.json' }));
  const changed = JSON.parse(rebaseInstallationFeed(local, previous, next));
  assert.equal(changed.feed, 'http://alpha.example:8080/downloads/server-updates.json'); assert.equal(changed.allowInsecureHttp, true); assert.equal(changed.publicKey, 'pinned-unchanged');
  const external = Buffer.from('{ "feed": "https://operator.example/updates.json", "publicKey": "pinned-unchanged" }\n');
  assert.deepEqual(rebaseInstallationFeed(external, previous, next), external);
  assert.deepEqual(rebaseInstallationFeed(local, null, next), local, 'bootstrap without an owned prior origin cannot rewrite an arbitrary operator feed');
});
test('Docker plan preserves unrelated mappings and actual proxy backend port, separate from public NAT ports', () => {
  const previous = initial(), config = normalizeServerAccess({ ...previous, https: { ...previous.https, listenPort: 24443 }, http: { ...previous.http, enabled: true, origin: 'http://alpha.example:8080' } });
  const bindings = [{ hostPort: 19480, containerPort: 18480, protocol: 'tcp', hostIp: '127.0.0.1' }, { hostPort: 22000, containerPort: 22000, protocol: 'udp', hostIp: '' }];
  const plan = planDockerAccess(config, { currentConfig: previous, bindings, proxyPort: 19480 });
  assert.deepEqual(plan.dockerPorts, [{ hostPort: 19480, containerPort: 24443, protocol: 'tcp', scheme: 'https' }, { hostPort: 18481, containerPort: 18481, protocol: 'tcp', scheme: 'http' }]);
  assert(plan.bindings.some(row => row.protocol === 'udp' && row.hostPort === 22000));
  const manifest = { schema: 1, config, revision: serverAccessHash(config), ...plan }, yaml = renderDockerAccessOverlay(manifest);
  assert.match(yaml, /ports: !override/); assert.match(yaml, /published: "19480"/); assert.doesNotMatch(yaml, /docker\.sock|privileged:|cap_add:|image:/);
  assert.throws(() => decodeDockerAccessManifest({ ...manifest, revision: '0'.repeat(64) }), /mapping_invalid/);
});
test('isolated Linux fixture constructor cannot select production or arbitrary service paths', () => {
  const adapter = { platform: 'linux-x64-systemd' };
  const fixture = new ServerAccessDeployment({ adapter, directory: '/fixture-owned', fixture: { id: '0123456789abcdef' } });
  assert.equal(fixture.serviceName, 'multitap-access-fixture-0123456789abcdef'); assert.equal(fixture.systemEnv, '/etc/multitap-access-fixture-0123456789abcdef/broker.env');
  for (const id of ['../../multitap-broker', 'broker', 'A'.repeat(16), '0'.repeat(33)]) assert.throws(() => new ServerAccessDeployment({ adapter, directory: '/fixture-owned', fixture: { id } }), /fixture_invalid/);
});

test('root-side identity history distinguishes initial legacy adoption from known-file loss without changing data directory ownership', () => {
  const known = Buffer.from('a'.repeat(64) + '\n'), replacement = Buffer.from('b'.repeat(64));
  assert.equal(accessIdentityDecision({ identity: null, witness: null }), null, 'first genuine legacy/bootstrap can create its initial identity');
  assert.equal(accessIdentityDecision({ identity: known, witness: null, hasJournal: true }), 'a'.repeat(64), 'migration adopts the existing identity rather than changing it');
  assert.equal(accessIdentityDecision({ identity: known, witness: known }), 'a'.repeat(64));
  for (const history of [{ witness: known }, { witness: null, hasPolicy: true }, { witness: null, hasJournal: true }])
    assert.throws(() => accessIdentityDecision({ identity: null, ...history }), /identity_missing/);
  assert.throws(() => accessIdentityDecision({ identity: replacement, witness: known }), /identity_changed/);
  assert.throws(() => accessIdentityDecision({ identity: known, witness: Buffer.from('malformed') }), /identity_invalid/);
});

// An example layout: HTTPS proxy on 0.0.0.0:18480, HTTP http://10.0.0.121:18480 with the 18481 listener default.
test('external port ≠ listener on this machine is refused; proxy, router and Docker hops are not', async () => {
  const live = { ...initial(), http: { enabled: true, origin: 'http://10.0.0.121:18480', listenHost: '0.0.0.0', listenPort: 18481 } };
  const own = async host => host === '10.0.0.121';
  assert.deepEqual(serverAccessPortMismatches(live), [{ scheme: 'http', hostname: '10.0.0.121', external: 18480, listenPort: 18481 }]);
  await assert.rejects(portPreflight(live, { isOwnHost: own }), /^Error: server_access_external_port_conflict$/);
  await portPreflight(live, { isOwnHost: async () => false }); // a router may map a foreign address
  await portPreflight({ ...live, http: { ...live.http, listenHost: '127.0.0.1' } }, { isOwnHost: own }); // loopback: only a local proxy reaches it
  await portPreflight({ ...live, http: { ...live.http, origin: 'http://alpha.example' } }, { isOwnHost: async () => true }); // HTTP served by the HTTPS proxy's host
  await portPreflight(live, { isOwnHost: own, dockerPorts: [{ scheme: 'http', hostPort: 18480, containerPort: 18481, protocol: 'tcp' }] });
  await assert.rejects(portPreflight(live, { isOwnHost: own, dockerPorts: [{ scheme: 'http', hostPort: 18481, containerPort: 18481, protocol: 'tcp' }] }), /external_port_conflict/);
});
test('native HTTP shares the HTTPS proxy listener on one port and passes the port checks', async () => {
  const shared = normalizeServerAccess({ ...initial(), http: { enabled: true, origin: 'http://10.0.0.121:18480', listenHost: '0.0.0.0', listenPort: 18480 } });
  assert.deepEqual(serverAccessListeners(shared).map(row => ({ protocol: row.protocol, host: row.host, port: row.port, origins: row.origins })),
    [{ protocol: 'http', host: '0.0.0.0', port: 18480, origins: ['https://alpha.example:8443', 'http://10.0.0.121:18480'] }]);
  assert.deepEqual(serverAccessPortMismatches(shared), []);
  await portPreflight(shared, { isOwnHost: async () => true, udpPort: 18481, appliedConfig: initial() });
  const request = (headers, peer) => ({ rawHeaders: Object.entries(headers).flat(), headers, socket: { encrypted: false, localPort: 18480, remoteAddress: peer, localAddress: '10.0.0.121' } });
  assert.equal(resolveRequestAccess(request({ host: '10.0.0.121:18480' }, '10.0.0.50'), shared).origin, 'http://10.0.0.121:18480');
  assert.equal(resolveRequestAccess(request({ host: 'alpha.example:8443', 'x-forwarded-proto': 'https' }, '192.0.2.10'), shared).origin, 'https://alpha.example:8443');
});
test('HTTP on the UDP media port is refused unless it was already applied that way', async () => {
  const next = { ...initial(), http: { enabled: true, origin: 'http://10.0.0.121:18481', listenHost: '0.0.0.0', listenPort: 18481 } };
  await assert.rejects(portPreflight(next, { isOwnHost: async () => true, udpPort: 18481, appliedConfig: initial() }), /^Error: server_access_udp_port_conflict$/);
  await portPreflight(next, { isOwnHost: async () => true, udpPort: 18481, appliedConfig: next });
  await portPreflight(next, { isOwnHost: async () => true, udpPort: null, appliedConfig: initial() });
});

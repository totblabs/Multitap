import { test } from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { serverAccessUnconfiguredRevision } from '../update/server-access-policy.mjs';
import { configuration, validatePublicOrigin } from '../src/config.mjs';
import { brokerWebSocketOrigin, describeTransport } from '../src/transport-policy.mjs';
import { deploymentEnvironment, localHealthOptions, localBrokerHealth, bootstrapHealthOrigin } from '../update/deployment-health.mjs';
import { validateDockerConfig, DockerAdapter } from '../update/docker.mjs';
import { Relay } from '../src/relay.mjs';

test('transport keeps HTTPS proxy defaults and requires an exact explicit HTTP server opt-in', () => {
  assert.equal(configuration({}).publicOrigin, 'https://localhost');
  assert.equal(configuration({}).tlsMode, 'proxy');
  assert.equal(configuration({}).tlsFiles, null);
  assert.throws(() => configuration({ MULTITAP_PUBLIC_ORIGIN: 'http://192.0.2.10:18400' }), /invalid_public_origin/);
  const config = configuration({ MULTITAP_PUBLIC_ORIGIN: 'http://192.0.2.10:18400/', MULTITAP_ALLOW_INSECURE_HTTP: '1', MULTITAP_PORT: '18500' });
  assert.equal(config.publicOrigin, 'http://192.0.2.10:18400');
  assert.equal(config.port, 18500, 'external NAT port is not the listener port');
  assert.deepEqual(describeTransport(config.publicOrigin, config), { protocol: 'broker-transport-v1', publicOrigin: config.publicOrigin, scheme: 'http', allowInsecureHttp: true, directTls: false });
  for (const flag of [true, 1, 'true', 'yes', ' 1', '1\n', null]) assert.throws(() => configuration({ MULTITAP_ALLOW_INSECURE_HTTP: flag }), /invalid_http_opt_in/);
});

test('common transport vectors preserve scheme, effective port and IPv6 authority', () => {
  for (const [raw, normalized, ws] of [
    ['https://EXAMPLE.test:443/', 'https://example.test', 'wss://example.test'],
    ['https://example.test:8443', 'https://example.test:8443', 'wss://example.test:8443'],
    ['http://example.test', 'http://example.test', 'ws://example.test'],
    ['http://192.0.2.10:18400/', 'http://192.0.2.10:18400', 'ws://192.0.2.10:18400'],
    ['http://127.0.0.1.:18400', 'http://127.0.0.1:18400', 'ws://127.0.0.1:18400'],
    ['http://[2001:db8::1]:18400', 'http://[2001:db8::1]:18400', 'ws://[2001:db8::1]:18400'],
  ]) {
    assert.equal(validatePublicOrigin(raw, { allowInsecureHttp: true }), normalized);
    assert.equal(brokerWebSocketOrigin(normalized), ws);
  }
  for (const raw of ['http://example.test', 'http://example.test:18480']) assert.throws(() => validatePublicOrigin(raw), /invalid_public_origin/);
});

test('production Host redirection URL builders use the same scheme and public port without changing permissions', () => {
  const session = { id: '12345678-1234-4234-9234-123456789abc', userId: 'fixture-user', desktopId: 'fixture-vm', usb: true, print: true, driveEnabled: true, driveFs: true };
  for (const [publicOrigin, ws] of [['https://outside.example:18400', 'wss://outside.example:18400'], ['http://192.0.2.10:18400', 'ws://192.0.2.10:18400']]) {
    const receiver = { publicOrigin, store: { devicePolicy: (user, desktop) => { assert.equal(user, session.userId); assert.equal(desktop, session.desktopId); return { drives: true, printers: false, smartCards: false, serialPorts: false }; } } };
    // Actual admitted sessions always retain their authenticated Host channel.
    const admitted = { ...session, host: { origin: publicOrigin } };
    const drive = Relay.prototype.driveStartFields.call(receiver, admitted), print = Relay.prototype.printStartFields.call(receiver, admitted), usb = Relay.prototype.usbStartFields.call(receiver, admitted);
    assert.equal(drive.driveUrl, ws + '/api/v1/hosts/sessions/' + session.id + '/drive');
    assert.equal(print.printUrl, ws + '/api/v1/hosts/sessions/' + session.id + '/print');
    assert.equal(usb.usbUrl, ws + '/api/v1/hosts/sessions/' + session.id + '/usb');
    assert.deepEqual(usb.usbGrant, { drives: true, printers: false, smartCards: false, serialPorts: false });
    assert.deepEqual(Relay.prototype.driveStartFields.call(receiver, { ...admitted, driveEnabled: false }), {});
    assert.deepEqual(Relay.prototype.printStartFields.call(receiver, { ...admitted, print: false }), {});
    assert.deepEqual(Relay.prototype.usbStartFields.call(receiver, { ...admitted, usb: false }), {});
  }
});

test('ambiguous, credential-bearing or non-origin URLs and nondecimal ports are rejected in both modes', () => {
  for (const raw of [undefined, null, '', 'https:host', ' https://host', 'https://host\n', 'http://host/path', 'http://host/.', 'http://host//', 'http://host?', 'http://host#', 'http://a@host', 'http://@host', 'http://host\\other', 'http://host:0', 'http://host:65536', 'http://host:1e3', 'http://host:+80', 'http://host:80\n', 'http://host:', 'http://[::1%25zone]:80', 'http://127.1', 'http://2130706433', 'http://0x7f000001', 'http://0177.0.0.1', 'http://host_name', 'http://-host', 'http://host-', 'http://host..example', "http://host'quote"]) {
    assert.throws(() => validatePublicOrigin(raw, { allowInsecureHttp: true }), /invalid_public_origin/);
  }
  for (const value of ['', ' 18480', '18480 ', '1e4', '+80', '65536', '0', true, null]) assert.throws(() => configuration({ MULTITAP_PORT: value }), /invalid_port/);
  for (const value of ['80', '443', '18480', '65535']) assert.equal(configuration({ MULTITAP_PORT: value }).port, Number(value));
});

test('direct TLS requires an HTTPS canonical origin and supplied absolute certificate/key pair without reading either', () => {
  const paths = process.platform === 'win32' ? { cert: 'C:/test-supplied/cert.pem', key: 'C:/test-supplied/key.pem' } : { cert: '/test-supplied/cert.pem', key: '/test-supplied/key.pem' };
  const env = { MULTITAP_PUBLIC_ORIGIN: 'https://example.test:18400', MULTITAP_TLS_MODE: 'direct', MULTITAP_TLS_CERT_FILE: paths.cert, MULTITAP_TLS_KEY_FILE: paths.key };
  assert.deepEqual(configuration(env).tlsFiles, { certFile: paths.cert, keyFile: paths.key });
  for (const patch of [{ MULTITAP_TLS_MODE: 'automatic' }, { MULTITAP_TLS_KEY_FILE: undefined }, { MULTITAP_TLS_CERT_FILE: 'relative.pem' }, { MULTITAP_TLS_KEY_FILE: paths.cert }, { MULTITAP_TLS_MODE: 'proxy' }, { MULTITAP_PUBLIC_ORIGIN: 'http://example.test', MULTITAP_ALLOW_INSECURE_HTTP: '1' }]) assert.throws(() => configuration({ ...env, ...patch }), /invalid_tls_/);
  for (const host of ['0.0.0.0', '127.0.0.1', '::', '::1']) assert.equal(configuration({ MULTITAP_HOST: host }).host, host);
  assert.equal(configuration({ MULTITAP_HOST: 'localhost' }).host, '127.0.0.1');
  for (const host of ['', '*', 'http://localhost', '0.0.0.0\n', '127.0.0.1:18480']) assert.throws(() => configuration({ MULTITAP_HOST: host }), /invalid_bind_host/);
});

test('deployment environment parses data, rejects duplicate/expanded transport settings and preserves actual listener port', () => {
  const config = deploymentEnvironment('NODE_ENV=production\nMULTITAP_PUBLIC_ORIGIN="http://192.0.2.10:18400"\nMULTITAP_ALLOW_INSECURE_HTTP=1\nMULTITAP_HOST=0.0.0.0\nMULTITAP_PORT=18500\n');
  const health = localHealthOptions(config);
  assert.equal(health.tls, false);
  assert.equal(health.options.hostname, '127.0.0.1');
  assert.equal(health.options.port, 18500);
  assert.equal(health.options.headers.Host, '192.0.2.10:18400');
  assert.equal(health.options.path, '/healthz');
  for (const text of ['MULTITAP_PUBLIC_ORIGIN=http://host\n', 'MULTITAP_PUBLIC_ORIGIN=https://host\nMULTITAP_PORT=18000\nMULTITAP_PORT=18001\n', 'MULTITAP_PUBLIC_ORIGIN=https://$(id)\n', 'MULTITAP_PUBLIC_ORIGIN=https://host\nMULTITAP_PORT=18480;true\n']) assert.throws(() => deploymentEnvironment(text), /update_installation_invalid/);
  assert.equal(deploymentEnvironment('MULTITAP_PUBLIC_ORIGIN=https://host\n').port, 18480, 'legacy default remains compatible');
  assert.equal(deploymentEnvironment('MULTITAP_PUBLIC_ORIGIN=https://host\n', { portOverride: 18400 }).port, 18400, 'Docker host mapping is distinct from container 18480');
});

test('direct-TLS local health uses local bind port with canonical SNI and certificate verification', () => {
  const config = deploymentEnvironment('MULTITAP_PUBLIC_ORIGIN=https://outside.example:18400\nMULTITAP_HOST=0.0.0.0\nMULTITAP_PORT=18500\nMULTITAP_TLS_MODE=direct\nMULTITAP_TLS_CERT_FILE=/etc/supplied/cert.pem\nMULTITAP_TLS_KEY_FILE=/etc/supplied/key.pem\n');
  const health = localHealthOptions(config);
  assert.equal(health.tls, true);
  assert.equal(health.options.hostname, '127.0.0.1');
  assert.equal(health.options.port, 18500);
  assert.equal(health.options.servername, 'outside.example');
  assert.equal(health.options.rejectUnauthorized, true);
  assert.equal(health.options.headers.Host, 'outside.example:18400');
  assert.ok(health.options.checkServerIdentity('127.0.0.1', { subjectaltname: 'DNS:other.example', subject: { CN: 'other.example' } }), 'canonical-name mismatch is not bypassed');
});

function memoryHealth(body, { status = 200, chunks, noEnd = false } = {}) {
  const calls = [];
  const request = (options, callback) => {
    calls.push(options);
    const req = new EventEmitter(); req.destroy = () => {}; req.end = () => queueMicrotask(() => {
      const response = new EventEmitter(); response.statusCode = status; response.destroy = () => {};
      callback(response);
      for (const chunk of chunks ?? [Buffer.from(JSON.stringify(body))]) response.emit('data', chunk);
      if (!noEnd) response.emit('end');
    });
    return req;
  };
  return { request, calls };
}

test('health refuses redirect, oversized body, wrong public transport and old HTTP-backend responses without retry/fallback', async () => {
  const config = deploymentEnvironment('MULTITAP_PUBLIC_ORIGIN=http://outside.example:18400\nMULTITAP_ALLOW_INSECURE_HTTP=1\nMULTITAP_PORT=18500\n');
  const good = { service: 'multitap-broker', status: 'ok', version: '0.5.77', transport: describeTransport(config.publicOrigin, config) };
  for (const change of [value => delete value.transport, value => { value.transport.publicOrigin = 'http://outside.example:18500'; }, value => { value.transport.scheme = 'https'; }, value => { value.transport.allowInsecureHttp = false; }]) {
    const value = structuredClone(good); change(value); const fake = memoryHealth(value);
    assert.equal(await localBrokerHealth(config, { http: fake.request, https: () => assert.fail('no TLS retry') }), null);
    assert.equal(fake.calls.length, 1);
  }
  for (const options of [{ status: 302 }, { chunks: [Buffer.alloc(65537)] }]) {
    const fake = memoryHealth(good, options); assert.equal(await localBrokerHealth(config, { http: fake.request }), null); assert.equal(fake.calls.length, 1);
  }
  const accepted = memoryHealth(good);
  assert.deepEqual(await localBrokerHealth(config, { http: accepted.request }), { service: 'multitap-broker', status: 'ok', version: '0.5.77' });
  const legacy = memoryHealth({ service: 'multitap-broker', status: 'ok', version: '0.5.76' });
  assert.equal((await localBrokerHealth(deploymentEnvironment('MULTITAP_PUBLIC_ORIGIN=https://legacy.example\n'), { http: legacy.request })).version, '0.5.76');
  const optional = memoryHealth({ service: 'multitap-broker', status: 'ok', version: '0.5.76', transport: { future: true } });
  assert.equal((await localBrokerHealth(deploymentEnvironment('MULTITAP_PUBLIC_ORIGIN=https://legacy.example\n'), { http: optional.request })).version, '0.5.76', 'HTTPS compatibility does not depend on optional transport metadata');
});

const dockerConfig = () => ({ projectDirectory: '/opt/multitap/project', composeFile: 'compose.yaml', envFile: '.env', overrideFile: 'multitap-image.json', projectName: 'multitap', serviceName: 'broker', dataVolume: 'multitap-broker-data', imageRepository: 'registry.example/broker', healthPort: 443, brokerUid: 1000, brokerGid: 1000, updatesDirectory: '/opt/multitap/updates' });
test('Docker native TLS allows only its explicit protected supplied-TLS mount, no broader host mount', () => {
  const config = { ...dockerConfig(), tlsDirectory: '/etc/multitap/supplied-tls' };
  assert.equal(validateDockerConfig(config).healthPort, 443);
  const adapter = new DockerAdapter(config);
  const value = { Id: '1'.repeat(64), Image: 'sha256:' + '2'.repeat(64), Config: { User: '1000:1000', Env: ['MULTITAP_TLS_MODE=direct', 'MULTITAP_TLS_CERT_FILE=/etc/multitap/tls/cert.pem', 'MULTITAP_TLS_KEY_FILE=/etc/multitap/tls/key.pem'], Labels: { 'com.docker.compose.project': 'multitap', 'com.docker.compose.service': 'broker', 'com.docker.compose.oneoff': 'False', 'com.docker.compose.config-hash': '3'.repeat(64) } }, HostConfig: { Privileged: false, ReadonlyRootfs: true, CapDrop: ['ALL'], NetworkMode: 'multitap_default', PortBindings: { '18480/tcp': [{ HostPort: '443', HostIp: '' }] } }, State: { Running: true }, NetworkSettings: { Ports: { '18480/tcp': [{ HostPort: '443', HostIp: '' }] } }, Mounts: [{ Type: 'volume', Name: 'multitap-broker-data', Destination: '/var/lib/multitap', RW: true }, { Type: 'bind', Source: config.tlsDirectory, Destination: '/etc/multitap/tls', RW: false }] };
  assert.equal(adapter.validateContainer(value), value);
  for (const mutate of [v => { v.Mounts[1].RW = true; }, v => { v.Mounts[1].Source = '/etc'; }, v => { v.Config.Env[0] = 'MULTITAP_TLS_MODE=proxy'; }, v => { v.Mounts.push({ Type: 'bind', Source: '/var/run/docker.sock', Destination: '/var/run/docker.sock', RW: false }); }]) { const v = structuredClone(value); mutate(v); assert.throws(() => adapter.validateContainer(v), /update_docker_installation_invalid/); }
  assert.throws(() => new DockerAdapter(dockerConfig()).validateContainer(value), /update_docker_installation_invalid/);
});

test('bootstrap basic health binds exact Root receiver and minimal non-client transport', async () => {
  const config = deploymentEnvironment('MULTITAP_HOST=0.0.0.0\nMULTITAP_PORT=18500\n'), origin = 'http://127.0.0.1:18500';
  const good = { service: 'multitap-broker', status: 'ok', authentication: 'id-password', version: '0.5.84',
    transport: { protocol: 'broker-transport-v1', publicOrigin: origin, scheme: 'http', allowInsecureHttp: false, directTls: false },
    access: { protocol: 'server-access-v1', revision: serverAccessUnconfiguredRevision, installationId: null, origin, bootstrapOnly: true } };
  assert.equal(bootstrapHealthOrigin(config), origin);
  assert.equal(bootstrapHealthOrigin({ ...config, host: '::', port: 80 }), 'http://[::1]');
  for (const patch of [{ host: 'untrusted-fixture.invalid' }, { port: 0 }, { port: 65536 }, { port: '18500' }, { tlsMode: 'direct' }, { publicOrigin: origin }]) assert.equal(bootstrapHealthOrigin({ ...config, ...patch }), null);
  const fake = memoryHealth(good); assert.equal((await localBrokerHealth(config, { http: fake.request })).version, good.version);
  assert.equal(fake.calls[0].headers.Host, '127.0.0.1:18500'); assert.equal(fake.calls[0].path, '/healthz');
  for (const patch of [{ protocol: 'unknown' }, { bootstrapOnly: false }, { origin: 'http://127.0.0.1:18501' }, { origin: 'http://untrusted-fixture.invalid:18500' },
    { revision: 'a'.repeat(64) }, { primaryOrigin: 'http://192.0.2.10' }, { allowedOrigins: ['http://192.0.2.10'] }, { healthProbeOnly: 'true' }]) {
    const invalid = memoryHealth({ ...good, access: { ...good.access, ...patch } }); assert.equal(await localBrokerHealth(config, { http: invalid.request }), null);
  }
  for (const patch of [{ publicOrigin: 'http://127.0.0.1:18501' }, { allowInsecureHttp: true }, { directTls: true }, { scheme: 'https' }]) {
    const invalid = memoryHealth({ ...good, transport: { ...good.transport, ...patch } }); assert.equal(await localBrokerHealth(config, { http: invalid.request }), null);
  }
  const legacy = memoryHealth({ ...good, access: { ...good.access, primaryOrigin: null, allowedOrigins: [] } });
  assert.equal((await localBrokerHealth(config, { http: legacy.request })).version, good.version, 'exact prior empty full profile stays compatible');
});

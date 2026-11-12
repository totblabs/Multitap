import { test } from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, sign, randomBytes } from 'node:crypto';
import { createServer as createTlsServer } from 'node:tls';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { CertificateMonitor, probeCertificate, pemCertificate, certificateAuditEvent } from '../src/certificate-expiry.mjs';

const DAY = 86400000;
// A self-signed P-256 certificate built here, so the test needs no OpenSSL.
const der = (tag, ...parts) => {
  const body = Buffer.concat(parts), length = body.length < 128 ? [body.length] : body.length < 256 ? [0x81, body.length] : [0x82, body.length >> 8, body.length & 255];
  return Buffer.concat([Buffer.from([tag, ...length]), body]);
};
const seq = (...parts) => der(0x30, ...parts), oid = hex => der(0x06, Buffer.from(hex, 'hex'));
const utc = date => der(0x17, Buffer.from(date.toISOString().replace(/[-:T]/g, '').slice(2, 14) + 'Z'));
function selfSigned(notAfter) {
  const { privateKey, publicKey } = generateKeyPairSync('ec', { namedCurve: 'P-256' });
  const algorithm = seq(oid('2a8648ce3d040302')), name = seq(der(0x31, seq(oid('550403'), der(0x0c, Buffer.from('localhost')))));
  const tbs = seq(der(0xa0, der(0x02, Buffer.from([2]))), der(0x02, Buffer.from([1])), algorithm, name, seq(utc(new Date(Date.now() - DAY)), utc(notAfter)), name, publicKey.export({ type: 'spki', format: 'der' }));
  const body = seq(tbs, algorithm, der(0x03, Buffer.from([0]), sign('sha256', tbs, privateKey)));
  return { cert: `-----BEGIN CERTIFICATE-----\n${body.toString('base64').match(/.{1,64}/g).join('\n')}\n-----END CERTIFICATE-----\n`, key: privateKey.export({ type: 'pkcs8', format: 'pem' }) };
}
const audits = store => store.db.prepare('SELECT actor,subject FROM audit_events WHERE event=? ORDER BY id').all(certificateAuditEvent).map(row => ({ ...row }));

test('A listener certificate warns 14 and 3 days ahead and audits each threshold once, across restarts', async () => {
  const store = new Store(':memory:'), start = Date.parse('2026-11-03T00:00:00Z');
  try {
    let clock = start;
    const certificate = { expiresAt: start + 20 * DAY, fingerprint: 'ab'.repeat(32) };
    const monitor = () => new CertificateMonitor({ store, listeners: [certificate, { expiresAt: start + 400 * DAY, fingerprint: 'cd'.repeat(32) }], now: () => clock });
    const first = monitor();
    assert.deepEqual(first.status(), { state: 'unknown', source: 'listener', checkedAt: null });
    assert.equal((await first.check()).state, 'ok'); assert.equal(first.status().daysLeft, 20); assert.deepEqual(audits(store), []);
    clock = start + 6 * DAY; // 14 days left
    assert.deepEqual(await first.check(), { state: 'warning', source: 'listener', expiresAt: '2026-11-23T00:00:00.000Z', daysLeft: 14, checkedAt: '2026-11-09T00:00:00.000Z' });
    await first.check(); await monitor().check();
    assert.deepEqual(audits(store), [{ actor: 'server', subject: `14:2026-11-23:${'ab'.repeat(8)}` }], 'once, also after a restart');
    clock = start + 17 * DAY + 1; // 3 days left (2.99...)
    assert.equal((await monitor().check()).state, 'critical');
    clock = start + 20 * DAY;
    assert.equal((await monitor().check()).state, 'expired');
    assert.deepEqual(audits(store).map(row => row.subject.split(':')[0]), ['14', '3']);
    // A broker that first looks inside 3 days records only the 3-day threshold.
    const late = new Store(':memory:');
    try { clock = start + 18 * DAY; await new CertificateMonitor({ store: late, listeners: [certificate], now: () => clock }).check(); assert.deepEqual(audits(late).map(row => row.subject.split(':')[0]), ['3']); }
    finally { late.close(); }
  } finally { store.close(); }
});

test('Behind a proxy the https origin is checked; a failed check says unknown and writes nothing', async () => {
  const store = new Store(':memory:'), now = Date.now();
  try {
    const asked = [];
    const monitor = new CertificateMonitor({ store, origin: 'https://vdi.fixture.invalid', probe: async origin => { asked.push(origin); return { expiresAt: now + 10 * DAY, fingerprint: 'ef'.repeat(32) }; }, now: () => now });
    assert.equal((await monitor.check()).state, 'warning'); assert.deepEqual(asked, ['https://vdi.fixture.invalid']);
    const failing = new CertificateMonitor({ store, origin: 'https://vdi.fixture.invalid', probe: async () => { throw new Error('ECONNREFUSED'); }, now: () => now });
    assert.deepEqual(await failing.check(), { state: 'unknown', source: 'origin', checkedAt: new Date(now).toISOString(), errorCode: 'tls_check_failed' });
    assert.equal(audits(store).length, 1);
    for (const origin of [null, 'http://vdi.fixture.invalid']) {
      const none = new CertificateMonitor({ store, origin, probe: () => assert.fail('no TLS to check') });
      assert.deepEqual(await none.check(), { state: 'not_applicable' });
    }
  } finally { store.close(); }
});

test('The probe reads the certificate a TLS server presents, and a PEM listener file gives the same', async t => {
  const notAfter = new Date(Math.floor((Date.now() + 10 * DAY) / 1000) * 1000), { cert, key } = selfSigned(notAfter);
  const server = createTlsServer({ cert, key }, socket => socket.end());
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => server.close(resolve)));
  const probed = await probeCertificate(`https://127.0.0.1:${server.address().port}`);
  assert.equal(probed.expiresAt, notAfter.getTime());
  assert.deepEqual(probed, pemCertificate(cert));
  assert.match(probed.fingerprint, /^[0-9a-f]{64}$/);
  // Nothing listening: an error, not a hang.
  const closed = createTlsServer({ cert, key }); await new Promise(resolve => closed.listen(0, '127.0.0.1', resolve));
  const port = closed.address().port; await new Promise(resolve => closed.close(resolve));
  await assert.rejects(probeCertificate(`https://127.0.0.1:${port}`, 2000));
});

test('server-info carries the certificate status to the administrator console', async t => {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), now = Date.now();
  store.addUser({ name: 'owner', passwordHash: 'fixture', role: 'admin' });
  const admin = newToken(); store.createSession(store.findUser('owner'), tokenHash(admin), now, 60000, 'admin');
  const monitor = new CertificateMonitor({ store, listeners: [{ expiresAt: now + 2 * DAY + 60000, fingerprint: '12'.repeat(32) }] });
  await monitor.check();
  const servers = [];
  for (const certificateMonitor of [monitor, null]) {
    const server = await buildServer({ store, vault, publicOrigin: 'https://cert.invalid', proxmox: { inventory: async () => [] }, certificateMonitor, logger: { error: () => assert.fail('Unexpected server error') } });
    await new Promise(resolve => server.listen(0, '127.0.0.1', resolve)); servers.push(server);
  }
  t.after(async () => { for (const server of servers) await new Promise(resolve => server.close(resolve)); store.close(); });
  const read = async server => (await fetch(`http://127.0.0.1:${server.address().port}/api/v1/admin/server-info`, { headers: { Cookie: '__Secure-multitap-admin=' + admin } })).json();
  const info = await read(servers[0]);
  assert.equal(info.tlsCertificate.state, 'critical'); assert.equal(info.tlsCertificate.daysLeft, 3); assert.equal(info.tlsCertificate.source, 'listener');
  assert.deepEqual((await read(servers[1])).tlsCertificate, { state: 'not_applicable' });
  await new Promise(resolve => servers.shift().close(resolve));
  assert.equal(monitor.closed, true, 'closing the server stops the checks');
});

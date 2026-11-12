import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as shellSetup from '../admin/proxmox-shell-setup.js';
import { decodeConnectionBlock, decodeRenewalBlock, enrollmentCommand, renewalCommand, enableConsoleCommand, dedicatedIdentity } from '../admin/proxmox-shell-setup.js';

// The broker's SHA-256 of the served script; the command checks the download against it.
const sha256 = 'ab'.repeat(32), o = { sha256 };
const setup = (url, hash = sha256) => `sh -c 'f=$(mktemp) || exit; curl -q -fsS "$1" -o "$f" && echo "$2  $f" | sha256sum -c --quiet - && shift 2 && python3 "$f" "$@"; s=$?; rm -f "$f"; exit $s' multitap-setup '${url}' ${hash}`;

test('Shell connection blocks survive terminal wrapping without accepting unsafe or incomplete settings', () => {
  const value = { endpoint: 'https://pve.example.com:8006/', tokenId: 'multitap-fixture@pve!broker', secret: 'fixture-secret-only', ca: '-----BEGIN CERTIFICATE-----\nfixture-public-ca\n-----END CERTIFICATE-----' };
  const block = data => 'MULTITAP-CONNECTION-V1:\n' + Buffer.from(JSON.stringify(data)).toString('base64').match(/.{1,76}/g).join('\n') + '\nEND-MULTITAP-CONNECTION';
  assert.deepEqual(decodeConnectionBlock('terminal output\n' + block(value) + '\nnext instruction'), { ...value, endpoint: 'https://pve.example.com:8006' });
  for (const bad of [{ ...value, endpoint: 'http://pve.example.com:8006' }, { ...value, endpoint: 'https://user:password@pve.example.com' }, { ...value, secret: '' }, { ...value, ca: 'not a certificate' }, { ...value, unexpected: 'field' }]) assert.throws(() => decodeConnectionBlock(block(bad)));
  assert.throws(() => decodeConnectionBlock(block(value).replace('END-MULTITAP-CONNECTION', '')));
  assert.throws(() => decodeConnectionBlock(block(value) + '\n' + block(value)));
  assert.equal(decodeConnectionBlock(block({ ...value, expiresAt: '2099-01-01T00:00:00Z' })).expiresAt, '2099-01-01T00:00:00.000Z');
  assert.throws(() => decodeConnectionBlock(block({ ...value, expiresAt: '2099-02-31T00:00:00Z' })));
});

test('Shell commands use each installation origin, quote arguments, and never require a secret', () => {
  const first = enrollmentCommand('https://first.example.com', o);
  const second = enrollmentCommand('https://second.example.com:9443', o);
  assert.equal(first, setup('https://first.example.com/setup/proxmox.py') + ' --lang ko');
  assert.match(second, /https:\/\/second\.example\.com:9443\/setup\/proxmox\.py/);
  assert.equal(second.includes('first.example.com'), false);
  assert.equal(first.includes('second.example.com'), false);
  assert.equal(renewalCommand('https://second.example.com:9443', 'multitap-fixture@pve!broker', 'https://pve.example.com:8006/', o), second + " --renew 'multitap-fixture@pve!broker' --endpoint 'https://pve.example.com:8006'");
  for (const origin of ['http://first.example.com', 'https://first.example.com/admin', 'https://user:password@first.example.com']) assert.throws(() => enrollmentCommand(origin, o));
  // Without the broker's hash (or with anything else) there is no command at all.
  for (const hash of [undefined, '', 'AB'.repeat(32), 'ab'.repeat(31), 'ab'.repeat(32) + ' ; id', "x'"]) assert.throws(() => enrollmentCommand('https://first.example.com', { sha256: hash }), String(hash));
  assert.throws(() => renewalCommand('https://first.example.com', 'owner;echo secret', 'https://pve.example.com'));
});

test('Renewal metadata accepts only a matching token and valid expiry without credential fields', () => {
  const tokenId = 'multitap-fixture@pve!broker';
  const block = data => 'MULTITAP-RENEWAL-V1:\n' + Buffer.from(JSON.stringify(data)).toString('base64') + '\nEND-MULTITAP-RENEWAL';
  const value = { tokenId, expiresAt: '2099-01-01T00:00:00Z' };
  assert.deepEqual(decodeRenewalBlock(block(value), tokenId), { tokenId, expiresAt: '2099-01-01T00:00:00.000Z' });
  for (const bad of [{ ...value, tokenId: 'other@pve!broker' }, { ...value, expiresAt: 'tomorrow' }, { ...value, expiresAt: '2099-02-31T00:00:00Z' }, { ...value, secret: 'never-accepted' }]) assert.throws(() => decodeRenewalBlock(block(bad), tokenId));
  assert.throws(() => decodeRenewalBlock(block(value) + block(value), tokenId));
});
test('Console enable command is a distinct quoted root-Shell action for the exact dedicated identity only', () => {
  const command=enableConsoleCommand('https://second.example.com:9443','multitap-fixture@pve!broker',o);
  assert.equal(command, setup('https://second.example.com:9443/setup/proxmox.py') + " --lang ko --enable-console 'multitap-fixture@pve!broker'");
  assert.equal(command.includes('--renew'),false);assert.equal(command.includes('--endpoint'),false);assert.equal(command.includes('--secret'),false);
  for(const token of ['root@pam!broker','other@pve!broker','multitap-a@pve!broker','multitap-fixture@pam!broker','multitap-fixture@pve!x','multitap-fixture@pve!broker;id',"multitap-fixture@pve!broker'",'multitap-fixture@pve!$(id)','multitap-fixture@pve!`id`','multitap-fixture@pve!broker\nwhoami','multitap-fixture@pve!broker\n','multitap-fixture@pve!broker\u2028'])assert.throws(()=>enableConsoleCommand('https://broker.example',token,o));
  for(const origin of ['http://broker.example','https://user:secret@broker.example','https://broker.example/admin','https://broker.example?secret=x'])assert.throws(()=>enableConsoleCommand(origin,'multitap-fixture@pve!broker',o));
  assert.match(renewalCommand('https://broker.example','multitap-fixture@pve!broker','https://pve.example',o),/ --renew /);
  assert.equal(renewalCommand('https://broker.example','multitap-fixture@pve!broker','https://pve.example',o).includes('--enable-console'),false);
});

test('The console command serves only the identity the setup script made; no sound-card command exists', () => {
  assert.equal(enableConsoleCommand('https://broker.example:9443', 'multitap-owner@pve!broker', o), setup('https://broker.example:9443/setup/proxmox.py') + " --lang ko --enable-console 'multitap-owner@pve!broker'");
  for (const [tokenId, own] of [['multitap-fixture@pve!broker', true], ['vdi-api@pve!multitap', false], ['root@pam!token', false], ['multitap-owner@pam!broker', false], ['MULTITAP-owner@pve!broker', false], ["multitap-owner@pve!broker'", false], ['multitap-owner@pve!broker;id', false], ['other-owner@pve!broker', false], [null, false], [undefined, false]]) {
    assert.equal(dedicatedIdentity(tokenId), own, String(tokenId));
    if (!own) assert.throws(() => enableConsoleCommand('https://broker.example', tokenId, o), String(tokenId));
  }
  // The sound card left Multitap: the console offers no command that grants VM.Config.HWType.
  assert.equal('enableSoundCardCommand' in shellSetup, false);
  assert.equal(Object.values(shellSetup).some(value => typeof value === 'function' && /sound|HWType/i.test(String(value))), false);
});

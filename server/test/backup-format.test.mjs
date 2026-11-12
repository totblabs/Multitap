import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync, existsSync, openSync, ftruncateSync, closeSync } from 'node:fs';
import { join } from 'node:path';
import { randomBytes } from 'node:crypto';
import { writeBackup, readBackup, readBackupHeader, decodeBackupHeader, backupPassphrase, backupMagic, backupChunkSize, backupMaxDatabaseBytes, backupKdf, BackupError } from '../src/backup-format.mjs';
import { temporaryDirectory } from './backup-fixture.mjs';

const passphrase = '맑은 가을 하늘 아래 열두 글자', fast = { name: 'scrypt', N: 32768, r: 8, p: 1 };
const code = expected => error => error instanceof BackupError && error.code === expected;
const manifest = (extra = {}) => ({ schema: 1, origin: 'https://vdi.fixture.example', deploymentKind: 'systemd', database: { schemaVersion: 19 }, vaultKey: Buffer.alloc(32, 7).toString('base64'), serverAccess: null, excluded: ['sessions'], ...extra });
// A stand-in database: recognisable text first, then bytes over several frames.
function sample(directory, size = backupChunkSize * 2 + 1234) {
  const path = join(directory, 'snapshot.sqlite'), bytes = Buffer.concat([Buffer.from('SQLite format 3\0 owner-account-name '), randomBytes(size)]);
  writeFileSync(path, bytes); return { path, bytes };
}
async function made(t, options = {}) {
  const directory = temporaryDirectory(t), { path, bytes } = sample(directory, options.size), output = join(directory, 'backup.mtbk');
  const written = await writeBackup({ passphrase, createdAt: '2026-11-10T07:30:00.000Z', serverVersion: '1.0.43', manifest: manifest(options.manifest), databasePath: path, outputPath: output, kdf: options.kdf ?? fast });
  return { directory, output, bytes, written, file: readFileSync(output) };
}
let serial = 0;
async function reads(directory, bytes, phrase = passphrase) {
  const path = join(directory, `candidate-${++serial}.mtbk`), database = join(directory, `out-${serial}.sqlite`);
  writeFileSync(path, bytes);
  return { result: await readBackup({ path, passphrase: phrase, databasePath: database }), database };
}
const headerLength = file => 12 + file.readUInt16BE(10);
function withHeader(file, change) {
  const length = file.readUInt16BE(10), value = JSON.parse(file.subarray(12, 12 + length).toString('utf8'));
  change(value);
  const json = Buffer.from(JSON.stringify(value)), head = Buffer.from(file.subarray(0, 12)); head.writeUInt16BE(json.length, 10);
  return Buffer.concat([head, json, file.subarray(12 + length)]);
}

test('a backup file round-trips, and without the passphrase shows only its header fields', async t => {
  const { directory, output, bytes, written, file } = await made(t, { manifest: { serverAccess: { protocol: 'server-access-v1', primaryOrigin: 'https://vdi.fixture.example' } } });
  assert.equal(written.size, file.length);
  assert.deepEqual([...file.subarray(0, 8)], [...backupMagic]);
  const header = await readBackupHeader(output);
  assert.equal(header.createdAt, '2026-11-10T07:30:00.000Z'); assert.equal(header.serverVersion, '1.0.43');
  assert.deepEqual(Object.keys(JSON.parse(file.subarray(12, headerLength(file)).toString('utf8'))), ['createdAt', 'serverVersion', 'kdf', 'cipher', 'payloadSize']);
  // Nothing of the content is readable: not the database, the address, the vault key or the manifest's field names.
  const body = file.subarray(headerLength(file)).toString('latin1');
  for (const text of ['SQLite format 3', 'owner-account-name', 'vdi.fixture.example', 'vaultKey', Buffer.alloc(32, 7).toString('base64'), 'server-access-v1']) assert.equal(body.includes(text), false, text);
  const { result, database } = await reads(directory, file);
  assert.equal(result.manifest.origin, 'https://vdi.fixture.example'); assert.equal(result.manifest.database.size, bytes.length);
  assert.deepEqual(result.manifest.serverAccess, { protocol: 'server-access-v1', primaryOrigin: 'https://vdi.fixture.example' });
  assert.deepEqual(readFileSync(database), bytes);
});

test('the exported file uses the stated key derivation (scrypt N=2^17, r=8, p=1) and a fresh salt and nonce each time', async t => {
  const first = await made(t, { kdf: backupKdf, size: 4000 }), second = await made(t, { kdf: backupKdf, size: 4000 });
  const header = file => JSON.parse(file.subarray(12, headerLength(file)).toString('utf8'));
  assert.deepEqual({ ...header(first.file).kdf, salt: null }, { name: 'scrypt', N: 131072, r: 8, p: 1, salt: null });
  assert.notEqual(header(first.file).kdf.salt, header(second.file).kdf.salt); assert.notEqual(header(first.file).cipher.nonce, header(second.file).cipher.nonce);
  assert.equal(Buffer.from(header(first.file).kdf.salt, 'base64').length, 32);
});

test('passphrase rule: 12 characters or more, the same text in another Unicode form gives the same key', async t => {
  assert.throws(() => backupPassphrase('열한글자열한글자열한글'), code('backup_passphrase_too_short'));
  assert.throws(() => backupPassphrase('short'), code('backup_passphrase_too_short'));
  assert.throws(() => backupPassphrase('x'.repeat(257)), code('backup_passphrase_invalid'));
  assert.throws(() => backupPassphrase(null), code('backup_passphrase_invalid'));
  assert.equal(backupPassphrase('열두글자열두글자열두글자').length > 0, true);
  const { directory, file } = await made(t, { size: 3000 });
  const decomposed = passphrase.normalize('NFD'); assert.notEqual(decomposed, passphrase);
  assert.equal((await reads(directory, file, decomposed)).result.serverVersion, '1.0.43');
});

test('a wrong passphrase reads nothing', async t => {
  const { directory, file } = await made(t, { size: 3000 });
  await assert.rejects(reads(directory, file, passphrase + '!'), code('backup_unreadable'));
  await assert.rejects(reads(directory, file, 'short'), code('backup_passphrase_too_short'));
});

test('a cut, padded or empty file is refused before any key is derived', async t => {
  const { directory, file } = await made(t);
  for (const size of [0, 7, 12, 40, headerLength(file), headerLength(file) + 10, file.length - 1, file.length - 16, file.length - backupChunkSize]) {
    const started = Date.now();
    await assert.rejects(reads(directory, file.subarray(0, size)), code('backup_file_invalid'), 'size ' + size);
    assert.ok(Date.now() - started < 400, 'no key derivation for size ' + size);
  }
  await assert.rejects(reads(directory, Buffer.concat([file, Buffer.alloc(1)])), code('backup_file_invalid'));
  await assert.rejects(reads(directory, Buffer.concat([file, file.subarray(file.length - 100)])), code('backup_file_invalid'));
});

test('every header field is authenticated: a changed one is refused', async t => {
  const { directory, file } = await made(t);
  const magic = Buffer.from(file); magic[1] ^= 1; await assert.rejects(reads(directory, magic), code('backup_file_invalid'));
  const newer = Buffer.from(file); newer.writeUInt16BE(2, 8); await assert.rejects(reads(directory, newer), code('backup_format_newer'));
  const zero = Buffer.from(file); zero.writeUInt16BE(0, 8); await assert.rejects(reads(directory, zero), code('backup_file_invalid'));
  const length = Buffer.from(file); length.writeUInt16BE(file.readUInt16BE(10) - 1, 10); await assert.rejects(reads(directory, length), code('backup_file_invalid'));
  const flip = text => { const bytes = Buffer.from(text, 'base64'); bytes[0] ^= 1; return bytes.toString('base64'); };
  const changes = {
    createdAt: value => { value.createdAt = '2026-11-10T07:30:00.001Z'; },
    serverVersion: value => { value.serverVersion = '1.0.42'; },
    'kdf.salt': value => { value.kdf.salt = flip(value.kdf.salt); },
    'kdf.N': value => { value.kdf.N = 65536; },
    'cipher.nonce': value => { value.cipher.nonce = flip(value.cipher.nonce); },
  };
  // The file stays well-formed, so only the authentication tag can refuse it.
  for (const [name, change] of Object.entries(changes)) await assert.rejects(reads(directory, withHeader(file, change)), code('backup_unreadable'), name);
  const invalid = {
    'kdf.name': value => { value.kdf.name = 'pbkdf2'; }, 'kdf.r': value => { value.kdf.r = 1; }, 'kdf.p': value => { value.kdf.p = 2; },
    'kdf.N small': value => { value.kdf.N = 1024; }, 'kdf.N huge': value => { value.kdf.N = 1048576; }, 'kdf.salt short': value => { value.kdf.salt = 'AAAA'; },
    'cipher.name': value => { value.cipher.name = 'aes-256-ctr'; }, 'cipher.chunkSize': value => { value.cipher.chunkSize = 1024; },
    payloadSize: value => { value.payloadSize -= 1; }, 'payloadSize text': value => { value.payloadSize = String(value.payloadSize); },
    extra: value => { value.note = 'x'; }, missing: value => { delete value.serverVersion; }, createdAtForm: value => { value.createdAt = '2026-11-10'; }, version: value => { value.serverVersion = '1.0'; },
  };
  for (const [name, change] of Object.entries(invalid)) await assert.rejects(reads(directory, withHeader(file, change)), code('backup_file_invalid'), name);
  assert.throws(() => decodeBackupHeader(withHeader(file, value => { value.payloadSize = backupMaxDatabaseBytes * 2; }), 1e12), code('backup_too_large'));
});

test('every frame and every tag is authenticated; frames cannot be swapped, dropped or repeated', async t => {
  const { directory, file } = await made(t), start = headerLength(file), frame = backupChunkSize + 16;
  const frames = Math.ceil((file.length - start) / frame); assert.equal(frames, 3);
  const changed = offset => { const bytes = Buffer.from(file); bytes[offset] ^= 0x80; return bytes; };
  for (let index = 0; index < frames; index++) {
    const begin = start + index * frame, end = Math.min(file.length, begin + frame);
    await assert.rejects(reads(directory, changed(begin + 5)), code('backup_unreadable'), 'ciphertext of frame ' + index);
    await assert.rejects(reads(directory, changed(end - 1)), code('backup_unreadable'), 'tag of frame ' + index);
    await assert.rejects(reads(directory, changed(end - 16)), code('backup_unreadable'), 'tag start of frame ' + index);
  }
  const swapped = Buffer.concat([file.subarray(0, start), file.subarray(start + frame, start + 2 * frame), file.subarray(start, start + frame), file.subarray(start + 2 * frame)]);
  await assert.rejects(reads(directory, swapped), code('backup_unreadable'));
  // A file that ends at a frame boundary, with its header rewritten to match, still fails:
  // the last frame is marked as the last one inside its nonce.
  const dropped = withHeader(file.subarray(0, start + 2 * frame), value => { value.payloadSize = backupChunkSize * 2; });
  await assert.rejects(reads(directory, dropped), code('backup_unreadable'));
});

test('a failed read leaves no usable database behind for the caller and names no content', async t => {
  const { directory, file } = await made(t);
  const bytes = Buffer.from(file); bytes[file.length - 20] ^= 1;
  const path = join(directory, 'bad.mtbk'), database = join(directory, 'bad.sqlite'); writeFileSync(path, bytes);
  const error = await readBackup({ path, passphrase, databasePath: database }).catch(value => value);
  assert.equal(error.code, 'backup_unreadable'); assert.equal(error.message, 'backup_unreadable');
  // The earlier frames were authentic; the file exists but the caller is told to discard it.
  assert.equal(existsSync(database), true);
  await assert.rejects(readBackup({ path, passphrase, databasePath: database }), /EEXIST/, 'an existing output is never overwritten');
});

test('a database over the bound is refused with a clear code before it is read', async t => {
  const directory = temporaryDirectory(t), path = join(directory, 'huge.sqlite'), fd = openSync(path, 'w');
  try { ftruncateSync(fd, backupMaxDatabaseBytes + 1); } finally { closeSync(fd); }
  const started = Date.now();
  await assert.rejects(writeBackup({ passphrase, createdAt: '2026-11-10T07:30:00.000Z', serverVersion: '1.0.43', manifest: manifest(), databasePath: path, outputPath: join(directory, 'out.mtbk'), kdf: fast }), code('backup_too_large'));
  assert.ok(Date.now() - started < 2000); assert.equal(existsSync(join(directory, 'out.mtbk')), false);
});

test('the manifest inside is validated field by field', async t => {
  const directory = temporaryDirectory(t), { path } = sample(directory, 2000);
  const attempt = (name, value) => writeBackup({ passphrase, createdAt: '2026-11-10T07:30:00.000Z', serverVersion: '1.0.43', manifest: value, databasePath: path, outputPath: join(directory, name + '.mtbk'), kdf: fast });
  await assert.rejects(attempt('a', manifest({ vaultKey: 'short' })), code('backup_file_invalid'));
  await assert.rejects(attempt('b', manifest({ origin: 'file:///etc/passwd' })), code('backup_file_invalid'));
  await assert.rejects(attempt('c', manifest({ excluded: ['../x'] })), code('backup_file_invalid'));
  await assert.rejects(attempt('d', { ...manifest(), path: '/var/lib/multitap/broker.sqlite' }), code('backup_file_invalid'), 'no extra field such as a path is accepted');
  await assert.rejects(attempt('e', manifest({ deploymentKind: 'windows' })), code('backup_file_invalid'));
});

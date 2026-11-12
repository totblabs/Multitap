// The backup file (백업·복원). One file, readable only with the passphrase the
// administrator typed at export. node:crypto only.
//
//   offset  size  field
//   0       8     magic  89 4D 54 42 4B 0D 0A 1A
//   8       2     format version, uint16 BE (1)
//   10      2     header length H, uint16 BE
//   12      H     header, JSON (UTF-8): createdAt, serverVersion, kdf, cipher, payloadSize
//   12+H    ...   frames: ciphertext (chunkSize bytes, the last one 1..chunkSize) + 16-byte tag
//
// Key: scrypt(passphrase NFKC, salt) -> 32 bytes. Every frame is AES-256-GCM
// with the nonce  prefix(7) | frame index uint32 BE | last flag(1)  and, as
// additional data, the SHA-256 of bytes 0..12+H. So the header is authenticated
// by every frame, frames cannot be reordered, dropped or appended, and nothing
// is released from a frame whose tag does not verify. The header's fields are
// the only readable part: when it was made, the server version, the KDF and
// cipher parameters and the payload size (which the file size shows anyway).
//
// Payload (inside the encryption): uint32 BE manifest length, the manifest
// JSON, then the database bytes. There are no file names or paths in the file;
// the reader writes the database to a path of its own choosing. Nothing is
// compressed, so nothing can expand.
import { createCipheriv, createDecipheriv, createHash, randomBytes, scrypt } from 'node:crypto';
import { open, stat } from 'node:fs/promises';
import { constants } from 'node:fs';

export const backupMagic = Buffer.from([0x89, 0x4d, 0x54, 0x42, 0x4b, 0x0d, 0x0a, 0x1a]);
export const backupFormatVersion = 1;
export const backupChunkSize = 65536;
export const backupKdf = Object.freeze({ name: 'scrypt', N: 131072, r: 8, p: 1 });
// The database inside one file. A larger one is refused at export, with the
// same bound on what a restore reads.
export const backupMaxDatabaseBytes = 256 * 1024 * 1024;
export const backupMaxManifestBytes = 65536;
const maxHeaderBytes = 1024, tagBytes = 16, prefixBytes = 12;
export const backupMaxFileBytes = prefixBytes + maxHeaderBytes + 4 + backupMaxManifestBytes + backupMaxDatabaseBytes + tagBytes * (Math.ceil((4 + backupMaxManifestBytes + backupMaxDatabaseBytes) / backupChunkSize) + 1);
export const backupPassphraseMinimum = 12, backupPassphraseMaximum = 256;

export class BackupError extends Error {
  constructor(code) { super(code); this.code = code; }
}
const fail = code => { throw new BackupError(code); };
const exact = (value, keys) => value !== null && typeof value === 'object' && !Array.isArray(value) && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
const isoTime = value => typeof value === 'string' && /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$/.test(value) && Number.isFinite(Date.parse(value)) && new Date(value).toISOString() === value;
export const backupVersionPattern = /^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$/;

// The rule the console states: 12 characters or more. The same text typed on
// another keyboard or system (composed or decomposed Hangul) gives the same key.
export function backupPassphrase(value) {
  if (typeof value !== 'string') fail('backup_passphrase_invalid');
  const text = value.normalize('NFKC');
  const length = [...text].length;
  if (length < backupPassphraseMinimum) fail('backup_passphrase_too_short');
  if (length > backupPassphraseMaximum || /[\x00-\x08\x0a-\x1f\x7f]/.test(text)) fail('backup_passphrase_invalid');
  return Buffer.from(text, 'utf8');
}

let derivations = 0;
// One derivation at a time: each takes about 134 MB while it runs.
async function deriveKey(passphrase, salt, { N, r, p }) {
  const secret = backupPassphrase(passphrase);
  if (derivations >= 1) { secret.fill(0); fail('backup_busy'); }
  derivations++;
  try {
    return await new Promise((resolve, reject) => scrypt(secret, salt, 32, { N, r, p, maxmem: 128 * N * r + 32 * 1024 * 1024 }, (error, key) => error ? reject(new BackupError('backup_unavailable')) : resolve(key)));
  } finally { secret.fill(0); derivations--; }
}

function nonce(prefix, index, last) {
  const value = Buffer.alloc(12); prefix.copy(value, 0); value.writeUInt32BE(index, 7); value[11] = last ? 1 : 0;
  return value;
}
const frameCount = payloadSize => Math.ceil(payloadSize / backupChunkSize);

function encodeHeader({ createdAt, serverVersion, salt, prefix, payloadSize, kdf }) {
  const json = Buffer.from(JSON.stringify({ createdAt, serverVersion, kdf: { name: 'scrypt', N: kdf.N, r: kdf.r, p: kdf.p, salt: salt.toString('base64') }, cipher: { name: 'aes-256-gcm', chunkSize: backupChunkSize, nonce: prefix.toString('base64') }, payloadSize }), 'utf8');
  if (json.length > maxHeaderBytes) fail('backup_unavailable');
  const head = Buffer.alloc(prefixBytes); backupMagic.copy(head, 0); head.writeUInt16BE(backupFormatVersion, 8); head.writeUInt16BE(json.length, 10);
  return Buffer.concat([head, json]);
}

// Checks everything that needs no passphrase and returns the header's public
// fields. `fileSize` must be exactly what the header describes, so a cut or
// padded file is refused before any key is derived.
export function decodeBackupHeader(bytes, fileSize) {
  if (!Buffer.isBuffer(bytes) || bytes.length < prefixBytes || !bytes.subarray(0, 8).equals(backupMagic)) fail('backup_file_invalid');
  const version = bytes.readUInt16BE(8), length = bytes.readUInt16BE(10);
  if (version > backupFormatVersion) fail('backup_format_newer');
  if (version !== backupFormatVersion || length < 2 || length > maxHeaderBytes || bytes.length < prefixBytes + length) fail('backup_file_invalid');
  let value;
  try { value = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes.subarray(prefixBytes, prefixBytes + length))); } catch { fail('backup_file_invalid'); }
  if (!exact(value, ['createdAt', 'serverVersion', 'kdf', 'cipher', 'payloadSize']) || !isoTime(value.createdAt) || typeof value.serverVersion !== 'string' || !backupVersionPattern.test(value.serverVersion)) fail('backup_file_invalid');
  const kdf = value.kdf, cipher = value.cipher;
  // Parameters outside this range are not made by any server and would only
  // make this one spend memory: N between 2^15 and 2^18, r 8, p 1.
  if (!exact(kdf, ['name', 'N', 'r', 'p', 'salt']) || kdf.name !== 'scrypt' || ![32768, 65536, 131072, 262144].includes(kdf.N) || kdf.r !== 8 || kdf.p !== 1 || typeof kdf.salt !== 'string') fail('backup_file_invalid');
  if (!exact(cipher, ['name', 'chunkSize', 'nonce']) || cipher.name !== 'aes-256-gcm' || cipher.chunkSize !== backupChunkSize || typeof cipher.nonce !== 'string') fail('backup_file_invalid');
  const salt = Buffer.from(kdf.salt, 'base64'), prefix = Buffer.from(cipher.nonce, 'base64');
  if (salt.length !== 32 || salt.toString('base64') !== kdf.salt || prefix.length !== 7 || prefix.toString('base64') !== cipher.nonce) fail('backup_file_invalid');
  const payloadSize = value.payloadSize;
  if (!Number.isSafeInteger(payloadSize) || payloadSize < 5 || payloadSize > 4 + backupMaxManifestBytes + backupMaxDatabaseBytes) fail(Number.isSafeInteger(payloadSize) && payloadSize > 0 ? 'backup_too_large' : 'backup_file_invalid');
  const headerLength = prefixBytes + length;
  if (fileSize !== headerLength + payloadSize + tagBytes * frameCount(payloadSize)) fail('backup_file_invalid');
  return { createdAt: value.createdAt, serverVersion: value.serverVersion, kdf: { N: kdf.N, r: kdf.r, p: kdf.p }, salt, prefix, payloadSize, headerLength, aad: createHash('sha256').update(bytes.subarray(0, headerLength)).digest() };
}

async function readExactly(file, length, position) {
  const buffer = Buffer.allocUnsafe(length); let done = 0;
  while (done < length) {
    const { bytesRead } = await file.read(buffer, done, length - done, position + done);
    if (!bytesRead) fail('backup_file_invalid');
    done += bytesRead;
  }
  return buffer;
}

// The public header of a file on disk; no passphrase, nothing decrypted.
export async function readBackupHeader(path) {
  const file = await open(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
  try {
    const info = await file.stat();
    if (!info.isFile() || info.size > backupMaxFileBytes) fail(info.isFile() ? 'backup_too_large' : 'backup_file_invalid');
    if (info.size < prefixBytes + 2 + 5 + tagBytes) fail('backup_file_invalid');
    const bytes = await readExactly(file, Math.min(info.size, prefixBytes + maxHeaderBytes), 0);
    return decodeBackupHeader(bytes, info.size);
  } finally { await file.close(); }
}

export function validateManifest(manifest) {
  if (!exact(manifest, ['schema', 'origin', 'deploymentKind', 'database', 'vaultKey', 'serverAccess', 'excluded']) || manifest.schema !== 1) fail('backup_file_invalid');
  if (!(manifest.origin === null || (typeof manifest.origin === 'string' && manifest.origin.length <= 512 && /^https?:\/\/[^\s\x00-\x20\x7f\\@?#%]+$/.test(manifest.origin)))) fail('backup_file_invalid');
  if (!['unknown', 'docker', 'systemd', 'external'].includes(manifest.deploymentKind)) fail('backup_file_invalid');
  const database = manifest.database;
  if (!exact(database, ['size', 'sha256', 'schemaVersion']) || !Number.isSafeInteger(database.size) || database.size < 512 || database.size > backupMaxDatabaseBytes ||
      typeof database.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(database.sha256) || !Number.isSafeInteger(database.schemaVersion) || database.schemaVersion < 1 || database.schemaVersion > 100000) fail('backup_file_invalid');
  if (typeof manifest.vaultKey !== 'string' || Buffer.from(manifest.vaultKey, 'base64').length !== 32 || Buffer.from(manifest.vaultKey, 'base64').toString('base64') !== manifest.vaultKey) fail('backup_file_invalid');
  if (!(manifest.serverAccess === null || (typeof manifest.serverAccess === 'object' && !Array.isArray(manifest.serverAccess)))) fail('backup_file_invalid');
  if (!Array.isArray(manifest.excluded) || manifest.excluded.length > 64 || manifest.excluded.some(name => typeof name !== 'string' || !/^[a-z_.]{1,64}$/.test(name))) fail('backup_file_invalid');
  return manifest;
}

async function fileDigest(path) {
  const file = await open(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0)), hash = createHash('sha256');
  try {
    const info = await file.stat(); if (!info.isFile()) fail('backup_unavailable');
    const buffer = Buffer.allocUnsafe(backupChunkSize); let position = 0;
    while (position < info.size) { const { bytesRead } = await file.read(buffer, 0, buffer.length, position); if (!bytesRead) fail('backup_unavailable'); hash.update(buffer.subarray(0, bytesRead)); position += bytesRead; }
    return { size: info.size, sha256: hash.digest('hex') };
  } finally { await file.close(); }
}

// Writes the encrypted file. The database is read from `databasePath` one
// frame at a time and never held in memory. `manifest` carries everything but
// the database's size and hash, which are measured here.
export async function writeBackup({ passphrase, createdAt, serverVersion, manifest, databasePath, outputPath, random = randomBytes, kdf = backupKdf }) {
  if (!isoTime(createdAt) || !backupVersionPattern.test(serverVersion ?? '')) fail('backup_unavailable');
  if ((await stat(databasePath)).size > backupMaxDatabaseBytes) fail('backup_too_large');
  const measured = await fileDigest(databasePath);
  if (measured.size > backupMaxDatabaseBytes) fail('backup_too_large');
  const full = validateManifest({ ...manifest, database: { size: measured.size, sha256: measured.sha256, schemaVersion: manifest.database.schemaVersion } });
  const manifestBytes = Buffer.from(JSON.stringify(full), 'utf8');
  if (manifestBytes.length > backupMaxManifestBytes) fail('backup_unavailable');
  const lead = Buffer.alloc(4); lead.writeUInt32BE(manifestBytes.length, 0);
  const payloadSize = 4 + manifestBytes.length + measured.size, frames = frameCount(payloadSize);
  const salt = random(32), prefix = random(7), header = encodeHeader({ createdAt, serverVersion, salt, prefix, payloadSize, kdf });
  const aad = createHash('sha256').update(header).digest();
  const key = await deriveKey(passphrase, salt, kdf);
  let source, target;
  try {
    source = await open(databasePath, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    target = await open(outputPath, 'wx', 0o600);
    await target.write(header);
    let pending = Buffer.concat([lead, manifestBytes]), position = 0, index = 0, written = header.length;
    const hash = createHash('sha256');
    while (index < frames) {
      while (pending.length < backupChunkSize && position < measured.size) {
        const buffer = Buffer.allocUnsafe(Math.min(backupChunkSize, measured.size - position));
        const { bytesRead } = await source.read(buffer, 0, buffer.length, position);
        if (!bytesRead) fail('backup_unavailable');
        hash.update(buffer.subarray(0, bytesRead)); position += bytesRead;
        pending = Buffer.concat([pending, buffer.subarray(0, bytesRead)]);
      }
      const plain = pending.subarray(0, backupChunkSize); pending = pending.subarray(plain.length);
      const last = index === frames - 1;
      if (!plain.length || (last && (pending.length || position !== measured.size))) fail('backup_unavailable');
      const cipher = createCipheriv('aes-256-gcm', key, nonce(prefix, index, last)); cipher.setAAD(aad);
      const frame = Buffer.concat([cipher.update(plain), cipher.final(), cipher.getAuthTag()]);
      await target.write(frame); written += frame.length; index++;
    }
    // The snapshot is this process's private file; a change underneath would
    // be a fault here, and the hash in the manifest would no longer be true.
    if (hash.digest('hex') !== measured.sha256) fail('backup_unavailable');
    await target.sync();
    return { size: written, createdAt, serverVersion, databaseBytes: measured.size };
  } finally { key.fill(0); await source?.close(); await target?.close(); }
}

// Decrypts `path` with the passphrase: the manifest is returned and the
// database is written to `databasePath` (a new file). A wrong passphrase and a
// changed file look the same (backup_unreadable): the first frame's tag fails.
// On any failure the partly written database file is the caller's to delete.
export async function readBackup({ path, passphrase, databasePath }) {
  const header = await readBackupHeader(path);
  const key = await deriveKey(passphrase, header.salt, header.kdf);
  let source, target;
  try {
    source = await open(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    const info = await source.stat();
    if (info.size !== header.headerLength + header.payloadSize + tagBytes * frameCount(header.payloadSize)) fail('backup_file_invalid');
    target = await open(databasePath, 'wx', 0o600);
    const frames = frameCount(header.payloadSize), hash = createHash('sha256');
    let position = header.headerLength, remaining = header.payloadSize, head = Buffer.alloc(0), manifest = null, databaseBytes = 0;
    for (let index = 0; index < frames; index++) {
      const size = Math.min(backupChunkSize, remaining), frame = await readExactly(source, size + tagBytes, position);
      position += frame.length; remaining -= size;
      const decipher = createDecipheriv('aes-256-gcm', key, nonce(header.prefix, index, index === frames - 1));
      decipher.setAAD(header.aad); decipher.setAuthTag(frame.subarray(size));
      let plain;
      try { plain = Buffer.concat([decipher.update(frame.subarray(0, size)), decipher.final()]); } catch { fail('backup_unreadable'); }
      if (!manifest) {
        head = Buffer.concat([head, plain]);
        if (head.length < 4) continue;
        const length = head.readUInt32BE(0);
        if (length < 2 || length > backupMaxManifestBytes) fail('backup_file_invalid');
        if (head.length < 4 + length) continue;
        try { manifest = validateManifest(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(head.subarray(4, 4 + length)))); }
        catch (error) { if (error instanceof BackupError) throw error; fail('backup_file_invalid'); }
        if (header.payloadSize !== 4 + length + manifest.database.size) fail('backup_file_invalid');
        plain = head.subarray(4 + length); head = null;
      }
      if (plain.length) { hash.update(plain); databaseBytes += plain.length; if (databaseBytes > manifest.database.size) fail('backup_file_invalid'); await target.write(plain); }
    }
    if (!manifest || databaseBytes !== manifest.database.size || hash.digest('hex') !== manifest.database.sha256) fail('backup_file_invalid');
    await target.sync();
    return { createdAt: header.createdAt, serverVersion: header.serverVersion, manifest };
  } finally { key.fill(0); await source?.close(); await target?.close(); }
}

export async function backupFileSize(path) { return (await stat(path)).size; }

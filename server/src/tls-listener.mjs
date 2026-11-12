import { open, lstat } from 'node:fs/promises';
import { constants } from 'node:fs';
import { createPrivateKey, X509Certificate } from 'node:crypto';
import { createSecureContext } from 'node:tls';

async function protectedPem(path, maximum) {
  const before = await lstat(path);
  if (!before.isFile() || before.isSymbolicLink() || before.size < 1 || before.size > maximum || (process.platform !== 'win32' && ((before.mode & 0o022) !== 0 || ![0, process.getuid()].includes(before.uid)))) throw Error('invalid_tls_credentials');
  const file = await open(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
  let buffer, accepted = false;
  try {
    const held = await file.stat();
    if (held.ino !== before.ino || held.dev !== before.dev || held.size !== before.size) throw Error('invalid_tls_credentials');
    buffer = Buffer.alloc(maximum + 1); let used = 0;
    for (;;) {
      const { bytesRead } = await file.read(buffer, used, buffer.length - used, null);
      if (!bytesRead) break;
      used += bytesRead;
      if (used > maximum) { buffer.fill(0); throw Error('invalid_tls_credentials'); }
    }
    const bytes = buffer.subarray(0, used);
    const after = await file.stat();
    if (bytes.length !== held.size || after.size !== held.size || after.mtimeMs !== held.mtimeMs || after.ctimeMs !== held.ctimeMs) throw Error('invalid_tls_credentials');
    accepted = true;
    return bytes;
  } finally {
    if (!accepted) buffer?.fill(0);
    try { await file.close(); } catch (error) { buffer?.fill(0); throw error; }
  }
}

// Explicit startup only. Importing this module never reads a certificate/key.
// No self-signed certificate generation, automatic renewal or trust changes.
export async function loadTlsListener(files) {
  if (files === null) return null;
  let key, transferred = false;
  try {
    const cert = await protectedPem(files.certFile, 256 * 1024);
    key = await protectedPem(files.keyFile, 64 * 1024);
    const leaf = new X509Certificate(cert), privateKey = createPrivateKey(key);
    if (!leaf.checkPrivateKey(privateKey)) throw Error('invalid_tls_credentials');
    // TLS consumes PEM bytes; the KeyObject above is only the matching check.
    // The startup caller wipes these bytes after the listener copies them.
    const options = { cert, key, minVersion: 'TLSv1.2' };
    createSecureContext(options); // Validate the complete chain/key options before bind.
    transferred = true;
    return options;
  } catch { throw Error('invalid_tls_credentials'); }
  finally { if (!transferred) key?.fill(0); }
}

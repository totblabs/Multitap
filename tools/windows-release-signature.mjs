// Windows client/Host release signatures (multitap-windows-release-signature-v1).
//
// The project's own Ed25519 key signs one canonical message per installer:
// product, version, file name, exact size and SHA-256 under a fixed domain
// line. The private key lives only on the release PC in
// .local/release-signing (never printed, never committed). Its public half is
// pinned in native/windows_admin/release_signature.hpp; client and Host
// builds verify against that pinned key before any installer starts. This is
// not Authenticode, a purchased certificate or an external signing service.
import { createPrivateKey, createPublicKey, generateKeyPairSync, sign, verify } from 'node:crypto';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

export const signatureDomain = 'multitap-windows-release-signature-v1';
export const privateKeyName = 'windows-release-private.pem';
export const publicKeyName = 'windows-release-public.pem';
const version = /^[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}$/;
const signaturePattern = /^[A-Za-z0-9+/]{86}==$/;
const pinnedHeader = resolve(dirname(fileURLToPath(import.meta.url)), '../native/windows_admin/release_signature.hpp');

// The exact bytes that are signed. Every field is validated first, so no
// value can contain a line break or change another line's meaning.
export function releaseMessage({ product, version: value, filename, size, sha256 }) {
  if (!['client', 'host'].includes(product) || typeof value !== 'string' || !version.test(value) ||
    filename !== `Multitap${product === 'host' ? '-Host' : ''}-${value}-windows-x64.exe` ||
    !Number.isSafeInteger(size) || size < 1 || size > 128 * 1024 * 1024 ||
    typeof sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(sha256)) throw Error('release_signature_input_invalid');
  return Buffer.from(`${signatureDomain}\nproduct=${product}\nversion=${value}\nfilename=${filename}\nsize=${size}\nsha256=${sha256}\n`, 'ascii');
}

// Canonical base64 of exactly 64 bytes (no alternative spellings).
export function validSignatureText(text) {
  return typeof text === 'string' && signaturePattern.test(text) && Buffer.from(text, 'base64').toString('base64') === text;
}

function ed25519(key, kind) {
  // A KeyObject of the wanted type is used as is (createPublicKey refuses a
  // public KeyObject); PEM/DER bytes and private keys are converted.
  const value = key?.type === kind ? key : kind === 'private' ? createPrivateKey(key) : createPublicKey(key);
  if (value.asymmetricKeyType !== 'ed25519') throw Error('release_signature_key_invalid');
  return value;
}

export function rawPublicKey(publicKey) {
  const der = ed25519(publicKey, 'public').export({ type: 'spki', format: 'der' });
  if (der.length !== 44) throw Error('release_signature_key_invalid');
  return der.subarray(12);
}

export function signRelease(entry, privateKey) {
  return sign(null, releaseMessage(entry), ed25519(privateKey, 'private')).toString('base64');
}

export function verifyRelease(entry, signature, publicKey) {
  if (!validSignatureText(signature)) return false;
  let message;
  try { message = releaseMessage(entry); } catch { return false; }
  return verify(null, message, ed25519(publicKey, 'public'), Buffer.from(signature, 'base64'));
}

// The 32-byte key compiled into client and Host builds.
export async function pinnedPublicKey(header = pinnedHeader) {
  const text = await readFile(header, 'utf8');
  const match = /kReleasePublicKeyHex(?:\[\])?\s*=\s*"([a-f0-9]{64})"/.exec(text);
  if (!match) throw Error('release_signature_pin_missing');
  return Buffer.from(match[1], 'hex');
}

// Once per release PC; an existing pair is preserved and checked, never replaced.
export async function createReleaseKey(directory) {
  await mkdir(directory, { recursive: true, mode: 0o700 });
  const privatePath = join(directory, privateKeyName), publicPath = join(directory, publicKeyName);
  let existing;
  try { existing = await readFile(privatePath); } catch (error) { if (error.code !== 'ENOENT') throw error; }
  if (existing) {
    if (createPublicKey(existing).export({ type: 'spki', format: 'pem' }) !== await readFile(publicPath, 'utf8')) throw Error('Existing release signing key pair mismatch');
    return { created: false, publicKey: rawPublicKey(await readFile(publicPath)).toString('hex') };
  }
  const pair = generateKeyPairSync('ed25519');
  await writeFile(privatePath, pair.privateKey.export({ type: 'pkcs8', format: 'pem' }), { flag: 'wx', mode: 0o600 });
  await writeFile(publicPath, pair.publicKey.export({ type: 'spki', format: 'pem' }), { flag: 'wx', mode: 0o644 });
  return { created: true, publicKey: rawPublicKey(pair.publicKey).toString('hex') };
}

// Release tooling entry: sign with the private key only when its public half
// is the pinned key, then verify the result before printing it.
export async function signForPublication(privateKeyPath, entry, header = pinnedHeader) {
  const privateKey = createPrivateKey(await readFile(privateKeyPath));
  const publicKey = createPublicKey(privateKey);
  if (!rawPublicKey(publicKey).equals(await pinnedPublicKey(header))) throw Error('Release signing key differs from the key pinned in client and Host builds');
  const signature = signRelease(entry, privateKey);
  if (!verifyRelease(entry, signature, publicKey)) throw Error('Release signature self-check failed');
  return signature;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try {
    const [action, ...args] = process.argv.slice(2);
    const entry = ([product, value, filename, size, sha256]) => ({ product, version: value, filename, size: /^[0-9]{1,9}$/.test(size ?? '') ? Number(size) : NaN, sha256 });
    if (action === 'create-key' && args.length === 1) {
      const result = await createReleaseKey(resolve(args[0]));
      console.log(JSON.stringify({ created: result.created, publicKey: result.publicKey, privateKeyPrinted: false }));
    } else if (action === 'sign' && args.length === 6) {
      console.log(JSON.stringify({ signature: await signForPublication(resolve(args[0]), entry(args.slice(1))) }));
    } else if (action === 'verify' && args.length === 7) {
      const ok = verifyRelease(entry(args.slice(1, 6)), args[6], await readFile(resolve(args[0])));
      console.log(JSON.stringify({ verified: ok }));
      if (!ok) process.exitCode = 1;
    } else if (action === 'public-raw' && args.length === 1) {
      console.log(rawPublicKey(await readFile(resolve(args[0]))).toString('hex'));
    } else if (action === 'pinned-key' && args.length === 0) {
      console.log((await pinnedPublicKey()).toString('hex'));
    } else throw Error('Expected create-key <dir> | sign <private.pem> <product> <version> <filename> <size> <sha256> | verify <public.pem> <product> <version> <filename> <size> <sha256> <signature> | public-raw <public.pem> | pinned-key');
  } catch (error) { console.error('Multitap release signature: ' + error.message); process.exitCode = 1; }
}

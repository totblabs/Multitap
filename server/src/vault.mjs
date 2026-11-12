import { randomBytes, createCipheriv, createDecipheriv, createHmac, timingSafeEqual } from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync, chmodSync } from 'node:fs';
import { dirname } from 'node:path';

export class Vault {
  constructor(key) { if (key.length !== 32) throw new Error('invalid_vault_key'); this.key = key; }
  static open(path) {
    mkdirSync(dirname(path), { recursive: true, mode: 0o700 });
    try { writeFileSync(path, randomBytes(32), { flag: 'wx', mode: 0o600 }); }
    catch (error) { if (error.code !== 'EEXIST') throw error; }
    chmodSync(path, 0o600);
    return new Vault(readFileSync(path));
  }
  seal(value) {
    const iv = randomBytes(12); const cipher = createCipheriv('aes-256-gcm', this.key, iv);
    cipher.setAAD(Buffer.from('multitap-proxmox-v1'));
    const encrypted = Buffer.concat([cipher.update(JSON.stringify(value), 'utf8'), cipher.final()]);
    return Buffer.concat([iv, cipher.getAuthTag(), encrypted]).toString('base64');
  }
  // iv (12) | tag (16) | ciphertext, as seal() writes it; the format and key
  // are unchanged. A shorter value, or a tag of any other length, is refused.
  open(value) {
    const bytes = Buffer.from(value, 'base64');
    if (bytes.length < 12 + 16) throw new Error('invalid_sealed_value');
    const cipher = createDecipheriv('aes-256-gcm', this.key, bytes.subarray(0, 12), { authTagLength: 16 });
    cipher.setAAD(Buffer.from('multitap-proxmox-v1')); cipher.setAuthTag(bytes.subarray(12, 28));
    return JSON.parse(Buffer.concat([cipher.update(bytes.subarray(28)), cipher.final()]).toString('utf8'));
  }
  csrf(token) { return createHmac('sha256', this.key).update('admin-csrf:' + token).digest('base64url'); }
  verifyCsrf(token, value) {
    if (typeof value !== 'string' || !/^[A-Za-z0-9_-]{43}$/.test(value)) return false;
    return timingSafeEqual(Buffer.from(this.csrf(token)), Buffer.from(value));
  }
}

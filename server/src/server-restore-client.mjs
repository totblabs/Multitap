import http from 'node:http';
import { HttpError } from './errors.mjs';

// 백업·복원: the broker's side of the local control socket (update/agent.mjs).
// It sends an operation id, the hashes of the two files it staged and the
// schema version: no path, no file content, no passphrase. An apply is never
// sent twice; the operation id it chose beforehand reconciles the status.
export class ServerRestoreClient {
  constructor(socketPath) {
    if (typeof socketPath !== 'string' || !/^\/[A-Za-z0-9_./-]{1,180}$/.test(socketPath) || socketPath.split('/').includes('..')) throw Error('invalid_update_socket');
    this.socketPath = socketPath;
  }
  request(path, input, { timeoutMs = 4000 } = {}) {
    if (!['/restore/status', '/restore/apply'].includes(path)) throw Error('restore_command_invalid');
    return new Promise((resolve, reject) => {
      const payload = input === undefined ? null : Buffer.from(JSON.stringify(input));
      if (payload?.length > 4096) return reject(new HttpError(400, 'restore_command_invalid'));
      let bytes = 0; const chunks = [];
      const unavailable = () => reject(new HttpError(503, 'restore_unavailable'));
      const request = http.request({ socketPath: this.socketPath, path, method: payload ? 'POST' : 'GET', timeout: timeoutMs, headers: payload ? { 'Content-Type': 'application/json', 'Content-Length': payload.length } : {} }, response => {
        response.on('data', chunk => { bytes += chunk.length; if (bytes > 16384) { request.destroy(); unavailable(); } else chunks.push(chunk); });
        response.once('error', unavailable); response.once('aborted', unavailable);
        response.once('end', () => {
          try {
            const value = JSON.parse(Buffer.concat(chunks).toString('utf8'));
            // An updater from before this feature answers 404 with its own code.
            if (![200, 202].includes(response.statusCode)) throw new HttpError([400, 409].includes(response.statusCode) ? response.statusCode : 503, /^restore_[a-z_]{1,80}$/.test(value?.error) ? value.error : 'restore_unavailable');
            resolve(value);
          } catch (error) { reject(error instanceof HttpError ? error : new HttpError(503, 'restore_unavailable')); }
        });
      });
      const timer = setTimeout(() => request.destroy(), timeoutMs); timer.unref?.();
      request.once('close', () => clearTimeout(timer)); request.once('error', unavailable); request.once('timeout', () => request.destroy()); request.end(payload);
    });
  }
  async status() {
    const value = await this.request('/restore/status');
    if (value?.protocol !== 'server-restore-v1' || value.supported !== true || typeof value.state !== 'string' || !/^[a-z_]{1,40}$/.test(value.state)) throw new HttpError(503, 'restore_unavailable');
    return value;
  }
  apply(input, context) { return this.request('/restore/apply', { input, context }, { timeoutMs: 10000 }); }
}

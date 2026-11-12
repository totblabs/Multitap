import http from 'node:http';
import { HttpError } from './errors.mjs';
import { serverAccessProtocol } from '../update/server-access-policy.mjs';

// The broker can reach only its installed local control socket. No retry of a
// save is permitted: the caller's preissued operationId reconciles status.
export class ServerAccessClient {
  constructor(socketPath) {
    if (typeof socketPath !== 'string' || !/^\/[A-Za-z0-9_./-]{1,180}$/.test(socketPath) || socketPath.split('/').includes('..')) throw Error('invalid_update_socket');
    this.socketPath = socketPath;
  }
  request(path, input, { timeoutMs = 4000 } = {}) {
    if (!['/access/status', '/access/test', '/access/save', '/access/cancel'].includes(path)) throw Error('server_access_command_invalid');
    return new Promise((resolve, reject) => {
      const payload = input === undefined ? null : Buffer.from(JSON.stringify(input));
      if (payload?.length > 16384) return reject(new HttpError(400, 'server_access_command_invalid'));
      let bytes = 0; const chunks = [];
      const unavailable = () => reject(new HttpError(503, 'server_access_unavailable'));
      const request = http.request({ socketPath: this.socketPath, path, method: payload ? 'POST' : 'GET', timeout: timeoutMs, headers: payload ? { 'Content-Type': 'application/json', 'Content-Length': payload.length } : {} }, response => {
        response.on('data', chunk => { bytes += chunk.length; if (bytes > 32768) { request.destroy(); unavailable(); } else chunks.push(chunk); });
        response.once('error', unavailable); response.once('aborted', unavailable);
        response.once('end', () => {
          try {
            const value = JSON.parse(Buffer.concat(chunks).toString('utf8'));
            if (![200, 202].includes(response.statusCode)) throw new HttpError([400, 409].includes(response.statusCode) ? response.statusCode : 503, /^server_access_[a-z_]{1,80}$/.test(value?.error) ? value.error : 'server_access_unavailable');
            resolve(value);
          } catch (error) { reject(error instanceof HttpError ? error : new HttpError(503, 'server_access_unavailable')); }
        });
      });
      const timer = setTimeout(() => request.destroy(), timeoutMs); timer.unref?.();
      request.once('close', () => clearTimeout(timer)); request.once('error', unavailable); request.once('timeout', () => request.destroy()); request.end(payload);
    });
  }
  async status() {
    const value = await this.request('/access/status');
    if (value?.protocol !== serverAccessProtocol || typeof value.managed !== 'boolean' || typeof value.applySupported !== 'boolean' || typeof value.state !== 'string' || typeof value.revision !== 'string' || !Object.hasOwn(value, 'config') || !Object.hasOwn(value, 'pendingConfig')) throw new HttpError(503, 'server_access_unavailable');
    return value;
  }
  test(input, context) { return this.request('/access/test', { input, context }, { timeoutMs: 15000 }); }
  save(input, context) { return this.request('/access/save', { input, context }); }
  // Answers once the engine has proved the installation unchanged.
  cancel(input) { return this.request('/access/cancel', { input }, { timeoutMs: 10000 }); }
}

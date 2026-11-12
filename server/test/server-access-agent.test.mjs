import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { listenUpdateAgent } from '../update/agent.mjs';
import { ServerAccessClient } from '../src/server-access-client.mjs';
import { ServerUpdateClient } from '../src/server-update.mjs';

test('actual Unix control socket keeps trusted context separate, returns known operation and excludes concurrent updates', { skip: process.platform !== 'linux' ? 'Linux Unix-socket boundary; not a Windows pass' : false }, async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-access-uds-')), socket = join(directory, 'control.sock'), calls = [];
  t.after(() => rm(directory, { recursive: true, force: true }));
  const operationId = '12345678-1234-4234-8234-123456789abc', context = { managementOrigin: 'https://alpha.example', fence: { token: 'e'.repeat(64), operationId } };
  const access = { busy: false, state: { state: 'idle' }, status: async () => ({ protocol: 'server-access-v1', managed: true, applySupported: true, deploymentKind: 'systemd', revision: 'a'.repeat(64), config: null, pendingConfig: null, state: 'idle' }), test: async (input, ctx) => { calls.push({ input, ctx }); return { testId: 'b'.repeat(64), scope: 'preflight' }; }, save: async (input, ctx) => { calls.push({ input, ctx }); access.busy = true; return { accepted: true, operationId: ctx.fence.operationId, state: 'waiting_idle' }; },
    // A cancel names only its operation; the engine's refusals are conflicts.
    cancel: async input => { calls.push({ cancel: input }); if (input.operationId !== operationId) throw Error('server_access_cancel_unavailable'); if (access.late) throw Error('server_access_cancel_too_late'); return { accepted: true, operationId, state: 'failed', cancelled: true }; } };
  const updates = { status: async () => ({ state: 'idle', feedConfigured: true, applySupported: true }), check: () => assert.fail('an access operation blocks new update work') };
  const server = await listenUpdateAgent(updates, socket, { accessEngine: access }); t.after(() => new Promise(resolve => server.close(resolve)));
  const client = new ServerAccessClient(socket); assert.equal((await client.status()).config, null);
  await client.test({ config: { only: 'fixture' }, baseRevision: 'a'.repeat(64) }, { managementOrigin: context.managementOrigin });
  const input = { config: { only: 'fixture' }, baseRevision: 'a'.repeat(64), testId: 'b'.repeat(64), confirm: true };
  const accepted = await client.save(input, context); assert.equal(accepted.operationId, operationId); assert.deepEqual(calls[1], { input, ctx: context });
  assert.equal(JSON.stringify(await client.status()).includes(context.fence.token), false);
  await assert.rejects(new ServerUpdateClient(socket).check(), error => error.code === 'update_busy');
  await assert.rejects(client.cancel({ operationId: '22345678-1234-4234-8234-123456789abc' }), error => error.status === 409 && error.code === 'server_access_cancel_unavailable');
  access.late = true; await assert.rejects(client.cancel({ operationId }), error => error.status === 409 && error.code === 'server_access_cancel_too_late');
  access.late = false; assert.deepEqual(await client.cancel({ operationId }), { accepted: true, operationId, state: 'failed', cancelled: true });
  assert.deepEqual(calls.at(-1), { cancel: { operationId } }, 'the cancel carries its operation and nothing else');
  await assert.rejects(client.request('/access/cancel', { input: { operationId }, context: {} }), error => error.status === 400 && error.code === 'server_access_command_invalid');
});

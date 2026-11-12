import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { VERSION } from '../src/http.mjs';

test('health and session version agrees with the packaged broker release', async () => {
  const release = JSON.parse(await readFile(new URL('../package.json', import.meta.url), 'utf8'));
  assert.match(VERSION, /^\d+\.\d+\.\d+$/);
  assert.equal(VERSION, release.version);
  const lock = JSON.parse(await readFile(new URL('../package-lock.json', import.meta.url), 'utf8'));
  assert.equal(lock.version, release.version);
  assert.equal(lock.packages[''].version, release.version);
  const docker=await readFile(new URL('../Dockerfile',import.meta.url),'utf8'),compose=await readFile(new URL('../../compose.yaml',import.meta.url),'utf8');
  assert.ok(docker.includes('ARG MULTITAP_VERSION='+release.version));assert.ok(compose.includes('MULTITAP_VERSION:-'+release.version));
});

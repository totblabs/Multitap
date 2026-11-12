import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer } from 'node:http';
import { downloadRoutes, releaseMetadata, maxReleaseManifestBytes } from '../src/downloads.mjs';

const entry = i => ({ product: 'client', version: `0.4.${i}`, filename: `Multitap-0.4.${i}-windows-x64.exe`, size: 64, sha256: 'a'.repeat(64) });
async function fixture(t) {
  const root = await mkdtemp(join(tmpdir(), 'multitap-release-notes-'));
  const routes = downloadRoutes(root), server = createServer(async (req,res) => { if (!await routes(req,res,new URL(req.url,'http://localhost').pathname)) { res.writeHead(404); res.end(); } });
  await new Promise(resolve => server.listen(0,'127.0.0.1',resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); await rm(root,{recursive:true,force:true}); });
  return { manifest: join(root,'releases.json'), origin: 'http://127.0.0.1:' + server.address().port };
}
test('release metadata is optional, text-only, UTF-8 bounded and UTC dates are canonicalized', () => {
  assert.deepEqual(releaseMetadata({}),{});
  assert.deepEqual(releaseMetadata({changelog:'한글 수정\n• 창 크기',releasedAt:'2026-10-23T12:34:56Z'}),{changelog:'한글 수정\n• 창 크기',releasedAt:'2026-10-23T12:34:56.000Z'});
  for (const changelog of ['😀'.repeat(2000),'가'.repeat(2666)+'ab','', 'a\t\r\nb']) assert.equal(releaseMetadata({changelog}).changelog,changelog);
  for (const changelog of ['😀'.repeat(2001),'가'.repeat(2667),'x\0y','x\u001by','x\u007fy','\ud800',null,[],42]) assert.equal(releaseMetadata({changelog}),null);
  for (const releasedAt of ['2026-02-30T12:00:00Z','2026-13-01T00:00:00Z','2026-10-23','2026-10-23T12:00:00+09:00','2026-10-23T12:00:00+00:00','2026-10-23T24:00:00Z',null,{},'2026-10-23T12:00:00.1234Z']) assert.equal(releaseMetadata({releasedAt}),null,String(releasedAt));
});
test('download API exposes only safe optional metadata and preserves old entries without notes', async t => {
  const f = await fixture(t);
  const old = entry(1), current = {...entry(2),changelog:'보안 및 화면 수정\n<script>literal text</script>',releasedAt:'2026-10-23T00:00:00Z',privateToken:'not-public'};
  await writeFile(f.manifest,JSON.stringify([old,current,{...entry(3),changelog:'invalid\0'}]));
  const response = await fetch(f.origin+'/api/v1/downloads'), list = await response.json();
  assert.equal(response.status,200); assert.equal(list.releases.length,2);
  assert.equal(Object.hasOwn(list.releases.find(e=>e.filename===old.filename),'changelog'),false);
  const exposed = list.releases.find(e=>e.filename===current.filename);
  assert.equal(exposed.changelog,current.changelog);
  assert.equal(exposed.releasedAt,'2026-10-23T00:00:00.000Z');
  assert.equal(Object.hasOwn(exposed,'privateToken'),false);
  assert.match(response.headers.get('content-type'),/^application\/json/);
  assert.equal(response.headers.get('x-content-type-options'),'nosniff');
  const head = await fetch(f.origin+'/api/v1/downloads',{method:'HEAD'});
  assert.equal(head.status,200); assert.equal((await head.arrayBuffer()).byteLength,0);
});
test('32 full UTF-8 release notes fit the bounded catalog; oversized catalog fails closed', async t => {
  const f = await fixture(t), notes = '가'.repeat(2666)+'ab';
  const bytes = Buffer.from(JSON.stringify(Array.from({length:32},(_,i) => ({...entry(i),changelog:notes}))));
  assert.ok(bytes.length > 32768 && bytes.length < maxReleaseManifestBytes);
  await writeFile(f.manifest,bytes);
  assert.equal((await (await fetch(f.origin+'/api/v1/downloads')).json()).releases.length,32);
  await writeFile(f.manifest,' '.repeat(maxReleaseManifestBytes+1));
  assert.deepEqual(await (await fetch(f.origin+'/api/v1/downloads')).json(),{releases:[]});
});
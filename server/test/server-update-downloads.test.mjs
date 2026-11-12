import test from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp,writeFile,rm,symlink} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {createServer} from 'node:http';
import {serverUpdateDownloads} from '../src/server-update-downloads.mjs';
import {digest} from '../update/protocol.mjs';

test('server feed and exact signed artifact are separate from Windows catalog and cannot expose neighboring files',async t=>{
  const dir=await mkdtemp(join(tmpdir(),'multitap-feed-'));t.after(()=>rm(dir,{recursive:true,force:true}));const bytes=Buffer.from('QA archive fixture'),name='Multitap-Server-0.5.22-linux-x64.tar.gz';
  const release={schema:1,product:'multitap-broker',version:'0.5.22',platform:'linux-x64-systemd',releasedAt:'2026-10-24T10:00:00.000Z',changelog:'한글 변경 내역',artifact:{path:'/downloads/'+name,size:bytes.length,sha256:digest(bytes)}};
  const feed=JSON.stringify({release,signature:Buffer.alloc(64).toString('base64')});await writeFile(join(dir,'server-updates.json'),feed);await writeFile(join(dir,name),bytes);await writeFile(join(dir,'vault.key'),'never public');
  const serve=serverUpdateDownloads(dir),server=createServer(async(req,res)=>{if(!await serve(req,res,new URL(req.url,'http://localhost').pathname)){res.writeHead(404);res.end();}});await new Promise(r=>server.listen(0,'127.0.0.1',r));t.after(()=>new Promise(r=>server.close(r)));const origin='http://127.0.0.1:'+server.address().port;
  const response=await fetch(origin+'/downloads/server-updates.json');assert.equal(await response.text(),feed);assert.match(response.headers.get('content-type'),/utf-8/);
  const file=await fetch(origin+'/downloads/'+name);assert.deepEqual(Buffer.from(await file.arrayBuffer()),bytes);assert.equal(file.headers.get('content-type'),'application/gzip');
  for(const path of ['/downloads/vault.key','/downloads/Multitap-Server-0.5.21-linux-x64.tar.gz','/downloads/../vault.key'])assert.equal((await fetch(origin+path)).status,404);
  await writeFile(join(dir,name),'different length');assert.equal((await fetch(origin+'/downloads/'+name)).status,404);
  assert.equal((await fetch(origin+'/downloads/server-updates.json',{method:'POST'})).status,405);
  await writeFile(join(dir,'server-updates.json'),'{}');assert.equal((await fetch(origin+'/downloads/server-updates.json')).status,404);
});

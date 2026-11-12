import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, writeFile, rm, symlink } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { BrowserConsoleAssets } from '../src/browser-console-assets.mjs';

async function fixture(t) {
  const directory=await mkdtemp(join(tmpdir(),'multitap-console-assets-'));
  t.after(()=>rm(directory,{recursive:true}));
  const bytes=Buffer.from('<!doctype html><title>Manifest fixture</title>');
  await writeFile(join(directory,'index.html'),bytes);
  const entry={path:'index.html',bytes:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex')};
  const manifest={schema:1,product:'multitap-browser-console',files:[entry]};
  const save=value=>writeFile(join(directory,'assets-manifest.json'),JSON.stringify(value));await save(manifest);
  return {directory,bytes,entry,manifest,save};
}
test('manifest bounds and exact schema reject traversal, duplicates, extra fields, missing shell and wrong bytes/hash',async t=>{
  const f=await fixture(t);
  const invalid=[
    {...f.manifest,schema:2},{...f.manifest,unexpected:true},{...f.manifest,files:[]},
    {...f.manifest,files:[f.entry,f.entry]},
    ...['../index.html','/index.html','a//index.html','a/./index.html','a\\index.html','a/%2e%2e/index.html'].map(path=>({...f.manifest,files:[{...f.entry,path}]})),
    {...f.manifest,files:[{...f.entry,bytes:4*1024*1024+1}]},
    {...f.manifest,files:[{...f.entry,bytes:f.entry.bytes+1}]},
    {...f.manifest,files:[{...f.entry,sha256:'0'.repeat(64)}]},
    {...f.manifest,files:[{...f.entry,extra:'ignored?'}]},
    {...f.manifest,files:[{...f.entry,path:'assets-manifest.json'}]},
  ];
  for(const manifest of invalid){await f.save(manifest);await assert.rejects(new BrowserConsoleAssets(f.directory).get('index.html'),e=>e.status===503&&e.code==='console_assets_unavailable');}
  await f.save(f.manifest);const cache=new BrowserConsoleAssets(f.directory);assert.deepEqual((await cache.get('index.html')).bytes,f.bytes);
  await writeFile(join(f.directory,'unlisted.txt'),'not-public');await assert.rejects(cache.get('unlisted.txt'),e=>e.status===404);
  await writeFile(join(f.directory,'index.html'),'replacement');assert.deepEqual((await cache.get('index.html')).bytes,f.bytes,'only immutable verified bytes are served for this process');
});
test('a symlink asset cannot substitute a file outside the allowlisted asset directory',async t=>{
  const f=await fixture(t);await writeFile(join(f.directory,'outside.txt'),f.bytes);
  try{await symlink(join(f.directory,'outside.txt'),join(f.directory,'linked.txt'),'file');}
  catch(error){if(['EPERM','EACCES'].includes(error.code)){t.skip('OS account cannot create symlink fixture');return;}throw error;}
  await f.save({...f.manifest,files:[f.entry,{...f.entry,path:'linked.txt'}]});
  await assert.rejects(new BrowserConsoleAssets(f.directory).get('index.html'),e=>e.code==='console_assets_unavailable');
});

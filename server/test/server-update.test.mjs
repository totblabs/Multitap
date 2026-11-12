import test from 'node:test';
import assert from 'node:assert/strict';
import {generateKeyPairSync,sign} from 'node:crypto';
import {mkdtemp,readFile,writeFile,chmod,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {gzipSync,gunzipSync} from 'node:zlib';
import {spawnSync} from 'node:child_process';
import {symlink} from 'node:fs/promises';
import {decodeFeed,digest,compareVersion,feedUrl,fetchBytes} from '../update/protocol.mjs';
import {packPackage,unpackPackage} from '../update/package.mjs';
import {UpdateEngine} from '../update/engine.mjs';
import {listenUpdateAgent} from '../update/agent.mjs';
import {ServerUpdateClient} from '../src/server-update.mjs';

const keys=generateKeyPairSync('ed25519'),publicKey=keys.publicKey.export({type:'spki',format:'pem'});
function fixture(version='0.5.22'){
  const files=new Map();for(const name of ['src/index.mjs','src/http.mjs','src/store.mjs','src/bootstrap.mjs','admin/index.html','admin/app.js','update/agent.mjs','update/provision-broker.sh','node_modules/ws/package.json'])files.set(name,Buffer.from('fixture'));
  files.set('package.json',Buffer.from(JSON.stringify({name:'multitap-broker',version})));
  files.set('package-lock.json',Buffer.from(JSON.stringify({version,packages:{'':{version}}})));
  const archive=packPackage(files),release={schema:1,product:'multitap-broker',version,platform:'linux-x64-systemd',releasedAt:'2026-10-24T10:00:00.000Z',changelog:'서버 업데이트와 복구를 개선했습니다.',artifact:{path:`/downloads/Multitap-Server-${version}-linux-x64.tar.gz`,size:archive.length,sha256:digest(archive)}};
  const feed=r=>Buffer.from(JSON.stringify({release:r,signature:sign(null,Buffer.from(JSON.stringify(r)),keys.privateKey).toString('base64')}));
  return {files,archive,release,feed:feed(release),signFeed:feed,releaseId:digest(Buffer.from(JSON.stringify(release)))};
}
test('signed server feed rejects changed bytes, foreign keys, invalid metadata, downgrade confusion and non-HTTPS sources',()=>{
  const f=fixture();assert.equal(decodeFeed(f.feed,publicKey).releaseId,f.releaseId);
  const changed=JSON.parse(f.feed);changed.release.changelog+='changed';assert.throws(()=>decodeFeed(Buffer.from(JSON.stringify(changed)),publicKey),/signature/);
  const other=generateKeyPairSync('ed25519');assert.throws(()=>decodeFeed(f.feed,other.publicKey.export({type:'spki',format:'pem'})),/signature/);
  for(const patch of [{version:'1.00.0'},{platform:'windows'},{releasedAt:'2026-02-30T00:00:00.000Z'},{changelog:'???'},{extra:1},{artifact:{...f.release.artifact,path:'/etc/passwd'}},{artifact:{...f.release.artifact,size:129*1024*1024}}])assert.throws(()=>decodeFeed(f.signFeed({...f.release,...patch}),publicKey));
  for(const url of ['http://example.test/a','https://user:password@example.test/a','https://example.test/a?token=x','https://example.test/a#x','https://example.test\\evil/a'])assert.throws(()=>feedUrl(url));
  assert.equal(compareVersion('0.5.9','0.5.22'),-1);assert.equal(compareVersion('1.0.0','0.99.99'),1);
  const docker={...f.release,platform:'linux-x64-docker',artifact:{...f.release.artifact,path:'/downloads/Multitap-Server-0.5.22-linux-x64-docker.tar.gz'}};assert.equal(decodeFeed(f.signFeed(docker),publicKey).release.platform,'linux-x64-docker');assert.throws(()=>decodeFeed(f.signFeed({...docker,artifact:f.release.artifact}),publicKey));
});
test('strict package parser accepts only bounded regular product files and refuses traversal, links, checksum and version mismatch',()=>{
  const f=fixture();assert.equal(unpackPackage(f.archive,f.release.version).size,f.files.size);assert.throws(()=>unpackPackage(f.archive,'0.5.23'),/version/);
  for(const name of ['../src/x','/src/x','src/../x','src//x','vault.key','src/host-token.json','src/machine.dpapi'])assert.throws(()=>packPackage(new Map([[name,Buffer.from('x')]])));
  for(const at of [0,100,124,156,157,257,345]){const tar=gunzipSync(f.archive);tar[at]^=1;assert.throws(()=>unpackPackage(gzipSync(tar),f.release.version));}
  const missing=new Map(f.files);missing.delete('src/index.mjs');assert.throws(()=>unpackPackage(packPackage(missing),f.release.version),/incomplete/);
});
test('bounded HTTPS download rejects oversized, truncated and redirect responses',async()=>{
  for(const response of [new Response('large',{headers:{'content-length':'999'}}),new Response('x',{headers:{'content-length':'3'}}),new Response('x',{status:302}),new Response('long')])await assert.rejects(fetchBytes(new URL('https://fixture.example/x'),3,async()=>response));
  assert.equal((await fetchBytes(new URL('https://fixture.example/x'),3,async(_url,o)=>{assert.equal(o.redirect,'error');return new Response('abc');})).toString(),'abc');
});
async function setup(t,{badHash=false,failApply=false,recover=true}={}){
  const dir=await mkdtemp(join(tmpdir(),'multitap-update-'));await chmod(dir,0o700);t.after(()=>rm(dir,{recursive:true,force:true}));
  const f=fixture(),calls=[];let version='0.5.21';
  const adapter={platform:'linux-x64-systemd',version:async()=>version,prepare:async input=>{calls.push('prepare');return {fromVersion:version,toVersion:input.release.version,stage:input.stage};},apply:async(job,phase)=>{calls.push('apply');for(const p of ['backing_up','applying','restarting','health_check'])await phase(p);if(failApply)throw Error('update_health_failed');version=job.toVersion;},recover:async()=>{calls.push('recover');version='0.5.21';return recover;}};
  const engine=new UpdateEngine({directory:dir,feed:'https://updates.fixture.example/downloads/server-updates.json',publicKey,adapter,reserveBytes:0,fetcher:async url=>new Response(String(url).endsWith('.json')?f.feed:badHash?Buffer.alloc(f.archive.length):f.archive)});await engine.initialize();return {engine,f,calls,dir,adapter};
}
const linux={skip:process.platform!=='linux'?'Unix socket and Linux permission boundary':false};
test('real symlink CLI invokes startup validation rather than silently exiting without a service',linux,async t=>{
  const directory=await mkdtemp(join(tmpdir(),'multitap-updater-cli-'));t.after(()=>rm(directory,{recursive:true,force:true}));
  const alias=join(directory,'current');await symlink(new URL('../update/',import.meta.url).pathname,alias);
  const config=join(directory,'invalid-config.json');await writeFile(config,'{}',{mode:0o600});
  const result=spawnSync(process.execPath,[join(alias,'agent.mjs')],{env:{...process.env,MULTITAP_UPDATE_CONFIG:config},encoding:'utf8',timeout:5000});
  assert.equal(result.status,1);assert.match(result.stderr,/updater startup failed/);
});
test('real files + local agent socket serialize check/apply and persist verified update state',linux,async t=>{
  const {engine,f,calls,dir}=await setup(t),socket=join(dir,'control.sock'),server=await listenUpdateAgent(engine,socket);t.after(()=>new Promise(r=>server.close(r)));const client=new ServerUpdateClient(socket);
  assert.equal((await client.status()).state,'idle');await client.check();await engine.task;assert.equal((await client.status()).state,'available');
  assert.throws(()=>engine.apply({releaseId:'0'.repeat(64),fromVersion:'0.5.21'}),/check_required/);
  const accepted=await client.apply({releaseId:f.releaseId,fromVersion:'0.5.21'});assert.ok(accepted.operationId);await engine.task;
  const status=await client.status();assert.equal(status.state,'succeeded');assert.equal(status.currentVersion,'0.5.22');assert.deepEqual(calls,['prepare','apply']);
  assert.equal(JSON.stringify(status).includes(dir),false);assert.equal(JSON.parse(await readFile(join(dir,'status.json'))).phase,'succeeded');
  assert.ok((await readFile(join(dir,accepted.operationId,'verified/src/index.mjs'))).length>0);
});
test('checksum failure never reaches stop/prepare, apply failure verifies rollback and uncertainty remains blocked',linux,async t=>{
  for(const options of [{badHash:true},{failApply:true},{failApply:true,recover:false}])await t.test(JSON.stringify(options),async t=>{
    const {engine,f,calls}=await setup(t,options);engine.check();assert.throws(()=>engine.check(),/busy/);await engine.task;
    engine.apply({releaseId:f.releaseId,fromVersion:'0.5.21'});await engine.task;
    assert.equal((await engine.status()).state,options.badHash?'failed':options.recover===false?'recovery_required':'rolled_back');
    if(options.badHash)assert.deepEqual(calls,[]);else assert.deepEqual(calls,['prepare','apply','recover']);
    if(options.recover===false)assert.throws(()=>engine.check(),/recovery_required/);
  });
});
test('service restart recognizes an interrupted persisted operation and requires explicit adapter reconciliation',linux,async t=>{
  const {engine,dir,calls,adapter}=await setup(t);await writeFile(join(dir,'status.json'),JSON.stringify({phase:'applying',job:{id:'fixture'}}));
  const next=new UpdateEngine({directory:dir,feed:'https://updates.fixture.example/feed',publicKey,adapter});await next.initialize();assert.equal((await next.status()).state,'rolled_back');assert.deepEqual(calls,['recover']);
});
test('a fresh check cannot reuse a completed update job or roll back a successful release after restart',linux,async t=>{
  const {engine,dir,calls,adapter,f}=await setup(t);engine.check();await engine.task;engine.apply({releaseId:f.releaseId,fromVersion:'0.5.21'});await engine.task;assert.equal((await engine.status()).state,'succeeded');
  engine.check();assert.equal(engine.state.job,null);assert.equal(engine.state.operationId,null);await engine.task;
  await writeFile(join(dir,'status.json'),JSON.stringify({...engine.state,phase:'checking',job:{fromVersion:'older-successful-operation'}}));
  const next=new UpdateEngine({directory:dir,feed:'https://updates.fixture.example/feed',publicKey,adapter});await next.initialize();assert.equal((await next.status()).state,'failed');assert.deepEqual(calls,['prepare','apply']);assert.equal(await adapter.version(),'0.5.22');
});
test('a correctly signed release for another deployment cannot be offered or applied',linux,async t=>{
  const {engine,adapter,calls}=await setup(t);adapter.platform='linux-x64-docker';engine.check();await engine.task;assert.equal(engine.state.phase,'failed');assert.equal(engine.state.errorCode,'update_platform_unsupported');assert.deepEqual(calls,[]);
});

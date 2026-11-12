import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, writeFile, readFile, readdir, rm, symlink, lstat, link } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createHash } from 'node:crypto';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { createDownloadBundle, validateDownloadBundle, seedDownloadBundle, bundleEntries, installerReceipt, androidReceipt } from '../src/download-bundle.mjs';
import { readReleases, publicDownloadCatalog } from '../src/downloads.mjs';
import { latestFile } from '../src/landing.mjs';

const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const bytes = Buffer.from('MZ isolated non-executable product fixture: no Windows payload execution');
const apkBytes = Buffer.from('PK isolated non-installable Android fixture: no package payload');
const abis = ['arm64-v8a', 'armeabi-v7a', 'x86_64'];
const signer = 'c'.repeat(64);
const entry = (product='client',i=1) => ({product,version:`0.9.${i}`,filename:`Multitap${product==='host'?'-Host':''}-0.9.${i}-windows-x64.exe`,size:bytes.length,sha256:sha(bytes)});
const apk = (abi='arm64-v8a',version='0.9.1') => ({product:'android-client',version,abi,filename:`Multitap-${version}-android-${abi}.apk`,format:'apk',size:apkBytes.length,sha256:sha(apkBytes)});
const android = (version='0.9.1') => abis.map(abi => apk(abi, version));
const receipt = e => ({...e,payloadVerified:true,files:(e.product==='host'?['bin/MultitapHostSettings.exe','multitap-host.json']:['bin/Multitap.exe','MultitapClientSetup.exe']).map(path=>({path,size:1,sha256:'a'.repeat(64)})),privateBuildPath:'not-copied'});
// The shape tools/verify-android-release.ps1 writes next to each release APK.
const apkReceipt = (e, overrides = {}) => ({policyVersion:1,product:e.product,version:e.version,abi:e.abi,filename:e.filename,size:e.size,sha256:e.sha256,signerSha256:signer,payloadVerified:true,signatureVerified:true,contentSignaturesVerified:true,contentScanPrefixBytes:4096,inflatedAllEntries:true,minSdk:26,targetSdk:36,deviceValidated:false,entryCount:42,expandedBytes:4096,versionCode:2001,...overrides});
const isApk = e => e.product === 'android-client';
const products = () => [entry(), entry('host'), ...android()];
async function fixture(t,entries=products(),{receiptFor}={}) {
  const root=await mkdtemp(join(tmpdir(),'multitap-bundle-')); t.after(()=>rm(root,{recursive:true,force:true}));
  const artifacts=join(root,'artifacts'),data=join(root,'data'),bundle=join(root,'bundle'),manifest=join(root,'manifest.json');
  await mkdir(artifacts); await mkdir(data);
  for(const e of entries){
    await writeFile(join(artifacts,e.filename),isApk(e)?apkBytes:bytes);
    if(isApk(e)) await writeFile(join(artifacts,e.filename+'.verification.json'),JSON.stringify(receiptFor?.(e)??apkReceipt(e)));
    else await writeFile(join(artifacts,e.filename+'.json'),JSON.stringify(receipt(e)));
  }
  await writeFile(manifest,JSON.stringify(entries));
  return {root,artifacts,data,bundle,manifest,entries,target:join(data,'downloads')};
}
async function prepare(f,options){return createDownloadBundle(f.manifest,f.artifacts,f.bundle,options);}
async function oldCatalog(f,entries){await mkdir(f.target);await writeFile(join(f.target,'releases.json'),JSON.stringify(entries,null,4)+'\n');}

test('verified bundle creation/seed preserve source bytes, existing catalog fields, payloads and unrelated private state',async t=>{
  const f=await fixture(t,[{...entry(),changelog:'화면 및 입력 수정',releasedAt:'2026-10-23T00:00:00.000Z',privateField:'not-copied'},entry('host'),...android().map(e=>({...e,changelog:'안드로이드 수정'}))]);
  const originalManifest=await readFile(f.manifest);await prepare(f);
  assert.deepEqual(await readFile(f.manifest),originalManifest);
  assert.equal((await validateDownloadBundle(f.bundle)).entries.length,5);
  assert.equal((await readFile(join(f.bundle,'releases.json'),'utf8')).includes('privateField'),false);
  assert.equal((await readFile(join(f.bundle,f.entries[0].filename+'.receipt.json'),'utf8')).includes('privateBuildPath'),false);
  const old={...entry('client',0),retained:'exact existing entry'};
  await oldCatalog(f,[old]); await writeFile(join(f.target,old.filename),bytes);
  await writeFile(join(f.data,'vault.key'),'unrelated fixture private bytes');
  const result=await seedDownloadBundle(f.bundle,f.target);
  assert.equal(result.seeded,5);assert.equal(result.catalogEntries,6);
  // One notes record per product version: client 0.9.0 and 0.9.1, Host 0.9.1, Android 0.9.1.
  assert.deepEqual(result.history,{recorded:4,unchanged:0,conflicts:0});
  assert.deepEqual(JSON.parse(await readFile(join(f.target,'releases.json')))[0],old);
  assert.deepEqual(await readFile(join(f.target,old.filename)),bytes);
  assert.equal(await readFile(join(f.data,'vault.key'),'utf8'),'unrelated fixture private bytes');
  // Every catalog entry also has its notes-only history record; an entry
  // without a catalog date takes its installer's time on the volume.
  assert.deepEqual(JSON.parse(await readFile(join(f.target,'release-notes','client-0.9.1.json'))),{product:'client',version:'0.9.1',releasedAt:'2026-10-23T00:00:00.000Z',changelog:'화면 및 입력 수정'});
  assert.equal(JSON.parse(await readFile(join(f.target,'release-notes','android-client-0.9.1.json'))).changelog,'안드로이드 수정');
  const oldRecord=JSON.parse(await readFile(join(f.target,'release-notes','client-0.9.0.json')));
  assert.equal(oldRecord.releasedAt,new Date(Math.floor((await lstat(join(f.target,old.filename))).mtimeMs)).toISOString());
  assert.equal(Object.hasOwn(oldRecord,'retained'),false);
  const once=await readFile(join(f.target,'releases.json'));
  assert.deepEqual(await seedDownloadBundle(f.bundle,f.target),{seeded:0,catalogChanged:false,catalogEntries:6,empty:false,history:{recorded:0,unchanged:4,conflicts:0}});
  assert.deepEqual(await readFile(join(f.target,'releases.json')),once,'idempotent seed preserves original manifest bytes');
  assert.equal((await readdir(f.target)).some(n=>n.startsWith('.seed-')||n==='.publish.lock'),false);
});
test('seeded Android APKs are served by the catalog and /get/android next to the Windows installers',async t=>{
  const f=await fixture(t);await prepare(f);
  for(const e of android()){
    const copied=JSON.parse(await readFile(join(f.bundle,e.filename+'.receipt.json')));
    assert.deepEqual(Object.keys(copied).sort(),['abi','contentSignaturesVerified','entryCount','expandedBytes','filename','minSdk','payloadVerified','policyVersion','product','sha256','signatureVerified','signerSha256','size','targetSdk','version','versionCode'].sort(),'only fixed receipt fields reach the image');
  }
  await seedDownloadBundle(f.bundle,f.target);
  const releases=await readReleases(f.target),listed=publicDownloadCatalog(releases);
  assert.deepEqual([...new Set(listed.map(r=>r.product))].sort(),['android-client','client','host']);
  assert.deepEqual(listed.filter(r=>r.product==='android-client').map(r=>r.abi).sort(),[...abis].sort());
  assert.equal(latestFile(releases,'android').filename,'Multitap-0.9.1-android-arm64-v8a.apk');
  assert.equal(latestFile(releases,'client').filename,'Multitap-0.9.1-windows-x64.exe');
  for(const e of android()) assert.deepEqual(await readFile(join(f.target,e.filename)),apkBytes);
});
test('a newer image bundle adds releases and never removes or rewrites older installers or APKs',async t=>{
  const first=await fixture(t);await prepare(first);await seedDownloadBundle(first.bundle,first.target);
  const before=await readFile(join(first.target,'releases.json'));
  const next=await fixture(t,[entry('client',2),entry('host',2),...android('0.9.2')]);await prepare(next);
  const result=await seedDownloadBundle(next.bundle,first.target);
  assert.equal(result.seeded,5);assert.equal(result.catalogEntries,10);
  const catalog=JSON.parse(await readFile(join(first.target,'releases.json')));
  assert.deepEqual(catalog.slice(0,5),JSON.parse(before),'older catalog entries keep their exact order and fields');
  for(const e of products()) assert.deepEqual(await readFile(join(first.target,e.filename)),isApk(e)?apkBytes:bytes);
  assert.equal(latestFile(await readReleases(first.target),'android').version,'0.9.2');
});
test('Android bundle entries need a complete, verified, single-signer APK set',async t=>{
  // Every ABI of an Android version, and Android at all.
  await assert.rejects(prepare(await fixture(t,[entry(),entry('host'),apk('arm64-v8a'),apk('x86_64')])),/every ABI/);
  await assert.rejects(prepare(await fixture(t,[entry(),entry('host')])),/Android APKs/);
  await assert.rejects(prepare(await fixture(t,[entry(),...android()])),/Host installers/);
  // One application id, one signer.
  await assert.rejects(prepare(await fixture(t,products(),{receiptFor:e=>apkReceipt(e,e.abi==='x86_64'?{signerSha256:'d'.repeat(64)}:{})})),/different signers/);
  // The verifier's own pass flags and identity must match the exact bytes.
  for(const patch of [{signatureVerified:false},{payloadVerified:false},{contentSignaturesVerified:false},{policyVersion:2},{sha256:'e'.repeat(64)},{size:1},{abi:'x86_64'},{signerSha256:'C'.repeat(64)},{minSdk:40,targetSdk:36},{versionCode:0},{entryCount:0},{expandedBytes:0}]){
    const f=await fixture(t,products(),{receiptFor:e=>apkReceipt(e,e.abi==='arm64-v8a'?patch:{})});
    await assert.rejects(prepare(f),/APK provenance mismatch/,JSON.stringify(patch));
    await assert.rejects(readdir(f.bundle),e=>e.code==='ENOENT','no bundle output is left behind');
  }
  assert.throws(()=>androidReceipt(apkReceipt(apk()),entry()),/APK provenance mismatch/);
  assert.throws(()=>installerReceipt(receipt(entry()),apk()),/Installer provenance mismatch/);
});
test('the image validator rechecks APK bytes, copied receipts and signer agreement',async t=>{
  const f=await fixture(t);await prepare(f);
  const target=android()[1];
  await writeFile(join(f.bundle,target.filename),Buffer.alloc(apkBytes.length));
  await assert.rejects(validateDownloadBundle(f.bundle),/hash conflict/);
  await writeFile(join(f.bundle,target.filename),apkBytes);
  await writeFile(join(f.bundle,target.filename+'.receipt.json'),JSON.stringify(apkReceipt(target,{signerSha256:'d'.repeat(64)})));
  await assert.rejects(validateDownloadBundle(f.bundle),/provenance hash mismatch/);
});
test('bundles carry only Windows installers and APKs, never ZIP archives',()=>{
  const archive={...entry(),filename:'Multitap-0.9.1-windows-x64.zip'};
  assert.equal(bundleEntries(Buffer.from(JSON.stringify([archive]))).length,1,'the volume catalog may still hold archives');
  assert.throws(()=>bundleEntries(Buffer.from(JSON.stringify([archive])),{bundleOnly:true}),/Invalid release catalog entry/);
  assert.equal(bundleEntries(Buffer.from(JSON.stringify(products())),{bundleOnly:true}).length,5);
  assert.throws(()=>bundleEntries(Buffer.from(JSON.stringify([{...apk(),format:'archive'}])),{bundleOnly:true}),/Invalid/);
});
test('all bundle integrity/provenance validation occurs before installation volume mutation',async t=>{
  const f=await fixture(t);await prepare(f);
  await writeFile(join(f.bundle,f.entries[1].filename),Buffer.alloc(bytes.length));
  await assert.rejects(seedDownloadBundle(f.bundle,f.target),/hash conflict/);
  assert.deepEqual(await readdir(f.data),[]);
});
test('existing orphan payload conflict fails before copying any new product or replacing a catalog',async t=>{
  const f=await fixture(t);await prepare(f);await oldCatalog(f,[]);
  const manifest=await readFile(join(f.target,'releases.json'));
  await writeFile(join(f.target,f.entries[3].filename),Buffer.alloc(apkBytes.length));
  await assert.rejects(seedDownloadBundle(f.bundle,f.target),/hash conflict/);
  assert.deepEqual(await readFile(join(f.target,'releases.json')),manifest);
  assert.deepEqual((await readdir(f.target)).sort(),[f.entries[3].filename,'releases.json'].sort());
});
test('immutable catalog content and release-note conflicts preserve the exact old catalog and files',async t=>{
  for(const patch of [{sha256:'b'.repeat(64)},{changelog:'old immutable notes'}]){
    const f=await fixture(t,[{...entry(),changelog:'new notes'},entry('host'),...android()]);await prepare(f);await oldCatalog(f,[{...f.entries[0],...patch}]);
    const manifest=await readFile(join(f.target,'releases.json'));
    await assert.rejects(seedDownloadBundle(f.bundle,f.target),/Immutable release catalog conflict/);
    assert.deepEqual(await readFile(join(f.target,'releases.json')),manifest);assert.deepEqual(await readdir(f.target),['releases.json']);
  }
});
test('1024-entry raw cap never silently evicts old Windows or Android entries',async t=>{
  const f=await fixture(t);await prepare(f);await oldCatalog(f,Array.from({length:1020},(_,i)=>entry('client',i+10)));
  const manifest=await readFile(join(f.target,'releases.json'));
  await assert.rejects(seedDownloadBundle(f.bundle,f.target),/catalog is full/);
  assert.deepEqual(await readFile(join(f.target,'releases.json')),manifest);assert.deepEqual(await readdir(f.target),['releases.json']);
  await writeFile(join(f.target,'releases.json'),JSON.stringify(Array.from({length:1019},(_,i)=>entry('client',i+10))));
  const result=await seedDownloadBundle(f.bundle,f.target);
  assert.equal(result.catalogEntries,1024);assert.equal(result.history.recorded,1022);
});
test('Docker bundle seed grows a 32-entry installation without changing existing history, payloads or vault bytes',async t=>{
  const f=await fixture(t);await prepare(f);
  const existing=Array.from({length:32},(_,i)=>({...entry('client',i+10),retained:{index:i,note:'exact old metadata'}}));
  await oldCatalog(f,existing);
  for(const e of existing) await writeFile(join(f.target,e.filename),bytes);
  await writeFile(join(f.data,'vault.key'),'exact unrelated vault fixture');
  const result=await seedDownloadBundle(f.bundle,f.target);
  assert.equal(result.seeded,5);assert.equal(result.catalogEntries,37);
  const merged=JSON.parse(await readFile(join(f.target,'releases.json')));
  assert.deepEqual(merged.slice(0,32),existing);
  for(const e of existing) assert.deepEqual(await readFile(join(f.target,e.filename)),bytes);
  assert.equal(await readFile(join(f.data,'vault.key'),'utf8'),'exact unrelated vault fixture');
  const before=await readFile(join(f.target,'releases.json'));
  assert.equal((await seedDownloadBundle(f.bundle,f.target)).seeded,0);
  assert.deepEqual(await readFile(join(f.target,'releases.json')),before);
});
test('shared publication lock is honored and another publisher lock is never removed',async t=>{
  const f=await fixture(t);await prepare(f);await oldCatalog(f,[]);
  for(const content of ['owned by another publisher','',JSON.stringify({owner:'another-tool',nonce:'x'})]){
    await writeFile(join(f.target,'.publish.lock'),content);
    await assert.rejects(seedDownloadBundle(f.bundle,f.target),e=>e.code==='EEXIST'&&/if no release tool is running, remove that file/.test(e.message));
    assert.equal(await readFile(join(f.target,'.publish.lock'),'utf8'),content);
    assert.deepEqual((await readdir(f.target)).sort(),['.publish.lock','releases.json'],'no seed temporary is left behind');
  }
  assert.deepEqual(JSON.parse(await readFile(join(f.target,'releases.json'))),[]);
});
test('a start stopped during the seed does not block later starts: its own lock and temporaries are taken over',async t=>{
  const f=await fixture(t);await prepare(f);await oldCatalog(f,[]);
  // What a seed killed mid-copy leaves: its named lock (still hard-linked to its
  // part file when killed between the two steps) and a partial installer copy.
  const id='0b1e2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d',part=join(f.target,'.seed-lock-'+id);
  await writeFile(part,JSON.stringify({owner:'multitap-docker-seed',nonce:'interrupted',pid:7,startedAt:'2026-11-03T00:00:00.000Z'})+'\n');
  try{await link(part,join(f.target,'.publish.lock'));}catch(e){if(e.code!=='EPERM')throw e;await writeFile(join(f.target,'.publish.lock'),await readFile(part));}
  await writeFile(join(f.target,'.seed-'+id.replace('0b','1b')),bytes.subarray(0,7));
  await writeFile(join(f.target,'.seed-not-a-temporary'),'kept');
  const result=await seedDownloadBundle(f.bundle,f.target);
  assert.equal(result.seeded,5);assert.equal(result.recoveredLeftovers,3,'the lock, its part file and the partial copy');
  assert.deepEqual((await readdir(f.target)).filter(n=>n.startsWith('.')).sort(),['.seed-not-a-temporary']);
  for(const e of products()) assert.deepEqual(await readFile(join(f.target,e.filename)),isApk(e)?apkBytes:bytes);
  assert.equal((await seedDownloadBundle(f.bundle,f.target)).seeded,0);
});
test('empty bundles require explicit development consent at both preparation and validation',async t=>{
  const f=await fixture(t,[]);
  await assert.rejects(prepare(f),/Windows client and Host installers and the Android APKs/);
  await prepare(f,{allowEmpty:true});
  await assert.rejects(validateDownloadBundle(f.bundle),/Windows client and Host installers/);
  assert.equal((await validateDownloadBundle(f.bundle,{allowEmpty:true})).empty,true);
  assert.deepEqual(await seedDownloadBundle(f.bundle,f.target,{allowEmpty:true}),{seeded:0,catalogChanged:false,empty:true});
  assert.deepEqual(await readdir(f.data),[]);
});
test('bundle refuses accidental runtime/private files, changed provenance, missing products and output replacement',async t=>{
  const f=await fixture(t);await prepare(f);
  await assert.rejects(prepare(f),e=>e.code==='EEXIST');
  await writeFile(join(f.bundle,'vault.key'),'private fixture');
  await assert.rejects(validateDownloadBundle(f.bundle),/Unexpected bundle content/);
  await rm(join(f.bundle,'vault.key'));
  await writeFile(join(f.bundle,f.entries[0].filename+'.receipt.json'),'{}');
  await assert.rejects(validateDownloadBundle(f.bundle),/provenance hash mismatch/);
  const onlyClient=await fixture(t,[entry(),...android()]);await assert.rejects(prepare(onlyClient),/Windows client and Host/);
});
test('bundle parser and provenance inventory reject unsafe identity, format, paths and secrets',()=>{
  for(const bad of [{...entry(),filename:'../installer.exe'},{...entry(),sha256:'A'.repeat(64)},{...entry(),size:128*1024*1024+1},{...entry(),changelog:'x\0'},{...apk(),filename:'../x.apk'},{...apk(),abi:'mips'}]) assert.throws(()=>bundleEntries(Buffer.from(JSON.stringify([bad])),{bundleOnly:true}),/Invalid/);
  assert.throws(()=>bundleEntries(Buffer.from(JSON.stringify([entry(),entry()]))),/Invalid/);
  assert.throws(()=>bundleEntries(Buffer.from([0xff])),/Invalid release JSON or UTF-8/);
  assert.throws(()=>bundleEntries(Buffer.from('private-fixture-fragment')),error=>error.message==='Invalid release JSON or UTF-8');
  for(const path of ['../secret','C:/secret','bin/credentials.dpapi','vault.key','bin/other.exe','bin\\file','bin/\u0000file']){
    const r=receipt(entry());r.files.push({path,size:1,sha256:'a'.repeat(64)});
    assert.throws(()=>installerReceipt(r,entry()),/Unsafe/);
  }
  assert.throws(()=>installerReceipt({...receipt(entry()),payloadVerified:false},entry()),/mismatch/);
});
// tools/release/host-installer-receipt.ps1: the MSI Host package has no setup program, its root carries multitap-host.json.
test('an MSI Host receipt is accepted with multitap-host.json and refused without it or its settings program',()=>{
  const host={...entry(),product:'host',filename:'Multitap-Host-1.0.54-windows-x64.exe',version:'1.0.54'};
  const msi=paths=>({...host,payloadVerified:true,files:paths.map(path=>({path,size:1,sha256:'a'.repeat(64)}))});
  const all=['multitap-host.json','bin/MultitapHost.exe','bin/MultitapHostSettings.exe','bin/MultitapHostTool.exe','bin/MultitapDisplay.exe','bin/winfsp/winfsp-2.1.25156.msi'];
  assert.deepEqual(installerReceipt(msi(all),host).files.map(f=>f.path),all);
  assert.throws(()=>installerReceipt(msi(all.filter(p=>p!=='multitap-host.json')),host),/Incomplete/);
  assert.throws(()=>installerReceipt(msi(all.filter(p=>p!=='bin/MultitapHostSettings.exe')),host),/Incomplete/);
  assert.throws(()=>installerReceipt(msi([...all,'bin/setup-helper.exe']),host),/Unsafe/);
});
test('bundle source and target installer symlinks cannot redirect payload verification',async t=>{
  const f=await fixture(t);await prepare(f);
  const link=join(f.bundle,f.entries[0].filename),outside=join(f.root,'outside.exe');await writeFile(outside,bytes);await rm(link);
  try{await symlink(outside,link,'file');}catch(e){if(process.platform==='win32'&&['EPERM','EACCES'].includes(e.code)){t.skip('Windows account cannot create file symlinks; execute on Linux.');return;}throw e;}
  await assert.rejects(validateDownloadBundle(f.bundle),/Unsafe/);
  await rm(link);await writeFile(link,bytes);await oldCatalog(f,[]);
  await symlink(outside,join(f.target,f.entries[1].filename),'file');
  await assert.rejects(seedDownloadBundle(f.bundle,f.target),/Unsafe/);
  assert.deepEqual(JSON.parse(await readFile(join(f.target,'releases.json'))),[]);
});

// The exact commands the Dockerfile (validate) and docker-entrypoint.sh (seed)
// run, as separate processes on temporary directories.
const cli = fileURLToPath(new URL('../src/download-bundle.mjs', import.meta.url));
const run = promisify(execFile);
const node = (...args) => run(process.execPath, [cli, ...args], { timeout: 30000 });
test('image build validate and container start seed commands work on a temporary volume',async t=>{
  const f=await fixture(t);await prepare(f);
  assert.deepEqual(JSON.parse((await node('validate',f.bundle)).stdout),{validated:true,releases:5,empty:false});
  const first=JSON.parse((await node('seed',f.bundle,f.target)).stdout);
  assert.equal(first.seeded,5);assert.equal(first.catalogEntries,5);
  // Restart with the same image: nothing changes. A newer image only adds.
  assert.equal(JSON.parse((await node('seed',f.bundle,f.target)).stdout).seeded,0);
  const next=await fixture(t,[entry('client',2),entry('host',2),...android('0.9.2')]);await prepare(next);
  assert.equal(JSON.parse((await node('seed',next.bundle,f.target)).stdout).catalogEntries,10);
  for(const e of products()) assert.deepEqual(await readFile(join(f.target,e.filename)),isApk(e)?apkBytes:bytes);
  // A damaged image bundle stops the start before the volume changes, without echoing input.
  const catalog=await readFile(join(f.target,'releases.json'));
  await writeFile(join(next.bundle,next.entries[2].filename),Buffer.alloc(apkBytes.length));
  await assert.rejects(node('seed',next.bundle,f.target),error=>error.code===1&&/^Multitap release bundle: Immutable release hash conflict\s*$/.test(error.stderr));
  assert.deepEqual(await readFile(join(f.target,'releases.json')),catalog);
  // The development opt-in is explicit on the command line too.
  const empty=await fixture(t,[]);await prepare(empty,{allowEmpty:true});
  await assert.rejects(node('validate',empty.bundle),error=>error.code===1);
  assert.deepEqual(JSON.parse((await node('validate',empty.bundle,'--allow-empty')).stdout),{validated:true,releases:0,empty:true});
});
test('Docker deliberately copies only the prepared bundle and seeds before account bootstrap',async()=>{
  const [docker,ignore,entrypoint]=await Promise.all(['../Dockerfile','../.dockerignore','../docker-entrypoint.sh'].map(path=>readFile(new URL(path,import.meta.url),'utf8')));
  assert.match(docker,/COPY release-bundle\/ \/bundle\//);assert.match(docker,/COPY --from=bundle \/bundle\/ \/opt\/multitap-release-bundle\//);
  // Modes are set in the bundle stage: a chmod in the final stage would store the installers twice.
  assert.match(docker,/chmod 0555 \/bundle && find \/bundle -mindepth 1 -maxdepth 1 -type f -exec chmod 0444 \{\} \+/);
  assert.doesNotMatch(docker,/chmod [0-7]+ \/opt\/multitap-release-bundle/);
  assert.match(docker,/download-bundle\.mjs validate/);assert.match(docker,/ARG MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE=false/);
  assert.match(ignore,/!release-bundle\/\*\*/);assert.doesNotMatch(docker,/COPY .*\.artifacts/);
  assert.ok(entrypoint.indexOf('download-bundle.mjs seed')<entrypoint.indexOf('bootstrap.mjs keep'));
  assert.doesNotMatch(entrypoint,/vdi\.totb\.me|rm -|chown/);
});

test('a volume put back from a copy keeps its release dates: whole-second or other file times are no conflict, a date or notes the catalog states still are',async t=>{
  const {utimes,stat}=await import('node:fs/promises');
  const f=await fixture(t,[entry(),entry('host'),...android()]);await prepare(f);
  assert.deepEqual((await seedDownloadBundle(f.bundle,f.target)).history,{recorded:3,unchanged:0,conflicts:0});
  const names=['client-0.9.1.json','host-0.9.1.json','android-client-0.9.1.json'],records=async()=>Promise.all(names.map(name=>readFile(join(f.target,'release-notes',name),'utf8')));
  const before=await records();
  for(const text of before)assert.match(JSON.parse(text).releasedAt,/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$/);
  // What plain tar restores: every installer's time cut to a whole second. Where that happens to be the
  // recorded instant already, another second stands for it (a copy made later).
  for(const e of f.entries){
    const path=join(f.target,e.filename),seconds=Math.floor((await stat(path)).mtimeMs/1000);
    await utimes(path,seconds,seconds);
    if(before.some(text=>JSON.parse(text).releasedAt===new Date(seconds*1000).toISOString()))await utimes(path,seconds-1,seconds-1);
  }
  for(let start=0;start<2;start++)assert.deepEqual((await seedDownloadBundle(f.bundle,f.target)).history,{recorded:0,unchanged:3,conflicts:0},'start '+(start+1));
  assert.deepEqual(await records(),before,'the dates recorded at publication stay');
  // A record that has no date yet takes the file's time, once.
  const undated=join(f.target,'release-notes','host-0.9.1.json');await rm(undated);
  await writeFile(undated,JSON.stringify({product:'host',version:'0.9.1'})+'\n');
  const filled=await seedDownloadBundle(f.bundle,f.target);
  assert.equal(filled.history.conflicts,0);assert.equal(filled.history.recorded,1);
  const hostFile=f.entries.find(e=>e.product==='host').filename;
  assert.equal(JSON.parse(await readFile(undated,'utf8')).releasedAt,new Date(Math.floor((await stat(join(f.target,hostFile))).mtimeMs)).toISOString());
  assert.deepEqual((await seedDownloadBundle(f.bundle,f.target)).history,{recorded:0,unchanged:3,conflicts:0});
  // A real conflict: the catalog itself states another date, or other notes, than the record holds.
  const catalog=join(f.target,'releases.json'),entries=JSON.parse(await readFile(catalog,'utf8'));
  await writeFile(catalog,JSON.stringify(entries.map(e=>e.product==='client'?{...e,releasedAt:'2020-01-01T00:00:00.000Z'}:e.product==='host'?{...e,changelog:'다른 내용'}:e),null,4)+'\n');
  const client=join(f.target,'release-notes','client-0.9.1.json'),kept=await readFile(client,'utf8');
  await writeFile(undated,JSON.stringify({product:'host',version:'0.9.1',changelog:'원래 내용'})+'\n');
  const conflicting=await seedDownloadBundle(f.bundle,f.target);
  assert.equal(conflicting.history.conflicts,2);assert.equal(conflicting.history.unchanged,1);
  assert.equal(await readFile(client,'utf8'),kept,'and the record is kept as it is');
});

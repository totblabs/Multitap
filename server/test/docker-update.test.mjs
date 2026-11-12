import {test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp,mkdir,writeFile,readFile,lstat,chmod,readdir,symlink} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {execFile} from 'node:child_process';
import {spawnSync} from 'node:child_process';
import {promisify} from 'node:util';
import {DatabaseSync} from 'node:sqlite';
import {DockerAdapter,validateDockerConfig,validateDockerImage,trustedDockerPath} from '../update/docker.mjs';
import {digest} from '../update/protocol.mjs';
import {packPackage} from '../update/package.mjs';

const linux={skip:process.platform!=='linux'||process.getuid?.()!==0?'root Linux filesystem/SQLite helper test; Docker CLI is injected':false};
const operation='12345678-1234-4234-9234-123456789abc',oldImage='sha256:'+'1'.repeat(64),newImage='sha256:'+'2'.repeat(64),oldContainer='3'.repeat(64),newContainer='4'.repeat(64),repo='registry.fixture.example/multitap/broker',image=repo+'@sha256:'+'5'.repeat(64),exec=promisify(execFile);
const configuration=(root='/opt/multitap-fixture')=>({projectDirectory:root+'/project',composeFile:'compose.yaml',envFile:'.env',overrideFile:'multitap-image.json',projectName:'multitap-fixture',serviceName:'broker',dataVolume:'multitap-fixture-data',imageRepository:repo,healthPort:18480,brokerUid:1000,brokerGid:1000,updatesDirectory:root+'/updates'});
const metadata={schema:1,product:'multitap-broker',version:'0.5.24',image,platform:'linux/amd64',imageBytes:1024};
test('Docker updater configuration pins product paths, service, repository and one volume',()=>{
  const config=configuration();assert.equal(validateDockerConfig(config).serviceName,'broker');
  for(const value of [{...config,serviceName:'other'},{...config,projectDirectory:'/opt/a/../b'},{...config,composeFile:'../compose.yaml'},{...config,envFile:'https://evil'},{...config,imageRepository:'nginx:latest'},{...config,projectName:'foreign-app'},{...config,brokerUid:0},{...config,brokerGid:0},{...config,healthPort:0},{...config,extra:true}])assert.throws(()=>validateDockerConfig(value),/update_docker_config_invalid/);
});
test('Docker signed metadata requires exact repository digest, version, platform and size',()=>{
  assert.equal(validateDockerImage(metadata,'0.5.24',repo),metadata);
  for(const value of [{...metadata,image:repo+':latest'},{...metadata,image:'registry.other.example/broker@sha256:'+'5'.repeat(64)},{...metadata,image:image+';id'},{...metadata,version:'0.5.23'},{...metadata,platform:'linux/arm64'},{...metadata,imageBytes:0},{...metadata,imageBytes:3*1024*1024*1024},{...metadata,extra:true}])assert.throws(()=>validateDockerImage(value,'0.5.24',repo),/update_docker_image_invalid/);
});
test('Docker helper plan uses fixed old image and restricted mounts without a shell',()=>{
  const adapter=new DockerAdapter(configuration()),job={operationId:operation,stage:'/opt/multitap-fixture/updates/'+operation,previousImage:oldImage};
  const args=adapter.helperArgs(job,'backup','fixed-code',['/var/lib/multitap','/backup',operation]);
  assert.equal(args[0],'run');assert.ok(args.includes('--read-only'));assert.ok(args.includes('--network'));assert.ok(args.includes('none'));assert.ok(args.includes('1000:1000'));assert.ok(args.includes(oldImage));assert.ok(args.includes('type=volume,source=multitap-fixture-data,target=/var/lib/multitap,readonly'));assert.ok(!args.join(' ').includes('docker.sock'));assert.ok(!args.includes('/bin/sh'));
  const compose=adapter.compose(['up','--detach','--no-deps','--no-build','--pull','never','--force-recreate','broker']);assert.ok(!compose.includes('down'));assert.ok(!compose.includes('--remove-orphans'));assert.equal(compose.at(-1),'broker');
});

async function fixture(t){
  const root=await mkdtemp(join(tmpdir(),'multitap-docker-update-')),config=configuration(root),data=root+'/volume';
  for(const p of [config.projectDirectory,config.updatesDirectory,data,root+'/docker'])await mkdir(p,{mode:0o700});
  const files={compose:config.projectDirectory+'/'+config.composeFile,env:config.projectDirectory+'/'+config.envFile,override:config.projectDirectory+'/'+config.overrideFile};
  await writeFile(config.projectDirectory+'/multitap-update-channel.json',JSON.stringify(new DockerAdapter(config).channel()),{mode:0o600});
  await writeFile(files.compose,'services:\n  broker:\n    image: local-broker\n',{mode:0o600});await writeFile(files.env,'MULTITAP_PUBLIC_ORIGIN=https://cloud.fixture.example\n',{mode:0o600});const originalOverride=JSON.stringify({services:{broker:{image:oldImage,pull_policy:'never'}}});await writeFile(files.override,originalOverride,{mode:0o600});
  const database=new DatabaseSync(data+'/broker.sqlite');database.exec("PRAGMA user_version=11; CREATE TABLE users(name TEXT);INSERT INTO users VALUES('retained-user');");database.close();await chmod(data+'/broker.sqlite',0o600);await writeFile(data+'/vault.key',Buffer.alloc(32,7),{mode:0o600});await writeFile(data+'/initial-account.json','{"configured":true}',{mode:0o600});
  const helperRoot=root+'/helper';await mkdir(helperRoot,{mode:0o700});await mkdir(helperRoot+'/releases',{mode:0o755});await mkdir(helperRoot+'/releases/0.5.23',{mode:0o755});await writeFile(helperRoot+'/.multitap-managed-docker-updater','',{mode:0o600});await symlink(helperRoot+'/releases/0.5.23',helperRoot+'/current');
  const stage=join(config.updatesDirectory,operation);await mkdir(stage,{mode:0o700});await mkdir(stage+'/verified',{mode:0o700});await mkdir(stage+'/verified/update',{mode:0o700});const archive=stage+'/server.tar.gz';
  const packageFiles=new Map();for(const name of ['src/index.mjs','src/http.mjs','src/store.mjs','src/bootstrap.mjs','admin/index.html','admin/app.js','update/agent.mjs','update/engine.mjs','update/protocol.mjs','update/package.mjs','update/docker.mjs','update/systemd.mjs','update/deployment-health.mjs','update/transport-policy.mjs','update/server-access-policy.mjs','update/server-access-engine.mjs','update/server-access-health.mjs','update/server-access-deployment.mjs','update/server-access-docker-policy.mjs','update/restore-engine.mjs','update/restore-deployment.mjs','update/provision-broker.sh','node_modules/ws/package.json'])packageFiles.set(name,Buffer.from('// signed fixture bytes; not executed\n'));
  packageFiles.set('package.json',Buffer.from(JSON.stringify({name:'multitap-broker',version:'0.5.24'})));packageFiles.set('package-lock.json',Buffer.from(JSON.stringify({version:'0.5.24',packages:{'':{version:'0.5.24'}}})));packageFiles.set('update/docker-image.json',Buffer.from(JSON.stringify(metadata)));
  await writeFile(archive,packPackage(packageFiles),{mode:0o600});await writeFile(stage+'/verified/update/docker-image.json',JSON.stringify(metadata),{mode:0o600});
  const release={version:'0.5.24',platform:'linux-x64-docker',artifact:{sha256:digest(await readFile(archive))}};
  const calls=[],phases=[];let currentImage=oldImage,currentId=oldContainer,running=true,absent=false,newHealthy=true,failPull=false,failBackup=false,removedOnUp=false,foreignHelper=false,mutateAfterPull=false;
  const labels=()=>({'com.docker.compose.project':config.projectName,'com.docker.compose.service':'broker','com.docker.compose.oneoff':'False','com.docker.compose.config-hash':'6'.repeat(64)});
  const container=()=>({Id:currentId,Image:currentImage,Config:{User:'1000:1000',Labels:labels()},HostConfig:{Privileged:false,ReadonlyRootfs:true,CapDrop:['ALL'],NetworkMode:'multitap-fixture_default',PortBindings:{'18480/tcp':[{HostPort:'18480',HostIp:''}]}},State:{Running:running},Mounts:[{Destination:'/var/lib/multitap',Type:'volume',Name:config.dataVolume,RW:true}],NetworkSettings:{Ports:running?{'18480/tcp':[{HostPort:'18480',HostIp:'0.0.0.0'}]}:{}}});
  const run=async args=>{
    calls.push([...args]);
    if(args[0]==='compose'&&args.includes('ps'))return absent?'':currentId+'\n';
    if(args[0]==='container'&&args[1]==='inspect')return JSON.stringify([container()]);
    if(args[0]==='volume'&&args[1]==='inspect')return JSON.stringify([{Name:config.dataVolume,Driver:'local',Scope:'local',Mountpoint:data,Options:null}]);
    if(args[0]==='info')return JSON.stringify(root+'/docker');
    if(args[0]==='image'&&args[1]==='pull'){if(failPull)throw Error('update_docker_command_failed');if(mutateAfterPull)await writeFile(files.env,'changed-root-config',{mode:0o600});return '';}
    if(args[0]==='image'&&args[1]==='inspect')return JSON.stringify([{Id:newImage,Os:'linux',Architecture:'amd64',Size:1024,RepoDigests:[image],Config:{Labels:{'org.opencontainers.image.version':'0.5.24','org.opencontainers.image.title':'multitap-broker'}}}]);
    if(args[0]==='container'&&args[1]==='ls')return foreignHelper?'7'.repeat(64):'';
    if(args[0]==='container'&&args[1]==='stop'){assert.equal(args.at(-1),currentId);running=false;return currentId;}
    if(args[0]==='container'&&args[1]==='start'){assert.equal(args.at(-1),oldContainer);running=true;return currentId;}
    if(args[0]==='run'){
      const codeIndex=args.indexOf('-e'),code=args[codeIndex+1],role=args.find(v=>v.startsWith('me.multitap.update.role=')).split('=')[1];
      assert.equal(args[codeIndex-2],oldImage);assert.equal(running,false);assert.ok(args.includes('--rm'));assert.ok(args.includes('--read-only'));assert.ok(args.includes('none'));assert.ok(args.includes('ALL'));assert.ok(!args.includes('--privileged'));
      if(role==='backup'&&failBackup)throw Error('update_docker_command_failed');
      const fixed=args.slice(codeIndex+2).map(v=>v==='/var/lib/multitap'?data:v==='/backup'?stage+'/backup':v);
      const result=await exec(process.execPath,['--input-type=module','-e',code,...fixed],{maxBuffer:1024*1024});return result.stdout;
    }
    if(args[0]==='compose'&&args.includes('up')){
      assert.equal(args.at(-1),'broker');assert.ok(args.includes('--no-deps'));assert.ok(args.includes('--no-build'));assert.ok(args.includes('never'));assert.ok(!args.includes('down'));
      const override=JSON.parse(await readFile(files.override,'utf8')).services.broker.image;
      if(override===image){currentImage=newImage;currentId=newContainer;if(removedOnUp){absent=true;throw Error('update_docker_command_failed');}const db=new DatabaseSync(data+'/broker.sqlite');db.exec("UPDATE users SET name='new-version-write';PRAGMA user_version=12;");db.close();}
      else{assert.equal(override,oldImage);currentImage=oldImage;currentId=oldContainer;absent=false;}
      running=true;return '';
    }
    throw Error('Unexpected injected Docker command '+JSON.stringify(args));
  };
  // Real target files and SQLite; ancestor/root deployment ownership and Docker
  // execution are explicitly injected here, not a claimed daemon/container test.
  const trust=async(path,kind='file')=>{const s=await lstat(path);assert.equal(s.isSymbolicLink(),false);assert.equal(kind==='directory'?s.isDirectory():s.isFile(),true);assert.equal(s.uid,0);assert.equal(s.mode&0o022,0);};
  const adapter=new DockerAdapter(config,{run,trust,helperRoot,health:async()=>absent||!running?null:currentImage===newImage?(newHealthy?'0.5.24':null):'0.5.23',sleep:async()=>{},space:async()=>({bavail:4*1024*1024,bsize:1024})});
  t.diagnostic('Injected Docker CLI; actual temporary SQLite/file helpers; fixture retained '+root);
  return {adapter,config,stage,archive,release,data,helperRoot,packageFiles,files,calls,phases,originalOverride,input:{operationId:operation,stage,archive,release,fromVersion:'0.5.23'},phase:async p=>phases.push(p),set:value=>{if('newHealthy'in value)newHealthy=value.newHealthy;if('failPull'in value)failPull=value.failPull;if('failBackup'in value)failBackup=value.failBackup;if('removedOnUp'in value)removedOnUp=value.removedOnUp;if('foreignHelper'in value)foreignHelper=value.foreignHelper;if('mutateAfterPull'in value)mutateAfterPull=value.mutateAfterPull;},container};
}
test('Docker pull and image validation happen before any broker stop',linux,async t=>{
  const f=await fixture(t);f.set({failPull:true});await assert.rejects(f.adapter.prepare(f.input),/update_docker_command_failed/);assert.ok(f.calls.some(a=>a[0]==='image'&&a[1]==='pull'));assert.ok(!f.calls.some(a=>a[0]==='container'&&a[1]==='stop'));assert.equal(f.container().State.Running,true);
});
test('Docker apply snapshots actual SQLite/vault then recreates only owned service',linux,async t=>{
  const f=await fixture(t),job=await f.adapter.prepare(f.input);await f.adapter.apply(job,f.phase);assert.equal(await f.adapter.version(),'0.5.24');assert.deepEqual(f.phases,['backing_up','applying','restarting','health_check']);const db=new DatabaseSync(f.stage+'/backup/broker.sqlite',{readOnly:true});assert.equal(db.prepare('SELECT name FROM users').get().name,'retained-user');assert.equal(db.prepare('PRAGMA user_version').get().user_version,11);db.close();assert.equal((await f.adapter.state(job)).phase,'succeeded');assert.ok(f.calls.findIndex(a=>a[0]==='image'&&a[1]==='pull')<f.calls.findIndex(a=>a[0]==='container'&&a[1]==='stop'));assert.ok(!f.calls.some(a=>a.includes('down')||a.includes('volume')&&a.includes('rm')));
});
test('healthy Docker update publishes only signed helper bytes and switches its root pointer',linux,async t=>{
  const f=await fixture(t),job=await f.adapter.prepare(f.input);await writeFile(f.stage+'/verified/update/agent.mjs','unsigned staged edit',{mode:0o600});await f.adapter.apply(job,f.phase);assert.equal(await f.adapter.helperCurrent(),f.adapter.nextHelper(job));assert.deepEqual(await readFile(f.adapter.nextHelper(job)+'/update/agent.mjs'),f.packageFiles.get('update/agent.mjs'));await f.adapter.save(job,'health_check');assert.equal(await f.adapter.recover(job,f.phase),true);assert.equal(await f.adapter.helperCurrent(),job.previousHelper);assert.equal(await f.adapter.version(),'0.5.23');
});
test('unrecognized helper file refuses promotion and leaves original helper executable selected',linux,async t=>{
  const f=await fixture(t),job=await f.adapter.prepare(f.input),target=f.adapter.nextHelper(job);await mkdir(target,{mode:0o755});await writeFile(target+'/not-in-signed-release.mjs','unexpected',{mode:0o644});await assert.rejects(f.adapter.apply(job,f.phase),/update_docker_installation_changed/);assert.equal(await f.adapter.helperCurrent(),job.previousHelper);assert.equal(await f.adapter.recover(job,f.phase),true);
});
test('failed Docker health restores old image, SQLite schema and protected account files',linux,async t=>{
  const f=await fixture(t),job=await f.adapter.prepare(f.input),vault=digest(await readFile(f.data+'/vault.key'));f.set({newHealthy:false});await assert.rejects(f.adapter.apply(job,f.phase),/update_health_failed/);assert.equal(await f.adapter.recover(job,f.phase),true);const db=new DatabaseSync(f.data+'/broker.sqlite',{readOnly:true});assert.equal(db.prepare('SELECT name FROM users').get().name,'retained-user');assert.equal(db.prepare('PRAGMA user_version').get().user_version,11);db.close();assert.equal(digest(await readFile(f.data+'/vault.key')),vault);assert.equal(await readFile(f.files.override,'utf8'),f.originalOverride);assert.ok((await readdir(f.data)).some(n=>n.startsWith('.multitap-update-failed-')));assert.equal(await f.adapter.version(),'0.5.23');assert.equal((await f.adapter.state(job)).phase,'restored');
});
test('backup failure restarts original stopped container without replacing live database',linux,async t=>{
  const f=await fixture(t),job=await f.adapter.prepare(f.input),before=digest(await readFile(f.data+'/broker.sqlite'));f.set({failBackup:true});await assert.rejects(f.adapter.apply(job,f.phase));assert.equal((await f.adapter.state(job)).phase,'stopped');assert.equal(await f.adapter.recover(job,f.phase),true);assert.equal(digest(await readFile(f.data+'/broker.sqlite')),before);assert.ok(!f.calls.some(a=>a[0]==='compose'&&a.includes('up')));
});
test('lost restore ACK never replays backup over writes accepted by recovered old broker',linux,async t=>{
  const f=await fixture(t),job=await f.adapter.prepare(f.input);f.set({newHealthy:false});await assert.rejects(f.adapter.apply(job,f.phase));assert.equal(await f.adapter.recover(job,f.phase),true);const state=await f.adapter.state(job);await f.adapter.save(job,'restore_ready',{backupSha:state.backupSha});const db=new DatabaseSync(f.data+'/broker.sqlite');db.exec("UPDATE users SET name='legitimate-after-recovery'");db.close();const restores=f.calls.filter(a=>a[0]==='run'&&a.includes('me.multitap.update.role=restore')).length;assert.equal(await f.adapter.recover(job,f.phase),true);assert.equal(f.calls.filter(a=>a[0]==='run'&&a.includes('me.multitap.update.role=restore')).length,restores);const after=new DatabaseSync(f.data+'/broker.sqlite',{readOnly:true});assert.equal(after.prepare('SELECT name FROM users').get().name,'legitimate-after-recovery');after.close();
});
test('failed recreation with no remaining container still restores committed backup',linux,async t=>{
  const f=await fixture(t),job=await f.adapter.prepare(f.input);f.set({removedOnUp:true});await assert.rejects(f.adapter.apply(job,f.phase),/update_docker_command_failed/);assert.equal(await f.adapter.recover(job,f.phase),true);assert.equal(await f.adapter.version(),'0.5.23');
});
test('changed root Compose inputs after image pull prevent any stop',linux,async t=>{
  const f=await fixture(t);f.set({mutateAfterPull:true});await assert.rejects(f.adapter.prepare(f.input),/update_version_changed/);assert.ok(!f.calls.some(a=>a[0]==='container'&&a[1]==='stop'));
});
test('damaged backup refuses rollback before stopping the currently running image',linux,async t=>{
  const f=await fixture(t),job=await f.adapter.prepare(f.input);f.set({newHealthy:false});await assert.rejects(f.adapter.apply(job,f.phase));await writeFile(f.stage+'/backup/vault.key',Buffer.alloc(32,8));const before=f.calls.filter(a=>a[0]==='container'&&a[1]==='stop').length;await assert.rejects(f.adapter.recover(job,f.phase),/update_backup_invalid/);assert.equal(f.calls.filter(a=>a[0]==='container'&&a[1]==='stop').length,before);
});
test('archive checksum and root protection fail before Docker pull',linux,async t=>{
  const f=await fixture(t);await writeFile(f.archive,'changed archive');await assert.rejects(f.adapter.prepare(f.input),/update_checksum_failed/);assert.ok(!f.calls.some(a=>a[0]==='image'&&a[1]==='pull'));await chmod(f.files.env,0o666);await assert.rejects(f.adapter.installationCheck());
});
test('actual root guard rejects symlinks and writable ancestors',linux,async t=>{
  const root=await mkdtemp(join(tmpdir(),'multitap-docker-guard-')),file=root+'/config.json';await writeFile(file,'{}',{mode:0o600});await symlink(file,root+'/link');await assert.rejects(trustedDockerPath(root+'/link'),/update_docker_installation_invalid/);await assert.rejects(trustedDockerPath(file),/update_docker_installation_invalid/);t.diagnostic('Guard rejects /tmp ancestor by design; no mutable /tmp path can become a production root config');
});
test('unverified surviving helper cannot be stopped as the broker or borrowed for recovery',linux,async t=>{
  const f=await fixture(t),job=await f.adapter.prepare(f.input);f.set({foreignHelper:true});await assert.rejects(f.adapter.apply(job,f.phase),/update_docker_helper_unverified/);assert.ok(!f.calls.some(a=>a[0]==='container'&&a[1]==='stop'));
});
test('wrong pulled-image digest or image version cannot reach broker stop',linux,async t=>{
  for(const change of [v=>v.RepoDigests=[],v=>v.Config.Labels['org.opencontainers.image.version']='0.5.999']){
    const f=await fixture(t),run=f.adapter.run;f.adapter.run=async args=>{const output=await run(args);if(args[0]==='image'&&args[1]==='inspect'){const parsed=JSON.parse(output);change(parsed[0]);return JSON.stringify(parsed);}return output;};
    await assert.rejects(f.adapter.prepare(f.input),/update_docker_image_invalid/);assert.ok(!f.calls.some(a=>a[0]==='container'&&a[1]==='stop'));
  }
});
test('Docker broker cannot expose host Docker socket or another project/data volume',linux,async t=>{
  for(const change of [v=>v.Config.Labels['com.docker.compose.project']='another-project',v=>v.Mounts.push({Type:'bind',Source:'/var/run/docker.sock',Destination:'/var/run/docker.sock',RW:true}),v=>v.Mounts[0].Name='foreign-data']){
    const f=await fixture(t),run=f.adapter.run;f.adapter.run=async args=>{const output=await run(args);if(args[0]==='container'&&args[1]==='inspect'){const parsed=JSON.parse(output);change(parsed[0]);return JSON.stringify(parsed);}return output;};await assert.rejects(f.adapter.prepare(f.input));assert.ok(!f.calls.some(a=>a[0]==='image'&&a[1]==='pull'));
  }
});
test('Docker installer Bash and embedded Node syntax parse without daemon execution',async t=>{
  const source=await readFile(new URL('../../deploy/linux/install-docker-updater.sh',import.meta.url),'utf8');
  const scripts=[...source.matchAll(/<<'NODE'\r?\n([\s\S]*?)\r?\nNODE/g)].map(m=>m[1]);assert.equal(scripts.length,3);
  for(const script of scripts){const r=spawnSync(process.execPath,['--input-type=module','--check'],{input:script,encoding:'utf8'});assert.equal(r.status,0,r.stderr);}
  if(process.platform==='linux'){const r=spawnSync('/bin/bash',['-n'],{input:source,encoding:'utf8'});assert.equal(r.status,0,r.stderr);}
  t.diagnostic('Syntax only. Installer, registry, Docker Engine and containers were not run.');
});


test('missing shared transport helper is rejected before image pull or broker stop',linux,async t=>{
  const f=await fixture(t);f.packageFiles.delete('update/deployment-health.mjs');
  const bytes=packPackage(f.packageFiles);await writeFile(f.archive,bytes,{mode:0o600});
  f.input.release.artifact.sha256=digest(bytes);
  await assert.rejects(f.adapter.prepare(f.input),/update_archive_incomplete/);
  assert.ok(!f.calls.some(a=>a[0]==='image'&&a[1]==='pull'));
  assert.ok(!f.calls.some(a=>a[0]==='container'&&a[1]==='stop'));
});

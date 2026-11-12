import { execFile, spawn } from 'node:child_process';
import { promisify } from 'node:util';
import { randomUUID } from 'node:crypto';
import { lstat, realpath, readFile, rename, symlink, unlink, open, mkdir } from 'node:fs/promises';
import { constants } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { digest, versionPattern } from './protocol.mjs';
import { readDeploymentEnvironment, localBrokerHealth } from './deployment-health.mjs';

const execute=promisify(execFile),root='/opt/multitap',data='/var/lib/multitap';
// The fixed provisioner bounds network/health waits itself. Do not kill its
// parent shell on a generic deadline and race rollback against a live child.
const run=(program,args)=>execute(program,args,{maxBuffer:256*1024,env:{PATH:'/usr/sbin:/usr/bin:/sbin:/bin',LANG:'C.UTF-8'}});
async function provisionRun(path,args,phase){
  const child=spawn('/bin/bash',[path,...args],{stdio:['ignore','pipe','pipe'],env:{PATH:'/usr/sbin:/usr/bin:/sbin:/bin',LANG:'C.UTF-8'}});
  const result=new Promise((resolve,reject)=>{child.once('error',reject);child.once('close',(code,signal)=>resolve({code,signal}));});
  let stdout='',pending='',progress=Promise.resolve();
  const output=(async()=>{for await(const b of child.stdout){const s=b.toString('utf8');if(stdout.length<262144)stdout+=s;pending+=s;let index;while((index=pending.indexOf('\n'))>=0){const line=pending.slice(0,index).trim();pending=pending.slice(index+1);const m=/^MULTITAP_UPDATE_PHASE (backing_up|applying|restarting|health_check)$/.exec(line);if(m)progress=progress.then(()=>phase(m[1]));}if(pending.length>8192)pending='';}})();
  const errors=(async()=>{for await(const _b of child.stderr){/* drain fixed tool output; never return private paths/errors to the browser */}})();
  const done=await result;await Promise.all([output,errors]);await progress;
  if(done.code!==0||done.signal)throw Error('update_apply_failed');return {stdout};
}
async function trusted(path){const s=await lstat(path);if(s.isSymbolicLink()||s.uid!==0||(s.mode&0o022))throw Error('update_installation_invalid');return s;}
async function optionalHash(path){try{const s=await lstat(path);if(!s.isFile()||s.isSymbolicLink())throw Error('update_installation_invalid');return digest(await readFile(path));}catch(e){if(e.code==='ENOENT')return null;throw e;}}
const updaterUnitPath='/etc/systemd/system/multitap-updater.service';
const updaterUnitText=bytes=>{try{return new TextDecoder('utf-8',{fatal:true}).decode(bytes);}catch{throw Error('update_updater_unit_invalid');}};
export function planManagedUpdaterUnit(text){
  if(typeof text!=='string'||Buffer.byteLength(text)>32768||text.includes('\0')||!text.startsWith('# Multitap managed updater\n')&&!text.startsWith('# Multitap managed updater\r\n'))throw Error('update_updater_unit_invalid');
  for(const [key,value]of [['User','root'],['Group','multitap'],['ProtectSystem','strict'],['Restart','always'],['RuntimeDirectory','multitap-update'],['ExecStart','/opt/multitap/runtime/node-v24.22.0-linux-x64/bin/node --preserve-symlinks-main /opt/multitap/current/update/agent.mjs']]){
    const rows=[...text.matchAll(new RegExp('^'+key+'=([^\\r\\n]*)\\r?$','gm'))];if(rows.length!==1||rows[0][1]!==value)throw Error('update_updater_unit_invalid');
  }
  const rows=[...text.matchAll(/^ReadWritePaths=([^\r\n]*)\r?$/gm)];if(rows.length!==1)throw Error('update_updater_unit_invalid');
  const paths=rows[0][1].split(/\s+/).filter(Boolean);
  if(paths.length<1||paths.some(value=>!/^[-+]?\/[A-Za-z0-9_./-]+$/.test(value)||value.split('/').includes('..'))||!paths.includes('/etc/systemd/system'))throw Error('update_updater_unit_invalid');
  if(paths.includes('/etc/multitap')||paths.includes('-/etc/multitap'))return {before:text,after:text,changed:false};
  const after=text.replace(/^ReadWritePaths=([^\r\n]*)/m,(_line,value)=>'ReadWritePaths='+value+' /etc/multitap');
  return {before:text,after,changed:true};
}
async function updaterUnitBytes(path){
  const stat=await trusted(path);if(!stat.isFile()||stat.size>32768)throw Error('update_updater_unit_invalid');
  const file=await open(path,'r');try{const opened=await file.stat();if(opened.dev!==stat.dev||opened.ino!==stat.ino||!opened.isFile()||opened.uid!==0||(opened.mode&0o022))throw Error('update_updater_unit_invalid');const bytes=await file.readFile();if(bytes.length>32768)throw Error('update_updater_unit_invalid');return {bytes,stat:opened};}finally{await file.close();}
}
async function immutableUnitFile(path,bytes){
  let file;try{file=await open(path,'wx',0o600);}catch(error){if(error.code!=='EEXIST')throw error;const prior=await updaterUnitBytes(path);if(digest(prior.bytes)!==digest(bytes))throw Error('update_updater_unit_changed');return;}
  try{await file.chmod(0o600);await file.writeFile(bytes);await file.sync();}finally{await file.close();}
  const dir=await open(join(path,'..'),'r');try{await dir.sync();}finally{await dir.close();}
}
export async function prepareUpdaterUnitMigration(stage,{unitPath=updaterUnitPath}={}){
  const directory=await trusted(stage);if(!directory.isDirectory()||await realpath(stage)!==stage)throw Error('update_updater_unit_invalid');
  let original;try{original=await updaterUnitBytes(unitPath);}catch(error){if(error.code==='ENOENT')return null;throw error;}
  const plan=planManagedUpdaterUnit(updaterUnitText(original.bytes)),descriptor={schema:1,stage,unitPath,backupPath:join(stage,'updater-unit.before'),beforeSha256:digest(original.bytes),afterSha256:digest(Buffer.from(plan.after)),mode:original.stat.mode&0o777,gid:original.stat.gid,changed:plan.changed};
  await immutableUnitFile(descriptor.backupPath,original.bytes);
  await immutableUnitFile(join(stage,'updater-unit.plan.json'),Buffer.from(JSON.stringify(descriptor)));
  return descriptor;
}
async function readUpdaterUnitPlan(stage,{unitPath=updaterUnitPath}={}){
  try{const directory=await trusted(stage);if(!directory.isDirectory()||await realpath(stage)!==stage)throw Error('update_updater_unit_invalid');}catch(error){if(error.code==='ENOENT')return null;throw error;}
  let bytes;try{bytes=(await updaterUnitBytes(join(stage,'updater-unit.plan.json'))).bytes;}catch(error){if(error.code==='ENOENT')return null;throw error;}
  const value=JSON.parse(bytes);
  if(!value||value.schema!==1||value.stage!==stage||value.unitPath!==unitPath||value.backupPath!==join(stage,'updater-unit.before')||!Number.isInteger(value.mode)||value.mode<0||value.mode>0o777||(value.mode&0o022)||!Number.isInteger(value.gid)||value.gid<0||typeof value.changed!=='boolean')throw Error('update_updater_unit_invalid');
  const backup=await updaterUnitBytes(value.backupPath),plan=planManagedUpdaterUnit(updaterUnitText(backup.bytes));
  if(value.beforeSha256!==digest(backup.bytes)||value.afterSha256!==digest(Buffer.from(plan.after))||value.changed!==plan.changed)throw Error('update_updater_unit_changed');
  return {...value,before:backup.bytes,after:Buffer.from(plan.after)};
}
export async function verifyUpdaterUnitMigration(descriptor,{expected='either',unitPath=updaterUnitPath}={}){
  if(!descriptor)return;
  const plan=await readUpdaterUnitPlan(descriptor.stage,{unitPath});if(!plan)throw Error('update_updater_unit_invalid');
  for(const key of ['schema','stage','unitPath','backupPath','beforeSha256','afterSha256','mode','gid','changed'])if(plan[key]!==descriptor[key])throw Error('update_updater_unit_changed');
  const current=await updaterUnitBytes(unitPath),sha=digest(current.bytes);
  const allowed=expected==='before'?[plan.beforeSha256]:expected==='after'?[plan.afterSha256]:[plan.beforeSha256,plan.afterSha256];
  if(!allowed.includes(sha)||(current.stat.mode&0o777)!==plan.mode||current.stat.gid!==plan.gid)throw Error('update_updater_unit_changed');
}
export async function applyUpdaterUnitMigration(stage,{restore=false,unitPath=updaterUnitPath,reload=()=>run('/bin/systemctl',['daemon-reload'])}={}){
  const plan=await readUpdaterUnitPlan(stage,{unitPath});if(!plan)return;
  await verifyUpdaterUnitMigration(plan,{unitPath});
  if(!plan.changed)return;
  const wanted=restore?plan.before:plan.after,current=await updaterUnitBytes(unitPath);
  if(digest(current.bytes)!==digest(wanted)){
    const temporary=unitPath+'.multitap-'+randomUUID();
    try{
      const file=await open(temporary,'wx',plan.mode);
      try{await file.chown(0,plan.gid);await file.chmod(plan.mode);await file.writeFile(wanted);await file.sync();}finally{await file.close();}
      // Never replace a unit changed by another administrator after validation.
      const last=await updaterUnitBytes(unitPath);if(digest(last.bytes)!==digest(current.bytes)||(last.stat.mode&0o777)!==plan.mode||last.stat.gid!==plan.gid)throw Error('update_updater_unit_changed');
      await rename(temporary,unitPath);const parent=await open(join(unitPath,'..'),'r');try{await parent.sync();}finally{await parent.close();}
    }finally{await unlink(temporary).catch(error=>{if(error.code!=='ENOENT')throw error;});}
  }
  // Reload definitions only. The executing updater keeps its namespace and
  // completes the durable update/rollback before its existing self-refresh.
  await reload();await verifyUpdaterUnitMigration(plan,{expected:restore?'before':'after',unitPath});
}
export async function migrateUpdaterUnitAtStartup(directory,{unitPath=updaterUnitPath,reload=()=>run('/bin/systemctl',['daemon-reload'])}={}){
  // An old updater invokes its old provisioner. Its first new executable must
  // therefore migrate the unit before opening a socket or accepting work. A
  // changed definition takes effect only after this startup returns normally
  // and Restart=always creates the next process's mount namespace.
  let original;try{original=await updaterUnitBytes(unitPath);}catch(error){if(error.code==='ENOENT')return false;throw error;}
  const parent=await trusted(directory);if(!parent.isDirectory()||await realpath(directory)!==directory)throw Error('update_updater_unit_invalid');
  const stage=join(directory,'updater-unit-migration-v1');
  const existing=await readUpdaterUnitPlan(stage,{unitPath});
  if(!existing&&!planManagedUpdaterUnit(updaterUnitText(original.bytes)).changed)return false;
  await mkdir(stage,{mode:0o700}).catch(error=>{if(error.code!=='EEXIST')throw error;});
  const descriptor=existing??await prepareUpdaterUnitMigration(stage,{unitPath});
  const completionPath=join(stage,'updater-unit.reloaded.json'),completion=Buffer.from(JSON.stringify({schema:1,afterSha256:descriptor.afterSha256}));
  let reloaded=false;try{const prior=await updaterUnitBytes(completionPath);if(digest(prior.bytes)!==digest(completion))throw Error('update_updater_unit_changed');reloaded=true;}catch(error){if(error.code!=='ENOENT')throw error;}
  await verifyUpdaterUnitMigration(descriptor,{unitPath});
  if(reloaded&&digest(original.bytes)===descriptor.afterSha256)return false;
  try{
    // A crash after rename but before daemon-reload/this durable checkpoint
    // retries the reload and one startup exit; the file alone is not proof.
    await applyUpdaterUnitMigration(stage,{unitPath,reload});
    await immutableUnitFile(completionPath,completion);return true;
  }
  catch(error){
    try{await applyUpdaterUnitMigration(stage,{restore:true,unitPath,reload});}
    catch{throw Error('update_updater_unit_recovery_required');}
    throw Error('update_updater_unit_migration_failed');
  }
}
const currentPath=async()=>{
  await trusted(root);await trusted(root+'/releases');await trusted(root+'/.multitap-managed');
  const link=await lstat(root+'/current');if(!link.isSymbolicLink()||link.uid!==0)throw Error('update_installation_invalid');
  const path=await realpath(root+'/current');if(!/^\/opt\/multitap\/releases\/[A-Za-z0-9._-]+$/.test(path))throw Error('update_installation_invalid');await trusted(path);return path;
};
async function health(version){
  try{const config=await readDeploymentEnvironment('/etc/multitap/broker.env'),value=await localBrokerHealth(config);return value?.version===version;}catch{return false;}
}
const waitHealth=async(version)=>{for(let i=0;i<25;i++){if(await health(version))return true;await new Promise(resolve=>setTimeout(resolve,500));}return false;};
// Copies a file of the broker's own data directory as root without following
// a link planted there: opened with O_NOFOLLOW, it must be a regular file with
// one name owned by that account (as restore-deployment.mjs copy() demands).
// The copy is made exclusively, then given to the account through its handle.
export async function copyBrokerFile(from,to,owner){
  const source=await open(from,constants.O_RDONLY|(constants.O_NOFOLLOW??0));let target;
  try{
    const s=await source.stat();if(!s.isFile()||s.nlink!==1||s.uid!==owner.uid)throw Error('update_backup_invalid');
    target=await open(to,'wx',0o600);if(process.platform!=='win32'){await target.chown(owner.uid,owner.gid);await target.chmod(0o600);}
    const buffer=Buffer.allocUnsafe(1048576);let position=0;
    while(true){const {bytesRead}=await source.read(buffer,0,buffer.length,position);if(!bytesRead)break;position+=bytesRead;await target.write(buffer.subarray(0,bytesRead));}
    await target.sync();
  }finally{await source.close();await target?.close();}
}
async function checkBackup(path,expectedSchema){
  const s=await lstat(path);if(!s.isFile()||s.isSymbolicLink()||s.size<512)throw Error('update_backup_invalid');
  const {DatabaseSync}=await import('node:sqlite'),db=new DatabaseSync(path,{readOnly:true});
  try{const rows=db.prepare('PRAGMA quick_check').all();if(rows.length!==1||Object.values(rows[0])[0]!=='ok'||db.prepare('PRAGMA user_version').get().user_version!==expectedSchema)throw Error('update_backup_invalid');}finally{db.close();}
}
export class SystemdAdapter {
  platform='linux-x64-systemd';
  async version(){const path=await currentPath(),p=JSON.parse(await readFile(join(path,'package.json'),'utf8'));if(p.name!=='multitap-broker'||!versionPattern.test(p.version))throw Error('update_installation_invalid');return p.version;}
  async prepare({operationId,release,archive,stage,fromVersion}){
    if(process.arch!=='x64')throw Error('update_platform_unsupported');
    const previous=await currentPath();if(await this.version()!==fromVersion)throw Error('update_version_changed');
    if(stage!=='/opt/multitap-stage/updates/'+operationId||!/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/.test(operationId))throw Error('update_job_invalid');
    const updaterUnit=await prepareUpdaterUnitMigration(stage);
    const service='/etc/systemd/system/multitap-broker.service';await trusted(service);
    if(!(await readFile(service,'utf8')).includes('# Multitap managed broker'))throw Error('update_installation_invalid');
    const envPath='/etc/multitap/broker.env';await trusted(envPath);
    // The protected installation file owns scheme, consent and the real health
    // port. 'keep' preserves every original byte through apply and rollback.
    const origin=(await readDeploymentEnvironment(envPath)).publicOrigin;
    const {DatabaseSync}=await import('node:sqlite'),db=new DatabaseSync(data+'/broker.sqlite',{readOnly:true});let schema;
    try{schema=db.prepare('PRAGMA user_version').get().user_version;}finally{db.close();}
    const name=release.version+'-u'+operationId.replaceAll('-','');
    return {operationId,archive,stage,fromVersion,toVersion:release.version,sha256:release.artifact.sha256,previous,name,origin,schema,updaterUnit,
      preserved:{vault:await optionalHash(data+'/vault.key'),env:await optionalHash(envPath),initial:await optionalHash(data+'/initial-account.json'),access:await optionalHash(data+'/server-access.json'),accessIdentity:await optionalHash(data+'/server-access.identity')}};
  }
  async validate(job){
    if(!job||!versionPattern.test(job.fromVersion)||!versionPattern.test(job.toVersion)||!/^\d+\.\d+\.\d+-u[a-f0-9]{32}$/.test(job.name)||!/^\/opt\/multitap\/releases\/[A-Za-z0-9._-]+$/.test(job.previous)||!/^\/opt\/multitap-stage\/updates\/[a-f0-9-]{36}$/.test(job.stage)||job.archive!==job.stage+'/server.tar.gz'||!/^[a-f0-9]{64}$/.test(job.sha256)||!Number.isSafeInteger(job.schema))throw Error('update_job_invalid');
    if(!/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/.test(job.operationId)||job.stage!=='/opt/multitap-stage/updates/'+job.operationId||job.name!==job.toVersion+'-u'+job.operationId.replaceAll('-','')||job.previous===root+'/releases/'+job.name||job.schema<1||job.schema>1000)throw Error('update_job_invalid');
    await trusted(job.stage);await trusted(job.archive);await trusted(job.previous);
    if(digest(await readFile(job.archive))!==job.sha256)throw Error('update_checksum_failed');
    if(job.updaterUnit){if(job.updaterUnit.stage!==job.stage)throw Error('update_job_invalid');await verifyUpdaterUnitMigration(job.updaterUnit);}
  }
  async preserved(job){if(await optionalHash(data+'/vault.key')!==job.preserved.vault||await optionalHash('/etc/multitap/broker.env')!==job.preserved.env||await optionalHash(data+'/initial-account.json')!==job.preserved.initial||(Object.hasOwn(job.preserved,'access')&&await optionalHash(data+'/server-access.json')!==job.preserved.access)||(Object.hasOwn(job.preserved,'accessIdentity')&&await optionalHash(data+'/server-access.identity')!==job.preserved.accessIdentity))throw Error('update_preservation_failed');}
  async apply(job,phase){
    await this.validate(job);if(await currentPath()!==job.previous||await this.version()!==job.fromVersion)throw Error('update_version_changed');
    const provision=fileURLToPath(new URL('./provision-broker.sh',import.meta.url));await trusted(provision);
    // This is a fixed owned deployment script with fixed validated arguments.
    // Browser input cannot select an executable, service, shell or path.
    await phase('backing_up');
    const result=await provisionRun(provision,[job.archive,job.sha256,job.name,'keep',job.origin],phase);
    if(!result.stdout.includes('MULTITAP_UPDATE_PHASE complete'))throw Error('update_apply_failed');
    await phase('health_check');
    if(await currentPath()!==root+'/releases/'+job.name||!(await waitHealth(job.toVersion)))throw Error('update_health_failed');
    await this.preserved(job);
    await verifyUpdaterUnitMigration(job.updaterUnit,{expected:'after'});
  }
  async recover(job,phase){
    if(!job)return true;
    await this.validate(job);
    const current=await currentPath();
    if(![job.previous,root+'/releases/'+job.name].includes(current))throw Error('update_installation_changed');
    if(current===job.previous&&await health(job.fromVersion)){
      // The provisioner may already have rolled back. Replacing that live DB
      // again would discard legitimate writes accepted after its recovery.
      await checkBackup(data+'/broker.sqlite',job.schema);await applyUpdaterUnitMigration(job.stage,{restore:true});await this.preserved(job);return true;
    }
    const backup=data+'/backups/broker-'+job.name+'.sqlite';let exists=true;try{await lstat(backup);}catch(e){if(e.code==='ENOENT')exists=false;else throw e;}
    // Provision failed before a validated backup or migration: original code,
    // original DB and a healthy original service are a safe unchanged outcome.
    if(!exists){if(current===job.previous&&await health(job.fromVersion)){await applyUpdaterUnitMigration(job.stage,{restore:true});await this.preserved(job);return true;}throw Error('update_recovery_required');}
    await checkBackup(backup,job.schema);
    await phase('restarting');await run('/bin/systemctl',['stop','multitap-broker']);
    // Stop success is required before replacing SQLite/WAL. Preserve the
    // migrated database as a private failure artifact rather than deleting it.
    const suffix='.update-'+job.operationId;
    const restore=data+'/broker.sqlite.restore-'+job.operationId;
    // Checked again now that the broker is stopped: a plain file of the broker's account, copied without following links.
    const owner=await lstat(data);if(!owner.isDirectory()||owner.isSymbolicLink())throw Error('update_installation_invalid');
    await copyBrokerFile(backup,restore,owner);await checkBackup(restore,job.schema);
    for(const name of ['broker.sqlite','broker.sqlite-wal','broker.sqlite-shm']){
      try{await rename(data+'/'+name,data+'/'+name+suffix);}catch(e){if(e.code!=='ENOENT')throw e;}
    }
    await rename(restore,data+'/broker.sqlite');
    const link=root+'/current.restore-'+job.operationId;await symlink(job.previous,link);await rename(link,root+'/current');
    await this.preserved(job);await run('/bin/systemctl',['restart','multitap-broker']);await phase('health_check');
    const healthy=await currentPath()===job.previous&&await waitHealth(job.fromVersion);
    await applyUpdaterUnitMigration(job.stage,{restore:true});return healthy;
  }
}

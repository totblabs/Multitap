import { execFile, spawn } from 'node:child_process';
import { promisify } from 'node:util';
import { readFile, lstat, mkdir, chmod, chown, open, rename, realpath, symlink, readlink, readdir } from 'node:fs/promises';
import { dirname, join, posix } from 'node:path';
import { atomicJson } from './engine.mjs';
import { digest, versionPattern, compareVersion } from './protocol.mjs';
import { unpackPackage } from './package.mjs';
import { deploymentEnvironment, localBrokerHealth, validBootstrapLocalHealth } from './deployment-health.mjs';
import { decodeDockerAccessManifest, renderDockerAccessOverlay, inspectDockerBindings } from './server-access-docker-policy.mjs';
import { serverAccessTransport, legacyServerAccess, serverAccessHash } from './server-access-policy.mjs';
import { deploymentEnvironmentValues } from './deployment-health.mjs';

const helperModules=['agent.mjs','engine.mjs','protocol.mjs','package.mjs','docker.mjs','systemd.mjs','deployment-health.mjs','transport-policy.mjs','server-access-policy.mjs','server-access-engine.mjs','server-access-health.mjs','server-access-deployment.mjs','server-access-docker-policy.mjs','restore-engine.mjs','restore-deployment.mjs'];
const execute=promisify(execFile),uuid=/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/,sha=/^[a-f0-9]{64}$/,imageId=/^sha256:[a-f0-9]{64}$/,containerId=/^[a-f0-9]{64}$/;
const exact=(v,keys)=>v&&typeof v==='object'&&!Array.isArray(v)&&Object.keys(v).length===keys.length&&keys.every(k=>Object.hasOwn(v,k));
const fail=code=>{throw Error(code);};
const absolute=p=>typeof p==='string'&&p.startsWith('/')&&p!=='/'&&posix.normalize(p)===p&&!/[\x00-\x20\x7f\\]/.test(p);
const fileName=s=>typeof s==='string'&&/^[A-Za-z0-9_.-]{1,80}$/.test(s)&&!['.','..'].includes(s);
export function validateDockerConfig(c){
  const required=['projectDirectory','composeFile','envFile','overrideFile','projectName','serviceName','dataVolume','imageRepository','healthPort','brokerUid','brokerGid','updatesDirectory'];
  if(!c||!exact(c,Object.hasOwn(c,'tlsDirectory')?[...required,'tlsDirectory']:required)||(Object.hasOwn(c,'tlsDirectory')&&!absolute(c.tlsDirectory))||!absolute(c.projectDirectory)||!absolute(c.updatesDirectory)||c.projectDirectory===c.updatesDirectory||!fileName(c.composeFile)||!fileName(c.envFile)||!fileName(c.overrideFile)||new Set([c.composeFile,c.envFile,c.overrideFile]).size!==3||!/^multitap(?:[-_][a-z0-9_-]{1,40})?$/.test(c.projectName)||c.serviceName!=='broker'||!/^multitap[-_][a-zA-Z0-9_.-]{1,60}$/.test(c.dataVolume)||!Number.isInteger(c.healthPort)||c.healthPort<1||c.healthPort>65535||c.brokerUid!==1000||!Number.isInteger(c.brokerGid)||c.brokerGid<1||c.brokerGid>2147483647)fail('update_docker_config_invalid');
  if(typeof c.imageRepository!=='string'||c.imageRepository.length>180||!/^([a-z0-9.-]+(?::[0-9]{1,5})?)\/[a-z0-9]+(?:[._/-][a-z0-9]+)*$/.test(c.imageRepository)||(!c.imageRepository.split('/')[0].includes('.')&&!c.imageRepository.split('/')[0].includes(':')&&!c.imageRepository.startsWith('localhost/')))fail('update_docker_config_invalid');
  if([c.composeFile,c.envFile,c.overrideFile].includes('multitap-update-channel.json'))fail('update_docker_config_invalid');
  return Object.freeze({...c});
}
export function validateDockerImage(value,version,repository){
  if(!exact(value,['schema','product','version','image','platform','imageBytes'])||value.schema!==1||value.product!=='multitap-broker'||value.version!==version||!versionPattern.test(version)||value.platform!=='linux/amd64'||typeof value.image!=='string'||!value.image.startsWith(repository+'@sha256:')||!sha.test(value.image.slice((repository+'@sha256:').length))||!Number.isSafeInteger(value.imageBytes)||value.imageBytes<=0||value.imageBytes>2*1024*1024*1024)fail('update_docker_image_invalid');
  return value;
}
export async function trustedDockerPath(path,kind='file'){
  if(!absolute(path))fail('update_docker_installation_invalid');
  let current=path,first=true;
  while(true){const s=await lstat(current);if(s.isSymbolicLink()||s.uid!==0||(s.mode&0o022)||(first?(kind==='file'?!s.isFile():!s.isDirectory()):!s.isDirectory()))fail('update_docker_installation_invalid');if(current==='/')return;current=dirname(current);first=false;}
}
const runDefault=async(args)=>{
  try{return (await execute('/usr/bin/docker',['--host=unix:///var/run/docker.sock',...args],{maxBuffer:1024*1024,env:{PATH:'/usr/sbin:/usr/bin:/sbin:/bin',LANG:'C.UTF-8',DOCKER_CONFIG:'/etc/multitap/docker-auth'}})).stdout;}
  catch{fail('update_docker_command_failed');}
};
const runInputDefault=(args,input)=>new Promise((resolve,reject)=>{
  const child=spawn('/usr/bin/docker',['--host=unix:///var/run/docker.sock',...args],{stdio:['pipe','pipe','pipe'],env:{PATH:'/usr/sbin:/usr/bin:/sbin:/bin',LANG:'C.UTF-8',DOCKER_CONFIG:'/etc/multitap/docker-auth'}});
  let size=0,failed=false;const chunks=[],timer=setTimeout(()=>{failed=true;child.kill('SIGTERM');},8000);timer.unref?.();
  child.stdout.on('data',bytes=>{size+=bytes.length;if(size>32768){failed=true;child.kill('SIGTERM');}else chunks.push(bytes);});child.stderr.resume();
  child.once('error',()=>{clearTimeout(timer);reject(Error('update_docker_command_failed'));});child.once('close',code=>{clearTimeout(timer);code===0&&!failed?resolve(Buffer.concat(chunks).toString('utf8')):reject(Error('update_docker_command_failed'));});
  child.stdin.on('error',()=>{});child.stdin.end(input);
});
const one=text=>{try{const values=JSON.parse(text);if(!Array.isArray(values)||values.length!==1)return fail('update_docker_installation_invalid');return values[0];}catch{fail('update_docker_installation_invalid');}};

// This fixed helper runs in the verified OLD image, not in browser-supplied code.
// Its production source mount is read-only. Tests execute this same program on
// temporary SQLite/files through an injected Docker CLI boundary.
async function backupMain(){
  const fs=await import('node:fs/promises'),crypto=await import('node:crypto'),sqlite=await import('node:sqlite');
  const [source,target,operation]=process.argv.slice(1),hash=b=>crypto.createHash('sha256').update(b).digest('hex');
  if(!/^[a-f0-9-]{36}$/.test(operation))throw Error('operation');
  const db=new sqlite.DatabaseSync(source+'/broker.sqlite',{readOnly:true});let schema;
  try{schema=db.prepare('PRAGMA user_version').get().user_version;await sqlite.backup(db,target+'/broker.sqlite');}finally{db.close();}
  const copy=new sqlite.DatabaseSync(target+'/broker.sqlite',{readOnly:true});try{const r=copy.prepare('PRAGMA quick_check').all();if(r.length!==1||Object.values(r[0])[0]!=='ok')throw Error('integrity');}finally{copy.close();}
  const files={};
  for(const name of ['broker.sqlite','vault.key','initial-account.json']){
    const path=(name==='broker.sqlite'?target:source)+'/'+name;let bytes;
    try{const s=await fs.lstat(path);if(!s.isFile()||s.isSymbolicLink()||s.size>(name==='broker.sqlite'?1024*1024*1024:1024*1024))throw Error('file');bytes=await fs.readFile(path);}catch(e){if(e.code==='ENOENT'&&name==='initial-account.json'){files[name]=null;continue;}throw e;}
    if(name==='vault.key'&&bytes.length!==32)throw Error('vault');
    if(name!=='broker.sqlite')await fs.writeFile(target+'/'+name,bytes,{flag:'wx',mode:0o600});
    await fs.chmod(target+'/'+name,0o600);files[name]={size:bytes.length,sha256:hash(bytes)};
    const f=await fs.open(target+'/'+name,'r');try{await f.sync();}finally{await f.close();}
  }
  const report={schema:1,operation,databaseSchema:schema,files};await fs.writeFile(target+'/manifest.json',JSON.stringify(report),{flag:'wx',mode:0o600});
  const f=await fs.open(target+'/manifest.json','r');try{await f.sync();}finally{await f.close();}
  console.log(JSON.stringify(report));
}
async function restoreMain(){
  const fs=await import('node:fs/promises'),crypto=await import('node:crypto'),sqlite=await import('node:sqlite');
  const [target,backup,operation,expected]=process.argv.slice(1),hash=b=>crypto.createHash('sha256').update(b).digest('hex');
  const manifest=await fs.readFile(backup+'/manifest.json');if(hash(manifest)!==expected)throw Error('manifest');const r=JSON.parse(manifest);if(r.operation!==operation)throw Error('operation');
  for(const [name,value] of Object.entries(r.files)){if(!['broker.sqlite','vault.key','initial-account.json'].includes(name))throw Error('file');if(value){const s=await fs.lstat(backup+'/'+name),b=await fs.readFile(backup+'/'+name);if(!s.isFile()||s.isSymbolicLink()||b.length!==value.size||hash(b)!==value.sha256)throw Error('backup');}}
  const db=new sqlite.DatabaseSync(backup+'/broker.sqlite',{readOnly:true});try{const a=db.prepare('PRAGMA quick_check').all();if(a.length!==1||Object.values(a[0])[0]!=='ok'||db.prepare('PRAGMA user_version').get().user_version!==r.databaseSchema)throw Error('integrity');}finally{db.close();}
  const failed=target+'/.multitap-update-failed-'+operation;await fs.mkdir(failed,{mode:0o700}).catch(e=>{if(e.code!=='EEXIST')throw e;});const s=await fs.lstat(failed);if(!s.isDirectory()||s.isSymbolicLink())throw Error('failed-directory');
  for(const name of ['broker.sqlite','broker.sqlite-wal','broker.sqlite-shm','vault.key','initial-account.json']){
    const path=target+'/'+name;let old;try{const a=await fs.lstat(path);if(!a.isFile()||a.isSymbolicLink())throw Error('current-file');old=await fs.readFile(path);}catch(e){if(e.code==='ENOENT')continue;throw e;}
    const preserved=failed+'/'+name+'-'+hash(old);try{await fs.writeFile(preserved,old,{flag:'wx',mode:0o600});}catch(e){const p=await fs.lstat(preserved);if(e.code!=='EEXIST'||!p.isFile()||p.isSymbolicLink()||hash(await fs.readFile(preserved))!==hash(old))throw e;}
    if(name.endsWith('-wal')||name.endsWith('-shm')||r.files[name]===null)await fs.unlink(path);
  }
  for(const [name,value] of Object.entries(r.files))if(value){
    const path=target+'/'+name,temporary=path+'.restore-'+operation;
    try{await fs.writeFile(temporary,await fs.readFile(backup+'/'+name),{flag:'wx',mode:0o600});}catch(e){const p=await fs.lstat(temporary);if(e.code!=='EEXIST'||!p.isFile()||p.isSymbolicLink()||hash(await fs.readFile(temporary))!==value.sha256)throw e;}
    const f=await fs.open(temporary,'r');try{await f.sync();}finally{await f.close();}await fs.rename(temporary,path);
  }
  const directory=await fs.open(target,'r');try{await directory.sync();}finally{await directory.close();}
  console.log(JSON.stringify({restored:true,operation}));
}
export const dockerBackupProgram='('+backupMain.toString()+')().catch(()=>{console.error("backup_failed");process.exitCode=1;})';
export const dockerRestoreProgram='('+restoreMain.toString()+')().catch(()=>{console.error("restore_failed");process.exitCode=1;})';

export function dockerAccessHealthVersion(value,config,localTransport=null){
  if(value?.service!=='multitap-broker'||value.status!=='ok'||value.authentication!=='id-password'||!versionPattern.test(value.version))return null;
  if(!config)return validBootstrapLocalHealth(value,localTransport)?value.version:null;
  try{
    if(value.access?.protocol!=='server-access-v1'||value.access.revision!==serverAccessHash(config)||value.access.origin!==config.primaryOrigin||value.access.bootstrapOnly!==false||
      (value.access.healthProbeOnly!==undefined&&typeof value.access.healthProbeOnly!=='boolean')||
      (Object.hasOwn(value.access,'primaryOrigin')&&value.access.primaryOrigin!==config.primaryOrigin))return null;
  }catch{return null;}
  return value.version;
}

export class DockerAdapter {
  constructor(config,{run=runDefault,runInput=runInputDefault,trust=trustedDockerPath,health,sleep=ms=>new Promise(r=>setTimeout(r,ms)),space,helperRoot='/opt/multitap-docker-updater'}={}){
    this.config=validateDockerConfig(config);this.platform='linux-x64-docker';this.run=run;this.runInput=runInput;this.trust=trust;this.sleep=sleep;this.space=space;
    this.health=health??(async()=>{try{
      const transport=await this.transport(),legacy=await localBrokerHealth({...transport,host:'127.0.0.1'},{internalAuthority:false});if(legacy)return legacy.version;
      const container=await this.container(),env=deploymentEnvironmentValues(await readFile(this.paths().env,'utf8')),config=this.accessManifest?.config??(env.MULTITAP_PUBLIC_ORIGIN?legacyServerAccess(env):null),primary=config?(config.primaryOrigin===config.https.origin?config.https:config.http):null;
      const port=primary?.listenPort??Number(env.MULTITAP_PORT??18480);
      const value=JSON.parse(await this.runInput(['exec','--interactive',container.Id,'/usr/local/bin/node','/app/update/server-access-health.mjs','--local-probe'],Buffer.from(JSON.stringify({config,port}))));
      const localTransport={publicOrigin:null,tlsMode:'proxy',tlsFiles:null,bootstrapOnly:true,host:'127.0.0.1',port};
      return dockerAccessHealthVersion(value,config,localTransport);
    }catch{return null;}});
    this.configSha=digest(Buffer.from(JSON.stringify(this.config)));
    if(!absolute(helperRoot))fail('update_docker_config_invalid');this.helperRoot=helperRoot;
  }
  paths(){const c=this.config;return {compose:join(c.projectDirectory,c.composeFile),env:join(c.projectDirectory,c.envFile),channel:join(c.projectDirectory,'multitap-update-channel.json'),override:join(c.projectDirectory,c.overrideFile)};}
  channel(){return {services:{broker:{environment:{MULTITAP_UPDATE_SOCKET:'/run/multitap-docker-update/control.sock'},group_add:[String(this.config.brokerGid)],volumes:[{type:'bind',source:'/run/multitap-docker-update',target:'/run/multitap-docker-update',read_only:true,bind:{create_host_path:false}}]}}};}
  compose(args){const c=this.config,p=this.paths();return ['compose','--project-name',c.projectName,'--project-directory',c.projectDirectory,'--env-file',p.env,'-f',p.compose,'-f',p.channel,'-f',p.override,...(this.accessOverlay?['-f',this.accessOverlay]:[]),...args];}
  async loadAccessOverlay(){
    this.accessOverlay=null;this.accessManifest=null;
    const c=this.config,meta=join(c.projectDirectory,'multitap-server-access.json'),overlay=join(c.projectDirectory,'multitap-server-access.yaml');
    // Preserve an old installation that already uses a reserved name for a
    // different purpose. The new access adapter refuses to overwrite it.
    if([c.composeFile,c.envFile,c.overrideFile].some(name=>['multitap-server-access.json','multitap-server-access.yaml'].includes(name)))return;
    try{await lstat(meta);}catch(error){if(error.code==='ENOENT')return;throw error;}
    await this.trust(meta);await this.trust(overlay);
    const bytes=await readFile(meta);if(bytes.length>32768)fail('update_docker_installation_invalid');
    let value;try{value=decodeDockerAccessManifest(JSON.parse(bytes));if(await readFile(overlay,'utf8')!==renderDockerAccessOverlay(value))throw Error();}catch{fail('update_docker_installation_invalid');}
    this.accessOverlay=overlay;this.accessManifest=value;
  }
  async transport(){
    if(this.accessManifest){const result=serverAccessTransport(this.accessManifest.config),scheme=this.accessManifest.config.primaryOrigin===this.accessManifest.config.https.origin?'https':'http',mapping=this.accessManifest.dockerPorts.find(row=>row.scheme===scheme);return {...result,host:'127.0.0.1',port:mapping.hostPort};}
    const path=this.paths().env;await this.trust(path);
    const config=deploymentEnvironment(await readFile(path,'utf8'),{portOverride:this.config.healthPort});
    const direct=config.tlsMode==='direct';
    if(!config.bootstrapOnly&&(direct!==Object.hasOwn(this.config,'tlsDirectory')||(direct&&(config.tlsFiles.certFile!=='/etc/multitap/tls/cert.pem'||config.tlsFiles.keyFile!=='/etc/multitap/tls/key.pem'))))fail('update_docker_installation_invalid');
    return config;
  }
  async files(){
    await this.loadAccessOverlay();
    const p=this.paths();await this.trust(this.config.projectDirectory,'directory');await this.trust(this.config.updatesDirectory,'directory');const hashes={};for(const [name,path] of Object.entries(p)){await this.trust(path);const bytes=await readFile(path);if(bytes.length>1024*1024)fail('update_docker_installation_invalid');hashes[name]=digest(bytes);}
    if(this.config.tlsDirectory){
      const dir=this.config.tlsDirectory;await this.trust(dir,'directory');
      if(JSON.stringify((await readdir(dir)).sort())!==JSON.stringify(['cert.pem','key.pem']))fail('update_docker_installation_invalid');
      for(const [name,maximum]of [['cert.pem',262144],['key.pem',65536]]){const path=join(dir,name);await this.trust(path);const stat=await lstat(path);if(stat.size<1||stat.size>maximum)fail('update_docker_installation_invalid');hashes['tls:'+name]=digest(await readFile(path));}
    }
    if(this.accessManifest){hashes.serverAccess=digest(await readFile(join(this.config.projectDirectory,'multitap-server-access.json')));hashes.serverAccessOverlay=digest(await readFile(this.accessOverlay));}
    const channel=JSON.parse(await readFile(p.channel,'utf8'));if(JSON.stringify(channel)!==JSON.stringify(this.channel()))fail('update_docker_installation_invalid');
    const override=JSON.parse(await readFile(p.override,'utf8')),pin=override?.services?.broker?.image;const digestPin=typeof pin==='string'&&pin.startsWith(this.config.imageRepository+'@sha256:')&&sha.test(pin.slice((this.config.imageRepository+'@sha256:').length));if(!exact(override,['services'])||!exact(override.services,['broker'])||!exact(override.services.broker,['image','pull_policy'])||override.services.broker.pull_policy!=='never'||(!imageId.test(pin??'')&&!digestPin))fail('update_docker_installation_invalid');
    return hashes;
  }
  validateContainer(value,{images,running,portBindings}={}){
    const c=this.config,v=value,labels=v?.Config?.Labels,mounts=v?.Mounts;
    if(!containerId.test(v?.Id??'')||!imageId.test(v.Image??'')||labels?.['com.docker.compose.project']!==c.projectName||labels?.['com.docker.compose.service']!==c.serviceName||labels?.['com.docker.compose.oneoff']!=='False'||!sha.test(labels?.['com.docker.compose.config-hash']??'')||!Array.isArray(mounts)||!v.State||typeof v.State.Running!=='boolean'||(running!==undefined&&v.State.Running!==running)||(images&&!images.includes(v.Image)))fail('update_docker_installation_invalid');
    const data=mounts.filter(m=>m.Destination==='/var/lib/multitap');if(data.length!==1||data[0].Type!=='volume'||data[0].Name!==c.dataVolume||data[0].RW!==true)fail('update_docker_volume_changed');
    if(v.Config.User!=='1000:1000'||v.HostConfig?.Privileged!==false||v.HostConfig?.ReadonlyRootfs!==true||!v.HostConfig?.CapDrop?.includes('ALL')||v.HostConfig.NetworkMode==='host')fail('update_docker_installation_invalid');
    const tlsMount=mounts.filter(m=>m.Destination==='/etc/multitap/tls');
    if(c.tlsDirectory){
      if(tlsMount.length!==1||tlsMount[0].Type!=='bind'||tlsMount[0].Source!==c.tlsDirectory||tlsMount[0].RW!==false)fail('update_docker_installation_invalid');
      const values=v.Config.Env;if(!this.accessManifest&&(!Array.isArray(values)||['MULTITAP_TLS_MODE=direct','MULTITAP_TLS_CERT_FILE=/etc/multitap/tls/cert.pem','MULTITAP_TLS_KEY_FILE=/etc/multitap/tls/key.pem'].some(expected=>values.filter(value=>value.startsWith(expected.split('=')[0]+'=')).length!==1||!values.includes(expected))))fail('update_docker_installation_invalid');
    }else if(tlsMount.length)fail('update_docker_installation_invalid');
    if(mounts.some(m=>m!==data[0]&&!(c.tlsDirectory&&m===tlsMount[0])&&!(m.Type==='bind'&&m.Source==='/run/multitap-docker-update'&&m.Destination==='/run/multitap-docker-update'&&m.RW===false)))fail('update_docker_installation_invalid');
    // Runtime network mappings may disappear when a container stops. The
    // immutable host binding remains in HostConfig and must still match before
    // accepting stop/backup/recovery; do not mistake that state for a new VM.
    if(this.accessManifest||portBindings){
      const expected=portBindings??this.accessManifest.bindings;
      if(JSON.stringify(inspectDockerBindings(v))!==JSON.stringify(expected))fail('update_docker_installation_invalid');
      if(!portBindings&&(!Array.isArray(v.Config.Env)||!v.Config.Env.includes('MULTITAP_SERVER_ACCESS_FILE=/var/lib/multitap/server-access.json')))fail('update_docker_installation_invalid');
      for(const row of expected)if(v.State.Running&&!v.NetworkSettings?.Ports?.[row.containerPort+'/'+row.protocol]?.some(value=>Number(value.HostPort)===row.hostPort&&(!row.hostIp||value.HostIp===row.hostIp)))fail('update_docker_installation_invalid');
    }else{
      const configured=v.HostConfig.PortBindings?.['18480/tcp'],matches=ports=>Array.isArray(ports)&&ports.some(p=>String(p.HostPort)===String(c.healthPort)&&['','127.0.0.1','0.0.0.0','::','::1'].includes(p.HostIp));
      if(!matches(configured)||(v.State.Running&&!matches(v.NetworkSettings?.Ports?.['18480/tcp'])))fail('update_docker_installation_invalid');
    }
    return v;
  }
  async container({allowAbsent=false}={}){
    const ids=(await this.run(this.compose(['ps','--all','--quiet',this.config.serviceName]))).trim().split(/\s+/).filter(Boolean);if(!ids.length&&allowAbsent)return null;if(ids.length!==1||!containerId.test(ids[0]))fail('update_docker_installation_invalid');const value=this.validateContainer(one(await this.run(['container','inspect',ids[0]])));if(value.Id!==ids[0])fail('update_docker_installation_invalid');return value;
  }
  async volume(){const v=one(await this.run(['volume','inspect',this.config.dataVolume]));if(v.Name!==this.config.dataVolume||v.Driver!=='local'||v.Scope!=='local'||(v.Options&&Object.keys(v.Options).length)||typeof v.Mountpoint!=='string'||!v.Mountpoint.startsWith('/'))fail('update_docker_volume_changed');return {Name:v.Name,Driver:v.Driver,Scope:v.Scope,Mountpoint:v.Mountpoint,Options:v.Options??null};}
  async version(){await this.files();const c=await this.container(),version=await this.health();if(c.State.Running&&version)return version;fail('update_docker_health_unavailable');}
  async installationCheck(){await this.files();const c=await this.container();this.validateContainer(c,{running:true});const version=await this.health();if(!version)fail('update_docker_health_unavailable');await this.volume();return {version,container:c.Id,image:c.Image};}
  async disk(required,path=this.config.updatesDirectory){const stat=this.space?await this.space(path):await (await import('node:fs/promises')).statfs(path);if(Number(stat.bavail)*Number(stat.bsize)<256*1024*1024+required)fail('update_disk_space');}
  async helperCurrent(){
    await this.trust(this.helperRoot,'directory');await this.trust(join(this.helperRoot,'.multitap-managed-docker-updater'));await this.trust(join(this.helperRoot,'releases'),'directory');
    const link=join(this.helperRoot,'current'),stat=await lstat(link);if(!stat.isSymbolicLink()||stat.uid!==0)fail('update_docker_installation_invalid');const current=await realpath(link),prefix=this.helperRoot+'/releases/';if(!current.startsWith(prefix)||!/^\d+\.\d+\.\d+(?:-u[a-f0-9]{32})?$/.test(current.slice(prefix.length)))fail('update_docker_installation_invalid');await this.trust(current,'directory');return current;
  }
  nextHelper(job){return this.helperRoot+'/releases/'+job.toVersion+'-u'+job.operationId.replaceAll('-','');}
  async switchHelper(job,path){
    if(![job.previousHelper,this.nextHelper(job)].includes(path))fail('update_job_invalid');const current=await this.helperCurrent();if(![job.previousHelper,this.nextHelper(job)].includes(current))fail('update_docker_installation_changed');if(current===path)return;
    await this.trust(path,'directory');const temporary=this.helperRoot+'/current.swap-'+job.operationId;
    try{await symlink(path,temporary);}catch(e){if(e.code!=='EEXIST')throw e;const s=await lstat(temporary);if(!s.isSymbolicLink()||s.uid!==0||await readlink(temporary)!==path)fail('update_docker_installation_changed');}
    await rename(temporary,this.helperRoot+'/current');const parent=await open(this.helperRoot,'r');try{await parent.sync();}finally{await parent.close();}
  }
  async publishHelper(job){
    const archive=await readFile(job.archive);if(digest(archive)!==job.sha256)fail('update_checksum_failed');const files=unpackPackage(archive,job.toVersion),selected=[...files].filter(([name])=>name.startsWith('update/'));
    for(const required of helperModules)if(!files.has('update/'+required))fail('update_archive_incomplete');
    await this.disk(selected.reduce((sum,[,bytes])=>sum+bytes.length,0),this.helperRoot);
    const target=this.nextHelper(job);try{await mkdir(target,{mode:0o755});}catch(e){if(e.code!=='EEXIST')throw e;}await this.trust(target,'directory');
    for(const [name,bytes] of selected){
      const path=join(target,name),parent=dirname(path);await mkdir(parent,{recursive:true,mode:0o755});await this.trust(parent,'directory');
      try{const f=await open(path,'wx',0o644);try{await f.writeFile(bytes);await f.sync();}finally{await f.close();}}catch(e){if(e.code!=='EEXIST')throw e;}
      await this.trust(path);if(digest(await readFile(path))!==digest(bytes))fail('update_docker_installation_changed');
    }
    const expected=new Set(selected.map(([name])=>name));
    const walk=async directory=>{for(const entry of await readdir(directory,{withFileTypes:true})){const path=join(directory,entry.name);if(entry.isDirectory()){await this.trust(path,'directory');await walk(path);}else{await this.trust(path);if(!expected.delete(path.slice(target.length+1)))fail('update_docker_installation_changed');}}};await walk(target);if(expected.size)fail('update_docker_installation_changed');
    await this.switchHelper(job,target);
  }
  async prepare({operationId,release,archive,stage,fromVersion}){
    const c=this.config;if(!uuid.test(operationId)||stage!==join(c.updatesDirectory,operationId)||archive!==join(stage,'server.tar.gz')||release.platform!==this.platform||!versionPattern.test(release.version)||!versionPattern.test(fromVersion)||compareVersion(release.version,fromVersion)<=0)fail('update_job_invalid');
    const files=await this.files(),old=await this.container(),previousHelper=await this.helperCurrent();this.validateContainer(old,{running:true});if(await this.health()!==fromVersion)fail('update_version_changed');
    await this.trust(stage,'directory');await this.trust(archive);const archiveBytes=await readFile(archive);if(digest(archiveBytes)!==release.artifact.sha256)fail('update_checksum_failed');
    const packageFiles=unpackPackage(archiveBytes,release.version);for(const required of helperModules)if(!packageFiles.has('update/'+required))fail('update_archive_incomplete');
    const metadataBytes=packageFiles.get('update/docker-image.json');if(!metadataBytes||metadataBytes.length>4096)fail('update_docker_image_invalid');const metadata=validateDockerImage(JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(metadataBytes)),release.version,c.imageRepository);
    const volume=await this.volume();const dockerRoot=JSON.parse(await this.run(['info','--format','{{json .DockerRootDir}}']));if(!absolute(dockerRoot))fail('update_docker_installation_invalid');await this.trust(dockerRoot,'directory');await this.disk(metadata.imageBytes,dockerRoot);await this.disk(0);
    await this.run(['image','pull','--platform','linux/amd64',metadata.image]);
    const image=one(await this.run(['image','inspect',metadata.image]));if(!imageId.test(image.Id??'')||image.Os!=='linux'||image.Architecture!=='amd64'||image.Size!==metadata.imageBytes||!Array.isArray(image.RepoDigests)||!image.RepoDigests.includes(metadata.image)||image.Config?.Labels?.['org.opencontainers.image.version']!==release.version||image.Config?.Labels?.['org.opencontainers.image.title']!=='multitap-broker')fail('update_docker_image_invalid');
    await this.disk(0);if(JSON.stringify(await this.files())!==JSON.stringify(files)||(await this.container()).Id!==old.Id||await this.health()!==fromVersion)fail('update_version_changed');
    return {kind:'docker',operationId,stage,archive,sha256:release.artifact.sha256,fromVersion,toVersion:release.version,configSha:this.configSha,files,previousContainer:old.Id,previousImage:old.Image,previousConfigHash:old.Config.Labels['com.docker.compose.config-hash'],previousOverride:await readFile(this.paths().override,'utf8'),previousHelper,image:metadata.image,imageId:image.Id,imageBytes:metadata.imageBytes,volume};
  }
  async validate(job){
    const c=this.config;if(!job||job.kind!=='docker'||!uuid.test(job.operationId??'')||job.configSha!==this.configSha||job.stage!==join(c.updatesDirectory,job.operationId)||job.archive!==join(job.stage,'server.tar.gz')||!sha.test(job.sha256??'')||!versionPattern.test(job.fromVersion)||!versionPattern.test(job.toVersion)||!containerId.test(job.previousContainer??'')||!imageId.test(job.previousImage??'')||!imageId.test(job.imageId??'')||!sha.test(job.previousConfigHash??'')||typeof job.previousOverride!=='string'||job.previousOverride.length>2048)fail('update_job_invalid');
    validateDockerImage({schema:1,product:'multitap-broker',version:job.toVersion,image:job.image,platform:'linux/amd64',imageBytes:job.imageBytes},job.toVersion,c.imageRepository);
    await this.trust(job.stage,'directory');await this.trust(job.archive);if(digest(await readFile(job.archive))!==job.sha256)fail('update_checksum_failed');
    const current=await this.files();if(Object.keys(current).length!==Object.keys(job.files).length||Object.entries(job.files).some(([name,hash])=>name!=='override'&&current[name]!==hash)||![job.files.override,digest(Buffer.from(JSON.stringify(this.override(job.image))))].includes(current.override)||JSON.stringify(await this.volume())!==JSON.stringify(job.volume))fail('update_docker_installation_changed');
    const prefix=this.helperRoot+'/releases/';if(typeof job.previousHelper!=='string'||!job.previousHelper.startsWith(prefix)||!/^\d+\.\d+\.\d+(?:-u[a-f0-9]{32})?$/.test(job.previousHelper.slice(prefix.length))||![job.previousHelper,this.nextHelper(job)].includes(await this.helperCurrent()))fail('update_docker_installation_changed');await this.trust(job.previousHelper,'directory');
  }
  journal(job){return join(job.stage,'docker-transaction.json');}
  async state(job){try{await this.trust(this.journal(job));const s=JSON.parse(await readFile(this.journal(job),'utf8'));if(s.jobSha!==digest(Buffer.from(JSON.stringify(job)))||!['prepared','stop_requested','stopped','backup_ready','up_requested','health_check','succeeded','restoring','restore_ready','restored'].includes(s.phase))fail('update_job_invalid');return s;}catch(e){if(e.code==='ENOENT')return null;throw e;}}
  async save(job,phase,extra={}){const old=await this.state(job);const s={...old,...extra,jobSha:digest(Buffer.from(JSON.stringify(job))),phase};await atomicJson(this.journal(job),s);return s;}
  override(image){return {services:{broker:{image,pull_policy:'never'}}};}
  helperName(job,role){return 'multitap-update-'+role+'-'+job.operationId;}
  helperArgs(job,role,program,args){const backup=join(job.stage,'backup'),readOnly=role==='backup';return ['run','--rm','--name',this.helperName(job,role),'--label','me.multitap.update.operation='+job.operationId,'--label','me.multitap.update.role='+role,'--network','none','--read-only','--cap-drop','ALL','--security-opt','no-new-privileges','--user','1000:1000','--mount','type=volume,source='+this.config.dataVolume+',target=/var/lib/multitap'+(readOnly?',readonly':''),'--mount','type=bind,source='+backup+',target=/backup'+(readOnly?'':',readonly'),'--entrypoint','/usr/local/bin/node',job.previousImage,'--input-type=module','-e',program,...args];}
  async quiesceHelpers(job){
    const ids=(await this.run(['container','ls','--all','--quiet','--no-trunc','--filter','label=me.multitap.update.operation='+job.operationId])).trim().split(/\s+/).filter(Boolean);
    for(const id of ids){if(!containerId.test(id))fail('update_docker_helper_unverified');const v=one(await this.run(['container','inspect',id])),role=v.Config?.Labels?.['me.multitap.update.role'];
      if(v.Id!==id||!['backup','restore'].includes(role)||v.Config?.Labels?.['me.multitap.update.operation']!==job.operationId||v.Name!=='/'+this.helperName(job,role)||v.Image!==job.previousImage||v.HostConfig?.NetworkMode!=='none'||v.HostConfig?.ReadonlyRootfs!==true||v.HostConfig?.AutoRemove!==true||v.HostConfig?.Privileged!==false||!v.HostConfig?.CapDrop?.includes('ALL')||!v.HostConfig?.SecurityOpt?.some(s=>s==='no-new-privileges'||s==='no-new-privileges=true')||v.Config?.User!=='1000:1000')fail('update_docker_helper_unverified');
      const mounts=v.Mounts??[],data=mounts.find(m=>m.Destination==='/var/lib/multitap'),backup=mounts.find(m=>m.Destination==='/backup');if(mounts.length!==2||data?.Type!=='volume'||data.Name!==this.config.dataVolume||data.RW!==(role==='restore')||backup?.Type!=='bind'||backup.Source!==join(job.stage,'backup')||backup.RW!==(role==='backup'))fail('update_docker_helper_unverified');
      if(v.State?.Running)await this.run(['container','stop','--time','10',id]);
    }
    // --rm helpers should vanish. Do not trust a stopped acknowledgement while
    // any exact helper still appears; a subsequent recovery can retry safely.
    const remaining=(await this.run(['container','ls','--all','--quiet','--no-trunc','--filter','label=me.multitap.update.operation='+job.operationId])).trim();if(remaining)fail('update_docker_helper_unverified');
  }
  async backup(job){
    const path=join(job.stage,'backup');await mkdir(path,{mode:0o700});await chown(path,1000,1000);
    await this.run(this.helperArgs(job,'backup',dockerBackupProgram,['/var/lib/multitap','/backup',job.operationId]));
    await this.quiesceHelpers(job);const bytes=await readFile(join(path,'manifest.json')),r=JSON.parse(bytes);if(r.schema!==1||r.operation!==job.operationId||!Number.isInteger(r.databaseSchema)||r.databaseSchema<1||r.databaseSchema>1000||!exact(r.files,['broker.sqlite','vault.key','initial-account.json']))fail('update_backup_invalid');
    for(const [name,value] of Object.entries(r.files)){if(value===null){if(name!=='initial-account.json')fail('update_backup_invalid');continue;}const p=join(path,name),s=await lstat(p),b=await readFile(p);if(!s.isFile()||s.isSymbolicLink()||b.length!==value.size||digest(b)!==value.sha256)fail('update_backup_invalid');await chown(p,0,0);await chmod(p,0o600);}
    await chown(join(path,'manifest.json'),0,0);await chmod(join(path,'manifest.json'),0o600);await chown(path,0,0);return digest(bytes);
  }
  async backupHash(job,expected){const path=join(job.stage,'backup');await this.trust(path,'directory');await this.trust(join(path,'manifest.json'));const bytes=await readFile(join(path,'manifest.json'));if(digest(bytes)!==expected)fail('update_backup_invalid');const r=JSON.parse(bytes);if(r.operation!==job.operationId)fail('update_backup_invalid');for(const [name,v] of Object.entries(r.files)){if(!['broker.sqlite','vault.key','initial-account.json'].includes(name))fail('update_backup_invalid');if(v){await this.trust(join(path,name));const b=await readFile(join(path,name));if(b.length!==v.size||digest(b)!==v.sha256)fail('update_backup_invalid');}}return r;}
  async healthy(version,job){for(let i=0;i<25;i++){try{const c=await this.container();this.validateContainer(c,{images:[version===job.toVersion?job.imageId:job.previousImage],running:true});if(await this.health()===version)return true;}catch{}await this.sleep(500);}return false;}
  async apply(job,phase){
    await this.validate(job);const old=await this.container();if(old.Id!==job.previousContainer||old.Image!==job.previousImage||old.Config.Labels['com.docker.compose.config-hash']!==job.previousConfigHash||await this.health()!==job.fromVersion||digest(Buffer.from(job.previousOverride))!==job.files.override)fail('update_version_changed');
    await this.save(job,'prepared');await this.quiesceHelpers(job);await phase('backing_up');await this.save(job,'stop_requested');await this.run(['container','stop','--time','15',old.Id]);this.validateContainer(await this.container(),{images:[job.previousImage],running:false});await this.save(job,'stopped');
    const backupSha=await this.backup(job);await this.save(job,'backup_ready',{backupSha});await phase('applying');await atomicJson(this.paths().override,this.override(job.image));await this.save(job,'up_requested');
    await phase('restarting');await this.run(this.compose(['up','--detach','--no-deps','--no-build','--pull','never','--force-recreate',this.config.serviceName]));await this.save(job,'health_check');await phase('health_check');
    if(!(await this.healthy(job.toVersion,job)))fail('update_health_failed');await this.publishHelper(job);await this.save(job,'succeeded');
  }
  async recover(job,phase){
    await this.validate(job);await this.quiesceHelpers(job);const state=await this.state(job),current=await this.container({allowAbsent:true});if(current)this.validateContainer(current,{images:[job.previousImage,job.imageId]});
    if(!state){if(current?.Id===job.previousContainer&&current.Image===job.previousImage&&await this.health()===job.fromVersion)return true;fail('update_recovery_required');}
    if(state.phase==='restored'&&current?.Image===job.previousImage&&await this.health()===job.fromVersion){await this.switchHelper(job,job.previousHelper);return true;}
    if(['prepared','stop_requested','stopped'].includes(state.phase)){
      if(current?.Id!==job.previousContainer||current.Image!==job.previousImage||JSON.stringify(await this.files())!==JSON.stringify(job.files))fail('update_docker_installation_changed');
      if(!current.State.Running){await phase('restarting');await this.run(['container','start',current.Id]);}await phase('health_check');if(!(await this.healthy(job.fromVersion,job)))fail('update_health_failed');await this.switchHelper(job,job.previousHelper);await this.save(job,'restored');return true;
    }
    // A health-verified old service after an earlier completed recovery must
    // retain legitimate new writes instead of replaying the backup again.
    if(state.phase==='restore_ready'){
      if(digest(await readFile(this.paths().override))!==digest(Buffer.from(job.previousOverride))||(current?.State.Running&&current.Image!==job.previousImage))fail('update_docker_installation_changed');
      if(!(current?.State.Running&&await this.health()===job.fromVersion))await this.run(this.compose(['up','--detach','--no-deps','--no-build','--pull','never','--force-recreate',this.config.serviceName]));
      await phase('health_check');if(!(await this.healthy(job.fromVersion,job)))fail('update_health_failed');await this.switchHelper(job,job.previousHelper);await this.save(job,'restored');return true;
    }
    await this.backupHash(job,state.backupSha);await this.save(job,'restoring');await phase('restarting');if(current?.State.Running)await this.run(['container','stop','--time','15',current.Id]);const stopped=await this.container({allowAbsent:true});if(stopped)this.validateContainer(stopped,{images:[job.previousImage,job.imageId],running:false});
    const path=join(job.stage,'backup'),r=await this.backupHash(job,state.backupSha);await chmod(path,0o755);await chmod(join(path,'manifest.json'),0o444);for(const [name,value] of Object.entries(r.files))if(value)await chmod(join(path,name),0o444);
    try{await this.run(this.helperArgs(job,'restore',dockerRestoreProgram,['/var/lib/multitap','/backup',job.operationId,state.backupSha]));await this.quiesceHelpers(job);}finally{await chmod(path,0o700);await chmod(join(path,'manifest.json'),0o600);for(const [name,value] of Object.entries(r.files))if(value)await chmod(join(path,name),0o600);}
    // Preserve the exact original override bytes for matching existing Compose
    // config/hash, rather than substituting a mutable image tag or edited YAML.
    const temporary=this.paths().override+'.restore-'+job.operationId,f=await open(temporary,'wx',0o600);try{await f.writeFile(job.previousOverride);await f.sync();}finally{await f.close();}await rename(temporary,this.paths().override);
    await this.save(job,'restore_ready');await this.run(this.compose(['up','--detach','--no-deps','--no-build','--pull','never','--force-recreate',this.config.serviceName]));await phase('health_check');if(!(await this.healthy(job.fromVersion,job)))fail('update_health_failed');await this.switchHelper(job,job.previousHelper);await this.save(job,'restored');return true;
  }
}

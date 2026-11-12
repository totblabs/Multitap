import { randomUUID } from 'node:crypto';
import { readFile, writeFile, open, rename, mkdir, lstat, statfs } from 'node:fs/promises';
import { join, dirname } from 'node:path';
import { activePhases, compareVersion, decodeFeed, digest, feedUrl, fetchBytes, publicUpdate } from './protocol.mjs';
import { unpackPackage, extractPackage } from './package.mjs';

const safeError=error=>/^update_[a-z_]{1,70}$/.test(error?.message)?error.message:'update_failed';
export async function atomicJson(path,value) {
  const temporary=path+'.'+randomUUID()+'.tmp';
  const file=await open(temporary,'wx',0o600);try{await file.writeFile(JSON.stringify(value));await file.sync();}finally{await file.close();}
  await rename(temporary,path);
  if(process.platform==='linux'){const directory=await open(dirname(path),'r');try{await directory.sync();}finally{await directory.close();}}
}
export class UpdateEngine {
  constructor({directory,feed,publicKey,adapter,fetcher=fetch,now=()=>new Date().toISOString(),reserveBytes=256*1024*1024,allowInsecureHttp=false}){
    if(typeof allowInsecureHttp!=='boolean')throw Error('update_feed_invalid');
    this.transportOptions={allowInsecureHttp};this.directory=directory;this.feed=feedUrl(feed,this.transportOptions);this.key=publicKey;this.adapter=adapter;this.fetcher=fetcher;this.now=now;this.reserve=reserveBytes;this.state={phase:'idle'};this.busy=false;this.task=Promise.resolve();
  }
  async initialize(){
    await mkdir(this.directory,{recursive:true,mode:0o700});
    const root=await lstat(this.directory);if(!root.isDirectory()||root.isSymbolicLink()||(root.mode&0o077))throw Error('update_storage_invalid');
    try{this.state=JSON.parse(await readFile(join(this.directory,'status.json'),'utf8'));}catch(error){if(error.code!=='ENOENT')throw Error('update_storage_invalid');}
    if(activePhases.has(this.state.phase)||this.state.phase==='interrupted'){
      if(['checking','downloading','verifying'].includes(this.state.phase)||!this.state.job){await this.set({phase:'failed',errorCode:'update_interrupted',job:null});return;}
      this.busy=true;
      try{
        await this.set({phase:'interrupted',errorCode:'update_interrupted'});
        const recovered=await this.adapter.recover(this.state.job,this.phase.bind(this));
        await this.set({phase:recovered?'rolled_back':'recovery_required',errorCode:recovered?'update_interrupted':'update_recovery_required'});
      }catch{await this.set({phase:'recovery_required',errorCode:'update_recovery_required'});}
      finally{this.busy=false;}
    }
  }
  async status(){return publicUpdate(this.state,await this.adapter.version());}
  async set(values){this.state={...this.state,...values};await atomicJson(join(this.directory,'status.json'),this.state);}
  async phase(phase){if(!['backing_up','applying','restarting','health_check'].includes(phase))throw Error('update_phase_invalid');await this.set({phase});}
  check(){
    if(this.busy)throw Error('update_busy');
    if(this.state.phase==='recovery_required')throw Error('update_recovery_required');
    this.busy=true;
    this.task=(async()=>{
      await this.set({phase:'checking',errorCode:null,downloadedBytes:0,totalBytes:0,job:null,operationId:null});
      try{
        const {release,releaseId}=decodeFeed(await fetchBytes(this.feed,32768,this.fetcher,()=>{},this.transportOptions),this.key),current=await this.adapter.version();
        if(release.platform!==this.adapter.platform)throw Error('update_platform_unsupported');
        await this.set({phase:compareVersion(release.version,current)>0?'available':'current',release,releaseId,checkedAt:this.now()});
      }catch(error){await this.set({phase:'failed',errorCode:safeError(error),release:null,releaseId:null,checkedAt:this.now()});}
    })().finally(()=>{this.busy=false;});
    return {accepted:true};
  }
  apply({releaseId,fromVersion}){
    if(this.busy)throw Error('update_busy');
    if(this.state.phase!=='available'||releaseId!==this.state.releaseId||typeof fromVersion!=='string')throw Error('update_check_required');
    this.busy=true;
    const operationId=randomUUID(),release=this.state.release;
    this.task=(async()=>{
      let job;
      try{
        const current=await this.adapter.version();
        if(current!==fromVersion||compareVersion(release.version,current)<=0)throw Error('update_version_changed');
        const space=await statfs(this.directory);if(Number(space.bavail)*Number(space.bsize)<this.reserve+release.artifact.size*4)throw Error('update_disk_space');
        await this.set({phase:'downloading',operationId,errorCode:null,downloadedBytes:0,totalBytes:release.artifact.size,job:null});
        let progress=0;
        const bytes=await fetchBytes(new URL(release.artifact.path,this.feed),release.artifact.size,this.fetcher,total=>{progress=total;this.state.downloadedBytes=total;},this.transportOptions);
        await this.set({phase:'verifying',downloadedBytes:progress});
        if(bytes.length!==release.artifact.size||digest(bytes)!==release.artifact.sha256)throw Error('update_checksum_failed');
        const files=unpackPackage(bytes,release.version),stage=join(this.directory,operationId);
        await mkdir(stage,{mode:0o700});
        const archive=join(stage,'server.tar.gz');await writeFile(archive,bytes,{flag:'wx',mode:0o600});await extractPackage(files,join(stage,'verified'));
        job=await this.adapter.prepare({operationId,release,archive,stage,fromVersion});
        // The persisted job contains only validated immutable deployment paths.
        // No stop/switch/migration begins before this checkpoint is durable.
        await this.set({phase:'backing_up',job});
        await this.adapter.apply(job,this.phase.bind(this));
        if(await this.adapter.version()!==release.version)throw Error('update_health_failed');
        await this.set({phase:'succeeded',completedAt:this.now()});
      }catch(error){
        let restored=false;
        if(job){try{restored=await this.adapter.recover(job,this.phase.bind(this));}catch{}}
        await this.set({phase:job?(restored?'rolled_back':'recovery_required'):'failed',errorCode:restored?safeError(error):job?'update_recovery_required':safeError(error)});
      }
    })().finally(()=>{this.busy=false;});
    return {accepted:true,operationId};
  }
}

import http from 'node:http';
import { HttpError } from './errors.mjs';
import { phases } from '../update/protocol.mjs';

const metadataTtlMs = 10 * 60 * 1000, retryMs = 60 * 1000, requestMs = 4000;

// Only a local, installation-configured Unix socket is accepted. The broker
// never receives a shell command, package URL, root credential or Docker socket.
export class ServerUpdateClient {
  constructor(socketPath,{now=Date.now}={}){
    if(typeof socketPath!=='string'||!/^\/[A-Za-z0-9_./-]{1,180}$/.test(socketPath)||socketPath.split('/').includes('..'))throw Error('invalid_update_socket');
    this.socketPath=socketPath;
    this.now=now;this.discovery=null;this.retryAt=0;this.observedSuccess=null;
  }
  async request(path,input,{timeoutMs=requestMs}={}){
    return new Promise((resolve,reject)=>{
      const payload=input===undefined?null:Buffer.from(JSON.stringify(input));let length=0;const chunks=[];
      const req=http.request({socketPath:this.socketPath,path,method:payload?'POST':'GET',headers:payload?{'Content-Type':'application/json','Content-Length':payload.length}:{},timeout:timeoutMs},res=>{
        res.on('data',chunk=>{length+=chunk.length;if(length>16384){req.destroy();reject(new HttpError(503,'server_update_unavailable'));}else chunks.push(chunk);});
        res.on('error',()=>reject(new HttpError(503,'server_update_unavailable')));
        res.on('end',()=>{try{const value=JSON.parse(Buffer.concat(chunks).toString('utf8'));if(res.statusCode!==200&&res.statusCode!==202)throw new HttpError(res.statusCode===409?409:503,/^update_[a-z_]+$/.test(value?.error)?value.error:'server_update_unavailable');resolve(value);}catch(error){reject(error instanceof HttpError?error:new HttpError(503,'server_update_unavailable'));}});
      });
      // An absolute deadline also bounds a peer that keeps sending small chunks.
      const deadline=setTimeout(()=>req.destroy(new Error('update_timeout')),timeoutMs);deadline.unref?.();
      req.on('close',()=>clearTimeout(deadline));
      req.on('timeout',()=>req.destroy());req.on('error',()=>reject(new HttpError(503,'server_update_unavailable')));req.end(payload);
    });
  }
  async status(options){
    const value=await this.request('/status',undefined,options);
    if(!phases.has(value?.state)||typeof value.feedConfigured!=='boolean'||typeof value.applySupported!=='boolean')throw new HttpError(503,'server_update_unavailable');
    return value;
  }
  // Discovery uses the existing updater's signed metadata check, never apply.
  // Only one discovery runs per broker, including across administrator sessions.
  discover({authorize=()=>{}}={}){
    if(this.discovery)return this.discovery;
    const started=performance.now();
    const remaining=()=>{const ms=Math.floor(requestMs-(performance.now()-started));if(ms<=0)throw new HttpError(503,'server_update_unavailable');return {timeoutMs:ms};};
    this.discovery=(async()=>{
      const value=await this.status(remaining()),now=this.now();
      if(!value.feedConfigured)return value;
      // Do not erase an apply failure, rollback or recovery requirement. An
      // explicit check remains available after the administrator reviews it.
      // Apply may fail (for example, disk space) before it writes operationId.
      // Its verified release remains present; only failed metadata checks clear it.
      const metadataFailure=value.state==='failed'&&!value.operationId&&!value.latest&&!value.releaseId&&value.errorCode!=='update_interrupted';
      if(!['idle','current','available','succeeded'].includes(value.state)&&!metadataFailure)return value;
      // Give the console time to observe successful completion/reload before
      // checking a later release. Persisted successes get the same grace period.
      if(value.state==='succeeded'){
        const marker=value.operationId??value.releaseId??value.currentVersion;
        if(marker!==this.observedSuccess){this.observedSuccess=marker;this.retryAt=Math.max(this.retryAt,now+metadataTtlMs);return value;}
      }
      const checkedAt=Date.parse(value.checkedAt),ttl=value.state==='failed'?retryMs:metadataTtlMs;
      if(now<this.retryAt||(Number.isFinite(checkedAt)&&checkedAt<=now&&now-checkedAt<ttl))return value;
      // The status request yielded: a logged-out or demoted administrator is
      // no longer allowed to enqueue even this metadata-only check.
      authorize();
      this.retryAt=now+retryMs;
      try{await this.check(remaining());}
      catch(error){if(error?.code!=='update_busy')throw error;}
      return await this.status(remaining());
    })().catch(error=>{if(![401,403].includes(error?.status))this.retryAt=this.now()+retryMs;throw error;}).finally(()=>{this.discovery=null;});
    return this.discovery;
  }
  check(options){return this.request('/check',{},options);}
  apply(input){return this.request('/apply',input);}
}

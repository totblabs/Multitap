import { createHash } from 'node:crypto';
import { WebSocketServer, WebSocket } from 'ws';
import { tokenHash } from './security.mjs';

const timestamp = /(?:At|_at)$/;
function stable(value, depth=0, omit=null) {
  if(depth>12)throw new Error('snapshot_bounds');
  if(value===null||typeof value==='boolean'||typeof value==='string')return value;
  if(typeof value==='number'){if(!Number.isFinite(value))throw new Error('snapshot_bounds');return value;}
  if(Array.isArray(value)){if(value.length>10000)throw new Error('snapshot_bounds');return value.map(item=>stable(item,depth+1,omit));}
  if(!value||typeof value!=='object')throw new Error('snapshot_bounds');
  const keys=Object.keys(value).filter(key=>!timestamp.test(key)&&!(depth===0&&omit?.has(key))).sort();
  if(keys.length>128)throw new Error('snapshot_bounds');
  return Object.fromEntries(keys.map(key=>[key,stable(value[key],depth+1,omit)]));
}
// omit: top-level desktop keys left out (the live resources feed carries CPU/RAM itself).
export function desktopFingerprint(desktops,{omit}={}) {
  const skip=omit?new Set(omit):null;
  if(!Array.isArray(desktops)||desktops.length>10000)throw new Error('snapshot_bounds');
  const hash=createHash('sha256');let bytes=0;
  for(const desktop of [...desktops].sort((a,b)=>String(a.id).localeCompare(String(b.id)))) {
    const encoded=JSON.stringify(stable(desktop,0,skip));bytes+=Buffer.byteLength(encoded)+1;
    if(bytes>2*1024*1024)throw new Error('snapshot_bounds');
    hash.update(encoded);hash.update('\n');
  }
  return hash.digest('hex');
}
// Optional pushes a client asks for with X-ToTB-Live: a client that does not ask
// (a released one) gets only ready/changed and keeps its list-reread path.
// notices (notice-v1, notices.mjs): a contentless push; the client then reads
// GET /api/v1/notices.
const liveKnown=new Set(['resources','catalog','notices']);
function liveFeatures(header) {
  if(typeof header!=='string'||header.length>64)return new Set();
  return new Set(header.split(',').map(item=>item.trim().toLowerCase()).filter(item=>liveKnown.has(item)));
}
const resourceChunk=40;
function resourceMap(desktops) {
  const map=new Map();
  for(const desktop of desktops) {
    const resources=desktop?.resources;
    if(!resources||typeof resources!=='object'||typeof desktop.id!=='string')continue;
    const {checkedAt,...rest}=resources;
    map.set(desktop.id,{sig:JSON.stringify(rest),value:resources});
  }
  return map;
}
export class DesktopEvents {
  constructor({store,publicOrigin,snapshot,refresh,tickMs=1000,authorizationMs=250,heartbeatMs=10000,refreshTimeoutMs=9000,maxClients=500,maxQueue=16384,now=Date.now}) {
    Object.assign(this,{store,publicOrigin,snapshot,refresh,refreshTimeoutMs,maxClients:Math.min(500,maxClients),maxQueue,now});
    this.clients=new Map();this.catalogRevision=0;this.noticesRevision=0;this.closed=false;this.ticking=false;this.scheduled=false;
    this.wss=new WebSocketServer({noServer:true,maxPayload:1024,maxFragments:16,perMessageDeflate:false,closeTimeout:1000});
    this.timer=setInterval(()=>this.notify(),tickMs);this.timer.unref();
    this.authorization=setInterval(()=>this.revalidate(),authorizationMs);this.authorization.unref();
    this.heartbeat=setInterval(()=>this.checkHeartbeat(),heartbeatMs);this.heartbeat.unref();
  }
  identity(hash) {
    const user=this.store.authenticate(hash,this.now());
    return user&&!user.must_change_credentials?user:null;
  }
  revocationReason(entry) { return this.store.sessionRevocationReason(entry.hash, this.now(), entry.userId) ?? 'authorization_revoked'; }
  upgrade(req,socket,head,url,reject) {
    if(url.pathname!=='/api/v1/events')return false;
    const authorizationHeaders=req.rawHeaders.filter((value,index)=>index%2===0&&value.toLowerCase()==='authorization').length;
    if(req.method!=='GET'||req.url!=='/api/v1/events'||url.search||url.hash||req.headers.origin!==(req.brokerOrigin??this.publicOrigin)||authorizationHeaders>1||req.headers['sec-websocket-protocol']!==undefined){reject(403,'Forbidden');return true;}
    const token=/^Bearer ([A-Za-z0-9_-]{43})$/.exec(req.headers.authorization??'')?.[1];
    const hash=token?tokenHash(token):null,user=hash?this.identity(hash):null;
    if(!user){reject(401,'Unauthorized');return true;}
    if(this.closed){reject(503,'Unavailable');return true;}
    if(this.clients.has(hash)){reject(409,'Conflict');return true;}
    if(this.wss.clients.size>=this.maxClients){reject(503,'Unavailable');return true;}
    this.wss.handleUpgrade(req,socket,head,ws=>{
      const entry={ws,hash,userId:user.id,live:liveFeatures(req.headers['x-totb-live']),sentResources:new Map(),resourcesRevision:0,fingerprint:null,revision:0,alive:true,createdAt:this.now(),windowStart:this.now(),controls:0};
      this.clients.set(hash,entry);
      const cleanup=()=>{if(this.clients.get(hash)===entry)this.clients.delete(hash);};
      ws.on('close',cleanup);ws.on('error',cleanup);
      ws.on('message',()=>ws.close(1008,'unexpected_message'));
      const control=()=>{const now=this.now();if(now-entry.windowStart>=1000){entry.windowStart=now;entry.controls=0;}if(++entry.controls>16){ws.close(1008,'rate_limited');return false;}return true;};
      ws.on('ping',control);ws.on('pong',()=>{if(control())entry.alive=true;});
      this.notify();
    });
    return true;
  }
  revalidate() {
    if(this.closed)return;
    for(const entry of this.clients.values()) {
      const user=this.identity(entry.hash);
      if(!user||user.id!==entry.userId)entry.ws.close(1008,this.revocationReason(entry));
    }
  }
  checkHeartbeat() {
    if(this.closed)return;
    this.revalidate();
    for(const entry of this.clients.values()) {
      if(entry.ws.readyState!==WebSocket.OPEN)continue;
      if(!entry.alive){entry.ws.terminate();continue;}
      if(this.now()-entry.createdAt>8*60*60*1000){entry.ws.close(1001,'watch_expired');continue;}
      entry.alive=false;entry.ws.ping();
    }
  }
  notify() {
    if(this.closed||!this.clients.size)return;
    this.revalidate();
    if(this.ticking||this.scheduled)return;
    this.scheduled=true;
    queueMicrotask(()=>{this.scheduled=false;void this.tick();});
  }
  send(entry,value) {
    if(entry.ws.readyState!==WebSocket.OPEN)return;
    if(entry.ws.bufferedAmount>this.maxQueue){entry.ws.close(1013,'events_backpressure');return;}
    entry.ws.send(JSON.stringify(value),error=>{if(error)entry.ws.terminate();});
  }
  // Only the values that changed since this connection last got them, for the
  // VMs of this user's own snapshot; a VM that left it is cleared with null.
  sendResources(entry,map) {
    if(entry.fingerprint===null)return;
    const changed=[];
    for(const [id,{sig,value}] of map)if(entry.sentResources.get(id)!==sig){entry.sentResources.set(id,sig);changed.push({id,resources:value});}
    for(const id of [...entry.sentResources.keys()])if(!map.has(id)){entry.sentResources.delete(id);changed.push({id,resources:null});}
    for(let index=0;index<changed.length;index+=resourceChunk) {
      if(entry.resourcesRevision===Number.MAX_SAFE_INTEGER){entry.ws.close(1012,'watch_expired');return;}
      this.send(entry,{type:'resources',revision:++entry.resourcesRevision,desktops:changed.slice(index,index+resourceChunk)});
    }
  }
  // The public download catalog changed: clients that asked for it re-read it.
  catalogChanged() {
    if(this.closed)return;
    const revision=++this.catalogRevision;
    for(const entry of this.clients.values())if(entry.live.has('catalog')&&entry.fingerprint!==null)this.send(entry,{type:'catalog',revision});
  }
  // notice-v1: these accounts (null: everyone) have notices to read. Only
  // connections that asked for it and already have their first list hear it.
  noticesChanged(userIds=null) {
    if(this.closed)return;
    const users=userIds?new Set(userIds):null,revision=++this.noticesRevision;
    for(const entry of this.clients.values())if(entry.live.has('notices')&&entry.fingerprint!==null&&(!users||users.has(entry.userId)))this.send(entry,{type:'notices',revision});
  }
  // Whether any client keeps the live channel open (the resources sampler's
  // fast rate applies only then).
  get watching() { return this.clients.size>0; }
  async tick() {
    if(this.closed||this.ticking||!this.clients.size)return;
    this.ticking=true;let timeout;
    try {
      this.revalidate();
      const userIds=[...new Set([...this.clients.values()].filter(entry=>entry.ws.readyState===WebSocket.OPEN&&this.identity(entry.hash)?.id===entry.userId).map(entry=>entry.userId))];
      if(!userIds.length)return;
      await Promise.race([Promise.resolve().then(()=>this.refresh(userIds)),new Promise((_,reject)=>{timeout=setTimeout(()=>reject(new Error('snapshot_timeout')),this.refreshTimeoutMs);timeout.unref();this.cancelRefresh=()=>reject(new Error('watch_closed'));})]);
      if(this.closed)return;
      const fingerprints=new Map();
      for(const entry of this.clients.values()) {
        const user=this.identity(entry.hash);
        if(!user||user.id!==entry.userId){entry.ws.close(1008,this.revocationReason(entry));continue;}
        if(entry.ws.readyState!==WebSocket.OPEN)continue;
        try {
          if(!fingerprints.has(user.id)){const desktops=await this.snapshot(entry.hash);fingerprints.set(user.id,{full:desktopFingerprint(desktops),core:desktopFingerprint(desktops,{omit:['resources']}),resources:resourceMap(desktops)});}
          if(this.closed)return;
          if(this.identity(entry.hash)?.id!==entry.userId){entry.ws.close(1008,this.revocationReason(entry));continue;}
          const snap=fingerprints.get(user.id),fingerprint=entry.live.has('resources')?snap.core:snap.full;
          if(entry.fingerprint===null){entry.fingerprint=fingerprint;this.send(entry,{type:'desktops.ready'});}
          else if(entry.fingerprint!==fingerprint){entry.fingerprint=fingerprint;if(entry.revision===Number.MAX_SAFE_INTEGER){entry.ws.close(1012,'watch_expired');continue;}this.send(entry,{type:'desktops.changed',revision:++entry.revision});}
          if(entry.live.has('resources'))this.sendResources(entry,snap.resources);
        } catch {entry.ws.close(1013,'snapshot_unavailable');}
      }
    } catch {
      if(!this.closed)for(const entry of this.clients.values())entry.ws.close(1013,'snapshot_unavailable');
    } finally {clearTimeout(timeout);this.cancelRefresh=null;this.ticking=false;}
  }
  close() {
    if(this.closed)return;this.closed=true;
    clearInterval(this.timer);clearInterval(this.authorization);clearInterval(this.heartbeat);
    this.cancelRefresh?.();
    for(const entry of this.clients.values())entry.ws.close(1001,'server_stopping');
    this.clients.clear();this.wss.close();
  }
}

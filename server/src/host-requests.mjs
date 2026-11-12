import { randomUUID } from 'node:crypto';
import { HttpError, fields } from './errors.mjs';
import { newToken, tokenHash, text } from './security.mjs';
import { hostRequestCode } from './verification-codes.mjs';
import { pcRegistration, approveLocalPc } from './local-pc.mjs';
import { HostVmLookup } from './host-vm-lookup.mjs';

const uuid=/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i;
const key=value=>typeof value==='string'&&/^[A-Za-z0-9_-]{43}$/.test(value);
const validUuid=value=>typeof value==='string'&&uuid.test(value)&&!/^0{8}(?:-0{4}){3}-0{12}$/.test(value)&&!/^f{8}(?:-f{4}){3}-f{12}$/i.test(value);
class Limit {
  constructor(count){this.count=count;this.entries=new Map();}
  take(id,now){let row=this.entries.get(id);if(!row||row.until<=now){if(this.entries.size>=10000)this.entries.delete(this.entries.keys().next().value);row={count:0,until:now+60000};this.entries.set(id,row);}return ++row.count<=this.count;}
}

// A request's SMBIOS UUID/hostname are unverified claims until an administrator
// explicitly approves it after a fresh Proxmox identity check. Never auto-enroll.
// Anyone who can read a VM's SMBIOS UUID can file a request claiming it, so
// the console shows each request's verification code (host-request-code-v1)
// to compare with the Host's own settings window, and a VM claimed by more
// than one live request cannot be approved until the extra ones are rejected.
// A request from a VM that never had a Host is placed by looking its UUID up
// in Proxmox (host-vm-lookup.mjs); that only fills in node and VM ID for the
// administrator, it proves nothing about who sent the request.
const maxClaimsPerMachine=4;
export class HostRequests {
  constructor({store,vault,proxmox,changed=()=>{},now=Date.now,ttlMs=600000,maxPending=100,maxRows=1000,createLimit=20,statusLimit=60,statusAddressLimit=600,code=hostRequestCode,lookup,lookupOptions={},lookupWaitMs=2500}) {
    Object.assign(this,{store,vault,proxmox,changed,now,ttlMs,code,lookupWaitMs,maxPending:Math.min(100,maxPending),maxRows:Math.min(1000,maxRows)});
    this.lookup=lookup??new HostVmLookup({proxmox,now,...lookupOptions});
    this.creates=new Limit(createLimit);this.polls=new Limit(statusLimit);this.addressPolls=new Limit(statusAddressLimit);
    // Requests whose Host declared (X-Multitap-Host-Request-Code) that it shows
    // the code. Self-reported and kept in memory: Hosts poll every 3 s, so a
    // restart relearns it at once. A Host that does not show the code never sends it.
    this.codeHosts=new Map();
  }
  showsCode(id,shows){if(!shows)return;this.codeHosts.delete(id);this.codeHosts.set(id,true);if(this.codeHosts.size>2*this.maxRows)this.codeHosts.delete(this.codeHosts.keys().next().value);}
  // Other live pending requests that claim the same machine (and so the same VM).
  rivalRows(row,now=this.now()){return this.store.db.prepare("SELECT id,key_hash FROM host_requests WHERE machine_uuid=? AND status='pending' AND expires_at>? AND id<>? ORDER BY created_at,id").all(row.machine_uuid,now,row.id);}
  rivals(row,now=this.now()){return this.rivalRows(row,now).map(value=>value.id);}
  // With rival claims the administrator must state that this request's code
  // matches the Host's own window. That is impossible to check for a Host that
  // shows no code, and meaningless when a rival carries the same code.
  assertSingleClaim(row,acknowledged,now=this.now()){
    const rivals=this.rivalRows(row,now);if(!rivals.length)return;
    if(!this.codeHosts.has(row.id))throw new HttpError(409,'host_request_duplicate_unverifiable');
    const code=this.code(row.key_hash);if(rivals.some(value=>this.code(value.key_hash)===code))throw new HttpError(409,'host_request_code_collision');
    if(acknowledged!==true)throw new HttpError(409,'host_request_duplicate');
  }
  commissioned(){if(!this.vault)throw new HttpError(503,'admin_unavailable');if(this.store.setting('proxmox_initialized')!=='1'||!this.store.setting('proxmox'))throw new HttpError(428,'proxmox_setup_required');}
  prune(){const now=this.now();this.store.db.prepare("UPDATE host_requests SET status='expired',credentials_sealed=NULL WHERE expires_at<=? AND status IN ('pending','approved')").run(now);this.store.db.prepare('DELETE FROM host_requests WHERE expires_at<=?').run(now-86400000);}
  public(row){return {requestId:row.id,status:row.status,expiresAt:new Date(row.expires_at).toISOString(),retryAfterSeconds:3};}
  create(input,address='unknown',showsCode=false){
    this.commissioned();if(!this.creates.take(address,this.now()))throw new HttpError(429,'too_many_host_requests');
    fields(input,['requestKey','hostname','machineUuid','version']);
    if(!key(input.requestKey)||!validUuid(input.machineUuid))throw new HttpError(400,'invalid_host_request');
    let hostname,version;try{hostname=text(input.hostname,128);version=text(input.version,32);}catch{throw new HttpError(400,'invalid_host_metadata');}
    if(!/^\d+\.\d+\.\d+$/.test(version))throw new HttpError(400,'invalid_host_metadata');
    const hash=tokenHash(input.requestKey),machineUuid=input.machineUuid.toLowerCase();this.prune();
    const existing=this.store.db.prepare('SELECT * FROM host_requests WHERE key_hash=?').get(hash);
    if(existing){if(existing.machine_uuid!==machineUuid||existing.hostname!==hostname||existing.version!==version)throw new HttpError(409,'host_request_conflict');this.showsCode(existing.id,showsCode);return this.public(existing);}
    // A second claim on the same machine is accepted and flagged instead of
    // refused: refusing let whoever asked first (possibly not the real Host)
    // hold the only request the administrator could see.
    if(this.store.db.prepare("SELECT count(*) AS n FROM host_requests WHERE machine_uuid=? AND status='pending' AND expires_at>?").get(machineUuid,this.now()).n>=maxClaimsPerMachine)throw new HttpError(409,'host_request_pending');
    if(this.store.db.prepare("SELECT count(*) AS n FROM host_requests WHERE status='pending'").get().n>=this.maxPending||this.store.db.prepare('SELECT count(*) AS n FROM host_requests').get().n>=this.maxRows)throw new HttpError(429,'host_request_capacity');
    const id=randomUUID(),now=this.now(),expiresAt=now+this.ttlMs;
    this.store.db.prepare("INSERT INTO host_requests(id,key_hash,hostname,machine_uuid,version,status,created_at,expires_at) VALUES(?,?,?,?,?,'pending',?,?)").run(id,hash,hostname,machineUuid,version,now,expiresAt);
    this.store.audit('host_request_created','unverified-host',id,now);this.showsCode(id,showsCode);
    return this.public(this.store.db.prepare('SELECT * FROM host_requests WHERE id=?').get(id));
  }
  claimed(input,address){
    fields(input,['requestId','requestKey']);
    if(!key(input.requestKey)||typeof input.requestId!=='string'||!uuid.test(input.requestId))throw new HttpError(400,'invalid_host_request');
    const hash=tokenHash(input.requestKey),now=this.now();
    if(!this.addressPolls.take(address,now)||!this.polls.take(hash,now))throw new HttpError(429,'too_many_host_request_polls');
    this.prune();const row=this.store.db.prepare('SELECT * FROM host_requests WHERE id=? AND key_hash=?').get(input.requestId,hash);
    if(!row)throw new HttpError(404,'host_request_not_found');return row;
  }
  currentCredentials(row){
    if(!row.credentials_sealed||!this.vault)throw new HttpError(503,'host_request_credentials_unavailable');
    let sealed;try{sealed=this.vault.open(row.credentials_sealed);}catch{throw new HttpError(503,'host_request_credentials_unavailable');}
    const value=sealed?.credentials;
    if(sealed?.kind!=='host-request-v1'||sealed.requestId!==row.id||!value||!key(value.hostToken)||value.hostId!==row.host_id||value.desktopId!==row.desktop_id)throw new HttpError(503,'host_request_credentials_unavailable');
    const host=this.store.authenticateHost(tokenHash(value.hostToken));
    return host?.id===value.hostId&&host.desktop_id===value.desktopId?value:null;
  }
  status(input,address='unknown',showsCode=false){
    const row=this.claimed(input,address);this.showsCode(row.id,showsCode);
    if(row.status==='approved'){
      const credentials=this.currentCredentials(row);
      if(!credentials){this.store.db.prepare("UPDATE host_requests SET status='rejected',credentials_sealed=NULL WHERE id=? AND status='approved'").run(row.id);return this.public({...row,status:'rejected'});}
      return {...this.public(row),credentials};
    }
    return this.public(row);
  }
  cancel(input,address='unknown'){
    const row=this.claimed(input,address);
    if(row.status==='approved')throw new HttpError(409,'host_request_already_approved');
    if(row.status==='pending'){this.store.db.prepare("UPDATE host_requests SET status='rejected',decided_at=?,decision_actor='request-owner' WHERE id=? AND status='pending'").run(this.now(),row.id);this.store.audit('host_request_cancelled','request-owner',row.id,this.now());return this.public({...row,status:'rejected'});}
    return this.public(row);
  }
  // The stored Proxmox settings while they can be used, else null.
  lookupRevision(){try{this.commissioned();return this.store.setting('proxmox')??null;}catch{return null;}}
  // Where Proxmox says this request's machine is, for a request the desktops
  // table cannot place. state: known (the table places it) | matched |
  // ambiguous | not_found | unavailable | searching (host-vm-lookup.mjs).
  vmLookup(row,candidates,revision=this.lookupRevision()){
    if(candidates.length)return {state:'known'};
    if(!revision)return {state:'unavailable',errorCode:'proxmox_setup_required'};
    const view=this.lookup.view(revision,row.machine_uuid,row.created_at);
    if(view.state!=='matched')return view;
    const rows=this.store.db.prepare("SELECT d.id,EXISTS(SELECT 1 FROM hosts h WHERE h.desktop_id=d.id) AS has_host FROM desktops d WHERE d.kind='vm' AND d.pve_vmid=? ORDER BY d.id").all(view.vm.vmid);
    return {state:'matched',vm:{node:view.vm.node,vmid:view.vm.vmid,name:view.vm.name??`VM ${view.vm.vmid}`,desktopId:rows.length===1?rows[0].id:null,hasExistingHost:rows.some(value=>!!value.has_host)}};
  }
  // The console's request list: first let Proxmox place the requests the
  // table cannot (bounded and rate-limited there), waiting only briefly. A
  // read that takes longer shows as 'searching'; the next poll picks it up.
  async pendingResolved(){
    const revision=this.lookupRevision();
    if(revision){
      this.prune();
      const wanted=this.store.db.prepare("SELECT r.machine_uuid AS uuid,r.created_at AS createdAt FROM host_requests r WHERE r.status='pending' AND NOT EXISTS (SELECT 1 FROM desktops d WHERE d.kind='vm' AND d.pve_available=1 AND d.pve_machine_uuid=r.machine_uuid AND d.pve_machine_checked_at IS NOT NULL) ORDER BY r.created_at,r.id LIMIT ?").all(this.maxPending);
      let config=null;try{config=this.vault.open(revision);}catch{/* Unreadable settings: nothing is looked up. */}
      const running=config?this.lookup.refresh(revision,config,wanted):null;
      if(running){let timer;await Promise.race([running,new Promise(resolve=>{timer=setTimeout(resolve,this.lookupWaitMs);timer.unref?.();})]);clearTimeout(timer);}
    }
    return this.pending();
  }
  pending(){
    this.prune();const revision=this.lookupRevision();
    const requests=this.store.db.prepare("SELECT * FROM host_requests WHERE status='pending' ORDER BY created_at,id LIMIT ?").all(this.maxPending).map(row=>{
      const candidates=this.store.db.prepare("SELECT d.id,d.pve_vmid,d.name,d.pve_node,EXISTS(SELECT 1 FROM hosts h WHERE h.desktop_id=d.id) AS has_host FROM desktops d WHERE d.kind='vm' AND d.pve_available=1 AND d.pve_machine_uuid=? AND d.pve_machine_checked_at IS NOT NULL ORDER BY d.pve_vmid").all(row.machine_uuid).map(vm=>({desktopId:vm.id,vmid:vm.pve_vmid,name:vm.name,node:vm.pve_node,hasExistingHost:!!vm.has_host}));
      // codeDisplay: 'shown' when this Host declared that its settings window
      // shows the same code; 'unavailable' for released Hosts that cannot.
      const rivals=this.rivals(row),vmLookup=this.vmLookup(row,candidates,revision);
      return {...this.public(row),hostname:row.hostname,machineUuid:row.machine_uuid,version:row.version,createdAt:new Date(row.created_at).toISOString(),matchState:candidates.length===1?'matched':candidates.length?'ambiguous':'unknown',suggestedDesktopId:candidates.length===1?candidates[0].desktopId:null,candidates,
        verificationCode:this.code(row.key_hash),codeDisplay:this.codeHosts.has(row.id)?'shown':'unavailable',duplicateRequestIds:rivals,vmLookup,localPcRegistration:pcRegistration(this,row,candidates,vmLookup)};
    });return {requests,pendingCount:requests.length};
  }
  request(id){if(typeof id!=='string'||!uuid.test(id))throw new HttpError(400,'invalid_host_request');this.prune();const row=this.store.db.prepare('SELECT * FROM host_requests WHERE id=?').get(id);if(!row)throw new HttpError(404,'host_request_not_found');return row;}
  approved(row,desktopId,replaceExisting){
    if(!row)throw new HttpError(409,'host_request_conflict');
    if(row.expires_at<=this.now())throw new HttpError(409,'host_request_expired');
    if(row.status!=='approved')return null;
    if(row.desktop_id!==desktopId||!!row.replace_existing!==replaceExisting||!this.currentCredentials(row))throw new HttpError(409,'host_request_conflict');
    return {ok:true,requestId:row.id,status:'approved',desktopId:row.desktop_id};
  }
  async approve(id,input,authorize){
    this.commissioned();fields(input,['desktopId','node','vmid','acknowledgeCode','replaceExisting','acknowledgeDuplicates','serverMatch']);
    const known=input.desktopId!==undefined;
    if((known ? typeof input.desktopId!=='string'||!/^pve-[0-9]+$/.test(input.desktopId)||input.node!==undefined||input.vmid!==undefined : typeof input.node!=='string'||!/^[A-Za-z0-9_.-]{1,128}$/.test(input.node)||!Number.isSafeInteger(input.vmid)||input.vmid<100)||
      (input.replaceExisting!==undefined&&typeof input.replaceExisting!=='boolean')||(input.acknowledgeDuplicates!==undefined&&typeof input.acknowledgeDuplicates!=='boolean')||(input.acknowledgeCode!==undefined&&typeof input.acknowledgeCode!=='boolean')||(input.serverMatch!==undefined&&(typeof input.serverMatch!=='boolean'||known)))throw new HttpError(400,'invalid_host_approval');
    // serverMatch: the console sends back the node and VM ID that this server
    // found for the request's UUID (vmLookup 'matched'). The client cannot
    // choose the VM this way: see assertServerMatch.
    // Every path (a VM the table knows, the server's match, typed node/VM, and
    // replacing a registered Host) needs the administrator's explicit tick that
    // the code on the Host screen is the same: anyone who knows a VM's SMBIOS
    // UUID can file a request, and only the code shows it came from that VM.
    const replaceExisting=input.replaceExisting===true,auto=!known&&input.serverMatch===true;authorize();const first=this.request(id);
    this.assertCodeVerified(id,input);
    if(auto)this.assertServerMatch(first,input);
    const matches=known?this.store.db.prepare("SELECT * FROM desktops WHERE id=? AND kind='vm' AND pve_vmid IS NOT NULL").all(input.desktopId):this.store.db.prepare("SELECT * FROM desktops WHERE kind='vm' AND pve_vmid=?").all(input.vmid);
    if(matches.length>1)throw new HttpError(409,'desktop_changed');
    const desktop=matches[0]??null;
    if(known&&!desktop)throw new HttpError(404,'desktop_not_found');
    const targetId=desktop?.id??`pve-${input.vmid}`,vmid=desktop?.pve_vmid??input.vmid,node=known?desktop.pve_node:input.node;
    const vmRows=this.store.db.prepare("SELECT id FROM desktops WHERE kind='vm' AND pve_vmid=? ORDER BY id").all(vmid).map(value=>value.id);
    if(vmRows.length>1)throw new HttpError(409,'desktop_changed');
    if(typeof node!=='string'||!/^[A-Za-z0-9_.-]{1,128}$/.test(node))throw new HttpError(409,'host_vm_mapping_required');
    if(!desktop&&this.store.db.prepare('SELECT 1 FROM desktops WHERE id=?').get(targetId))throw new HttpError(409,'desktop_changed');
    const idempotent=this.approved(first,targetId,replaceExisting);if(idempotent){if(!known&&desktop?.pve_node!==node)throw new HttpError(409,'host_request_conflict');return idempotent;}
    if(first.status!=='pending')throw new HttpError(409,'host_request_'+first.status);
    this.assertSingleClaim(first,input.acknowledgeDuplicates);
    const originalHost=this.store.db.prepare('SELECT id FROM hosts WHERE desktop_id=?').get(targetId)?.id??null;
    if(originalHost&&!replaceExisting)throw new HttpError(409,'host_already_registered');
    const revision=this.store.setting('proxmox'),config=this.vault.open(revision);
    const identity=await this.proxmox.machineIdentity(config,vmid,{node,expectedUuid:first.machine_uuid});
    const actor=authorize();this.commissioned();
    if(this.store.setting('proxmox')!==revision)throw new HttpError(409,'proxmox_settings_changed');
    if(!validUuid(identity?.machineUuid)||identity.machineUuid.toLowerCase()!==first.machine_uuid)throw new HttpError(409,'host_machine_mismatch');
    const result=this.store.transaction(()=>{
      authorize();this.commissioned();const now=this.now(),row=this.store.db.prepare('SELECT * FROM host_requests WHERE id=?').get(id);
      if(this.store.setting('proxmox')!==revision)throw new HttpError(409,'proxmox_settings_changed');
      const repeated=this.approved(row,targetId,replaceExisting);if(repeated){const bound=this.store.db.prepare('SELECT pve_node,pve_vmid FROM desktops WHERE id=?').get(targetId);if(bound?.pve_node!==node||bound.pve_vmid!==vmid)throw new HttpError(409,'host_request_conflict');return repeated;}
      if(!row||row.status!=='pending'||row.expires_at<=now)throw new HttpError(409,row?.expires_at<=now?'host_request_expired':'host_request_conflict');
      if(['key_hash','machine_uuid','hostname','version','created_at','expires_at'].some(key=>row[key]!==first[key]))throw new HttpError(409,'host_request_conflict');
      this.assertCodeVerified(id,input);
      if(auto)this.assertServerMatch(row,input,revision);
      this.assertSingleClaim(row,input.acknowledgeDuplicates,now);
      const current=this.store.db.prepare('SELECT * FROM desktops WHERE id=?').get(targetId);
      if(JSON.stringify(this.store.db.prepare("SELECT id FROM desktops WHERE kind='vm' AND pve_vmid=? ORDER BY id").all(vmid).map(value=>value.id))!==JSON.stringify(vmRows))throw new HttpError(409,'desktop_changed');
      if(desktop ? !current||['id','kind','pve_vmid','pve_node','pve_machine_uuid','pve_available'].some(key=>current[key]!==desktop[key]) : !!current||!!this.store.db.prepare('SELECT 1 FROM desktops WHERE pve_vmid=?').get(vmid))throw new HttpError(409,'desktop_changed');
      if((this.store.db.prepare('SELECT id FROM hosts WHERE desktop_id=?').get(targetId)?.id??null)!==originalHost)throw new HttpError(409,'host_registration_changed');
      if(!desktop)this.store.db.prepare("INSERT INTO desktops(id,name,kind,description,os,enabled,created_at,pve_vmid,pve_available,pve_node,pve_power_status,pve_seen_at) VALUES(?,?,'vm',?,'Windows',1,?,?,1,?,'unknown',?)").run(targetId,(auto&&this.matchedName(row,revision))||`VM ${vmid}`,`Proxmox VM ${vmid}`,now,vmid,node,now);
      this.store.db.prepare('UPDATE desktops SET pve_available=1,pve_node=?,pve_os_type=?,pve_os_family=?,pve_os_source=? WHERE id=?').run(node,identity.pveOsType??null,identity.osFamily??'unknown',identity.osSource??'unknown',targetId);
      const hostId=randomUUID(),hostToken=newToken(),credentials={hostId,hostToken,desktopId:targetId};
      const sealed=this.vault.seal({kind:'host-request-v1',requestId:id,credentials});
      this.store.db.prepare("UPDATE host_requests SET status='rejected',credentials_sealed=NULL WHERE desktop_id=? AND status='approved'").run(targetId);
      // Same machine (a reinstall) keeps the assignments; another machine does not inherit them.
      this.store.replacingHost(targetId,row.machine_uuid,actor,now);
      this.store.db.prepare('DELETE FROM hosts WHERE desktop_id=?').run(targetId);
      this.store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run(hostId,targetId,tokenHash(hostToken),row.hostname,row.machine_uuid,row.version,now);
      this.store.db.prepare('DELETE FROM host_enrollments WHERE desktop_id=?').run(targetId);
      this.store.bindMachineIdentity(targetId,row.machine_uuid,actor,now);
      this.store.db.prepare("UPDATE host_requests SET status='approved',desktop_id=?,host_id=?,credentials_sealed=?,decision_actor=?,decided_at=?,replace_existing=? WHERE id=? AND status='pending'").run(targetId,hostId,sealed,actor,now,replaceExisting?1:0,id);
      this.store.audit('host_request_approved',actor,id,now);this.store.audit('host_enrolled',hostId,targetId,now);
      return {ok:true,requestId:id,status:'approved',desktopId:targetId};
    });this.changed();return result;
  }
  // The Host showed this request's code (host-request-code-v1) and the
  // administrator ticked that it matches. A Host too old to show the code
  // cannot be approved: it is updated first.
  assertCodeVerified(id,input){if(input.acknowledgeCode!==true||!this.codeHosts.has(id))throw new HttpError(409,'host_request_code_required');}
  // The node and VM ID of a serverMatch approval must be the single VM this
  // server itself found for the request's UUID under the current settings.
  assertServerMatch(row,input,revision=this.store.setting('proxmox')){
    const view=this.lookup.view(revision,row.machine_uuid,row.created_at);
    if(view.state!=='matched'||view.vm.node!==input.node||view.vm.vmid!==input.vmid)throw new HttpError(409,'host_request_match_changed');
  }
  // Proxmox's name of the matched VM, for a desktop row this approval creates.
  matchedName(row,revision){
    const name=this.lookup.view(revision,row.machine_uuid,row.created_at).vm?.name;
    try{return typeof name==='string'&&name.trim()?text(name.trim().slice(0,80),80):null;}catch{return null;}
  }
  // Whether Proxmox places this request's machine on a VM (so it is no local PC).
  liveVm(row){const revision=this.lookupRevision();return !!revision&&['matched','ambiguous'].includes(this.lookup.view(revision,row.machine_uuid,row.created_at).state);}
  approveLocalPc(id,input,authorize){return approveLocalPc(this,id,input,authorize);}
  reject(id,authorize){
    const actor=authorize();this.commissioned();const row=this.request(id);
    if(row.status==='approved'||row.status==='expired')throw new HttpError(409,'host_request_'+row.status);
    if(row.status==='pending'){this.store.db.prepare("UPDATE host_requests SET status='rejected',decided_at=?,decision_actor=? WHERE id=? AND status='pending'").run(this.now(),actor,id);this.store.audit('host_request_rejected',actor,id,this.now());}
    return {ok:true,requestId:id,status:'rejected'};
  }
}

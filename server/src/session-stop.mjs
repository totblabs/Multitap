import {randomUUID} from 'node:crypto';
import {HttpError} from './errors.mjs';

export const sessionStopProtocol='stop-v1';
const uuid=/^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i;
const resultFields=['type','sessionId','sessionStopProtocol','stopRequestId','workerQuiescent','profileRequired','profileRestored'];
// The Host's own report that a secure desktop (a UAC prompt, Ctrl+Alt+Del) of
// the unlocked console session shows: a current Host says so (secureDesktop);
// an older Host reports the same state as an interactive session whose display
// probe found no interactive desktop.
export function secureDesktopShowing(capabilities){
  return capabilities?.service===true&&capabilities.interactiveSession===true&&capabilities.streaming===false&&capabilities.virtualMonitorControl!==true&&
    (capabilities.secureDesktop===true||capabilities.viewportFailureReason==='no_interactive_session');
}

// One bounded record per connected Host/desktop, never a timeout-based success.
// The exact Host completion and its following valid capability update are two
// separate barriers. Neither a stopped socket nor cached readiness is enough.
export class SessionStop {
  constructor(relay,{readyMs=45000,pollMs=1000}={}){
    this.relay=relay;this.readyMs=readyMs;this.pollMs=pollMs;this.pending=new Map();
  }
  current(record){
    const r=this.relay;
    return r.hosts.get(record.desktopId)===record.host&&
      r.store.authenticateHost(record.host.tokenHash)?.id===record.host.identity.id;
  }
  begin(session){
    if(session.mode!=='stream'||!session.hostDispatched||session.stopProtocol!==sessionStopProtocol||this.relay.hosts.get(session.desktopId)!==session.host)return null;
    const now=Date.now(),record={desktopId:session.desktopId,sessionId:session.id,host:session.host,
      requestId:randomUUID(),phase:'pending',checkedAt:now,deadline:now+this.readyMs,nextPoll:now+this.pollMs,completionVerified:false,workerQuiescent:false};
    // Admission already excludes a pending predecessor. Do not overwrite its
    // correlation if an unexpected duplicate stop reaches this method.
    if(this.pending.has(record.desktopId))return null;
    this.pending.set(record.desktopId,record);return record;
  }
  fields(record){return record?{sessionStopProtocol,stopRequestId:record.requestId}:{};}
  guard(desktopId,mode='stream'){if(this.blocked(desktopId,mode))throw new HttpError(409,'session_cleanup_pending');}
  blocked(desktopId,mode='stream'){
    const record=this.pending.get(desktopId);
    // Once the old capture process is proved quiescent, an explicitly
    // confirmed console return must remain available for unlocking the
    // Windows session that display recovery may require. It never clears
    // the display barrier or counts as a successful profile restoration.
    return !!record&&!(mode==='console'&&record.workerQuiescent);
  }
  view(desktopId){
    const record=this.pending.get(desktopId);
    return record?{pending:true,phase:record.phase,workerQuiescent:record.workerQuiescent,checkedAt:new Date(record.checkedAt).toISOString()}:{pending:false};
  }
  result(host,value){
    const record=this.pending.get(host.identity.desktop_id);
    if(!record||record.host!==host||!this.current(record)||
       !value||typeof value!=='object'||Array.isArray(value)||
       Object.keys(value).length!==resultFields.length||resultFields.some(k=>!Object.hasOwn(value,k))||
       value.type!=='host.session.stopped'||value.sessionStopProtocol!==sessionStopProtocol||
       value.sessionId!==record.sessionId||value.stopRequestId!==record.requestId||!uuid.test(value.stopRequestId)||
       ['workerQuiescent','profileRequired','profileRestored'].some(k=>typeof value[k]!=='boolean')||
       (record.profileRequired!==undefined&&record.profileRequired!==value.profileRequired)||
       (!value.workerQuiescent&&value.profileRestored))return false;
    record.checkedAt=Date.now();
    record.profileRequired=value.profileRequired;
    record.workerQuiescent=value.workerQuiescent;
    record.completionVerified=value.workerQuiescent&&value.profileRestored;
    record.phase=record.completionVerified?'awaiting_capabilities':'recovery_required';
    if(record.completionVerified)host.capabilitiesPending=true;
    return true;
  }
  capabilities(host){
    const record=this.pending.get(host.identity.desktop_id);
    if(record&&record.host===host&&record.completionVerified&&this.current(record)&&
       host.capabilities?.service===true&&host.capabilities.sessionStopProtocol===sessionStopProtocol&&
       // A completed profile restore can precede Windows' fresh owned-output
       // observation. Releasing on any service heartbeat exposes a transient
       // zero-output snapshot to the next count/mode preflight. Preserve the
       // barrier until normal display admission actually becomes available.
       // With no signed-in Windows user (signed out inside the stream) there
       // is no user display to come back, and that readiness needs one: the
       // barrier would hold every stream, the sign-in screen included, until
       // the Host restarts. The verified restore still counts.
       // Locked (Win+L inside the stream) is the same: the session stays but
       // its display cannot come back until unlocked, which the console is for
       // (it would answer console_cleanup_pending until a restart).
       // A secure desktop of the unlocked session (a UAC prompt) is the third
       // state of that kind: Windows refuses every display change from the
       // session until the prompt is answered, the Host's display cannot come
       // back before that, and the console is where it is answered. Otherwise
       // a stream that ends under the prompt holds the barrier, the Console
       // answers console_cleanup_pending, and only a restart ends it.
       // Nothing else is admitted: the Host's
       // own capabilities still refuse a stream and a display change.
       (!record.profileRequired||host.capabilities.interactiveSession===false||
        ['windows_session_locked','windows_login_required'].includes(host.capabilities.consoleRecoveryReason)||
        secureDesktopShowing(host.capabilities)||this.relay.displaySupported(host))){
      this.pending.delete(record.desktopId);return true;
    }
    return false;
  }
  hostLost(host){for(const[id,record]of this.pending)if(record.host===host)this.pending.delete(id);}
  tick(){
    const now=Date.now();
    for(const record of this.pending.values()){
      if(!this.current(record))continue; // Ordinary Host revocation removes it.
      if(now>=record.deadline&&record.phase!=='awaiting_capabilities')record.phase='unknown';
      if(record.completionVerified||now<record.nextPoll)continue;
      record.nextPoll=now+(now>=record.deadline?Math.max(5000,this.pollMs):this.pollMs);
      this.relay.send(record.host.ws,{type:'host.session.stop.status',sessionId:record.sessionId,...this.fields(record)});
    }
  }
}

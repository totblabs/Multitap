import {HttpError} from './errors.mjs';
import {udpMediaCapability,udpMediaAllowed,udpKeyMessage} from './udp-media.mjs';
import {udpDirectProtocol,localCandidate} from './udp-direct.mjs';

export const udpTracksProtocol='udp-tracks-v1';
export const udpTracksCapability=value=>value?.udpTracksHostProtocol===udpTracksProtocol;
const integer=(n,low,high)=>Number.isSafeInteger(n)&&n>=low&&n<=high;
const shape=(value,required,optional=[])=>value&&typeof value==='object'&&!Array.isArray(value)&&required.every(key=>Object.hasOwn(value,key))&&Object.keys(value).every(key=>required.includes(key)||optional.includes(key));
const scopeFields=['sessionId','track','topologyEpoch','epoch','generation'];

export function requestedUdpTracks(mode,protocol,tracks){
  if(protocol===undefined)return false;
  if(mode!=='stream'||protocol!==udpTracksProtocol||!tracks)throw new HttpError(400,'invalid_udp_tracks_protocol');
  return true;
}
export function negotiateUdpTracks(requested,mediaRequested,host,tracks,udp,desktopId){
  return requested===true&&mediaRequested===true&&!!tracks&&udp?.available===true&&udpMediaAllowed(udp,desktopId)&&udpMediaCapability(host?.capabilities)&&udpTracksCapability(host?.capabilities);
}
export function udpTrackScope(session,value){
  if(!session.udpTracks||!session.udpGranted||!session.udpTrackScopes||value.sessionId!==session.id)return null;
  const scope=session.udpTrackScopes.get(value.track);
  return scope&&value.topologyEpoch===scope.topologyEpoch&&value.epoch===scope.epoch&&value.generation===scope.generation?scope:null;
}
export function udpTrackKey(session,value){
  if(!shape(value,['type','protocol',...scopeFields,'publicKey'])||value.type!=='udp.tracks.key'||!udpTrackScope(session,value))return null;
  return udpKeyMessage({type:'udp.key',protocol:value.protocol,publicKey:value.publicKey});
}
export function udpTrackLocal(session,value){
  return shape(value,['type','protocol',...scopeFields,'address','port'],['candidateType'])&&value.type==='udp.tracks.local'&&value.protocol===udpDirectProtocol&&udpTrackScope(session,value)&&localCandidate(value)?value:null;
}
export function udpTrackSignature(info){
  if(!info||!integer(info.topologyEpoch,1,0xffffffff)||!Array.isArray(info.tracks)||info.tracks.length<1||info.tracks.length>2||info.tracks.some(row=>!['0','1'].includes(row.id)||!integer(row.epoch,1,65535)))return null;
  return JSON.stringify([info.topologyEpoch,...info.tracks.map(row=>[row.id,row.epoch])]);
}

// UDP carries the frames past the broker. Only reports of new, actually
// accepted UDP arrivals keep an established output's watchdog alive. They
// never create its first IDR, readiness or canonical WSS PTS watermark.
export function udpTrackFeedback(session,value,now=Date.now()){
  const state=session.trackState;
  if(!session.udpTracks||!session.udpGranted||!state?.ready||!shape(value,['type','protocol','topologyEpoch','generation','tracks'])||value.type!=='udp.tracks.feedback'||value.protocol!==udpTracksProtocol||value.topologyEpoch!==state.info.topologyEpoch||value.generation!==session.udpTrackGeneration||!Array.isArray(value.tracks)||value.tracks.length<1||value.tracks.length>2)return false;
  const seen=new Set(),rows=[];
  for(const row of value.tracks){
    if(!shape(row,['track','epoch','pts','frames','bytes'])||seen.has(row.track)||!integer(row.pts,0,Number.MAX_SAFE_INTEGER)||!integer(row.frames,1,240)||!integer(row.bytes,1,64*1024*1024))return false;
    seen.add(row.track);const scope=session.udpTrackScopes?.get(row.track),video=state.video.get(row.track);
    if(!scope||!video?.key||scope.topologyEpoch!==value.topologyEpoch||scope.generation!==value.generation||scope.epoch!==row.epoch||video.epoch!==row.epoch)return false;
    rows.push({row,video});
  }
  const observed=session.udpTrackFeedback??=new Map();
  for(const {row,video} of rows){
    const previous=Math.max(video.pts??-1,observed.get(row.track)??-1);
    if(row.pts<=previous)continue;
    observed.set(row.track,row.pts);video.lastAt=now;
  }
  return true;
}

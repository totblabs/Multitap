import { safeEncoderInfo } from './encoder-diagnostics.mjs';
import { HttpError } from './errors.mjs';
import { randomUUID } from 'node:crypto';
import { isDeepStrictEqual } from 'node:util';
import { aggregatePixels, videoGeometry, videoPlacement } from './video-geometry.mjs';
import { streamSizeProtocolV2, withinDecoderLimits, validDecoderLimits } from './stream-size.mjs';

export const trackProtocol = 'tracks-v1';
export const trackTransport = 'media-tracks-v1';
const maxPacket = 4 * 1024 * 1024, maxPixels = 8294400, u32 = 0xffffffff;
const integer = (n, min, max) => Number.isSafeInteger(n) && n >= min && n <= max;
const even = (n, min, max) => integer(n, min, max) && n % 2 === 0;
const shape = (v, required, optional = []) => !!v && typeof v === 'object' && !Array.isArray(v) &&
  required.every(k => Object.hasOwn(v, k)) && Object.keys(v).every(k => required.includes(k) || optional.includes(k));
const id = v => v === '0' || v === '1';
const overlaps = (a, b) => a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
const rank = t => t?.id === '0' ? 0 : t?.id === '1' ? 1 : 2;
const ordered = tracks => [...tracks].sort((a, b) => rank(a) - rank(b));
const same = (a, b) => { try { return isDeepStrictEqual(a, b); } catch { return false; } };

export function trackRequests(value, sizeProtocol = null) {
  if (!Array.isArray(value) || value.length < 1 || value.length > 2) return null;
  const capacity = sizeProtocol === streamSizeProtocolV2;
  if (value.some(t => !shape(t, ['id','width','height','x','y']) || !id(t.id) ||
      !(capacity ? videoGeometry(t.width,t.height,320,180) : even(t.width,320,3840) && even(t.height,180,2160)) ||
      !(capacity ? videoPlacement(t.width,t.height,t.x,t.y) :
        integer(t.x,-32768,32768-t.width) && integer(t.y,-32768,32768-t.height)))) return null;
  const pixels = aggregatePixels(value);
  if (new Set(value.map(t => t.id)).size !== value.length || pixels === null || (!capacity && pixels > maxPixels) ||
      (value.length === 2 && overlaps(value[0],value[1]))) return null;
  return Object.freeze(ordered(value).map(t => Object.freeze({id:t.id,width:t.width,height:t.height,x:t.x,y:t.y})));
}
export function requestedTracks(mode, protocol, value, options, sizeProtocol = null) {
  if (protocol === undefined && value === undefined) return null;
  if (mode !== 'stream' || protocol !== trackProtocol) throw new HttpError(400, 'invalid_media_protocol');
  const tracks = trackRequests(value, sizeProtocol);
  if (!tracks || (tracks.length === 2 ? options.monitor !== 'all' : options.monitor !== tracks[0].id) ||
      tracks.some(t => t.width > options.maxWidth || t.height > options.maxHeight)) throw new HttpError(400, 'invalid_track_request');
  return tracks;
}
export function tracksSupported(host, now, heartbeatMs) {
  const c = host?.capabilities;
  return !!c && !host.capabilitiesPending && now-(host.capabilitiesAt??0) <= heartbeatMs*3 &&
    c.videoTrackProtocol === trackProtocol && c.trackViewportProvisioning === true && c.service && c.interactiveSession &&
    c.streaming && c.virtualDisplayReady && [1,2].includes(c.monitors?.length) && c.monitors.every(m => id(m?.id));
}
// opus (audio-opus-tracks-v1): an audio payload is a u16 sequence + an Opus
// packet of any length instead of whole 4-byte PCM frames.
export function parseTrackPacket(data, opus = false) {
  if (!Buffer.isBuffer(data) || data.length <= 28 || data.length > maxPacket ||
      data[0]!==0x54 || data[1]!==0x56 || data[2]!==0x54 || data[3]!==0x31 ||
      data[7] || data.readUInt32LE(24) !== data.length-28) return null;
  const packet = {kind:data[4],flags:data[5],track:String(data[6]),topology:data.readUInt32LE(8),epoch:data.readUInt32LE(12),pts:Number(data.readBigUInt64LE(16)),payload:data.subarray(28)};
  if (!packet.topology || !Number.isSafeInteger(packet.pts) ||
      (packet.kind === 1 ? (!id(packet.track) || !packet.epoch || (packet.flags&~1)) :
       packet.kind === 2 ? (data[6] !== 255 || packet.epoch || packet.flags || (!opus && packet.payload.length%4)) : true)) return null;
  return packet;
}
function initialIdr(bytes) {
  let mask=0, first=true;
  for(let n=0;n+3<bytes.length;) {
    let prefix=0;
    if(bytes[n]===0 && bytes[n+1]===0 && bytes[n+2]===1) prefix=3;
    else if(n+4<bytes.length && bytes[n]===0 && bytes[n+1]===0 && bytes[n+2]===0 && bytes[n+3]===1) prefix=4;
    if(!prefix){if(first && bytes[n]!==0)return false;++n;continue;}
    first=false;const header=bytes[n+prefix];if(header&128)return false;
    const type=header&31;if(type===5)mask|=1;if(type===7)mask|=2;if(type===8)mask|=4;
    n+=prefix+1;
  }
  return mask===7;
}

export class TrackStream {
  // audioOpus: the session was granted audio-opus-tracks-v1 (the Host may then state "opus").
  constructor(sessionId, requests, options, { now=Date.now, prepareMs=90000, readyMs=15000, mediaMs=10000, allowRestored=false, sizeProtocol=null, decoderLimits=null, audioOpus=false } = {}) {
    Object.assign(this,{sessionId,requests,options,now,prepareMs,readyMs,mediaMs,sizeProtocol,decoderLimits});this.allowRestored=allowRestored===true;this.audioOpus=audioOpus===true;
    this.pending={topologyEpoch:1,epochs:new Map(requests.map(t=>[t.id,1])),tracks:requests,fps:options.fps};
    this.deadline=now()+prepareMs;this.info=null;this.video=new Map();this.audioPts=null;this.clientReady=false;
  }
  get ready(){return !!this.info && !this.pending && this.clientReady && [...this.video.values()].every(t=>t.key);}
  get expired(){return this.deadline!==null && this.now()>this.deadline;}
  get stalled(){return this.ready && [...this.video.values()].some(t=>this.now()-t.lastAt>this.mediaMs);}
  get preparing(){return this.pending!==null || !this.ready;}
  // tracks-resume-v1: the client showed every output and nothing is changing
  // (a single output's streamReady). Stays true while restart() waits for IDRs.
  get established(){return !!this.info && !this.pending && this.clientReady;}
  get awaitingKeys(){return this.established && [...this.video.values()].some(t=>!t.key);}
  // A resumed session: no video (nor input or audio) until a fresh IDR on
  // every output, which must come within readyMs.
  restart(){for(const t of this.video.values())t.key=false;this.deadline=this.now()+this.readyMs;}
  start(){return {mediaProtocol:trackProtocol,topologyEpoch:1,tracks:this.requests.map(t=>({...t,epoch:1}))};}
  acceptInfo(value) {
    if(!value || value.sessionId!==this.sessionId)return 'stale';
    if(this.info && integer(value.topologyEpoch,1,u32) && value.topologyEpoch<this.info.topologyEpoch)return 'stale';
    const expected=this.pending;
    if(!expected) return Array.isArray(value.tracks) && same({...value,tracks:ordered(value.tracks)},this.info)?'duplicate':'invalid';
    if(value.topologyEpoch!==expected.topologyEpoch || value.resizeId!==expected.resizeId || value.requestId!==expected.requestId)return 'stale';
    if(!shape(value,['type','sessionId','codec','topologyEpoch','fpsTarget','bitrateTarget','tracks','audio'],['resizeId','requestId']) ||
       value.type!=='stream.tracks.info' || value.codec!=='h264' || value.fpsTarget!==expected.fps ||
       !integer(value.bitrateTarget,500000,this.options.bitrate) || !Array.isArray(value.tracks) || value.tracks.length!==expected.tracks.length ||
       !shape(value.audio,['sampleRate','channels','format','available']) || value.audio.sampleRate!==48000 || value.audio.channels!==2 ||
       !(value.audio.format==='s16le' || (this.audioOpus && value.audio.format==='opus')) || typeof value.audio.available!=='boolean' || (value.audio.available && !this.options.audio))return 'invalid';
    const tracks=ordered(value.tracks);let offsetX=null,offsetY=null,primary=0,bits=0;
    for(let n=0;n<tracks.length;n++) {
      const t=tracks[n],r=expected.tracks[n];
      if(!shape(t,['id','epoch','width','height','sourceWidth','sourceHeight','left','top','primary','bitrateTarget']) || t.id!==r.id ||
         t.epoch!==expected.epochs.get(t.id) ||
         !(this.sizeProtocol === streamSizeProtocolV2
           ? videoGeometry(t.width,t.height) && t.width<=r.width && t.height<=r.height && withinDecoderLimits(t,expected.decoderLimits??this.decoderLimits,expected.fps)
           : t.width===r.width && t.height===r.height) || t.sourceWidth!==r.width || t.sourceHeight!==r.height ||
         !(this.sizeProtocol === streamSizeProtocolV2 ? videoPlacement(t.sourceWidth,t.sourceHeight,t.left,t.top) :
           integer(t.left,-32768,32768-t.sourceWidth) && integer(t.top,-32768,32768-t.sourceHeight)) || typeof t.primary!=='boolean' ||
         !integer(t.bitrateTarget,500000,this.options.bitrate))return 'invalid';
      if(n===0){offsetX=t.left-r.x;offsetY=t.top-r.y;}
      if(t.left-r.x!==offsetX || t.top-r.y!==offsetY)return 'invalid';
      primary+=Number(t.primary);bits+=t.bitrateTarget;
    }
    if(primary>1 || bits!==value.bitrateTarget)return 'invalid';
    if(this.allowRestored&&(tracks[0].id!=='0'||!tracks[0].primary||tracks[0].left!==0||tracks[0].top!==0))return 'invalid';
    this.info={...value,tracks:tracks.map(t=>({...t})),audio:{...value.audio}};
    this.requests=expected.tracks;this.options={...this.options,fps:expected.fps};if(expected.decoderLimits)this.decoderLimits=expected.decoderLimits;this.pending=null;
    this.video=new Map(tracks.map(t=>[t.id,{epoch:t.epoch,key:false,pts:null}]));
    this.audioPts=null;this.clientReady=false;this.deadline=this.now()+this.readyMs;return 'accepted';
  }
  media(data) {
    const p=parseTrackPacket(data,this.info?.audio.format==='opus');if(!p)return 'invalid';
    if(!this.info || this.pending)return 'waiting';
    if(p.topology<this.info.topologyEpoch)return 'stale';
    if(p.topology!==this.info.topologyEpoch)return 'invalid';
    if(p.kind===2) {
      if(!this.info.audio.available)return 'invalid';
      if(!this.ready)return 'waiting';
      if(this.audioPts!==null && p.pts<=this.audioPts)return 'invalid';
      this.audioPts=p.pts;return 'forward';
    }
    const state=this.video.get(p.track);if(!state)return 'invalid';
    if(p.epoch<state.epoch)return 'stale';
    if(p.epoch!==state.epoch || (state.pts!==null && p.pts<=state.pts))return 'invalid';
    if(!state.key){if(!(p.flags&1))return 'waiting';if(!initialIdr(p.payload))return 'invalid';}
    state.key=true;state.pts=p.pts;state.lastAt=this.now();
    // The last IDR a restart() waited for.
    if(this.established && this.deadline!==null && [...this.video.values()].every(t=>t.key))this.deadline=null;
    return 'forward';
  }
  acknowledge(value) {
    if(!shape(value,['type','topologyEpoch','tracks']) || value.type!=='stream.tracks.ready' ||
       !integer(value.topologyEpoch,1,u32) || !Array.isArray(value.tracks) || !this.info)return 'invalid';
    if(value.topologyEpoch<(this.pending?.topologyEpoch??this.info.topologyEpoch))return 'stale';
    if(this.pending || value.topologyEpoch!==this.info.topologyEpoch || value.tracks.length!==this.video.size)return 'invalid';
    const tracks=ordered(value.tracks);
    if(tracks.some((t,n)=>!shape(t,['id','epoch']) || t.id!==this.info.tracks[n].id || t.epoch!==this.video.get(t.id)?.epoch) ||
       [...this.video.values()].some(t=>!t.key))return 'invalid';
    this.clientReady=true;this.deadline=null;return 'accepted';
  }
  // validRelative: the session granted 게임 마우스 (relative-mouse-tracks-v1):
  // movement, and buttons and wheel at the current cursor (no track, no position).
  input(value, validLegacy, validRelative = null) {
    if(!this.info || this.pending || !this.ready)return 'waiting';
    if(!value)return 'invalid';
    if(validRelative && value.type==='input' && (value.event==='moveRelative' || (['button','wheel'].includes(value.event) && !Object.hasOwn(value,'track')))){
      if(!validRelative(value) || !integer(value.topologyEpoch,1,u32))return 'invalid';
      if(value.topologyEpoch<this.info.topologyEpoch)return 'stale';
      return value.topologyEpoch===this.info.topologyEpoch?'forward':'invalid';
    }
    const pointer=['move','button','wheel'].includes(value.event);
    const fields=value.event==='key'?['vk','scan','down','extended']:value.event==='move'?['x','y']:value.event==='button'?['x','y','button','down']:value.event==='wheel'?['x','y','delta']:null;
    if(!fields || !shape(value,['type','event','topologyEpoch',...fields,...(pointer?['track','epoch']:[])]) ||
       !integer(value.topologyEpoch,1,u32) || (pointer && !integer(value.epoch,1,u32)))return 'invalid';
    if(value.topologyEpoch<this.info.topologyEpoch)return 'stale';
    if(value.topologyEpoch!==this.info.topologyEpoch)return 'invalid';
    if(pointer){const t=this.video.get(value.track);if(!t)return 'invalid';if(value.epoch<t.epoch)return 'stale';if(value.epoch!==t.epoch)return 'invalid';}
    const base={...value};delete base.topologyEpoch;delete base.track;delete base.epoch;
    return validLegacy(base)?'forward':'invalid';
  }
  resize(value) {
    if(!shape(value,['type','requestId','topologyEpoch','tracks','fps'],this.sizeProtocol===streamSizeProtocolV2?['decoderLimits']:[]) ||
       (value.decoderLimits!==undefined && (this.sizeProtocol!==streamSizeProtocolV2 || !validDecoderLimits(value.decoderLimits,streamSizeProtocolV2))) || value.type!=='stream.tracks.resize' ||
       typeof value.requestId!=='string' || !/^[A-Za-z0-9_-]{1,64}$/.test(value.requestId) || !integer(value.topologyEpoch,1,u32) || ![30,60].includes(value.fps))return {reason:'invalid_track_request'};
    const tracks=trackRequests(value.tracks,this.sizeProtocol);
    if(!tracks || !this.info || tracks.length!==this.info.tracks.length || tracks.some((t,n)=>t.id!==this.info.tracks[n].id))return {reason:'invalid_track_request'};
    if(value.topologyEpoch!==this.info.topologyEpoch)return {reason:'stale_topology'};
    if(this.pending || !this.ready)return {reason:'resize_in_progress'};
    if(this.info.topologyEpoch===u32)return {reason:'epoch_exhausted'};
    const epochs=new Map();
    for(const t of tracks){const old=this.info.tracks.find(v=>v.id===t.id),changed=t.width!==(this.sizeProtocol===streamSizeProtocolV2?old.sourceWidth:old.width) || t.height!==(this.sizeProtocol===streamSizeProtocolV2?old.sourceHeight:old.height) || value.fps!==this.info.fpsTarget;
      if(changed && old.epoch===u32)return {reason:'epoch_exhausted'};epochs.set(t.id,old.epoch+Number(changed));}
    this.pending={topologyEpoch:this.info.topologyEpoch+1,epochs,tracks,fps:value.fps,requestId:value.requestId,resizeId:randomUUID(),...(value.decoderLimits?{decoderLimits:{...value.decoderLimits}}:{})};
    this.clientReady=false;this.deadline=this.now()+this.prepareMs;
    return {topologyEpoch:this.pending.topologyEpoch,requestId:value.requestId,resizeId:this.pending.resizeId,
      tracks:tracks.map(t=>({...t,epoch:epochs.get(t.id)})),fps:value.fps,timeoutMs:this.prepareMs,...(this.pending.decoderLimits?{decoderLimits:this.pending.decoderLimits}:{})};
  }
  failedResize(value) {
    return this.pending?.resizeId && shape(value,['type','sessionId','requestId','resizeId','topologyEpoch','reason']) &&
      value.sessionId===this.sessionId && value.requestId===this.pending.requestId && value.resizeId===this.pending.resizeId &&
      value.topologyEpoch===this.pending.topologyEpoch;
  }
  restoredResize(value) {
    if(!this.allowRestored)return 'invalid';
    const pending=this.pending;
    if(!pending?.resizeId||!this.info||value?.sessionId!==this.sessionId||value.requestId!==pending.requestId||value.resizeId!==pending.resizeId||value.topologyEpoch!==pending.topologyEpoch)return 'stale';
    if(this.expired||!shape(value,['type','sessionId','requestId','resizeId','topologyEpoch','fps','tracks','reason'])||value.type!=='stream.tracks.resize.restored'||value.fps!==this.options.fps||
       !['unsupported_display_mode','display_change_failed','virtual_display_required','monitor_not_available','resize_failed'].includes(value.reason)||!Array.isArray(value.tracks)||value.tracks.length!==this.requests.length)return 'invalid';
    const tracks=ordered(value.tracks);
    if(tracks.some((t,n)=>!shape(t,['id','width','height','x','y','epoch'])||t.id!==this.requests[n].id||t.width!==this.requests[n].width||t.height!==this.requests[n].height||t.x!==this.requests[n].x||t.y!==this.requests[n].y||t.epoch!==this.info.tracks[n].epoch))return 'invalid';
    const restored={...value,tracks:tracks.map(t=>({...t}))};
    if(pending.restored)return same(pending.restored,restored)?'duplicate':'invalid';
    // Retain the already authorized NEW topology/request/resize IDs. Only the
    // previously applied dimensions/FPS/media epochs may be restored, and a new
    // metadata + fresh IDRs + actual client-ready ACK are still required.
    this.pending={...pending,tracks:this.requests,epochs:new Map(this.info.tracks.map(t=>[t.id,t.epoch])),fps:this.options.fps,decoderLimits:this.decoderLimits,restored};
    this.clientReady=false;return 'accepted';
  }
  stats(value) {
    if(!value || value.sessionId!==this.sessionId || !this.info || this.pending || value.topologyEpoch!==this.info.topologyEpoch)return null;
    if(!shape(value,['type','sessionId','topologyEpoch','measuredMs','tracks']) || !integer(value.measuredMs,1,10000) ||
       !Array.isArray(value.tracks) || value.tracks.length!==this.info.tracks.length)return false;
    const tracks=ordered(value.tracks);
    if(tracks.some((t,n)=>!shape(t,['id','epoch','capturedFps','encodedFps','bitrateKbps'],['encoderInfo']) ||
        t.id!==this.info.tracks[n].id || t.epoch!==this.info.tracks[n].epoch ||
        ['capturedFps','encodedFps','bitrateKbps'].some(k=>!Number.isFinite(t[k]) || t[k]<0 || t[k]>(k==='bitrateKbps'?1000000:100000))))return false;
    return {...value,tracks:tracks.map(t=>{const {encoderInfo,...base}=t;const info=safeEncoderInfo(encoderInfo);return info?{...base,encoderInfo:info}:base;})};
  }
}

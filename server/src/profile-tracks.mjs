import { HttpError } from './errors.mjs';
import { tracksSupported } from './track-media.mjs';
import { streamConsoleProfileProtocol } from './stream-console-profile.mjs';
import { videoGeometry } from './video-geometry.mjs';
import { streamSizeCapability, streamSizeProtocolV2 } from './stream-size.mjs';
export const profileTracksProtocol='group-v1';
export function requestedProfileTracks(mode,value,tracks){
  if(value===undefined)return false;
  if(mode!=='stream'||value!==profileTracksProtocol||!tracks)throw new HttpError(400,'invalid_media_protocol');
  return true;
}
export function profileTracksCapability(c){
  return !!c&&c.streamProfileProtocol===streamConsoleProfileProtocol&&c.streamProfileTracksProtocol===profileTracksProtocol&&
    c.videoTrackProtocol==='tracks-v1'&&c.trackViewportProvisioning===true&&c.viewportProvisioning===true&&c.dynamicResolution===true&&c.resizeProtocol==='epoch-v1'&&
    c.service===true&&c.interactiveSession===true&&c.streaming===true&&c.virtualDisplayReady===true&&Array.isArray(c.monitors)&&[1,2].includes(c.monitors.length)&&c.monitors.some(m=>m?.id==='0')&&
    new Set(c.monitors.map(m=>m?.id)).size===c.monitors.length&&c.monitors.every(m=>['0','1'].includes(m?.id)&&
      (streamSizeCapability(c,streamSizeProtocolV2) ? videoGeometry(m.width,m.height,320,180) : Number.isSafeInteger(m.width)&&m.width>=320&&m.width<=3840&&!(m.width%2)&&Number.isSafeInteger(m.height)&&m.height>=180&&m.height<=2160&&!(m.height%2)));
}
export function profileTracksSupported(host,now,heartbeatMs){return tracksSupported(host,now,heartbeatMs)&&profileTracksCapability(host.capabilities);}

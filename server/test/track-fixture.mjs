// Protocol-only synthetic NAL markers. These are not decodable H.264 or a
// Windows capture/decoder acceptance fixture; actual MFT vectors are separate.
export const requests = [
  { id:'0',width:640,height:360,x:-640,y:-100 },
  { id:'1',width:800,height:600,x:0,y:0 },
];
export const options = { maxWidth:1280,maxHeight:720,fps:30,bitrate:8000000,audio:true,monitor:'all' };
export const key = Buffer.from([0,0,0,1,0x67,1,0,0,1,0x68,1,0,0,1,0x65,1]);
export const delta = Buffer.from([0,0,1,0x41,1]);
export function packet({ track='0',topology=1,epoch=1,pts=0,kind=1,payload=key,flags=1 }={}) {
  const data=Buffer.alloc(28+payload.length);
  data.write('TVT1');data[4]=kind;data[5]=flags;data[6]=Number(track);
  data.writeUInt32LE(topology,8);data.writeUInt32LE(epoch,12);
  data.writeBigUInt64LE(BigInt(pts),16);data.writeUInt32LE(payload.length,24);payload.copy(data,28);
  return data;
}
export const sound = (topology=1,pts=0) => packet({kind:2,track:255,epoch:0,flags:0,topology,pts,payload:Buffer.from([0,1,0,2,0,3,0,4])});
export function info(sessionId='session-fixture', command={topologyEpoch:1,tracks:requests.map(t=>({...t,epoch:1})),fps:30}) {
  return {
    type:'stream.tracks.info',sessionId,codec:'h264',topologyEpoch:command.topologyEpoch,
    fpsTarget:command.fps,bitrateTarget:8000000,
    tracks:command.tracks.map(t=>({id:t.id,epoch:t.epoch,width:t.width,height:t.height,sourceWidth:t.width,
      sourceHeight:t.height,left:t.x+640,top:t.y+100,primary:t.id==='0',bitrateTarget:4000000})),
    audio:{sampleRate:48000,channels:2,format:'s16le',available:true},
    ...(command.resizeId?{resizeId:command.resizeId,requestId:command.requestId}:{}),
  };
}
export const acknowledgement = (metadata=info()) => ({type:'stream.tracks.ready',topologyEpoch:metadata.topologyEpoch,tracks:metadata.tracks.map(({id,epoch})=>({id,epoch}))});
export const pointer = (changes={}) => ({type:'input',event:'move',track:'1',epoch:1,topologyEpoch:1,x:.25,y:.75,...changes});
export const keyboard = (changes={}) => ({type:'input',event:'key',topologyEpoch:1,vk:91,scan:0,down:true,extended:false,...changes});
export const resize = (changes={}) => ({type:'stream.tracks.resize',requestId:'resize-fixture',topologyEpoch:1,fps:30,
  tracks:requests.map(t=>t.id==='1'?{...t,width:960,height:540}:t),...changes});

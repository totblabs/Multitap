import { test } from 'node:test';
import assert from 'node:assert/strict';
import { aggregatePixels, videoGeometry } from '../src/video-geometry.mjs';
import { streamSizeProtocolV2 as v2, ownedOutputSize, encodedSize, validDecoderLimits,
  requestedStreamSize, negotiateStreamSize, withinDecoderLimits } from '../src/stream-size.mjs';
import { trackRequests, TrackStream } from '../src/track-media.mjs';

// Device-independent protocol fixtures; these do not assert hardware support.
const decoder={maxWidth:8192,maxHeight:8192,maxPixels:8192*8192,maxPixelRate:0};
test('v2 retains requested 4K+, portrait and ultrawide geometry without a product pixel ceiling',()=>{
  for(const [width,height] of [[3840,2160],[5120,2880],[5120,1440],[2880,5120],[4320,7680]]) {
    assert.equal(ownedOutputSize(width,height,v2),true);
    assert.equal(encodedSize(width,height,v2),true);
  }
  for(const [w,h] of [[NaN,2],[Infinity,2],[5120.5,2880],[-2,180],[5121,2880],[0x7ffffffe,0x7ffffffe]])
    assert.equal(videoGeometry(w,h),false);
});
test('v2 preserves actual decoder declarations and requires three-peer negotiation',()=>{
  assert.equal(validDecoderLimits(decoder,v2),true);
  const asked=requestedStreamSize('stream',v2,decoder,true,null);
  assert.deepEqual(asked,decoder);
  assert.equal(negotiateStreamSize(asked,{capabilities:{streamSizeProtocol:v2,viewportEncoderLimitsProtocol:'separate-v1'}},null,{width:5120,height:2880},{maxWidth:8192,maxHeight:8192},v2),asked);
  assert.throws(()=>negotiateStreamSize(asked,{capabilities:{streamSizeProtocol:'stream-size-v1',viewportEncoderLimitsProtocol:'separate-v1'}},null,null,null,v2),e=>e.code==='stream_size_unavailable');
  for(const delta of [{maxWidth:Infinity},{maxPixels:Number.MAX_SAFE_INTEGER+1},{maxPixelRate:-1},{extra:1}])
    assert.equal(validDecoderLimits({...decoder,...delta},v2),false);
});
test('two 4K outputs have no fixed aggregate pixel ceiling and preserve nonoverlap and identity checks',()=>{
  const pair=[{id:'0',width:3840,height:2160,x:0,y:0},{id:'1',width:3840,height:2160,x:3840,y:0}];
  assert.equal(aggregatePixels(pair),2*3840*2160);
  assert.deepEqual(trackRequests(pair,v2),pair);
  assert.equal(trackRequests(pair),null,'old peer retains its actual old contract');
  assert.equal(trackRequests([pair[0],{...pair[1],x:3800}],v2),null);
  assert.equal(trackRequests([pair[0],{...pair[1],id:'0'}],v2),null);
});
test('v2 track source pixels stay native while encoded pictures fit the declared decoder',()=>{
  const pair=[{id:'0',width:5120,height:2880,x:0,y:0},{id:'1',width:5120,height:2880,x:5120,y:0}];
  const limits={maxWidth:2560,maxHeight:1440,maxPixels:2560*1440,maxPixelRate:0};
  const state=new TrackStream('test-session',pair,{fps:60,bitrate:20000000,audio:false},{sizeProtocol:v2,decoderLimits:limits});
  const info={type:'stream.tracks.info',sessionId:'test-session',codec:'h264',topologyEpoch:1,fpsTarget:60,bitrateTarget:20000000,
    audio:{sampleRate:48000,channels:2,format:'s16le',available:false},
    tracks:pair.map((r,n)=>({id:r.id,epoch:1,width:2560,height:1440,sourceWidth:r.width,sourceHeight:r.height,left:r.x,top:r.y,primary:n===0,bitrateTarget:10000000}))};
  assert.equal(state.acceptInfo(info),'accepted');
  assert.equal(state.info.tracks[0].sourceWidth,5120);
  assert.equal(withinDecoderLimits({width:5120,height:2880},limits,60),false);
});

test('scaled v2 track resize epochs compare physical source, including position-only and shrinking to old encoded size',()=>{
  function ready(){
    const request=[{id:'0',width:3840,height:2160,x:0,y:0}];
    const limits={maxWidth:3840,maxHeight:2160,maxPixels:3840*2160,maxPixelRate:0};
    const state=new TrackStream('source-epoch',request,{fps:60,bitrate:8000000,audio:false},{sizeProtocol:v2,decoderLimits:limits});
    assert.equal(state.acceptInfo({type:'stream.tracks.info',sessionId:'source-epoch',codec:'h264',topologyEpoch:1,fpsTarget:60,bitrateTarget:8000000,
      tracks:[{id:'0',epoch:1,width:1920,height:1080,sourceWidth:3840,sourceHeight:2160,left:0,top:0,primary:true,bitrateTarget:8000000}],
      audio:{sampleRate:48000,channels:2,format:'s16le',available:false}}),'accepted');
    const nal=Buffer.from([0,0,1,0x67,0,0,1,0x68,0,0,1,0x65]);
    const packet=Buffer.alloc(28+nal.length);packet.write('TVT1');packet[4]=1;packet[5]=1;packet.writeUInt32LE(1,8);packet.writeUInt32LE(1,12);packet.writeUInt32LE(nal.length,24);nal.copy(packet,28);
    assert.equal(state.media(packet),'forward');
    assert.equal(state.acknowledge({type:'stream.tracks.ready',topologyEpoch:1,tracks:[{id:'0',epoch:1}]}),'accepted');
    return state;
  }
  const moved=ready().resize({type:'stream.tracks.resize',requestId:'position',topologyEpoch:1,fps:60,tracks:[{id:'0',width:3840,height:2160,x:100,y:0}]});
  assert.equal(moved.tracks[0].epoch,1,'unchanged physical size preserves its media epoch');
  const smaller=ready().resize({type:'stream.tracks.resize',requestId:'smaller',topologyEpoch:1,fps:60,tracks:[{id:'0',width:1920,height:1080,x:0,y:0}]});
  assert.equal(smaller.tracks[0].epoch,2,'physical shrink is a change even when equal to old encoded size');
});

test('v2 placements use checked Windows LONG endpoints; old peers retain their geometry contract',()=>{
  for(const x of [40000,-40000,-0x80000000,0x7fffffff-320]) {
    const request=[{id:'0',width:320,height:180,x,y:0}];
    assert.ok(trackRequests(request,v2),'representable coordinate '+x);
    assert.equal(trackRequests(request),null,'not offered to an old peer');
  }
  for(const [x,y] of [[-0x80000001,0],[0x7fffffff-319,0],[0,0x7fffffff-179],[Number.MAX_SAFE_INTEGER,0]]) {
    assert.equal(trackRequests([{id:'0',width:320,height:180,x,y}],v2),null);
  }
});

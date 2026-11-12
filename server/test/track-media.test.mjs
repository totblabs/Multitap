import { test } from 'node:test';
import assert from 'node:assert/strict';
import { TrackStream,trackRequests,requestedTracks,tracksSupported,parseTrackPacket } from '../src/track-media.mjs';
import { requests,options,key,delta,packet,sound,info,acknowledgement,pointer,keyboard,resize } from './track-fixture.mjs';

function state(settings={}) {return new TrackStream('session-fixture',trackRequests(requests),options,settings);}
function prepared() {const s=state();assert.equal(s.acceptInfo(info()),'accepted');return s;}
function ready() {const s=prepared();for(const track of ['0','1'])assert.equal(s.media(packet({track})),'forward');assert.equal(s.acknowledge(acknowledgement()),'accepted');return s;}

test('Track request geometry is bounded, nonoverlapping, immutable and preserves gaps/negative placement',()=>{
  const result=trackRequests([...requests].reverse());assert.deepEqual(result,requests);assert.ok(Object.isFrozen(result)&&result.every(Object.isFrozen));
  assert.deepEqual(trackRequests([requests[1]]),[requests[1]]);
  const bad=[null,{},[],[...requests,requests[0]],[requests[0],requests[0]],
    requests.map(t=>({...t,id:0})),requests.map(t=>({...t,width:641})),requests.map(t=>({...t,x:0,y:0})),
    [{...requests[0],x:-32769}],[{...requests[0],x:32768}],
    [{id:'0',width:3840,height:2160,x:-3840,y:0},{id:'1',width:3840,height:2160,x:0,y:0}],
    [{...requests[0],device:'console'}],[{...requests[0],height:178}]];
  for(const value of bad)assert.equal(trackRequests(value),null);
  assert.equal(trackRequests([{id:'0',width:1920,height:1080,x:-1920,y:0},{id:'1',width:2560,height:1440,x:0,y:-1440}]).length,2);
});

test('Track negotiation is explicit, separate from RDP and enforces the initial requested viewport limit',()=>{
  assert.equal(requestedTracks('stream',undefined,undefined,options),null);
  assert.deepEqual(requestedTracks('stream','tracks-v1',requests,options),requests);
  for(const args of [['rdp','tracks-v1',requests,undefined],['stream','tracks-v2',requests,options],
    ['stream',undefined,requests,options],['stream','tracks-v1',requests,{...options,monitor:'0'}],
    ['stream','tracks-v1',[requests[1]],{...options,monitor:'0'}],['stream','tracks-v1',requests,{...options,maxWidth:640}]]){
    assert.throws(()=>requestedTracks(...args),e=>e.status===400);
  }
});

test('Track capability requires fresh explicit service/display/interactive support, never a version guess',()=>{
  const capabilities={service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,
    videoTrackProtocol:'tracks-v1',trackViewportProvisioning:true,monitors:[{id:'0'},{id:'1'}]};
  const host={capabilities,capabilitiesAt:1000};assert.equal(tracksSupported(host,1100,100),true);
  assert.equal(tracksSupported(host,1301,100),false);assert.equal(tracksSupported({...host,capabilitiesPending:true},1100,100),false);
  for(const field of ['service','interactiveSession','streaming','virtualDisplayReady','trackViewportProvisioning']){
    assert.equal(tracksSupported({...host,capabilities:{...capabilities,[field]:false}},1100,100),false);
  }
  for(const videoTrackProtocol of [undefined,'tracks-v2'])assert.equal(tracksSupported({...host,capabilities:{...capabilities,videoTrackProtocol}},1100,100),false);
  assert.equal(tracksSupported({...host,capabilities:{...capabilities,monitors:[{id:'console'}]}},1100,100),false);
});

test('TVT1 parser validates exact bytes, lengths, kinds, reserved bits, PCM alignment and safe PTS',()=>{
  const data=packet();assert.deepEqual(parseTrackPacket(data),{kind:1,flags:1,track:'0',topology:1,epoch:1,pts:0,payload:key});
  assert.equal(parseTrackPacket(packet({pts:Number.MAX_SAFE_INTEGER})).pts,Number.MAX_SAFE_INTEGER);
  const mutations=[[0,0xd4],[1,0xd6],[2,0xd4],[3,0xb1],[4,3],[5,2],[6,2],[7,1],[8,0],[12,0],[24,1]];
  for(const [offset,value] of mutations){const bad=Buffer.from(data);bad[offset]=value;assert.equal(parseTrackPacket(bad),null);}
  for(const value of [null,new Uint8Array(data),data.subarray(0,28),data.subarray(0,data.length-1),Buffer.concat([data,Buffer.from([0])]),Buffer.alloc(4*1024*1024+1)])assert.equal(parseTrackPacket(value),null);
  const unsafe=packet();unsafe.writeBigUInt64LE(9007199254740992n,16);assert.equal(parseTrackPacket(unsafe),null);
  assert.equal(parseTrackPacket(sound()).kind,2);
  for(const offset of [5,6,12]){const bad=sound();bad[offset]=1;assert.equal(parseTrackPacket(bad),null);}
  assert.equal(parseTrackPacket(packet({kind:2,track:255,epoch:0,flags:0,payload:Buffer.alloc(3)})),null);
});

test('Applied metadata proves exact requested geometry/layout and total bitrate before resetting decoder readiness',()=>{
  const s=state();assert.equal(s.acceptInfo({...info(),sessionId:'retired'}),'stale');
  const invalid=[{...info(),codec:'other'},{...info(),fpsTarget:60},{...info(),extra:'ignored?'},
    {...info(),tracks:info().tracks.map(t=>({...t,sourceWidth:t.width+2}))},
    {...info(),tracks:info().tracks.map(t=>({...t,left:t.id==='1'?t.left+1:t.left}))},
    {...info(),tracks:info().tracks.map(t=>({...t,primary:true}))},
    {...info(),bitrateTarget:7999999},{...info(),tracks:[info().tracks[0],info().tracks[0]]},
    {...info(),audio:{...info().audio,sampleRate:44100}}];
  for(const value of invalid){assert.equal(s.acceptInfo(value),'invalid');assert.equal(s.info,null);assert.ok(s.pending);}
  const metadata=info();metadata.tracks.reverse();assert.equal(s.acceptInfo(metadata),'accepted');assert.equal(s.ready,false);
  const reordered={audio:info().audio,tracks:[...info().tracks].reverse(),...Object.fromEntries(Object.entries(info()).filter(([k])=>!['audio','tracks'].includes(k)))};
  assert.equal(s.acceptInfo(reordered),'duplicate');assert.equal(s.video.size,2);
  assert.equal(s.acceptInfo({...info(),bitrateTarget:7000000}),'invalid');
  const silent=new TrackStream('session-fixture',requests,{...options,audio:false});assert.equal(silent.acceptInfo(info()),'invalid');
});

test('Both actual initial NAL markers and client decode acknowledgement gate input/shared PCM',()=>{
  const s=prepared();assert.equal(s.media(sound()),'waiting');assert.equal(s.media(packet({flags:0,payload:delta})),'waiting');
  assert.equal(s.input(pointer(),()=>true),'waiting');assert.equal(s.media(packet({track:'1'})),'forward');assert.equal(s.ready,false);
  assert.equal(s.media(packet({track:'0'})),'forward');assert.equal(s.ready,false);assert.equal(s.media(sound()),'waiting');
  assert.equal(s.acknowledge(acknowledgement()),'accepted');assert.equal(s.ready,true);assert.equal(s.media(sound()),'forward');
  assert.equal(s.media(sound()),'invalid');assert.equal(s.media(sound(1,1)),'forward');
  const lying=prepared();assert.equal(lying.media(packet({payload:delta})),'invalid');
  const early=prepared();assert.equal(early.acknowledge(acknowledgement()),'invalid');
});

test('Track PTS ordering is independent and old/future topology or output epochs cannot select another output',()=>{
  const s=ready();assert.equal(s.media(packet({track:'0',pts:100,flags:0,payload:delta})),'forward');
  assert.equal(s.media(packet({track:'1',pts:1,flags:0,payload:delta})),'forward');
  assert.equal(s.media(packet({track:'0',pts:99})),'invalid');assert.equal(s.media(packet({track:'1',epoch:2,pts:2})),'invalid');
  assert.equal(s.media(packet({topology:2,pts:101})),'invalid');
  const secondary=new TrackStream('session-fixture',[requests[1]],{...options,monitor:'1'});
  const metadata=info('session-fixture',{topologyEpoch:1,tracks:[{...requests[1],epoch:1}],fps:30});metadata.bitrateTarget=4000000;
  assert.equal(secondary.acceptInfo(metadata),'accepted');assert.equal(secondary.media(packet()),'invalid');
  assert.equal(secondary.media(packet({track:'1'})),'forward');assert.equal(secondary.acknowledge(acknowledgement(metadata)),'accepted');
});

test('Ready acknowledgements and inputs require exact current output identities and reject extra injection fields',()=>{
  const s=ready();for(const value of [{...acknowledgement(),topologyEpoch:'1'},
    {...acknowledgement(),tracks:[{id:{toString:0},epoch:1},null]},
    {...acknowledgement(),tracks:[{id:'0',epoch:1},{id:'0',epoch:1}]}])assert.equal(s.acknowledge(value),'invalid');
  let calls=0;const valid=base=>{++calls;assert.equal('track' in base,false);assert.equal('epoch' in base,false);assert.equal('topologyEpoch' in base,false);return true;};
  assert.equal(s.input(pointer(),valid),'forward');assert.equal(s.input(keyboard(),valid),'forward');assert.equal(calls,2);
  for(const value of [pointer({track:'console'}),pointer({epoch:'1'}),pointer({epoch:2}),pointer({topologyEpoch:'1'}),
    pointer({sessionId:'other'}),keyboard({track:'0',epoch:1}),{...keyboard(),secret:1}])assert.equal(s.input(value,valid),'invalid');
  assert.equal(calls,2);assert.equal(s.input(pointer(),()=>false),'invalid');
});

test('Viewport transition is correlated, blocks input/audio, advances only changed output epoch and requires both fresh keys',()=>{
  const s=ready(),oldMetadata=info(),command=s.resize(resize());assert.equal(command.topologyEpoch,2);
  assert.deepEqual(command.tracks.map(t=>[t.id,t.epoch]),[['0',1],['1',2]]);assert.equal(JSON.stringify(command).includes('epochs'),false);
  assert.equal(s.resize(resize()).reason,'resize_in_progress');assert.equal(s.ready,false);
  assert.equal(s.input(pointer(),()=>true),'waiting');assert.equal(s.media(sound()),'waiting');
  assert.equal(s.acknowledge(acknowledgement()),'stale');assert.equal(s.acceptInfo(oldMetadata),'stale');
  assert.equal(s.acceptInfo({...info('session-fixture',command),resizeId:'late'}),'stale');
  assert.equal(s.acceptInfo(info('session-fixture',command)),'accepted');assert.equal(s.media(packet()),'stale');
  assert.equal(s.media(packet({track:'1',topology:2,epoch:1})),'stale');
  assert.equal(s.media(packet({track:'0',topology:2})),'forward');assert.equal(s.ready,false);
  assert.equal(s.media(packet({track:'1',topology:2,epoch:2})),'forward');
  assert.equal(s.acknowledge(acknowledgement(info('session-fixture',command))),'accepted');
  assert.equal(s.input(pointer({topologyEpoch:1}),()=>true),'stale');assert.equal(s.input(pointer({topologyEpoch:2,epoch:1}),()=>true),'stale');
  assert.equal(s.input(pointer({topologyEpoch:2,epoch:2}),()=>true),'forward');assert.equal(s.media(sound(2)),'forward');
  assert.equal(s.acceptInfo(oldMetadata),'stale');
  assert.equal(s.resize(resize()).reason,'stale_topology');assert.equal(s.ready,true);assert.equal(s.pending,null);
});

test('Invalid viewport changes preserve ready state and position-only change still resets the common topology',()=>{
  const s=ready();for(const value of [resize({tracks:[]}),resize({tracks:[requests[0]]}),resize({fps:120}),resize({requestId:'../path'}),resize({extra:true})]){
    assert.ok(s.resize(value).reason);assert.equal(s.ready,true);assert.equal(s.pending,null);
  }
  const command=s.resize(resize({tracks:requests.map(t=>({...t,x:t.x+20}))}));assert.deepEqual(command.tracks.map(t=>t.epoch),[1,1]);
  assert.equal(command.topologyEpoch,2);assert.equal(s.ready,false);
  const failure={type:'stream.tracks.resize.failed',sessionId:'session-fixture',requestId:command.requestId,resizeId:command.resizeId,topologyEpoch:2,reason:'resize_failed'};
  assert.ok(s.failedResize(failure));assert.equal(s.failedResize({...failure,resizeId:'old'}),false);
});

test('Track preparation/decode deadlines are bounded and generation counters never wrap',()=>{
  let now=100;const s=state({now:()=>now,prepareMs:10,readyMs:5});now=111;assert.equal(s.expired,true);
  assert.equal(s.acceptInfo(info()),'accepted');assert.equal(s.expired,false);now=117;assert.equal(s.expired,true);
  for(const track of ['0','1'])s.media(packet({track}));s.acknowledge(acknowledgement());now=10000;assert.equal(s.expired,false);
  s.info.topologyEpoch=0xffffffff;assert.equal(s.resize(resize({topologyEpoch:0xffffffff})).reason,'epoch_exhausted');assert.equal(s.pending,null);
  s.info.topologyEpoch=1;s.info.tracks[1].epoch=0xffffffff;assert.equal(s.resize(resize()).reason,'epoch_exhausted');
});

test('Per-output telemetry is measured, bounded, scoped and ignored during reconfiguration',()=>{
  const s=ready(),stats={type:'stream.tracks.stats',sessionId:'session-fixture',topologyEpoch:1,measuredMs:1000,
    tracks:[{id:'0',epoch:1,capturedFps:29,encodedFps:28.5,bitrateKbps:1200},{id:'1',epoch:1,capturedFps:0,encodedFps:0,bitrateKbps:0}]};
  assert.deepEqual(s.stats(stats),stats);assert.equal(s.stats({...stats,sessionId:'other'}),null);
  assert.equal(s.stats({...stats,measuredMs:0}),false);assert.equal(s.stats({...stats,tracks:stats.tracks.map(t=>({...t,capturedFps:Infinity}))}),false);
  s.resize(resize());assert.equal(s.stats(stats),null);
});

test('Shared PCM, telemetry and one live output cannot hide a stalled second video output',()=>{
  let now=0;const s=state({now:()=>now,mediaMs:100});s.acceptInfo(info());
  for(const track of ['0','1'])s.media(packet({track}));s.acknowledge(acknowledgement());
  now=90;s.media(packet({track:'0',pts:1}));s.media(sound());assert.equal(s.stalled,false);
  now=101;s.media(packet({track:'0',pts:2}));s.media(sound(1,1));s.acknowledge(acknowledgement());assert.equal(s.stalled,true);
});

test('Hostile JSON object identities, primitives and nesting cannot throw through track message handlers',()=>{
  const values=[null,{},[],0,'1',false,{toString:0},{id:{toString:0}},...['__proto__','constructor'].map(id=>({id,epoch:1}))];
  let nested={};for(let n=0;n<1000;n++)nested={a:nested};values.push(nested);
  for(const value of values){const s=prepared();assert.doesNotThrow(()=>s.acceptInfo({...info(),tracks:[value,value]}));
    assert.doesNotThrow(()=>s.acknowledge({...acknowledgement(),tracks:[value,value]}));
    assert.doesNotThrow(()=>s.stats({type:'stream.tracks.stats',sessionId:'session-fixture',topologyEpoch:1,measuredMs:1,tracks:[value,value]}));
    assert.doesNotThrow(()=>s.resize({...resize(),tracks:[value,value]}));}
  for(const value of values){const s=ready();assert.doesNotThrow(()=>s.acceptInfo({...info(),tracks:[value,value]}));
    assert.doesNotThrow(()=>s.input(value,()=>false));assert.doesNotThrow(()=>s.acknowledge(value));assert.doesNotThrow(()=>s.resize(value));}
});

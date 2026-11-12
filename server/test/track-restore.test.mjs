import{test}from'node:test';import assert from'node:assert/strict';
import{TrackStream,trackRequests}from'../src/track-media.mjs';
import{requests,options,packet,info,acknowledgement,pointer,resize}from'./track-fixture.mjs';
function profileInfo(...args){const result=info(...args),root=result.tracks.find(t=>t.id==='0');return{...result,tracks:result.tracks.map(t=>({...t,left:t.left-root.left,top:t.top-root.top}))};}
function ready(settings={}){const state=new TrackStream('session-fixture',trackRequests(requests),options,{allowRestored:true,...settings});assert.equal(state.acceptInfo(profileInfo()),'accepted');for(const track of['0','1'])assert.equal(state.media(packet({track})),'forward');assert.equal(state.acknowledge(acknowledgement()),'accepted');return state;}
function restored(command){return{type:'stream.tracks.resize.restored',sessionId:'session-fixture',requestId:command.requestId,resizeId:command.resizeId,topologyEpoch:command.topologyEpoch,fps:options.fps,tracks:requests.map(t=>({...t,epoch:1})),reason:'unsupported_display_mode'};}
test('Verified restore consumes a new topology while retaining only previous geometry/FPS/epochs, then waits for fresh media/decoder ACK',()=>{
  const s=ready();const command=s.resize(resize());const deadline=s.deadline;const rollback=restored(command);
  assert.equal(s.restoredResize(rollback),'accepted');assert.equal(s.deadline,deadline);assert.equal(s.ready,false);assert.equal(s.input(pointer(),()=>true),'waiting');
  assert.equal(s.acceptInfo(profileInfo('session-fixture',rollback)),'accepted');assert.equal(s.info.topologyEpoch,2);assert.equal(s.ready,false);
  assert.equal(s.media(packet({track:'0'})),'stale');assert.equal(s.acknowledge(acknowledgement(profileInfo('session-fixture',rollback))),'invalid');
  for(const track of['0','1'])assert.equal(s.media(packet({track,topology:2,epoch:1,pts:0})),'forward');
  assert.equal(s.acknowledge(acknowledgement(profileInfo('session-fixture',rollback))),'accepted');assert.equal(s.ready,true);
  assert.equal(s.input(pointer(),()=>true),'stale');assert.equal(s.input(pointer({topologyEpoch:2,epoch:1}),()=>true),'forward');
  assert.equal(s.resize({...resize(),topologyEpoch:2}).topologyEpoch,3);
});
test('Restored responses are exact, idempotent and cannot extend the original preparation deadline',()=>{
  let now=1;const s=ready({now:()=>now});const command=s.resize(resize()),value=restored(command),deadline=s.deadline;
  assert.equal(s.restoredResize({...value,tracks:[...value.tracks].reverse()}),'accepted');now=500;
  assert.equal(s.restoredResize(value),'duplicate');assert.equal(s.deadline,deadline);
  assert.equal(s.restoredResize({...value,reason:'resize_failed'}),'invalid');assert.equal(s.pending.restored.reason,'unsupported_display_mode');
  now=deadline+1;assert.equal(s.restoredResize(value),'invalid');assert.equal(s.expired,true);
});
test('Unknown, extra, speculative or unproved restored state never changes the authorized pending request',()=>{
  for(const mutate of[
    v=>({...v,extra:true}),v=>({...v,fps:v.fps===30?60:30}),v=>({...v,reason:'arbitrary exception text'}),
    v=>({...v,tracks:v.tracks.slice(0,1)}),v=>({...v,tracks:[v.tracks[0],v.tracks[0]]}),
    v=>({...v,tracks:v.tracks.map(t=>({...t,sourceWidth:t.width}))}),
    ...['width','height','x','y','epoch'].map(key=>v=>({...v,tracks:v.tracks.map((t,n)=>n===0?{...t,[key]:t[key]+2}:t)})),
    v=>({...v,tracks:[null,v.tracks[1]]}),v=>({...v,tracks:[{id:{toString:0}},v.tracks[1]]})
  ]){const s=ready(),command=s.resize(resize()),before=structuredClone(s.pending);assert.equal(s.restoredResize(mutate(restored(command))),'invalid');assert.deepEqual(s.pending,before);assert.equal(s.ready,false);}
  for(const allowRestored of[false,undefined,1,'true']){const s=ready({allowRestored});const command=s.resize(resize());assert.equal(s.restoredResize(restored(command)),'invalid');assert.equal(s.pending.restored,undefined);}
});
test('Stale restores and initial restore claims cannot adopt a different session, request or topology',()=>{
  const initial=new TrackStream('session-fixture',trackRequests(requests),options,{allowRestored:true});assert.equal(initial.restoredResize(restored({topologyEpoch:1})),'stale');
  for(const field of['sessionId','requestId','resizeId','topologyEpoch']){const s=ready(),command=s.resize(resize()),before=structuredClone(s.pending),value=restored(command);assert.equal(s.restoredResize({...value,[field]:field==='topologyEpoch'?1:'other'}),'stale');assert.deepEqual(s.pending,before);}
});
test('Profile tracks require owned zero to be the actual origin/primary before any video can become ready',()=>{
  for(const mutate of[v=>({...v,tracks:v.tracks.map(t=>({...t,primary:t.id==='1'}))}),v=>({...v,tracks:v.tracks.map(t=>({...t,primary:false}))}),v=>({...v,tracks:v.tracks.map(t=>({...t,left:t.left+2}))}),v=>({...v,tracks:v.tracks.map(t=>({...t,top:t.top+2}))})]){const state=new TrackStream('session-fixture',trackRequests(requests),options,{allowRestored:true});assert.equal(state.acceptInfo(mutate(profileInfo())),'invalid');assert.equal(state.info,null);}
});

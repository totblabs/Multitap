import{test}from'node:test';import assert from'node:assert/strict';
import{profileTracksProtocol,requestedProfileTracks,profileTracksCapability,profileTracksSupported}from'../src/profile-tracks.mjs';
const caps=()=>({streamProfileProtocol:'console-profile-v1',streamProfileTracksProtocol:'group-v1',videoTrackProtocol:'tracks-v1',trackViewportProvisioning:true,viewportProvisioning:true,dynamicResolution:true,resizeProtocol:'epoch-v1',service:true,interactiveSession:true,streaming:true,virtualDisplayReady:true,monitors:[{id:'0',width:1280,height:720,primary:false},{id:'1',width:1600,height:900,primary:false}]});
test('Group profile tuple is explicit and never inferred from media protocol or Host version',()=>{
  assert.equal(profileTracksProtocol,'group-v1');assert.equal(requestedProfileTracks('stream',undefined,[{}]),false);assert.equal(requestedProfileTracks('stream','group-v1',[{}]),true);
  for(const values of[['rdp','group-v1',[{}]],['stream','group-v2',[{}]],['stream',null,[{}]],['stream',true,[{}]],['stream','group-v1',null]])assert.throws(()=>requestedProfileTracks(...values),e=>e.status===400);
  const host={capabilities:caps(),capabilitiesAt:1000};assert.equal(profileTracksSupported(host,1100,100),true);assert.equal(profileTracksSupported({...host,capabilitiesPending:true},1100,100),false);assert.equal(profileTracksSupported(host,1301,100),false);
  assert.equal(profileTracksCapability({...caps(),monitors:caps().monitors.slice(0,1)}),true);
});
test('Partial, mistyped, foreign or malformed group capability tuples are rejected',()=>{
  for(const key of Object.keys(caps()))for(const value of[undefined,null,false,1,'unknown'])assert.equal(profileTracksCapability({...caps(),[key]:value}),false,`${key}:${String(value)}`);
  for(const monitors of[{},[],[null],[{id:'console',width:1280,height:720}],caps().monitors.map(t=>({...t,id:'0'})),caps().monitors.map(t=>({...t,width:1281})),caps().monitors.map(t=>({...t,height:2162}))])assert.equal(profileTracksCapability({...caps(),monitors}),false);
});

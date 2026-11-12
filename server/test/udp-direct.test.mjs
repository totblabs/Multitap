import test from 'node:test';
import assert from 'node:assert/strict';
import { createSocket } from 'node:dgram';
import { setTimeout as delay } from 'node:timers/promises';
import { UdpRelay } from '../src/udp-relay.mjs';
import { bindDatagram } from '../src/media-udp.mjs';
import { localCandidate, directCapability, udpDirectProtocol } from '../src/udp-direct.mjs';
import { udpGrantMessage } from '../src/udp-media.mjs';
import { configuration } from '../src/config.mjs';

const until=async predicate=>{for(let n=0;n<1000;n++){if(predicate())return;await delay(4);}assert.fail('fixture_timeout');};
test('LAN and STUN candidates are bounded hints and require authenticated path checks',()=>{
  for(const address of ['10.1.2.3','172.16.0.1','172.31.255.254','192.168.1.2'])assert.deepEqual(localCandidate({protocol:udpDirectProtocol,address,port:1234}),{address,port:1234});
  for(const address of ['127.0.0.1','0.0.0.0','169.254.1.1','224.0.0.1','192.0.2.1','172.32.0.1','localhost','10.1.2.999','10.01.2.3','::1'])assert.equal(localCandidate({protocol:udpDirectProtocol,address,port:1234}),null);
  for(const port of [0,65536,1.2,'1234'])assert.equal(localCandidate({protocol:udpDirectProtocol,address:'10.1.2.3',port}),null);
  assert.equal(directCapability({udpDirectHostProtocol:udpDirectProtocol}),true);assert.equal(directCapability({udpDirectProtocol}),false);assert.equal(directCapability({udpDirectHostProtocol:'other'}),false);
  assert.deepEqual(localCandidate({protocol:udpDirectProtocol,candidateType:'srflx',address:'192.0.2.1',port:54321}),{address:'192.0.2.1',port:54321});
  assert.equal(localCandidate({protocol:udpDirectProtocol,candidateType:'srflx',address:'192.168.1.2',port:54321}),null);
});
test('legacy STUN remains bounded but relay-only normal configuration has no default destination',()=>{
  const env={MULTITAP_UDP_PORT:'18481'};
  assert.deepEqual(configuration(env,{readAccessFile:()=>null}).udp.stun,[]);
  assert.equal(configuration(env,{readAccessFile:()=>null}).udp.allowPeerDirect,false);
  assert.deepEqual(configuration({...env,MULTITAP_STUN_SERVERS:'stun.example:3478'},{readAccessFile:()=>null}).udp.stun,[{host:'stun.example',port:3478}]);
  assert.deepEqual(configuration({...env,MULTITAP_STUN_SERVERS:'off'},{readAccessFile:()=>null}).udp.stun,[]);
  assert.throws(()=>configuration({...env,MULTITAP_STUN_SERVERS:'a.example:3478,b.example:3478,c.example:3478'},{readAccessFile:()=>null}),/invalid_udp_stun/);
});
test('dormant direct grant formatting needs explicit trusted policy and media keys',()=>{
  const grant={expiresAt:123},leg={id:Buffer.alloc(8),ticket:Buffer.alloc(32)};
  assert.equal(udpGrantMessage(grant,[],leg,true,true,[],true).directProtocol,udpDirectProtocol);
  assert.equal(udpGrantMessage(grant,[],leg,true,true).directProtocol,undefined);
  assert.equal(udpGrantMessage(grant,[],leg,true).directProtocol,undefined);
  assert.equal(udpGrantMessage(grant,[],leg,false,true,[],true).directProtocol,undefined);
});
test('trusted constructor-only dormant peer path offers only current bindings, never stale or revoked grants',async t=>{
  const live=new Set(['one','two']),offers=[];
  const relay=new UdpRelay({port:0,host:'127.0.0.1',allowPeerDirect:true,live:id=>live.has(id),logger:{warn(){}}});
  relay.onPeer=(...value)=>offers.push(value);t.after(()=>relay.close());await until(()=>relay.running);
  const first=relay.issue('one',{direct:true}),second=relay.issue('two',{direct:true});
  const peer=createSocket('udp4');await new Promise(done=>peer.bind(0,'127.0.0.1',done));peer.on('message',()=>{});t.after(()=>peer.close());
  const send=bytes=>new Promise((done,fail)=>peer.send(bytes,relay.port,'127.0.0.1',error=>error?fail(error):done()));
  for(let n=0;n<10&&!offers.length;n++){await send(bindDatagram(first.client.ticket,first.client.id));await delay(8);}
  assert.deepEqual(offers,[['one','host',[{address:'127.0.0.1',port:peer.address().port}]]]);
  relay.local('one','client',{protocol:udpDirectProtocol,address:'192.168.1.2',port:1234});
  assert.deepEqual(offers.at(-1),['one','host',[{address:'127.0.0.1',port:peer.address().port},{address:'192.168.1.2',port:1234}]]);
  const before=offers.length;
  relay.local('one','client',{protocol:udpDirectProtocol,address:'10.1.2.3',port:4321});
  relay.bound({session:'two',id:first.client.id.toString('latin1'),address:'192.0.2.1',port:1234});
  assert.equal(offers.length,before,'duplicate locals and another session ID cannot redirect a peer');
  relay.issue('one',{direct:true});relay.bound({session:'one',id:first.client.id.toString('latin1'),address:'192.0.2.1',port:1234});assert.equal(offers.length,before);
  live.delete('two');relay.bound({session:'two',id:second.client.id.toString('latin1'),address:'192.0.2.1',port:1234});assert.equal(offers.length,before);
});

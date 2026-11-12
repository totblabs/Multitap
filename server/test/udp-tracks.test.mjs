import test from 'node:test';
import assert from 'node:assert/strict';
import {createSocket} from 'node:dgram';
import {setTimeout as delay} from 'node:timers/promises';
import {UdpRelay} from '../src/udp-relay.mjs';
import {bindDatagram,ackDatagram,dataDatagram} from '../src/media-udp.mjs';

const until=async predicate=>{for(let n=0;n<1000;n++){if(predicate())return;await delay(4);}assert.fail('fixture_timeout');};
const scope=(track,epoch=11)=>({track,topologyEpoch:7,epoch,generation:1});
async function fixture(t,{allowPeerDirect=false}={}){
  const relay=new UdpRelay({host:'127.0.0.1',port:0,allowPeerDirect,live:id=>id==='parent',logger:{warn(){}}});t.after(()=>relay.close());await until(()=>relay.running);
  const peers=[];
  async function peer(){
    const socket=createSocket('udp4'),received=[];socket.on('message',bytes=>received.push(bytes));await new Promise(done=>socket.bind(0,'127.0.0.1',done));t.after(()=>socket.close());
    const send=bytes=>new Promise((done,fail)=>socket.send(bytes,relay.port,'127.0.0.1',error=>error?fail(error):done()));
    const value={send,received,port:socket.address().port};peers.push(value);return value;
  }
  async function bind(peer,leg){const ack=ackDatagram(leg.ticket,leg.id);for(let n=0;n<200&&!peer.received.some(bytes=>bytes.equals(ack));n++){await peer.send(bindDatagram(leg.ticket,leg.id));await delay(5);}assert.ok(peer.received.some(bytes=>bytes.equals(ack)),'bound leg');}
  return {relay,peer,bind,peers};
}

test('two independent output pairs share one authorized parent and never forward into each other',async t=>{
  const f=await fixture(t,{allowPeerDirect:true}),offers=[];f.relay.onPeer=(...args)=>offers.push(args);
  const grant=f.relay.issue('parent',{direct:true,tracks:[scope('0'),scope('1',12)]});assert.equal(grant.tracks.length,2);
  const clients=[],hosts=[];
  for(const row of grant.tracks){const client=await f.peer(),host=await f.peer();clients.push(client);hosts.push(host);await f.bind(client,row.client);await f.bind(host,row.host);}
  assert.equal(f.relay.stats().sessions,1);assert.equal(f.relay.stats().bindings,4);
  for(let n=0;n<2;n++){
    const bytes=dataDatagram(grant.tracks[n].client.id,Buffer.from([0x91,n]));await clients[n].send(bytes);await until(()=>hosts[n].received.some(value=>value.equals(bytes)));
    assert.equal(hosts[1-n].received.some(value=>value.equals(bytes)),false,'an output packet reached the other decoder');
  }
  await until(()=>offers.length===4);
  assert.equal(offers.filter(row=>row[3]?.track==='0').length,2);assert.equal(offers.filter(row=>row[3]?.track==='1').length,2);
  const before=offers.length;
  f.relay.local('parent','client',{protocol:'udp-direct-v1',...scope('0'),topologyEpoch:6,address:'192.168.1.2',port:1234});assert.equal(offers.length,before,'retired topology offered an address');
  f.relay.local('parent','client',{protocol:'udp-direct-v1',...scope('0'),generation:2,address:'192.168.1.2',port:1234});assert.equal(offers.length,before,'another grant generation offered an address');
  f.relay.local('parent','client',{protocol:'udp-direct-v1',...scope('0'),address:'192.168.1.2',port:1234});
  assert.equal(offers.at(-1)[3].track,'0');assert.equal(offers.at(-1)[2].some(row=>row.address==='192.168.1.2'),true);
  f.relay.revoke('parent');await until(()=>f.relay.stats().bindings===0);assert.equal(f.relay.stats().sessions,0);assert.equal(f.relay.direct.size,0);
  const dropped=f.relay.stats().dropped.unknown_id;await clients[1].send(dataDatagram(grant.tracks[1].client.id,Buffer.from([0x92])));await until(()=>f.relay.stats().dropped.unknown_id>dropped);
});

test('invalid output scopes do not replace a working grant, and regrant retires all old legs',async t=>{
  const f=await fixture(t),grant=f.relay.issue('parent',{direct:true,tracks:[scope('0'),scope('1',12)]});
  const peer=await f.peer();await f.bind(peer,grant.tracks[0].client);
  for(const tracks of [[],[null],[scope('0'),scope('0')],[scope('0'),{...scope('1'),topologyEpoch:8}],[scope('0',65536)],[{...scope('0'),track:0}]])assert.equal(f.relay.issue('parent',{direct:true,tracks}),null);
  assert.equal(f.relay.stats().sessions,1);assert.equal(f.relay.stats().bindings,1);
  const next=f.relay.issue('parent',{direct:true,tracks:[scope('0',13),scope('1',14)]});await until(()=>f.relay.stats().bindings===0);
  assert.notDeepEqual(next.tracks[0].client.id,grant.tracks[0].client.id);
  const before=f.relay.stats().dropped.unknown_id;await peer.send(bindDatagram(grant.tracks[0].client.ticket,grant.tracks[0].client.id));await until(()=>f.relay.stats().dropped.unknown_id>before);
});

// Loopback-only trusted DOM -> unchanged noVNC Keyboard -> actual RFB bytes.
import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {createRequire} from 'node:module';
import {readFile,writeFile,mkdir} from 'node:fs/promises';
import {resolve,dirname,extname} from 'node:path';
import {fileURLToPath,pathToFileURL} from 'node:url';
import {createHash} from 'node:crypto';
const root=resolve(dirname(fileURLToPath(import.meta.url)),'..'),assets=resolve(root,'server/admin/console');
const opts=Object.fromEntries(process.argv.slice(2).map(x=>x.split(/=(.*)/s).slice(0,2)));
assert(opts['--playwright-module']&&opts['--browser'],'Existing Playwright and browser paths required; no install');
const {chromium}=await import(pathToFileURL(resolve(opts['--playwright-module'])));
const {WebSocketServer}=createRequire(resolve(root,'server/package.json'))('ws');
const output=resolve(opts['--output']||resolve(root,'.artifacts/browser-console/hangul-wire'));
await mkdir(output,{recursive:true});
const hash=b=>createHash('sha256').update(b).digest('hex');
const manifest=JSON.parse(await readFile(resolve(assets,'assets-manifest.json'),'utf8'));
const sourcePaths=[fileURLToPath(import.meta.url),resolve(assets,'assets-manifest.json'),...manifest.files.map(r=>resolve(assets,r.path))];
const sourceHashes=async()=>Object.fromEntries(await Promise.all(sourcePaths.map(async p=>[p.slice(root.length+1).replaceAll('\\','/'),hash(await readFile(p))])));
const before=await sourceHashes(),report={kind:'isolated-trusted-dom-novnc-rfb-wire',checks:[],started:new Date().toISOString(),notAcceptance:['actual Windows guest IME','production console','physical keyboard driver events']};
const fixtureJs=`import RFB from '/assets/vendor/novnc/1.7.0/core/rfb.js';
import {createHangulKeyAdapter} from '/assets/hangul-key.js';
const remote=document.getElementById('remote');
const rfb=new RFB(remote,new URL('/rfb',location.href).href.replace('http:','ws:'));
rfb.addEventListener('connect',()=>{window.connected=true;rfb.focus();});
window.fixture={rfb,enabled:false,active:true,documentFocused:true,events:[]};
const a=createHangulKeyAdapter({getRfb:()=>rfb,canSend:()=>window.fixture.active&&window.fixture.documentFocused&&document.hasFocus()&&!document.hidden});
// Deliberately registered AFTER new RFB: ancestor capture must precede its target listener.
for(const type of ['keydown','keyup'])remote.addEventListener(type,e=>{window.fixture.events.push({type:e.type,key:e.key,code:e.code,keyCode:e.keyCode,isTrusted:e.isTrusted,repeat:e.repeat});if(window.fixture.enabled)a.handle(e);},true);
window.fixture.reset=()=>{a.reset();rfb.blur();};
window.addEventListener('blur',()=>window.fixture.reset());`;
const server=createServer(async(req,res)=>{
  try{
    const path=new URL(req.url,'http://127.0.0.1').pathname;
    if(path==='/'){res.setHeader('Content-Type','text/html');return res.end('<!doctype html><input id="login"><div id="remote" style="width:640px;height:360px"></div><script type="module" src="/fixture.js"></script>');}
    if(path==='/fixture.js'){res.setHeader('Content-Type','text/javascript');return res.end(fixtureJs);}
    const row=manifest.files.find(r=>'/assets/'+r.path===path);if(!row){res.writeHead(404);return res.end();}
    const data=await readFile(resolve(assets,row.path));assert.equal(hash(data),row.sha256);assert.equal(data.length,row.bytes);
    res.setHeader('Content-Type',extname(path)==='.js'?'text/javascript':'application/octet-stream');res.end(data);
  }catch{res.writeHead(500);res.end();}
});
let qemu=true,wire=[],connections=[];
const wss=new WebSocketServer({server});
wss.on('connection',socket=>{
  connections.push(socket);let input=Buffer.alloc(0),stage=0,acked=false;
  socket.send(Buffer.from('RFB 003.008\n'));
  socket.on('message',data=>{
    input=Buffer.concat([input,data]);
    while(input.length){
      if(stage===0){if(input.length<12)return;assert.equal(input.subarray(0,12).toString(),'RFB 003.008\n');input=input.subarray(12);socket.send(Buffer.from([1,1]));stage=1;continue;}
      if(stage===1){assert.equal(input[0],1);input=input.subarray(1);socket.send(Buffer.alloc(4));stage=2;continue;}
      if(stage===2){input=input.subarray(1);const b=Buffer.alloc(28);b.writeUInt16BE(640);b.writeUInt16BE(360,2);Buffer.from([32,24,0,1,0,255,0,255,0,255,0,8,16,0,0,0]).copy(b,4);b.writeUInt32BE(4,20);b.write('Test',24);socket.send(b);stage=3;continue;}
      const type=input[0];let length;
      if(type===0)length=20;else if(type===2){if(input.length<4)return;length=4+input.readUInt16BE(2)*4;}
      else if(type===3)length=10;else if(type===4)length=8;else if(type===5)length=6;
      else if(type===6){if(input.length<8)return;length=8+input.readUInt32BE(4);}else if(type===255)length=12;else throw Error('unexpected fixture client type');
      if(input.length<length)return;const packet=Buffer.from(input.subarray(0,length));input=input.subarray(length);wire.push(packet);
      if(type===2&&qemu&&!acked){const enc=[];for(let i=4;i<packet.length;i+=4)enc.push(packet.readInt32BE(i));assert(enc.includes(-258));const ack=Buffer.alloc(16);ack.writeUInt16BE(1,2);ack.writeUInt16BE(640,8);ack.writeUInt16BE(360,10);ack.writeInt32BE(-258,12);socket.send(ack);acked=true;}
    }
  });
});
await new Promise(r=>server.listen(0,'127.0.0.1',r));const origin=`http://127.0.0.1:${server.address().port}`;
let browser,page,context,cdp;
const delay=()=>new Promise(r=>setTimeout(r,50));
const packets=()=>wire.filter(p=>p[0]===4||p[0]===255);
const decoded=()=>packets().map(p=>({type:p[0],down:p[0]===255?p.readUInt16BE(2):p[1],keysym:p.readUInt32BE(4),...(p[0]===255?{code:p.readUInt32BE(8)}:{})}));
async function event(type,extra={}){await cdp.send('Input.dispatchKeyEvent',{type,key:'HangulMode',code:'Lang1',windowsVirtualKeyCode:21,nativeVirtualKeyCode:21,...extra});}
async function check(name,fn){wire=[];await fn();await delay();report.checks.push({name,result:'pass',wire:packets().map(p=>p.toString('hex'))});process.stdout.write('PASS '+name+'\n');}
function tap(type=255){assert.deepEqual(decoded(),[1,0].map(down=>({type,down,keysym:0xff31,...(type===255?{code:0x72}:{})})));}
async function focus(){await page.locator('#remote canvas').focus();await page.evaluate(()=>window.fixture.rfb.focus());}
async function open(extended=true){
  qemu=extended;wire=[];context=await browser.newContext();page=await context.newPage();
  page.on('request',r=>assert.equal(new URL(r.url()).origin,origin,'loopback requests only'));
  await page.goto(origin);await page.waitForFunction(()=>window.connected===true);await focus();cdp=await context.newCDPSession(page);await delay();
  report.platform=await page.evaluate(()=>navigator.platform);assert.match(report.platform,/^Win/);
}
try{
  browser=await chromium.launch({headless:true,executablePath:resolve(opts['--browser']),args:['--no-first-run','--disable-background-networking','--disable-component-update']});report.browser=await browser.version();
  await open();
  await check('unmodified vendor drops trusted Windows orphan Hangul release',async()=>{await event('keyUp');await delay();assert.deepEqual(decoded(),[]);assert.equal(await page.evaluate(()=>window.fixture.events.at(-1).isTrusted),true);});
  await check('unmodified vendor paired Hangul emits exact QEMU Lang1 0x72',async()=>{await event('keyDown');await event('keyUp');await delay();tap();});
  await page.evaluate(()=>window.fixture.enabled=true);
  await check('adapter repairs trusted orphan with one QEMU tap',async()=>{await event('keyUp');await delay();tap();});
  await check('capture registered after vendor preserves paired path without a duplicate',async()=>{await event('keyDown');await event('keyUp');await delay();tap();});
  await check('paired auto repeat remains vendor behavior without adapter taps',async()=>{await event('keyDown');await event('keyDown',{autoRepeat:true});await event('keyUp');await delay();assert.deepEqual(decoded().map(p=>p.down),[1,1,0]);assert(decoded().every(p=>p.keysym===0xff31&&p.code===0x72));});
  await check('fresh down-only presses are not permanently swallowed',async()=>{await event('keyDown');await event('keyDown');await delay();assert.deepEqual(decoded().map(p=>p.down),[1,1]);await event('keyUp');await delay();assert.deepEqual(decoded().map(p=>p.down),[1,1,0]);});
  await check('blur reset has no stale suppression of the first new orphan',async()=>{await event('keyDown');await page.evaluate(()=>window.fixture.reset());await page.locator('#login').focus();await event('keyUp');await focus();wire=[];await event('keyUp');await delay();tap();});
  await check('repeat-only observed down then release is passed through without repair',async()=>{await event('keyDown',{autoRepeat:true});await event('keyUp');await delay();tap();});
  await check('login input and synthetic lifecycle release never synthesize a toggle',async()=>{await page.locator('#login').focus();await event('keyUp');await focus();await page.evaluate(()=>document.querySelector('#remote canvas').dispatchEvent(new KeyboardEvent('keyup',{key:'HangulMode',code:'Lang1',keyCode:21,bubbles:true})));await delay();assert.deepEqual(decoded(),[]);});
  await check('unfocused-document input gate and viewOnly prevent orphan repair',async()=>{await page.evaluate(()=>window.fixture.documentFocused=false);await event('keyUp');await page.evaluate(()=>{window.fixture.documentFocused=true;window.fixture.rfb.viewOnly=true;});await event('keyUp');await page.evaluate(()=>window.fixture.rfb.viewOnly=false);await delay();assert.deepEqual(decoded(),[]);});
  await check('actual background document focus prevents a trusted orphan repair',async()=>{await cdp.send('Emulation.setFocusEmulationEnabled',{enabled:false});const foreground=await context.newPage();await foreground.bringToFront();assert.equal(await page.evaluate(()=>document.hasFocus()),false);await event('keyUp');await delay();assert.deepEqual(decoded(),[]);await foreground.close();await cdp.send('Emulation.setFocusEmulationEnabled',{enabled:true});await page.bringToFront();await focus();});
  await check('ordinary Ctrl Alt A and IME Process are never remapped to Hangul',async()=>{await event('keyUp',{key:'Process',code:'Lang1',windowsVirtualKeyCode:229,nativeVirtualKeyCode:229});for(const [key,code,vk] of [['Control','ControlLeft',17],['Alt','AltLeft',18],['a','KeyA',65]]){await event('keyDown',{key,code,windowsVirtualKeyCode:vk,nativeVirtualKeyCode:vk});await event('keyUp',{key,code,windowsVirtualKeyCode:vk,nativeVirtualKeyCode:vk});}await delay();assert.equal(decoded().length,6);assert(decoded().every(p=>p.keysym!==0xff31));});
  await check('mixed Process Lang1 down and HangulMode release retain exactly the vendor pair',async()=>{await event('keyDown',{key:'Process',code:'Lang1',windowsVirtualKeyCode:229,nativeVirtualKeyCode:229});await event('keyUp');await delay();assert.deepEqual(decoded(),[1,0].map(down=>({type:255,down,keysym:0,code:0x72})));await event('keyUp');await delay();assert.deepEqual(decoded().slice(2),[1,0].map(down=>({type:255,down,keysym:0xff31,code:0x72})));});
  await context.close();await open(false);await page.evaluate(()=>window.fixture.enabled=true);
  await check('without QEMU acknowledgement public API retains legacy RFB key packets',async()=>{await event('keyUp');await delay();tap(4);});
  await context.close();report.result='pass';
}catch(e){report.result='fail';report.error=String(e.stack||e);throw e;}
finally{
  await browser?.close();for(const s of wss.clients)s.terminate();await new Promise(r=>wss.close(r));await new Promise(r=>server.close(r));
  report.sourceHashes=await sourceHashes();report.sourceUnchanged=JSON.stringify(before)===JSON.stringify(report.sourceHashes);report.finished=new Date().toISOString();await writeFile(resolve(output,'receipt.json'),JSON.stringify(report,null,2)+'\n');assert.equal(report.sourceUnchanged,true);
}

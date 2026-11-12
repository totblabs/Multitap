// Isolated, headless UI/protocol fixtures. Never a Windows guest acceptance test.
import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {createRequire} from 'node:module';
import {readFile,mkdir,writeFile} from 'node:fs/promises';
import {fileURLToPath,pathToFileURL} from 'node:url';
import {dirname,resolve,extname} from 'node:path';
import {createHash} from 'node:crypto';

const root=resolve(dirname(fileURLToPath(import.meta.url)),'..');
const assets=resolve(root,'server/admin/console');
const options=Object.fromEntries(process.argv.slice(2).map(arg=>arg.split(/=(.*)/s).slice(0,2)));
if(!options['--playwright-module']||!options['--browser'])throw Error('Explicit existing --playwright-module=<index.mjs> and --browser=<exe> required; no installation is performed.');
const {chromium}=await import(pathToFileURL(resolve(options['--playwright-module'])));
const require=createRequire(resolve(root,'server/package.json'));
const {WebSocketServer}=require('ws');
const output=resolve(options['--output']||resolve(root,'.artifacts/browser-console/ui'));
await mkdir(output,{recursive:true});
const manifest=JSON.parse(await readFile(resolve(assets,'assets-manifest.json'),'utf8'));
const fixtureBefore=createHash('sha256').update(await readFile(fileURLToPath(import.meta.url))).digest('hex');
const manifestBefore=createHash('sha256').update(await readFile(resolve(assets,'assets-manifest.json'))).digest('hex');
const before=Object.fromEntries(await Promise.all(manifest.files.map(async row=>[row.path,createHash('sha256').update(await readFile(resolve(assets,row.path))).digest('hex')])));
const report={kind:'isolated-headless-http-rfb-fixtures',notAcceptance:['production TLS','actual QEMU/Windows image','guest keyboard/IME mapping','real clipboard policy enforcement'],scopeChange:'Connected console shows only its tool grip; the folded bar opens on a click. Backend permission/Origin/clipboard bounds/revocation remain server-test scope.',started:new Date().toISOString(),checks:[],screenshots:[],frameTimeline:[]};
let fixture={};
const requests=[],wire=[],connections=[],frameTraces=[];
const csrf='fixture-csrf-token-only-not-a-real-secret';
const sessionId='11111111-2222-4333-8444-555555555555';
const cookie='console-fixture=synthetic-only';
const headers={'Content-Security-Policy':"default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; font-src 'self'; img-src 'self' data: blob:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'",'Cache-Control':'no-store','Referrer-Policy':'no-referrer'};
function respond(res,status,body){res.writeHead(status,{...headers,'Content-Type':'application/json; charset=utf-8'});res.end(status===204?undefined:JSON.stringify(body));}
const server=createServer(async(req,res)=>{
  try{
    const url=new URL(req.url,'http://fixture.local');
    if(url.pathname.startsWith('/console/api/')){
      const chunks=[];for await(const chunk of req)chunks.push(chunk);
      const body=chunks.length?JSON.parse(Buffer.concat(chunks)):null;
      requests.push({path:url.pathname,method:req.method,csrf:req.headers['x-multitap-console-csrf'],authorization:req.headers.authorization,query:url.search,bodyKeys:body?Object.keys(body).sort():[]});
      if(url.pathname.endsWith('/session'))return respond(res,fixture.sessionError?503:fixture.authenticated?200:401,fixture.sessionError?{error:{code:'console_unavailable'}}:fixture.authenticated?{user:{username:'fixture-user'},csrfToken:csrf}:{error:{code:'unauthorized'}});
      if(req.method==='GET'&&/^\/console\/api\/desktops\/lab_149\/sessions\/[0-9a-f-]{36}$/.test(url.pathname)){
        const preparation={...(fixture.qemuPreparation||{status:'unknown'})};
        if(fixture.preparationDelayMs)await new Promise(resolve=>setTimeout(resolve,fixture.preparationDelayMs));
        return respond(res,fixture.preparationError?503:200,fixture.preparationError?{error:{code:'console_unavailable'}}:{qemuConsolePreparation:preparation});
      }
      if(url.pathname.endsWith('/login')){
        if(fixture.loginError)return respond(res,401,{error:{code:'invalid_credentials'}});
        if(fixture.mfa)return respond(res,202,{mfa:{protocol:'mfa-v1',challengeToken:'synthetic-memory-only',expiresAt:new Date(Date.now()+60000).toISOString(),methods:['totp','recovery']},csrfToken:'synthetic-challenge-csrf-only'});
        res.setHeader('Set-Cookie',`${cookie}; HttpOnly; SameSite=Strict; Path=/console`);
        fixture.authenticated=true;return respond(res,200,{user:{username:'fixture-user'},csrfToken:csrf});
      }
      assert.equal(req.headers['x-multitap-console-csrf'],csrf);
      if(req.method==='DELETE')return respond(res,fixture.cleanupError?503:(fixture.cleanupStatus||204),fixture.cleanupError?{error:{code:'console_unavailable'}}:fixture.cleanupStatus?{error:{code:fixture.cleanupCode}}:undefined);
      if(url.pathname.endsWith('/logout')){if(fixture.logoutError)return respond(res,503,{error:{code:'console_unavailable'}});fixture.authenticated=false;res.setHeader('Set-Cookie',`${cookie}; HttpOnly; SameSite=Strict; Path=/console; Max-Age=0`);return respond(res,204);}
      if(url.pathname.endsWith('/connect')){
        if(fixture.connectUnauthorized)return respond(res,401,{error:{code:'unauthorized'}});
        if(fixture.busy)return respond(res,409,{error:{code:'console_busy'}});
        if(fixture.connectError)return respond(res,409,{error:{code:fixture.connectError}});
        if(fixture.pending)return respond(res,202,{pending:true,code:'console_preparing'});
        return respond(res,200,{id:sessionId,wsPath:`/console/ws/${sessionId}`,expiresAt:new Date(Date.now()+60000).toISOString(),desktop:{id:'lab_149',name:'테스트 VM · 149'},clipboardPolicy:fixture.policy||{textToRemote:false,textToLocal:false},...(fixture.qemuPreparation?{qemuConsolePreparation:fixture.qemuPreparation}:{})});
      }
      return respond(res,404,{error:{code:'not_found'}});
    }
    const path=url.pathname.startsWith('/console/assets/')?url.pathname.slice('/console/assets/'.length):'index.html';
    const row=manifest.files.find(item=>item.path===path);
    if(!row){res.writeHead(404,headers);return res.end();}
    const data=await readFile(resolve(assets,row.path));
    const types={'.html':'text/html; charset=utf-8','.js':'text/javascript; charset=utf-8','.css':'text/css; charset=utf-8','.woff2':'font/woff2','.txt':'text/plain; charset=utf-8','.json':'application/json'};
    res.writeHead(200,{...headers,'Content-Type':types[extname(path)]||'application/octet-stream'});res.end(data);
  }catch{respond(res,500,{error:{code:'fixture_failed'}});}
});
const wss=new WebSocketServer({server});
function rawFrame(){
  const w=640,h=360,packet=Buffer.alloc(16+w*h*4);packet.writeUInt16BE(1,2);packet.writeUInt16BE(w,8);packet.writeUInt16BE(h,10);
  if(!fixture.blackFrame)for(let y=0;y<h;y++)for(let x=0;x<w;x++){const offset=16+(y*w+x)*4;packet[offset]=20+Math.floor(25*x/w);packet[offset+1]=30+Math.floor(35*y/h);packet[offset+2]=42; if(x>70&&x<570&&y>130&&y<230){packet[offset]=218;packet[offset+1]=151;packet[offset+2]=45;}}
  return packet;
}
wss.on('connection',socket=>{
  connections.push(socket);let input=Buffer.alloc(0),stage=0,sentFrame=false;
  const trace={requests:[],frames:[]};frameTraces.push(trace);
  const thisFrame=fixture.frames?.[connections.length-1]??fixture.frame!==false;
  socket.send(Buffer.from('RFB 003.008\n'));
  socket.on('message',data=>{
    input=Buffer.concat([input,data]);
    while(input.length){
      if(stage===0){if(input.length<12)return;assert.equal(input.subarray(0,12).toString(),'RFB 003.008\n');input=input.subarray(12);socket.send(Buffer.from([1,fixture.credentials?2:1]));stage=1;continue;}
      if(stage===1){if(input.length<1)return;const selected=input[0];input=input.subarray(1);assert.equal(selected,fixture.credentials?2:1);socket.send(fixture.credentials?Buffer.alloc(16):Buffer.alloc(4));stage=fixture.credentials?99:2;continue;}
      if(stage===99)return;
      if(stage===2){input=input.subarray(1);const name=Buffer.from('Isolated protocol fixture');const init=Buffer.alloc(24+name.length);init.writeUInt16BE(640,0);init.writeUInt16BE(360,2);Buffer.from([32,24,0,1,0,255,0,255,0,255,0,8,16,0,0,0]).copy(init,4);init.writeUInt32BE(name.length,20);name.copy(init,24);socket.send(init);stage=3;continue;}
      const type=input[0];let length;
      if(type===0)length=20;
      else if(type===2){if(input.length<4)return;length=4+input.readUInt16BE(2)*4;}
      else if(type===3)length=10;
      else if(type===4)length=8;
      else if(type===5)length=6;
      else if(type===6){if(input.length<8)return;length=8+input.readUInt32BE(4);}
      else if(type===255)length=12;
      else throw Error(`Unexpected fixture RFB client type${type}`);
      if(input.length<length)return;
      const message=Buffer.from(input.subarray(0,length));wire.push(message);input=input.subarray(length);
      if(type===3&&trace.requests.length<64)trace.requests.push({at:Date.now(),incremental:message[1]===1,width:message.readUInt16BE(6),height:message.readUInt16BE(8)});
      if(type===3&&!sentFrame&&thisFrame){sentFrame=true;trace.frames.push({queuedAt:Date.now(),width:640,height:360});socket.send(rawFrame());}
    }
  });
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const origin=`http://127.0.0.1:${server.address().port}`;
const browser=await chromium.launch({headless:true,executablePath:resolve(options['--browser']),args:['--no-first-run','--disable-background-networking','--disable-component-update']});
report.browser=await browser.version();
async function check(name,fn){const started=Date.now();await fn();report.checks.push({name,result:'pass',elapsedMs:Date.now()-started});process.stdout.write(`PASS ${name}\n`);}
async function open(config={},path='/console/lab_149'){
  fixture={...config};requests.length=0;wire.length=0;connections.length=0;frameTraces.length=0;
  const context=await browser.newContext({viewport:{width:1366,height:768},colorScheme:'dark',...(config.embedded?{userAgent:'Multitap-ConsoleWebView/1'}:{})});
  // A stand-in for the app WebView's message bridge (window.chrome.webview).
  if(config.bridge)await context.addInitScript(()=>{window.__posted=[];window.chrome={webview:{postMessage:message=>window.__posted.push(message),addEventListener:(type,listener)=>{if(type==='message')window.__host=listener;}}};});
  const page=await context.newPage();const errors=[];
  page.on('pageerror',()=>errors.push('pageerror'));
  page.on('request',request=>assert.equal(new URL(request.url()).origin,origin,'no external network'));
  await page.goto(`${origin}${path}`);
  return {page,context,errors};
}
async function connected(page){await page.waitForFunction(()=>document.getElementById('connection-state').textContent.includes('콘솔 연결됨'));}
async function painted(page){await page.waitForFunction(()=>{const canvas=document.querySelector('#remote canvas');if(!canvas?.width||!canvas.height)return false;const pixel=canvas.getContext('2d').getImageData(0,0,1,1).data;return pixel[0]===20&&pixel[1]===30&&pixel[2]===42&&pixel[3]===255;});}
async function observeCanvas(page,phase){
  const canvas=await page.evaluate(()=>{const c=document.querySelector('#remote canvas');if(!c?.width||!c.height)return null;const rect=c.getBoundingClientRect();return {at:Date.now(),width:c.width,height:c.height,rect:{x:rect.x,y:rect.y,width:rect.width,height:rect.height},displayScale:rect.width/c.width,pixel:[...c.getContext('2d').getImageData(0,0,1,1).data],hasFocus:document.hasFocus()};});
  report.frameTimeline.push({phase,viewport:page.viewportSize(),canvas,requests:[...(frameTraces[0]?.requests||[])],frames:[...(frameTraces[0]?.frames||[])]});
}
async function until(fn){for(let i=0;i<100;i++){if(fn())return;await new Promise(r=>setTimeout(r,20));}assert.fail('fixture predicate timeout');}
const pointerMasks=()=>wire.filter(b=>b[0]===5).map(b=>b[1]);
try{
  await check('opaque black pixels never prove a visible desktop or erase unknown topology',async()=>{
    const {page,context}=await open({authenticated:true,blackFrame:true,qemuPreparation:{status:'unknown'}});await connected(page);
    await until(()=>frameTraces[0]?.frames.length===1);await page.waitForTimeout(1400);
    assert.equal((await page.locator('#connection-state').textContent()).includes('화면 데이터 수신'),false);
    assert.equal(await page.evaluate(()=>document.body.dataset.qemuConsole),'unconfirmed');
    await page.locator('#frame-note').waitFor({state:'visible',timeout:12000});assert.match(await page.locator('#frame-note').textContent(),/아직 안 보여요/);
    assert.equal(connections.length,1,'black pixels must not trigger automatic reconnect');await context.close();
  });
  await check('visible pixels show no note and do not upgrade unknown topology',async()=>{
    for(const status of ['unknown','active','isolated']){
      const {page,context}=await open({authenticated:true,qemuPreparation:{status}});await connected(page);await painted(page);
      await page.waitForFunction(()=>document.getElementById('connection-state').textContent.includes('화면 데이터 수신'));
      assert.equal(await page.locator('#frame-note').isVisible(),false);
      assert.equal(await page.evaluate(()=>document.body.dataset.qemuConsole),status==='isolated'?'isolated':'unconfirmed');
      assert.equal(connections.length,1);await context.close();
    }
  });
  await check('Host proof loss and status failures become unknown without reconnecting PVE',async()=>{
    for(const failed of [false,true]){
      const {page,context}=await open({authenticated:true,qemuPreparation:{status:'isolated'}});await connected(page);await painted(page);
      await page.waitForFunction(()=>document.getElementById('connection-state').textContent.includes('화면 데이터 수신'));
      assert.equal(await page.evaluate(()=>document.body.dataset.qemuConsole),'isolated');
      if(failed)fixture.preparationError=true;else fixture.qemuPreparation={status:'unknown'};
      await page.waitForFunction(()=>document.body.dataset.qemuConsole==='unconfirmed',null,{timeout:15000});
      assert.equal(await page.locator('#frame-note').isVisible(),false);
      assert.equal(connections.length,1);assert.equal(connections[0].readyState,1);await context.close();
    }
  });
  await check('a late proof response cannot change a replacement console window',async()=>{
    const {page,context}=await open({authenticated:true,qemuPreparation:{status:'isolated'},preparationDelayMs:1800});await connected(page);await painted(page);
    fixture.qemuPreparation={status:'unknown'};
    await page.waitForFunction(()=>document.getElementById('connection-state').textContent.includes('화면 데이터 수신'));
    for(let i=0;i<600&&!requests.some(r=>r.method==='GET'&&r.path.includes('/sessions/'));i++)await page.waitForTimeout(20);
    assert(requests.some(r=>r.method==='GET'&&r.path.includes('/sessions/')));
    connections[0].close(1000,'transport_closed');await page.locator('#retry').waitFor({state:'visible'});
    fixture.qemuPreparation={status:'isolated'};fixture.preparationDelayMs=0;await page.locator('#retry').click();await connected(page);await painted(page);
    await page.waitForTimeout(2100);assert.equal(await page.locator('#frame-note').isVisible(),false);assert.equal(connections.length,2);await context.close();
  });
  await check('embedded console expiry asks to reopen in app without a password form',async()=>{
    const {page,context}=await open({embedded:true});
    await page.getByRole('heading',{name:'콘솔 인증이 끝났어요',exact:true}).waitFor();
    assert.equal(await page.locator('#login-panel').isVisible(),false);
    assert.equal(await page.locator('#retry').isVisible(),false);
    assert.match(await page.locator('#notice-text').textContent(),/앱에서 다시/);
    assert.equal(connections.length,0);assert.equal(requests.filter(value=>value.path==='/console/api/login').length,0);
    await context.close();
  });
  await check('login obscured/no storage/no bearer, official RFB handshake and actual paint proof',async()=>{
    const {page,context,errors}=await open();
    await page.locator('#login-panel').waitFor({state:'visible'});assert.equal(await page.locator('#password').getAttribute('type'),'password');
    await page.locator('#username').fill('fixture-user');await page.locator('#password').fill('fixture-password');await page.locator('#login-submit').click();await connected(page);
    await painted(page);
    await page.waitForFunction(()=>document.getElementById('connection-state').textContent.includes('화면 데이터 수신'));
    assert.equal(await page.locator('#password').inputValue(),'');assert.equal(await page.evaluate(()=>localStorage.length+sessionStorage.length),0);
    assert.equal(await page.evaluate(()=>document.cookie),'');assert.equal(requests.filter(r=>r.path.endsWith('/connect')).length,1);
    assert(requests.every(r=>!r.authorization&&!r.query));assert.deepEqual(requests.find(r=>r.path.endsWith('/login')).bodyKeys,['password','username']);
    const format=wire.find(b=>b[0]===0);assert.deepEqual([...format.subarray(4)],[32,24,0,1,0,255,0,255,0,255,0,8,16,0,0,0]);
    assert.equal(await page.locator('#remote>div').evaluate(e=>getComputedStyle(e).backgroundColor),'rgb(0, 0, 0)');
    const enc=wire.find(b=>b[0]===2);assert(enc.readUInt16BE(2)>=24);assert.equal(await page.locator('#clipboard-open').isDisabled(),true);
    assert.deepEqual(errors,[]);await context.close();
  });
  await check('initial session failure offers bounded manual retry before any new connection',async()=>{
    const {page,context}=await open({authenticated:true,sessionError:true});await page.locator('#retry').waitFor({state:'visible'});assert.equal(requests.filter(r=>r.path.endsWith('/connect')).length,0);
    fixture.sessionError=false;await page.locator('#retry').click();await connected(page);assert.equal(requests.filter(r=>r.path.endsWith('/session')).length,2);assert.equal(requests.filter(r=>r.path.endsWith('/connect')).length,1);await context.close();
  });
  await check('held buttons and keys release on window blur and pagehide without a toolbar',async()=>{
    const {page,context,errors}=await open({authenticated:true});await connected(page);
    for(const [button,buttons]of [[0,1],[2,2],[1,4],[3,8],[4,16]]){
      wire.length=0;
      await page.evaluate(({button,buttons})=>{const c=document.querySelector('#remote canvas');c.dispatchEvent(new MouseEvent('mousedown',{button,buttons,clientX:300,clientY:300,bubbles:true,cancelable:true}));window.dispatchEvent(new Event('blur'));},{button,buttons});
      await until(()=>pointerMasks().includes(0));assert(pointerMasks().at(-1)===0);
    }
    wire.length=0;await page.locator('#remote canvas').focus();await page.keyboard.down('Control');await page.keyboard.down('Shift');
    await page.evaluate(()=>window.dispatchEvent(new Event('blur')));
    await until(()=>wire.filter(b=>b[0]===4&&b[1]===0).length>=2);
    await page.evaluate(()=>{const c=document.querySelector('#remote canvas');c.dispatchEvent(new MouseEvent('mousedown',{button:2,buttons:2,clientX:350,clientY:300,bubbles:true,cancelable:true}));c.dispatchEvent(new MouseEvent('mousedown',{button:1,buttons:6,clientX:350,clientY:300,bubbles:true,cancelable:true}));c.dispatchEvent(new MouseEvent('mousemove',{buttons:6,clientX:400,clientY:320,bubbles:true,cancelable:true}));window.dispatchEvent(new Event('blur'));});
    await until(()=>pointerMasks().at(-1)===0);
    await page.locator('#remote canvas').focus();await page.keyboard.down('Alt');await page.evaluate(()=>window.dispatchEvent(new Event('pagehide')));
    await until(()=>wire.some(b=>b[0]===4&&b[1]===0&&b.readUInt32BE(4)===0xffe9));await until(()=>connections[0].readyState===3);
    assert.deepEqual(errors,[]);await context.close();
  });
  await check('pending console preparation is manual retry, never an infinite poll',async()=>{
    const {page,context}=await open({authenticated:true,pending:true});await page.getByRole('heading',{name:'콘솔을 준비하고 있어요'}).waitFor();
    await page.waitForTimeout(250);assert.equal(requests.filter(r=>r.path.endsWith('/connect')).length,1);
    await page.locator('#retry').click();assert.equal(requests.filter(r=>r.path.endsWith('/connect')).length,2);await context.close();
  });
  await check('login rejection and future MFA challenge clear password and never authorize a socket',async()=>{
    for(const mode of ['loginError','mfa']){
      const {page,context}=await open({[mode]:true});await page.locator('#login-panel').waitFor({state:'visible'});await page.locator('#username').fill('fixture-user');await page.locator('#password').fill('fixture-password');await page.locator('#login-submit').click();
      await page.locator('#login-message').waitFor({state:'visible'});assert.equal(await page.locator('#password').inputValue(),'');assert.match(await page.locator('#login-message').textContent(),mode==='mfa'?/추가 인증/:/아이디와 비밀번호/);
      assert.equal(requests.filter(r=>r.path.endsWith('/connect')).length,0);assert.equal(connections.length,0);assert.equal(await page.evaluate(()=>localStorage.length+sessionStorage.length),0);await context.close();
    }
  });
  await check('denied unsolicited clipboard stays empty, busy state has explicit close-current-session action',async()=>{
    const {page,context}=await open({authenticated:true});await connected(page);
    const incoming=Buffer.from('not allowed','latin1'),packet=Buffer.alloc(8+incoming.length);packet[0]=3;packet.writeUInt32BE(incoming.length,4);incoming.copy(packet,8);connections[0].send(packet);await page.waitForTimeout(100);
    assert.equal(await page.locator('#clipboard-text').inputValue(),'');assert.equal(await page.locator('#clipboard-open').isDisabled(),true);await context.close();
    const next=await open({authenticated:true,busy:true});await next.page.getByRole('heading',{name:'다른 원격 연결이 사용 중이에요'}).waitFor();assert.match(await next.page.locator('#notice-text').textContent(),/스트리밍을 종료/);assert.equal(connections.length,0);await next.context.close();
  });
  await check('unexpected VNC credentials fail closed without Proxmox credential form',async()=>{
    const {page,context,errors}=await open({authenticated:true,credentials:true});await page.getByRole('heading',{name:'콘솔 인증 방식이 맞지 않아요'}).waitFor();
    assert.equal(await page.locator('input:visible').count(),0);assert.equal(await page.locator('#send-cad').isDisabled(),true);assert.deepEqual(errors,[]);await context.close();
  });
  await check('hidden clipboard has no automatic OS read/write or outgoing transfer',async()=>{
    const {page,context}=await open({authenticated:true,policy:{textToRemote:true,textToLocal:true}});await connected(page);
    await page.evaluate(()=>{window.fixtureWrites=0;Object.defineProperty(navigator,'clipboard',{value:{writeText:async()=>{window.fixtureWrites++;}},configurable:true});});
    const incoming=Buffer.from('synthetic clipboard fixture','latin1');const packet=Buffer.alloc(8+incoming.length);packet[0]=3;packet.writeUInt32BE(incoming.length,4);incoming.copy(packet,8);connections[0].send(packet);
    await page.waitForFunction(()=>!document.getElementById('clipboard-copy').disabled);assert.equal(await page.evaluate(()=>window.fixtureWrites),0);
    assert.equal(await page.locator('#clipboard-open').isVisible(),false);assert.equal(await page.locator('#clipboard-copy').isVisible(),false);
    assert.equal(wire.filter(b=>b[0]===6).length,0);assert.equal(await page.evaluate(()=>window.fixtureWrites),0);await context.close();
  });
  await check('cleanup failure blocks a fresh lease; stale frame never marks a new connection ready',async()=>{
    const {page,context}=await open({authenticated:true,frames:[true,false]});await connected(page);await page.waitForFunction(()=>document.getElementById('connection-state').textContent.includes('화면 데이터 수신'));
    connections[0].close(1000,'<unsafe-private-reason>');await page.locator('#retry').waitFor({state:'visible'});assert(!await page.locator('body').textContent().then(t=>t.includes('unsafe-private')));
    fixture.cleanupError=true;await page.locator('#retry').click();await page.getByRole('heading',{name:'이전 연결 정리를 확인하지 못했어요'}).waitFor();assert.equal(requests.filter(r=>r.path.endsWith('/connect')).length,1);
    fixture.cleanupError=false;fixture.cleanupStatus=404;fixture.cleanupCode='not_found';await page.locator('#retry').click();await page.getByRole('heading',{name:'이전 연결 정리를 확인하지 못했어요'}).waitFor();assert.equal(requests.filter(r=>r.path.endsWith('/connect')).length,1);
    fixture.cleanupCode='console_session_not_found';await page.locator('#retry').click();await connected(page);await page.waitForTimeout(1300);assert.equal(await page.locator('#connection-state').textContent(),'콘솔 연결됨');
    await page.locator('#frame-note').waitFor({state:'visible',timeout:12000});assert.match(await page.locator('#frame-note').textContent(),/아직 안 보여요/);assert.equal(connections.length,2);await context.close();
  });
  await check('invalid desktop route never starts API or websocket authorization',async()=>{
    const {page,context}=await open({},'/console/bad/route');await page.getByRole('heading',{name:'올바른 VM 주소가 아니에요'}).waitFor();assert.equal(requests.length,0);assert.equal(connections.length,0);await context.close();
  });
  await check('logout remains authoritative after VM cleanup failure and never clears auth on logout failure',async()=>{
    const {page,context}=await open({authenticated:true});await connected(page);connections[0].close(1000,'transport_closed');await page.locator('#retry').waitFor({state:'visible'});fixture.cleanupError=true;fixture.logoutError=true;
    await page.locator('#logout').click();await page.getByRole('heading',{name:'로그아웃하지 못했어요'}).waitFor();assert.equal(await page.locator('#login-panel').isHidden(),true);assert.equal(await page.locator('#logout').isVisible(),true);assert.equal(fixture.authenticated,true);assert.equal(requests.filter(r=>r.path.endsWith('/logout')).length,1);
    fixture.logoutError=false;await page.locator('#logout').click();await page.locator('#login-panel').waitFor({state:'visible'});assert.equal(fixture.authenticated,false);assert.equal(requests.filter(r=>r.path.endsWith('/logout')).length,2);await context.close();
  });
  await check('expired login stays explicit and never starts a socket',async()=>{
    const {page,context}=await open({authenticated:true,connectUnauthorized:true});await page.locator('#login-panel').waitFor({state:'visible'});
    assert.equal(await page.locator('#connection-state').textContent(),'로그인이 만료됐어요');assert.equal(connections.length,0);assert.equal(await page.locator('.toolbar').isVisible(),true);await context.close();
  });
  await check('connected canvas fills the viewport with only the tool grip; auth/errors remain readable',async()=>{
    for(const [width,height,scale,theme]of [[1920,1080,1,'dark'],[1366,768,1.5,'light'],[960,640,2,'dark'],[360,640,1.5,'light']]){
      const {page,context,errors}=await open();await page.setViewportSize({width,height});await page.evaluate(({scale,theme})=>{document.documentElement.style.fontSize=`${16*scale}px`;document.documentElement.dataset.theme=theme;},{scale,theme});await page.locator('#login-panel').waitFor({state:'visible'});
      await page.locator('#login-submit').scrollIntoViewIfNeeded();assert(await page.locator('#login-submit').isVisible());
      await page.locator('#login-panel').evaluate(e=>{e.scrollTop=0;});
      assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
      let name=`${width}x${height}-${theme}-${scale*100}-login.png`;await page.screenshot({path:resolve(output,name)});report.screenshots.push({name,width,height,textScale:scale,theme,synthetic:true});
      await page.locator('#username').fill('fixture-user');await page.locator('#password').fill('fixture-password');await page.locator('#login-submit').click();await connected(page);
      await observeCanvas(page,'connected-before-paint-gate');
      await painted(page);
      await observeCanvas(page,'known-raw-frame-pixel');
      name=`${width}x${height}-${theme}-${scale*100}-console.png`;await page.screenshot({path:resolve(output,name)});report.screenshots.push({name,width,height,textScale:scale,theme,synthetic:true});
      await observeCanvas(page,'after-png-capture');
      assert.equal(await page.locator('button:visible').count(),1);assert.equal(await page.locator('#toolbar-grip').isVisible(),true);assert.equal(await page.locator('.toolbar').isVisible(),false);
      const workspace=await page.locator('#workspace').boundingBox();assert(workspace&&workspace.x===0&&workspace.y===0&&Math.abs(workspace.width-width)<1&&Math.abs(workspace.height-height)<1);
      const canvas=await page.locator('#remote canvas').boundingBox();assert(canvas&&canvas.x>=-1&&canvas.y>=-1&&canvas.x+canvas.width<=width+1&&canvas.y+canvas.height<=height+1);
      assert(Math.min(Math.abs(canvas.width-width),Math.abs(canvas.height-height))<=2);assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
      connections[0].close(1000,'console_permission_denied');await page.getByRole('heading',{name:'콘솔에 연결할 권한이 없어요'}).waitFor();
      assert.equal(await page.locator('.toolbar').isVisible(),true);assert.equal(await page.locator('#retry').isVisible(),true);assert.match(await page.locator('#notice-text').textContent(),/관리자/);
      name=`${width}x${height}-${theme}-${scale*100}-permission-error.png`;await page.screenshot({path:resolve(output,name)});report.screenshots.push({name,width,height,textScale:scale,theme,synthetic:true});
      assert.deepEqual(errors,[]);await context.close();
    }
  });
  const visible=(page,id)=>page.locator('#'+id).isVisible();
  await check('the app window shows only app tools; a browser tab keeps logout, new window and theme',async()=>{
    const before=await open({authenticated:true,embedded:true,connectError:'console_display_state_missing'});
    await before.page.getByRole('heading',{name:'원래 화면의 복구 정보가 없어요'}).waitFor();
    for(const id of ['logout','new-window','theme'])assert.equal(await visible(before.page,id),false,id);
    assert.equal(await before.page.locator('.brand').isVisible(),false,'no second wordmark under the app title');assert.equal(await visible(before.page,'help-open'),true);await before.context.close();
    // 전체 화면 only where the window fills the screen: a browser tab, or the app window with its bridge.
    for(const [embedded,bridge] of [[true,false],[true,true],[false,false]]){
      const {page,context,errors}=await open({authenticated:true,embedded,bridge});await connected(page);
      assert.equal(await page.locator('.toolbar').isVisible(),false);
      await page.locator('#toolbar-grip').click();await page.locator('.toolbar').waitFor({state:'visible'});
      assert.equal(await page.locator('#toolbar-grip').getAttribute('aria-expanded'),'true');
      for(const id of ['reconnect','send-cad','help-open','toolbar-fold'])assert.equal(await visible(page,id),true,id);
      assert.equal(await visible(page,'fullscreen'),!embedded||bridge,'fullscreen');
      assert.equal(await visible(page,'clipboard-open'),false,'clipboard only where a direction is allowed');
      for(const id of ['logout','new-window','theme'])assert.equal(await visible(page,id),!embedded,id);
      for(const id of ['reconnect','send-cad','fullscreen','help-open'])assert.ok((await page.locator('#'+id).getAttribute('aria-label')).length>0,id);
      assert.deepEqual(errors,[]);await context.close();
    }
    const allowed=await open({authenticated:true,embedded:true,policy:{textToRemote:true,textToLocal:false}});await connected(allowed.page);
    await allowed.page.locator('#toolbar-grip').click();assert.equal(await visible(allowed.page,'clipboard-open'),true);await allowed.context.close();
  });
  await check('a failure card has one action line, retry inside the card and the code behind 자세히',async()=>{
    const {page,context,errors}=await open({authenticated:true,embedded:true,connectError:'console_display_state_missing'});
    await page.getByRole('heading',{name:'원래 화면의 복구 정보가 없어요'}).waitFor();
    assert.equal(await page.locator('#notice').getAttribute('data-tone'),'error');assert.equal(await page.locator('#notice #retry').isVisible(),true);
    assert.equal(await page.locator('#notice-text').textContent(),'관리자에게 VM 화면 상태 확인을 요청하세요.');
    assert.equal(await visible(page,'notice-more'),true);assert.equal(await visible(page,'notice-detail'),false,'detail stays folded');
    await page.locator('#notice-more summary').click();
    assert.match(await page.locator('#notice-detail').textContent(),/작업은 그대로/);assert.match(await page.locator('#notice-code').textContent(),/console_display_state_missing/);
    fixture.connectError=null;await page.locator('#retry').click();await connected(page);assert.equal(requests.filter(r=>r.path.endsWith('/connect')).length,2);
    connections[0].close(1000,'transport_closed');await page.getByRole('heading',{name:'콘솔 연결이 끝났어요'}).waitFor();
    assert.equal(await page.locator('#notice').getAttribute('data-tone'),'ended');assert.equal(await visible(page,'notice-more'),false,'a plain end has no code');assert.equal(await visible(page,'retry'),true);
    fixture.connectError='<unsafe-server-text>';await page.locator('#retry').click();await page.getByRole('heading',{name:'연결하지 못했어요'}).waitFor();
    assert.equal(await visible(page,'notice-more'),false);assert(!(await page.locator('body').textContent()).includes('unsafe-server'));
    assert.deepEqual(errors,[]);await context.close();
  });
  await check('the open tool bar folds after a command, on Escape, on a press into the picture and with its fold button',async()=>{
    const {page,context,errors}=await open({authenticated:true,embedded:true});await connected(page);await painted(page);
    const expand=async()=>{await page.locator('#toolbar-grip').click();await page.locator('.toolbar').waitFor({state:'visible'});};
    const folded=()=>page.locator('.toolbar').waitFor({state:'hidden'});
    await expand();await page.keyboard.press('Escape');await folded();assert.equal(await page.locator('#toolbar-grip').getAttribute('aria-expanded'),'false');
    await expand();wire.length=0;await page.locator('#send-cad').click();await folded();
    await until(()=>wire.filter(b=>b[0]===4).length>=6);assert.deepEqual(wire.filter(b=>b[0]===4).map(b=>b[1]),[1,1,1,0,0,0],'Ctrl, Alt, Del down then up');
    await expand();await page.locator('#remote canvas').click({position:{x:40,y:300}});await folded();
    await expand();await page.locator('#toolbar-fold').click();await folded();
    await expand();await page.locator('#help-open').click();await page.locator('#help-dialog').waitFor({state:'visible'});await folded();
    assert.equal(await page.locator('#help-dialog .app-only').isVisible(),true);assert.equal(await page.locator('#help-dialog .browser-only').isVisible(),false);
    assert.equal(connections.length,1);assert.deepEqual(errors,[]);await context.close();
  });
  await check('the app close request shows the page question and answers only with fixed strings',async()=>{
    const {page,context,errors}=await open({authenticated:true,embedded:true,bridge:true});await connected(page);
    const posted=()=>page.evaluate(()=>window.__posted);
    assert.deepEqual(await posted(),['multitap-console:ready']);
    for(const data of ['multitap-console:close','close-request','multitap-console:close-request ',{type:'multitap-console:close-request'},null])await page.evaluate(data=>window.__host({data}),data);
    assert.equal(await visible(page,'close-dialog'),false);assert.deepEqual(await posted(),['multitap-console:ready'],'anything but the one request is ignored');
    const ask=()=>page.evaluate(()=>window.__host({data:'multitap-console:close-request'}));
    await ask();await page.locator('#close-dialog').waitFor({state:'visible'});
    assert.equal(await page.getByRole('heading',{name:'콘솔을 닫을까요?'}).isVisible(),true);assert.equal(await page.locator('#close-text').textContent(),'VM과 작업은 그대로 유지돼요.');
    assert.equal(await page.evaluate(()=>document.activeElement.id),'close-confirm');
    await page.locator('#close-cancel').click();await page.locator('#close-dialog').waitFor({state:'hidden'});
    await ask();await page.locator('#close-dialog').waitFor({state:'visible'});await page.keyboard.press('Escape');await page.locator('#close-dialog').waitFor({state:'hidden'});
    await ask();await ask();await page.locator('#close-confirm').click();
    assert.deepEqual(await posted(),['multitap-console:ready','multitap-console:close-shown','multitap-console:close-cancel','multitap-console:close-shown','multitap-console:close-cancel','multitap-console:close-shown','multitap-console:close-shown','multitap-console:close']);
    assert.equal(connections[0].readyState,1,'the page does not end the connection itself; closing the window does');
    assert.deepEqual(errors,[]);await context.close();
    const tab=await open({authenticated:true,bridge:true});await connected(tab.page);
    assert.deepEqual(await tab.page.evaluate(()=>window.__posted),[],'a browser tab never speaks to an app window');await tab.context.close();
  });
  report.result='pass';
}catch(error){report.result='fail';report.error=String(error.stack||error);throw error;}
finally{
  await browser.close();for(const socket of wss.clients)socket.terminate();await new Promise(r=>wss.close(r));await new Promise(r=>server.close(r));
  const after=Object.fromEntries(await Promise.all(manifest.files.map(async row=>[row.path,createHash('sha256').update(await readFile(resolve(assets,row.path))).digest('hex')])));
  report.fixtureSourceSha256=createHash('sha256').update(await readFile(fileURLToPath(import.meta.url))).digest('hex');
  report.fixtureSourceUnchanged=fixtureBefore===report.fixtureSourceSha256;
  report.manifestSha256=createHash('sha256').update(await readFile(resolve(assets,'assets-manifest.json'))).digest('hex');
  report.manifestUnchanged=manifestBefore===report.manifestSha256;
  report.sourceUnchanged=JSON.stringify(before)===JSON.stringify(after);report.sourceHashes=after;report.finished=new Date().toISOString();
  await writeFile(resolve(output,'receipt.json'),JSON.stringify(report,null,2)+'\n');assert.equal(report.sourceUnchanged,true);assert.equal(report.fixtureSourceUnchanged,true);assert.equal(report.manifestUnchanged,true);
}

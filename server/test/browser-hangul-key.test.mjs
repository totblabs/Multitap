import test from 'node:test';
import assert from 'node:assert/strict';
import {createHangulKeyAdapter} from '../admin/console/hangul-key.js';

function fixture(platform='Win32') {
  const sent=[], canvas={tagName:'CANVAS',ownerDocument:{}};
  canvas.ownerDocument.activeElement=canvas;
  const rfb={viewOnly:false,sendKey:(...args)=>sent.push(args)};
  let active=true;
  const adapter=createHangulKeyAdapter({getRfb:()=>rfb,canSend:()=>active,platform});
  const event=(type,extra={})=>({type,key:'HangulMode',code:'Lang1',keyCode:0x15,isTrusted:true,target:canvas,repeat:false,
    prevented:false,stopped:false,preventDefault(){this.prevented=true;},stopImmediatePropagation(){this.stopped=true;},...extra});
  return {sent,canvas,rfb,adapter,event,setActive:value=>active=value};
}
test('Windows identified orphan Korean release emits one public Lang1 tap',()=>{
  const f=fixture(),e=f.event('keyup'); assert.equal(f.adapter.handle(e),true);
  assert.deepEqual(f.sent,[[0xff31,'Lang1']]); assert.equal(e.prevented,true); assert.equal(e.stopped,true);
  f.adapter.handle(f.event('keyup')); assert.equal(f.sent.length,2);
});
test('paired events, fresh down-only presses and held repeats keep the original vendor path',()=>{
  const f=fixture();
  for(const type of ['keydown','keydown','keyup'])assert.equal(f.adapter.handle(f.event(type)),false);
  assert.deepEqual(f.sent,[]);
  assert.equal(f.adapter.handle(f.event('keydown',{repeat:true})),false);
  assert.equal(f.adapter.handle(f.event('keyup')),false);assert.equal(f.sent.length,0);
});
test('lifecycle reset sends no fake key and never discards the first new orphan release',()=>{
  const f=fixture();f.adapter.handle(f.event('keydown'));f.adapter.reset();f.adapter.reset();
  f.adapter.handle(f.event('keyup'));assert.equal(f.sent.length,1);
  f.adapter.handle(f.event('keyup'));assert.equal(f.sent.length,2);
});
test('physical Lang1 observations preserve a mixed Process down and identified release on the vendor path',()=>{
  const f=fixture();
  assert.equal(f.adapter.handle(f.event('keydown',{key:'Process',keyCode:229,isComposing:true})),false);
  assert.equal(f.adapter.handle(f.event('keyup')),false);assert.deepEqual(f.sent,[]);
  assert.equal(f.adapter.handle(f.event('keyup')),true);assert.deepEqual(f.sent,[[0xff31,'Lang1']]);
  f.adapter.handle(f.event('keydown',{key:'Process',keyCode:229,isComposing:true}));
  assert.equal(f.adapter.handle(f.event('keyup',{key:'Process',keyCode:229,isComposing:true})),false);
  assert.equal(f.adapter.handle(f.event('keyup')),true);assert.equal(f.sent.length,2);
});
test('missing-code identified VK_HANGUL and unidentified Lang1 retain physical identity',()=>{
  const f=fixture();for(const extra of [{key:'Unidentified',code:'',keyCode:0x15},{key:'Unidentified',code:'Lang1',keyCode:0},{key:'HangulMode',code:'AltRight',keyCode:0x15}])assert.equal(f.adapter.handle(f.event('keyup',extra)),true);
  assert.deepEqual(f.sent,Array.from({length:3},()=>[0xff31,'Lang1']));
});
test('ordinary Unicode/composition, Alt/Ctrl/F2/Hanja/Kana and unrelated releases pass untouched',()=>{
  const f=fixture();for(const extra of [{key:'한',code:'KeyA',keyCode:229,isComposing:true},{key:'Process',code:'',keyCode:229},{key:'Process',code:'Lang1',keyCode:229,isComposing:true},{key:'Alt',code:'AltRight',keyCode:18},{key:'Control',code:'ControlLeft',keyCode:17},{key:'F2',code:'F2',keyCode:113},{key:'HanjaMode',code:'Lang2',keyCode:25},{key:'KanaMode',code:'Lang1',keyCode:21},{key:'a',code:'KeyA',keyCode:21}]){
    const e=f.event('keyup',extra);assert.equal(f.adapter.handle(e),false);assert.equal(e.prevented,false);
  }assert.equal(f.sent.length,0);
});
test('login fields, unfocused canvas, synthetic lifecycle events, non-Windows and viewOnly do not repair',()=>{
  for(const extra of [{target:{tagName:'INPUT'}},{isTrusted:false},{type:'keypress'}]){const f=fixture();assert.equal(f.adapter.handle(f.event('keyup',extra)),false);assert.equal(f.sent.length,0);}
  const f=fixture();f.canvas.ownerDocument.activeElement=null;assert.equal(f.adapter.handle(f.event('keyup')),false);
  const linux=fixture('Linux x86_64');assert.equal(linux.adapter.handle(linux.event('keyup')),false);
  const view=fixture();view.rfb.viewOnly=true;assert.equal(view.adapter.handle(view.event('keyup')),false);
  const paused=fixture();paused.setActive(false);assert.equal(paused.adapter.handle(paused.event('keyup')),false);
});

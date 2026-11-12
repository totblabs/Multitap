import test from 'node:test';
import assert from 'node:assert/strict';
import {parseHTML} from 'linkedom';
import {hostEntries,logBoard,entryFacts} from '../admin/js/timeline.js';

const restore = (overrides = {}) => ({action:280,stage:3,active:true,ok:true,format:1,cause:0,error:0,owned:true,pending:false,...overrides});
const geometry = (stage, overrides = {}) => ({action:280,stage,items:0,monitors:1,owned:true,ok:true,active:true,changed:true,width:1920,height:1080,expected:0,missing:0,recovery:false,stale:false,...overrides});
const entries = values => hostEntries([{desktopId:'fixture',diagnostics:{version:'1.0.12',events:values.map(value=>({time:'2026-11-04T09:42:43.406Z',event:'hostAction',values:Object.freeze(value)}))}}]);

test('actual display restoration fields cannot become thumbnail privacy or busy messages',()=>{
  const values=[restore(),restore({format:2}),restore({format:3,ok:false,cause:8,error:87,pending:true}),{action:280,stage:5,ok:false,cause:1}];
  const original=JSON.stringify(values),rows=entries(values);
  assert.match(rows[0].title,/화면 복원/);assert.match(rows[0].detail.join(' '),/전체 화면.*복원/);
  assert.match(rows[1].detail.join(' '),/콘솔만/);assert.match(rows[1].detail.join(' '),/전체.*확인/);
  assert.equal(rows[2].failure,true);assert.match(rows[2].detail.join(' '),/87/);
  assert.match(rows[3].detail.join(' '),/기록.*미수신/);assert.equal(rows[3].failure,false,'missing diagnostic is not an observed restore failure');
  for(const row of rows)assert.doesNotMatch(row.detail.join(' '),/잠겨|로그인 화면|너무 자주|찍지/);
  assert.equal(JSON.stringify(values),original);
});

test('restore owned and pending describe state-file evidence, never device ownership or queued work',()=>{
  const rows=entries([restore({pending:true}),restore({owned:false}),restore({active:false,ok:false,format:0,owned:false})]);
  assert.match(rows[0].detail.join(' '),/상태 파일.*남/);
  assert.match(rows[1].detail.join(' '),/상태 파일.*미확인/);
  assert.match(rows[2].detail.join(' '),/결과.*미확인/);
  for(const row of rows){assert.equal(row.failure,false);assert.doesNotMatch(row.detail.join(' '),/장치 소유|작업 대기|연결 준비됨|전체 화면 구성을 복원했다고/);}
});

test('before saved and after geometry are observations, including inactive and unknown outputs',()=>{
  const rows=entries([geometry(1),geometry(2),geometry(4,{ok:false,active:false,width:0,height:0}),geometry(1,{items:2,monitors:0,owned:false,ok:false,active:false,changed:false,width:0,height:0})]);
  assert.match(rows[0].detail.join(' '),/변경 전/);assert.match(rows[1].detail.join(' '),/저장된/);assert.match(rows[2].detail.join(' '),/복원 후/);
  assert.match(rows[0].detail.join(' '),/1920.*1080/);assert.match(rows[2].detail.join(' '),/비활성/);
  for(const row of rows){assert.equal(row.failure,false);assert.doesNotMatch(row.title,/미리보기/);}
});

test('exact thumbnail shape retains locked and sign-in privacy without a capture attempt claim',()=>{
  const rows=entries([0,1,2,3,4,5].map(stage=>({action:280,stage,ok:stage!==0,kib:stage===1?12:0})));
  assert.match(rows[3].detail.join(' '),/잠겨 있어 찍지 않음/);assert.match(rows[4].detail.join(' '),/로그인 화면이라 찍지 않음/);
  assert.equal(rows[0].failure,true);assert.equal(rows[3].failure,false);assert.match(rows[1].detail.join(' '),/12/);
  assert.ok(rows.every(row=>/미리보기/.test(row.title)));
});

test('mixed incomplete contradictory and unknown action280 records stay neutral with raw evidence',()=>{
  const values=[{action:280,stage:3,ok:false},restore({kib:0}),{action:280,stage:5,ok:false,cause:1,kib:0},geometry(1,{error:87}),restore({active:false}),restore({owned:false,pending:true}),{action:280,stage:99,ok:false,kib:0},restore({format:99}),restore({stage:'3'})];
  for(const row of entries(values)){
    assert.match(row.title,/형식 미확인/);assert.equal(row.failure,false);
    assert.match(row.rawDiagnostic,/진단 원본/);assert.doesNotMatch(row.detail.join(' '),/잠겨|너무 자주|복원 완료|현재 연결 준비/);
  }
});

test('timeline keeps display failure highlighting and original numbers behind closed raw details',()=>{
  const previous={document:globalThis.document,Node:globalThis.Node};
  const dom=parseHTML('<html><body></body></html>');globalThis.document=dom.document;globalThis.Node=dom.Node;
  try{
    const row=entries([restore({format:3,ok:false,cause:8,error:87,pending:true})])[0];
    const ctx={state:{ui:{}},render(){},desktopById:()=>null,userByName:()=>null,link:text=>text};
    const node=[logBoard(ctx,[row])].flat()[0].querySelector('tbody tr[data-entry]');
    assert.ok(node.classList.contains('is-failure'));assert.match(node.querySelector('td.col-cause').textContent,/복원 실패/);
    assert.doesNotMatch(node.textContent,/owned=|pending=/,'the original numbers are not in the row');
    const facts=Object.fromEntries([...entryFacts(ctx,row).querySelectorAll('.facts>div')].map(div=>[div.querySelector('dt').textContent,div.querySelector('dd').textContent]));
    assert.doesNotMatch(facts['내용'],/owned=|pending=/);
    assert.match(facts['진단 원본'],/error=87.*owned=true.*pending=true/);
  }finally{for(const [key,value]of Object.entries(previous)){if(value===undefined)delete globalThis[key];else globalThis[key]=value;}}
});

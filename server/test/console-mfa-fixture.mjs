// console.js text comes from console/i18n.js (docs/i18n.md); its imports are removed below, so the context gets the same functions.
import { t, translatePage } from '../admin/console/i18n.js';
import {createAccountSecurityController} from '../admin/js/account-security-controller.js';
import {createAccountSecurityView} from '../admin/js/account-security-view.js';
import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {runInNewContext} from 'node:vm';
import {parseHTML} from 'linkedom';
import {createConsoleAuthentication,normalizeConsoleRecoveryCode} from '../admin/console/console-auth.js';
const html=await readFile(new URL('../admin/console/index.html',import.meta.url),'utf8');
const script=(await readFile(new URL('../admin/console/console.js',import.meta.url),'utf8')).replace(/^import .+;\r?\n/gm,'').replace(/\bboot\(\);\s*$/,'')
  // Browser-only account security loads lazily; hand it the same modules.
  .replace(/import\('\/admin\/js\/account-security-controller\.js'\),import\('\/admin\/js\/account-security-view\.js'\),/,'{createAccountSecurityController},{createAccountSecurityView},');
const tick=()=>new Promise(resolve=>setImmediate(resolve));
const challengeToken='M'.repeat(43),mfaCsrf='P'.repeat(43),sessionCsrf='S'.repeat(43),confirmationToken='C'.repeat(43);
const session={status:200,body:{csrfToken:sessionCsrf,user:{username:'fixture-user'}}};
const prepare={status:202,body:{code:'console_preparing'}};
const question=(details={})=>({status:409,body:{error:{code:'login_confirmation_required',details:{protocol:'session-access-v1',challengeToken:confirmationToken,expiresInMs:120000,...details}}}});
function fixture(responses){
  const {document,window:dom}=parseHTML(html),window=new EventTarget();
  const requests=[],timers=new Map();let sequence=0,wall=Date.now(),mono=0;
  for(const node of document.querySelectorAll('dialog')){node.open=false;node.showModal=()=>{node.open=true;};node.close=()=>{node.open=false;node.dispatchEvent(new dom.Event('close'));};}
  const schedule=fn=>{const id=++sequence;timers.set(id,fn);return id;},cancel=id=>timers.delete(id);
  const fetch=async(path,options)=>{
    requests.push({path,...options,body:options.body?JSON.parse(options.body):undefined});
    const next=responses.shift();assert.ok(next,'Unexpected request: '+path);
    const response=typeof next==='function'?await next():next;
    return {status:response.status,ok:response.status>=200&&response.status<300,json:async()=>response.body};
  };
  const context={createAccountSecurityController,createAccountSecurityView,document,window,location:{pathname:'/console/pve-170',origin:'https://fixture.invalid',protocol:'https:',hostname:'fixture.invalid'},navigator:{userAgent:'Fixture browser'},
    matchMedia:()=>({matches:false,addEventListener(){}}),fetch,AbortSignal,URL,initLogging(){},releaseCapture(){},createHangulKeyAdapter:()=>({reset(){}}),t,translatePage,setTimeout:schedule,clearTimeout:cancel,
    createConsoleAuthentication:options=>createConsoleAuthentication({...options,now:()=>wall,monotonic:()=>mono,schedule,cancel})};
  runInNewContext(script,context);
  const $=id=>document.getElementById(id);
  return {document,$,requests,timers,window,dom,
    password(){ $('username').value='fixture-user';$('password').value='synthetic-password';$('login-form').dispatchEvent(new dom.Event('submit',{cancelable:true})); },
    submit(code,{recovery=false}={}){ $('mfa-recovery').checked=recovery;$('mfa-code').value=code;$('mfa-form').dispatchEvent(new dom.Event('submit',{cancelable:true})); },
    click(id){$(id).dispatchEvent(new dom.Event('click',{cancelable:true}));},
    challenge(extra={}){return {status:202,body:{mfa:{protocol:'mfa-v1',challengeToken,expiresAt:new Date(wall+120000).toISOString(),methods:['totp','recovery'],...extra},csrfToken:mfaCsrf}};},
    advance(ms){wall+=ms;mono+=ms;},
    cleanup(){window.dispatchEvent(new Event('pagehide'));}
  };
}

export {fixture,tick,session,prepare};

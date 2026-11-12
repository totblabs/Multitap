// Authentication presentation only. Server challenges, cookies and authority
// remain server-owned. Secrets stay in this document's pending operation.
import { t } from './i18n.js';
const token = value => typeof value === 'string' && /^[A-Za-z0-9_-]{43}$/.test(value);
const keys = (value, expected) => value && typeof value === 'object' && !Array.isArray(value) &&
  Object.keys(value).length === expected.length && expected.every(key => Object.hasOwn(value,key));
export function normalizeConsoleRecoveryCode(value) {
  if(typeof value !== 'string') return null;
  if(value.length===32&&/^[a-fA-F0-9]{32}$/.test(value)) return value.toLowerCase();
  if(value.length===39&&/^(?:[a-fA-F0-9]{4}-){7}[a-fA-F0-9]{4}$/.test(value)) return value.replaceAll('-','').toLowerCase();
  return null;
}
const descriptions = {
  mfa_invalid_code:t('console.console-auth.1'),
  invalid_mfa_fields:t('console.console-auth.2'),
  too_many_attempts:t('console.console-auth.3'),
  mfa_challenge_expired:t('console.console-auth.4'),
  mfa_client_update_required:t('console.console-auth.5'),
  mfa_requires_https:t('console.console-auth.6'),
  mfa_unavailable:t('console.console-auth.7'),
  console_mfa_unavailable:t('console.console-auth.7'),
  csrf_required:t('console.console-auth.8'),
  login_confirmation_changed:t('console.console-auth.9'),
  login_confirmation_expired:t('console.console-auth.10'),
};

export function createConsoleAuthentication({document,request,begin,current,setBusy,confirmExisting,
  onSession,onPassword,onError,discardLateSession=()=>{},origin,now=Date.now,
  monotonic=()=>globalThis.performance?.now?.() ?? Date.now(),schedule=setTimeout,cancel=clearTimeout}) {
  const $=id=>document.getElementById(id);
  let pending=null,timer=null,closed=false;
  const own=(generation)=>!closed&&current(generation);
  const error=(code,mfa=false)=>{
    const text=descriptions[code];
    if(text){const node=$(mfa?'mfa-message':'login-message');node.textContent=text;node.hidden=false;}
    else onError(code,mfa);
  };
  function clearPending(){
    cancel(timer);timer=null;
    if(pending){pending.challengeToken='';pending.csrfToken='';pending=null;}
    $('mfa-code').value='';$('mfa-message').textContent='';$('mfa-message').hidden=true;
  }
  function passwordView(){
    $('mfa-form').hidden=true;$('login-form').hidden=false;
    $('login-heading').textContent=t('console.console-auth.11');
    $('login-intro').textContent=t('console.console-auth.12');
  }
  function retire(){clearPending();passwordView();}
  function returnToPassword(message){retire();onPassword(message);}
  function remaining(value){const wall=now(),mono=monotonic();if(wall<value.at||mono<value.mono)return 0;return Math.max(0,Math.min(value.until-wall,value.monoUntil-mono));}
  function updateClock(){
    const value=pending;if(!value||!own(value.generation))return;
    const left=remaining(value);
    if(left<=0){returnToPassword(descriptions.mfa_challenge_expired);setBusy(false);return;}
    const seconds=Math.ceil(left/1000);
    $('mfa-remaining').textContent=t('console.console-auth.13', { count: Math.floor(seconds/60), String: String(seconds%60).padStart(2,'0') });
    timer=schedule(updateClock,1000);
  }
  function mode(){
    const recovery=$('mfa-recovery').checked;
    $('mfa-code').value='';$('mfa-message').hidden=true;
    $('mfa-code-label').textContent=recovery?t('console.console-auth.14'):t('console.console-auth.15');
    $('mfa-code').type=recovery?'password':'text';
    $('mfa-code').inputMode=recovery?'text':'numeric';
    $('mfa-code').maxLength=recovery?39:6;
    $('mfa-code').autocomplete=recovery?'off':'one-time-code';
    $('mfa-code').placeholder=recovery?'': t('console.console-auth.16');
  }
  function receiveChallenge(data,generation){
    if(!keys(data,['mfa','csrfToken'])||!keys(data.mfa,['protocol','challengeToken','expiresAt','methods'])||
       data.mfa.protocol!=='mfa-v1'||!token(data.mfa.challengeToken)||!token(data.csrfToken)||
       typeof data.mfa.expiresAt!=='string'||!Array.isArray(data.mfa.methods)||data.mfa.methods.length<1||data.mfa.methods.length>2||
       new Set(data.mfa.methods).size!==data.mfa.methods.length||data.mfa.methods.some(value=>!['totp','recovery'].includes(value))) throw {code:'invalid_response'};
    if(new URL(origin).protocol!=='https:')throw {code:'mfa_requires_https'};
    const at=now(),mono=monotonic(),until=Date.parse(data.mfa.expiresAt),left=until-at;
    if(!Number.isFinite(until)||left<=0||left>120000)throw {code:'invalid_response'};
    clearPending();pending={challengeToken:data.mfa.challengeToken,csrfToken:data.csrfToken,generation,at,mono,until,monoUntil:mono+left};
    $('password').value='';$('login-message').hidden=true;
    $('login-form').hidden=true;$('mfa-form').hidden=false;
    $('login-heading').textContent=t('console.console-auth.17');$('login-intro').textContent=t('console.console-auth.18');
    $('mfa-recovery').checked=!data.mfa.methods.includes('totp');
    $('mfa-mode').hidden=data.mfa.methods.length===1;
    mode();updateClock();$('mfa-code').focus();
  }
  async function admit(response,generation){
    if(!own(generation)){
      if(response?.status===200)await discardLateSession(response.data);
      return;
    }
    if(response?.status!==200)throw {code:'invalid_response'};
    await onSession(response.data,generation);
  }
  async function consent(failure,generation,recovery){
    // A definite successful factor stage cannot be reused, even if its new
    // consent details are malformed or the person later cancels consent.
    clearPending();passwordView();
    const details=failure.details;
    try {
      if(!await confirmExisting(details,generation,recovery)){
        if(own(generation))returnToPassword(recovery?t('console.console-auth.19'):undefined);
        return null;
      }
      if(!own(generation))return null;
      return await request('/login/confirm','POST',{challengeToken:details.challengeToken,confirm:true});
    } finally {if(details&&typeof details==='object')details.challengeToken='';}
  }
  async function passwordSubmit(event){
    event.preventDefault();if(closed||!$('mfa-form').hidden||$('login-submit').disabled)return;
    retire();const generation=begin();setBusy(true);
    let password=$('password').value;$('password').value='';
    try{
      let response;
      try{response=await request('/login','POST',{username:$('username').value,password},{headers:{'X-Multitap-Auth-Protocol':'mfa-v1'}});}
      catch(failure){
        if(!own(generation))return;
        if(failure.status!==409||failure.code!=='login_confirmation_required')throw failure;
        $('password').value='';password='';response=await consent(failure,generation,false);if(!response)return;
      }
      if(response.status===202){if(own(generation))receiveChallenge(response.data,generation);return;}
      await admit(response,generation);
    }catch(failure){if(own(generation)){passwordView();error(failure.code);}}
    finally{password='';if(own(generation)){$('password').value='';setBusy(false);}}
  }
  async function mfaSubmit(event){
    event.preventDefault();const value=pending;
    if(!value||!own(value.generation)||$('mfa-submit').disabled)return;
    if(remaining(value)<=0){returnToPassword(descriptions.mfa_challenge_expired);return;}
    const recovery=$('mfa-recovery').checked;
    let proof=recovery?$('mfa-code').value:$('mfa-code').value.trim();
    if(recovery){proof=normalizeConsoleRecoveryCode(proof);if(!proof){error('invalid_mfa_fields',true);return;}}
    else if(!/^[0-9]{6}$/.test(proof)){error('invalid_mfa_fields',true);return;}
    const generation=value.generation;
    setBusy(true);$('mfa-code').value='';$('mfa-message').hidden=true;
    try{
      let response;
      try{response=await request('/mfa/verify','POST',{challengeToken:value.challengeToken,[recovery?'recoveryCode':'code']:proof},{headers:{'X-Multitap-Console-CSRF':value.csrfToken}});}
      catch(failure){
        if(!own(generation))return;
        if(failure.status===409&&failure.code==='login_confirmation_required'){
          response=await consent(failure,generation,recovery);if(!response)return;
        }else throw failure;
      }
      if(own(generation))clearPending();
      await admit(response,generation);
    }catch(failure){
      if(!own(generation))return;
      if(['mfa_invalid_code','invalid_mfa_fields','too_many_attempts'].includes(failure.code)&&pending){error(failure.code,true);}
      else{
        returnToPassword(descriptions[failure.code]||t('console.console-auth.20'));
      }
    }finally{proof='';if(own(generation))setBusy(false);}
  }
  const cancelled=()=>{if(closed)return;returnToPassword(t('console.console-auth.21'));setBusy(false);};
  $('login-form').addEventListener('submit',passwordSubmit);
  $('mfa-form').addEventListener('submit',mfaSubmit);
  $('mfa-recovery').addEventListener('change',mode);
  $('mfa-cancel').addEventListener('click',cancelled);
  return {retire,destroy(){closed=true;retire();$('login-form').removeEventListener('submit',passwordSubmit);$('mfa-form').removeEventListener('submit',mfaSubmit);$('mfa-recovery').removeEventListener('change',mode);$('mfa-cancel').removeEventListener('click',cancelled);}};
}

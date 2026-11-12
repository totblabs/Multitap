import RFB from './vendor/novnc/1.7.0/core/rfb.js';
import {releaseCapture} from './vendor/novnc/1.7.0/core/util/events.js';
import {initLogging} from './vendor/novnc/1.7.0/core/util/logging.js';
import {createHangulKeyAdapter} from './hangul-key.js';
import {createConsoleAuthentication} from './console-auth.js';
import { t, translatePage } from './i18n.js';
// The static markup in the chosen language once, before anything below writes into it.
translatePage(document);

// Do not log remote clipboard, server names, credentials or transport messages.
initLogging('none');
const $ = id => document.getElementById(id);
// Presentation hint only; server authorization never trusts the user agent.
const embeddedConsole = /\bMultitap-ConsoleWebView\/1\b/.test(navigator.userAgent);
document.body.classList.add(embeddedConsole?'embedded':'browser');
// The Windows app's console window: it answers here (close question) and
// fills the screen for the page. Older app builds and Android have neither.
const appWindow = embeddedConsole&&typeof globalThis.chrome?.webview?.postMessage==='function'&&typeof globalThis.chrome.webview.addEventListener==='function'?globalThis.chrome.webview:null;
const fullscreenWorks = !embeddedConsole||!!appWindow;
// The app's console window serves only /console/ (console_webview_policy.h):
// static imports from /admin/js are refused there, the whole module would fail
// and the window would stay on '계정을 확인하고 있어요'. Account
// security is browser-only anyway, so its modules load only in a browser.
const accountSecurityModules = embeddedConsole ? Promise.resolve(null) : Promise.all([
  import('/admin/js/account-security-controller.js'),import('/admin/js/account-security-view.js'),
]).then(([controller,view])=>({createAccountSecurityController:controller.createAccountSecurityController,createAccountSecurityView:view.createAccountSecurityView}),()=>null);
const desktopId = /^\/console\/([a-zA-Z0-9][a-zA-Z0-9_-]{0,79})$/.exec(location.pathname)?.[1];
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
let csrf = null, rfb = null, socket = null, lease = null, busy = false, epoch = 0;
let loginConfirmation = null;
const ownDocument=document,ownOrigin=location.origin;
let authGeneration=0,authenticatedSession=null,accountSecurityController=null;
const authOwner=()=>({generation:authGeneration,session:authenticatedSession,csrf});
const authCurrent=owner=>document===ownDocument&&location.origin===ownOrigin&&owner.generation===authGeneration&&owner.session===authenticatedSession&&owner.csrf===csrf;
let policy = {textToRemote:false,textToLocal:false}, receivedClipboard = false;
let frameTimers = [], transportTimer = null, preparationTimer = null, pointer = null;
const heldButtons = new Set();
const heldKeys = new Map();
const hangulKey = createHangulKeyAdapter({getRfb:()=>rfb,canSend:()=>!busy&&!document.hidden&&document.hasFocus()&&document.body.classList.contains('console-connected')});
// [title, one action line, optional detail behind 자세히]. Only these fixed
// codes select text; server-provided text is never shown.
const display = t('console.console.1');
const assignment = [t('console.console.2'),t('console.console.3')];
const errors = {
  invalid_credentials:[t('console.console.4'),t('console.console.5')],
  login_failed:[t('console.console.4'),t('console.console.5')],
  rate_limited:[t('console.console.6'),t('console.console.7')],
  too_many_attempts:[t('console.console.6'),t('console.console.7')],
  login_busy:[t('console.console.8'),t('console.console.9')],
  login_confirmation_changed:[t('console.console.10'),t('console.console.11')],
  login_confirmation_expired:[t('console.console.12'),t('console.console.11')],
  session_reauthentication_required:[t('console.console.13'),t('console.console.14')],
  invalid_credentials_format:[t('console.console.15'),t('console.console.16')],
  credentials_change_required:[t('console.console.17'),t('console.console.18')],
  csrf_required:[t('console.console.10'),t('console.console.19')],
  desktop_forbidden:assignment,
  desktop_not_found:assignment,
  forbidden:assignment,
  console_preparing:[t('console.console.20'),t('console.console.21')],
  console_cleanup_pending:[t('console.console.22'),t('console.console.21')],
  console_busy:[t('console.console.23'),t('console.console.24')],
  console_taken_over:[t('console.console.25'),t('console.console.26')],
  console_unavailable:[t('console.console.27'),t('console.console.28')],
  console_timeout:[t('console.console.29'),t('console.console.30')],
  console_protocol_error:[t('console.console.31'),t('console.console.32')],
  console_display_inactive:[t('console.console.33'),t('console.console.34'),display],
  console_display_state_missing:[t('console.console.35'),t('console.console.36'),display],
  console_display_state_invalid:[t('console.console.37'),t('console.console.36'),display],
  console_display_identity_changed:[t('console.console.38'),t('console.console.36'),display],
  console_display_foreign_changed:[t('console.console.39'),t('console.console.40'),display],
  console_display_unavailable:[t('console.console.41'),t('console.console.42'),display],
  // The Host stopped its display change because Windows changed under it (a lock, a sign-out, another display change)
  // or the request ended. There is no "earlier recovery" to wait for, so the text does not speak of one.
  console_display_cancelled:[t('console.console.43'),t('console.console.44'),display],
  console_display_busy:[t('console.console.45'),t('console.console.46'),display],
  console_display_timeout:[t('console.console.47'),t('console.console.48'),display],
  console_disabled:[t('console.console.49'),t('console.console.50')],
  console_limit_reached:[t('console.console.51'),t('console.console.52')],
  console_permission_denied:[t('console.console.53'),t('console.console.54')],
  console_no_display:[t('console.console.99'),t('console.console.100')],
  additional_authentication:[t('console.console.55'),t('console.console.56')],
  cleanup_failed:[t('console.console.57'),t('console.console.58')],
  logout_failed:[t('console.console.59'),t('console.console.60')],
  transport_closed:[t('console.console.61'),t('console.console.62')],
  // The connection to the server ended before the Console answered (a refused or dropped WebSocket): not a
  // plain end, and the code is this page's own.
  transport_refused:[t('console.console.27'),t('console.console.notOpenedF4')],
  // End reasons the server sends on the socket (console-relay.mjs retire): the assignment or the login ended,
  // or the server is stopping. A plain 'you can connect again' was wrong for both.
  authorization_revoked:assignment,
  server_stopping:[t('console.console.serverStoppingF4'),t('console.console.21')],
  protocol_error:[t('console.console.63'),t('console.console.64')],
};
const waiting = new Set(['console_preparing','console_cleanup_pending','console_display_busy','console_limit_reached','rate_limited','too_many_attempts','login_busy','server_stopping']);
const locked = new Set(['desktop_forbidden','desktop_not_found','forbidden','authorization_revoked','console_permission_denied','credentials_change_required','csrf_required']);
const toneOf = code => code==='transport_closed'?'ended':waiting.has(code)?'wait':locked.has(code)?'lock':'error';
const icons = {progress:'monitor',wait:'clock',error:'alert',ended:'unplug',lock:'lock'};
const connected = () => document.body.classList.contains('console-connected');
function syncTools(){
  $('reconnect').hidden=!connected();
  // A group with nothing to show takes no separator.
  for(const group of document.querySelectorAll('.tools>.group:not(.fold-group)'))
    group.hidden=![...group.children].some(tool=>!tool.hidden&&!(embeddedConsole&&tool.classList.contains('browser-only')))||(embeddedConsole&&group.classList.contains('browser-only'));
  $('identity').dataset.tip=connected()?`${$('desktop-name').textContent} · ${$('connection-state').textContent}`:'';
}
function status(text){$('connection-state').textContent=text;syncTools();}
function notice(title,text,{tone='progress',retry=false,detail='',code=''}={}){
  $('notice-title').textContent=title;$('notice-text').textContent=text;
  $('notice').dataset.tone=tone;$('notice-icon').setAttribute('href','#i-'+icons[tone]);
  $('retry').hidden=!retry;
  $('notice-detail').textContent=detail;$('notice-detail').hidden=!detail;
  $('notice-code').textContent=code?t('console.console.65', { code }):'';$('notice-code').hidden=!code;
  $('notice-more').open=false;$('notice-more').hidden=!detail&&!code;
  $('notice').hidden=false;document.body.dataset.tone=tone;
}
function showError(code){
  document.body.classList.remove('console-connected');setToolbar(false);
  const known=Object.hasOwn(errors,code);
  const [title,text,detail='']=known?errors[code]:[t('console.console.66'),t('console.console.67')];
  const tone=known?toneOf(code):'error';
  const vm=known&&(code.startsWith('console_')||code==='protocol_error'||code==='cleanup_failed');
  status(title);
  // A plain end needs no detail; failures keep their code for the administrator. A code this page has no
  // words for is shown too (a fixed code of the server or of this page, never a sentence).
  notice(title,text,{tone,retry:!!desktopId,detail:[detail,vm?t('console.console.68'):''].filter(Boolean).join('\n'),code:tone!=='ended'&&typeof code==='string'&&/^[a-z_]{1,48}$/.test(code)?code:''});
  $('takeover').hidden=code!=='console_busy'||!desktopId;
}
function setBusy(value){busy=value;$('login-submit').disabled=value;$('retry').disabled=value;$('reconnect').disabled=value;$('logout').disabled=value;$('mfa-submit').disabled=value;$('mfa-code').disabled=value;$('mfa-recovery').disabled=value;}
function isLatin1(text){return [...text].every(ch=>ch.codePointAt(0)<=255);}
function validSession(data){return data&&typeof data.user?.username==='string'&&typeof data.csrfToken==='string'&&data.csrfToken.length>=20&&data.csrfToken.length<=256;}
async function api(path,method='GET',body,options={}){
  const headers={Accept:'application/json'};
  if(body!==undefined)headers['Content-Type']='application/json';
  if(method!=='GET'&&csrf)headers['X-Multitap-Console-CSRF']=csrf;
  Object.assign(headers,options.headers??{});
  let response;
  try{response=await fetch(`/console/api${path}`,{method,headers,credentials:'same-origin',cache:'no-store',redirect:'error',signal:AbortSignal.timeout(/^\/desktops\/[A-Za-z0-9_-]+\/connect$/.test(path)?90000:15000),...(body===undefined?{}:{body:JSON.stringify(body)})});}
  catch{throw {code:'network_error'};}
  let data=null;
  if(response.status!==204){try{data=await response.json();}catch{throw {code:'invalid_response'};}}
  if(!response.ok)throw {code:response.status===401?'unauthorized':data?.error?.code||'request_failed',status:response.status,details:data?.error?.details};
  return {status:response.status,data};
}
function cancelLoginConfirmation(){loginConfirmation?.finish(false);}
function confirmExistingLogin(details,generation,recoveryConsumed=false){
  if(!details||typeof details!=='object'||Array.isArray(details)||Object.keys(details).some(key=>!['protocol','challengeToken','expiresInMs','existingLogin'].includes(key))||details.protocol!=='session-access-v1'||typeof details.challengeToken!=='string'||!/^[A-Za-z0-9_-]{43}$/.test(details.challengeToken)||!Number.isInteger(details.expiresInMs)||details.expiresInMs<=0||details.expiresInMs>120000)throw {code:'invalid_response'};
  $('login-confirm-recovery').hidden=!recoveryConsumed;
  cancelLoginConfirmation();
  return new Promise((resolve,reject)=>{
    const dialog=$('login-confirm-dialog');let timer;
    const pending={finish(accepted,expired=false){
      if(loginConfirmation!==pending)return;
      loginConfirmation=null;clearTimeout(timer);
      dialog.removeEventListener('cancel',cancel);dialog.removeEventListener('close',cancel);
      $('login-confirm-cancel').removeEventListener('click',cancel);$('login-confirm-accept').removeEventListener('click',accept);
      if(dialog.open)dialog.close();
      if(expired&&generation===epoch)reject({code:'login_confirmation_expired'});else resolve(accepted&&generation===epoch);
    }};
    const cancel=event=>{event?.preventDefault();pending.finish(false);};
    const accept=()=>pending.finish(true);
    loginConfirmation=pending;
    dialog.addEventListener('cancel',cancel);dialog.addEventListener('close',cancel);
    $('login-confirm-cancel').addEventListener('click',cancel);$('login-confirm-accept').addEventListener('click',accept);
    timer=setTimeout(()=>pending.finish(false,true),details.expiresInMs);
    try{dialog.showModal();$('login-confirm-cancel').focus();}catch{pending.finish(false);}
  });
}
function releaseInput(){
  hangulKey.reset();
  const canvas=$('remote').querySelector('canvas');
  const lastPointer=pointer;pointer=null;
  if(canvas){
    for(const key of [...heldKeys.values()])canvas.dispatchEvent(new KeyboardEvent('keyup',{...key,bubbles:true,cancelable:true}));
    let buttons=lastPointer?.buttons||0;
    for(const button of [...heldButtons]){
      buttons&=~({0:1,1:4,2:2,3:8,4:16}[button]||0);
      canvas.dispatchEvent(new MouseEvent('mouseup',{clientX:lastPointer?.x||0,clientY:lastPointer?.y||0,button,buttons,bubbles:true,cancelable:true}));
    }
  }
  heldKeys.clear();heldButtons.clear();releaseCapture();rfb?.blur();
}
// Connected: the tools hang from a grip at the top centre, like the session
// toolbar. They open on a click and fold after a command, on a press into
// the picture, on Escape or with the fold button.
function setToolbar(open){document.body.classList.toggle('toolbar-open',open);$('toolbar-grip').setAttribute('aria-expanded',String(open));}
function foldToolbar(focusRemote){
  if(!document.body.classList.contains('toolbar-open'))return;
  setToolbar(false);
  if(focusRemote&&connected()&&!document.querySelector('dialog[open]'))rfb?.focus();
}
function clearTransport(){
  releaseInput();frameTimers.forEach(clearTimeout);frameTimers=[];clearTimeout(transportTimer);transportTimer=null;clearTimeout(preparationTimer);preparationTimer=null;
  document.body.classList.remove('console-connected');setToolbar(false);$('touch-keys').blur();
  const previous=rfb;rfb=null;previous?.disconnect();socket?.close();socket=null;
  policy={textToRemote:false,textToLocal:false};receivedClipboard=false;
  touchCancel();$('touch-note').hidden=true;
  $('send-cad').disabled=true;$('clipboard-open').disabled=true;$('clipboard-send').disabled=true;$('clipboard-copy').disabled=true;
  for(const id of ['send-cad','clipboard-open','fullscreen'])$(id).hidden=true;
  $('clipboard-text').value='';$('clipboard-status').textContent='';$('frame-note').hidden=true;markQemuConsole(null);
  if($('clipboard-dialog').open)$('clipboard-dialog').close();
  syncTools();
}
async function releaseLease(){
  clearTransport();
  if(!lease)return;
  try{const result=await api(`/desktops/${encodeURIComponent(desktopId)}/sessions/${lease.id}`,'DELETE');if(result.status!==204)throw {code:'cleanup_failed'};lease=null;}
  catch(error){
    if(error.status===404&&error.code==='console_session_not_found'){lease=null;return;}
    if(error.code==='unauthorized'){lease=null;csrf=null;throw error;}
    throw {code:'cleanup_failed'};
  }
}
function showLogin(message,preserveResult){
  ++epoch;++authGeneration;authenticatedSession=null;
  if(preserveResult)accountSecurityController?.retire({preserveResult});
  else{accountSecurityController?.destroy();accountSecurityController=null;}
  consoleAuth.retire();setBusy(false);clearTransport();lease=null;
  $('account-security').hidden=true;
  cancelLoginConfirmation();
  if(embeddedConsole){
    clearTransport();lease=null;csrf=null;
    $('login-panel').hidden=true;$('logout').hidden=true;
    status(t('console.console.69'));notice(t('console.console.69'),t('console.console.70'),{tone:'lock'});
    return;
  }
  document.body.classList.remove('console-connected');setToolbar(false);document.body.dataset.tone='lock';
  csrf=null;$('login-panel').hidden=false;$('notice').hidden=true;$('logout').hidden=true;$('retry').hidden=true;
  $('login-message').hidden=!message;$('login-message').textContent=message||'';
  status(message||t('console.console.71'));$('username').focus();
}
function markQemuConsole(isolated){
  if(isolated===null){delete document.body.dataset.qemuConsole;$('connection-state').removeAttribute('title');return;}
  document.body.dataset.qemuConsole=isolated?'isolated':'unconfirmed';
  if(isolated)$('connection-state').removeAttribute('title');else $('connection-state').title=t('console.console.72');
}
// hint: the server's consoleHint ('signin_other_display', console-relay.mjs
// signinElsewhere): one line for the first 20 seconds, in place of the
// black-screen note, since it explains that picture too. It then goes away:
// it lay over the bottom of the picture for the whole connection.
function observeFrame(instance,generation,isolated,hint=false){
  let observed=false,waited=false;
  const current=()=>rfb===instance&&generation===epoch;
  // Whether the Host proved a console-only topology stays observable (data
  // attribute, status tooltip) but is not a message: a visible picture is enough.
  const render=()=>{
    markQemuConsole(isolated);
    if(observed)status(t('console.console.73'));
    if(hint){$('frame-note').textContent=t('console.console.101');$('frame-note').hidden=false;}
    else if(observed)$('frame-note').hidden=true;
    else if(waited){$('frame-note').textContent=t('console.console.74');$('frame-note').hidden=false;}
  };
  if(hint){render();frameTimers.push(setTimeout(()=>{if(!current())return;hint=false;$('frame-note').hidden=true;render();},20000));}
  const inspect=()=>{
    if(observed)return;
    const canvas=$('remote').querySelector('canvas');
    // Public bounded readback only; no screenshot is retained.
    if(canvas&&canvas.width*canvas.height>0&&canvas.width*canvas.height<=4_000_000){
      // An opaque black canvas can exist before a usable frame arrives.
      try{const image=instance.getImageData();for(let i=0;i<image.data.length;i+=4){if(image.data[i+3]!==0&&(image.data[i]!==0||image.data[i+1]!==0||image.data[i+2]!==0)){observed=true;break;}}}catch{/* Unknown is not a frame. */}
    }
  };
  for(const delay of [1000,5000,10000])frameTimers.push(setTimeout(()=>{
    if(!current())return;waited||=delay===10000;inspect();render();
  },delay));
  const poll=async()=>{
    preparationTimer=null;if(!current()||!lease)return;
    try{
      const result=await api(`/desktops/${encodeURIComponent(desktopId)}/sessions/${lease.id}`);
      if(!current())return;isolated=result.data?.qemuConsolePreparation?.status==='isolated';
    }catch(error){
      if(!current())return;
      if(error.code==='unauthorized'){clearTransport();lease=null;showLogin(t('console.console.75'));return;}
      isolated=false;
    }
    if(!current())return;inspect();render();
    preparationTimer=setTimeout(poll,10000);
  };
  preparationTimer=setTimeout(poll,10000);
}
// takeover: '스트리밍 끊고 연결' ends this account's own stream first; while
// its cleanup runs the page asks again once a second, at most 20 times.
// `waiting` is such a repeat: the one card '이전 연결을 정리하고 있어요' stays
// until an answer other than 'still cleaning up' comes (otherwise the card
// changes to '연결 중' and back every second).
let cleanupRetry=null;
async function connect({takeover=false,retries=0,waiting=false}={}){
  if(busy||!csrf||!desktopId)return;
  clearTimeout(cleanupRetry);cleanupRetry=null;
  setBusy(true);const generation=++epoch;$('retry').hidden=true;$('takeover').hidden=true;
  if(!waiting){status(t('console.console.76'));notice(t('console.console.77'),t('console.console.78'));}
  try{
    await releaseLease();
    const result=await api(`/desktops/${encodeURIComponent(desktopId)}/connect`,'POST',takeover?{takeover:true}:{});
    if(generation!==epoch)return;
    if(result.status===202){
      const code=result.data?.code==='console_preparing'?'console_preparing':'console_cleanup_pending';
      const left=takeover?20:retries;
      if(code==='console_cleanup_pending'&&left>0){
        document.body.classList.remove('console-connected');setToolbar(false);
        status(t('console.console.22'));notice(t('console.console.22'),t('console.console.cleanupWaitF4'));
        cleanupRetry=setTimeout(()=>{if(generation===epoch)void connect({retries:left-1,waiting:true});},1000);
      }else showError(code);
      return;
    }
    if(waiting){status(t('console.console.76'));notice(t('console.console.77'),t('console.console.78'));}
    const data=result.data;
    if(!uuid.test(data?.id||'')||data.wsPath!==`/console/ws/${data.id}`||data.desktop?.id!==desktopId||typeof data.desktop?.name!=='string'||data.desktop.name.length>200||!Number.isFinite(Date.parse(data.expiresAt)))throw {code:'invalid_response'};
    lease={id:data.id};policy={textToRemote:data.clipboardPolicy?.textToRemote===true,textToLocal:data.clipboardPolicy?.textToLocal===true};
    $('desktop-name').textContent=data.desktop.name||t('console.console.79');
    const url=new URL(data.wsPath,location.origin);url.protocol=location.protocol==='https:'?'wss:':'ws:';
    if(location.protocol!=='https:'&&!['localhost','127.0.0.1','[::1]'].includes(location.hostname))throw {code:'insecure_origin'};
    socket=new WebSocket(url);socket.binaryType='arraybuffer';
    // Closed before the Console ever answered: the connection did not open, it did not end.
    let closedCode='transport_refused';
    socket.addEventListener('close',event=>{if(Object.hasOwn(errors,event.reason))closedCode=event.reason;});
    const instance=new RFB($('remote'),socket,{shared:true});rfb=instance;
    instance.scaleViewport=true;instance.resizeSession=false;instance.clipViewport=false;instance.showDotCursor=false;instance.background='#000';
    instance.addEventListener('connect',()=>{
      if(rfb!==instance||epoch!==generation)return;
      clearTimeout(transportTimer);transportTimer=null;
      if(closedCode==='transport_refused')closedCode='transport_closed';
      document.querySelectorAll('dialog[open]:not(#close-dialog)').forEach(dialog=>dialog.close());
      document.body.classList.add('console-connected');document.body.dataset.tone='connected';
      $('notice').hidden=true;$('send-cad').disabled=false;
      const isolated=data.qemuConsolePreparation?.status==='isolated';
      markQemuConsole(isolated);
      // The clipboard command exists only where the administrator allowed a direction.
      const clipboard=policy.textToRemote||policy.textToLocal;
      $('send-cad').hidden=false;$('fullscreen').hidden=!fullscreenWorks;
      $('clipboard-open').hidden=!clipboard;$('clipboard-open').disabled=!clipboard;$('clipboard-send').disabled=!policy.textToRemote;
      status(t('console.console.80'));
      observeFrame(instance,generation,isolated,data.consoleHint==='signin_other_display');
    });
    instance.addEventListener('disconnect',()=>{if(rfb!==instance||epoch!==generation)return;clearTransport();showError(closedCode);});
    for(const event of ['credentialsrequired','serververification','securityfailure'])instance.addEventListener(event,()=>{
      if(rfb!==instance||epoch!==generation)return;clearTransport();showError('protocol_error');
    });
    instance.addEventListener('clipboard',event=>{
      if(rfb!==instance||epoch!==generation||!policy.textToLocal)return;
      const text=event.detail?.text;
      if(typeof text!=='string'||text.length>65536||!isLatin1(text))return;
      $('clipboard-text').value=text;receivedClipboard=true;$('clipboard-copy').disabled=false;
      $('clipboard-status').textContent=t('console.console.81');
    });
    transportTimer=setTimeout(()=>{if(rfb===instance&&epoch===generation){clearTransport();showError('console_unavailable');}},15000);
  }catch(error){if(generation!==epoch)return;if(error.code==='unauthorized'){clearTransport();lease=null;showLogin(t('console.console.75'));}else showError(error.code);}
  finally{if(generation===epoch)setBusy(false);}
}
function syncAccountSecurity(){ $('account-security').hidden=!csrf||!authenticatedSession||embeddedConsole||!!authenticatedSession.consoleWebView; }
const consoleAuth=createConsoleAuthentication({
  document,origin:ownOrigin,request:api,begin(){accountSecurityController?.destroy();accountSecurityController=null;++authGeneration;authenticatedSession=null;csrf=null;return ++epoch;},
  current:generation=>document===ownDocument&&location.origin===ownOrigin&&generation===epoch,
  setBusy,confirmExisting:confirmExistingLogin,
  onPassword:message=>showLogin(message),
  onError(code,mfa){const [title,text]=errors[code]||(code==='unauthorized'?errors.invalid_credentials:[t('console.console.4'),t('console.console.67')]);const node=$(mfa?'mfa-message':'login-message');node.textContent=title+'\n'+text;node.hidden=false;$('login-panel').hidden=false;status(title);},
  async onSession(data,generation){if(!validSession(data))throw {code:'invalid_response'};if(generation!==epoch)return;csrf=data.csrfToken;authenticatedSession=data;++authGeneration;syncAccountSecurity();$('login-panel').hidden=true;$('logout').hidden=false;setBusy(false);await connect();},
  async discardLateSession(data){if(!validSession(data))return;try{await api('/logout','POST',{}, {headers:{'X-Multitap-Console-CSRF':data.csrfToken}});}catch{}},
});
$('account-security').addEventListener('click',async()=>{
  if(busy||!csrf||!authenticatedSession||embeddedConsole||authenticatedSession.consoleWebView)return;
  const modules=await accountSecurityModules;
  if(!modules||busy||!csrf||!authenticatedSession||authenticatedSession.consoleWebView)return;
  const {createAccountSecurityController,createAccountSecurityView}=modules;
  accountSecurityController?.destroy();releaseInput();
  const owner=authOwner();let controller;
  const request=async(path,method='GET',data,options={})=>{
    if(!authCurrent(owner))throw {code:'admin_session_changed'};
    try{
      const response=await api(path,method,data,{headers:method==='GET'?{}:{'X-Multitap-Console-CSRF':owner.csrf}});
      if(!authCurrent(owner))throw {code:'admin_session_changed'};
      return options.accept?options.accept({status:response.status,body:response.data}):response.data;
    }catch(error){if(error.code==='unauthorized'&&authCurrent(owner))showLogin(t('console.console.82'));throw error;}
  };
  controller=createAccountSecurityController({document,createView:options=>createAccountSecurityView({...options,onClose(){options.onClose?.();if(!authenticatedSession)$('username').focus();}}),request,owner:()=>owner,isCurrent:authCurrent,origin:()=>location.origin,accountName:()=>owner.session.user.username,onClosed(){if(document===ownDocument&&location.origin===ownOrigin&&!authenticatedSession)$('username').focus();},
    retireAuthentication:(ticket,{uncertain=false}={})=>{if(accountSecurityController!==controller||!authCurrent(owner))return;showLogin(uncertain?t('console.console.83'):t('console.console.84'),ticket);if(uncertain)void api('/logout','POST',{}, {headers:{'X-Multitap-Console-CSRF':owner.csrf}}).catch(()=>{});}
  });
  accountSecurityController=controller;controller.open();
});
for(const id of ['retry','reconnect'])$(id).addEventListener('click',()=>csrf?connect():boot());
$('takeover').addEventListener('click',()=>csrf?connect({takeover:true}):boot());
$('logout').addEventListener('click',async()=>{
  if(busy)return;setBusy(true);++epoch;++authGeneration;authenticatedSession=null;accountSecurityController?.destroy();accountSecurityController=null;syncAccountSecurity();releaseInput();
  try{
    // VM assignment may disappear before its scoped cancellation. The broker's
    // session logout must still revoke the cookie authority and remaining grants.
    try{await releaseLease();}catch(error){if(error.code==='unauthorized')throw error;}
    const result=await api('/logout','POST',{});if(result.status!==204)throw {code:'logout_failed'};
    lease=null;clearTransport();showLogin(t('console.console.85'));
  }
  catch(error){if(error.code==='unauthorized'){clearTransport();lease=null;showLogin();}else showError(error.code==='csrf_required'?'csrf_required':'logout_failed');}
  finally{setBusy(false);}
});
$('send-cad').addEventListener('click',()=>{releaseInput();rfb?.sendCtrlAltDel();});
$('new-window').href=desktopId?`/console/${desktopId}`:'/';
// The app window follows the app's theme (its WebView reports it as the
// colour scheme); a browser tab may also switch for itself.
const lightScheme=matchMedia('(prefers-color-scheme: light)');let themeChosen=false;
function theme(value){
  document.documentElement.dataset.theme=value;
  const label=value==='light'?t('console.console.86'):t('console.console.87');
  $('theme').setAttribute('aria-label',label);$('theme').dataset.tip=label;$('theme').querySelector('use').setAttribute('href',value==='light'?'#i-moon':'#i-sun');
}
theme(lightScheme.matches?'light':'dark');
lightScheme.addEventListener('change',()=>{if(embeddedConsole||!themeChosen)theme(lightScheme.matches?'light':'dark');});
$('theme').addEventListener('click',()=>{themeChosen=true;theme(document.documentElement.dataset.theme==='light'?'dark':'light');});
$('fullscreen').addEventListener('click',async()=>{releaseInput();try{if(document.fullscreenElement)await document.exitFullscreen();else await document.documentElement.requestFullscreen();}catch{status(t('console.console.88'));}});
document.addEventListener('fullscreenchange',()=>{
  const label=document.fullscreenElement?t('console.console.89'):t('console.console.90');
  $('fullscreen').setAttribute('aria-label',label);$('fullscreen').dataset.tip=label;$('fullscreen').querySelector('use').setAttribute('href',document.fullscreenElement?'#i-collapse':'#i-expand');
});
for(const [button,dialog]of [['help-open','help-dialog'],['clipboard-open','clipboard-dialog']])$(button).addEventListener('click',()=>{releaseInput();$(dialog).showModal();});
document.querySelectorAll('[data-close]').forEach(button=>button.addEventListener('click',()=>$(button.dataset.close).close()));
$('toolbar-grip').addEventListener('click',()=>{
  releaseInput();setToolbar(true);syncTools();
  $('toolbar').querySelector('.tools .tool:not([hidden]):not(:disabled)')?.focus();
});
$('toolbar-fold').addEventListener('click',()=>foldToolbar(true));
$('toolbar').addEventListener('keydown',event=>{if(event.key==='Escape'&&document.body.classList.contains('toolbar-open')){event.preventDefault();foldToolbar(true);}});
// A command used from the open bar folds it (after its own handler ran).
$('toolbar').addEventListener('click',event=>{const tool=event.target.closest('.tools .tool');if(tool&&tool.id!=='toolbar-fold'&&connected())foldToolbar(true);});
// Touch: the grip is a mouse target,
// so a touch screen gets the Android session's 메뉴 and 키보드 buttons. 키보드
// focuses #touch-keys, an invisible field. Typing needs care in two ways:
// QEMU turns a bare keysym into its unshifted key ('!' becomes '1'),
// and a phone keyboard composes, predicts and corrects words in the field.
// So each character goes as its US key (KeyboardEvent code) with Shift held
// when needed, and the field is compared before and after every change:
// removed characters are Backspace, added ones are typed. 한글 is below;
// other characters outside Latin-1 are neither sent nor counted.
const touchScreen=matchMedia('(pointer: coarse)').matches||(embeddedConsole&&/Android/i.test(navigator.userAgent));
if(touchScreen)document.body.classList.add('touch');
const touchKeys=$('touch-keys'),touchRest='  ';
let touchLast=touchRest,touchComposing=false;
const touchSpecial={Enter:[0xff0d,'Enter'],Backspace:[0xff08,'Backspace'],Tab:[0xff09,'Tab'],Escape:[0xff1b,'Escape'],Delete:[0xffff,'Delete'],Home:[0xff50,'Home'],End:[0xff57,'End'],ArrowLeft:[0xff51,'ArrowLeft'],ArrowUp:[0xff52,'ArrowUp'],ArrowRight:[0xff53,'ArrowRight'],ArrowDown:[0xff54,'ArrowDown']};
const touchUs=new Map();
for(const [plain,shifted,code] of [['`','~','Backquote'],['1','!','Digit1'],['2','@','Digit2'],['3','#','Digit3'],['4','$','Digit4'],['5','%','Digit5'],['6','^','Digit6'],['7','&','Digit7'],['8','*','Digit8'],['9','(','Digit9'],['0',')','Digit0'],['-','_','Minus'],['=','+','Equal'],['[','{','BracketLeft'],[']','}','BracketRight'],['\\','|','Backslash'],[';',':','Semicolon'],["'",'"','Quote'],[',','<','Comma'],['.','>','Period'],['/','?','Slash']]){touchUs.set(plain,[code,false]);touchUs.set(shifted,[code,true]);}
for(let c=97;c<=122;c++){const ch=String.fromCharCode(c),code='Key'+ch.toUpperCase();touchUs.set(ch,[code,false]);touchUs.set(ch.toUpperCase(),[code,true]);}
touchUs.set(' ',['Space',false]);
// 한글: a syllable goes as the 두벌식 keys a
// Korean keyboard would press (ㅂㅈㄷㄱㅅ on q w e r t ...), and the VM's IME is
// switched with its 한/영 key (Right Alt, below). VNC does not carry the VM's
// mode, so the page asks the Host (ime-state-v1: GET .../sessions/:id/ime, a
// fresh reading of the window that has the keyboard in the signed-in console
// session). That answer is the only
// source; nothing is assumed or remembered. It is read when the keyboard opens
// and before the first character of a run that needs a mode (한글, or Latin
// letters); within a run (characters less than a second apart) the confirmed
// mode stands. 한/영 is pressed only when the Host says the other mode, and the
// run goes on only after the Host reports the new mode (up to a second);
// otherwise nothing more is typed and one line says so. When the Host cannot
// tell (an older Host, signed out, locked, a security prompt), Latin letters,
// digits and symbols go without any 한/영 key and 한글 is not sent.
const hangulCho='rRseEfaqQtTdwWczxvg';
const hangulJung=['k','o','i','O','j','p','u','P','h','hk','ho','hl','y','n','nj','np','nl','b','m','ml','l'];
const hangulJong=['','r','R','rt','s','sw','sg','e','f','fr','fa','fq','ft','fx','fv','fg','a','q','qt','t','T','d','w','c','z','x','v','g'];
const hangulJamo=['r','R','rt','s','sw','sg','e','E','f','fr','fa','fq','ft','fx','fv','fg','a','q','Q','qt','t','T','d','w','W','c','z','x','v','g','k','o','i','O','j','p','u','P','h','hk','ho','hl','y','n','nj','np','nl','b','m','ml','l'];
function hangulKeys(ch){
  const c=ch.codePointAt(0);
  if(c>=0xac00&&c<=0xd7a3){const s=c-0xac00;return hangulCho[Math.floor(s/588)]+hangulJung[Math.floor(s%588/28)]+hangulJong[s%28];}
  if(c>=0x3131&&c<=0x3163)return hangulJamo[c-0x3131];
  return null;
}
const touchSendable=ch=>{const c=ch.codePointAt(0);return c===0x0a||c===0x0d||(c>=0x20&&c<=0x7e)||(c>=0xa0&&c<=0xff)||hangulKeys(ch)!==null;};
const touchText=value=>[...value].filter(touchSendable).join('');
function touchReady(){return !!rfb&&!busy&&!rfb.viewOnly&&document.body.classList.contains('console-connected');}
function touchReset(){touchKeys.value=touchRest;touchLast=touchRest;try{touchKeys.setSelectionRange(touchRest.length,touchRest.length);}catch{}}
function touchKey(keysym,code){if(touchReady())rfb.sendKey(keysym,code);}
// The 한/영 key of a VM: QEMU's keyboard is a 101-key PS/2 keyboard, and Windows'
// Korean layout on it (kbd101a) switches with Right Alt;
// the Korean keyboard's own 한/영 scancode (Lang1) does nothing there.
function touchToggle(){rfb.sendKey(0xffea,'AltRight');}
// One US key. Caps Lock in the VM (QEMU's LED state, kept by noVNC 1.7.0 as
// _remoteCapsLock) turns letters around, so Shift is turned around for them.
function touchUsKey(ch){
  const us=touchUs.get(ch);
  if(!us){rfb.sendKey(ch.codePointAt(0));return;}
  const [code]=us;let shift=us[1];
  if(/^[a-zA-Z]$/.test(ch)&&rfb._remoteCapsLock===true)shift=!shift;
  if(shift)rfb.sendKey(0xffe1,'ShiftLeft',true);
  rfb.sendKey(ch.codePointAt(0),code,true);rfb.sendKey(ch.codePointAt(0),code,false);
  if(shift)rfb.sendKey(0xffe1,'ShiftLeft',false);
}
function touchChar(ch){
  if(!touchReady())return;
  const keysym=ch.codePointAt(0);
  if(keysym===0x0a||keysym===0x0d){rfb.sendKey(0xff0d,'Enter');return;}
  const hangul=hangulKeys(ch);
  if(hangul!==null){
    // 두벌식 keys are the jamo's places, not letters: Caps Lock must not flip them.
    for(const key of hangul){const [code,shift]=touchUs.get(key);if(shift)rfb.sendKey(0xffe1,'ShiftLeft',true);rfb.sendKey(key.codePointAt(0),code,true);rfb.sendKey(key.codePointAt(0),code,false);if(shift)rfb.sendKey(0xffe1,'ShiftLeft',false);}
    return;
  }
  touchUsKey(ch);
}
// The mode a character needs: true 한글, false Latin letters, undefined none
// (digits, symbols, space and Enter type the same in both modes).
const touchNeed=ch=>hangulKeys(ch)!==null?true:/^[a-zA-Z]$/.test(ch)?false:undefined;
const touchRunMs=1000,touchConfirmMs=1000,touchConfirmStepMs=100;
// The Host's answer of this run and when: korean true/false, null (it cannot
// tell) or undefined (not asked yet). Each character typed in the run renews `at`.
let touchIme={korean:undefined,at:0},touchQueue=[],touchPumping=false,touchGeneration=0,touchNoteTimer=null;
async function touchImeRead(){
  const owner=lease;if(!owner||!desktopId)return null;
  try{
    const result=await api(`/desktops/${encodeURIComponent(desktopId)}/sessions/${owner.id}/ime`);
    return lease===owner&&typeof result.data?.imeKorean==='boolean'?result.data.imeKorean:null;
  }catch{return null;}
}
const touchPause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
function touchNote(text){
  clearTimeout(touchNoteTimer);$('touch-note').textContent=text;$('touch-note').hidden=false;
  touchNoteTimer=setTimeout(()=>{$('touch-note').hidden=true;},6000);
}
// Ends what waits to be typed. After a refusal the field starts again from the
// VM's side: what was typed stays typed, and a Backspace in the empty field is
// still one Backspace in the VM.
function touchCancel(){touchGeneration++;touchQueue=[];touchPumping=false;touchIme={korean:undefined,at:0};}
function touchRefuse(key){touchCancel();touchReset();touchNote(t(key));}
// 'type' when the VM is in the mode `hangul` needs, or Latin may go without
// one; 'unknown' or 'unconfirmed' when nothing may be typed.
async function touchEnsure(hangul,generation){
  if(touchIme.korean===undefined||Date.now()-touchIme.at>=touchRunMs){
    const korean=await touchImeRead();if(generation!==touchGeneration)return 'gone';
    touchIme={korean,at:Date.now()};
  }
  if(touchIme.korean===null)return hangul?'unknown':'type';
  if(touchIme.korean===hangul)return 'type';
  if(!touchReady())return 'gone';
  touchToggle();touchIme={korean:undefined,at:0};
  const until=Date.now()+touchConfirmMs;
  while(Date.now()<until){
    await touchPause(touchConfirmStepMs);if(generation!==touchGeneration)return 'gone';
    const korean=await touchImeRead();if(generation!==touchGeneration)return 'gone';
    if(korean===hangul){touchIme={korean,at:Date.now()};return 'type';}
  }
  return 'unconfirmed';
}
async function touchPump(generation){
  touchPumping=true;
  while(generation===touchGeneration&&touchQueue.length){
    const op=touchQueue[0];
    if(op.need!==undefined){
      const decision=await touchEnsure(op.need,generation);
      if(generation!==touchGeneration)return;
      if(decision==='unknown'){touchRefuse('console.console.102');return;}
      if(decision!=='type'){touchRefuse('console.console.103');return;}
      touchIme.at=Date.now();
    }
    touchQueue.shift();op.send();
  }
  if(generation===touchGeneration)touchPumping=false;
}
// Every key goes through one queue, in order, so a character never overtakes
// the 한/영 switch it waits for.
function touchEnqueue(op){touchQueue.push(op);if(!touchPumping)void touchPump(touchGeneration);}
// Removed text: a 한글 syllable still being composed in the VM (the last
// character, nothing typed after it) loses one jamo per Backspace; anything
// else is one character per Backspace.
function touchChanged(){
  const value=[...touchText(touchKeys.value)],old=[...touchLast];
  let same=0;while(same<value.length&&same<old.length&&value[same]===old[same])same++;
  for(let n=old.length-1;n>=same;n--){
    const keys=n===old.length-1?hangulKeys(old[n]):null;
    for(let k=0;k<(keys?keys.length:1);k++)touchEnqueue({send:()=>touchKey(0xff08,'Backspace')});
  }
  for(const ch of value.slice(same))touchEnqueue({need:touchNeed(ch),send:()=>touchChar(ch)});
  touchLast=value.join('');
  if(!touchComposing&&(value.length<touchRest.length||touchKeys.value.length>64))touchReset();
}$('touch-menu').addEventListener('click',()=>{
  if(document.body.classList.contains('toolbar-open')){foldToolbar(false);return;}
  releaseInput();setToolbar(true);syncTools();
});
// The Android console window
// resizes for the on-screen keyboard (adjustResize), which would scale
// and move the picture again. While the keyboard is in use the page keeps the height it had
// before; the keyboard covers the lower part instead.
function touchHold(){if(document.body.classList.contains('touch-typing'))return;document.body.style.setProperty('--touch-held-height',document.documentElement.clientHeight+'px');document.body.classList.add('touch-typing');}
function touchRelease(){document.body.classList.remove('touch-typing');document.body.style.removeProperty('--touch-held-height');}
touchKeys.addEventListener('blur',()=>touchRelease());
$('touch-keyboard').addEventListener('click',()=>{
  if(!touchReady())return;
  foldToolbar(false);releaseInput();touchReset();touchHold();touchKeys.focus({preventScroll:true});
  // A fresh reading for the first characters (the Host also starts its reader).
  const generation=touchGeneration;
  void touchImeRead().then(korean=>{if(generation===touchGeneration&&touchIme.korean===undefined&&!touchQueue.length)touchIme={korean,at:Date.now()};});
});
touchKeys.addEventListener('keydown',event=>{
  const special=touchSpecial[event.key];
  if(!special||event.isComposing||touchComposing)return;
  event.preventDefault();touchEnqueue({send:()=>touchKey(special[0],special[1])});
});
touchKeys.addEventListener('compositionstart',()=>{touchComposing=true;});
touchKeys.addEventListener('compositionend',()=>{touchComposing=false;touchChanged();});
touchKeys.addEventListener('input',()=>touchChanged());$('clipboard-send').addEventListener('click',()=>{
  if(!rfb||!policy.textToRemote)return;
  const text=$('clipboard-text').value;
  if(!isLatin1(text)){$('clipboard-status').textContent=t('console.console.91');return;}
  rfb.clipboardPasteFrom(text);$('clipboard-status').textContent=t('console.console.92');
});
$('clipboard-copy').addEventListener('click',async()=>{
  if(!rfb||!policy.textToLocal||!receivedClipboard)return;
  try{await navigator.clipboard.writeText($('clipboard-text').value);$('clipboard-status').textContent=t('console.console.93');}
  catch{$('clipboard-status').textContent=t('console.console.94');}
});
$('remote').addEventListener('keydown',event=>{if(hangulKey.handle(event))return;if(event.target.tagName==='CANVAS'&&event.code)heldKeys.set(event.code,{key:event.key,code:event.code,location:event.location});},true);
$('remote').addEventListener('keyup',event=>{if(hangulKey.handle(event))return;heldKeys.delete(event.code);},true);
// A tap into the picture folds the open tools on a touch screen too (noVNC takes the touch events).
$('remote').addEventListener('pointerdown',()=>foldToolbar(false),true);
$('remote').addEventListener('mousedown',event=>{foldToolbar(false);if(event.target.tagName==='CANVAS'){heldButtons.add(event.button);pointer={x:event.clientX,y:event.clientY,buttons:event.buttons};}},true);
$('remote').addEventListener('mousemove',event=>{if(pointer){pointer.x=event.clientX;pointer.y=event.clientY;}},true);
$('remote').addEventListener('mouseup',event=>{heldButtons.delete(event.button);if(!event.buttons)pointer=null;else if(pointer)pointer.buttons=event.buttons;},true);
$('remote').addEventListener('focusout',event=>{if(!$('remote').contains(event.relatedTarget))releaseInput();});
window.addEventListener('blur',releaseInput);
document.addEventListener('visibilitychange',()=>{if(document.hidden)releaseInput();});
window.addEventListener('pagehide',()=>{++epoch;++authGeneration;authenticatedSession=null;csrf=null;consoleAuth.retire();accountSecurityController?.destroy();accountSecurityController=null;cancelLoginConfirmation();clearTransport();});
window.addEventListener('pageshow',event=>{
  if(event.persisted&&document===ownDocument&&location.origin===ownOrigin)showLogin(t('console.console.95'));
});
// The app window asks here before it closes: it sends the one
// request string, this page answers with fixed strings. Anything else is
// ignored. App builds without this bridge keep their own question.
if(appWindow){
  const dialog=$('close-dialog');
  appWindow.addEventListener('message',event=>{
    if(event.data!=='multitap-console:close-request')return;
    releaseInput();foldToolbar(false);
    appWindow.postMessage('multitap-console:close-shown');
    if(!dialog.open)dialog.showModal();
    $('close-confirm').focus();
  });
  // Answered at once (not from the later close event), so a new request
  // right after a cancel can never be answered by the old cancel.
  const answer=message=>{if(dialog.open)dialog.close();appWindow.postMessage(message);};
  $('close-confirm').addEventListener('click',()=>answer('multitap-console:close'));
  $('close-cancel').addEventListener('click',()=>answer('multitap-console:close-cancel'));
  dialog.addEventListener('cancel',event=>{event.preventDefault();answer('multitap-console:close-cancel');});
  appWindow.postMessage('multitap-console:ready');
}
async function boot(){
  if(busy)return;
  if(!desktopId){status(t('console.console.96'));notice(t('console.console.97'),t('console.console.98'),{tone:'error'});return;}
  setBusy(true);let accepted=false;const generation=++epoch;
  try{const {data}=await api('/session');if(generation!==epoch)return;if(!validSession(data))throw {code:'invalid_response'};csrf=data.csrfToken;authenticatedSession=data;++authGeneration;syncAccountSecurity();$('logout').hidden=false;accepted=true;}
  catch(error){if(generation!==epoch)return;if(error.code==='unauthorized')showLogin();else showError(error.code);}
  finally{if(generation===epoch)setBusy(false);}
  if(accepted&&generation===epoch)await connect();
}
syncTools();
boot();

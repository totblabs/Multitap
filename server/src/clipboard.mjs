import { createHash } from 'node:crypto';
import { WebSocketServer, WebSocket } from 'ws';
import { tokenHash } from './security.mjs';
import { anyClipboardAllowed, clipboardPolicyRestricted, intersectClipboardPolicy, clipboardTransferAllowed, clipboardFormatPermission } from './clipboard-policy.mjs';

// Peers that do not send X-Multitap-Clipboard-Limits check these exact numbers
// in clipboard.ready, so a pair that includes one of them stays on them.
export const clipboardLimits = Object.freeze({ chunkBytes:32768,maxFiles:128,maxFileBytes:256*1024*1024,maxTotalBytes:512*1024*1024,maxTextUnits:32768 });
// Peers that send X-Multitap-Clipboard-Limits: 2 understand these (and the
// disk_full / file_locked / limits_exceeded peer reasons).
export const clipboardLimitsV2 = Object.freeze({ chunkBytes:32768,maxFiles:128,maxFileBytes:4*1024*1024*1024,maxTotalBytes:8*1024*1024*1024,maxTextUnits:1024*1024,maxTextBytes:1024*1024,limitsProfile:2 });
const limitsV1Extended = Object.freeze({ ...clipboardLimits,maxTextBytes:128*1024,limitsProfile:2 });
// Peers that also send X-Multitap-Clipboard-Formats: 2 (and Limits: 2) understand rich text, images,
// folder trees and the sliding window. Host IPC frames stay below 64 KiB, so a chunk is 45 KiB (61440 base64
// characters) rather than larger; throughput comes from the window, not the chunk size.
const MiB=1024*1024;
export const clipboardLimitsFormats = Object.freeze({ ...clipboardLimitsV2,chunkBytes:46080,formatsProfile:2,maxEntries:10000,maxDepth:32,maxPathChars:160,maxRichBytes:8*MiB,maxImageBytes:256*MiB,maxImagePixels:64000000,windowBytes:MiB,windowUnits:32 });
const enforcedV1 = Object.freeze({ ...clipboardLimits,maxTextBytes:128*1024 });
// Paste-pull profile (pull-on-paste of files and folder trees): the relay advertises it only to a formats pair whose
// peers both sent X-Multitap-Clipboard-Pull: 1. It adds to the formats limits and changes nothing else about them.
export const clipboardLimitsPull = Object.freeze({ ...clipboardLimitsFormats,pullProfile:1,maxOffers:2 });
// Scale revision (no product limit on file sizes, totals or top-level counts; text and HTML up to 64 MiB; relative paths of 1024
// characters on the wire; the entry list is bounded only by a memory guard). The relay advertises it only to a paste-pull pair whose
// peers both sent X-Multitap-Clipboard-Scale: 1; every other pair keeps exactly the limits it had. Big copies cross as paste-pull
// offers (flow-controlled by the 1 MiB window, pulled when pasted), so nothing here depends on the size of what is copied; content
// pushed at copy time stays small (pushBelowBytes / pushBelowEntries, as the peers decide with shouldOffer).
// 2^53 - 1: sizes and offsets are JSON numbers.
export const clipboardLimitsScale = Object.freeze({ ...clipboardLimitsPull,scaleProfile:1,maxFiles:200000,maxFileBytes:Number.MAX_SAFE_INTEGER,maxTotalBytes:Number.MAX_SAFE_INTEGER,maxTextUnits:64*MiB,maxTextBytes:64*MiB,maxEntries:200000,maxDepth:512,maxPathChars:1024,maxRichBytes:64*MiB });
export const clipboardScaleHeader='x-multitap-clipboard-scale';
// On-paste revision (a copy is silent, files move only when pasted). The relay advertises it only to a scale pair
// whose peers both sent X-Multitap-Clipboard-OnPaste: 1. In such a pair nothing is pushed at copy time except small text (at most
// pushTextBytes of text, or of text and HTML) and small images (pushImageBytes of PNG): files and folders are always offered
// (clipboard.offer, pulled when pasted), and so is text, rich text or an image above those sizes (clipboard.offer with the format of
// the clipboard.begin it replaces; its items are pulled by index like the files of a tree). Two additive messages exist only here:
// clipboard.offer.withdraw {requestId: offerId} (the offering side's clipboard changed or its offer is no longer served) and
// clipboard.paste.waiting {requestId, active} (Host to client: an application in the VM is blocked waiting for data of the PC's offer).
export const clipboardOnPasteHeader='x-multitap-clipboard-onpaste';
export const clipboardSettledHeader='x-multitap-clipboard-settled';
export const clipboardSettledProtocol='text-image-250-v1';
export const clipboardIntentHeader='x-multitap-clipboard-intent';
export const clipboardIntentProtocol='text-image-pending-v1';
// Pull queue (without it a folder of ~700 small files pastes at ~27 ms per file): a paste reads files one after another and every pull
// is a round trip, one at a time per pair. In a pair whose peers both sent X-Multitap-Clipboard-PullQueue: 1 (on top of the on-paste
// revision) a requester may have up to pullQueueDepth pulls outstanding; the offering peer serves them in the order it received them, so
// data flows for the oldest one only and the next starts the moment it ends. Each pull's window is 4 MiB / 128 units, a pull that is
// cancelled before its turn is cancelled at the offering peer too, and such a pair may move pullQueueFastBytes per socket and second.
// clipboard.ready carries clipboardPullQueue: clipboardPullQueueProtocol (the numbers above); every other pair keeps one pull at a time.
export const clipboardPullQueueHeader='x-multitap-clipboard-pullqueue';
export const clipboardPullQueueProtocol='pull-queue-4-v1';
export const pullQueueDepth=4;
const pullQueueWindowBytes=4*1024*1024,pullQueueWindowUnits=128;
// Explorer cannot create a pasted item whose relative path has 260 or more characters (docs/remote-session-protocol.md, copy-source matrix).
const pastePathChars=259,pushBelowBytes=16*MiB,pushBelowEntries=256;
export const clipboardLimitsOnPaste = Object.freeze({ ...clipboardLimitsScale,onPasteProfile:1,pushTextBytes:MiB,pushImageBytes:8*MiB });
const offerFormats=new Set(['tree','text','richtext','image']);
const waitingPattern=/^wait-[1-9][0-9]{0,8}$/;
export const clipboardPullHeader='x-multitap-clipboard-pull';
export const clipboardLimitsHeader='x-multitap-clipboard-limits';
export const clipboardFormatsHeader='x-multitap-clipboard-formats';
const maxMessage=256*1024,maxManifest=48*1024,maxEntriesMessage=96*1024;
const requestPattern=/^[A-Za-z0-9_-]{1,64}$/;
const peerErrors=new Set(['clipboard_empty','clipboard_busy','unsupported_clipboard_format','clipboard_read_failed','clipboard_write_failed','clipboard_file_unavailable','clipboard_transfer_failed','no_interactive_session','clipboard_cancelled','clipboard_policy_denied','clipboard_changed','clipboard_limits_exceeded','clipboard_disk_full','clipboard_file_locked']);
// What a peer without the extended profile is sent instead of the newer reasons.
const legacyReason=Object.freeze({clipboard_limits_exceeded:'clipboard_transfer_failed',clipboard_disk_full:'clipboard_write_failed',clipboard_file_locked:'clipboard_file_unavailable'});
// A pull request id is pullc-<n> from the client or pullh-<n> from the Host, n rising per requester within a
// pair; the two namespaces never meet, and neither meets a transfer request id.
// Reasons that name the specific limit; only a scale pair knows them (the peers downgrade to clipboard_limits_exceeded otherwise, and the
// relay does the same for a peer that sends one in any other pair).
const scaleReasons=new Set(['clipboard_path_too_long','clipboard_text_too_large','clipboard_entry_count_limit','clipboard_depth_limit','clipboard_image_too_large']);
const pullPattern=/^pull[ch]-[1-9][0-9]{0,8}$/;
const pullReasons=new Set([...peerErrors,'clipboard_selected_file_changed','clipboard_offer_expired']);
const protocolErrors=new Set(['clipboard_path_too_long','clipboard_invalid_message','clipboard_limits_exceeded','clipboard_out_of_order','clipboard_hash_mismatch','clipboard_text_invalid','clipboard_busy','clipboard_policy_denied']);
const inFlightTypes=new Set(['clipboard.chunk','clipboard.item.end','clipboard.entries','clipboard.chunk.ack','clipboard.item.ack','clipboard.commit']);
const exact=(value,keys)=>Object.keys(value).length===keys.length&&keys.every(key=>Object.hasOwn(value,key));
function require(ok,reason='clipboard_invalid_message'){if(!ok)throw new Error(reason);}
function syncIdentity(requestId){
  require(/^[a-f0-9]{32}-[0-7][a-f0-9]{15}$/.test(requestId));
  const counter=BigInt('0x'+requestId.slice(33));require(counter>0n);
  return{prefix:requestId.slice(0,32),counter};
}
function retiredAcknowledgement(value,pairLimits=clipboardLimitsFormats){
  const limits={maxEntries:pairLimits.maxEntries??clipboardLimitsFormats.maxEntries,maxFileBytes:pairLimits.maxFileBytes??clipboardLimitsV2.maxFileBytes};
  if(['clipboard.cancel','clipboard.applied'].includes(value.type))return exact(value,['type','requestId']);
  if(value.type==='clipboard.error')return exact(value,['type','requestId','reason'])&&(peerErrors.has(value.reason)||scaleReasons.has(value.reason));
  if(value.type==='clipboard.item.ack')return exact(value,['type','requestId','index'])&&Number.isSafeInteger(value.index)&&value.index>=0&&value.index<limits.maxEntries;
  if(value.type==='clipboard.chunk.ack')return exact(value,['type','requestId','index','offset'])&&Number.isSafeInteger(value.index)&&value.index>=0&&value.index<limits.maxEntries&&Number.isSafeInteger(value.offset)&&value.offset>=0&&value.offset<=Math.max(limits.maxFileBytes,clipboardLimitsV2.maxFileBytes);
  return false;
}
// The server-wide byte budget per window, charged per key (a clipboard pair). When it is exceeded the key that used the most
// of the window is the one to end, repeatedly until the total fits, never merely the key whose message crossed the line.
export class FairByteBudget{
  constructor(limit,windowMs=1000){this.limit=limit;this.windowMs=windowMs;this.at=undefined;this.total=0;this.used=new Map();}
  // Records ytes for key; returns the keys that must be closed (heaviest first), already removed from the account.
  charge(key,bytes,now=Date.now()){
    if(this.at===undefined||now-this.at>=this.windowMs){this.at=now;this.total=0;this.used.clear();}
    this.total+=bytes;this.used.set(key,(this.used.get(key)??0)+bytes);
    const closing=[];
    while(this.total>this.limit&&this.used.size>0){
      let heavy,most=-1;for(const [candidate,amount] of this.used)if(amount>most){heavy=candidate;most=amount;}
      this.total-=most;this.used.delete(heavy);closing.push(heavy);
    }
    return closing;
  }
}
export function safeName(name,max=240){
  if(typeof name!=='string'||!name||name.length>max||!name.isWellFormed()||/[\x00-\x1f<>:"/\\|?*]/.test(name)||/[. ]$/.test(name)||['.','..'].includes(name))return false;
  // Windows drops trailing spaces and dots of the part before the first dot ("CON .txt" is CON); upper-casing is ASCII only,
  // as in the native safeLeaf this mirrors.
  const stem=name.split('.')[0].replace(/[. ]+$/,'').replace(/[a-z]/g,letter=>letter.toUpperCase());
  return !/^(?:CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|(?:COM|LPT)[1-9¹²³])$/.test(stem);
}
// Every format belongs to one administrator permission: plain and rich text are
// "text"; images (binary content) and files/folder trees are "files".
const formatsOnly=new Set(['richtext','image','tree']);
export const clipboardFormatClass=format=>Object.hasOwn(clipboardFormatPermission,format)?clipboardFormatPermission[format]:null;
const sizeInRange=(size,max,min=0)=>Number.isSafeInteger(size)&&size>=min&&size<=max;
function manifest(value,bytes,limits,formats=false,offer=false){
  require(bytes<=maxManifest,'clipboard_limits_exceeded');
  // On-paste pair: what is pushed at copy time is small; anything bigger is announced with clipboard.offer.
  const pushed=limits.onPasteProfile===1&&!offer;
  if(formatsOnly.has(value.format)){
    require(formats===true);
    require(exact(value,['type','requestId','format','items'])&&Array.isArray(value.items));
    if(value.format==='richtext'){
      const order=['text','html','rtf'];let at=-1;
      require(value.items.length>=1&&value.items.length<=3);
      let richTotal=0;
      const parts=value.items.map((item,position)=>{
        require(item&&typeof item==='object'&&!Array.isArray(item)&&exact(item,['kind','size'])&&order.includes(item.kind));
        const rank=order.indexOf(item.kind);require(rank>at&&(position>0||item.kind==='text'));at=rank;
        require(sizeInRange(item.size,item.kind==='text'?limits.maxTextBytes:limits.maxRichBytes,item.kind==='text'?0:1),'clipboard_limits_exceeded');
        richTotal+=item.size;
        return{size:item.size,kind:item.kind};
      });
      if(pushed)require(richTotal<=limits.pushTextBytes,'clipboard_limits_exceeded');
      return parts;
    }
    require(value.format==='image'&&value.items.length===1);
    const [item]=value.items;
    require(item&&typeof item==='object'&&!Array.isArray(item)&&exact(item,['kind','size','width','height'])&&item.kind==='png');
    require(Number.isSafeInteger(item.width)&&Number.isSafeInteger(item.height)&&item.width>=1&&item.height>=1&&item.width<=65535&&item.height<=65535);
    require(item.width*item.height<=limits.maxImagePixels&&sizeInRange(item.size,limits.maxImageBytes,1),'clipboard_limits_exceeded');
    if(pushed)require(item.size<=limits.pushImageBytes,'clipboard_limits_exceeded');
    return[{size:item.size,kind:'png',width:item.width,height:item.height}];
  }
  if(value.format==='text'){
    require(exact(value,['type','requestId','format','size']));
    require(Number.isSafeInteger(value.size)&&value.size>=0&&value.size<=limits.maxTextBytes,'clipboard_limits_exceeded');
    if(pushed)require(value.size<=limits.pushTextBytes,'clipboard_limits_exceeded');
    return[{size:value.size}];
  }
  require(value.format==='files'&&exact(value,['type','requestId','format','files']));
  // Files are never pushed in an on-paste pair: they are offered and pulled when pasted.
  require(!pushed,'clipboard_limits_exceeded');
  require(Array.isArray(value.files)&&value.files.length>=1&&value.files.length<=limits.maxFiles,'clipboard_limits_exceeded');
  // Pushed content is received into a private folder: in a scale pair only what the peers would not offer (shouldOffer) is pushed.
  if(limits.scaleProfile)require(value.files.length<=pushBelowEntries,'clipboard_limits_exceeded');
  let total=0;const names=new Set();
  return value.files.map(file=>{
    require(file&&typeof file==='object'&&!Array.isArray(file)&&exact(file,['name','size'])&&safeName(file.name));
    require(Number.isSafeInteger(file.size)&&file.size>=0&&file.size<=limits.maxFileBytes,'clipboard_limits_exceeded');
    total+=file.size;require(total<=limits.maxTotalBytes&&(!limits.scaleProfile||total<=pushBelowBytes),'clipboard_limits_exceeded');
    const folded=file.name.normalize('NFC').toUpperCase();require(!names.has(folded));names.add(folded);
    return{name:file.name,size:file.size};
  });
}

// A folder tree is announced by clipboard.begin (counts and totals) and then
// described by clipboard.entries pages: parents first, safe components, no
// duplicates, the declared counts and sizes exactly met. File entries become
// the data items (indices are file ordinals); folders carry no data.
function treeBegin(value,bytes,limits,offer=false){
  require(bytes<=maxManifest,'clipboard_limits_exceeded');
  require(exact(value,['type','requestId','format','entries','files','bytes','tops','skipped']));
  for(const key of ['entries','files','bytes','tops','skipped'])require(Number.isSafeInteger(value[key])&&value[key]>=0);
  require(value.skipped<=limits.maxEntries);
  require(value.entries>=1&&value.entries<=limits.maxEntries&&value.tops>=1&&value.tops<=limits.maxFiles&&value.bytes<=limits.maxTotalBytes,'clipboard_limits_exceeded');
  // A tree is never pushed in an on-paste pair; in a scale pair a tree pushed at copy time is small, a bigger one must be offered.
  if(limits.onPasteProfile===1&&!offer)require(false,'clipboard_limits_exceeded');
  if(limits.scaleProfile&&!offer)require(value.bytes<=pushBelowBytes&&value.entries<=pushBelowEntries,'clipboard_limits_exceeded');
  require(value.files<=value.entries&&value.tops<=value.entries&&(value.files>0||value.bytes===0));
  return{entries:value.entries,files:value.files,bytes:value.bytes,tops:value.tops,count:0,fileCount:0,byteSum:0,topCount:0,names:new Set(),dirs:new Set(),items:[]};
}
// An offer's entries also carry the last write time (milliseconds since 1970) of every file and folder, for the
// receiver's virtual files; a pushed tree's entries do not.
const maxMtime=253402300799999;
function treeEntries(tree,value,bytes,limits,offer=false){
  require(bytes<=maxEntriesMessage,'clipboard_limits_exceeded');
  require(exact(value,['type','requestId','start','entries'])&&value.start===tree.count&&Array.isArray(value.entries));
  require(value.entries.length>=1&&value.entries.length<=256&&tree.count+value.entries.length<=tree.entries);
  for(const entry of value.entries){
    require(entry&&typeof entry==='object'&&!Array.isArray(entry)&&typeof entry.path==='string'&&(entry.kind==='file'||entry.kind==='dir'));
    // attrs: the Windows attribute bits a pasted item may carry (read-only, hidden, system, archive).
    require(exact(entry,[...(entry.kind==='file'?['path','kind','size','attrs']:['path','kind','attrs']),...(offer?['mtime']:[])])&&Number.isSafeInteger(entry.attrs)&&entry.attrs>=0&&(entry.attrs&~0x27)===0);
    require(!offer||(Number.isSafeInteger(entry.mtime)&&entry.mtime>=0&&entry.mtime<=maxMtime));
    const parts=entry.path.split('/');
    require(parts.length>=1&&entry.path.length>0&&parts.every(part=>safeName(part,limits.scaleProfile?255:240)));
    require(parts.length<=limits.maxDepth,'clipboard_limits_exceeded');
    require(entry.path.length<=limits.maxPathChars&&(!limits.scaleProfile||entry.path.length<=pastePathChars),limits.scaleProfile?'clipboard_path_too_long':'clipboard_limits_exceeded');
    const folded=parts.map(part=>part.normalize('NFC').toUpperCase());
    if(folded.length>1)require(tree.dirs.has(folded.slice(0,-1).join('/')));else tree.topCount++;
    const key=folded.join('/');require(!tree.names.has(key));tree.names.add(key);
    if(entry.kind==='dir')tree.dirs.add(key);
    else{
      require(Number.isSafeInteger(entry.size)&&entry.size>=0&&entry.size<=limits.maxFileBytes,'clipboard_limits_exceeded');
      tree.byteSum+=entry.size;require(tree.byteSum<=tree.bytes,'clipboard_limits_exceeded');tree.fileCount++;tree.items.push({size:entry.size});
    }
    tree.count++;
  }
  if(tree.count<tree.entries)return false;
  require(tree.fileCount===tree.files&&tree.byteSum===tree.bytes&&tree.topCount===tree.tops);
  return true;
}

// Relay only: no filesystem access, remote paths, background clipboard reads or
// whole-file buffering. File chunks remain provisional until both hashes and
// the explicit commit/applied exchange complete on this exact session pair.
export class ClipboardRelay{
  constructor(relay,{pairReadyMs=15000,idleMs=30000,totalMs=900000,extendedTotalMs=6*3600*1000,maxQueue=1024*1024,pullIdleMs=15*60*1000,budgetBytes=96*1024*1024,fastBytes=64*1024*1024,fastMessages=2048,queueFastBytes=128*1024*1024,queueFastMessages=4096}={}){
    Object.assign(this,{relay,pairReadyMs,idleMs,totalMs,extendedTotalMs,maxQueue,pullIdleMs,budgetBytes,fastBytes,fastMessages,queueFastBytes,queueFastMessages});this.pairs=new Set();this.activeTransfers=0;
    this.budget=new FairByteBudget(budgetBytes);
    this.wss=new WebSocketServer({noServer:true,maxPayload:maxMessage,perMessageDeflate:false,maxFragments:128,closeTimeout:1000});
  }
  // `stored`: the assignment's permissions as read for the message being handled (one read per message: the database cannot change
  // while one message is handled, so every check of that message still sees the live grant).
  policy(session,stored=this.relay.store.clipboardPolicy(session.userId,session.desktopId)){return intersectClipboardPolicy(session.clipboardPolicy,stored);}
  available(session,stored,signinOwner=null){return session.mode==='stream'&&session.clipboardEnabled&&session.host.capabilities?.clipboard===true&&(!session.clipboardSync||session.host.capabilities?.clipboardSync===true)&&this.relay.current(session)&&session.ws?.readyState===WebSocket.OPEN&&this.relay.authorized(session)&&!clipboardPolicyRestricted(session.clipboardPolicy,stored??=this.relay.store.clipboardPolicy(session.userId,session.desktopId))&&anyClipboardAllowed(this.policy(session,stored))&&(!session.signinPolicy||this.relay.signinRedirectAvailable?.(session,signinOwner)===true);}
  // channel-resume-v1: while the stream waits for a side the channel may be dialled again.
  unavailableReason(session){return this.relay.channels?.away(session)?'clipboard_stream_away':!this.relay.current(session)||!this.relay.authorized(session)?'authorization_revoked':clipboardPolicyRestricted(session.clipboardPolicy,this.relay.store.clipboardPolicy(session.userId,session.desktopId))?'clipboard_policy_changed':'clipboard_unavailable';}
  upgrade(req,socket,head,url,reject){
    const hostId=/^\/api\/v1\/hosts\/sessions\/([^/]+)\/clipboard$/.exec(url.pathname)?.[1];
    const clientId=/^\/api\/v1\/sessions\/([^/]+)\/clipboard$/.exec(url.pathname)?.[1];
    if(hostId===undefined&&clientId===undefined)return false;
    const role=hostId===undefined?'client':'host',session=this.relay.sessions.get(hostId??clientId);
    // channel-resume-v1 (channel-resume.mjs): not now (503) while the stream is away, never again (409) after its grant was narrowed.
    const refusal=session?this.relay.channels?.refusal(session,'clipboard')??0:0;
    if(!session||(!refusal&&!this.available(session))){reject(401,'Unauthorized');return true;}
    if(role==='client'){
      const token=/^Bearer ([A-Za-z0-9_-]{43})$/.exec(req.headers.authorization??'')?.[1];
      if(!token||tokenHash(token)!==session.tokenHash){reject(401,'Unauthorized');return true;}
    }else{
      const token=/^Host ([A-Za-z0-9_-]{43})$/.exec(req.headers.authorization??'')?.[1],hash=token?tokenHash(token):null;
      const identity=hash?this.relay.store.authenticateHost(hash):null;
      if(!identity||identity.id!==session.host.identity.id||hash!==session.host.tokenHash){reject(401,'Unauthorized');return true;}
    }
    if(refusal){reject(refusal,refusal===503?'Unavailable':'Conflict');return true;}
    // The same peer dials again while its old socket is still held: that pair is gone.
    if(session.clipboardPair?.[role]&&this.relay.channels?.enabled(session))this.closePair(session.clipboardPair,'clipboard_transport_closed');
    if(session.clipboardPair?.[role]){reject(409,'Conflict');return true;}
    if(!session.clipboardPair&&this.pairs.size>=128){reject(503,'Unavailable');return true;}
    const extended=req.headers[clipboardLimitsHeader]==='2';
    // The formats profile builds on the extended limits: announcing it alone changes nothing.
    const formats=extended&&req.headers[clipboardFormatsHeader]==='2';
    const pull=formats&&req.headers[clipboardPullHeader]==='1';
    // The scale revision builds on the paste-pull profile (big copies are offered, not pushed): announcing it alone changes nothing.
    const scale=pull&&req.headers[clipboardScaleHeader]==='1';
    // The on-paste revision builds on the scale revision (offers of any size, no push of files): announcing it alone changes nothing.
    const onPaste=scale&&req.headers[clipboardOnPasteHeader]==='1';
    const settled=onPaste&&req.headers[clipboardSettledHeader]==='1';
    const intent=settled&&req.headers[clipboardIntentHeader]==='1';
    const pullQueue=onPaste&&req.headers[clipboardPullQueueHeader]==='1';
    const observedPair=session.clipboardPair;
    const guard=()=>!socket.destroyed&&this.available(session)&&session.clipboardPair===observedPair&&!observedPair?.[role]&&!observedPair?.closed&&(observedPair||this.pairs.size<128);
    const accept=()=>this.wss.handleUpgrade(req,socket,head,ws=>this.attach(ws,session,role,extended,formats,pull,scale,onPaste,settled,intent,pullQueue));
    if(session.signinPolicy&&this.relay.admitSigninRedirect)void this.relay.admitSigninRedirect(session,guard,accept,()=>reject(401,'Unauthorized'));
    else if(!session.signinPolicy&&guard())accept();else reject(401,'Unauthorized');return true;
  }
  // The limits that apply to a pair: the formats profile only when both peers
  // announced it, the extended profile when both announced that, otherwise the
  // released numbers every peer checks exactly.
  pairLimits(pair){return pair.onPasteProfile?clipboardLimitsOnPaste:pair.scaleProfile?clipboardLimitsScale:pair.pullProfile?clipboardLimitsPull:pair.formats?clipboardLimitsFormats:pair.extended?clipboardLimitsV2:enforcedV1;}
  attach(ws,session,role,extended=false,formats=false,pull=false,scale=false,onPaste=false,settled=false,intent=false,pullQueue=false){
    if(!this.available(session)){ws.close(1008,'redirect_unavailable');return;}
    let pair=session.clipboardPair;
    if(!pair){pair={session,signinOwner:this.relay.signinRedirectOwner?.(session)??null,createdAt:Date.now(),ready:false,closed:false,seen:session.clipboardSync?null:new Set(),prefix:null,highWater:0n,terminal:new Map(),transfer:null,offers:[],pulls:[],pullHigh:{client:0,host:0},pullProfile:false,onPasteProfile:false,pullQueueProfile:false,watchEnabled:false,lastSequence:null,lastChangedAt:0,changedTimer:null,pendingChanged:null};session.clipboardPair=pair;this.pairs.add(pair);}
    pair[role]=ws;pair[role+'Alive']=true;pair[role+'Extended']=extended;pair[role+'Formats']=formats;pair[role+'Pull']=pull;pair[role+'Scale']=scale;pair[role+'OnPaste']=onPaste;
    pair[role+'Settled']=settled===true;
    pair[role+'Intent']=intent===true;
    pair[role+'PullQueue']=pullQueue===true;
    const rate={at:Date.now(),messages:0,bytes:0};
    ws.on('pong',()=>{if(this.current(pair,role,ws))pair[role+'Alive']=true;});
    ws.on('error',()=>this.closePair(pair,'clipboard_transport_closed'));
    ws.on('close',()=>this.closePair(pair,'clipboard_transport_closed'));
    ws.on('message',(data,binary)=>{
      if(!this.current(pair,role,ws))return;
      pair[role+'Traffic']=true;
      const stored=this.relay.store.clipboardPolicy(session.userId,session.desktopId);
      if(!this.available(session,stored,pair.signinOwner))return this.closePair(pair,this.unavailableReason(session));
      const now=Date.now();if(now-rate.at>=1000){rate.at=now;rate.messages=0;rate.bytes=0;}
      // A windowed (formats) pair is allowed to move at link speed: more messages and bytes per second.
      const fast=pair.formats===true;
      // A pair of the scale revision moves tens of GB: it is slowed to the limit (its sockets are paused until the second is over,
      // so the window stops the sender) instead of being ended, which would be a size limit by another name. Older pairs end as before.
      const scaled=pair.scaleProfile===true;
      // A pull-queue pair moves a pasted file at the speed of the peers' 4 MiB windows.
      const queued=pair.pullQueueProfile===true;
      if(++rate.messages>(fast?(queued?this.queueFastMessages:this.fastMessages):512)||(rate.bytes+=data.length)>(fast?(queued?this.queueFastBytes:this.fastBytes):8*1024*1024)){
        if(!scaled)return this.closePair(pair,'clipboard_rate_limit');
        this.throttle(pair,rate.at+1000-now);
      }
      // The shared budget is charged per pair. Crossing it ends the pair that used the most of this second, not whichever
      // pair happened to send the message that crossed it, so a light pair is not closed because of a heavy one.
      let ended=false;
      for(const heavy of this.budget.charge(pair,data.length,now)){
        if(heavy.scaleProfile===true){this.throttle(heavy,this.budget.windowMs-(now-this.budget.at));continue;}
        this.closePair(heavy,'clipboard_rate_limit');if(heavy===pair)ended=true;
      }
      if(ended)return;
      if(binary||data.length>maxMessage||!pair.ready)return this.closePair(pair,'clipboard_invalid_message');
      try{
        const value=JSON.parse(data.toString('utf8'));
        require(value&&typeof value==='object'&&!Array.isArray(value)&&typeof value.type==='string');
        if(value.type==='clipboard.watch'||value.type==='clipboard.changed'||value.type==='clipboard.intent'){this.notification(pair,role,value,stored);return;}
        require(typeof value.requestId==='string'&&requestPattern.test(value.requestId));
        this.receive(pair,role,value,data.length,stored);
      }catch(error){this.closePair(pair,protocolErrors.has(error.message)?error.message:'clipboard_invalid_message');}
    });
    if(pair.client&&pair.host){
      pair.ready=true;pair.extended=pair.clientExtended===true&&pair.hostExtended===true;
      pair.formats=pair.extended&&pair.clientFormats===true&&pair.hostFormats===true;
      pair.pullProfile=pair.formats&&pair.clientPull===true&&pair.hostPull===true;
      pair.scaleProfile=pair.pullProfile&&pair.clientScale===true&&pair.hostScale===true;
      pair.onPasteProfile=pair.scaleProfile&&pair.clientOnPaste===true&&pair.hostOnPaste===true;
      pair.settledProfile=pair.onPasteProfile&&session.clipboardProtocol==='clipboard-sync-v1'&&session.clipboardSync===true&&pair.clientSettled===true&&pair.hostSettled===true;
      pair.intentProfile=pair.settledProfile&&pair.clientIntent===true&&pair.hostIntent===true;
      pair.pullQueueProfile=pair.onPasteProfile&&pair.clientPullQueue===true&&pair.hostPullQueue===true;
      // A released peer is sent the exact released shape and numbers; an
      // extended peer is told the pair's effective limits.
      const readyFor=role=>({type:'clipboard.ready',sessionId:session.id,protocol:'clipboard-v1',limits:pair[role+'Extended']?(pair.onPasteProfile?clipboardLimitsOnPaste:pair.scaleProfile?clipboardLimitsScale:pair.pullProfile?clipboardLimitsPull:pair.formats?clipboardLimitsFormats:pair.extended?clipboardLimitsV2:limitsV1Extended):clipboardLimits,...(pair.settledProfile?{clipboardSettled:clipboardSettledProtocol}:{}),...(pair.intentProfile?{clipboardIntent:clipboardIntentProtocol}:{}),...(pair.pullQueueProfile?{clipboardPullQueue:clipboardPullQueueProtocol}:{})});
      this.send(pair,'client',session.clipboardProtocol==='clipboard-sync-v1'?{...readyFor('client'),clipboardPolicy:this.policy(session),clipboardSync:session.clipboardSync===true}:readyFor('client'));
      this.send(pair,'host',{...readyFor('host'),clipboardPolicy:this.policy(session),clipboardSync:session.clipboardSync===true});
    }
  }
  // The reason a peer reported, as this pair may carry it: the specific-limit reasons only in a scale pair (null: not a reason at all).
  peerReason(pair,reason){return peerErrors.has(reason)?reason:scaleReasons.has(reason)?(pair.scaleProfile===true?reason:'clipboard_limits_exceeded'):null;}
  current(pair,role,ws){return !pair.closed&&pair.session.clipboardPair===pair&&pair[role]===ws;}
  // Pauses both sockets of a pair for `ms` (at most a second); the unread bytes wait in the TCP window, so the sender's own window fills and it waits.
  throttle(pair,ms){
    if(pair.closed||pair.throttled)return;
    pair.throttled=true;
    for(const role of ['client','host'])pair[role]?.pause?.();
    const timer=setTimeout(()=>{pair.throttled=false;if(pair.closed)return;for(const role of ['client','host'])pair[role+'Alive']=true;for(const role of ['client','host'])if(pair[role]?.readyState===WebSocket.OPEN)pair[role].resume();},Math.max(10,Math.min(1000,Math.ceil(ms))));
    timer.unref();
  }
  clearChanged(pair){if(pair.changedTimer)clearTimeout(pair.changedTimer);pair.changedTimer=null;pair.pendingChanged=null;}
  notification(pair,role,value,stored){
    require(value.sessionId===pair.session.id);
    const policy=this.policy(pair.session,stored),allowed=policy.textToLocal||policy.filesToLocal;
    if(value.type==='clipboard.watch'){
      require(role==='client'&&exact(value,pair.intentProfile?['type','sessionId','enabled','watchEpoch']:['type','sessionId','enabled'])&&typeof value.enabled==='boolean');
      if(pair.intentProfile){
        require(Number.isInteger(value.watchEpoch)&&value.watchEpoch>0&&value.watchEpoch<=0xffffffff);
        if(value.enabled){if(value.watchEpoch<=(pair.watchEpoch??0))return;pair.watchEpoch=value.watchEpoch;pair.lastSequence=null;pair.lastIntentSequence=null;pair.lastIntentKind=null;this.clearChanged(pair);}
        else if(value.watchEpoch!==pair.watchEpoch)return;
      }
      require(!value.enabled||allowed,'clipboard_policy_denied');
      require(!value.enabled||(pair.session.clipboardProtocol==='clipboard-sync-v1'&&pair.session.clipboardSync===true));
      pair.watchEnabled=value.enabled;if(!value.enabled)this.clearChanged(pair);
      if(pair.session.clipboardSync===true)this.send(pair,'host',value);return;
    }
    if(value.type==='clipboard.intent'){
      require(pair.intentProfile===true&&role==='host'&&exact(value,['type','sessionId','sequence','kind','watchEpoch'])&&
        Number.isInteger(value.sequence)&&value.sequence>=0&&value.sequence<=0xffffffff&&
        Number.isInteger(value.watchEpoch)&&value.watchEpoch>0&&value.watchEpoch<=0xffffffff&&['text','richtext','image','other'].includes(value.kind));
      if(value.watchEpoch<(pair.watchEpoch??0)||!pair.watchEnabled)return;
      require(value.watchEpoch===pair.watchEpoch&&allowed,'clipboard_policy_denied');
      require(value.kind==='other'||clipboardTransferAllowed(policy,value.kind,'host'),'clipboard_policy_denied');
      if(pair.lastIntentSequence===value.sequence&&pair.lastIntentKind===value.kind)return;
      pair.lastIntentSequence=value.sequence;pair.lastIntentKind=value.kind;this.clearChanged(pair);
      this.send(pair,'client',value);return;
    }
    const settled=Object.hasOwn(value,'settled');
    const changedFields=['type','sessionId','sequence',...(settled?['settled']:[]),...(pair.intentProfile?['watchEpoch']:[])];
    require(role==='host'&&exact(value,changedFields)&&(!settled||(pair.settledProfile===true&&value.settled===clipboardSettledProtocol))&&Number.isInteger(value.sequence)&&value.sequence>=0&&value.sequence<=0xffffffff);
    if(pair.intentProfile){require(Number.isInteger(value.watchEpoch)&&value.watchEpoch>0&&value.watchEpoch<=0xffffffff);if(value.watchEpoch<(pair.watchEpoch??0)||!pair.watchEnabled)return;require(value.watchEpoch===pair.watchEpoch);}
    require(allowed&&pair.watchEnabled,'clipboard_policy_denied');
    require(pair.session.clipboardProtocol==='clipboard-sync-v1'&&pair.session.clipboardSync===true);
    // DWORD clipboard sequence wraps. Equality is a duplicate notification;
    // ordering belongs to this live session/peer, never a numeric comparison.
    if(pair.lastSequence===value.sequence)return;pair.lastSequence=value.sequence;
    pair.pendingChanged=value;
    const emit=()=>{
      pair.changedTimer=null;
      if(pair.closed||!pair.watchEnabled||!this.available(pair.session,undefined,pair.signinOwner)){this.clearChanged(pair);return;}
      const current=this.policy(pair.session);if(!current.textToLocal&&!current.filesToLocal){this.clearChanged(pair);return;}
      const pending=pair.pendingChanged;pair.pendingChanged=null;
      if(pending){pair.lastChangedAt=Date.now();this.send(pair,'client',pending);}
    };
    const wait=100-(Date.now()-pair.lastChangedAt);
    if(wait<=0){if(pair.changedTimer)clearTimeout(pair.changedTimer);emit();}
    else if(!pair.changedTimer){pair.changedTimer=setTimeout(emit,wait);pair.changedTimer.unref();}
  }
  // `encoded`: the message already serialized (a checked chunk, see receivePull), else JSON.stringify(value).
  send(pair,role,value,encoded=JSON.stringify(value)){
    if(pair.closed)return false;
    // A pull-queue pair's 4 MiB window is about 5.4 MiB of JSON: it may queue eight times as much.
    const ws=pair[role],size=Buffer.byteLength(encoded),queue=this.maxQueue*(pair.pullQueueProfile===true?8:pair.formats===true?4:1);
    // A window of 1 MiB raw data is about 1.4 MiB of JSON: a windowed pair may queue four times as much.
    if(ws?.readyState!==WebSocket.OPEN||size>maxMessage||ws.bufferedAmount+size>queue){this.closePair(pair,'clipboard_backpressure');return false;}
    ws.send(encoded,error=>{if(error)this.closePair(pair,'clipboard_transport_closed');});return true;
  }
  reserve(pair,requestId,from,read=false){
    require(!pair.transfer,'clipboard_busy');
    require(!(pair.pullProfile===true&&pullPattern.test(requestId)));
    const identity=pair.session.clipboardSync?syncIdentity(requestId):null;
    if(identity)require((pair.prefix===null||identity.prefix===pair.prefix)&&identity.counter>pair.highWater);
    else require(!pair.seen.has(requestId)&&pair.seen.size<256);
    require(this.activeTransfers<32,'clipboard_busy');
    if(identity){pair.prefix=identity.prefix;pair.highWater=identity.counter;}else pair.seen.add(requestId);
    this.activeTransfers++;
    // index/offset is the position the sender has sent up to; units are the sent
    // chunks / item ends the receiver has not acknowledged yet. A released pair
    // has a window of exactly one unit (stop and wait).
    const now=Date.now(),windowed=pair.formats===true;
    return pair.transfer={requestId,from,to:from==='host'?'client':'host',stage:read?'begin':'data',startedAt:now,at:now,index:0,offset:0,units:[],inflight:0,windowUnits:windowed?clipboardLimitsFormats.windowUnits:1,windowBytes:windowed?clipboardLimitsFormats.windowBytes:clipboardLimits.chunkBytes,decoder:null,textUnits:0,textInvalid:false};
  }
  release(pair){if(pair.transfer){pair.transfer=null;this.activeTransfers--;}}
  remember(pair,requestId,value){
    pair.terminal.set(requestId,{...value,drops:0});
    while(pair.terminal.size>256)pair.terminal.delete(pair.terminal.keys().next().value);
  }
  receive(pair,role,value,bytes,stored){
    const policy=this.policy(pair.session,stored);
    if(pair.transfer?.format)require(clipboardTransferAllowed(policy,pair.transfer.format,pair.transfer.from),'clipboard_policy_denied');
    for(const pull of pair.pulls)require(clipboardTransferAllowed(policy,pull.format,pull.from),'clipboard_policy_denied');
    // Pull requests and the data of a pull carry `pull-<n>` ids and have their own state (one active pull per pair, or a pull queue).
    if(pair.pullProfile===true&&pullPattern.test(value.requestId)){this.receivePull(pair,role,value,bytes,policy);return;}
    // On-paste revision: two messages outside any transfer. They name an offer (withdraw) or a waiting episode (paste.waiting) and
    // are not transfer requests, so they stay out of the request-id bookkeeping below.
    if(value.type==='clipboard.offer.withdraw'){this.withdrawOffer(pair,role,value);return;}
    if(value.type==='clipboard.paste.waiting'){
      require(pair.onPasteProfile===true&&role==='host'&&exact(value,['type','requestId','active'])&&waitingPattern.test(value.requestId)&&typeof value.active==='boolean');
      this.send(pair,'client',value);return;
    }
    const terminal=pair.terminal.get(value.requestId);
    if(terminal?.deadline&&pair.transfer?.requestId!==value.requestId){
      // A deadline crosses a bounded amount of data/control already in flight,
      // including stop-and-wait peers. Late data must not end the fresh copy.
      const sourceStart=['clipboard.begin','clipboard.offer'].includes(value.type)&&role===terminal.from;
      const retiredAck=value.type==='clipboard.applied'
        ? terminal.status==='uncertain'&&terminal.to===role&&exact(value,['type','requestId'])
        : retiredAcknowledgement(value,this.pairLimits(pair));
      if(sourceStart||inFlightTypes.has(value.type)||retiredAck){
        require(++terminal.drops<=(pair.formats===true?clipboardLimitsFormats.windowUnits*2:8));return;
      }
    }
    // With a window, a cancel or error crosses up to a window of messages already in flight.
    // They are dropped (bounded), not a protocol violation that would close the pair.
    if(pair.formats===true&&terminal&&pair.transfer?.requestId!==value.requestId&&inFlightTypes.has(value.type)){
      require(++terminal.drops<=clipboardLimitsFormats.windowUnits*2);return;
    }
    if(pair.session.clipboardSync){
      const identity=syncIdentity(value.requestId);
      require(pair.prefix===null||identity.prefix===pair.prefix);
      const starts=value.type==='clipboard.read'||((value.type==='clipboard.begin'||value.type==='clipboard.offer')&&role==='client');
      if(!starts&&pair.transfer?.requestId!==value.requestId){
        require(pair.prefix!==null&&identity.counter<=pair.highWater&&retiredAcknowledgement(value,this.pairLimits(pair)));
        if(value.type==='clipboard.cancel'&&terminal?.status==='complete')this.send(pair,role,{type:'clipboard.complete',requestId:value.requestId});
        return;
      }
    }
    // Completion may race a user's cancel on the other socket. Retired ACKs
    // cannot finish, cancel or corrupt a later transfer on this same pair.
    if(value.type==='clipboard.applied'&&terminal?.status==='uncertain'&&terminal.to===role&&exact(value,['type','requestId']))return;
    if(value.type==='clipboard.cancel'&&terminal?.status==='complete'&&exact(value,['type','requestId'])){this.send(pair,role,{type:'clipboard.complete',requestId:value.requestId});return;}
    if(value.type==='clipboard.read'){
      const settledRead=Object.hasOwn(value,'expectedSequence')||Object.hasOwn(value,'textImageOnly');
      require(role==='client'&&(settledRead?pair.settledProfile===true&&exact(value,['type','requestId','expectedSequence','textImageOnly',...(pair.intentProfile?['watchEpoch']:[])])&&typeof value.textImageOnly==='boolean'&&(value.textImageOnly||pair.intentProfile===true)&&Number.isInteger(value.expectedSequence)&&value.expectedSequence>=0&&value.expectedSequence<=0xffffffff:(exact(value,['type','requestId'])||exact(value,['type','requestId','format']))));
      if(settledRead&&pair.intentProfile)require(Number.isInteger(value.watchEpoch)&&value.watchEpoch===pair.watchEpoch&&pair.watchEnabled);
      require(value.format===undefined||['text','files'].includes(value.format));
      require(value.format===undefined?(policy.textToLocal||policy.filesToLocal):clipboardTransferAllowed(policy,value.format,'host'),'clipboard_policy_denied');
      const transfer=this.reserve(pair,value.requestId,'host',true);transfer.requestedFormat=value.format;
      transfer.textImageOnly=settledRead&&value.textImageOnly===true;
      this.send(pair,'host',value);return;
    }
    // An offer announces files and folders by their manifest only; the receiving peer pulls the bytes when
    // the user pastes. It is a tree manifest (clipboard.entries pages follow) that ends the transfer
    // instead of streaming data: the receiver answers clipboard.applied once its data object is published.
    if(value.type==='clipboard.offer'){
      if(role==='host'&&pair.transfer?.textImageOnly)require(['text','richtext','image'].includes(value.format));
      // Text, rich text and images can be offered only in an on-paste pair (the offer replaces a push too big for it).
      require(pair.pullProfile===true&&(value.format==='tree'||(pair.onPasteProfile===true&&offerFormats.has(value.format))));
      require(clipboardTransferAllowed(policy,value.format,role),'clipboard_policy_denied');
      const limits=this.pairLimits(pair),tree=value.format==='tree'?treeBegin(value,bytes,limits,true):null;
      const items=tree?[]:manifest(value,bytes,limits,true,true);let transfer=pair.transfer;
      if(role==='client')transfer=this.reserve(pair,value.requestId,'client');
      else require(transfer?.stage==='begin'&&transfer.requestId===value.requestId&&transfer.from===role&&(transfer.requestedFormat===undefined||transfer.requestedFormat===clipboardFormatClass(value.format)));
      // A tree offer waits for its clipboard.entries pages; the others are complete with this message.
      Object.assign(transfer,{items,format:value.format,tree,stage:tree?'entries':'offered',mode:'offer',hash:createHash('sha256'),at:Date.now()});
      if(!tree)this.registerOffer(pair,transfer);
      this.send(pair,transfer.to,value);return;
    }
    if(value.type==='clipboard.begin'){
      if(role==='host'&&pair.transfer?.textImageOnly)require(['text','richtext','image'].includes(value.format));
      require(['text','files'].includes(value.format)||(pair.formats===true&&formatsOnly.has(value.format)));
      require(clipboardTransferAllowed(policy,value.format,role),'clipboard_policy_denied');
      const limits=this.pairLimits(pair),tree=value.format==='tree'?treeBegin(value,bytes,limits):null;
      const items=tree?[]:manifest(value,bytes,limits,pair.formats===true);let transfer=pair.transfer;
      if(role==='client')transfer=this.reserve(pair,value.requestId,'client');
      else require(transfer?.stage==='begin'&&transfer.requestId===value.requestId&&transfer.from===role&&(transfer.requestedFormat===undefined||transfer.requestedFormat===clipboardFormatClass(value.format)));
      Object.assign(transfer,{items,format:value.format,tree,stage:tree?'entries':'data',hash:createHash('sha256'),at:Date.now()});
      this.send(pair,transfer.to,value);return;
    }
    const transfer=pair.transfer;
    require(transfer&&value.requestId===transfer.requestId);transfer.at=Date.now();
    if(value.type==='clipboard.cancel'){
      require(exact(value,['type','requestId']));
      if(transfer.stage==='applied'){
        this.remember(pair,transfer.requestId,{status:'uncertain',to:transfer.to});
        const uncertain={type:'clipboard.error',requestId:transfer.requestId,reason:'clipboard_commit_uncertain'};
        this.send(pair,'client',uncertain);this.send(pair,'host',uncertain);
      }else{this.remember(pair,transfer.requestId,{status:'cancelled',to:transfer.to});this.send(pair,'client',value);this.send(pair,'host',value);}
      if(transfer.mode==='offer'&&transfer.stage!=='applied')this.dropOffer(pair,transfer.requestId);
      this.release(pair);return;
    }
    if(value.type==='clipboard.error'){
      const reason=this.peerReason(pair,value.reason);
      require(exact(value,['type','requestId','reason'])&&reason!==null);
      if(transfer.mode==='offer')this.dropOffer(pair,transfer.requestId);
      this.remember(pair,transfer.requestId,{status:'failed',to:transfer.to});for(const peer of ['client','host'])this.send(pair,peer,pair[peer+'Extended']||!legacyReason[reason]?{...value,reason}:{...value,reason:legacyReason[reason]});this.release(pair);return;
    }
    // Acknowledgements are cumulative: each names the exact end of a sent chunk
    // (index, offset) or item end and releases that unit and every earlier one.
    const acknowledge=(kind,index,offset)=>{
      const at=transfer.units.findIndex(unit=>unit.kind===kind&&unit.index===index&&unit.end===offset);
      require(at>=0,'clipboard_out_of_order');
      for(const unit of transfer.units.splice(0,at+1))transfer.inflight-=unit.bytes;
    };
    if(value.type==='clipboard.chunk.ack'){
      require(role===transfer.to&&transfer.stage==='data'&&exact(value,['type','requestId','index','offset']));
      acknowledge('chunk',value.index,value.offset);this.send(pair,transfer.from,value);return;
    }
    if(value.type==='clipboard.item.ack'){
      require(role===transfer.to&&transfer.stage==='data'&&exact(value,['type','requestId','index']));
      acknowledge('end',value.index,-1);this.send(pair,transfer.from,value);return;
    }
    if(value.type==='clipboard.entries'){
      require(role===transfer.from&&transfer.stage==='entries'&&transfer.tree);
      if(treeEntries(transfer.tree,value,bytes,this.pairLimits(pair),transfer.mode==='offer')){
        transfer.items=transfer.tree.items;
        if(transfer.mode==='offer'){transfer.stage='offered';this.registerOffer(pair,transfer);}else transfer.stage='data';
      }
      this.send(pair,transfer.to,value);return;
    }
    if(value.type==='clipboard.applied'){
      require(role===transfer.to&&(transfer.stage==='applied'||transfer.stage==='offered')&&exact(value,['type','requestId']));
      this.remember(pair,transfer.requestId,{status:'complete',to:transfer.to});
      const complete={type:'clipboard.complete',requestId:transfer.requestId};this.send(pair,'client',complete);this.send(pair,'host',complete);this.release(pair);return;
    }
    require(role===transfer.from&&transfer.stage==='data');
    if(value.type==='clipboard.commit'){
      require(exact(value,['type','requestId']));require(transfer.units.length===0&&transfer.index===transfer.items.length,'clipboard_out_of_order');
      transfer.stage='applied';this.send(pair,transfer.to,value);return;
    }
    const limits=this.pairLimits(pair);
    require(transfer.units.length<transfer.windowUnits&&transfer.index<transfer.items.length&&value.index===transfer.index,'clipboard_out_of_order');
    const item=transfer.items[transfer.index],plain=transfer.format==='text'||(transfer.format==='richtext'&&item.kind==='text');
    if(value.type==='clipboard.chunk'){
      require(exact(value,['type','requestId','index','offset','data'])&&typeof value.data==='string'&&value.data.length<=Math.ceil(limits.chunkBytes/3)*4);
      require(Number.isSafeInteger(value.offset)&&value.offset===transfer.offset,'clipboard_out_of_order');
      const decoded=Buffer.from(value.data,'base64');
      require(decoded.length>=1&&decoded.length<=limits.chunkBytes&&decoded.toString('base64')===value.data&&decoded.length<=item.size-transfer.offset,'clipboard_limits_exceeded');
      require(transfer.inflight+decoded.length<=transfer.windowBytes,'clipboard_out_of_order');
      transfer.hash.update(decoded);transfer.offset+=decoded.length;
      // Text is checked as it streams (valid UTF-8, no NUL, the unit limit) instead of being kept: 64 MiB per transfer would be too much to hold.
      if(plain){
        transfer.decoder??=new TextDecoder('utf-8',{fatal:true,ignoreBOM:true});
        // The verdict is given at the item end, as before: the chunks are forwarded meanwhile.
        if(!transfer.textInvalid){
          try{const piece=transfer.decoder.decode(decoded,{stream:true});transfer.textUnits+=piece.length;if(piece.includes('\0')||transfer.textUnits>limits.maxTextUnits)transfer.textInvalid=true;}
          catch{transfer.textInvalid=true;}
        }
      }
      transfer.units.push({kind:'chunk',index:transfer.index,end:transfer.offset,bytes:decoded.length});transfer.inflight+=decoded.length;
      this.send(pair,transfer.to,value);return;
    }
    if(value.type==='clipboard.item.end'){
      require(exact(value,['type','requestId','index','sha256'])&&typeof value.sha256==='string'&&/^[a-f0-9]{64}$/.test(value.sha256));
      require(transfer.offset===item.size,'clipboard_out_of_order');require(transfer.hash.digest('hex')===value.sha256,'clipboard_hash_mismatch');
      if(plain){
        if(!transfer.textInvalid){
          try{const piece=(transfer.decoder??=new TextDecoder('utf-8',{fatal:true,ignoreBOM:true})).decode();transfer.textUnits+=piece.length;if(piece.includes('\0')||transfer.textUnits>limits.maxTextUnits)transfer.textInvalid=true;}
          catch{transfer.textInvalid=true;}
        }
        require(!transfer.textInvalid,'clipboard_text_invalid');transfer.decoder=null;transfer.textUnits=0;
      }
      // The next item may start at once; its first chunk must still fit the window.
      transfer.units.push({kind:'end',index:transfer.index,end:-1,bytes:0});
      transfer.index++;transfer.offset=0;transfer.hash=createHash('sha256');
      this.send(pair,transfer.to,value);return;
    }
    throw new Error('clipboard_invalid_message');
  }
  // The manifests a peer currently offers for pulling: the latest clipboardLimitsPull.maxOffers per source
  // peer. They live exactly as long as the pair (the session); an older offer is retired by a newer one.
  registerOffer(pair,transfer){
    const mine=pair.offers.filter(offer=>offer.from===transfer.from);
    while(mine.length>=clipboardLimitsPull.maxOffers)pair.offers.splice(pair.offers.indexOf(mine.shift()),1);
    pair.offers.push({id:transfer.requestId,from:transfer.from,items:transfer.items,format:transfer.format});
  }
  dropOffer(pair,id){const at=pair.offers.findIndex(offer=>offer.id===id);if(at>=0)pair.offers.splice(at,1);}
  // The offering peer's clipboard changed (or it no longer serves the offer): the offer is retired for both peers. A pull of it that
  // is running ends for the requester with clipboard_offer_expired. An offer the relay does not know (already retired by a newer
  // one) is ignored, so a late withdraw is harmless.
  withdrawOffer(pair,role,value){
    require(pair.onPasteProfile===true&&exact(value,['type','requestId']));
    const at=pair.offers.findIndex(offer=>offer.id===value.requestId&&offer.from===role);
    if(at<0)return;
    pair.offers.splice(at,1);
    for(const pull of pair.pulls.filter(candidate=>candidate.offerId===value.requestId)){
      this.remember(pair,pull.requestId,{status:'failed',to:pull.to});this.releasePull(pair,pull);
      this.send(pair,pull.to,{type:'clipboard.error',requestId:pull.requestId,reason:'clipboard_offer_expired'});
    }
    this.send(pair,role==='client'?'host':'client',value);
  }
  // A pull is over (complete, failed or cancelled); the next one in the queue is the oldest now, and its idle clock starts.
  releasePull(pair,pull){
    const at=pair.pulls.indexOf(pull);if(at<0)return;
    pair.pulls.splice(at,1);this.activeTransfers--;
    if(at===0&&pair.pulls.length)pair.pulls[0].at=Date.now();
  }
  releasePulls(pair){this.activeTransfers-=pair.pulls.length;pair.pulls=[];}
  // Acknowledgements are cumulative: each names the exact end of a sent chunk (index, offset) or item end and
  // releases that unit and every earlier one.
  acknowledgeUnits(transfer,kind,index,offset){
    const at=transfer.units.findIndex(unit=>unit.kind===kind&&unit.index===index&&unit.end===offset);
    require(at>=0,'clipboard_out_of_order');
    for(const unit of transfer.units.splice(0,at+1))transfer.inflight-=unit.bytes;
  }
  // Pull: the peer that received an offer asks the offering peer for the bytes of one item from an offset
  // (clipboard.pull); the answer is the ordinary windowed clipboard.chunk / clipboard.item.end stream under the
  // pull's own id, acknowledged by the requester. One pull is active per pair; a requester serialises its own.
  // A pull-queue pair: up to pullQueueDepth pulls of one requester, served in order (data only for the oldest).
  receivePull(pair,role,value,bytes,policy){
    require(pair.pullProfile===true);
    const id=value.requestId,peer=role==='client'?'host':'client';
    if(value.type==='clipboard.pull'){
      require(pullPattern.test(id)&&exact(value,['type','requestId','offerId','index','offset'])&&typeof value.offerId==='string'&&requestPattern.test(value.offerId));
      require(Number.isSafeInteger(value.index)&&value.index>=0&&Number.isSafeInteger(value.offset)&&value.offset>=0);
      require(id[4]===(role==='client'?'c':'h'));
      const n=Number(id.slice(6));require(n>pair.pullHigh[role]);pair.pullHigh[role]=n;
      // A request the relay cannot honour is answered to the requester alone and does not end the channel.
      const refuse=reason=>{this.remember(pair,id,{status:'failed',to:role});this.send(pair,role,{type:'clipboard.error',requestId:id,reason});};
      const offer=pair.offers.find(candidate=>candidate.id===value.offerId);
      if(!offer)return refuse('clipboard_offer_expired');
      require(offer.from!==role);
      require(clipboardTransferAllowed(policy,offer.format,offer.from),'clipboard_policy_denied');
      require(value.index<offer.items.length&&value.offset<=offer.items[value.index].size);
      const queue=pair.pullQueueProfile===true;
      if(pair.pulls.length>=(queue?pullQueueDepth:1)||pair.pulls.some(pull=>pull.to!==role)||this.activeTransfers>=32)return refuse('clipboard_busy');
      const now=Date.now();this.activeTransfers++;
      pair.pulls.push({requestId:id,from:offer.from,to:role,offerId:offer.id,format:offer.format,items:offer.items,index:value.index,offset:value.offset,startedAt:now,at:now,units:[],inflight:0,windowUnits:queue?pullQueueWindowUnits:clipboardLimitsFormats.windowUnits,windowBytes:queue?pullQueueWindowBytes:clipboardLimitsFormats.windowBytes,hash:createHash('sha256')});
      this.send(pair,offer.from,value);return;
    }
    const pull=pair.pulls.find(candidate=>candidate.requestId===id);
    if(!pull){
      // What the pull's two peers still had in flight when it ended (a cancel, an error, the last acks) is
      // dropped, bounded, instead of being a protocol violation that would close the channel.
      const terminal=pair.terminal.get(id);
      require(terminal&&['clipboard.chunk','clipboard.item.end','clipboard.chunk.ack','clipboard.item.ack','clipboard.cancel','clipboard.error'].includes(value.type)&&++terminal.drops<=(pair.pullQueueProfile===true?pullQueueWindowUnits:clipboardLimitsFormats.windowUnits)*2);
      return;
    }
    pull.at=Date.now();
    if(value.type==='clipboard.cancel'){
      require(exact(value,['type','requestId']));
      this.remember(pair,id,{status:'cancelled',to:pull.to});this.releasePull(pair,pull);this.send(pair,peer,value);return;
    }
    if(value.type==='clipboard.error'){
      const reason=pullReasons.has(value.reason)?value.reason:this.peerReason(pair,value.reason);
      require(exact(value,['type','requestId','reason'])&&reason!==null);
      this.remember(pair,id,{status:'failed',to:pull.to});this.releasePull(pair,pull);this.send(pair,peer,{...value,reason});return;
    }
    if(value.type==='clipboard.chunk.ack'){
      require(role===pull.to&&exact(value,['type','requestId','index','offset']));
      this.acknowledgeUnits(pull,'chunk',value.index,value.offset);this.send(pair,pull.from,value);return;
    }
    if(value.type==='clipboard.item.ack'){
      require(role===pull.to&&exact(value,['type','requestId','index']));
      this.acknowledgeUnits(pull,'end',value.index,-1);this.send(pair,pull.from,value);return;
    }
    // Data is served in order: only the oldest pull of the queue may carry any.
    require(role===pull.from&&pull===pair.pulls[0]&&value.index===pull.index&&pull.units.length<pull.windowUnits,'clipboard_out_of_order');
    const item=pull.items[pull.index],limits=clipboardLimitsFormats;
    if(value.type==='clipboard.chunk'){
      require(exact(value,['type','requestId','index','offset','data'])&&typeof value.data==='string'&&value.data.length<=Math.ceil(limits.chunkBytes/3)*4);
      require(Number.isSafeInteger(value.offset)&&value.offset===pull.offset,'clipboard_out_of_order');
      const decoded=Buffer.from(value.data,'base64');
      require(decoded.length>=1&&decoded.length<=limits.chunkBytes&&decoded.toString('base64')===value.data&&decoded.length<=item.size-pull.offset,'clipboard_limits_exceeded');
      require(pull.inflight+decoded.length<=pull.windowBytes,'clipboard_out_of_order');
      pull.hash.update(decoded);pull.offset+=decoded.length;
      pull.units.push({kind:'chunk',index:pull.index,end:pull.offset,bytes:decoded.length});pull.inflight+=decoded.length;
      // Rebuilt from the checked fields (a pull id, two safe integers, canonical base64: nothing to escape) instead of JSON.stringify
      // of the 61 KB string; the receiver gets the same message.
      this.send(pair,pull.to,value,`{"type":"clipboard.chunk","requestId":"${id}","index":${value.index},"offset":${value.offset},"data":"${value.data}"}`);return;
    }
    if(value.type==='clipboard.item.end'){
      // The hash covers the bytes of this pull (the whole item when it started at offset 0).
      require(exact(value,['type','requestId','index','sha256'])&&typeof value.sha256==='string'&&/^[a-f0-9]{64}$/.test(value.sha256));
      require(pull.offset===item.size,'clipboard_out_of_order');require(pull.hash.digest('hex')===value.sha256,'clipboard_hash_mismatch');
      this.remember(pair,id,{status:'complete',to:pull.to});this.releasePull(pair,pull);
      this.send(pair,peer,value);return;
    }
    throw new Error('clipboard_invalid_message');
  }
  closePair(pair,reason){
    if(pair.closed)return;
    this.releasePulls(pair);
    if(pair.transfer?.stage==='applied')reason='clipboard_commit_uncertain';
    pair.closed=true;this.clearChanged(pair);this.release(pair);this.pairs.delete(pair);
    if(pair.session.clipboardPair===pair)pair.session.clipboardPair=null;
    this.relay.channels?.closed(pair.session,'clipboard',reason);
    for(const role of ['client','host']){
      const ws=pair[role];if(!ws)continue;
      if(ws.readyState===WebSocket.OPEN){
        const value=JSON.stringify({type:'clipboard.error',reason});
        if(ws.bufferedAmount+Buffer.byteLength(value)<=this.maxQueue)ws.send(value);
        ws.close(1008,reason.slice(0,100));
      }else ws.terminate();
    }
  }
  closeSession(session,reason){if(session.clipboardPair)this.closePair(session.clipboardPair,reason);this.relay.channels?.closed(session,'clipboard',reason);}
  revalidate(){
    const now=Date.now();for(const pair of this.pairs){
      if(!this.available(pair.session,undefined,pair.signinOwner)){
        const reason=this.unavailableReason(pair.session);
        // Login expiry can cross its deadline after Relay's main media check
        // earlier in this sweep. Retire this same controller now; do not leave
        // an apparently live stream after both clipboard peers have closed.
        if(reason==='authorization_revoked'&&this.relay.sessions.get(pair.session.id)===pair.session&&!this.relay.resume?.held(pair.session))this.relay.stop(pair.session,reason);
        else this.closePair(pair,reason);
        continue;
      }
      if(!pair.ready&&now-pair.createdAt>this.pairReadyMs){this.closePair(pair,'clipboard_pair_timeout');continue;}
      const pull=pair.pulls[0];
      if(pull&&now-pull.at>this.pullIdleMs){
        // A pull nobody continues (a paused Explorer dialog is allowed to wait far longer than a push) ends alone.
        const cancel={type:'clipboard.cancel',requestId:pull.requestId};
        this.remember(pair,pull.requestId,{status:'cancelled',to:pull.to});this.releasePull(pair,pull);this.send(pair,'client',cancel);this.send(pair,'host',cancel);
      }
            // A transfer ends when it stops making progress (idle). A scale pair has no total duration: a 64 MiB text or a slow link is not an error
      // while bytes keep moving (every message refreshes transfer.at); older pairs keep their total.
      const transfer=pair.transfer;
      if(transfer&&(now-transfer.at>this.idleMs||(pair.scaleProfile!==true&&now-transfer.startedAt>(pair.extended?this.extendedTotalMs:this.totalMs)))){
        // An abandoned copy ends alone, just like an expired pull above. Keep
        // the authenticated pair for the next copy and retire late packets.
        // A commit already sent to the receiver remains explicitly uncertain.
        const uncertain=transfer.stage==='applied';
        const failure={type:'clipboard.error',requestId:transfer.requestId,reason:uncertain?'clipboard_commit_uncertain':'clipboard_timeout'};
        this.remember(pair,transfer.requestId,{status:uncertain?'uncertain':'failed',to:transfer.to,from:transfer.from,deadline:true});
        if(transfer.mode==='offer')this.dropOffer(pair,transfer.requestId);
        this.release(pair);this.send(pair,'client',failure);this.send(pair,'host',failure);
      }
    }
  }
  heartbeat(){
    for(const pair of this.pairs)for(const role of ['client','host']){
      const ws=pair[role];if(ws?.readyState!==WebSocket.OPEN)continue;
      // A throttled pair is not read for up to a second: its silence (and an unread pong) is the relay's doing, not the peer's.
      if(pair.throttled){pair[role+'Alive']=true;continue;}
      // A peer that sent anything since the last check is alive and is not
      // pinged: the Host's WinHTTP could write its pong inside a large chunk
      // frame it is sending (relay.mjs checkHeartbeat).
      if(pair[role+'Traffic']){pair[role+'Traffic']=false;pair[role+'Alive']=true;continue;}
      if(pair[role+'Alive']===false){this.closePair(pair,'clipboard_transport_closed');break;}
      pair[role+'Alive']=false;ws.ping();
    }
  }
  close(){for(const pair of this.pairs)this.closePair(pair,'server_stopping');this.wss.close();}
}

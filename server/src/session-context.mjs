import { createHash, randomBytes as secureRandomBytes } from 'node:crypto';
import { HttpError } from './errors.mjs';

const contextPattern=/^[A-Za-z0-9_-]{43}$/;
const digest=value=>createHash('sha256').update(value).digest('hex');
const ttlMs=120000,maximumOffers=1024,offersPerAccount=4;
const deniedText=/[\x00-\x1f\x7f-\x9f\u061c\u200e\u200f\u2028\u2029\u202a-\u202e\u2066-\u2069]/;
function label(value,limit){if(typeof value!=='string'||!value.trim()||!value.isWellFormed()||[...value].length>limit||deniedText.test(value))throw new HttpError(503,'session_context_unavailable');return value;}
function desktop(value){if(!value||typeof value.id!=='string'||!/^[A-Za-z0-9][A-Za-z0-9_-]{0,79}$/.test(value.id))throw new HttpError(503,'session_context_unavailable');return{id:value.id,name:label(value.name,128)};}
function iso(value){return Number.isSafeInteger(value)&&value>=0&&value<=8640000000000000?new Date(value).toISOString():null;}

// Negotiated on the two new native-client request paths only. No header is
// an authority claim, and legacy requests retain their original DTO shape.
export function requestedSessionContext(req,name){
 const value=req.headers[name];if(value===undefined)return false;
 const count=(req.rawHeaders??[]).filter((v,i)=>i%2===0&&v.toLowerCase()===name).length;
 if(value!=='v1'||count>1)throw new HttpError(400,'invalid_context_protocol');return true;
}

// The callback is server-owned; rows are the exact password-verified challenge
// snapshot. Only explicit safe keys are returned, never token hashes or IPs.
export function existingLoginView(user,rows,desktopsForSession){
 if(!Array.isArray(rows)||rows.length<1||rows.length>5||typeof desktopsForSession!=='function')throw new HttpError(503,'session_context_unavailable');
 let remaining=5;
 const sessions=rows.map(row=>{
  const createdAt=iso(row.createdAt);if(!createdAt||typeof row.issuer!=='string'||!/^[A-Za-z0-9_-]{1,64}$/.test(row.issuer))throw new HttpError(503,'session_context_unavailable');
  const found=desktopsForSession(row);if(!Array.isArray(found))throw new HttpError(503,'session_context_unavailable');
  const unique=new Map(found.map(value=>{const safe=desktop(value);return[safe.id,safe];}));
  const desktops=[...unique.values()].sort((a,b)=>a.id.localeCompare(b.id)).slice(0,remaining);remaining-=desktops.length;
  return{issuer:row.issuer,createdAt,desktops};
 });
 return{username:label(user.username,64),sessionCount:sessions.length,sessions};
}

function snapshot(sessions,identify){
 if(!Array.isArray(sessions)||sessions.length<1||sessions.length>5)throw new HttpError(409,'takeover_context_stale');
 const seen=new Set();
 return sessions.map(session=>{
  if(!session||typeof session.id!=='string'||!session.id||seen.has(session.id)||typeof session.userId!=='string'||typeof session.tokenHash!=='string'||!/^[a-f0-9]{64}$/.test(session.tokenHash)||typeof session.desktopId!=='string')throw new HttpError(409,'takeover_context_stale');
  seen.add(session.id);return{identity:identify(session),id:session.id,userId:session.userId,tokenHash:session.tokenHash,desktopId:session.desktopId};
 }).sort((a,b)=>a.id.localeCompare(b.id));
}
function same(a,b){return a.length===b.length&&a.every((row,i)=>['identity','id','userId','tokenHash','desktopId'].every(key=>row[key]===b[i][key]));}

// Bounded, timer-free and one-use. Consent belongs to one authenticated token,
// one desktop and the exact live objects/identities the user reviewed. The
// Relay must recheck permissions then consume synchronously before stopping.
export class TakeoverContexts {
 #offers=new Map();#identities=new WeakMap();#now;#random;
 constructor({now=Date.now,randomBytes=secureRandomBytes}={}){if(typeof now!=='function'||typeof randomBytes!=='function')throw Error('invalid_takeover_context_options');this.#now=now;this.#random=randomBytes;}
 #time(){const now=this.#now();if(!Number.isSafeInteger(now)||now<0||now>8640000000000000-ttlMs)throw new HttpError(503,'session_context_unavailable');return now;}
 #prune(now){for(const[key,value]of this.#offers)if(now<value.at||now>=value.until)this.#offers.delete(key);}
 #snapshot(sessions){return snapshot(sessions,session=>{let identity=this.#identities.get(session);if(!identity){identity=Symbol();this.#identities.set(session,identity);}return identity;});}
 issue({userId,tokenHash,desktop:target,sessions,usernameForSession}){
  const now=this.#time();this.#prune(now);const observed=this.#snapshot(sessions),safeDesktop=desktop(target);
  if(typeof userId!=='string'||!userId||typeof tokenHash!=='string'||!/^[a-f0-9]{64}$/.test(tokenHash)||observed.some(row=>row.desktopId!==target.id))throw new HttpError(409,'takeover_context_stale');
  const views=[...sessions].sort((a,b)=>a.id.localeCompare(b.id)).map(session=>({username:label(usernameForSession(session),64),createdAt:iso(session.createdAt),deviceLabel:null}));
  const own=[...this.#offers].filter(([,value])=>value.userId===userId);if(own.length>=offersPerAccount)this.#offers.delete(own[0][0]);
  if(this.#offers.size>=maximumOffers)throw new HttpError(503,'session_context_unavailable');
  for(let attempt=0;attempt<4;attempt++){
   const bytes=this.#random(32);if(!Buffer.isBuffer(bytes)||bytes.length!==32)throw new HttpError(503,'session_context_unavailable');const context=bytes.toString('base64url'),key=digest(context);if(this.#offers.has(key))continue;
   this.#offers.set(key,{userId,tokenHash,desktopId:target.id,observed,at:now,until:now+ttlMs});
   return{protocol:'takeover-context-v1',context,expiresInSeconds:120,desktop:safeDesktop,sessions:views};
  }
  throw new HttpError(503,'session_context_unavailable');
 }
 consume({context,userId,tokenHash,desktopId,sessions}){
  if(typeof context!=='string'||!contextPattern.test(context))throw new HttpError(400,'invalid_takeover_context');
  const now=this.#time(),key=digest(context),bound=this.#offers.get(key);this.#offers.delete(key);
  if(!bound)throw new HttpError(409,'takeover_context_stale');
  if(now<bound.at||now>=bound.until)throw new HttpError(410,'takeover_context_expired');
  if(bound.userId!==userId||bound.tokenHash!==tokenHash||bound.desktopId!==desktopId||!same(bound.observed,this.#snapshot(sessions)))throw new HttpError(409,'takeover_context_stale');
 }
}

import { createHash, createPublicKey, verify } from 'node:crypto';

export const phases = new Set(['idle','checking','available','current','downloading','verifying','backing_up','applying','restarting','health_check','succeeded','rolled_back','failed','interrupted','recovery_required']);
export const activePhases = new Set(['checking','downloading','verifying','backing_up','applying','restarting','health_check']);
export const digest = bytes => createHash('sha256').update(bytes).digest('hex');
export const versionPattern = /^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$/;
export function compareVersion(a,b) {
  if (!versionPattern.test(a) || !versionPattern.test(b)) throw Error('update_version_invalid');
  const x=a.split('.').map(Number),y=b.split('.').map(Number);
  for(let i=0;i<3;i++) if(x[i]!==y[i])return Math.sign(x[i]-y[i]);
  return 0;
}
const exact=(value,keys)=>value&&typeof value==='object'&&!Array.isArray(value)&&Object.keys(value).length===keys.length&&keys.every(key=>Object.hasOwn(value,key));
export function feedUrl(text, { allowInsecureHttp = false } = {}) {
  let value;try{value=new URL(text);}catch{throw Error('update_feed_invalid');}
  if((value.protocol!=='https:'&&!(value.protocol==='http:'&&allowInsecureHttp===true))||value.username||value.password||value.hash||value.search||value.port==='0'||typeof text!=='string'||/[\x00-\x20\x7f\\]/.test(text))throw Error('update_feed_invalid');
  return value;
}
export function validateRelease(value) {
  if(!exact(value,['schema','product','version','platform','releasedAt','changelog','artifact'])||value.schema!==1||value.product!=='multitap-broker'||!versionPattern.test(value.version)||!['linux-x64-systemd','linux-x64-docker'].includes(value.platform))throw Error('update_manifest_invalid');
  if(typeof value.releasedAt!=='string'||!/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z$/.test(value.releasedAt)||!Number.isFinite(Date.parse(value.releasedAt))||new Date(value.releasedAt).toISOString()!==value.releasedAt)throw Error('update_manifest_invalid');
  if(typeof value.changelog!=='string'||!value.changelog.isWellFormed()||!value.changelog.trim()||Buffer.byteLength(value.changelog)>8000||/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f\ufffd]/u.test(value.changelog)||/\?{3,}/.test(value.changelog))throw Error('update_manifest_invalid');
  const a=value.artifact;
  const suffix=value.platform==='linux-x64-docker'?'linux-x64-docker':'linux-x64';
  if(!exact(a,['path','size','sha256'])||a.path!==`/downloads/Multitap-Server-${value.version}-${suffix}.tar.gz`||!Number.isSafeInteger(a.size)||a.size<=0||a.size>128*1024*1024||typeof a.sha256!=='string'||!/^[a-f0-9]{64}$/.test(a.sha256))throw Error('update_manifest_invalid');
  return value;
}
export function decodeFeed(bytes,publicKey) {
  if(bytes.length>32768)throw Error('update_manifest_invalid');
  let envelope;try{envelope=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(bytes));}catch{throw Error('update_manifest_invalid');}
  if(!exact(envelope,['release','signature'])||typeof envelope.signature!=='string'||!/^[A-Za-z0-9+/]{86}==$/.test(envelope.signature))throw Error('update_signature_invalid');
  const release=validateRelease(envelope.release),key=createPublicKey(publicKey);
  if(key.asymmetricKeyType!=='ed25519'||!verify(null,Buffer.from(JSON.stringify(release)),key,Buffer.from(envelope.signature,'base64')))throw Error('update_signature_invalid');
  return {release,releaseId:digest(Buffer.from(JSON.stringify(release)))};
}
export async function fetchBytes(url,limit,fetcher=fetch,progress=()=>{},{allowInsecureHttp=false}={}) {
  let target;try{target=new URL(url);}catch{throw Error('update_download_failed');}
  if((target.protocol!=='https:'&&!(target.protocol==='http:'&&allowInsecureHttp===true))||target.username||target.password||target.hash||target.search)throw Error('update_download_failed');
  const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),120000);timer.unref?.();
  try {
    const response=await fetcher(url,{redirect:'error',signal:controller.signal});
    if(!response.ok||!response.body)throw Error('update_download_failed');
    const length=response.headers.get('content-length');
    if(length!==null&&(!/^\d+$/.test(length)||Number(length)>limit))throw Error('update_download_size');
    const chunks=[];let total=0;
    for await(const value of response.body){const b=Buffer.from(value);total+=b.length;if(total>limit)throw Error('update_download_size');chunks.push(b);progress(total);}
    if(length!==null&&Number(length)!==total)throw Error('update_download_size');
    return Buffer.concat(chunks,total);
  } catch(error) {if(/^update_/.test(error.message))throw error;throw Error('update_download_failed');}
  finally {clearTimeout(timer);}
}
export function publicUpdate(value,currentVersion,configured=true) {
  const result={state:phases.has(value?.phase)?value.phase:'idle',feedConfigured:configured,applySupported:configured,currentVersion};
  if(value?.checkedAt)result.checkedAt=value.checkedAt;
  if(value?.operationId)result.operationId=value.operationId;
  if(value?.errorCode&&/^update_[a-z_]{1,70}$/.test(value.errorCode))result.errorCode=value.errorCode;
  if(Number.isSafeInteger(value?.downloadedBytes))result.downloadedBytes=value.downloadedBytes;
  if(Number.isSafeInteger(value?.totalBytes))result.totalBytes=value.totalBytes;
  if(value?.release){const r=validateRelease(value.release);result.latest={version:r.version,releasedAt:r.releasedAt,changelog:r.changelog,size:r.artifact.size};result.releaseId=value.releaseId;}
  return result;
}

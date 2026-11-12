import { gunzipSync, gzipSync } from 'node:zlib';
import { mkdir, writeFile } from 'node:fs/promises';
import { resolve, dirname } from 'node:path';
import { constants } from 'node:fs';

const maxExpanded=128*1024*1024;
const privateSegments=new Set(['.git','.hg','.svn','.bzr','.local','.ssh','secrets']);
const credentialNames=new Set(['.npmrc','.netrc','_netrc','.pypirc','.git-credentials']);
const environmentExamples=new Set(['.env.example','.env.sample','.env.template']);
export function isAllowedPackagePath(path) {
  return typeof path==='string'&&path.length<=99&&/^[A-Za-z0-9_@./+\-]+$/.test(path)&&!path.startsWith('/')&&!path.endsWith('/')&&!path.split('/').some(p=>!p||p==='.'||p==='..')&&
    (['package.json','package-lock.json'].includes(path)||['src/','admin/','setup/','update/','node_modules/'].some(root=>path.startsWith(root)))&&
    !path.toLowerCase().split('/').some((p,i,parts)=>privateSegments.has(p)||credentialNames.has(p)||(p==='.env'||p.startsWith('.env.'))&&!(i===parts.length-1&&environmentExamples.has(p)))&&
    !/\.(?:exe|dll|so|node|sqlite|dpapi|key|pem|pdb)$/i.test(path)&&
    !/(?:^|\/)(?:(?:settings|login|initial-account|host-token|credentials|server-access)\.json|server-access\.identity|host\.dat|broker\.sqlite[^/]*)$/i.test(path);
}
const octal=(buffer,start,length)=>{const text=buffer.subarray(start,start+length).toString('ascii').replace(/\0.*$/s,'').trim();if(!/^[0-7]+$/.test(text))throw Error('update_archive_invalid');return parseInt(text,8);};
export function unpackPackage(archive,version) {
  let tar;try{tar=gunzipSync(archive,{maxOutputLength:maxExpanded});}catch{throw Error('update_archive_invalid');}
  if(tar.length%512||tar.length<1536)throw Error('update_archive_invalid');
  const files=new Map();let offset=0,ended=false;
  while(offset<tar.length){
    const header=tar.subarray(offset,offset+512);offset+=512;
    if(header.every(b=>b===0)){if(tar.subarray(offset).some(b=>b!==0)||tar.length-offset<512)throw Error('update_archive_invalid');ended=true;break;}
    const name=header.subarray(0,100).toString('ascii').replace(/\0.*$/s,'');
    const zeroAfterNull=(field)=>{const z=field.indexOf(0);return z<0||field.subarray(z).every(b=>b===0);};
    if(!isAllowedPackagePath(name)||!zeroAfterNull(header.subarray(0,100))||files.has(name)||files.size>=4096||header.subarray(157,257).some(b=>b!==0)||header.subarray(345,500).some(b=>b!==0)||header.subarray(257,263).toString('ascii')!=='ustar\0'||header[156]!==48)throw Error('update_archive_invalid');
    const checksum=octal(header,148,8),actual=header.reduce((a,b,i)=>a+(i>=148&&i<156?32:b),0),size=octal(header,124,12),mode=octal(header,100,8);
    if(checksum!==actual||mode!==0o644||size>32*1024*1024||offset+size>tar.length)throw Error('update_archive_invalid');
    const end=offset+Math.ceil(size/512)*512;
    if(end>tar.length||tar.subarray(offset+size,end).some(b=>b!==0))throw Error('update_archive_invalid');
    files.set(name,tar.subarray(offset,offset+size));offset=end;
  }
  if(!ended)throw Error('update_archive_invalid');
  for(const name of ['package.json','package-lock.json','src/index.mjs','src/http.mjs','src/store.mjs','src/bootstrap.mjs','admin/index.html','admin/app.js','update/agent.mjs','update/provision-broker.sh','node_modules/ws/package.json'])if(!files.has(name))throw Error('update_archive_incomplete');
  let p,lock;try{p=JSON.parse(files.get('package.json'));lock=JSON.parse(files.get('package-lock.json'));}catch{throw Error('update_archive_invalid');}
  if(p.name!=='multitap-broker'||p.version!==version||lock.version!==version||lock.packages?.['']?.version!==version)throw Error('update_package_version');
  return files;
}
export function packPackage(files) {
  const parts=[];let size=1024;
  for(const [name,bytes] of [...files].sort(([a],[b])=>a.localeCompare(b,'en'))){
    if(!isAllowedPackagePath(name)||!Buffer.isBuffer(bytes)||bytes.length>32*1024*1024)throw Error('update_archive_invalid');
    const header=Buffer.alloc(512),put=(value,at,len)=>header.write(value,at,len,'ascii'),oct=(value,at,len)=>put(value.toString(8).padStart(len-1,'0')+'\0',at,len);
    put(name,0,100);oct(0o644,100,8);oct(0,108,8);oct(0,116,8);oct(bytes.length,124,12);oct(0,136,12);header.fill(32,148,156);header[156]=48;put('ustar\0',257,6);put('00',263,2);
    const sum=header.reduce((a,b)=>a+b,0);put(sum.toString(8).padStart(6,'0')+'\0 ',148,8);
    parts.push(header,bytes,Buffer.alloc((512-bytes.length%512)%512));size+=512+Math.ceil(bytes.length/512)*512;
    if(size>maxExpanded)throw Error('update_archive_invalid');
  }
  parts.push(Buffer.alloc(1024));return gzipSync(Buffer.concat(parts),{level:9});
}
// Destination is a newly created, root-owned private stage. Never extract links,
// special entries, parent paths or arbitrary tar metadata at the OS boundary.
export async function extractPackage(files,destination) {
  await mkdir(destination,{mode:0o700});
  for(const [name,bytes] of files){const path=resolve(destination,name);if(!path.startsWith(resolve(destination)+'/')&&!path.startsWith(resolve(destination)+'\\'))throw Error('update_archive_invalid');await mkdir(dirname(path),{recursive:true,mode:0o700});await writeFile(path,bytes,{mode:0o644,flag:constants.O_CREAT|constants.O_EXCL|constants.O_WRONLY|constants.O_NOFOLLOW});}
}

import {readFile,writeFile,readdir,lstat,mkdir} from 'node:fs/promises';
import {resolve,join,relative} from 'node:path';
import {createPrivateKey,sign} from 'node:crypto';
import {packPackage,unpackPackage} from '../server/update/package.mjs';
import {validateRelease,digest} from '../server/update/protocol.mjs';
import {validateDockerImage} from '../server/update/docker.mjs';

const [source,dependencies,notesPath,keyPath,output,dockerMetadataPath]=process.argv.slice(2);
if(!output)throw Error('Usage: build-server-update.mjs <server source> <production dependencies root> <UTF8 notes> <private signing key> <output>');
const root=resolve(source),files=new Map();
async function collect(directory,prefix){
  for(const e of await readdir(directory,{withFileTypes:true})){const path=join(directory,e.name),name=prefix+e.name,s=await lstat(path);if(s.isSymbolicLink())throw Error('Release contains link');if(s.isDirectory())await collect(path,name+'/');else if(s.isFile())files.set(name,await readFile(path));else throw Error('Release contains nonregular file');}
}
for(const name of ['package.json','package-lock.json'])files.set(name,await readFile(join(root,name)));
for(const name of ['src','admin','setup','update'])await collect(join(root,name),name+'/');
await collect(join(resolve(dependencies),'node_modules'),'node_modules/');
const version=JSON.parse(files.get('package.json')).version;
if(dockerMetadataPath){const metadata=JSON.parse(await readFile(dockerMetadataPath,'utf8'));if(typeof metadata.image!=='string'||!/^([a-z0-9.-]+(?::[0-9]{1,5})?)\/[a-z0-9]+(?:[._/-][a-z0-9]+)*@sha256:[a-f0-9]{64}$/.test(metadata.image))throw Error('Invalid immutable Docker image');validateDockerImage(metadata,version,metadata.image.split('@sha256:')[0]);files.set('update/docker-image.json',Buffer.from(JSON.stringify(metadata)));}
const archive=packPackage(files);unpackPackage(archive,version);
const release=validateRelease({schema:1,product:'multitap-broker',version,platform:dockerMetadataPath?'linux-x64-docker':'linux-x64-systemd',releasedAt:new Date().toISOString(),changelog:new TextDecoder('utf-8',{fatal:true}).decode(await readFile(notesPath)),artifact:{path:`/downloads/Multitap-Server-${version}-linux-x64${dockerMetadataPath?'-docker':''}.tar.gz`,size:archive.length,sha256:digest(archive)}});
const key=createPrivateKey(await readFile(keyPath));if(key.asymmetricKeyType!=='ed25519')throw Error('Wrong signing key');
const envelope={release,signature:sign(null,Buffer.from(JSON.stringify(release)),key).toString('base64')};
await mkdir(output,{recursive:true});await writeFile(join(output,release.artifact.path.split('/').at(-1)),archive,{flag:'wx'});
await writeFile(join(output,dockerMetadataPath?'server-updates-docker.json':'server-updates.json'),JSON.stringify(envelope),{flag:'wx'});
await writeFile(join(output,'receipt.json'),JSON.stringify({releaseId:digest(Buffer.from(JSON.stringify(release))),...release,files:files.size,source:relative(process.cwd(),root),installerExecuted:false},null,2),{flag:'wx'});
console.log(JSON.stringify({version,size:archive.length,sha256:release.artifact.sha256,files:files.size}));

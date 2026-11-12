import {generateKeyPairSync,createPublicKey} from 'node:crypto';
import {mkdir,readFile,writeFile} from 'node:fs/promises';
import {join} from 'node:path';
const directory=process.argv[2];if(!directory)throw Error('Private output directory required');
await mkdir(directory,{recursive:true,mode:0o700});
const privatePath=join(directory,'server-update-private.pem'),publicPath=join(directory,'server-update-public.pem');
let existing;
try{existing=await readFile(privatePath);}catch(error){if(error.code!=='ENOENT')throw error;}
if(existing){if(createPublicKey(existing).export({type:'spki',format:'pem'})!==await readFile(publicPath,'utf8'))throw Error('Existing signing key pair mismatch');console.log('Existing server update signing key preserved');}
else{const pair=generateKeyPairSync('ed25519');await writeFile(privatePath,pair.privateKey.export({type:'pkcs8',format:'pem'}),{flag:'wx',mode:0o600});await writeFile(publicPath,pair.publicKey.export({type:'spki',format:'pem'}),{flag:'wx',mode:0o644});console.log('Created server update signing key pair; private bytes not printed');}

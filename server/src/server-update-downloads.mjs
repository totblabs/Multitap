import { open,realpath,lstat } from 'node:fs/promises';
import { constants } from 'node:fs';
import { resolve } from 'node:path';
import { pipeline } from 'node:stream/promises';
import { validateRelease } from '../update/protocol.mjs';

export function serverUpdateDownloads(directory){
  return async(req,res,path)=>{
    if(!['/downloads/server-updates.json','/downloads/server-updates-docker.json'].includes(path)&&!/^\/downloads\/Multitap-Server-\d+\.\d+\.\d+-linux-x64(?:-docker)?\.tar\.gz$/.test(path))return false;
    const end=status=>{res.writeHead(status,{'Cache-Control':'no-store','X-Content-Type-Options':'nosniff'});res.end();return true;};
    if(!directory)return end(404);if(!['GET','HEAD'].includes(req.method))return end(405);
    let file;
    try{
      const docker=path.includes('-docker'),feedName=docker?'server-updates-docker.json':'server-updates.json';
      const root=await realpath(directory),feedPath=resolve(root,feedName),before=await lstat(feedPath);
      if(!before.isFile()||before.isSymbolicLink()||before.size>32768)return end(404);
      const feed=await open(feedPath,constants.O_RDONLY|(constants.O_NOFOLLOW??0));let bytes;
      try{const held=await feed.stat();if(held.ino!==before.ino||held.dev!==before.dev||held.size>32768)return end(404);bytes=await feed.readFile();}finally{await feed.close();}
      const envelope=JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(bytes)),release=validateRelease(envelope.release);
      if(typeof envelope.signature!=='string'||!/^[A-Za-z0-9+/]{86}==$/.test(envelope.signature))return end(404);
      if(path==='/downloads/'+feedName){res.writeHead(200,{'Content-Type':'application/json; charset=utf-8','Cache-Control':'no-cache','Content-Length':bytes.length,'X-Content-Type-Options':'nosniff'});res.end(req.method==='HEAD'?undefined:bytes);return true;}
      if(path!==release.artifact.path)return end(404);
      const target=resolve(root,path.slice('/downloads/'.length)),stat=await lstat(target);
      if(!stat.isFile()||stat.isSymbolicLink()||stat.size!==release.artifact.size)return end(404);
      file=await open(target,constants.O_RDONLY|(constants.O_NOFOLLOW??0));const held=await file.stat();if(held.ino!==stat.ino||held.dev!==stat.dev||held.size!==stat.size){await file.close();return end(404);}
      res.writeHead(200,{'Content-Type':'application/gzip','Content-Length':held.size,'Cache-Control':'no-cache','X-Content-Type-Options':'nosniff',ETag:'"sha256-'+release.artifact.sha256+'"'});
      if(req.method==='HEAD'){await file.close();res.end();}else await pipeline(file.createReadStream(),res);return true;
    }catch{await file?.close();if(res.headersSent)res.destroy();else end(404);return true;}
  };
}

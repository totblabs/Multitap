import http from 'node:http';
import { readFile, lstat, mkdir, chmod, unlink, realpath } from 'node:fs/promises';
import { dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { UpdateEngine } from './engine.mjs';
import { SystemdAdapter, migrateUpdaterUnitAtStartup } from './systemd.mjs';
import { DockerAdapter } from './docker.mjs';
import { ServerAccessEngine } from './server-access-engine.mjs';
import { ServerAccessDeployment } from './server-access-deployment.mjs';
import { ServerRestoreEngine } from './restore-engine.mjs';
import { ServerRestoreDeployment } from './restore-deployment.mjs';

export function updateDeployment(config){
  if(Object.hasOwn(config??{},'allowInsecureHttp')&&typeof config.allowInsecureHttp!=='boolean')throw Error('update_config_invalid');
  const fields=Object.keys(config??{}).filter(key=>key!=='allowInsecureHttp').sort().join(',');
  if(fields==='feed,publicKey')return {adapter:new SystemdAdapter(),directory:'/opt/multitap-stage/updates',runtime:'/run/multitap-update'};
  if(fields==='deployment,feed,publicKey'&&config.deployment?.kind==='docker'&&Object.keys(config.deployment).sort().join(',')==='config,kind'){
    const adapter=new DockerAdapter(config.deployment.config);
    if(adapter.config.updatesDirectory!=='/opt/multitap-stage/docker-updates')throw Error('update_config_invalid');
    return {adapter,directory:adapter.config.updatesDirectory,runtime:'/run/multitap-docker-update'};
  }
  throw Error('update_config_invalid');
}

export async function listenUpdateAgent(engine,socket,{accessEngine=null,restoreEngine=null}={}){
  await mkdir(dirname(socket),{recursive:true,mode:0o750});
  try{const s=await lstat(socket);if(!s.isSocket())throw Error('update_socket_invalid');await unlink(socket);}catch(e){if(e.code!=='ENOENT')throw e;}
  const server=http.createServer(async(req,res)=>{
    const send=(status,value)=>{res.writeHead(status,{'Content-Type':'application/json','Cache-Control':'no-store'});res.end(JSON.stringify(value));};
    try{
      // 백업·복원 (restore-engine.mjs): the broker names an operation and the
      // hashes of what it staged; it sends no path, file name or content.
      if(req.url?.startsWith('/restore/')){
        if(!restoreEngine)return send(503,{error:'restore_unavailable'});
        if(req.url==='/restore/status'&&req.method==='GET')return send(200,restoreEngine.status());
        if(req.url!=='/restore/apply'||req.method!=='POST')return send(404,{error:'restore_command_invalid'});
        let count=0;const parts=[];for await(const chunk of req){count+=chunk.length;if(count>4096)throw Error('restore_command_invalid');parts.push(chunk);}
        let body;try{body=JSON.parse(Buffer.concat(parts).toString('utf8'));}catch{throw Error('restore_command_invalid');}
        if(!body||Array.isArray(body)||typeof body!=='object'||Object.keys(body).sort().join(',')!=='context,input')throw Error('restore_command_invalid');
        if(engine.busy||accessEngine?.busy)throw Error('restore_busy');
        if(engine.state.phase==='recovery_required'||accessEngine?.state.state==='recovery_required')throw Error('restore_other_recovery_required');
        return send(202,await restoreEngine.apply(body.input,body.context));
      }
      if(req.url?.startsWith('/access/')){
        if(!accessEngine)return send(503,{error:'server_access_unavailable'});
        if(req.url==='/access/status'&&req.method==='GET')return send(200,await accessEngine.status());
        if(!['/access/test','/access/save','/access/cancel'].includes(req.url)||req.method!=='POST')return send(404,{error:'server_access_command_invalid'});
        let count=0;const parts=[];for await(const chunk of req){count+=chunk.length;if(count>16384)throw Error('server_access_command_invalid');parts.push(chunk);}
        let body;try{body=JSON.parse(Buffer.concat(parts).toString('utf8'));}catch{throw Error('server_access_command_invalid');}
        // A cancel names only its operation; it carries no trusted context.
        if(req.url==='/access/cancel'){
          if(!body||Array.isArray(body)||typeof body!=='object'||Object.keys(body).join(',')!=='input')throw Error('server_access_command_invalid');
          return send(200,await accessEngine.cancel(body.input));
        }
        if(!body||Array.isArray(body)||typeof body!=='object'||Object.keys(body).sort().join(',')!=='context,input')throw Error('server_access_command_invalid');
        const result=req.url==='/access/test'?await accessEngine.test(body.input,body.context):await accessEngine.save(body.input,body.context);
        return send(req.url==='/access/save'?202:200,result);
      }
      if(req.url==='/status'&&req.method==='GET'){send(200,await engine.status());return;}
      if(!['/check','/apply'].includes(req.url)||req.method!=='POST')return send(404,{error:'update_command_invalid'});
      let count=0,parts=[];for await(const chunk of req){count+=chunk.length;if(count>1024)throw Error('update_command_invalid');parts.push(chunk);}
      let input;try{input=JSON.parse(Buffer.concat(parts).toString('utf8'));}catch{throw Error('update_command_invalid');}
      if(!input||Array.isArray(input)||typeof input!=='object')throw Error('update_command_invalid');
      if(accessEngine?.busy||restoreEngine?.busy)throw Error('update_busy');
      if(accessEngine?.state.state==='recovery_required'||restoreEngine?.state.state==='recovery_required')throw Error('update_recovery_required');
      if(req.url==='/check'){if(Object.keys(input).length)throw Error('update_command_invalid');send(202,engine.check());}
      else{if(Object.keys(input).sort().join(',')!=='fromVersion,releaseId'||!/^[a-f0-9]{64}$/.test(input.releaseId??''))throw Error('update_command_invalid');send(202,engine.apply(input));}
    }catch(error){
      if(/^restore_[a-z_]{1,80}$/.test(error.message)){
        const conflict=/_(busy|recovery_required|operation_reused)$/.test(error.message);
        const invalid=!conflict&&/_(invalid|required)$/.test(error.message)&&!/_storage_invalid$/.test(error.message);
        send(conflict?409:invalid?400:503,{error:error.message});
      }else if(/^server_access_[a-z_]{1,80}$/.test(error.message)){
        const conflict=/_(busy|revision_changed|test_required|fence_lost|management_origin_changed|operation_reused|prior_marker_pending|cancel_unavailable|cancel_too_late)$/.test(error.message);
        const invalid=/_(invalid|required|conflict)$/.test(error.message)&&!/_storage_invalid$/.test(error.message);
        send(conflict?409:invalid?400:503,{error:error.message});
      }else send(['update_busy','update_check_required','update_version_changed','update_recovery_required'].includes(error.message)?409:503,{error:/^update_[a-z_]+$/.test(error.message)?error.message:'update_failed'});
    }
  });
  server.requestTimeout=5000;server.headersTimeout=5000;server.maxConnections=8;
  await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(socket,resolve);});await chmod(socket,0o660);return server;
}
async function main(){
  if(process.platform!=='linux'||process.getuid()!==0)throw Error('update_root_required');process.umask(0o077);
  const path=process.env.MULTITAP_UPDATE_CONFIG??'/etc/multitap/update.json',s=await lstat(path);
  if(!s.isFile()||s.isSymbolicLink()||s.uid!==0||(s.mode&0o077))throw Error('update_config_invalid');
  const config=JSON.parse(await readFile(path,'utf8'));
  if(typeof config.publicKey!=='string')throw Error('update_config_invalid');
  const deployment=updateDeployment(config);
  // systemd removes this RuntimeDirectory on a complete service stop/restart.
  // A second manually launched agent must not unlink a live control socket or
  // reconcile the same journal concurrently with the managed instance.
  await mkdir(deployment.runtime+'/agent.lock',{mode:0o700});
  const engine=new UpdateEngine({directory:deployment.directory,feed:config.feed,publicKey:config.publicKey,adapter:deployment.adapter,allowInsecureHttp:config.allowInsecureHttp===true});await engine.initialize();
  if(deployment.adapter.platform==='linux-x64-systemd'&&engine.state.phase!=='recovery_required'){
    try{
      if(await migrateUpdaterUnitAtStartup(deployment.directory)){
        console.log(JSON.stringify({service:'multitap-updater',state:'sandbox_refresh_pending'}));
        // No socket or work has started. Restart=always performs the one normal
        // startup refresh; an in-progress apply is never restarted here.
        return;
      }
    }catch(error){
      const errorCode=['update_updater_unit_migration_failed','update_updater_unit_recovery_required','update_updater_unit_changed','update_updater_unit_invalid'].includes(error.message)?error.message:'update_updater_unit_migration_failed';
      console.error(JSON.stringify({service:'multitap-updater',state:'unavailable',errorCode}));throw error;
    }
  }
  let accessEngine=null,accessAdapter=null,restoreEngine=null;
  try{
    accessAdapter=new ServerAccessDeployment({adapter:deployment.adapter,directory:deployment.directory+'/server-access-operations',updaterConfigPath:path});
    accessEngine=new ServerAccessEngine({adapter:accessAdapter,directory:deployment.directory+'/server-access-state',otherBusy:()=>engine.busy||restoreEngine?.busy===true});await accessEngine.initialize();
  }catch{accessEngine=null;console.error(JSON.stringify({feature:'server-access-v1',state:'unavailable',errorCode:'server_access_unavailable'}));}
  // The restore engine probes and fences the broker through the access
  // deployment, so it exists only where that one does.
  if(accessEngine){
    try{
      const restoreAdapter=new ServerRestoreDeployment({access:accessAdapter,directory:deployment.directory+'/restore-operations'});
      restoreEngine=new ServerRestoreEngine({adapter:restoreAdapter,directory:deployment.directory+'/restore-state',otherBusy:()=>engine.busy||accessEngine.busy});await restoreEngine.initialize();
    }catch{restoreEngine=null;console.error(JSON.stringify({feature:'server-restore-v1',state:'unavailable',errorCode:'restore_unavailable'}));}
  }
  await listenUpdateAgent(engine,deployment.runtime+'/control.sock',{accessEngine,restoreEngine});
  // Refresh the updater executable from the new immutable current release only
  // after its durable success. The systemd unit restarts this process; it never
  // exits while a child still performs backup/migration/restoration.
  const bootOperation=engine.state.operationId;
  const bootAccessOperation=accessEngine?.state.operationId;
  const refresh=setInterval(()=>{if(!engine.busy&&!accessEngine?.busy&&!restoreEngine?.busy&&((['succeeded','rolled_back'].includes(engine.state.phase)&&engine.state.operationId!==bootOperation)||(accessEngine&&['succeeded','rolled_back'].includes(accessEngine.state.state)&&accessEngine.state.operationId!==bootAccessOperation)))process.exit(0);},5000);refresh.unref();
  console.log(JSON.stringify({service:'multitap-updater',state:'ready'}));
}
const entry=process.argv[1]?await realpath(process.argv[1]).catch(()=>null):null;
if(entry===await realpath(fileURLToPath(import.meta.url)))main().catch(()=>{console.error('Multitap updater startup failed');process.exitCode=1;});

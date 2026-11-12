#!/bin/bash
# One-time Docker host integration. Normal subsequent updates use Admin buttons.
# This file has not been executed against a Docker daemon in the current QA.
set -euo pipefail
umask 077
source_dir=${1:?verified Multitap server source directory required}
config_file=${2:?root-protected Docker update configuration required}
root=/opt/multitap-docker-updater
runtime_version=24.22.0
[[ $(id -u) == 0 && $(uname -m) == x86_64 ]] || exit 1
[[ $source_dir == /opt/multitap-stage/*/verified && $config_file == /opt/multitap-stage/*/docker-update.json ]] || exit 1
[[ -S /var/run/docker.sock && -x /usr/bin/docker ]] || { echo 'Local Docker Engine is required.' >&2; exit 1; }
for command in systemctl curl tar sha256sum awk cmp install; do command -v "$command" >/dev/null; done
if [[ -e $root && ( ! -f $root/.multitap-managed-docker-updater || -L $root || $(stat -c %u "$root") != 0 ) ]]; then echo 'Unrecognized updater directory; preserved.' >&2; exit 1; fi
for protected in /opt "$root"; do
  if [[ -e $protected ]]; then
    [[ -d $protected && ! -L $protected && $(stat -c %u "$protected") == 0 ]] || exit 1
    mode=$(stat -c %a "$protected")
    (( (8#$mode & 8#022) == 0 )) || exit 1
  fi
done
if [[ -e /etc/systemd/system/multitap-docker-updater.service ]] && ! grep -q '# Multitap managed Docker updater' /etc/systemd/system/multitap-docker-updater.service; then exit 1; fi
if systemctl is-active --quiet multitap-docker-updater; then echo 'The managed updater is already active. Use Admin Server Update; this installer did not replace it.' >&2; exit 1; fi
install -d -o root -g root -m 755 "$root" "$root/runtime" "$root/releases"
touch "$root/.multitap-managed-docker-updater"
chmod 600 "$root/.multitap-managed-docker-updater"
runtime="$root/runtime/node-v${runtime_version}-linux-x64"
if [[ ! -x $runtime/bin/node ]]; then
  cache="$root/runtime/downloads"
  install -d -o root -g root -m 700 "$cache"
  name="node-v${runtime_version}-linux-x64.tar.xz"
  curl --fail --location --silent --show-error --retry 3 "https://nodejs.org/dist/v${runtime_version}/SHASUMS256.txt" -o "$cache/SHASUMS256.txt"
  curl --fail --location --silent --show-error --retry 3 "https://nodejs.org/dist/v${runtime_version}/$name" -o "$cache/$name"
  (cd "$cache"; grep "  $name\$" SHASUMS256.txt | sha256sum -c -)
  tar -xJf "$cache/$name" -C "$root/runtime"
  chmod -R a+rX "$runtime"
fi
node="$runtime/bin/node"
[[ $($node --version) == v${runtime_version} ]] || exit 1

# Validate fixed root config and the already-running owned project before this
# installer can stop/recreate anything. Source/config ancestry is checked too.
install -d -o root -g root -m 750 /etc/multitap
bootstrap="$root/bootstrap-result.json"
"$node" --input-type=module - "$source_dir" "$config_file" "$bootstrap" <<'NODE'
import {readFile,lstat,readdir,mkdir,writeFile} from 'node:fs/promises';
import {dirname} from 'node:path';
import {createPublicKey} from 'node:crypto';
import {pathToFileURL} from 'node:url';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
const [source,path,result]=process.argv.slice(2);
async function trustedDockerPath(p,kind='file'){let first=true;while(true){const s=await lstat(p);if(s.isSymbolicLink()||s.uid!==0||(s.mode&0o022)||(first?(kind==='file'?!s.isFile():!s.isDirectory()):!s.isDirectory()))throw Error('Untrusted updater input');if(p==='/')return;p=dirname(p);first=false;}}
await trustedDockerPath(source,'directory');await trustedDockerPath(source+'/package.json');await trustedDockerPath(path);
for(const name of await readdir(source+'/update'))if(name.endsWith('.mjs')||name.endsWith('.sh'))await trustedDockerPath(source+'/update/'+name);
const {DockerAdapter}=await import(pathToFileURL(source+'/update/docker.mjs'));
const stat=await lstat(path);if(stat.mode&0o077)throw Error('Private config required');
const config=JSON.parse(await readFile(path,'utf8'));
if(Object.keys(config).filter(k=>k!=='allowInsecureHttp').sort().join(',')!=='deployment,feed,publicKey'||(Object.hasOwn(config,'allowInsecureHttp')&&typeof config.allowInsecureHttp!=='boolean')||config.deployment?.kind!=='docker'||Object.keys(config.deployment).sort().join(',')!=='config,kind'||createPublicKey(config.publicKey).asymmetricKeyType!=='ed25519')throw Error('Docker updater config invalid');
const feed=new URL(config.feed);if((feed.protocol!=='https:'&&!(feed.protocol==='http:'&&config.allowInsecureHttp===true))||feed.username||feed.password||feed.search||feed.hash)throw Error('Explicit transport update feed required');
const adapter=new DockerAdapter(config.deployment.config),c=adapter.config,p=adapter.paths();
if(c.updatesDirectory!=='/opt/multitap-stage/docker-updates')throw Error('Unexpected update directory');
await trustedDockerPath(c.projectDirectory,'directory');await trustedDockerPath(p.compose);await trustedDockerPath(p.env);await mkdir(c.updatesDirectory,{recursive:true,mode:0o700});await trustedDockerPath(c.updatesDirectory,'directory');
const pkg=JSON.parse(await readFile(source+'/package.json','utf8'));if(pkg.name!=='multitap-broker'||!/^\d+\.\d+\.\d+$/.test(pkg.version))throw Error('Product source invalid');
const exec=promisify(execFile),run=async args=>(await exec('/usr/bin/docker',['--host=unix:///var/run/docker.sock',...args],{env:{PATH:'/usr/sbin:/usr/bin:/sbin:/bin',LANG:'C.UTF-8',DOCKER_CONFIG:'/etc/multitap/docker-auth'},maxBuffer:1048576})).stdout;
const base=['compose','--project-name',c.projectName,'--project-directory',c.projectDirectory,'--env-file',p.env,'-f',p.compose];
const ids=(await run([...base,'ps','--all','--quiet','broker'])).trim().split(/\s+/).filter(Boolean);if(ids.length!==1||!/^[a-f0-9]{64}$/.test(ids[0]))throw Error('Expected one existing broker');
const containers=JSON.parse(await run(['container','inspect',ids[0]]));if(containers.length!==1||containers[0].Id!==ids[0])throw Error('Container identity changed');
const current=adapter.validateContainer(containers[0],{running:true}),channel=JSON.stringify(adapter.channel()),override=JSON.stringify(adapter.override(current.Image));
let channelExisted=false;
for(const [file,bytes] of [[p.channel,channel],[p.override,override]]){
  try{await trustedDockerPath(file);const existing=await readFile(file,'utf8');if(file===p.channel){channelExisted=true;if(existing!==bytes)throw Error('Existing channel differs');}}
  catch(e){if(e.code!=='ENOENT')throw e;await writeFile(file,bytes,{flag:'wx',mode:0o600});}
}
const checked=await adapter.installationCheck();if(checked.version!==pkg.version)throw Error('Docker image must match this first updater package version');
const volume=await adapter.volume();if(!/^\/[A-Za-z0-9_./-]+$/.test(volume.Mountpoint)||volume.Mountpoint.split('/').includes('..'))throw Error('Managed volume path unsupported');
const out={version:pkg.version,projectDirectory:c.projectDirectory,updatesDirectory:c.updatesDirectory,gid:c.brokerGid,channelExisted,previousImage:current.Image,volumeMountpoint:volume.Mountpoint};await writeFile(result,JSON.stringify(out),{mode:0o600});
NODE

version=$("$node" -p 'JSON.parse(require("fs").readFileSync(process.argv[1],"utf8")).version' "$bootstrap")
gid=$("$node" -p 'JSON.parse(require("fs").readFileSync(process.argv[1],"utf8")).gid' "$bootstrap")
project=$("$node" -p 'JSON.parse(require("fs").readFileSync(process.argv[1],"utf8")).projectDirectory' "$bootstrap")
access_volume=$("$node" -p 'JSON.parse(require("fs").readFileSync(process.argv[1],"utf8")).volumeMountpoint' "$bootstrap")
updates=/opt/multitap-stage/docker-updates
release="$root/releases/$version"
install -d -o root -g root -m 755 "$release" "$release/update"
for file in "$source_dir"/update/*.mjs "$source_dir"/update/*.sh; do
  [[ -f $file ]] || continue
  target="$release/update/$(basename "$file")"
  if [[ -e $target ]]; then cmp -s "$file" "$target" || { echo 'Immutable updater release differs.' >&2; exit 1; }; else install -o root -g root -m 644 "$file" "$target"; fi
done
if [[ -e $root/current && ! -L $root/current ]]; then exit 1; fi
ln -sfn "$release" "$root/current"
touch "$root/.multitap-managed-docker-updater"
chmod 600 "$root/.multitap-managed-docker-updater"
if [[ -e /etc/multitap/docker-update.json ]]; then cmp -s "$config_file" /etc/multitap/docker-update.json || { echo 'Existing updater trust configuration differs; preserved.' >&2; exit 1; }; else install -o root -g root -m 600 "$config_file" /etc/multitap/docker-update.json; fi
install -d -o root -g root -m 700 /etc/multitap/docker-auth
cat > /etc/systemd/system/multitap-docker-updater.service <<SERVICE
# Multitap managed Docker updater
[Unit]
Description=Multitap verified Docker broker updater
After=network-online.target docker.service
Wants=network-online.target
Requires=docker.service

[Service]
Type=simple
User=root
Group=$gid
WorkingDirectory=$root/current
Environment=MULTITAP_UPDATE_CONFIG=/etc/multitap/docker-update.json
ExecStart=$node --preserve-symlinks-main $root/current/update/agent.mjs
Restart=always
RestartSec=3
TimeoutStopSec=180
RuntimeDirectory=multitap-docker-update
RuntimeDirectoryMode=0750
UMask=0027
NoNewPrivileges=true
PrivateTmp=true
PrivateDevices=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$root $updates $project $access_volume /etc/multitap /run/multitap-docker-update
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6

[Install]
WantedBy=multi-user.target
SERVICE
systemctl daemon-reload
systemctl enable --now multitap-docker-updater
systemctl is-active --quiet multitap-docker-updater

# A transient active state is not proof that the Docker adapter/config/socket
# started. Do not recreate the broker until the actual UDS protocol responds.
"$node" --input-type=module - <<'NODE'
import http from 'node:http';
const status=()=>new Promise((resolve)=>{const r=http.get({socketPath:'/run/multitap-docker-update/control.sock',path:'/status',timeout:1000},response=>{let body='';response.on('data',b=>{body+=b;if(body.length>32768)r.destroy();});response.on('end',()=>{try{const v=JSON.parse(body);resolve(response.statusCode===200&&v.feedConfigured===true&&v.applySupported===true);}catch{resolve(false);}});});r.on('timeout',()=>r.destroy());r.on('error',()=>resolve(false));});
let ready=false;for(let i=0;i<25;i++){if(await status()){ready=true;break;}await new Promise(r=>setTimeout(r,200));}if(!ready)throw Error('Docker updater UDS did not become ready; broker unchanged');
NODE

# The existing broker gets only the UDS directory read-only and a supplementary
# socket group. It never receives /var/run/docker.sock or host administrator APIs.
"$node" --input-type=module - "$root/current" /etc/multitap/docker-update.json "$bootstrap" <<'NODE'
import {readFile} from 'node:fs/promises';import {pathToFileURL} from 'node:url';
const [root,path,receipt]=process.argv.slice(2),{DockerAdapter}=await import(pathToFileURL(root+'/update/docker.mjs')),c=JSON.parse(await readFile(path,'utf8')),adapter=new DockerAdapter(c.deployment.config),prior=JSON.parse(await readFile(receipt,'utf8'));
const flags=['up','--detach','--no-deps','--no-build','--pull','never','--force-recreate','broker'];
async function healthy(){for(let i=0;i<25;i++){try{const checked=await adapter.installationCheck();if(checked.version===prior.version)return checked;}catch{}await new Promise(r=>setTimeout(r,500));}return null;}
try{await adapter.run(adapter.compose(flags));const checked=await healthy();if(!checked)throw Error('health');console.log(JSON.stringify({service:'multitap-docker-updater',state:'installed',brokerVersion:checked.version,privilegedDockerSocketExposedToBroker:false}));}
catch(error){const p=adapter.paths(),config=adapter.config,original=['compose','--project-name',config.projectName,'--project-directory',config.projectDirectory,'--env-file',p.env,'-f',p.compose,...(prior.channelExisted?['-f',p.channel]:[]),'-f',p.override,...flags];await adapter.run(original);if(!await healthy())throw Error('Docker broker channel installation recovery incomplete');throw Error('Docker update channel installation failed; previous broker configuration restored');}
NODE

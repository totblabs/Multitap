#!/bin/bash
set -euo pipefail
umask 077
config=${1:?root-protected update configuration required}
[[ $(id -u) == 0 && $config == /opt/multitap-stage/*/update.json ]] || exit 1
[[ -f /opt/multitap/.multitap-managed && -L /opt/multitap/current ]] || exit 1
node=/opt/multitap/runtime/node-v24.22.0-linux-x64/bin/node
"$node" --input-type=module - "$config" <<'NODE'
import {readFile,lstat} from 'node:fs/promises';
import {createPublicKey} from 'node:crypto';
const path=process.argv[2],s=await lstat(path);if(!s.isFile()||s.isSymbolicLink()||s.uid!==0||(s.mode&0o077))throw Error('Invalid updater config protection');
const c=JSON.parse(await readFile(path,'utf8')),u=new URL(c.feed);
if(Object.keys(c).filter(k=>k!=='allowInsecureHttp').sort().join(',')!=='feed,publicKey'||(Object.hasOwn(c,'allowInsecureHttp')&&typeof c.allowInsecureHttp!=='boolean')||(u.protocol!=='https:'&&!(u.protocol==='http:'&&c.allowInsecureHttp===true))||u.username||u.password||u.hash||u.search||createPublicKey(c.publicKey).asymmetricKeyType!=='ed25519')throw Error('Invalid updater configuration');
NODE
install -d -m 750 /etc/multitap
if [[ -e /etc/multitap/update.json ]]; then
  cmp -s "$config" /etc/multitap/update.json || { echo 'Existing updater trust configuration differs; preserved.' >&2; exit 1; }
else
  install -o root -g root -m 600 "$config" /etc/multitap/update.json
fi
if [[ -e /etc/systemd/system/multitap-updater.service ]] && ! grep -q '# Multitap managed updater' /etc/systemd/system/multitap-updater.service; then exit 1; fi
cat > /etc/systemd/system/multitap-updater.service <<'SERVICE'
# Multitap managed updater
[Unit]
Description=Multitap verified server updater
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
Group=multitap
AmbientCapabilities=CAP_SETUID CAP_SETGID
WorkingDirectory=/opt/multitap/current
ExecStart=/opt/multitap/runtime/node-v24.22.0-linux-x64/bin/node --preserve-symlinks-main /opt/multitap/current/update/agent.mjs
Restart=always
RestartSec=3
TimeoutStopSec=180
RuntimeDirectory=multitap-update
RuntimeDirectoryMode=0750
UMask=0027
NoNewPrivileges=true
PrivateTmp=true
PrivateDevices=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/opt/multitap /opt/multitap-stage /var/lib/multitap /etc/multitap /etc/systemd/system /run/multitap-update
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6

[Install]
WantedBy=multi-user.target
SERVICE
systemctl daemon-reload
systemctl enable --now multitap-updater
systemctl is-active --quiet multitap-updater

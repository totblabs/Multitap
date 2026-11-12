import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { pathToFileURL } from 'node:url';

// Baseline comparison only adds an export alias to the original private
// predicate. No predicate expression is copied into this regression.
const sourceUrl=process.env.MULTITAP_PACKAGE_PATH_SOURCE
  ?pathToFileURL(process.env.MULTITAP_PACKAGE_PATH_SOURCE)
  :new URL('../update/package.mjs',import.meta.url);
const source=await readFile(sourceUrl,'utf8');
const baseline=!source.includes('export function isAllowedPackagePath(');
const module=baseline
  ?await import('data:text/javascript;base64,'+Buffer.from(source+'\nexport { allowed as isAllowedPackagePath };\n').toString('base64'))
  :await import(sourceUrl.href);
const {isAllowedPackagePath}=module;

const blocked=[
  'src/.git/config','node_modules/example/.GIT/HEAD','admin/.hg/store/data',
  'setup/.svn/wc.db','update/.bzr/branch/format','src/.local/state.json',
  'admin/.ssh/config','node_modules/example/secrets/production.json',
  'src/.npmrc','node_modules/example/.netrc','update/_netrc',
  'setup/.pypirc','admin/.git-credentials','src/.env',
  'admin/.env.production','node_modules/example/.ENV.local',
  'src/.env.example.local','src/.env.example/state.json','update/server-access.json',
  'src/server-access.identity','node_modules/example/HOST-TOKEN.JSON',
  'setup/credentials.json','admin/settings.json','update/login.json',
  'src/initial-account.json','src/host.dat','src/broker.sqlite-wal',
  'src/broker.sqlite-shm','src/vault.key','src/private.pem',
  '../src/app.mjs','src/../app.mjs','src//app.mjs','/src/app.mjs',
  'src/app.mjs/','src/app.mjs\0','src\\app.mjs','unknown/app.mjs',
  'src/'+ 'a'.repeat(96),null,3,
];
const permitted=[
  'package.json','package-lock.json','src/index.mjs','admin/index.html',
  'setup/proxmox.py','update/agent.mjs','node_modules/ws/package.json',
  'node_modules/.bin/tool','admin/.well-known/security.txt',
  'src/.gitignore','src/.gitattributes','node_modules/example/LICENSE',
  'src/.env.example','admin/.env.sample','setup/.env.template','src/.ENV.EXAMPLE',
  'src/settings.example.json','update/server-access.example.json',
  'node_modules/example/environment.js','node_modules/example/secrets.js',
];
for(const [i,path] of blocked.entries())test(`private or invalid package path ${i+1}`,()=>{
  assert.equal(isAllowedPackagePath(path),false,`unexpected admission: ${String(path)}`);
});
for(const [i,path] of permitted.entries())test(`required package path ${i+1}`,()=>{
  assert.equal(isAllowedPackagePath(path),true,`unexpected rejection: ${path}`);
});
test('packing and unpacking call the same actual path predicate',()=>{
  const functionName=baseline?'allowed':'isAllowedPackagePath';
  for(const [start,end] of [['export function unpackPackage(','export function packPackage('],['export function packPackage(','export async function extractPackage(']]){
    const body=source.slice(source.indexOf(start),source.indexOf(end));
    assert.ok(body.includes(`if(!${functionName}(name)||`));
  }
});

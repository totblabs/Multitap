// Linux-only isolated QA. Extracts the production guard block, replacing all
// data paths and service/account commands with fixtures before executing it.
import assert from 'node:assert/strict';
import { DatabaseSync } from 'node:sqlite';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { mkdtemp, mkdir, writeFile, readFile, readlink, symlink, stat } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';

assert.equal(process.platform,'linux','Run the isolated rollback fixture on Linux with Bash.');
const sourcePath=resolve(process.argv[2]??join(import.meta.dirname,'provision-broker.sh'));
const source=await readFile(sourcePath,'utf8');
const syntax=spawnSync('bash',['-n',sourcePath],{encoding:'utf8'});
assert.equal(syntax.status,0,syntax.stderr);
const marker='# Guard rollback before stopping the broker';
const start=source.indexOf(marker),end=source.indexOf('ln -sfn "$release_dir" "$root/current"',start);
assert.ok(start>=0&&end>start,'Cannot locate the actual deployment rollback guard.');
const guard=source.slice(start,end);
assert.ok(guard.indexOf('trap rollback ERR')<guard.indexOf('  systemctl stop multitap-broker\n  install -d'),'Rollback trap must precede the first deployment stop.');
assert.ok(guard.indexOf("backup_ready=true")>guard.indexOf('mv -fT -- "$backup_temp" "$backup_file"'),'Only the validated atomically published backup may become ready.');
const quote=value=>"'"+value.replaceAll("'","'\\''")+"'";
const digest=value=>createHash('sha256').update(value).digest('hex');
const qaRoot=await mkdtemp(join(dirname(sourcePath),'rollback-fixture-'));
const results=[];
for(const scenario of ['backup-failure','invalid-backup','migration-failure','restart-unhealthy']) {
  const base=join(qaRoot,scenario),root=join(base,'product'),state=join(base,'data'),mock=join(base,'mock'),runtime=join(base,'runtime');
  for(const path of [join(root,'releases','previous'),join(root,'releases','candidate'),state,mock,join(runtime,'bin')])await mkdir(path,{recursive:true,mode:0o700});
  await symlink(join(root,'releases','previous'),join(root,'current'));
  const databasePath=join(state,'broker.sqlite'),accountPath=join(state,'initial-account.json');
  const db=new DatabaseSync(databasePath);
  db.exec("CREATE TABLE retained(id INTEGER PRIMARY KEY,value TEXT NOT NULL);INSERT INTO retained VALUES(1,'isolated-account-fixture');PRAGMA user_version=7;");db.close();
  await writeFile(accountPath,'{"fixture":"original-private-account"}\n',{mode:0o600});
  const originalHash=digest(await readFile(databasePath)),originalAccount=await readFile(accountPath,'utf8');
  const log=join(base,'mock-service.log');
  const scripts={
    systemctl:`#!/bin/bash\nset -eu\nif [[ $# == 3 && $1 == is-active && $2 == --quiet && $3 == multitap-broker ]]; then exit 0; fi\n[[ $# == 2 && $2 == multitap-broker && ( $1 == stop || $1 == restart ) ]] || exit 97\nprintf '%s\\n' "$1" >> "$QA_SERVICE_LOG"\n`,
    curl:`#!/bin/bash\nset -eu\n[[ "$*" == '--fail --silent --max-time 2 http://127.0.0.1:18480/healthz' ]] || exit 97\nif [[ $QA_SCENARIO == restart-unhealthy ]]; then exit 7; fi\nprintf '%s' '{"service":"multitap-broker","status":"ok"}'\n`,
    runuser:`#!/bin/bash\nset -eu\n[[ $1 == -u && $2 == multitap && $3 == -- ]] || exit 97\nshift 3\nexec "$@"\n`,
    chown:`#!/bin/bash\nset -eu\n[[ $1 == multitap:multitap && $2 == "$QA_CASE_ROOT/"* ]] || exit 97\n`,
    install:`#!/bin/bash\nset -eu\nfinal_path=''\nfor argument in "$@"; do final_path="$argument"; done\n[[ $1 == -d && $final_path == "$QA_CASE_ROOT/"* ]] || exit 97\nexec /usr/bin/install -d -m 700 -- "$final_path"\n`,
  };
  for(const [name,text]of Object.entries(scripts))await writeFile(join(mock,name),text,{mode:0o700});
  // execvp may skip a noexec PATH entry and execute the real /usr/bin command.
  // Require executable mocks before the guard and bind every intercepted
  // command by absolute path. A QA filesystem failure must never reach systemd.
  const mockEnv={...process.env,QA_CASE_ROOT:base,QA_SERVICE_LOG:log,QA_SCENARIO:scenario};
  const executableMock=spawnSync(join(mock,'systemctl'),['is-active','--quiet','multitap-broker'],{encoding:'utf8',env:mockEnv});
  assert.equal(executableMock.status,0,'QA mock commands are not executable; refusing to run the provisioning guard.');
  const preload=join(base,'corrupt-backup.cjs');
  await writeFile(preload,`const sqlite=require('node:sqlite');const original=sqlite.backup;sqlite.backup=async(...args)=>{const result=await original(...args);require('node:fs').writeFileSync(args[1],'deliberately-invalid-fixture');return result;};require('node:module').syncBuiltinESMExports();\n`,{mode:0o600});
  const nodeWrapper=`#!/bin/bash\nset -eu\nif [[ $QA_SCENARIO == backup-failure || $QA_SCENARIO == restart-unhealthy ]]; then echo fixture_backup_refused >&2; exit 19; fi\nif [[ $QA_SCENARIO == invalid-backup ]]; then exec "$QA_NODE" --require "$QA_PRELOAD" "$@"; fi\nexec "$QA_NODE" "$@"\n`;
  await writeFile(join(runtime,'bin','node'),nodeWrapper,{mode:0o700});
  const isolated=guard.replaceAll('/var/lib/multitap',state).replace(/\b(systemctl|curl|runuser|chown|install)\b(?=\s)/g,
    (_match,command)=>quote(join(mock,command)));
  assert.ok(!/\b(systemctl|curl|runuser|chown|install)\s/.test(isolated),'Unbound external fixture command.');
  assert.ok(!isolated.includes('/var/lib/multitap')&&!isolated.includes('/etc/')&&!isolated.includes('/opt/multitap/'),'Fixture contains a production data or configuration path.');
  const script=`#!/bin/bash\nset -euo pipefail\numask 077\nroot=${quote(root)}\nruntime=${quote(runtime)}\nrelease=candidate\nrelease_dir=${quote(join(root,'releases','candidate'))}\nprevious=${quote(join(root,'releases','previous'))}\nhealth_tls_mode=proxy\nhealth_url=http://127.0.0.1:18480/healthz\n${isolated}\nln -sfn "$release_dir" "$root/current"\n"$QA_NODE" --input-type=module - ${quote(databasePath)} ${quote(accountPath)} <<'MUTATE'\nimport {DatabaseSync} from 'node:sqlite';import{writeFileSync}from'node:fs';const db=new DatabaseSync(process.argv[2]);db.exec("PRAGMA user_version=8;UPDATE retained SET value='changed-by-isolated-migration';");db.close();writeFileSync(process.argv[3],'{"fixture":"changed-by-isolated-migration"}\\n');\nMUTATE\nfalse\n`;
  const executable=join(base,'case.sh');await writeFile(executable,script,{mode:0o700});
  const checked=spawnSync('bash',['-n',executable],{encoding:'utf8'});assert.equal(checked.status,0,checked.stderr);
  const run=spawnSync('bash',[executable],{encoding:'utf8',timeout:20000,env:{...process.env,PATH:mock+':/usr/bin:/bin',QA_CASE_ROOT:base,QA_SERVICE_LOG:log,QA_SCENARIO:scenario,QA_NODE:process.execPath,QA_PRELOAD:preload}});
  assert.equal(run.status,1,`Unexpected fixture exit for ${scenario}: ${run.stderr}`);
  const services=(await readFile(log,'utf8')).trim().split('\n');assert.deepEqual(services,['stop','stop','restart']);
  assert.equal(await readlink(join(root,'current')),join(root,'releases','previous'));
  const restored=new DatabaseSync(databasePath,{readOnly:true});
  try{assert.equal(restored.prepare('PRAGMA user_version').get().user_version,7);assert.equal(restored.prepare('SELECT value FROM retained WHERE id=1').get().value,'isolated-account-fixture');assert.equal(Object.values(restored.prepare('PRAGMA quick_check').get())[0],'ok');}finally{restored.close();}
  assert.equal(await readFile(accountPath,'utf8'),originalAccount);
  const backupPath=join(state,'backups','broker-candidate.sqlite');
  if(scenario==='migration-failure') {
    assert.equal((await stat(backupPath)).mode&0o777,0o600);assert.equal((await stat(databasePath)).mode&0o777,0o600);
    assert.ok(run.stderr.includes('validated database backup'));
  } else {
    await assert.rejects(stat(backupPath),{code:'ENOENT'});assert.equal(digest(await readFile(databasePath)),originalHash);
    assert.ok(run.stderr.includes(scenario==='restart-unhealthy'?'automatic recovery is incomplete':'kept the original database'));
    assert.ok(run.stderr.includes(scenario==='invalid-backup'?'file is not a database':'fixture_backup_refused'),'The intended backup failure was not reached.');
  }
  results.push({scenario,passed:true,serviceCallsMocked:true,previousReleaseRestored:true,databaseReplaced:scenario==='migration-failure',originalAccountPreserved:true,productionServiceChanged:false,productionDatabaseAccessed:false});
}
// Execute the production capacity guard with controlled filesystem observations.
const spaceStart=source.indexOf('# Deployment free-space guard:'),spaceEnd=source.indexOf('# End deployment free-space guard.',spaceStart);
assert.ok(spaceStart>=0&&spaceEnd>spaceStart&&spaceEnd<start);
const spaceGuard=source.slice(spaceStart,spaceEnd);
for(const available of [0,1024,262143,262144,524288]){
 const base=join(qaRoot,'space-'+available),mock=join(base,'mock');await mkdir(mock,{recursive:true});
 await writeFile(join(mock,'df'),`#!/bin/bash\nprintf 'Filesystem 1024-blocks Used Available Capacity Mounted\\nfixture 1048576 0 ${available} 0%% /fixture\\n'\n`,{mode:0o700});
  const executableMock=spawnSync(join(mock,'df'),['-Pk',base],{encoding:'utf8'});
  assert.equal(executableMock.status,0,'QA df mock is not executable; refusing to run the capacity guard.');
  const script=join(base,'guard.sh');await writeFile(script,'#!/bin/bash\nset -euo pipefail\n'+spaceGuard.replaceAll('/var/lib/multitap',base).replaceAll('/var/lib',base).replaceAll('/opt',base).replace(/\bdf\b(?=\s)/g,quote(join(mock,'df')))+'\n',{mode:0o700});
 const run=spawnSync('bash',[script],{encoding:'utf8',env:{...process.env,PATH:mock+':/usr/bin:/bin'}});
 assert.equal(run.status,available<262144?1:0,run.stderr);
 results.push({scenario:'free-space-'+available,passed:true,productionServiceChanged:false,productionDatabaseAccessed:false});
}
console.log(JSON.stringify({kind:'isolated-provision-rollback',bashSyntax:true,fixtureRoot:qaRoot,cases:results}));

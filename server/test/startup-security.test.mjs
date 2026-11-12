import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, dirname, resolve, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import { configuration } from '../src/config.mjs';
import { startupSecurityWarnings } from '../src/startup-security.mjs';

const none = () => null;
const legacy = env => configuration({ ...env }, { readAccessFile:none });
const record = role => ({ service:'multitap-broker', event:'security_warning', code:'cleartext_http_enabled',
  scope:'configured_http_endpoint', originRole:role, exposure:'not_inferred', credentialsAndSessionDataMayBePlaintext:true });

test('Default transport stays HTTPS-only; explicit HTTP opt-in remains required', () => {
  assert.deepEqual(startupSecurityWarnings(legacy({})),[]);
  assert.throws(()=>legacy({MULTITAP_PUBLIC_ORIGIN:'http://127.0.0.1:18480'}));
  const config=legacy({MULTITAP_PUBLIC_ORIGIN:'http://127.0.0.1:18480',MULTITAP_ALLOW_INSECURE_HTTP:'1'});
  assert.deepEqual(startupSecurityWarnings(config),[record('primary')]);
  assert.equal(config.accessConfig.http.enabled,true);
});

test('Additional internal-looking HTTP endpoint warns without claiming public exposure or leaking addresses', () => {
  const config=legacy({MULTITAP_PUBLIC_ORIGIN:'https://public-fixture.invalid'});
  config.accessConfig.http={...config.accessConfig.http,enabled:true,origin:'http://192.0.2.121:18480'};
  const warnings=startupSecurityWarnings(config);
  assert.deepEqual(warnings,[record('additional')]);
  const output=JSON.stringify(warnings);
  for(const value of ['192.0.2.121','public-fixture.invalid','http://','https://'])assert.equal(output.includes(value),false);
});

test('TLS proxy backend and bootstrap-only HTTP are not advertised as an enabled application HTTP origin', () => {
  const proxy=legacy({MULTITAP_PUBLIC_ORIGIN:'https://public-fixture.invalid',MULTITAP_TLS_MODE:'proxy'});
  assert.equal(proxy.accessConfig.https.mode,'proxy');
  assert.deepEqual(startupSecurityWarnings(proxy),[]);
  const bootstrap=legacy({NODE_ENV:'production',MULTITAP_DATA_DIR:'/fixture'});
  assert.equal(bootstrap.bootstrapOnly,true);
  assert.deepEqual(startupSecurityWarnings(bootstrap),[]);
});

test('Actual broker startup emits one bounded structured warning only for explicit HTTP', {timeout:15000}, async t=>{
  const directory=await mkdtemp(join(tmpdir(),'multitap-startup-security-'));
  let child,exited;
  t.after(async()=>{
    // Close our owned child before removing SQLite/WAL files on Windows.
    if(child&&child.exitCode===null&&child.signalCode===null)child.kill('SIGTERM');
    await exited?.catch(()=>{});
    assert.equal(dirname(resolve(directory)),resolve(tmpdir()));
    assert.ok(basename(directory).startsWith('multitap-startup-security-'));
    await rm(directory,{recursive:true});
  });
  const reservation=createServer();
  await new Promise((done,fail)=>{reservation.once('error',fail);reservation.listen(0,'127.0.0.1',done);});
  const port=reservation.address().port;
  await new Promise(resolve=>reservation.close(resolve));
  const env={...process.env,NODE_ENV:'development',MULTITAP_DATA_DIR:directory,MULTITAP_HOST:'127.0.0.1',
    MULTITAP_PORT:String(port),MULTITAP_PUBLIC_ORIGIN:`http://127.0.0.1:${port}`,MULTITAP_ALLOW_INSECURE_HTTP:'1',
    MULTITAP_TLS_MODE:'proxy'};
  for(const key of ['MULTITAP_SERVER_ACCESS_FILE','MULTITAP_TLS_CERT_FILE','MULTITAP_TLS_KEY_FILE','MULTITAP_UPDATE_SOCKET'])delete env[key];
  child=spawn(process.execPath,[fileURLToPath(new URL('../src/index.mjs',import.meta.url))],{env,stdio:['ignore','pipe','pipe']});
  exited=once(child,'exit');
  let stdout='',stderr='';
  const records=text=>text.split(/\r?\n/).flatMap(line=>{try{return [JSON.parse(line)];}catch{return [];}});
  await new Promise((done,fail)=>{
    const timer=setTimeout(()=>fail(Error('startup_fixture_timeout')),10000);
    const check=()=>{
      if(stdout.length+stderr.length>32768){clearTimeout(timer);fail(Error('startup_fixture_output_limit'));return;}
      if(records(stdout).some(row=>row.service==='multitap-broker'&&row.listeners)&&records(stderr).some(row=>row.code==='cleartext_http_enabled')){
        clearTimeout(timer);done();
      }
    };
    child.stdout.on('data',bytes=>{stdout+=bytes.toString('utf8');check();});
    child.stderr.on('data',bytes=>{stderr+=bytes.toString('utf8');check();});
    child.once('error',error=>{clearTimeout(timer);fail(error);});
    child.once('exit',()=>{clearTimeout(timer);fail(Error('broker_exited_before_startup'));});
  });
  assert.deepEqual(records(stderr).filter(row=>row.event==='security_warning'),[record('primary')]);
  const response=await fetch(`http://127.0.0.1:${port}/healthz`);
  assert.equal(response.status,200);assert.equal((await response.json()).service,'multitap-broker');
});

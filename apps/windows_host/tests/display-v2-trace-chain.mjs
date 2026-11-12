// Native in-memory producer/private-parser/journal/delivery fixture -> the
// exact already deployed Broker106 consumer. No network or guest commands.
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {resolve} from 'node:path';
import {safeHostEvent,safeDiagnosticReport} from '../../../server/src/host-diagnostics.mjs';

const root=fileURLToPath(new URL('../../../',import.meta.url));
const exe=process.argv[2];assert.ok(exe,'Pass the freshly compiled MultitapHostDisplayV2TraceTest.exe path');
const consumer='server/src/host-diagnostics.mjs',blob='9103b410a03ba1966b5b2954200316a69495f14f';
assert.equal(execFileSync('git',['-C',root,'rev-parse',`e86223b16f394419e5ad9dc0961279fb547e98e3:${consumer}`],{encoding:'utf8'}).trim(),blob);
assert.equal(execFileSync('git',['-C',root,'hash-object',consumer],{encoding:'utf8'}).trim(),blob,'Consumer differs from the actual deployed106 source');
const fixture=JSON.parse(execFileSync(resolve(exe),['--fixture'],{encoding:'utf8',windowsHide:true,maxBuffer:1024*1024}));
assert.equal(fixture.kind,'display-v2-trace-native-fixture');assert.equal(fixture.osDisplayChanges,0);
assert.ok(fixture.checks>60);assert.ok(fixture.journal.length>8);assert.ok(fixture.reports.length>1);
const serial=x=>JSON.stringify(x),wanted=new Set();
for(const row of fixture.journal){
  const safe=safeHostEvent(row);assert.ok(safe,'Broker106 rejected a real producer row');assert.deepEqual(safe,row,'Broker106 removed a trace field');
  assert.equal(row.event,'hostAction');assert.equal(row.values.action,280);wanted.add(serial(safe));
}
const delivered=new Set();let reports=0;
for(const report of fixture.reports){
  assert.ok(Buffer.byteLength(serial(report))<=6144);
  const safe=safeDiagnosticReport(report);assert.ok(safe,'Broker106 rejected the actual report/delivery output');
  assert.ok(safe.events.length<=8);for(const row of safe.events)delivered.add(serial(row));reports++;
}
assert.deepEqual([...delivered].sort(),[...wanted].sort(),'A trace stage/track disappeared in report delivery/106 consumption');
const geometry=fixture.journal.filter(r=>[1,2,4].includes(r.values.stage));
const signed=bits=>bits>=0x80000000?bits-0x100000000:bits;
assert.ok(geometry.some(r=>signed(r.values.expected)===-1280&&signed(r.values.missing)===-720),'Negative coordinate bit encoding was lost');
assert.ok(geometry.some(r=>r.values.recovery===true&&r.values.stale===1),'Older saved-state reuse was lost');
assert.ok(fixture.journal.some(r=>r.values.stage===3&&r.values.format===3&&r.values.pending===true&&r.values.owned===true),'Failed restore/state-preserved evidence was lost');
assert.ok(fixture.journal.some(r=>r.values.stage===3&&r.values.format===0&&r.values.pending===false&&r.values.owned===true),'No-existing-state outcome was lost');
assert.ok(fixture.journal.some(r=>r.values.stage===5&&r.values.cause===1),'Missing stopped telemetry was lost');
assert.ok(!/12345678-1234-4234-8234-123456789abc|older-private-session|PRIVATE-|displayTrace|worker\.v2\.trace/.test(serial(fixture.journal)+serial(fixture.reports)),'Private fields reached the public journal/report');
const sha=b=>createHash('sha256').update(b).digest('hex');
console.log(JSON.stringify({kind:'display-v2-trace-full-chain',passed:true,nativeChecks:fixture.checks,journalRows:fixture.journal.length,reports,deliveredRows:delivered.size,consumerBroker:'1.0.6',consumerGitBlob:blob,executableSha256:sha(readFileSync(resolve(exe))),chain:'actual DisplaySessionV2 producer -> private envelope codec/strict parser/strip -> production journal formatter + reader filter -> actual report selection/ACK backlog -> deployed106 event/report consumers',networkCalls:0,guestCommands:0,osDisplayChanges:0}));

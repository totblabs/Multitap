// Existing isolated software decoder only. No package installation, desktop,
// network, service, VM, registry or display access.
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {readFileSync, writeFileSync, existsSync} from 'node:fs';
import {resolve, join, basename} from 'node:path';
const [ffmpegArg, ffprobeArg, ...directories] = process.argv.slice(2);
if (!ffmpegArg || !ffprobeArg || !directories.length) throw new Error('Usage: node validate.mjs EXISTING_FFMPEG EXISTING_FFPROBE RESULT_DIRECTORY ...');
const [ffmpeg, ffprobe] = [ffmpegArg, ffprobeArg].map(value => resolve(value));
if (![ffmpeg, ffprobe].every(existsSync)) throw new Error('Existing QA decoder binaries required.');
const hash = path => createHash('sha256').update(readFileSync(path)).digest('hex');
const run = (file, args) => execFileSync(file, args, {encoding:'utf8', windowsHide:true, timeout:30000, maxBuffer:1024*1024});
const mean = values => values.reduce((sum, value) => sum + value, 0) / values.length;
const all = [];
for (const directoryArg of directories) {
  const directory = resolve(directoryArg), raw = readFileSync(join(directory, 'native.jsonl'));
  const content = (raw[0] === 0xff && raw[1] === 0xfe ? raw.toString('utf16le') : raw.toString('utf8')).replace(/^\uFEFF/, '');
  const records = content.trim().split(/\r?\n/).map(line => JSON.parse(line));
  const config = records.find(value => value.kind === 'dual-output-encode-config');
  const samples = records.filter(value => value.kind === 'dual-output-encode-sample');
  if (!config || samples.length !== config.repeats * 12) throw new Error('Incomplete benchmark case set.');
  const decoded = [];
  for (const sample of samples) {
    for (const track of sample.tracks) {
      if (basename(track.file) !== track.file || !track.file.endsWith('.h264')) throw new Error('Unsafe generated stream name.');
      const path = join(directory, track.file);
      if (readFileSync(path).length !== track.bytes || track.encodedFrames !== track.submittedFrames) throw new Error('Generated stream byte/frame count mismatch.');
      // Every scenario/policy/architecture at repeat zero is independently
      // decoded in full. Repeated timing samples are checked for bytes/counts.
      if (sample.repeat !== 0) continue;
      const probe = JSON.parse(run(ffprobe, ['-v','error','-select_streams','v:0','-show_entries','stream=width,height,profile,pix_fmt','-of','json',path])).streams?.[0];
      if (!probe || probe.width !== track.width || probe.height !== track.height || probe.pix_fmt !== 'yuv420p') throw new Error('Independent decoder geometry/format mismatch.');
      const progress = run(ffmpeg, ['-hide_banner','-loglevel','error','-nostdin','-xerror','-err_detect','explode','-threads','2','-hwaccel','none','-i',path,'-map','0:v:0','-progress','pipe:1','-nostats','-f','null','-']);
      const frames = [...progress.matchAll(/^frame=(\d+)\s*$/gm)].map(match => Number(match[1])).at(-1);
      if (frames !== track.encodedFrames) throw new Error('Independent decoder frame count mismatch.');
      decoded.push({file:track.file, frames, width:probe.width, height:probe.height, profile:probe.profile, pixelFormat:probe.pix_fmt, sha256:hash(path), passed:true});
    }
  }
  const groups = [];
  for (const scene of ['both-motion','one-motion-one-static','both-static']) for (const policy of ['unconditional','changed-gated']) {
    const pair = {};
    for (const architecture of ['composite','two-tracks']) {
      const rows = samples.filter(value => value.scene === scene && value.policy === policy && value.architecture === architecture);
      if (rows.length !== config.repeats || rows.some(value => !Number.isFinite(value.encodeCpuMs) || value.encodeCpuMs <= 0 || value.sourcePixelsPerTick !== 4147200 || value.totalTargetBitrate !== 12000000)) throw new Error('Invalid timing/repeat/equivalent-input contract.');
      pair[architecture] = {
        repeatCount:rows.length, cpuMeanMs:mean(rows.map(value => value.encodeCpuMs)),
        cpuMinMs:Math.min(...rows.map(value => value.encodeCpuMs)), cpuMaxMs:Math.max(...rows.map(value => value.encodeCpuMs)),
        wallMeanMs:mean(rows.map(value => value.encodeWallMs)),
        oneCoreEquivalentPercentMean:mean(rows.map(value => value.oneCoreEquivalentPercent)),
        actualMediaMbpsMean:mean(rows.map(value => value.actualMediaMbps)),
        submittedFrames:rows.map(value => value.submittedFrames), encodedFrames:rows.map(value => value.encodedFrames),
      };
    }
    groups.push({scene,policy,...pair,twoTracksCpuChangePercent:(pair['two-tracks'].cpuMeanMs / pair.composite.cpuMeanMs - 1) * 100});
  }
  const report = {
    kind:'dual-output-encode-comparison',config,groups,decoded,
    policyEffects:['both-motion','one-motion-one-static','both-static'].map(scene => ({
      scene,
      ...Object.fromEntries(['composite','two-tracks'].map(architecture => {
        const baseline=groups.find(value => value.scene===scene&&value.policy==='unconditional')[architecture].cpuMeanMs;
        const gated=groups.find(value => value.scene===scene&&value.policy==='changed-gated')[architecture].cpuMeanMs;
        return [architecture,{unconditionalCpuMeanMs:baseline,gatedCpuMeanMs:gated,cpuChangePercent:(gated/baseline-1)*100}];
      })),
    })),
    decodedStreams:decoded.length,independentlyDecodedFrames:decoded.reduce((sum,value) => sum + value.frames,0),
    scope:'Actual product software Encoder only; unpaced precomputed equal NV12 pixels; memory output collection and frame-submission bookkeeping included; initialization separately measured',
    policyImplementation:config.policyImplementation ?? 'oracle-scenario-dirty-flags-v0',
    actualDirtyComparisonMeasured:config.actualDirtyComparisonMeasured ?? false,
    captureOrCompositingMeasured:false,networkOrPresentationMeasured:false,qualityEquivalenceProven:false,
    realVm149Tested:false,finishedDualSession:false,productDefaultsChanged:false,
    cpuCounter:'GetProcessTimes user+kernel process CPU; one-core equivalent is CPU/media-duration, not Task Manager total CPU or an actual live-session measurement',
    decoder:{ffmpegSHA256:hash(ffmpeg),ffprobeSHA256:hash(ffprobe),software:true,threads:2},
  };
  writeFileSync(join(directory,'validated.json'),JSON.stringify(report,null,2));
  all.push({directory:basename(directory),fps:config.fpsTarget,policyImplementation:report.policyImplementation,groups,decodedStreams:report.decodedStreams,independentlyDecodedFrames:report.independentlyDecodedFrames});
  for (const group of groups) console.log(JSON.stringify({fps:config.fpsTarget,scene:group.scene,policy:group.policy,compositeCpuMs:group.composite.cpuMeanMs,twoTracksCpuMs:group['two-tracks'].cpuMeanMs,twoTracksCpuChangePercent:group.twoTracksCpuChangePercent}));
}
writeFileSync(new URL('./summary.json',import.meta.url),JSON.stringify(all,null,2));

// Uses only generated fixture files and the already available isolated FFmpeg
// QA tools. It never opens a desktop, service, VM, account or network socket.
import {execFileSync} from 'node:child_process';
import {mkdirSync, readFileSync, writeFileSync, existsSync} from 'node:fs';
import {resolve, join} from 'node:path';

const [nativeArg, ffmpegArg, ffprobeArg, outputArg, ...extra] = process.argv.slice(2);
if (extra.length || !outputArg) throw new Error('Usage: node measure.mjs NATIVE_EXE EXISTING_FFMPEG EXISTING_FFPROBE NEW_OUTPUT_DIRECTORY');
const [native, ffmpeg, ffprobe, output] = [nativeArg, ffmpegArg, ffprobeArg, outputArg].map(value => resolve(value));
if ([native,ffmpeg,ffprobe].some(path => !existsSync(path))) throw new Error('Existing QA binaries are required; this tool installs nothing.');
if (existsSync(output)) throw new Error('Refusing to replace an existing output directory.');
mkdirSync(output, {recursive: true});
const run = (file, args) => execFileSync(file,args,{encoding:'utf8',windowsHide:true,timeout:60000,maxBuffer:1024*1024});
const raw = run(native,[output]);
writeFileSync(join(output,'native.jsonl'),raw);
const records = raw.trim().split(/\r?\n/).map(line => JSON.parse(line));
const results = [];
function metrics(reference, sampled, decoded, width, height) {
  const groups = {text: {count:0, sampling:0, codec:0, total:0, mismatches:0}, colourInk:{count:0,sampling:0,codec:0,total:0,mismatches:0}};
  if ([reference,sampled,decoded].some(bytes => bytes.length !== width*height*4)) throw new Error('Independent decoder returned incomplete geometry.');
  for (let y=0;y<Math.min(310,height);y++) for(let x=16;x<Math.min(960,width);x++) {
    const at=(y*width+x)*4, rgb=[reference[at+2],reference[at+1],reference[at]];
    const colourInk=Math.max(...rgb)-Math.min(...rgb)>32 && Math.min(...rgb)<230;
    for(const key of colourInk?['text','colourInk']:['text']) for(let channel=0;channel<3;channel++) {
      const group=groups[key], a=rgb[channel], b=sampled[at+channel], c=decoded[at+channel];
      group.count++;group.sampling+=(a-b)**2;group.codec+=(b-c)**2;group.total+=(a-c)**2;
      if(a!==c)group.mismatches++;
    }
  }
  const psnr=(squared,count) => squared===0 ? null : 10*Math.log10(255*255*count/squared);
  return Object.fromEntries(Object.entries(groups).map(([key,group]) => [key,{
    rgbSamples:group.count, referenceToNv12PsnrDb:psnr(group.sampling,group.count),
    nv12ToH264PsnrDb:psnr(group.codec,group.count), referenceToH264PsnrDb:psnr(group.total,group.count),
    rgbMismatchSamples:group.mismatches, exactReferenceRgb:group.mismatches===0,
  }]));
}
for(const item of records.filter(record => record.kind==='generated-text-encode')) {
  const base=join(output,item.name), sampled=base+'-nv12.rgba', decoded=base+'-decoded.rgba';
  const colour='scale=in_range=limited:out_range=full:in_color_matrix=bt601';
  const common=['-hide_banner','-loglevel','error','-nostdin','-xerror','-threads','2','-filter_threads','1'];
  run(ffmpeg,[...common,'-f','rawvideo','-pixel_format','nv12','-video_size',`${item.width}x${item.height}`,
    '-framerate','60','-i',base+'.nv12','-vf',colour,'-frames:v','1','-pix_fmt','rgba','-f','rawvideo',sampled]);
  run(ffmpeg,[...common,'-err_detect','explode','-hwaccel','none','-i',base+'.h264',
    '-vf',`select=eq(n\\,${item.frames-1}),${colour}`,'-fps_mode','passthrough','-frames:v','1','-pix_fmt','rgba','-f','rawvideo',decoded]);
  const probe=JSON.parse(run(ffprobe,['-v','error','-select_streams','v:0','-show_entries',
    'stream=profile,pix_fmt,width,height,color_space,color_range','-of','json',base+'.h264'])).streams?.[0];
  if (!probe || probe.width!==item.width || probe.height!==item.height) throw new Error('Independent stream metadata mismatch.');
  const quality=metrics(readFileSync(base+'.bgra'),readFileSync(sampled),readFileSync(decoded),item.width,item.height);
  const result={...item,independentDecodePassed:true,independentProfile:probe.profile,independentPixelFormat:probe.pix_fmt,
    independentColourSpace:probe.color_space??null,independentRange:probe.color_range??null,
    ...quality, shortColdStream:true, realVmTested:false, productPolicyChanged:false};
  results.push(result);
  console.log(JSON.stringify(result));
}
writeFileSync(join(output,'quality.json'),JSON.stringify(results,null,2));
writeFileSync(join(output,'summary.json'),JSON.stringify({generatedFrames:results.reduce((sum,item)=>sum+item.frames,0),
  fixtures:results.length, decoder:'existing isolated FFmpeg software H264, threads=2',
  nativeProcessPriorityRequested:'below normal',nativeAffinityLogicalCpusRequested:2,
  scenesRead:'generated memory DIB only',desktopCaptured:false,serviceOrDisplayChanged:false,
  product444Offered:false,productLosslessOffered:false,realVm149Acceptance:false},null,2));

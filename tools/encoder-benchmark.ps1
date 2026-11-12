# Runs MultitapHostEncoderBenchmark (apps/windows_host/src/encoder_benchmark.cpp)
# into a NEW output directory while sampling whole-machine CPU load every 2 s
# (other processes share this machine), then prints a summary table. Reads no
# desktop or user file; writes only the output directory and its siblings.
#   tools/encoder-benchmark.ps1 -Executable <exe> -Out <new dir> -ArgLine "cores=4 sizes=1080 rates=8,20"
#   tools/encoder-benchmark.ps1 -Summarize <dir>/results.jsonl
param([string]$Executable, [string]$Out, [string]$ArgLine = '', [string]$Summarize)
$ErrorActionPreference = 'Stop'
function Show-Summary([string]$Path) {
  $rows = Get-Content -LiteralPath $Path -Encoding UTF8 | Where-Object { $_.Trim() } | ForEach-Object { $_ | ConvertFrom-Json }
  $rows | ForEach-Object {
    $quality = if ($_.scene -eq 'page') { 'first {0:N1} ({5:N0} KB, {6:N0} ms) / end {1:N1} / tts35 {2:N0} ms / link {3:N0} ms / IDR {4:N1}' -f $_.switchFirstInkDb, $_.pageEndInkDb, $_.tts35MeanMs, $_.tts35LinkMaxMs, $_.startIdrInkDb, ($_.switchFirstBytes / 1000), $_.switchFirstLinkMs } else { 'moving {0:N1} / static {1:N1}' -f $_.steadyMovingInkDb, $_.steadyStaticInkDb }
    [pscustomobject]@{
      size = "$($_.width)x$($_.height)"; scene = $_.scene; rate = $_.rate; encoder = $_.encoder
      mbps = '{0:N1}' -f $_.mbps; maxKB = '{0:N0}' -f ($_.maxFrameBytes / 1000)
      'enc p50/p95 ms' = '{0:N2}/{1:N2}' -f $_.encodeP50Ms, $_.encodeP95Ms
      'cpu ms/frame' = '{0:N2}' -f $_.cpuPerSubmitMs; cores = '{0:N2}' -f $_.encoderCores
      'conv ms' = '{0:N2}' -f $_.convertP50Ms; 'pipe cpu ms' = '{0:N2}' -f $_.pipelineCpuMs; 'pipe fps' = '{0:N0}' -f $_.pipelineFps
      'link p50/p95 ms' = '{0:N0}/{1:N0}' -f $_.linkDelayP50Ms, $_.linkDelayP95Ms
      'ink dB' = $quality
    }
  } | Format-Table -AutoSize | Out-String -Width 260
}
if ($Summarize) { Show-Summary $Summarize; exit 0 }
if (-not $Executable -or -not $Out) { throw 'Provide -Executable and -Out (or -Summarize).' }
if (Test-Path -LiteralPath $Out) { throw "Output exists: $Out" }
$cpus = [Environment]::ProcessorCount
$process = Start-Process -FilePath $Executable -ArgumentList "`"$Out`" $ArgLine" -RedirectStandardOutput "$Out.stdout.txt" -RedirectStandardError "$Out.stderr.txt" -PassThru -NoNewWindow
$null = $process.Handle  # keeps ExitCode readable after exit
$load = "$Out.cpuload.csv"
'time,totalPercent,benchmarkPercentOfMachine,otherPercentOfMachine' | Out-File -Encoding utf8 $load
$lastCpu = 0.0; $lastTime = Get-Date
while (-not $process.HasExited) {
  Start-Sleep -Seconds 2
  try {
    $process.Refresh(); $now = Get-Date
    $total = [double](Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'").PercentProcessorTime
    $cpu = $process.TotalProcessorTime.TotalMilliseconds
    $own = 100.0 * ($cpu - $lastCpu) / (($now - $lastTime).TotalMilliseconds * $cpus)
    $lastCpu = $cpu; $lastTime = $now
    '{0},{1:N1},{2:N1},{3:N1}' -f $now.ToString('o'), $total, $own, [math]::Max(0, $total - $own) | Add-Content -Encoding utf8 $load
  } catch {}
}
$process.WaitForExit()
$samples = Import-Csv -LiteralPath $load
if ($samples) { 'other processes: mean {0:N1}% of the machine, max {1:N1}%' -f (($samples | Measure-Object otherPercentOfMachine -Average).Average), (($samples | Measure-Object otherPercentOfMachine -Maximum).Maximum) }
"exit $($process.ExitCode)"
if (Test-Path -LiteralPath (Join-Path $Out 'results.jsonl')) { Show-Summary (Join-Path $Out 'results.jsonl') }

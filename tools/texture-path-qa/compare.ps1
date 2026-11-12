param([Parameter(Mandatory)][string]$Base, [Parameter(Mandatory)][string]$New, [Parameter(Mandatory)][string]$Out, [int]$Repeat = 2, [int]$Seconds = 15, [string[]]$Cases)
# Runs each case with both harness builds, interleaved, appending one JSON line
# per run to $Out. Case: label|width|height|fps|pattern|hz|jitterMs|driftPpm
#   pattern 0 = steady fps, 1 = pairs (two packets back to back at fps/2),
#           2 = 60 Hz host ticks encoding `fps` of them, arriving 0..jitter ms late
#   hz 0 = the real DWM composition rate; N = Flutter frame scheduling at N Hz
#   drift = source clock faster by N ppm (sweeps the arrival phase)
if (-not $Cases) {
  $Cases = @('760p37-pairs|1264|760|37|1|60|0|0', '760p37-steady|1264|760|37|0|60|0|0', '760p60-drift|1264|760|60|0|60|0|5000',
    '760p59-host-j4-drift|1264|760|59|2|60|4|5000', '1440p60-drift|2560|1440|60|0|60|0|5000', '760p37-pairs-dwm|1264|760|37|1|0|0|0')
}
foreach ($k in 1..$Repeat) {
  foreach ($case in $Cases) {
    foreach ($engine in @(@('base', $Base), @('new', $New))) {
      $p = $case.Split('|')
      $arguments = @("--label=$($engine[0])|$($p[0])|$k", "--width=$($p[1])", "--height=$($p[2])", "--fps=$($p[3])", "--burst=$($p[4])", "--hz=$($p[5])", "--jitter=$($p[6])", "--drift=$($p[7])", '--window=1280x800', "--seconds=$Seconds", '--warmup=3', "--out=$Out")
      $process = Start-Process -FilePath $engine[1] -ArgumentList $arguments -PassThru
      if (-not $process.WaitForExit(($Seconds + 120) * 1000)) { $process.Kill(); Write-Warning "timeout $($engine[0]) $($p[0])" }
    }
  }
}

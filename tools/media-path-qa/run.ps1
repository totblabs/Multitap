param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][string]$Out, [int]$Seconds = 15, [int]$Repeat = 1, [string[]]$Cases)
# Runs each case (label|harness arguments separated by spaces) $Repeat times,
# one process per run, appending one JSON line per run to $Out.
if (-not $Cases) {
  $Cases = @(
    'idle|--fps=18',
    'idle-audio|--fps=18 --audio=100 --audioDevice=1',
    'burn10|--fps=18 --audio=100 --audioDevice=1 --burn=10',
    'burn10-boost|--fps=18 --audio=100 --audioDevice=1 --burn=10 --boost=1',
    'burn20|--fps=18 --audio=100 --audioDevice=1 --burn=20',
    'burn20-boost|--fps=18 --audio=100 --audioDevice=1 --burn=20 --boost=1'
  )
}
foreach ($k in 1..$Repeat) {
  foreach ($case in $Cases) {
    $label, $rest = $case.Split('|', 2)
    $arguments = @("--label=$label|$k") + $rest.Split(' ', [StringSplitOptions]::RemoveEmptyEntries) + @("--seconds=$Seconds", '--warmup=3', "--out=$Out")
    $process = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru
    if (-not $process.WaitForExit(($Seconds + 120) * 1000)) { $process.Kill(); Write-Warning "timeout $label" }
  }
}

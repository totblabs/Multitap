param([Parameter(Mandatory)][string]$In)
# Averages compare.ps1 runs per case and engine.
$rows = Get-Content -LiteralPath $In | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { -not $_.error }
foreach ($group in ($rows | Group-Object { ($_.label.Split('|'))[1] })) {
  foreach ($engine in 'base', 'new') {
    $set = @($group.Group | Where-Object { $_.label.StartsWith("$engine|") })
    if (-not $set.Count) { continue }
    $avg = { param($name) ($set | Measure-Object -Property $name -Average).Average }
    $wait = ($set | ForEach-Object { if ($null -ne $_.engine.textureWaitTotalMs -and $_.engine.textureFrames) { $_.engine.textureWaitTotalMs / $_.engine.textureFrames } } | Measure-Object -Average).Average
    '{0,-22} {1,-4} decode {2,5:N1}  texture {3,5:N1}  skipped/s {4,5:N1}  upload {5,5:N3} ms  wait {6,6} ms  decode {7,4:N2} ms  process CPU {8,4:N0}% of one core  runs {9}' -f `
      $group.Name, $engine, (& $avg 'decodeFps'), (& $avg 'textureFps'), (($set | ForEach-Object { $_.skipped / $_.seconds } | Measure-Object -Average).Average), (& $avg 'uploadMs'), `
      $(if ($wait) { '{0:N1}' -f $wait } else { 'n/a' }), (& $avg 'decodeMs'), (& $avg 'cpuPercentOfOneCore'), $set.Count
  }
}

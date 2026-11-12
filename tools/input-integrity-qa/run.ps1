$ErrorActionPreference = 'Stop'
$qaRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$executable = Join-Path $qaRoot 'build/Release/input_integrity_qa.exe'
if (!(Test-Path -LiteralPath $executable)) { throw 'Build the independent QA executable first.' }
$runId = [Guid]::NewGuid().ToString()
$runDirectory = Join-Path $qaRoot (Join-Path 'results' $runId)
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null
$controller = Start-Process -FilePath $executable -ArgumentList @('--controller', $runId, ('"' + $runDirectory + '"')) -WindowStyle Hidden -PassThru
$deadline = [DateTime]::UtcNow.AddSeconds(8)
while (!(Test-Path -LiteralPath (Join-Path $runDirectory 'controller.ready'))) {
  if ($controller.HasExited -or [DateTime]::UtcNow -gt $deadline) { throw 'Medium controller was not ready.' }
  Start-Sleep -Milliseconds 100
}
$target = Start-Process -FilePath $executable -ArgumentList @('--target', $runId) -Verb RunAs -WindowStyle Hidden -PassThru
$deadline = [DateTime]::UtcNow.AddSeconds(75)
while (!(Test-Path -LiteralPath (Join-Path $runDirectory 'medium.ready'))) {
  if ($controller.HasExited -or [DateTime]::UtcNow -gt $deadline) {
    $result = Join-Path $runDirectory 'result.json'
    if (Test-Path -LiteralPath $result) { Get-Content -LiteralPath $result -Raw }
    throw 'Own-window medium phase did not pass; high injection was not attempted.'
  }
  Start-Sleep -Milliseconds 100
}
$high = Start-Process -FilePath $executable -ArgumentList @('--high', $runId) -Verb RunAs -WindowStyle Hidden -PassThru
$deadline = [DateTime]::UtcNow.AddSeconds(25)
while (!$controller.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100; $controller.Refresh() }
if (!$controller.HasExited) { throw 'Bounded QA controller did not finish.' }
$resultPath = Join-Path $runDirectory 'result.json'
Get-Content -LiteralPath $resultPath -Raw
[pscustomobject]@{runDirectory=$runDirectory; controllerExitCode=$controller.ExitCode; targetHasExited=$target.HasExited; highHasExited=$high.HasExited} | ConvertTo-Json
if ($controller.ExitCode -ne 0) { throw 'Input integrity reproduction did not pass.' }

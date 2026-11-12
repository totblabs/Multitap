param([Parameter(Mandatory=$true)][string]$ResultPath)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$program=Join-Path $projectRoot 'apps/windows_host/build-0.2.4-candidate/Release/MultitapHostClipboardTest.exe'
$run=Join-Path $projectRoot ('.local/host-clipboard-qa/run-'+[guid]::NewGuid().ToString('N'))
try{
  $output=@(& $program $run 2>&1)
  $code=$LASTEXITCODE
  [ordered]@{exitCode=$code;output=$output;interactiveClipboardAccess=$false;productionHostChanged=$false} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $ResultPath -Encoding UTF8
  exit $code
}catch{
  [ordered]@{exitCode=1;error=$_.Exception.Message;interactiveClipboardAccess=$false;productionHostChanged=$false} | ConvertTo-Json | Set-Content -LiteralPath $ResultPath -Encoding UTF8
  exit 1
}

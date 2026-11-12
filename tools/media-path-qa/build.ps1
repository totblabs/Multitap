param([string]$OutDir = (Join-Path $env:TEMP 'multitap-media-qa'), [string]$Flutter)
# Builds the media-path harness: a `flutter create` Windows app with this
# folder's sources, linked against the client engine (RemoteEngine, CPU H.264
# decoder) of the working tree. Prints the path of the built media_qa.exe.
# Keep $OutDir short (MAX_PATH).
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Flutter) { $Flutter = Join-Path $projectRoot '.tools/flutter/bin/flutter.bat' }
$app = Join-Path $OutDir 'app'
if (-not (Test-Path -LiteralPath (Join-Path $app 'pubspec.yaml'))) {
  & $Flutter create --platforms=windows --project-name media_qa --empty $app | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'flutter create failed.' }
}
Copy-Item -Recurse -Force (Join-Path $PSScriptRoot 'app/*') $app
$env:MQ_ENGINE_DIR = (Join-Path $projectRoot 'apps/windows_client/windows/runner').Replace('\', '/')
Push-Location -LiteralPath $app
try {
  & $Flutter build windows --release | Out-Host
  if ($LASTEXITCODE -ne 0) { throw 'Harness build failed.' }
} finally { Pop-Location }
Join-Path $app 'build/windows/x64/runner/Release/media_qa.exe'

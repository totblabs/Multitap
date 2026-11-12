param([string]$OutDir = (Join-Path $env:TEMP 'multitap-texture-qa'), [string]$Ref, [string]$Flutter)
# Builds the texture-path harness: a `flutter create` Windows app with this
# folder's sources, linked against the client engine (RemoteEngine, CPU H.264
# decoder) of the working tree, or of git ref $Ref for a before/after pair.
# Prints the path of the built texture_qa.exe. Keep $OutDir short (MAX_PATH).
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Flutter) { $Flutter = Join-Path $projectRoot '.tools/flutter/bin/flutter.bat' }
$name = if ($Ref) { 'ref-' + ($Ref -replace '[^A-Za-z0-9]', '') } else { 'worktree' }
$app = Join-Path $OutDir "app-$name"
if (-not (Test-Path -LiteralPath (Join-Path $app 'pubspec.yaml'))) {
  & $Flutter create --platforms=windows --project-name texture_qa --empty $app | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'flutter create failed.' }
}
Copy-Item -Recurse -Force (Join-Path $PSScriptRoot 'app/*') $app
if ($Ref) {
  $source = Join-Path $OutDir "src-$name"
  if (-not (Test-Path -LiteralPath $source)) {
    New-Item -ItemType Directory -Force $source | Out-Null
    $zip = Join-Path $OutDir "$name.zip"
    & git -C $projectRoot archive --format=zip -o $zip $Ref apps/windows_client/windows/runner native/windows_video
    if ($LASTEXITCODE -ne 0) { throw 'git archive failed.' }
    Expand-Archive -Force $zip $source; Remove-Item -LiteralPath $zip
  }
  $env:TQ_ENGINE_DIR = (Join-Path $source 'apps/windows_client/windows/runner').Replace('\', '/')
} else {
  $env:TQ_ENGINE_DIR = (Join-Path $projectRoot 'apps/windows_client/windows/runner').Replace('\', '/')
}
Push-Location -LiteralPath $app
try {
  & $Flutter build windows --release | Out-Host
  if ($LASTEXITCODE -ne 0) { throw 'Harness build failed.' }
} finally { Pop-Location }
Join-Path $app 'build/windows/x64/runner/Release/texture_qa.exe'

param(
  # Relay trees to run against: "current" is this checkout's server/, or any "label=<commit-ish>" (extracted with
  # git archive) / "label=<dir>".
  [string[]]$Relays = @('current'),
  # Substring of "relay/mode/scenario" to run a subset.
  [string]$Only = '',
  [string]$Work = (Join-Path ([IO.Path]::GetTempPath()) 'multitap-clipboard-e2e'),
  [switch]$SkipBuild,
  # A checkout whose windows/flutter/ephemeral exists (a worktree that never built the client has none).
  [string]$FlutterEphemeral = ''
)
$ErrorActionPreference = 'Stop'
(Get-Process -Id $PID).PriorityClass = 'BelowNormal'
$Work = [IO.Path]::GetFullPath($Work)
# Every invocation keeps its evidence in new directories. No recursive cleanup is
# needed (including long-path trees and node_modules junctions from older runs).
$runId = [Guid]::NewGuid().ToString('N')
# (powershell -File passes "a,b" as one string)
$Relays = @($Relays | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$tools = Join-Path $repo '.tools'
if (-not (Test-Path -LiteralPath $tools)) { throw 'The repository .tools directory (Visual Studio build tools, Flutter, Node) is required.' }
$toolsTarget = (Get-Item -LiteralPath $tools -Force).Target
$toolsReal = if ($toolsTarget) { @($toolsTarget)[0] } else { $tools }
$node = Join-Path $toolsReal 'node-v24.22.0-win-x64/node.exe'
if (-not (Test-Path -LiteralPath $node)) { $node = (Get-Command node -ErrorAction Stop).Source }
$vs = Join-Path $toolsReal 'visualstudio-buildtools'
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (-not $FlutterEphemeral) {
  $FlutterEphemeral = Join-Path $repo 'apps/windows_client/windows/flutter/ephemeral'
  if (-not (Test-Path -LiteralPath (Join-Path $FlutterEphemeral 'cpp_client_wrapper/include'))) {
    $common = (& git -C $repo rev-parse --path-format=absolute --git-common-dir).Trim()
    $FlutterEphemeral = Join-Path (Split-Path -Parent $common) 'apps/windows_client/windows/flutter/ephemeral'
  }
}
if (-not (Test-Path -LiteralPath (Join-Path $FlutterEphemeral 'cpp_client_wrapper/include'))) { throw "Flutter ephemeral not found ($FlutterEphemeral); build the client once or pass -FlutterEphemeral." }
New-Item -ItemType Directory -Force $Work | Out-Null
$build = Join-Path $Work 'build'

if (-not $SkipBuild) {
  & $cmake -S (Join-Path $PSScriptRoot '.') -B $build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vs" "-DMULTITAP_FLUTTER_EPHEMERAL=$($FlutterEphemeral.Replace('\','/'))" | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
  & $cmake --build $build --config Release --parallel 2 --target clipboard_e2e_host clipboard_e2e_client -- /m:2 | Select-Object -Last 4
  if ($LASTEXITCODE -ne 0) { throw 'Building the helpers failed.' }
}

# The relay trees (src/ + package.json, with the current node_modules reached through a junction).
$serverNodeModules = Join-Path $repo 'server/node_modules'
if (-not (Test-Path -LiteralPath $serverNodeModules)) { & npm ci --prefix (Join-Path $repo 'server') | Out-Null }
$servers = @()
foreach ($relay in $Relays) {
  $label = $relay; $source = $relay
  if ($relay -match '^([^=]+)=(.+)$') { $label = $Matches[1]; $source = $Matches[2] }
  if ($label -notmatch '^[a-zA-Z0-9._-]+$') { throw "Unsafe relay label: $label" }
  if ($source -eq 'current') { $servers += "current=$(Join-Path $repo 'server')"; continue }
  if (Test-Path -LiteralPath $source -PathType Container) { $servers += "$label=$source"; continue }
  $commit = $source
  $directory = Join-Path $Work "relay-$label-$runId"
  New-Item -ItemType Directory -Force $directory | Out-Null
  $archive = Join-Path $Work "relay-$label-$runId.tar"
  & git -C $repo archive --format=tar -o $archive $commit server/src server/package.json
  if ($LASTEXITCODE -ne 0) { throw "git archive $commit failed." }
  & tar -xf $archive -C $directory
  New-Item -ItemType Junction -Path (Join-Path $directory 'server/node_modules') -Target $serverNodeModules | Out-Null
  $version = (Get-Content -LiteralPath (Join-Path $directory 'server/package.json') -Raw | ConvertFrom-Json).version
  Write-Host "Relay $label = commit $commit (broker $version)"
  $servers += "$label=$(Join-Path $directory 'server')"
}

$env:MULTITAP_CLIPBOARD_E2E = '1'
$env:E2E_NODE = $node
$env:E2E_RELAY_FIXTURE = Join-Path $PSScriptRoot 'relay_fixture.mjs'
$env:E2E_SERVERS = ($servers -join ';')
$env:E2E_HOST_EXE = Join-Path $build 'Release/clipboard_e2e_host.exe'
$env:E2E_CLIENT_EXE = Join-Path $build 'Release/clipboard_e2e_client.exe'
$env:E2E_WORK = Join-Path $Work "run-$runId"
$env:E2E_ONLY = $Only
New-Item -ItemType Directory -Force $env:E2E_WORK | Out-Null
Push-Location (Join-Path $repo 'apps/windows_client')
try {
  & (Join-Path $repo 'flutter.cmd') test test/e2e/clipboard_e2e_test.dart --reporter expanded --concurrency 1
  exit $LASTEXITCODE
} finally { Pop-Location }

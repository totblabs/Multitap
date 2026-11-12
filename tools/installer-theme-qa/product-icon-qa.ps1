param([string]$OutputDirectory = '.artifacts/host-brand-audit')

# Run in an x64 MSVC + Windows SDK build environment. This test creates no
# windows and performs no installation, service action or desktop capture.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
  $OutputDirectory = Join-Path $root $OutputDirectory
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($output) | Out-Null
foreach ($tool in @('rc.exe', 'cl.exe')) {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
    throw "$tool is required in the process build environment."
  }
}
$runner = Join-Path $root 'apps/windows_client/windows/runner'
$resource = (Join-Path $runner 'Runner.rc').Replace('\', '/')
$icon = (Join-Path $root 'apps/windows_host/resources/host.ico').Replace('\', '/')
$source = Join-Path $PSScriptRoot 'product_icon_qa.cpp'
$results = @()
foreach ($variant in @('host', 'client', 'missing')) {
  $prefix = Join-Path $output ('branding-' + $variant)
  $arguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MT', '/O2', $source)
  if ($variant -ne 'missing') {
    # Exercise both real Runner.rc version branches with fixed fixture values.
    $build = if ($variant -eq 'host') { 20007 } else { 197 }
    $definition = @"
#define FLUTTER_VERSION "1.0.7"
#define FLUTTER_VERSION_MAJOR 1
#define FLUTTER_VERSION_MINOR 0
#define FLUTTER_VERSION_PATCH 7
#define FLUTTER_VERSION_BUILD $build
#include "$resource"
1 ICON "$icon"
"@
    [IO.File]::WriteAllText($prefix + '.rc', $definition, [Text.UTF8Encoding]::new($false))
    & rc.exe /nologo ('/i' + $runner) ('/fo' + $prefix + '.res') ($prefix + '.rc')
    if ($LASTEXITCODE -ne 0) { throw "Resource compilation failed: $variant" }
    $arguments += $prefix + '.res'
  } else {
    $arguments += '/DMULTITAP_ICON_MISSING_TEST'
  }
  $arguments += @(('/Fo' + $prefix + '.obj'), ('/Fe' + $prefix + '.exe'), '/link',
    'gdiplus.lib', 'user32.lib', 'gdi32.lib', 'version.lib')
  & cl.exe @arguments
  if ($LASTEXITCODE -ne 0) { throw "QA compilation failed: $variant" }
  $resultText = & ($prefix + '.exe') $variant $output
  if ($LASTEXITCODE -ne 0) { throw "Offscreen rendering/resource check failed: $variant" }
  $result = $resultText | ConvertFrom-Json
  if ($result.passed -ne $true) { throw "QA did not report success: $variant" }
  $results += [ordered]@{ variant = $variant; checks = $result }
}
[ordered]@{
  passed = $true
  scope = 'Offscreen icon rendering and compiled Runner.rc metadata; no installed application changes'
  cases = $results
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'product-icon-verification.json') -Encoding UTF8
$results | ConvertTo-Json -Depth 6

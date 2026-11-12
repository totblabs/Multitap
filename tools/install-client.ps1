param([switch]$Launch, [string]$Version = '', [string]$BundlePath = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Version) {
    $pubspec = Get-Content -LiteralPath (Join-Path $projectRoot 'apps/windows_client/pubspec.yaml') -Raw -Encoding UTF8
    if ($pubspec -notmatch '(?m)^version:\s*([0-9]+\.[0-9]+\.[0-9]+)\+') { throw 'Invalid client version.' }
    $Version = $Matches[1]
}
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw 'Invalid client version.' }
$clientVersion = $Version
$bundle = if ($BundlePath) { if ([System.IO.Path]::IsPathRooted($BundlePath)) { $BundlePath } else { Join-Path $projectRoot $BundlePath } } else { Join-Path $projectRoot 'apps/windows_client/build/windows/x64/runner/Release' }
if (-not (Test-Path -LiteralPath (Join-Path $bundle 'Multitap.exe'))) { throw 'Run Build-Client.cmd first.' }
$redistRoot = Join-Path $projectRoot '.tools/visualstudio-buildtools/VC/Redist/MSVC'
$crt = Get-ChildItem -LiteralPath $redistRoot -Directory | Sort-Object Name -Descending | ForEach-Object {
    $candidate = Join-Path $_.FullName 'x64/Microsoft.VC143.CRT'
    if (Test-Path -LiteralPath $candidate) { Get-Item -LiteralPath $candidate }
} | Select-Object -First 1
if ($null -eq $crt) { throw 'Bundled Microsoft VC runtime was not found.' }
Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' | Copy-Item -Destination $bundle -Force
$installRoot = Join-Path $env:LOCALAPPDATA 'Multitap/Client'
$installDirectory = Join-Path $installRoot $clientVersion
$resolvedRoot = [System.IO.Path]::GetFullPath($installRoot).TrimEnd('\') + '\'
$resolvedInstall = [System.IO.Path]::GetFullPath($installDirectory)
if (-not $resolvedInstall.StartsWith($resolvedRoot, [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected installation path.' }
$markerPath = Join-Path $installDirectory 'multitap-client.json'
if ((Test-Path -LiteralPath $installDirectory) -and (-not (Test-Path -LiteralPath $markerPath))) { throw 'Unrecognized installation directory.' }
New-Item -ItemType Directory -Force -Path $installDirectory | Out-Null
Get-ChildItem -LiteralPath $bundle | Copy-Item -Destination $installDirectory -Recurse -Force
@{ product='Multitap'; version=$clientVersion; mode='cloud-auth'; installed=(Get-Date).ToString('o') } | ConvertTo-Json | Set-Content -LiteralPath $markerPath -Encoding UTF8
$shell = New-Object -ComObject WScript.Shell
$shortcutPath = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Multitap.lnk'
$shortcut = $shell.CreateShortcut($shortcutPath)
if ((Test-Path -LiteralPath $shortcutPath) -and $shortcut.TargetPath -and (-not $shortcut.TargetPath.StartsWith($resolvedRoot, [System.StringComparison]::OrdinalIgnoreCase))) { throw 'Another app owns this shortcut.' }
$shortcut.TargetPath = Join-Path $installDirectory 'Multitap.exe'
$shortcut.WorkingDirectory = $installDirectory
$shortcut.Description = 'Multitap - cloud sign-in and assigned desktops'
$shortcut.IconLocation = "$($shortcut.TargetPath),0"
$shortcut.Save()
Write-Output "Installed: $($shortcut.TargetPath)"
Write-Output "Shortcut: $shortcutPath"
if ($Launch) { $app = Start-Process -FilePath $shortcut.TargetPath -WorkingDirectory $installDirectory -PassThru; Write-Output "Launched Multitap process: $($app.Id)" }

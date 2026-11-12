<#
.SYNOPSIS
Copies verified Microsoft x64 release VC runtime DLLs into a local build bundle.
.DESCRIPTION
Uses a prepared official VC143.CRT directory; never installs a global runtime.
Debug/nonredistributable runtimes and untrusted/incorrect-architecture files are
rejected before copying. Loading the resulting application is a separate check.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Bundle,
    [string]$RuntimeDirectory,
    [version]$MinimumVersion = '14.44.35207.0'
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$bundleItem=Get-Item -LiteralPath $Bundle -Force
if(-not $bundleItem.PSIsContainer -or ($bundleItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'A plain existing build bundle is required.' }
$Bundle=$bundleItem.FullName
if(-not $RuntimeDirectory) {
    $candidates=@()
    $standard=Join-Path $projectRoot '.tools/visualstudio-buildtools/VC/Redist/MSVC'
    if(Test-Path -LiteralPath $standard -PathType Container) {
        $candidates+=@(Get-ChildItem -LiteralPath $standard -Directory | ForEach-Object { Join-Path $_.FullName 'x64/Microsoft.VC143.CRT' })
    }
    $portable=Join-Path $projectRoot '.tools/msvc-portable'
    if(Test-Path -LiteralPath $portable -PathType Container) {
        foreach($prepared in @(Get-ChildItem -LiteralPath $portable -Directory -Filter 'redist-*')) {
            $versions=Join-Path $prepared.FullName 'Contents/VC/Redist/MSVC'
            if(Test-Path -LiteralPath $versions -PathType Container) {
                $candidates+=@(Get-ChildItem -LiteralPath $versions -Directory | ForEach-Object { Join-Path $_.FullName 'x64/Microsoft.VC143.CRT' })
            }
        }
    }
    $RuntimeDirectory=$candidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'vcruntime140.dll') -PathType Leaf } |
        Sort-Object { [version](Get-Item -LiteralPath (Join-Path $_ 'vcruntime140.dll')).VersionInfo.FileVersion } -Descending | Select-Object -First 1
    if(-not $RuntimeDirectory) { throw 'Prepare an official x64 Microsoft.VC143.CRT directory or pass -RuntimeDirectory.' }
}
$runtimeItem=Get-Item -LiteralPath $RuntimeDirectory -Force
if(-not $runtimeItem.PSIsContainer -or ($runtimeItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
   $runtimeItem.Name -cne 'Microsoft.VC143.CRT' -or $runtimeItem.FullName -match '(?i)[\\/]debug_nonredist[\\/]') {
    throw 'Only the plain x64 release Microsoft.VC143.CRT directory is accepted.'
}
$allowed=@('concrt140.dll','msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll','msvcp140_atomic_wait.dll','msvcp140_codecvt_ids.dll','vccorlib140.dll','vcruntime140.dll','vcruntime140_1.dll','vcruntime140_threads.dll')
$required=@('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')
$validated=@(Get-ChildItem -LiteralPath $runtimeItem.FullName -File -Filter '*.dll' | ForEach-Object {
    if($_.Name -cnotin $allowed -or ($_.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unexpected runtime file.' }
    $bytes=[IO.File]::ReadAllBytes($_.FullName)
    if($bytes.Length -lt 64 -or $bytes[0] -ne 77 -or $bytes[1] -ne 90) { throw 'Invalid runtime executable header.' }
    $pe=[BitConverter]::ToInt32($bytes,60)
    if($pe -lt 64 -or $pe -gt $bytes.Length-24 -or [BitConverter]::ToUInt32($bytes,$pe) -ne 17744 -or [BitConverter]::ToUInt16($bytes,$pe+4) -ne 34404) { throw 'The VC runtime must be x64.' }
    $signature=Get-AuthenticodeSignature -LiteralPath $_.FullName
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|, )CN=Microsoft (Windows( Software Compatibility Publisher)?|Corporation)(,|$)' -or
       $signature.SignerCertificate.Subject -notmatch '(^|, )O=Microsoft Corporation(,|$)') { throw 'Microsoft runtime signature verification failed.' }
    $version=[version]$_.VersionInfo.FileVersion
    if($version -lt $MinimumVersion) { throw 'VC runtime is older than the required toolset runtime.' }
    [pscustomobject]@{name=$_.Name;source=$_.FullName;version=$version.ToString();sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
foreach($name in $required) { if($name -cnotin $validated.name) { throw 'Required VC runtime file missing.' } }
foreach($entry in $validated) {
    $target=Join-Path $Bundle $entry.name
    if(Test-Path -LiteralPath $target) {
        $existing=Get-Item -LiteralPath $target -Force
        if($existing.PSIsContainer -or ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Runtime destination must be a plain file.' }
    }
}
foreach($entry in $validated) {
    $target=Join-Path $Bundle $entry.name
    Copy-Item -LiteralPath $entry.source -Destination $target -Force
    if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry.sha256) { throw 'Copied runtime differs from its verified source.' }
}
[pscustomobject]@{kind='verified-app-local-vc-runtime';globalInstallation=$false;executed=$false;minimumVersion=$MinimumVersion.ToString();files=@($validated | Select-Object name,version,sha256)}

# Build receipt (<exe>.json) of an MSI-based Host installer, the same shape as
# the Windows client's: payloadVerified, filename, size, sha256, version,
# product and the payload inventory (path, size, sha256). The GitHub release
# bundle (tools/release/github-release-bundle.mjs) and the image's download
# bundle require it next to every installer.
#
#   tools\release\host-installer-receipt.ps1 -Build .artifacts\host-installer\<ver>-<stamp> -Payload .artifacts\h<NN>-payload
#
# Read-only on the build: it writes only <exe>.json next to the installer
# (refuses to overwrite one) and never rebuilds or changes the installer.
# Checks before writing anything:
#   1. the installer's bytes match the build record (build.json exeSha256/exeBytes);
#   2. package.msi matches the build record (packageSha256) and is embedded
#      byte for byte in the installer;
#   3. the MSI's File table names exactly the payload's files, with the same sizes;
#   4. the payload's multitap-host.json carries the same version.
param(
  [Parameter(Mandatory)][string]$Build,
  [Parameter(Mandatory)][string]$Payload,
  [string]$Out
)
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path $Build).Path; $Payload = (Resolve-Path $Payload).Path
$record = Get-Content -Raw -Encoding UTF8 (Join-Path $Build 'build.json') | ConvertFrom-Json
$version = [string]$record.version
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw "build.json version is not x.y.z: $version" }
$exeName = "Multitap-Host-$version-windows-x64.exe"
$exe = Join-Path $Build $exeName; $msi = Join-Path $Build 'package.msi'
if (-not $Out) { $Out = "$exe.json" }
if (Test-Path -LiteralPath $Out) { throw "receipt already exists, not overwritten: $Out" }
function Sha([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }

# 1. Installer = build record.
$exeInfo = Get-Item -LiteralPath $exe; $exeSha = Sha $exe
if ($exeInfo.Length -ne [int64]$record.exeBytes -or $exeSha -ne ([string]$record.exeSha256).ToLowerInvariant()) { throw "$exeName does not match build.json" }

# 2. package.msi = build record, embedded in the installer.
if ((Sha $msi) -ne ([string]$record.packageSha256).ToLowerInvariant()) { throw 'package.msi does not match build.json packageSha256' }
$latin = [Text.Encoding]::GetEncoding(28591)
$exeText = $latin.GetString([IO.File]::ReadAllBytes($exe)); $msiText = $latin.GetString([IO.File]::ReadAllBytes($msi))
$head = $msiText.Substring(0, [Math]::Min(65536, $msiText.Length)); $at = $exeText.IndexOf($head, [StringComparison]::Ordinal)
if ($at -lt 0 -or $at + $msiText.Length -gt $exeText.Length -or -not [string]::Equals($exeText.Substring($at, $msiText.Length), $msiText, [StringComparison]::Ordinal)) { throw 'package.msi is not embedded byte for byte in the installer' }
if ($exeText.IndexOf($head, $at + 1, [StringComparison]::Ordinal) -ge 0) { throw 'package.msi start appears twice in the installer' }
Remove-Variable exeText, msiText, head

# 3. MSI File table = payload files (long name and size; the MSI has no paths in this table).
$tsv = [IO.Path]::Combine([IO.Path]::GetTempPath(), 'multitap-host-receipt-' + [guid]::NewGuid().ToString('N') + '.tsv')
try {
  & cscript.exe //nologo (Join-Path $PSScriptRoot '..\host-installer\msi-dump.vbs') $msi $tsv 'SELECT `FileName`,`FileSize` FROM `File`'
  if ($LASTEXITCODE) { throw 'reading the MSI File table failed' }
  $inMsi = @(Get-Content -LiteralPath $tsv -Encoding Unicode | Where-Object { $_ } | ForEach-Object { $name, $size = $_ -split "`t"; (($name -split '\|')[-1]).ToLowerInvariant() + "`t" + $size })
} finally { Remove-Item -LiteralPath $tsv -ErrorAction SilentlyContinue }
if (-not $inMsi.Count) { throw 'the MSI File table is empty' }
$files = @(Get-ChildItem -LiteralPath $Payload -Recurse -File | Sort-Object FullName)
$inPayload = $files | ForEach-Object { $_.Name.ToLowerInvariant() + "`t" + $_.Length }
$missing = @(Compare-Object ($inMsi | Sort-Object) ($inPayload | Sort-Object))
if ($missing.Count) { throw ('MSI and payload differ: ' + (($missing | Select-Object -First 10 | ForEach-Object { "$($_.SideIndicator) $($_.InputObject -replace "`t", ' ')" }) -join '; ')) }

# 4. Payload version.
$manifest = Get-Content -Raw -Encoding UTF8 (Join-Path $Payload 'multitap-host.json') | ConvertFrom-Json
if ([string]$manifest.version -ne $version) { throw "payload multitap-host.json is $($manifest.version), installer is $version" }

$inventory = foreach ($file in $files) {
  [ordered]@{ path = $file.FullName.Substring($Payload.Length).TrimStart('\').Replace('\', '/'); sha256 = Sha $file.FullName; size = $file.Length }
}
$receipt = [ordered]@{ payloadVerified = $true; product = 'host'; filename = $exeName; size = $exeInfo.Length; version = $version; sha256 = $exeSha; files = @($inventory) }
$json = $receipt | ConvertTo-Json -Depth 5
$stream = [IO.File]::Open($Out, [IO.FileMode]::CreateNew)
try { $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json + "`n"); $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
"receipt: $Out ($($files.Count) files)"

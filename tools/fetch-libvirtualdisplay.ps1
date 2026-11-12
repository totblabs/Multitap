param([string]$Destination, [switch]$VerifyOnly, [string]$SdkPath, [string]$DriverPath)
# Downloads the pinned libvirtualdisplay inputs of the native Host build
# (apps/windows_host/third_party/libvirtualdisplay/upstream.json) into .tools/libvirtualdisplay:
#   libvirtualdisplay-<commit>/                       the SDK source tree  -> tools/host-installer/build-native.ps1
#   libvirtualdisplay-1.6.5-windows-x64/driver/       the original signed driver, as the Host package carries it (bin/native-display-driver)
# Checks the archive SHA-256 values, the source tree digest, every extracted file hash and the
# driver signer thumbprint. Developer tool: nothing is installed, trusted or run.
#
# -VerifyOnly downloads and extracts nothing: it checks folders that already exist (the layout
# above under -Destination, or any -SdkPath / -DriverPath given) against the same pin and returns
# [pscustomobject]@{ Sdk; Driver }. A driver folder may also hold the upstream
# SunshineVirtualDisplayDriver.pdb, which the Host package leaves out.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$projectRoot = Split-Path -Parent $PSScriptRoot
$pin = Get-Content -LiteralPath (Join-Path $projectRoot 'apps/windows_host/third_party/libvirtualdisplay/upstream.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $Destination) { $Destination = Join-Path $projectRoot '.tools/libvirtualdisplay' }
$Destination = [IO.Path]::GetFullPath($Destination)
$downloads = Join-Path $Destination 'downloads'
if (-not $VerifyOnly) { New-Item -ItemType Directory -Path $downloads -Force | Out-Null }
elseif (-not $SdkPath -and -not $DriverPath -and -not (Test-Path -LiteralPath $Destination -PathType Container)) { throw "$Destination does not exist; run tools/fetch-libvirtualdisplay.ps1 first." }

# .NET file APIs: the source tree is deep, and Get-FileHash/Resolve-Path fail beyond MAX_PATH.
function Get-Sha256([string]$Path) {
  $stream = [IO.File]::OpenRead($Path); $sha = [Security.Cryptography.SHA256]::Create()
  try { return -join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('x2') }) } finally { $sha.Dispose(); $stream.Dispose() }
}
function Test-Archive([string]$Path, $Archive) { (Test-Path -LiteralPath $Path -PathType Leaf) -and (Get-Item -LiteralPath $Path).Length -eq $Archive.size -and (Get-Sha256 $Path) -eq $Archive.sha256 }
function Get-Archive($Archive) {
  $path = Join-Path $downloads $Archive.name
  if (-not (Test-Archive $path $Archive)) {
    $part = $path + '.part'
    Invoke-WebRequest -Uri $Archive.url -OutFile $part -UseBasicParsing -TimeoutSec 300
    Move-Item -LiteralPath $part -Destination $path -Force
  }
  return $path
}
# Entry names must stay below the archive's single root folder.
function Get-RelativeEntry($Entry, [string]$Root) {
  $name = $Entry.FullName
  if (-not $name.StartsWith($Root + '/', [StringComparison]::Ordinal)) { throw "Unexpected archive entry outside $Root" }
  $relative = $name.Substring($Root.Length + 1)
  if ($relative -match '(^|/)\.\.?(/|$)|\\|:|^/') { throw "Unsafe archive entry: $name" }
  return $relative
}
# The tree digest of upstream.json: SHA-256 over '<sha256>  <path>\n' lines sorted by path.
function Get-TreeDigest([string]$Directory) {
  $base = [IO.Path]::GetFullPath($Directory).TrimEnd('\')
  $rows = @{}
  foreach ($directory in @($base) + @([IO.Directory]::EnumerateDirectories($base, '*', [IO.SearchOption]::AllDirectories))) {
    if ([IO.File]::GetAttributes($directory) -band [IO.FileAttributes]::ReparsePoint) { throw "Linked folder in the source tree: $directory" }
  }
  foreach ($file in [IO.Directory]::EnumerateFiles($base, '*', [IO.SearchOption]::AllDirectories)) {
    if ([IO.File]::GetAttributes($file) -band [IO.FileAttributes]::ReparsePoint) { throw "Linked file in the source tree: $file" }
    $rows[$file.Substring($base.Length + 1).Replace('\', '/')] = Get-Sha256 $file
  }
  $paths = [string[]]@($rows.Keys); [Array]::Sort($paths, [StringComparer]::Ordinal)
  $text = -join ($paths | ForEach-Object { $rows[$_] + '  ' + $_ + "`n" })
  $sha = [Security.Cryptography.SHA256]::Create()
  try { $digest = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)) | ForEach-Object { $_.ToString('x2') }) } finally { $sha.Dispose() }
  return [pscustomobject]@{ Files = $paths.Count; Digest = $digest }
}
# Extracts the selected entries into a fresh staging folder, then moves it into place.
function Expand-Pinned([string]$ArchivePath, [string]$Root, [string]$Target, [scriptblock]$Select) {
  $staging = Join-Path $Destination ('.staging-' + [guid]::NewGuid().ToString('N'))
  $zip = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
  try {
    $selected = @(foreach ($entry in $zip.Entries) {
      if ($entry.FullName.EndsWith('/')) { continue }
      $relative = Get-RelativeEntry $entry $Root
      if (-not (& $Select $relative)) { continue }
      $final = Join-Path $Target ($relative.Replace('/', '\'))
      if ($final.Length -ge 260) { throw "$final is $($final.Length) characters, beyond the Windows path limit: choose a shorter -Destination." }
      [pscustomobject]@{ Entry = $entry; Relative = $relative }
    })
    try {
      foreach ($item in $selected) {
        $out = Join-Path $staging ($item.Relative.Replace('/', '\'))
        New-Item -ItemType Directory -Path (Split-Path -Parent $out) -Force | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($item.Entry, $out, $false)
      }
    } catch { Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue; throw }
  } finally { $zip.Dispose() }
  Move-Item -LiteralPath $staging -Destination $Target
}
# The SDK tree digest and the licence (the fetched copy and the tracked one) against the pin.
function Assert-PinnedSource([string]$Directory) {
  if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { throw "$Directory does not exist." }
  $tree = Get-TreeDigest $Directory
  if ($tree.Files -ne $pin.sourceArchive.files -or $tree.Digest -ne $pin.sourceArchive.treeSha256) { throw "$Directory does not match the pinned libvirtualdisplay source ($($pin.sourceCommit)); remove the folder and run this script again." }
  if ((Get-Sha256 (Join-Path $Directory 'LICENSE')) -ne $pin.licenseFileSha256 -or (Get-Sha256 (Join-Path $projectRoot $pin.licenseFile)) -ne $pin.licenseFileSha256) { throw 'The libvirtualdisplay licence differs from the pinned text.' }
}
# The four driver files byte for byte, nothing else but the upstream .pdb, and the pinned signer.
function Assert-PinnedDriver([string]$Directory) {
  if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { throw "$Directory does not exist." }
  $files = @{}; foreach ($p in $pin.releaseArchive.extracted.PSObject.Properties) { if ($p.Name.StartsWith('driver/')) { $files[$p.Name.Substring(7)] = $p.Value } }
  foreach ($item in @(Get-ChildItem -LiteralPath $Directory -Force)) {
    if ($item.PSIsContainer -or -not ($files.ContainsKey($item.Name) -or $item.Name -ceq 'SunshineVirtualDisplayDriver.pdb')) { throw "$Directory holds $($item.Name), which is not a pinned driver file." }
  }
  foreach ($name in $files.Keys) {
    $path = Join-Path $Directory $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Sha256 $path) -ne $files[$name]) { throw "$name in $Directory does not match upstream.json." }
  }
  $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new((Join-Path $Directory 'SunshineVirtualDisplayDriver.cer'))
  $catalog = Get-AuthenticodeSignature -LiteralPath (Join-Path $Directory 'SunshineVirtualDisplayDriver.cat')
  if ($certificate.Thumbprint -cne $pin.releaseArchive.signing.thumbprint -or -not $catalog.SignerCertificate -or $catalog.SignerCertificate.Thumbprint -cne $pin.releaseArchive.signing.thumbprint) { throw 'The driver signer is not the pinned upstream certificate.' }
}

if ($VerifyOnly) {
  if (-not $SdkPath) { $SdkPath = Join-Path $Destination $pin.sourceArchive.root }
  if (-not $DriverPath) { $DriverPath = Join-Path (Join-Path $Destination $pin.releaseArchive.root) 'driver' }
  $SdkPath = [IO.Path]::GetFullPath($SdkPath); $DriverPath = [IO.Path]::GetFullPath($DriverPath)
  Assert-PinnedSource $SdkPath
  Assert-PinnedDriver $DriverPath
  return [pscustomobject]@{ Sdk = $SdkPath; Driver = $DriverPath }
}

# --- SDK source -------------------------------------------------------------------------
$src = $pin.sourceArchive
$sourceDir = Join-Path $Destination $src.root
if (-not (Test-Path -LiteralPath $sourceDir)) {
  $archive = Get-Archive $src
  if (-not (Test-Archive $archive $src)) {
    # GitHub creates source archives on request; the tree digest still identifies the source.
    Write-Warning ($src.name + ' differs from the pinned archive bytes; accepting it only if the extracted tree matches the pinned digest.')
  }
  Expand-Pinned $archive $src.root $sourceDir { param($p) $true }
}
Assert-PinnedSource $sourceDir

# --- signed driver package --------------------------------------------------------------
$rel = $pin.releaseArchive
$releaseDir = Join-Path $Destination $rel.root
$expected = @{}; foreach ($p in $rel.extracted.PSObject.Properties) { $expected[$p.Name] = $p.Value }
if (-not (Test-Path -LiteralPath $releaseDir)) {
  $archive = Get-Archive $rel
  if (-not (Test-Archive $archive $rel)) { Remove-Item -LiteralPath $archive -Force; throw ($rel.name + ' does not match the pinned SHA-256/size.') }
  Expand-Pinned $archive $rel.root $releaseDir { param($p) $expected.ContainsKey($p) }
}
$present = @(Get-ChildItem -LiteralPath $releaseDir -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($releaseDir.Length + 1).Replace('\', '/') })
if ($present.Count -ne $expected.Count) { throw "$releaseDir holds other files than the pinned ones; remove the folder and run this script again." }
foreach ($name in $expected.Keys) {
  $path = Join-Path $releaseDir ($name.Replace('/', '\'))
  if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Sha256 $path) -ne $expected[$name]) { throw "$name does not match upstream.json; remove $releaseDir and run this script again." }
}
$driverDir = Join-Path $releaseDir 'driver'
Assert-PinnedDriver $driverDir

Write-Host ('libvirtualdisplay ' + $pin.version + ' (' + $pin.sourceCommit.Substring(0, 8) + ') verified in ' + $Destination)
Write-Host ('  SDK source: ' + $sourceDir)
Write-Host ('  driver:     ' + $driverDir)

# Makes a Host MSI for a new version from the previous authored one: replaces the
# payload files that changed, updates File sizes/versions, rebuilds the cabinet,
# and moves ProductVersion, ProductCode, the Upgrade ranges and the version folder.
#   powershell -ExecutionPolicy Bypass -File tools\host-installer\reversion-msi.ps1 `
#     -BaseMsi <previous product.msi> -Version <new version> -Payload <dir laid out like Host\<version>\> -Out <new.msi>
# The payload must contain exactly the base package's files (same relative paths; a
# path that contains the old version, e.g. the corresponding-source zip, uses the new one).
param(
  [Parameter(Mandatory)][string]$BaseMsi,
  [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
  [Parameter(Mandatory)][string]$Payload,
  [Parameter(Mandatory)][string]$Out,
  # The cabinet the base package was authored with (its payload.cab stream).
  [Parameter(Mandatory)][string]$BaseCab
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$BaseMsi = (Resolve-Path $BaseMsi).Path; $Payload = (Resolve-Path $Payload).Path
$Out = [IO.Path]::GetFullPath($Out)
$work = $Out + '.work'; if (Test-Path $work) { Remove-Item $work -Recurse -Force }; New-Item -ItemType Directory $work | Out-Null
Copy-Item $BaseMsi $Out -Force

function Query([string]$sql) {
  $f = Join-Path $work ('q' + [guid]::NewGuid().ToString('N') + '.tsv')
  & cscript //nologo (Join-Path $here 'msi-dump.vbs') $Out $f $sql | Out-Null
  if ($LASTEXITCODE) { throw "query failed: $sql" }
  @(Get-Content -LiteralPath $f -Encoding Unicode | Where-Object { $_ -ne '' } | ForEach-Object { , ($_ -split "`t") })
}
function LongName([string]$v) { if ($v.Contains('|')) { $v.Split('|')[1] } else { $v } }
# RFC 4122 name-based (SHA-1) GUID in the package family's namespace (the UpgradeCode).
function NameGuid([string]$name, [string]$namespace) {
  $ns = ([guid]$namespace).ToByteArray()
  [Array]::Reverse($ns, 0, 4); [Array]::Reverse($ns, 4, 2); [Array]::Reverse($ns, 6, 2)
  $sha = [Security.Cryptography.SHA1]::Create().ComputeHash([byte[]]($ns + [Text.Encoding]::UTF8.GetBytes($name)))
  $b = $sha[0..15]; $b[6] = ($b[6] -band 0x0f) -bor 0x50; $b[8] = ($b[8] -band 0x3f) -bor 0x80
  [Array]::Reverse($b, 0, 4); [Array]::Reverse($b, 4, 2); [Array]::Reverse($b, 6, 2)
  '{' + ([guid][byte[]]$b).ToString().ToUpperInvariant() + '}'
}

$props = @{}; foreach ($r in (Query 'SELECT Property, Value FROM Property')) { $props[$r[0]] = $r[1] }
$old = $props['ProductVersion']; $upgrade = $props['UpgradeCode']
if ($old -eq $Version) { throw "base is already $Version" }
if ((NameGuid "Product/$old/x64" $upgrade) -ne $props['ProductCode']) { throw 'ProductCode rule does not reproduce the base package' }
$productCode = NameGuid "Product/$Version/x64" $upgrade

# Directory tree -> path of every file relative to the version folder.
$dirs = @{}; foreach ($r in (Query 'SELECT Directory, Directory_Parent, DefaultDir FROM Directory')) { $dirs[$r[0]] = @{ parent = $r[1]; name = (LongName $r[2]); raw = $r[2] } }
# The install folder: a Directory row with DefaultDir "." under Host (= Host itself), so every version
# installs to Host\bin and overwrites in place; a base package may instead have a Host\<version> row.
$versionDir = @($dirs.Keys | Where-Object { $dirs[$_].name -eq $old })
if ($versionDir.Count -eq 0) { $versionDir = @($dirs.Keys | Where-Object { $dirs[$_].raw -eq '.' -and $dirs[$_].parent -and $dirs[$dirs[$_].parent].name -eq 'Host' }) }
if ($versionDir.Count -ne 1) { throw 'install folder row not found' }; $versionDir = $versionDir[0]
function RelDir([string]$d) {
  $parts = @(); while ($d -and $d -ne $versionDir) { $n = $dirs[$d].name; if ($n -ne '.') { $parts = , $n + $parts }; $d = $dirs[$d].parent; if (-not $d) { throw 'file outside the version folder' } }
  $parts -join '\'
}
$compDir = @{}; foreach ($r in (Query 'SELECT Component, Directory_ FROM Component')) { $compDir[$r[0]] = $r[1] }
$files = Query 'SELECT File, Component_, FileName, FileSize, Version, Sequence FROM File ORDER BY Sequence'

# Base cabinet -> one file per File key.
$cab = (Resolve-Path $BaseCab).Path; $extract = Join-Path $work 'files'; New-Item -ItemType Directory $extract | Out-Null
& expand.exe -F:* $cab $extract | Out-Null; if ($LASTEXITCODE) { throw 'expand failed' }
if (@(Get-ChildItem $extract -File).Count -ne $files.Count) { throw 'base cabinet does not hold exactly the package files' }

# Payload files whose name changed since an earlier release: path in the base package -> file name now.
# A base package that still has the old name gets that File row renamed; its key, component and short
# name stay, as they do for the source zip whose name carries the version.
$renamed = @{ 'bin\data\flutter_assets\assets\licenses\GPL-3.0.txt' = 'AGPL-3.0.txt' }

$sql = [Collections.Generic.List[string]]::new(); $seen = @{}; $changed = 0
foreach ($f in $files) {
  $key, $comp, $fileName, $size, $ver = $f[0], $f[1], $f[2], $f[3], $f[4]
  $long = LongName $fileName; $dir = RelDir $compDir[$comp]; $rel = (@($dir, $long) | Where-Object { $_ }) -join '\'
  $newRel = $rel.Replace($old, $Version); $src = Join-Path $Payload $newRel; $newName = $fileName.Replace($old, $Version)
  if (-not (Test-Path -LiteralPath $src -PathType Leaf) -and $renamed.ContainsKey($rel) -and $fileName.Contains('|')) {
    $newRel = (@($dir, $renamed[$rel]) | Where-Object { $_ }) -join '\'; $src = Join-Path $Payload $newRel
    $newName = $fileName.Split('|')[0] + '|' + $renamed[$rel]
  }
  if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "payload missing: $newRel" }
  $seen[$newRel.ToLowerInvariant()] = $true
  $base = Join-Path $extract $key; if (-not (Test-Path -LiteralPath $base)) { throw "cab missing $key" }
  $set = @()
  if ($newName -ne $fileName) { $set += "``FileName``='$($newName.Replace("'", "''"))'" }
  if ((Get-FileHash -LiteralPath $src).Hash -ne (Get-FileHash -LiteralPath $base).Hash) {
    Copy-Item -LiteralPath $src -Destination $base -Force; $changed++
    $set += "``FileSize``=$((Get-Item -LiteralPath $src).Length)"
    if ($ver) { $vi = (Get-Item -LiteralPath $src).VersionInfo; $set += "``Version``='$($vi.FileMajorPart).$($vi.FileMinorPart).$($vi.FileBuildPart).$($vi.FilePrivatePart)'" }
  }
  if ($set.Count) { $sql.Add("UPDATE ``File`` SET $($set -join ', ') WHERE ``File``='$key'") }
}
$extra = @(Get-ChildItem -LiteralPath $Payload -Recurse -File | ForEach-Object { $_.FullName.Substring($Payload.Length + 1) } | Where-Object { -not $seen[$_.ToLowerInvariant()] })
if ($extra.Count) { throw "payload has files the package does not: $($extra -join ', ')" }

# New cabinet in Sequence order (file names = File keys, as the Media table expects).
$ddf = Join-Path $work 'payload.ddf'; $newCab = Join-Path $work 'payload.cab'
$lines = @('.OPTION EXPLICIT', '.Set CabinetNameTemplate=payload.cab', ".Set DiskDirectoryTemplate=$work", '.Set Cabinet=on', '.Set Compress=on', '.Set CompressionType=MSZIP',
  '.Set MaxDiskSize=0', '.Set CabinetFileCountThreshold=0', '.Set FolderFileCountThreshold=0', '.Set FolderSizeThreshold=0', '.Set MaxCabinetSize=0', '.Set UniqueFiles=on',
  ".Set RptFileName=$work\setup.rpt", ".Set InfFileName=$work\setup.inf")
foreach ($f in $files) { $lines += "`"$(Join-Path $extract $f[0])`" $($f[0])" }
[IO.File]::WriteAllLines($ddf, $lines, [Text.Encoding]::ASCII)
& makecab.exe /F $ddf | Out-Null; if ($LASTEXITCODE -or -not (Test-Path $newCab)) { throw 'makecab failed' }

$sql.Add("UPDATE ``Property`` SET ``Value``='$Version' WHERE ``Property``='ProductVersion'")
$sql.Add("UPDATE ``Property`` SET ``Value``='$productCode' WHERE ``Property``='ProductCode'")
# VersionMin/VersionMax are part of the Upgrade primary key: replace the rows.
foreach ($u in (Query 'SELECT UpgradeCode, VersionMin, VersionMax, Language, Attributes, Remove, ActionProperty FROM Upgrade')) {
  $min, $max = $u[1], $u[2]
  if ($u[6] -eq 'MT_UPGRADE_PRODUCTS') { $max = $Version } elseif ($u[6] -eq 'MT_NEWER_PRODUCTS') { $min = $Version } else { continue }
  $q = { param($s) if ($s -eq '') { 'NULL' } else { "'" + $s.Replace("'", "''") + "'" } }
  $sql.Add("DELETE FROM ``Upgrade`` WHERE ``ActionProperty``='$($u[6])'")
  $sql.Add("INSERT INTO ``Upgrade`` (``UpgradeCode``, ``VersionMin``, ``VersionMax``, ``Language``, ``Attributes``, ``Remove``, ``ActionProperty``) VALUES ($(& $q $u[0]), $(& $q $min), $(& $q $max), $(& $q $u[3]), $($u[4]), $(& $q $u[5]), '$($u[6])')")
}
$sql.Add("UPDATE ``Directory`` SET ``DefaultDir``='.' WHERE ``Directory``='$versionDir'")
$opsText = ($sql | ForEach-Object { "SQL`t$_" }) + "STREAM`tpayload.cab`t$newCab"
[IO.File]::WriteAllText((Join-Path $work 'ops.txt'), (($opsText -join "`r`n") + "`r`n"), [Text.Encoding]::Unicode)
$r = & cscript //nologo (Join-Path $here 'msi-apply.vbs') $Out (Join-Path $work 'ops.txt'); if ($LASTEXITCODE -or ($r | Select-Object -Last 1) -ne 'ok') { throw "apply failed: $r" }

# Anything else still naming the old version is reported, not guessed at.
$left = @(); foreach ($t in 'Property', 'Registry', 'Shortcut', 'ServiceInstall', 'Upgrade', 'Directory', 'LaunchCondition') {
  foreach ($row in (Query "SELECT * FROM ``$t``")) { if (($row -join "`t").Contains($old)) { $left += "$t`: $($row -join ' | ')" } }
}
[ordered]@{ version = $Version; from = $old; productCode = $productCode; changedFiles = $changed; files = $files.Count; out = $Out; sha256 = (Get-FileHash $Out).Hash.ToLowerInvariant(); oldVersionLeft = $left } | ConvertTo-Json -Depth 4

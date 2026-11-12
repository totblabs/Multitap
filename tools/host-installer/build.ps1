# Builds Multitap-Host-<version>-windows-x64.exe from an authored Host MSI.
#   powershell -ExecutionPolicy Bypass -File tools\host-installer\build.ps1 -Msi <authored product.msi> -Version <version>
# Output: .artifacts\host-installer\<version>-<time>\ (package.msi, the EXE, build.json)
param(
  [Parameter(Mandatory)][string]$Msi,
  [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$src = Join-Path $root 'apps\windows_host\installer'
$out = Join-Path $root ('.artifacts\host-installer\' + $Version + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $out | Out-Null

# 1. Plain MSI: copy, simplify, check.
$package = Join-Path $out 'package.msi'
Copy-Item -LiteralPath $Msi -Destination $package
$packageCode = '{' + [guid]::NewGuid().ToString().ToUpperInvariant() + '}'
$r = & cscript //nologo (Join-Path $PSScriptRoot 'simplify-msi.vbs') $package $packageCode (Join-Path $root 'apps\windows_host\resources\host.ico')
if ($LASTEXITCODE -ne 0 -or ($r | Select-Object -Last 1) -ne 'ok') { throw "simplify-msi failed: $r" }
$productVersion = (($r | Where-Object { $_ -like 'version=*' }) -replace '^version=', '')
if ($productVersion -ne $Version) { throw "MSI ProductVersion $productVersion differs from -Version $Version" }

# 2. Resources and compile-time version.
$parts = $Version.Split('.')
$esc = { param($p) $p.Replace('\', '\\') }
$rc = (Get-Content -LiteralPath (Join-Path $src 'host_installer.rc') -Raw -Encoding UTF8).
  Replace('"..\\resources\\host_setup.ico"', '"' + (& $esc (Join-Path $root 'apps\windows_host\resources\host_setup.ico')) + '"').
  Replace('MULTITAP_PACKAGE_PATH', '"' + (& $esc $package) + '"').
  Replace('MULTITAP_VERSION_COMMA', ($parts -join ',') + ',0').
  Replace('MULTITAP_VERSION_TEXT', '"' + $Version + '"')
[IO.File]::WriteAllText((Join-Path $out 'host_installer.rc'), $rc, [Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $out 'version.h'), "#define MULTITAP_HOST_VERSION `"$Version`"`r`n", [Text.UTF8Encoding]::new($false))

# 3. Compile and link (static CRT: no runtime install needed).
. (Join-Path $root '.tools\msvc-portable\env.ps1')
Push-Location $out
try {
  & rc.exe /nologo /fo host_installer.res host_installer.rc
  if ($LASTEXITCODE -ne 0) { throw 'rc failed' }
  & cl.exe /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W4 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ("/FI" + (Join-Path $out 'version.h')) /c (Join-Path $src 'host_installer.cpp') /Fohost_installer.obj
  if ($LASTEXITCODE -ne 0) { throw 'cl failed' }
  $exe = "Multitap-Host-$Version-windows-x64.exe"
  # DLL planting: the two libraries that are not KnownDLLs are delay-loaded
  # (resolved only after wWinMain's SetDefaultDllDirectories), and the loader resolves the static imports
  # from System32 only (/DEPENDENTLOADFLAG:0x800 = LOAD_LIBRARY_SEARCH_SYSTEM32).
  & link.exe /nologo /SUBSYSTEM:WINDOWS /OUT:$exe host_installer.obj host_installer.res shell32.lib ole32.lib advapi32.lib user32.lib wtsapi32.lib userenv.lib `
    delayimp.lib /DELAYLOAD:wtsapi32.dll /DELAYLOAD:userenv.dll /DEPENDENTLOADFLAG:0x800 `
    /MANIFEST:EMBED /MANIFESTUAC:NO ("/MANIFESTINPUT:" + (Join-Path $src 'host_installer.manifest'))
  if ($LASTEXITCODE -ne 0) { throw 'link failed' }
} finally { Pop-Location }

$exePath = Join-Path $out $exe
$info = [ordered]@{
  version = $Version; packageCode = $packageCode
  sourceMsi = $Msi; sourceMsiSha256 = (Get-FileHash -LiteralPath $Msi).Hash.ToLowerInvariant()
  packageSha256 = (Get-FileHash -LiteralPath $package).Hash.ToLowerInvariant()
  exe = $exePath; exeBytes = (Get-Item $exePath).Length; exeSha256 = (Get-FileHash -LiteralPath $exePath).Hash.ToLowerInvariant()
  builtAt = (Get-Date).ToUniversalTime().ToString('o')
}
$info | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'build.json') -Encoding UTF8
$info | ConvertTo-Json

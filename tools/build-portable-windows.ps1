<#
.SYNOPSIS
Builds the real Flutter Windows client or Host settings with prepared local tools.
.DESCRIPTION
Uses official Flutter configuration/AOT assembly and the project's CMake runner
with portable MSVC/Ninja. No Visual Studio registration, SDK patch, installation,
GUI launch, signing or Windows setting change occurs. The raw Host bundle keeps
Multitap.exe; the existing Host packager renames it and sets its administrator
manifest. The verified official VC runtime is copied into each local bundle.
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-portable-windows.ps1 -Variant Client
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-portable-windows.ps1 -Variant HostSettings -Parallel 20
#>
[CmdletBinding()]
param(
    [ValidateSet('Client', 'HostSettings')][string]$Variant = 'Client',
    [string]$BuildName = '',
    [ValidateRange(0, 65535)][int]$BuildNumber = 0,
    [string]$ToolRoot = '',
    [string]$ArtifactRoot = '',
    [string]$RuntimeDirectory = '',
    [ValidateRange(1, 128)][int]$Parallel = [Math]::Min(12, [Environment]::ProcessorCount),
    [switch]$ResolveOnly,
    [switch]$Help
)
$ErrorActionPreference = 'Stop'
if ($Help) {
    Write-Output 'Build-Client.cmd --portable'
    Write-Output 'tools/build-portable-windows.ps1 [-Variant Client|HostSettings] [-BuildName x.y.z] [-BuildNumber N] [-Parallel N] [-ToolRoot path] [-ArtifactRoot path] [-RuntimeDirectory path] [-ResolveOnly]'
    Write-Output 'Defaults: Client pubspec version/build; Host CMake version, build 20000 + minor * 100 + patch. Raw bundles only; no install or launch.'
    return
}
$project = Split-Path -Parent $PSScriptRoot
$app = Join-Path $project 'apps/windows_client'
function Resolve-ProjectPath([string]$Path, [string]$Default) {
    if (-not $Path) { $Path = $Default }
    if (-not [IO.Path]::IsPathRooted($Path)) { $Path = Join-Path $project $Path }
    return [IO.Path]::GetFullPath($Path)
}
$ToolRoot = Resolve-ProjectPath $ToolRoot '.tools'
$ArtifactRoot = Resolve-ProjectPath $ArtifactRoot '.artifacts/portable-windows'
$pubspec = Get-Content -LiteralPath (Join-Path $app 'pubspec.yaml') -Raw -Encoding UTF8
$clientVersion = [regex]::Match($pubspec, '(?m)^version:\s*([0-9]+\.[0-9]+\.[0-9]+)\+([0-9]+)\s*$')
$hostSource = Get-Content -LiteralPath (Join-Path $project 'apps/windows_host/CMakeLists.txt') -Raw -Encoding UTF8
$hostVersion = [regex]::Match($hostSource, 'project\(MultitapHost VERSION ([0-9]+\.[0-9]+\.[0-9]+)\s')
if (-not $clientVersion.Success -or -not $hostVersion.Success) { throw 'Source component versions are missing.' }
if (-not $BuildName) { $BuildName = if ($Variant -eq 'Client') { $clientVersion.Groups[1].Value } else { $hostVersion.Groups[1].Value } }
if ($BuildName -notmatch '^[0-9]{1,5}\.[0-9]{1,5}\.[0-9]{1,5}$' -or @($BuildName.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count) { throw 'BuildName must be x.y.z with Windows version parts in 0..65535.' }
if (-not $BuildNumber) {
    # Host settings: 20000 + minor * 100 + patch, so the number keeps rising across 1.0.z (20000 + z) and 1.1.z (20100 + z).
    $BuildNumber = if ($Variant -eq 'Client') { [int]$clientVersion.Groups[2].Value } else { 20000 + [int]$BuildName.Split('.')[1] * 100 + [int]$BuildName.Split('.')[2] }
}
if ($BuildNumber -lt 1 -or $BuildNumber -gt 65535 -or ($Variant -eq 'Client' -and $BuildNumber -ge 20000) -or ($Variant -eq 'HostSettings' -and $BuildNumber -lt 20000)) { throw 'Client build numbers must be 1..19999; Host settings use the reserved 20000..65535 range.' }
$label = $Variant.ToLowerInvariant()
$versionLabel = "$BuildName+$BuildNumber"
$build = Join-Path $ArtifactRoot "build-$label"
$bundle = Join-Path $ArtifactRoot "bundle-$label/$versionLabel"
$logPrefix = Join-Path $ArtifactRoot "$label-$versionLabel"
$compilerEnvironment = Join-Path $ToolRoot 'msvc-portable/env.ps1'
$flutterEnvironment = Join-Path $ToolRoot 'flutter/env.ps1'
$flutter = Join-Path $ToolRoot 'flutter/bin/flutter.bat'
$cmake = Join-Path $ToolRoot 'cmake/cmake-3.31.8-windows-x86_64/bin/cmake.exe'
$ninja = Join-Path $ToolRoot 'ninja/1.13.2/ninja.exe'
if ($ResolveOnly) {
    [ordered]@{variant=$Variant;buildName=$BuildName;buildNumber=$BuildNumber;parallel=$Parallel;tools=$ToolRoot;build=$build;bundle=$bundle;built=$false;installed=$false} | ConvertTo-Json
    return
}

foreach ($required in @($compilerEnvironment, $flutterEnvironment, $flutter, $cmake, $ninja)) {
    if (-not [IO.File]::Exists($required)) { throw "Prepared portable input is missing: $required" }
}
function Invoke-LoggedNative([string]$Executable, [string[]]$Argument, [string]$Log, [switch]$AllowFailure) {
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @Argument *> $Log
        $nativeExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    $logText = [IO.File]::ReadAllText($Log)
    [IO.File]::WriteAllText($Log, $logText, [Text.UTF8Encoding]::new($false))
    if ($nativeExit -ne 0 -and -not $AllowFailure) { throw "Build step failed ($nativeExit). See $Log" }
    return $nativeExit
}
$environmentNames = @('PATH','INCLUDE','LIB','LIBPATH','VCToolsInstallDir','VCINSTALLDIR','VSCMD_ARG_HOST_ARCH','VSCMD_ARG_TGT_ARCH','VisualStudioVersion','WindowsSdkDir','WindowsSDKVersion','UCRTVersion','UniversalCRTSdkDir','PUB_CACHE','FLUTTER_SUPPRESS_ANALYTICS','DART_SUPPRESS_ANALYTICS','CMAKE_BUILD_PARALLEL_LEVEL','MULTITAP_WINDOWS_VARIANT')
$savedEnvironment = @{}
foreach ($environmentName in $environmentNames) { $savedEnvironment[$environmentName] = [Environment]::GetEnvironmentVariable($environmentName, 'Process') }
$pushed = $false
try {
    # The prepared compiler environment verifies its signed compiler component
    # hashes before exposing them to this process. No global tool setup runs.
    . $compilerEnvironment
    . $flutterEnvironment
    $env:CMAKE_BUILD_PARALLEL_LEVEL = [string]$Parallel
    $env:MULTITAP_WINDOWS_VARIANT = $Variant
    New-Item -ItemType Directory -Force -Path $ArtifactRoot | Out-Null
    Push-Location $app
    $pushed = $true
    $inputFiles = @('pubspec.yaml','windows/CMakeLists.txt','windows/flutter/CMakeLists.txt','windows/runner/CMakeLists.txt','windows/runner/main.cpp','windows/runner/Runner.rc','../../native/windows_admin/release_signature.hpp')
    $sourceHashes = @{}
    foreach ($inputFile in $inputFiles) { $sourceHashes[$inputFile] = (Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash.ToLowerInvariant() }
    # Official Flutter writes generated_config.cmake before looking up a
    # registered VS installation. Preserve that diagnostic; do not fabricate VS.
    $configLog = "$logPrefix-flutter-config.log"
    $configExit = Invoke-LoggedNative $flutter @('build','windows','--release','--no-pub','--config-only',"--build-name=$BuildName","--build-number=$BuildNumber") $configLog -AllowFailure
    foreach ($inputFile in $inputFiles) { if ((Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sourceHashes[$inputFile]) { throw "Flutter migrated source: $inputFile. Review it before continuing." } }
    $config = Get-Content -LiteralPath 'windows/flutter/ephemeral/generated_config.cmake' -Raw -Encoding UTF8
    if ($config -notmatch [regex]::Escape("set(FLUTTER_VERSION `"$versionLabel`" PARENT_SCOPE)") -or $config -notmatch [regex]::Escape("set(FLUTTER_VERSION_BUILD $BuildNumber PARENT_SCOPE)")) { throw 'Generated Flutter version mismatch.' }
    if ($configExit -ne 0 -and [IO.File]::ReadAllText($configLog) -notmatch 'Unable to find suitable Visual Studio toolchain') { throw "Unexpected Flutter configuration failure. See $configLog" }
    $defines = [regex]::Match($config, '"DART_DEFINES=([^"\r\n]*)"').Groups[1].Value
    $assemble = @('assemble','--no-version-check','--output=build','-dTargetPlatform=windows-x64','-dTrackWidgetCreation=true','-dBuildMode=release','-dTargetFile=lib/main.dart','-dTreeShakeIcons=true','-dDartObfuscation=false',"-dBuildName=$BuildName","-dBuildNumber=$BuildNumber","--DartDefines=$defines",'release_bundle_windows-x64_assets')
    Invoke-LoggedNative $flutter $assemble "$logPrefix-flutter-assemble.log" | Out-Null
    Invoke-LoggedNative $cmake @('-S','windows','-B',$build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',"-DCMAKE_MAKE_PROGRAM=$ninja","-DCMAKE_INSTALL_PREFIX=$bundle","-DMULTITAP_WINDOWS_VARIANT=$Variant") "$logPrefix-cmake-configure.log" | Out-Null
    Invoke-LoggedNative $cmake @('--build',$build,'--target','Multitap','--parallel',[string]$Parallel) "$logPrefix-cmake-build.log" | Out-Null
    Invoke-LoggedNative $cmake @('--install',$build,'--config','Release') "$logPrefix-cmake-stage.log" | Out-Null
    foreach ($inputFile in $inputFiles) { if ((Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash.ToLowerInvariant() -cne $sourceHashes[$inputFile]) { throw "Build source changed: $inputFile" } }
    $exe = Join-Path $bundle 'Multitap.exe'
    $info = (Get-Item -LiteralPath $exe).VersionInfo
    $expectedProduct = if ($Variant -eq 'Client') { 'Multitap' } else { 'Multitap Host' }
    $expectedOriginal = if ($Variant -eq 'Client') { 'Multitap.exe' } else { 'MultitapHostSettings.exe' }
    if ($info.ProductVersion -cne $versionLabel -or $info.ProductName -cne $expectedProduct -or $info.OriginalFilename -cne $expectedOriginal) { throw 'Compiled PE version or product identity mismatch.' }
    foreach ($requiredBundleFile in @('flutter_windows.dll','data/app.so','data/icudtl.dat','data/flutter_assets/AssetManifest.bin','data/licenses/libopus-COPYING.txt','data/licenses/WebView2-LICENSE.txt')) { if (-not [IO.File]::Exists((Join-Path $bundle $requiredBundleFile))) { throw "Bundle input is missing: $requiredBundleFile" } }
    $runtimeArguments = @{ Bundle = $bundle }
    if ($RuntimeDirectory) { $runtimeArguments.RuntimeDirectory = $RuntimeDirectory }
    & (Join-Path $PSScriptRoot 'copy-vc-runtime.ps1') @runtimeArguments | Out-Null
    $files = @(Get-ChildItem -LiteralPath $bundle -Recurse -File | ForEach-Object { [ordered]@{path=$_.FullName.Substring($bundle.Length+1).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()} })
    $packagingRequired = @()
    if ($Variant -eq 'HostSettings') { $packagingRequired += 'Host settings rename and requireAdministrator manifest via Host packager' }
    $receipt = [ordered]@{schemaVersion=1;variant=$Variant;method='Official Flutter generated configuration + release AOT + portable MSVC/CMake Ninja';flutterConfigExit=$configExit;version=$versionLabel;productName=$info.ProductName;internalName=$info.InternalName;originalFilename=$info.OriginalFilename;bundle=$bundle;parallel=$Parallel;sourceInputs=$sourceHashes;rawBundle=$true;verifiedVcRuntime=$true;packagingRequired=@($packagingRequired);installed=$false;executed=$false;signed=$false;files=$files}
    $receiptPath = "$logPrefix-build-receipt.json"
    [IO.File]::WriteAllText($receiptPath, ($receipt | ConvertTo-Json -Depth 7), [Text.UTF8Encoding]::new($false))
    [ordered]@{variant=$Variant;version=$versionLabel;bundle=$bundle;receipt=$receiptPath;installed=$false;executed=$false} | ConvertTo-Json
} finally {
    if ($pushed) { Pop-Location }
    foreach ($environmentName in $environmentNames) { [Environment]::SetEnvironmentVariable($environmentName, $savedEnvironment[$environmentName], 'Process') }
}

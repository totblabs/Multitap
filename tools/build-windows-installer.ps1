param([Parameter(Mandatory=$true)][ValidateSet('client')][string]$Product,[Parameter(Mandatory=$true)][string]$Version,[Parameter(Mandatory=$true)][string]$Payload,[string]$SetupSource,[switch]$Portable,[ValidateRange(1,128)][int]$Parallel=12)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'windows-executable-policy.ps1')
. (Join-Path $PSScriptRoot 'windows-payload-path-policy.ps1')
if($Version -notmatch '^\d+\.\d+\.\d+$'){throw 'Invalid version.'}
$root=Split-Path -Parent $PSScriptRoot
$source=(Resolve-Path -LiteralPath $Payload).Path
$compiler=Join-Path $root '.tools/nsis-download/nsis-3.13/makensis.exe'
$compilerArchive=Join-Path $root '.tools/nsis-download/nsis-3.13.zip'
if((Get-FileHash -LiteralPath $compilerArchive -Algorithm SHA256).Hash -ne 'BA63DFFC4410EE89193E1CB5A41989991BD77C61068DA17E3156D136B7B0B3D8'){throw 'Pinned NSIS archive hash mismatch.'}
$stage=Join-Path $root ('.artifacts/installer-build/'+$Product+'-'+$Version+'-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage|Out-Null
$stagePayload=Join-Path $stage 'payload';New-Item -ItemType Directory -Path $stagePayload|Out-Null
. (Join-Path $root 'deploy/windows/ClientProcesses.ps1')
Assert-ClientExecutableVersion (Join-Path $source 'Multitap.exe') $Version
$bin=Join-Path $stagePayload 'bin';New-Item -ItemType Directory -Path $bin|Out-Null
Get-ChildItem -LiteralPath $source|Copy-Item -Destination $bin -Recurse
$setup='MultitapClientSetup.exe';$admin='admin';$prefix='Multitap'
# The installer UI compiles the client version in (the update hand-off's
# installer-name check, its header). It is built here from the release
# source for exactly this version; a prebuilt copy with another version
# baked in makes every in-app update fall back to the full
# installer window.
$setupRoot=if($SetupSource){(Resolve-Path -LiteralPath $SetupSource).Path}else{$root}
# A shared tool directory must name the registered VS instance's real path.
$toolsRoot=Join-Path $root '.tools'
$toolsDirectory=Get-Item -LiteralPath $toolsRoot -Force
if($toolsDirectory.Attributes -band [IO.FileAttributes]::ReparsePoint){
  $targets=@($toolsDirectory.Target)
  if($targets.Count -ne 1){throw 'The build tools link must have one target.'}
  $toolsRoot=if([IO.Path]::IsPathRooted([string]$targets[0])){[string]$targets[0]}else{Join-Path $root ([string]$targets[0])}
}
if($Portable){
  # Explicit local build only: the prepared environment verifies compiler
  # hashes. Do not fabricate a registered Visual Studio installation.
  . (Join-Path $toolsRoot 'msvc-portable/env.ps1')
  $cmake=Join-Path $toolsRoot 'cmake/cmake-3.31.8-windows-x86_64/bin/cmake.exe'
  $ninja=Join-Path $toolsRoot 'ninja/1.13.2/ninja.exe'
  $setupBuild=Join-Path $setupRoot '.artifacts/installer-build/client-setup-portable'
  $nativePreference=$ErrorActionPreference
  try{
    $ErrorActionPreference='Continue'
    & $cmake -S (Join-Path $setupRoot 'apps/windows_host') -B $setupBuild -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=$ninja" *> (Join-Path $stage 'client-setup-configure.log')
    if($LASTEXITCODE -ne 0){throw 'Portable client installer UI configuration failed.'}
    & $cmake --build $setupBuild --target MultitapClientSetup --parallel $Parallel *> (Join-Path $stage 'client-setup-build.log')
    if($LASTEXITCODE -ne 0){throw 'Portable client installer UI build failed.'}
  }finally{$ErrorActionPreference=$nativePreference}
  $setupSource=Join-Path $setupBuild 'MultitapClientSetup.exe'
}else{
  $visualStudio=Join-Path ([IO.Path]::GetFullPath($toolsRoot)) 'visualstudio-buildtools'
  $cmake=Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
  $setupBuild=Join-Path $setupRoot 'apps/windows_host/build-client-setup'
  & $cmake -S (Join-Path $setupRoot 'apps/windows_host') -B $setupBuild -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$visualStudio"|Out-Null
  if($LASTEXITCODE -ne 0){throw 'Client installer UI configuration failed.'}
  & $cmake --build $setupBuild --config Release --target MultitapClientSetup|Out-Null
  if($LASTEXITCODE -ne 0){throw 'Client installer UI build failed.'}
  $setupSource=Join-Path $setupBuild 'Release/MultitapClientSetup.exe'
}
if(-not([Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($setupSource)).Contains("Multitap-$Version-windows-x64.exe"))){throw "Client installer UI was not built for $Version."}
Assert-MultitapAdministratorExecutable $setupSource|Out-Null
Copy-Item -LiteralPath $setupSource -Destination (Join-Path $stagePayload $setup)
Copy-Item -LiteralPath (Join-Path $root 'deploy/windows/Install-Client.ps1') -Destination $stagePayload
Copy-Item -LiteralPath (Join-Path $root 'deploy/windows/ClientProcesses.ps1') -Destination $stagePayload
# A malformed progress call still parses and only fails on the user's PC.
$scriptTokens=$null;$scriptErrors=$null;$scriptAst=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'deploy/windows/Install-Client.ps1'),[ref]$scriptTokens,[ref]$scriptErrors)
if($scriptErrors.Count){throw 'Install-Client.ps1 has syntax errors.'}
foreach($call in $scriptAst.FindAll({param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -ceq 'Report'},$true)){if($call.CommandElements.Count -ne 3 -or $call.CommandElements[1].Extent.Text -notmatch '\A-?\d+\z'){throw 'Install-Client.ps1 has a malformed Report call.'}}
$files=@(Get-ChildItem -LiteralPath $bin -Recurse -File|ForEach-Object{@{path=$_.FullName.Substring($bin.Length+1).Replace('\','/');size=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})
@{product='client';version=$Version;files=$files}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $stagePayload 'installer-payload.json') -Encoding UTF8
$files=@(Get-ChildItem -LiteralPath $stagePayload -Recurse -File|ForEach-Object{
  if($_.Extension -ieq '.ps1'){
    $scriptBytes=[IO.File]::ReadAllBytes($_.FullName)
    $scriptText=[IO.File]::ReadAllText($_.FullName,[Text.UTF8Encoding]::new($false,$true))
    if($scriptText -match '[^\x00-\x7f]' -and ($scriptBytes.Length -lt 3 -or $scriptBytes[0] -ne 239 -or $scriptBytes[1] -ne 187 -or $scriptBytes[2] -ne 191)){throw 'Windows PowerShell requires UTF-8 BOM for non-ASCII installer scripts.'}
  }
  $relative=$_.FullName.Substring($stagePayload.Length+1).Replace('\','/')
  if(-not (Test-MultitapWindowsPayloadPath $relative)){throw 'Private or non-product file in installer.'}
  @{path=$relative;size=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$output=Join-Path $root ".artifacts/releases/$prefix-$Version-windows-x64.exe"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output) | Out-Null
if(Test-Path -LiteralPath $output){throw 'Installer version already exists; use a new version.'}
& $compiler /V2 "/DPAYLOAD=$stagePayload" "/DOUTPUT=$output" "/DSETUP=$setup" "/DPRODUCT=$Product" "/DVERSION=$Version" "/DADMIN=$admin" (Join-Path $root 'deploy/windows/product.nsi')
if($LASTEXITCODE -ne 0){throw 'NSIS compilation failed.'}
# Inspect/extract the self-extractor without running the installer or changing Windows.
$verification=Join-Path $stage 'verify'
& (Join-Path $root '.tools/7zip/7z.exe') x $output "-o$verification" -y|Out-Null
if($LASTEXITCODE -ne 0){throw 'Installer extraction failed.'}
$extracted=Join-Path $verification '$PLUGINSDIR/payload'
foreach($file in $files){$path=Join-Path $extracted $file.path;if(-not(Test-Path -LiteralPath $path -PathType Leaf)-or (Get-Item -LiteralPath $path).Length -ne $file.size -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256){throw "Installer payload differs: $($file.path)"}}
$actual=@(Get-ChildItem -LiteralPath $extracted -Recurse -File)
if($actual.Count -ne $files.Count){throw 'Unexpected files embedded in installer.'}
$policies=@(Assert-MultitapAdministratorExecutable $output)
$policies+=Assert-MultitapAdministratorExecutable (Join-Path $extracted 'MultitapClientSetup.exe')
# The Windows client itself runs unelevated; its installer asks for administrator approval.
$policies+=Assert-MultitapAsInvokerExecutable (Join-Path $extracted 'bin/Multitap.exe')
$policies|ConvertTo-Json -Depth 3|Set-Content -LiteralPath ($output+'.policy.json') -Encoding UTF8
$item=Get-Item -LiteralPath $output;$sha=(Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
@{product=$Product;version=$Version;filename=$item.Name;size=$item.Length;sha256=$sha;compiler='NSIS 3.13';compilerArchiveSha256='ba63dffc4410ee89193e1cb5a41989991bd77c61068da17e3156d136b7b0b3d8';payloadVerified=$true;files=$files;runtimeInstallationTested=$false}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath ($output+'.json') -Encoding UTF8
Write-Output (@{installer=$item.Name;size=$item.Length;sha256=$sha;verifiedPayloadFiles=$files.Count;executedInstaller=$false}|ConvertTo-Json -Compress)

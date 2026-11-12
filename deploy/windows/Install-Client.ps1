param([string]$ProgressPipe)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'ClientProcesses.ps1')
$progress=$null;$progressReader=$null;$installerGate=$null;$installerGateHeld=$false
if($ProgressPipe){
  if($ProgressPipe -notmatch '^MultitapHostSetup-[0-9a-f]{32}$'){throw 'Invalid installation progress channel.'}
  # Two-way since payload-v1: the setup answers the file and shortcut requests below.
  $pipe=New-Object IO.Pipes.NamedPipeClientStream('.', $ProgressPipe, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::Asynchronous)
  $pipe.Connect(10000);$progress=New-Object IO.StreamWriter($pipe,[Text.UTF8Encoding]::new($false));$progress.AutoFlush=$true;$progressReader=New-Object IO.StreamReader($pipe,[Text.UTF8Encoding]::new($false))
}
# The words below reach the person through the setup window, in their Windows
# display language (docs/i18n.md): MULTITAP_LANG (ko|en), which the setup sets for
# this script so both say the same, else this process's UI culture; Korean for
# ko, English otherwise. Korean is the source text.
$ClientLanguage=if($env:MULTITAP_LANG -ceq 'ko' -or $env:MULTITAP_LANG -ceq 'en'){$env:MULTITAP_LANG}elseif([Globalization.CultureInfo]::CurrentUICulture.Name -like 'ko*'){'ko'}else{'en'}
function Words([string]$Korean,[string]$English){if($ClientLanguage -ceq 'ko'){$Korean}else{$English}}
function Report([int]$Step,[string]$Message){if($progress){$progress.WriteLine("$Step|"+$Message.Replace("`r",' ').Replace("`n",' '))}}
# ClientProcesses.ps1 is plain ASCII, so what it refuses arrives as a fixed code
# (it used to arrive as an English sentence in the failure view). Any other
# message already is the text to show.
# Self-contained (deploy/windows/tests/client-failure-text.test.ps1 loads it alone):
# [Language] defaults to the same choice as $ClientLanguage above.
function Get-ClientFailureText([string]$Message,[string]$Language=$(if($env:MULTITAP_LANG -ceq 'ko' -or $env:MULTITAP_LANG -ceq 'en'){$env:MULTITAP_LANG}elseif([Globalization.CultureInfo]::CurrentUICulture.Name -like 'ko*'){'ko'}else{'en'})){
  $korean=$Language -ceq 'ko'
  switch -CaseSensitive ($Message){
    'client_version_invalid'{if($korean){return '설치 파일의 버전 정보가 올바르지 않아요. 설치 파일을 다시 받아 실행하세요. 확인 코드: client_version_invalid.'};return 'The version information of the setup file is not valid. Download the setup file again and run it. Code: client_version_invalid.'}
    'client_executable_unsafe'{if($korean){return 'Multitap 실행 파일을 확인하지 못했어요. 설치 파일을 다시 받아 실행하세요. 확인 코드: client_executable_unsafe.'};return 'Could not verify the Multitap program file. Download the setup file again and run it. Code: client_executable_unsafe.'}
    'client_version_mismatch'{if($korean){return '설치 파일과 Multitap 실행 파일의 버전이 달라 설치를 멈췄어요. 설치 파일을 다시 받아 실행하세요. 확인 코드: client_version_mismatch.'};return 'The setup file and the Multitap program file have different versions, so the installation stopped. Download the setup file again and run it. Code: client_version_mismatch.'}
  }
  return $Message
}
# payload-v1: MultitapClientSetup.exe (the elevated setup that verified this
# package) writes the program files and the desktop shortcut; this script
# lists, requests and verifies them. A behaviour detection (AhnLab
# Safe Transaction, "실행 파일 생성") ends powershell.exe when it creates
# executables. The setup computes every destination itself from its own version.
function Invoke-ClientSetupRequest([string]$Request,[int]$TimeoutMs){
  if(-not $progress -or -not $progressReader){throw (Words '이 스크립트만으로는 설치할 수 없어요. Multitap 설치 파일을 실행하세요.' 'This script cannot install on its own. Run the Multitap setup file.')}
  $progress.Write($Request)
  $reply=$progressReader.ReadLineAsync()
  if(-not $reply.Wait($TimeoutMs)){throw (Words '설치 프로그램이 응답하지 않아요. 설치 파일을 다시 실행하세요.' 'The setup is not responding. Run the setup file again.')}
  $line=[string]$reply.Result
  if($line -cmatch '\Apayload-v1\|ok(\|[0-9]{1,9}){0,2}\z'){return}
  if($line -cmatch '\Apayload-v1\|error\|([a-z0-9_]{1,64})\z'){
    $code=$Matches[1]
    if($code -ceq 'shortcut_foreign'){throw (Words '바탕 화면의 Multitap 바로가기가 다른 프로그램을 가리켜요. 그 바로가기의 이름을 바꾼 뒤 다시 설치하세요. 확인 코드: shortcut_foreign.' 'The Multitap shortcut on the desktop points to another program. Rename that shortcut, then install again. Code: shortcut_foreign.')}
    if($code -ceq 'shortcut_unsafe'){throw (Words '바탕 화면의 Multitap 바로가기를 확인하지 못했어요. 그 바로가기의 이름을 바꾼 뒤 다시 설치하세요. 확인 코드: shortcut_unsafe.' 'Could not verify the Multitap shortcut on the desktop. Rename that shortcut, then install again. Code: shortcut_unsafe.')}
    throw (Words ('설치 프로그램이 요청을 마치지 못했어요. 확인 코드: '+$code+'.') ('The setup could not finish a request. Code: '+$code+'.'))
  }
  throw (Words '설치 프로그램의 응답을 확인하지 못했어요. 설치 파일을 다시 실행하세요.' 'Could not read the setup''s answer. Run the setup file again.')
}
function Request-ClientPayloadPlacement($Files,[string]$Version){
  $request=New-Object Text.StringBuilder
  [void]$request.Append("-6|place-v1|client|$Version|-`n")
  foreach($file in $Files){[void]$request.Append('-6|file|'+[string]$file.size+'|'+([string]$file.sha256).ToLowerInvariant()+'|'+[string]$file.path+"`n")}
  [void]$request.Append("-6|commit`n")
  Invoke-ClientSetupRequest $request.ToString() 1800000
}
function Request-ClientShortcut([string]$Version){Invoke-ClientSetupRequest "-6|shortcut-v1|client|$Version`n" 120000}
function Restore-ClientShortcut{Invoke-ClientSetupRequest "-6|shortcut-restore-v1|client`n" 120000}
# The verified installation replaces an earlier per-user one:
# the setup removes exactly its marked version folders, by handle and never
# through a link. Settings, sign-in memory and logs stay.
function Request-ClientLegacyRemoval([string]$Version){Invoke-ClientSetupRequest "-6|legacy-remove-v1|client|$Version`n" 300000}
# Older versions under Program Files go the same way. The version the new one
# replaced stays, and so does a folder still in use.
function Request-ClientOldRemoval([string]$Version,[string]$Replaced){Invoke-ClientSetupRequest "-6|old-remove-v1|client|$Version|$Replaced`n" 300000}
# The package copy the installer extracted under %TEMP% (a folder this user
# can change): the setup removes it by handle, never following a link.
function Request-ClientSourceRemoval{Invoke-ClientSetupRequest "-6|source-remove-v1|client`n" 300000}
function Assert-RegularDirectory([string]$Path){
  if(Test-Path -LiteralPath $Path){$item=Get-Item -LiteralPath $Path -Force;if(-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw (Words '설치할 폴더가 링크이거나 파일이에요. Program Files의 Multitap 폴더를 확인하세요.' 'The install folder is a link or a file. Check the Multitap folder in Program Files.')}}
}
# Program Files\Multitap\Client: Administrators own it; only SYSTEM and
# Administrators may change it (Users read and run), like the Host folder.
function Protect-ClientDirectory([string]$Path){
  $acl=New-Object Security.AccessControl.DirectorySecurity
  $acl.SetAccessRuleProtection($true,$false)
  $acl.SetOwner((New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')))
  foreach($entry in @(@('S-1-5-18','FullControl'),@('S-1-5-32-544','FullControl'),@('S-1-5-32-545','ReadAndExecute'))){$sid=New-Object Security.Principal.SecurityIdentifier($entry[0]);$acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,$entry[1],'ContainerInherit,ObjectInherit','None','Allow')))}
  Set-Acl -LiteralPath $Path -AclObject $acl
}
# The installer log lives in this user's %LOCALAPPDATA%: written only when no
# folder on the way is a link, so this elevated script never writes elsewhere.
function Write-ClientInstallLog([string]$Text){
  try{
    $local=[IO.Path]::GetFullPath($env:LOCALAPPDATA).TrimEnd('\');$path=$local
    foreach($name in @('Multitap','Logs','Installer')){
      $path=Join-Path $path $name
      if(Test-Path -LiteralPath $path){$item=Get-Item -LiteralPath $path -Force;if(-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){return}}
      else{New-Item -ItemType Directory -Path $path|Out-Null}
    }
    $log=Join-Path $path 'install-client.log'
    if(Test-Path -LiteralPath $log){$item=Get-Item -LiteralPath $log -Force;if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $item.Length -gt 1048576){return}}
    Add-Content -LiteralPath $log -Value ((Get-Date).ToString('o')+' '+$Text) -Encoding UTF8
  }catch{}
}
try{
  $installerGate=New-Object Threading.Mutex($false,'Global\MultitapClient.Installer')
  try{$installerGateHeld=$installerGate.WaitOne(0)}catch [Threading.AbandonedMutexException]{$installerGateHeld=$true}
  if(-not $installerGateHeld){throw (Words '다른 Multitap 설치 또는 제거가 진행 중이에요. 끝난 뒤 다시 시도하세요.' 'Another Multitap install or removal is running. Try again when it is done.')}
  Report 0 (Words '설치 파일과 대상 경로를 확인하고 있어요.' 'Checking the setup files and destination.')
  $metadata=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'installer-payload.json') -Raw -Encoding UTF8|ConvertFrom-Json
  if($metadata.product -ne 'client' -or $metadata.version -notmatch '^\d+\.\d+\.\d+$'){throw (Words '설치 파일의 제품 정보가 올바르지 않아요. 설치 파일을 다시 받아 실행하세요.' 'The product information in the setup file is not valid. Download the setup file again and run it.')}
  $source=Join-Path $PSScriptRoot 'bin'
  $sourceBoundary=[IO.Path]::GetFullPath($source).TrimEnd('\')+'\'
  foreach($file in $metadata.files){
    $path=[IO.Path]::GetFullPath((Join-Path $source $file.path))
    if(-not $path.StartsWith($sourceBoundary,[StringComparison]::OrdinalIgnoreCase) -or $file.path -match '(^|[/\\])\.\.?([/\\]|$)'){throw (Words '설치 파일에 허용되지 않는 경로가 있어요. 설치 파일을 다시 받아 실행하세요.' 'The setup file contains a path that is not allowed. Download the setup file again and run it.')}
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256 -or (Get-Item -LiteralPath $path).Length -ne $file.size){throw (Words '설치 파일을 확인하지 못했어요. 설치 파일을 다시 받아 실행하세요.' 'Could not verify the setup files. Download the setup file again and run it.')}
  }
  Assert-ClientExecutableVersion (Join-Path $source 'Multitap.exe') $metadata.version
  # The client requires administrator approval, so it lives where only an
  # administrator can change it (an earlier per-user copy under
  # %LOCALAPPDATA%\Multitap\Client\<version> is replaced below).
  # Settings, sign-in memory and logs stay in %LOCALAPPDATA%\Multitap.
  # One fixed folder, Program Files\Multitap\Client, overwritten in
  # place on update (earlier Client\<version> folders go below).
  $productRoot=Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'Multitap';$root=Join-Path $productRoot 'Client';$target=$root
  $legacyRoot=Join-Path $env:LOCALAPPDATA 'Multitap\Client'
  foreach($path in @($productRoot,$root)){Assert-RegularDirectory $path}
  $marker=Join-Path $target 'multitap-client.json';$previousMarker=$null
  if(Test-Path -LiteralPath $marker){
    if(-not(Test-Path -LiteralPath $marker -PathType Leaf)){throw (Words '설치할 폴더에 Multitap이 만들지 않은 항목이 있어요. Program Files\Multitap\Client 폴더를 확인하세요.' 'The install folder has an item Multitap did not create. Check the Program Files\Multitap\Client folder.')}
    $previousMarker=[IO.File]::ReadAllBytes($marker)
    $old=Get-Content -LiteralPath $marker -Raw -Encoding UTF8|ConvertFrom-Json
    if($old.product -ne 'Multitap' -or $old.version -notmatch '^\d+\.\d+\.\d+$'){throw (Words '설치할 폴더의 설치 정보가 Multitap 것이 아니에요. Program Files\Multitap\Client 폴더를 확인하세요.' 'The installation details in the install folder are not from Multitap. Check the Program Files\Multitap\Client folder.')}
  }
  # Native preflight and placement share one exact marked ownership rule.
  # Installed versions are verified without mutation.
  Invoke-ClientSetupRequest "-6|shortcut-preflight-v1|client|$($metadata.version)`n" 120000
  $shortcutPath=Join-Path ([Environment]::GetFolderPath('Desktop')) 'Multitap.lnk'
  $previousShortcutPath=$shortcutPath
  $shell=New-Object -ComObject WScript.Shell
  $shortcut=$shell.CreateShortcut($shortcutPath)
  $executable=Join-Path $target 'Multitap.exe'
  # Every package, version and path check passed. A seamless update
  # (update-handoff-v1) lets the running client close itself here, before
  # anything is replaced; other installer modes ignore this marker.
  Report -5 'preflight-complete-v1'
  # The running client may still be an earlier per-user one.
  Invoke-ClientSetupRequest "-6|client-stop-v1|client|$($metadata.version)`n" 120000
  Report 1 (Words '프로그램 파일을 설치하고 있어요.' 'Installing the program files.')
  if(-not(Test-Path -LiteralPath $root)){New-Item -ItemType Directory -Force -Path $root|Out-Null;Protect-ClientDirectory $root}
  # Write our ownership marker before copying so a partial installation can be retried
  # (the previous one is put back if placement fails, so the old version still starts).
  $markerWritten=$true
  @{product='Multitap';version=$metadata.version;mode='cloud-auth';installed=(Get-Date).ToString('o')}|ConvertTo-Json|Set-Content -LiteralPath $marker -Encoding UTF8
  # The setup writes the listed files into this marked folder: exact size and
  # SHA-256 per file, no links in the folder or below it, an identical file of
  # the same version kept, everything it changed undone if one file fails.
  Request-ClientPayloadPlacement -Files $metadata.files -Version $metadata.version;$placed=$true
  Report 2 (Words '설치 정보를 확인하고 있어요.' 'Checking the installation details.')
  if(-not(Test-Path -LiteralPath $executable -PathType Leaf)){throw (Words '설치한 Multitap 실행 파일을 찾지 못했어요. 다시 설치하세요.' 'The installed Multitap program file was not found. Install again.')}
  Report 3 (Words '바로가기를 만들고 있어요.' 'Creating the shortcut.')
  # A previous Client\<version> folder still exists here; a later failure puts the
  # earlier shortcut back (the setup keeps its exact bytes) so the installed
  # client keeps starting. The setup writes the shortcut (payload-v1).
  if(Test-Path -LiteralPath $previousShortcutPath){$previousShortcut=@{TargetPath=$shortcut.TargetPath}}
  Request-ClientShortcut $metadata.version;$shortcutChanged=$true
  Report 4 (Words '설치된 파일을 확인하고 있어요.' 'Checking the installed files.')
  foreach($file in $metadata.files){if((Get-FileHash -LiteralPath (Join-Path $target $file.path) -Algorithm SHA256).Hash -ne $file.sha256){throw (Words '설치된 파일을 확인하지 못했어요. 다시 설치하세요.' 'Could not verify the installed files. Install again.')}}
  Assert-ClientExecutableVersion $executable $metadata.version
  # The verified installation no longer needs the copy the installer extracted
  # under %TEMP%; remove it now instead of when the installer window closes
  # (a delayed Defender ML verdict can quarantine that leftover copy).
  # The setup removes it by handle: this elevated script never deletes
  # recursively in a folder the user can change.
  $temporary=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
  if([IO.Path]::GetFullPath($source).StartsWith($temporary,[StringComparison]::OrdinalIgnoreCase)){try{Request-ClientSourceRemoval}catch{}}
  # Only now, with the new installation verified and the shortcut moved, the
  # earlier per-user copy goes. A folder still in use stays.
  try{Request-ClientLegacyRemoval $metadata.version}catch{Write-ClientInstallLog ('legacy client cleanup: '+$_.Exception.Message)}
  # Every earlier Client\<version> folder (one still in use stays for the next update).
  try{Request-ClientOldRemoval $metadata.version $metadata.version}catch{Write-ClientInstallLog ('old client cleanup: '+$_.Exception.Message)}
  try{
    $stages=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'Multitap-Updates'
    foreach($stage in (Get-ClientStaleUpdateStages $stages (Get-Date).AddDays(-1))){try{Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction Stop}catch{Write-ClientInstallLog ('update stage cleanup: '+$_.Exception.Message)}}
  }catch{Write-ClientInstallLog ('update stage cleanup: '+$_.Exception.Message)}
  Invoke-ClientSetupRequest "-6|register-installation-v1|client|$($metadata.version)`n" 120000
  Report 5 (Words '클라이언트를 준비하고 있어요.' 'Getting the client ready.')
  # The verified native setup opens the client through the desktop shell's
  # unelevated token after this elevated installation process has exited.
  Report 6 (Words 'Multitap 설치를 마쳤어요.' 'Multitap is installed.')
}catch{
  $failure=Get-ClientFailureText $_.Exception.Message
  # Placement undid its own changes; the marker goes back to what it was.
  if($markerWritten -and -not $placed){try{if($previousMarker){[IO.File]::WriteAllBytes($marker,$previousMarker)}else{Remove-Item -LiteralPath $marker -Force}}catch{}}
  if($shortcutChanged -and $previousShortcut -and $previousShortcut.TargetPath -and $previousShortcut.TargetPath -ine $executable -and (Test-Path -LiteralPath $previousShortcut.TargetPath -PathType Leaf)){
    try{Restore-ClientShortcut;$failure+=(Words ' 바탕 화면 바로가기는 이전 버전으로 되돌렸어요.' ' The desktop shortcut was put back to the previous version.')}catch{$failure+=(Words ' 바탕 화면 바로가기를 되돌렸는지 확인하지 못했어요.' ' Could not confirm the desktop shortcut was put back.')}
  }
  # A seamless update shows only a code; keep the reason on this PC.
  Write-ClientInstallLog $failure
  try{Report -1 $failure}catch{};exit 1
}finally{if($installerGate){if($installerGateHeld){try{$installerGate.ReleaseMutex()}catch{}};$installerGate.Dispose()};if($progress){$progress.Dispose()}}

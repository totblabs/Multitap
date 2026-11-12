# Each update leaves its stage under ProgramData\Multitap-Updates (more than a
# gigabyte after many updates). Recursive deletion is safe only where nobody else can write, so
# the root must be SYSTEM and Administrators alone. A running update is fresh.
function Test-ClientProtectedFolder([string]$Path) {
  $acl=Get-Acl -LiteralPath $Path
  if($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin @('S-1-5-32-544','S-1-5-18')){return $false}
  foreach($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){if($rule.IdentityReference.Value -notin @('S-1-5-32-544','S-1-5-18')){return $false}}
  return $true
}
function Get-ClientStaleUpdateStages([string]$Root,[datetime]$Before,[scriptblock]$Protected={param($Path)Test-ClientProtectedFolder $Path}) {
  if(-not(Test-Path -LiteralPath $Root -PathType Container)){return @()}
  if((Get-Item -LiteralPath $Root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){return @()}
  if(-not(& $Protected $Root)){return @()}
  return @(Get-ChildItem -LiteralPath $Root -Directory -Force|Where-Object{
    $_.Name -cmatch '\A\{[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}\}\z' -and -not($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -and $_.LastWriteTime -lt $Before
  }|ForEach-Object{$_.FullName})
}

# This file is plain ASCII without a byte-order mark (Windows PowerShell 5.1 would misread
# Korean in it), so a failure that reaches a person leaves here as a fixed code. The words
# for each code are in Install-Client.ps1 (Get-ClientFailureText).
function Assert-ClientExecutableVersion([string]$Executable,[string]$Version) {
  if($Version -notmatch '\A[0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6}\z'){throw 'client_version_invalid'}
  $item=Get-Item -LiteralPath $Executable -Force
  if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'client_executable_unsafe'}
  foreach($value in @($item.VersionInfo.ProductVersion,$item.VersionInfo.FileVersion)){
    if(-not $value -or $value -notmatch '\A([0-9]{1,6}\.[0-9]{1,6}\.[0-9]{1,6})(?:\+[0-9]{1,10})?\z' -or $Matches[1] -cne $Version){
      throw 'client_version_mismatch'
    }
  }
}

function Test-ProcessGone($Process) {
  try{return $Process.HasExited}catch{return $true}
}

# [ClientRoot]: every installation root to look in. The client lives in
# Program Files\Multitap\Client; an earlier per-user installation runs from
# %LOCALAPPDATA%\Multitap\Client, so an update from one of those names both.
function Get-VerifiedClientProcess([string[]]$ClientRoot) {
  $roots=@($ClientRoot|Where-Object{$_}|ForEach-Object{[IO.Path]::GetFullPath($_).TrimEnd('\')})
  foreach($process in @(Get-Process -Name Multitap -ErrorAction SilentlyContinue)){
   # A seamless update tells the running client to close right before this
   # check: a client that exits while it is inspected is simply not running
   # (reading its StartTime throws; without the catch the update fails).
   try{
    try{$image=$process.Path}catch{continue}
    if(-not $image){continue}
    $image=[IO.Path]::GetFullPath($image)
    $root=@($roots|Where-Object{$image.StartsWith($_+'\',[StringComparison]::OrdinalIgnoreCase)})|Select-Object -First 1
    if(-not $root){continue}
    $version=[IO.Path]::GetFileName([IO.Path]::GetDirectoryName($image))
    $expected=Join-Path (Join-Path $root $version) 'Multitap.exe'
    if($version -notmatch '^\d{1,6}\.\d{1,6}\.\d{1,6}$' -or $image -ine $expected){throw 'Unexpected process in client installation directory.'}
    foreach($path in @($root,(Split-Path -Parent $image),$image)){
      $item=Get-Item -LiteralPath $path -Force
      if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Refusing linked client process path.'}
    }
    $marker=Join-Path (Split-Path -Parent $image) 'multitap-client.json'
    $entry=Get-Item -LiteralPath $marker -Force
    if($entry.Length -gt 4096 -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Unsafe client ownership marker.'}
    $metadata=[IO.File]::ReadAllText($marker,[Text.Encoding]::UTF8)|ConvertFrom-Json
    if($metadata.product -cne 'Multitap' -or $metadata.version -cne $version){throw 'Client process ownership mismatch.'}
    # Retain a process handle before waiting so a recycled PID is not targeted.
    $null=$process.Handle
    [pscustomobject]@{Process=$process;Image=$image;Started=$process.StartTime.ToUniversalTime().Ticks}
   }catch{
    if(Test-ProcessGone $process){continue}
    throw
   }
  }
}

function Wait-NoVerifiedClient([string[]]$ClientRoot,[int]$Milliseconds) {
  $deadline=[DateTime]::UtcNow.AddMilliseconds($Milliseconds)
  do{
    $alive=@(Get-VerifiedClientProcess $ClientRoot);foreach($entry in $alive){$entry.Process.Dispose()}
    if(-not $alive.Count){return $true}
    Start-Sleep -Milliseconds 200
  }while([DateTime]::UtcNow -lt $deadline)
  return $false
}

# Asks a verified client to close. A client resting in the notification area
# (its main window hidden) has no window CloseMainWindow could
# reach, so it is first asked to show itself: starting the same verified
# executable with --restore-existing makes the single-instance check restore the
# running one; that second process creates no window and ends at once. Older
# clients that know no such option simply activate the running one, which is
# the same thing. $Restore is replaceable for the test.
function Request-ClientWindowClose($Process,[string]$Image,[scriptblock]$Restore={param($Path) Start-Process -FilePath $Path -ArgumentList '--restore-existing' -WindowStyle Hidden -ErrorAction Stop|Out-Null},[int]$WaitMilliseconds=3000) {
  $Process.Refresh()
  if($Process.MainWindowHandle -eq [IntPtr]::Zero){
    try{& $Restore $Image}catch{}
    $deadline=[DateTime]::UtcNow.AddMilliseconds($WaitMilliseconds)
    while([DateTime]::UtcNow -lt $deadline -and -not(Test-ProcessGone $Process)){
      $Process.Refresh()
      if($Process.MainWindowHandle -ne [IntPtr]::Zero){break}
      Start-Sleep -Milliseconds 50
    }
  }
  return $Process.CloseMainWindow()
}

function Stop-VerifiedClientProcesses([string[]]$ClientRoot) {
  # A seamless update has just told the client (and its remote windows) to
  # close themselves: let them, rather than racing a second close request.
  if(Wait-NoVerifiedClient $ClientRoot 8000){return}
  $owned=@(Get-VerifiedClientProcess $ClientRoot)
  foreach($entry in $owned){
    $process=$entry.Process
    try{
      if($process.HasExited){continue}
      if([IO.Path]::GetFullPath($process.Path) -ine $entry.Image -or $process.StartTime.ToUniversalTime().Ticks -ne $entry.Started){throw 'Client process identity changed.'}
      $null=Request-ClientWindowClose $process $entry.Image
      if(-not $process.WaitForExit(5000)){
        if([IO.Path]::GetFullPath($process.Path) -ine $entry.Image -or $process.StartTime.ToUniversalTime().Ticks -ne $entry.Started){throw 'Client process identity changed before close.'}
        $process.Kill()
        if(-not $process.WaitForExit(5000)){throw 'Client did not exit.'}
      }
    }catch{
      # It exited on its own between the checks: the goal is reached.
      if(Test-ProcessGone $process){continue}
      throw
    }finally{$process.Dispose()}
  }
  # Remote windows of a closed client can take a moment to finish exiting.
  if(-not(Wait-NoVerifiedClient $ClientRoot 5000)){throw 'A client is still running. Installation was stopped before replacing files.'}
}

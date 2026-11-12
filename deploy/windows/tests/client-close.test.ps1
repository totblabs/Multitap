param([string]$Output)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../ClientProcesses.ps1')

# A stand-in for System.Diagnostics.Process: no real process is touched. The
# window appears (MainWindowHandle becomes non-zero) when the restore step ran.
function New-FakeClient([bool]$Shown,[bool]$Exited=$false) {
  $state=[pscustomobject]@{Shown=$Shown;Closed=0;Refreshed=0;Restored=@();Exited=$Exited}
  $process=New-Object psobject
  $process|Add-Member -NotePropertyName State -NotePropertyValue $state
  $process|Add-Member -MemberType ScriptProperty -Name MainWindowHandle -Value {if($this.State.Shown){[IntPtr]42}else{[IntPtr]::Zero}}
  $process|Add-Member -MemberType ScriptProperty -Name HasExited -Value {$this.State.Exited}
  $process|Add-Member -MemberType ScriptMethod -Name Refresh -Value {$this.State.Refreshed++}
  $process|Add-Member -MemberType ScriptMethod -Name CloseMainWindow -Value {$this.State.Closed++;return [bool]$this.State.Shown}
  return $process
}
function Assert($condition,[string]$message){if(-not $condition){throw "Client close QA failed: $message"}}
$image='C:\Program Files\Multitap\Client\0.4.99\Multitap.exe'

# 1. A visible client is simply asked to close: nothing is started.
$visible=New-FakeClient $true
$restoreCalls=0
$closed=Request-ClientWindowClose $visible $image {param($Path) $script:restoreCalls++} 200
Assert $closed 'a visible client was not asked to close'
Assert ($restoreCalls -eq 0) 'a visible client was restored needlessly'
Assert ($visible.State.Closed -eq 1) 'CloseMainWindow was not sent once'

# 2. A client resting in the tray is first shown (the verified image restores
#    the running one), then asked to close.
$hidden=New-FakeClient $false
$script:seen=@()
$closed=Request-ClientWindowClose $hidden $image {param($Path) $script:seen+=$Path;$hidden.State.Shown=$true} 2000
Assert ($script:seen.Count -eq 1 -and $script:seen[0] -eq $image) 'the verified executable was not used to restore the tray client'
Assert ($closed -and $hidden.State.Closed -eq 1) 'a hidden client was not asked to close after it was shown'

# 3. The restore step fails (or the window never appears): the close request is
#    still sent, nothing throws, and the caller goes on to its own timeout.
$stuck=New-FakeClient $false
$closed=Request-ClientWindowClose $stuck $image {param($Path) throw 'cannot start'} 300
Assert (-not $closed -and $stuck.State.Closed -eq 1) 'a failed restore was not tolerated'
$slow=New-FakeClient $false
$closed=Request-ClientWindowClose $slow $image {param($Path) $null=$Path} 300
Assert (-not $closed -and $slow.State.Closed -eq 1) 'a window that never appeared was not tolerated'

# 4. The client exits while it is being restored: no waiting for a window.
$gone=New-FakeClient $false $true
$watch=[Diagnostics.Stopwatch]::StartNew()
$null=Request-ClientWindowClose $gone $image {param($Path) $null=$Path} 5000
Assert ($watch.ElapsedMilliseconds -lt 2000) 'waited for a client that had already exited'

# 5. The default restore step starts exactly the verified image with the
#    documented option and a hidden window style (read from the function text).
$text=(Get-Command Request-ClientWindowClose).ScriptBlock.ToString()
Assert ($text -match '--restore-existing' -and $text -match '-WindowStyle Hidden' -and $text -match '-FilePath \$Path') 'the default restore step changed'

$result=[ordered]@{passed=$true;visibleClientClosedDirectly=$true;trayClientRestoredThenClosed=$true;failedRestoreTolerated=$true;exitedClientNotWaitedFor=$true;processExecuted=$false}
if($Output){$result|ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding UTF8}
$result|ConvertTo-Json -Compress

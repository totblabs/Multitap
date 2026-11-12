param([Parameter(Mandatory=$true)][string]$Executable,[switch]$Json)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$taskExe=(Resolve-Path -LiteralPath $Executable).Path
$taskHostRoot=Join-Path $taskRoot 'apps/windows_host'
if(-not $taskExe.StartsWith($taskHostRoot+'\',[StringComparison]::OrdinalIgnoreCase) -or $taskExe.Substring($taskHostRoot.Length).Replace('/','\') -notmatch '\A\\build-[0-9]+\.[0-9]+\.[0-9]+(?:-native)?(?:-candidate)?\\Release\\MultitapHost\.exe\z'){throw 'Tray QA requires an exact owned versioned executable'}
. (Join-Path $PSScriptRoot 'windows-executable-policy.ps1')
if((Get-MultitapExecutionPolicy $taskExe).level -ne 'asInvoker'){throw 'Tray QA must not request administrator elevation'}
$taskHash=(Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash.ToLowerInvariant()
$taskStart=[Diagnostics.ProcessStartInfo]::new($taskExe,'--tray-selftest')
$taskStart.UseShellExecute=$false;$taskStart.CreateNoWindow=$true;$taskStart.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
$taskProcess=[Diagnostics.Process]::Start($taskStart)
try {
  $null=$taskProcess.Handle
  if(-not $taskProcess.WaitForExit(10000)){$taskProcess.Kill();$taskProcess.WaitForExit();throw 'Owned tray QA child timed out'}
  if($taskProcess.ExitCode -ne 0){throw ('Actual Windows notification-area API test failed: '+$taskProcess.ExitCode)}
  $taskResult=[pscustomobject]@{kind='actual-shell-tray-lifecycle';passed=$true;sha256=$taskHash;exitCode=$taskProcess.ExitCode;realShellNotifyIcon=$true;iconCreateRecreateAccepted=$true;iconRemovalInvoked=$true;iconRemovalResultVerified=$false;qaMutexIndependentOfInstalledTray=$true;userInputInjected=$false;serviceOrDriverChanged=$false;visibleIconPlacementVerified=$false}
  if($Json){$taskResult|ConvertTo-Json -Compress}else{$taskResult}
} finally {$taskProcess.Dispose()}

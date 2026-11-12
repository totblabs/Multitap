param([Parameter(Mandatory=$true)][string]$Executable,[Parameter(Mandatory=$true)][string]$Version,[string]$Output)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../ClientProcesses.ps1')
Assert-ClientExecutableVersion $Executable $Version
$rejected=0
foreach($wrong in @('0.0.0','1.2','1.2.3-beta',"$Version`n",'1000000.2.3')){
  $caught=$false
  try { Assert-ClientExecutableVersion $Executable $wrong } catch {$caught=$true}
  if(-not $caught){throw 'Incorrect executable/package version was accepted.'}
  ++$rejected
}
$result=[ordered]@{passed=$true;actualProductVersion=(Get-Item -LiteralPath $Executable).VersionInfo.ProductVersion;expectedPackageVersion=$Version;actualPeVersionMatched=$true;invalidOrMismatchedRejected=$rejected;processExecuted=$false;installedFilesChanged=$false}
if($Output){$result|ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding UTF8}
$result|ConvertTo-Json -Compress

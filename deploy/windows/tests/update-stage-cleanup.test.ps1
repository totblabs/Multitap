$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../ClientProcesses.ps1')
function Assert-QA([bool]$Value,[string]$Reason){if(-not $Value){throw $Reason}}
$root=Join-Path ([IO.Path]::GetTempPath()) ('tv-stage-qa-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $root
$cases=0
try{
  $old='{11111111-2222-3333-4444-555555555555}';$fresh='{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}'
  foreach($name in @($old,$fresh,'not-a-stage','{11111111-2222-3333-4444-55555555555}')){$null=New-Item -ItemType Directory -Path (Join-Path $root $name)}
  $null=New-Item -ItemType File -Path (Join-Path $root '{22222222-2222-3333-4444-555555555555}')
  $null=New-Item -ItemType Junction -Path (Join-Path $root '{33333333-2222-3333-4444-555555555555}') -Target (Join-Path $root 'not-a-stage')
  $past=(Get-Date).AddDays(-3)
  foreach($name in @($old,'not-a-stage','{11111111-2222-3333-4444-55555555555}')){(Get-Item -LiteralPath (Join-Path $root $name)).LastWriteTime=$past}
  (Get-Item -LiteralPath (Join-Path $root '{33333333-2222-3333-4444-555555555555}') -Force).LastWriteTime=$past
  $trusted={param($Path)$true}
  $plan=@(Get-ClientStaleUpdateStages $root (Get-Date).AddDays(-1) $trusted)
  Assert-QA ($plan.Count -eq 1 -and (Split-Path -Leaf $plan[0]) -ceq $old) ('Unexpected stale stages: '+($plan -join ','))
  $cases++
  # With every age allowed, only the two real stages qualify, never the link.
  $all=@(Get-ClientStaleUpdateStages $root (Get-Date).AddDays(1) $trusted|ForEach-Object{Split-Path -Leaf $_}|Sort-Object)
  Assert-QA (($all -join ',') -ceq ((@($old,$fresh)|Sort-Object) -join ',')) ('A link or a file passed as a stage: '+($all -join ','))
  $cases++
  Assert-QA (@(Get-ClientStaleUpdateStages (Join-Path $root 'absent') (Get-Date) $trusted).Count -eq 0) 'A missing root produced stages'
  $cases++
  # The temp folder is writable by this user, so the real check must refuse it.
  Assert-QA (-not(Test-ClientProtectedFolder $root)) 'A user-writable folder passed as protected'
  Assert-QA (@(Get-ClientStaleUpdateStages $root (Get-Date).AddDays(-1)).Count -eq 0) 'An unprotected root was cleaned'
  $cases++
  Assert-QA (@(Get-ClientStaleUpdateStages $root (Get-Date).AddDays(-1) {param($Path)$false}).Count -eq 0) 'A refused root was cleaned'
  $cases++
  $source=[IO.File]::ReadAllText((Join-Path $PSScriptRoot '../Install-Client.ps1'))
  Assert-QA ($source.Contains('Get-ClientStaleUpdateStages $stages (Get-Date).AddDays(-1)')) 'The installer does not clean old stages'
  $cases++
}finally{
  Remove-Item -LiteralPath (Join-Path $root '{33333333-2222-3333-4444-555555555555}') -Force -Recurse -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $root -Recurse -Force
}
@{passed=$true;cases=$cases}|ConvertTo-Json -Compress

param([Parameter(Mandatory=$true)][string]$SourceRoot,[switch]$Baseline)
$ErrorActionPreference='Stop'
$script:checks=0;$script:failures=0
function Assert-Path([bool]$Passed,[string]$Label){
  $script:checks++
  if(-not $Passed){$script:failures++;Write-Output ('FAIL: '+$Label)}
}
function Read-Ast([string]$File){
  $tokens=$null;$errors=$null
  $ast=[Management.Automation.Language.Parser]::ParseFile($File,[ref]$tokens,[ref]$errors)
  if($errors.Count){throw 'Policy source parse failed.'}
  return $ast
}
$installer=Read-Ast (Join-Path $SourceRoot 'tools/build-windows-installer.ps1')
$guards=@($installer.FindAll({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.Contains("throw 'Private or non-product file in installer.'")},$true))
if($guards.Count -ne 1){throw 'Expected one actual payload admission guard.'}
if($Baseline){
  # Evaluate the original AST expression itself, not a copied deny regex.
  $condition=$guards[0].Clauses[0].Item1.Extent.Text
  if($condition -notmatch '^\$relative\s+-match\s+'){throw 'Unexpected baseline predicate.'}
  $actualPolicy=[ScriptBlock]::Create('param([string]$relative) -not ('+$condition+')')
}else{
  $helperPath=Join-Path $SourceRoot 'tools/windows-payload-path-policy.ps1'
  $helper=Read-Ast $helperPath
  $definitions=@($helper.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$true))
  if($definitions.Count -ne 1 -or $definitions[0].Name -cne 'Test-MultitapWindowsPayloadPath'){throw 'Unexpected payload policy helper.'}
  # Load only the reviewed pure function AST. No installer code is invoked.
  . ([ScriptBlock]::Create($definitions[0].Extent.Text))
  $actualPolicy={param([string]$relative) Test-MultitapWindowsPayloadPath $relative}
  Assert-Path ($guards[0].Clauses[0].Item1.Extent.Text -ceq '-not (Test-MultitapWindowsPayloadPath $relative)') 'actual installer guard invokes the pure policy'
  $dotSources=@($installer.FindAll({param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.InvocationOperator -eq [Management.Automation.Language.TokenKind]::Dot -and $node.Extent.Text.Contains("'windows-payload-path-policy.ps1'")},$true))
  Assert-Path ($dotSources.Count -eq 1) 'actual installer loads the pure policy once'
}
$blocked=@(
  'bin/.git/config','bin/example/.GIT/HEAD','bin/.hg/store/data',
  'bin/.svn/wc.db','bin/.bzr/branch/format','bin/.local/state.json',
  'bin/.ssh/config','bin/secrets/production.json','bin/.npmrc',
  'bin/.netrc','bin/_netrc','bin/.pypirc','bin/.git-credentials',
  'bin/.env','bin/.env.production','bin/.ENV.local','bin/.env.example.local','bin/.env.example/state.json',
  'bin/server-access.json','bin/server-access.identity','bin/HOST-TOKEN.JSON',
  'bin/credentials.json','bin/settings.json','bin/login.json','bin/initial-account.json',
  'bin/host.dat','bin/vault.key','bin/broker.sqlite-wal','bin/capture.h264',
  'bin/capture.frames','bin/state.dpapi','bin/symbols.pdb','bin\.git\config'
)
$permitted=@(
  'bin/MultitapHost.exe','bin/flutter_windows.dll','bin/data/icudtl.dat',
  'bin/node_modules/.bin/tool','bin/.well-known/security.txt','bin/.gitignore',
  'bin/.gitattributes','bin/LICENSE','bin/.env.example','bin/.env.sample',
  'bin/.env.template','bin/.ENV.EXAMPLE','bin/settings.example.json','bin/server-access.example.json',
  'bin/certificates/public-ca.pem','bin/third-party/NOTICE','bin/secrets.js',
  'bin/gpu-h264-source.zip','bin/x264-b35605a-source.zip'
)
foreach($path in $blocked){Assert-Path (-not (& $actualPolicy $path)) ('reject '+$path)}
foreach($path in $permitted){Assert-Path ([bool](& $actualPolicy $path)) ('permit '+$path)}
@{kind='windows-payload-path-policy';baseline=[bool]$Baseline;checks=$script:checks;failures=$script:failures;payloadFilesRead=0;installerInvoked=$false;archiveWrites=0}|ConvertTo-Json -Compress
if($script:failures){exit 1}

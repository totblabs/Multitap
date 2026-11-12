param([string]$Output)
# What the client setup's failure view shows when ClientProcesses.ps1 refuses a package.
# ClientProcesses.ps1 is plain ASCII without a byte-order mark (Windows PowerShell 5.1 would
# misread Korean in it), so it throws fixed codes; Install-Client.ps1 (UTF-8 with a
# byte-order mark) holds the words, so no English sentence reaches the
# failure view. Nothing is installed, started or changed: one folder is created and removed
# in TEMP, and powershell.exe is only read as "some executable of another version".
$ErrorActionPreference='Stop'
$cases=0
function Assert-QA([bool]$condition,[string]$message){$script:cases++;if(-not $condition){throw "FAIL: $message"}}
$processes=Join-Path $PSScriptRoot '../ClientProcesses.ps1';$installer=Join-Path $PSScriptRoot '../Install-Client.ps1'

# The encodings the two files rely on.
$bytes=[IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $processes).Path)
Assert-QA (@($bytes|Where-Object{$_ -gt 127}).Count -eq 0) 'ClientProcesses.ps1 is plain ASCII'
Assert-QA (-not($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)) 'ClientProcesses.ps1 has no byte-order mark'
$bytes=[IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $installer).Path)
Assert-QA ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) 'Install-Client.ps1 keeps its byte-order mark'

# Every refusal of the package check is a fixed code.
. $processes
function Get-Refusal([scriptblock]$action){try{& $action;return ''}catch{return [string]$_.Exception.Message}}
$other=(Get-Command powershell.exe).Source
$folder=Join-Path ([IO.Path]::GetTempPath()) ('multitap-client-failure-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory -Path $folder|Out-Null
try{
  $codes=[ordered]@{
    client_version_invalid=(Get-Refusal {Assert-ClientExecutableVersion $other '1.2'})
    client_executable_unsafe=(Get-Refusal {Assert-ClientExecutableVersion $folder '1.0.43'})
    client_version_mismatch=(Get-Refusal {Assert-ClientExecutableVersion $other '0.0.1'})
  }
}finally{Remove-Item -LiteralPath $folder -Force -ErrorAction SilentlyContinue}
foreach($code in $codes.Keys){Assert-QA ($codes[$code] -ceq $code) "the refusal is the code $code (got '$($codes[$code])')"}
$errors=$null;$tokens=$null
$source=[Management.Automation.Language.Parser]::ParseFile((Resolve-Path -LiteralPath $processes).Path,[ref]$tokens,[ref]$errors)
$check=$source.Find({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Assert-ClientExecutableVersion'},$true)
$thrown=@($check.FindAll({param($node)$node -is [Management.Automation.Language.ThrowStatementAst]},$true)|ForEach-Object{$_.Pipeline.Extent.Text.Trim("'")})
Assert-QA ($thrown.Count -eq 3 -and @($thrown|Where-Object{$_ -cnotmatch '\A[a-z_]{1,64}\z'}).Count -eq 0) 'the package check throws codes only'
Assert-QA (@($thrown|Where-Object{-not $codes.Contains($_)}).Count -eq 0) 'every code it can throw is covered here'

# Install-Client.ps1 turns each code into words, and its failure view uses that.
$errors=$null
$script=[Management.Automation.Language.Parser]::ParseFile((Resolve-Path -LiteralPath $installer).Path,[ref]$tokens,[ref]$errors)
Assert-QA ($errors.Count -eq 0) 'Install-Client.ps1 parses'
$text=$script.Find({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-ClientFailureText'},$true)
Assert-QA ($null -ne $text) 'Install-Client.ps1 has the words for the codes'
. ([scriptblock]::Create($text.Extent.Text))
$hangul='['+[char]0xAC00+'-'+[char]0xD7A3+']'
foreach($code in $codes.Keys){
  $shown=Get-ClientFailureText $code 'ko'
  Assert-QA ($shown -cne $code -and $shown -match $hangul) "$code is shown in Korean"
  Assert-QA ($shown.EndsWith(" $code.")) "$code stays visible as the confirmation code"
  Assert-QA ($shown.Replace($code,'') -notmatch '[A-Za-z]{2,} [A-Za-z]{2,}') "$code is not shown as an English sentence"
  Assert-QA (($shown -split '(?<=\.) ').Count -le 3) "$code is at most two sentences and the code"
  # English (docs/i18n.md): words for the code, no Hangul, the code kept, as short as the Korean.
  $english=Get-ClientFailureText $code 'en'
  Assert-QA ($english -cne $code -and $english -notmatch $hangul) "$code is shown in English"
  Assert-QA ($english.EndsWith(" $code.")) "$code stays visible as the code in English"
  Assert-QA (($english -split '(?<=\.) ').Count -le 3) "$code is at most two sentences and the code in English"
}
foreach($language in 'ko','en'){Assert-QA ((Get-ClientFailureText 'payload_unavailable' $language) -ceq 'payload_unavailable' -and (Get-ClientFailureText '' $language) -ceq '') "any other message is shown as it is ($language)"}
# The default follows MULTITAP_LANG, which the setup sets for the script.
$saved=$env:MULTITAP_LANG
try{
  $env:MULTITAP_LANG='en';Assert-QA ((Get-ClientFailureText 'client_version_invalid') -notmatch $hangul) 'MULTITAP_LANG=en picks English'
  $env:MULTITAP_LANG='ko';Assert-QA ((Get-ClientFailureText 'client_version_invalid') -match $hangul) 'MULTITAP_LANG=ko picks Korean'
}finally{$env:MULTITAP_LANG=$saved}
$handler=$script.Find({param($node)$node -is [Management.Automation.Language.CatchClauseAst] -and $node.Extent.Text -match 'Report -1 \$failure'},$true)
Assert-QA ($null -ne $handler -and $handler.Extent.Text -match '\$failure=Get-ClientFailureText \$_\.Exception\.Message') 'the failure view shows the words, not the code alone'

$result=[ordered]@{kind='client-failure-text';cases=$cases;codes=@($codes.Keys);processExecuted=$false;installedFilesChanged=$false}
if($Output){$result|ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding UTF8}
$result|ConvertTo-Json -Compress

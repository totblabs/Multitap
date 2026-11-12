# Path-only admission. Public CA certificates and example files are product inputs;
# private workspace directories and credential/runtime basenames are not.
function Test-MultitapWindowsPayloadPath([string]$Path) {
  if([string]::IsNullOrWhiteSpace($Path)){return $false}
  $relative=$Path.Replace('\','/')
  $privateSegments=@('.git','.hg','.svn','.bzr','.local','.ssh','secrets')
  $credentialNames=@('.npmrc','.netrc','_netrc','.pypirc','.git-credentials')
  $environmentExamples=@('.env.example','.env.sample','.env.template')
  $segments=$relative.Split('/')
  for($i=0;$i -lt $segments.Length;$i++){
    $segment=$segments[$i]
    if($privateSegments -contains $segment -or $credentialNames -contains $segment){return $false}
    if(($segment -ieq '.env' -or $segment.StartsWith('.env.',[StringComparison]::OrdinalIgnoreCase)) -and ($i -ne $segments.Length-1 -or $environmentExamples -notcontains $segment)){return $false}
  }
  if($relative -match '(^|/)(settings\.json|login\.json|host\.dat|vault\.key|broker\.sqlite.*|initial-account\.json|host-token\.json|credentials\.json|server-access\.json|server-access\.identity)$|\.(h264|frames|dpapi|pdb)$'){return $false}
  return $true
}

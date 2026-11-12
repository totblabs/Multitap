param([string]$Destination)
# Downloads the pinned WinFsp MSI (apps/windows_host/third_party/winfsp/upstream.json) into
# .tools/winfsp and checks its SHA-256 and Authenticode signature. Developer tool: nothing is
# installed, extracted or run. The Host package carries this MSI as bin/winfsp/.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$pin = Get-Content -LiteralPath (Join-Path $projectRoot 'apps/windows_host/third_party/winfsp/upstream.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $Destination) { $Destination = Join-Path $projectRoot '.tools/winfsp' }
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
$path = Join-Path $Destination $pin.msi.name
function Test-Pinned { (Test-Path -LiteralPath $path) -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $pin.msi.sha256 -and (Get-Item -LiteralPath $path).Length -eq $pin.msi.size }
if (-not (Test-Pinned)) {
  Invoke-WebRequest -Uri $pin.msi.url -OutFile $path -UseBasicParsing -TimeoutSec 300
  if (-not (Test-Pinned)) { Remove-Item -LiteralPath $path -Force; throw ($pin.msi.name + ' does not match the pinned SHA-256/size.') }
}
$signature = Get-AuthenticodeSignature -LiteralPath $path
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $pin.signature.msi.thumbprint) { throw 'The WinFsp MSI signature is not the pinned valid signature.' }
Write-Host ('WinFsp ' + $pin.version + ' verified in ' + $Destination + ' (signer ' + ($signature.SignerCertificate.Subject -replace ',.*', '') + ')')

param([string]$Destination, [switch]$UsbDk)
# Downloads the pinned usbip-win2 driver package (apps/windows_host/third_party/usbip-win2/upstream.json)
# into .tools/usb-drivers/usbip-win2-<release> and checks every SHA-256; with -UsbDk also the pinned
# UsbDk installer (apps/windows_client/third_party/usbdk/upstream.json) into .tools/usb-drivers/usbdk-<release>.
# Developer tool: nothing is installed, extracted or run, and the Host / client builds bundle a folder
# only when every hash matches.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
function Get-Pinned([string]$Url, [string]$Path, [string]$Sha256) {
  if ((Test-Path -LiteralPath $Path) -and (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Sha256) { return }
  Invoke-WebRequest -Uri $Url -OutFile $Path -TimeoutSec 300
  if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Sha256) { Remove-Item -LiteralPath $Path -Force; throw ((Split-Path -Leaf $Path) + ' does not match the pinned SHA-256.') }
}
$pin = Get-Content -LiteralPath (Join-Path $projectRoot 'apps/windows_host/third_party/usbip-win2/upstream.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $Destination) { $Destination = Join-Path $projectRoot ('.tools/usb-drivers/usbip-win2-' + $pin.release) }
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
$base = $pin.source -replace '/tag/', '/download/'
foreach ($file in $pin.files) { Get-Pinned ($base + '/' + $file.name) (Join-Path $Destination $file.name) $file.sha256 }
Write-Host ('usbip-win2 ' + $pin.release + ' verified in ' + $Destination)
if ($UsbDk) {
  $usb = Get-Content -LiteralPath (Join-Path $projectRoot 'apps/windows_client/third_party/usbdk/upstream.json') -Raw -Encoding UTF8 | ConvertFrom-Json
  $usbDestination = Join-Path $projectRoot ('.tools/usb-drivers/usbdk-' + $usb.release)
  New-Item -ItemType Directory -Path $usbDestination -Force | Out-Null
  Get-Pinned $usb.file.url (Join-Path $usbDestination $usb.file.name) $usb.file.sha256
  Write-Host ('UsbDk ' + $usb.release + ' verified in ' + $usbDestination)
}
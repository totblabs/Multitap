param([string]$Product)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Product) { $Product = Join-Path $projectRoot '.artifacts/virtual-display/product-0.1.6' }
$Product = [IO.Path]::GetFullPath($Product)
$helper = Join-Path $Product 'MultitapDisplay.exe'
if (-not (Test-Path -LiteralPath $helper -PathType Leaf)) { throw 'Build the virtual display product before QA.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'Run this non-installing QA from a non-elevated shell. No helper mutation command has been executed.'
}
function Invoke-Helper([string]$Executable, [string]$Command, [int]$ExpectedExit = 0) {
  $start = New-Object Diagnostics.ProcessStartInfo
  $start.FileName = $Executable; $start.Arguments = $Command
  $start.WorkingDirectory = Split-Path -Parent $Executable
  $start.UseShellExecute = $false; $start.CreateNoWindow = $true
  $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
  $process = New-Object Diagnostics.Process
  $process.StartInfo = $start
  try {
    if (-not $process.Start()) { throw 'Helper process did not start.' }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(30000)) { $process.Kill(); throw ('Helper timeout: ' + $Command) }
    $text = $stdout.GetAwaiter().GetResult().Trim()
    if ($process.ExitCode -ne $ExpectedExit) { throw ('Unexpected helper exit for ' + $Command + ': ' + $text) }
    if ($stderr.GetAwaiter().GetResult().Trim()) { throw 'Unexpected helper stderr.' }
    return ($text | ConvertFrom-Json)
  } finally { $process.Dispose() }
}
function Registry-Snapshot([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path)) { return [ordered]@{ exists = $false } }
  $values = [ordered]@{}
  (Get-ItemProperty -LiteralPath $Path).PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | Sort-Object Name | ForEach-Object { $values[$_.Name] = $_.Value }
  return [ordered]@{ exists = $true; values = $values }
}
function Machine-Snapshot {
  $devices = @(Get-CimInstance -ClassName Win32_PnPEntity -Filter "PNPClass='Display' OR PNPClass='Monitor'" | Sort-Object PNPDeviceID | Select-Object PNPDeviceID, Name, Status, ConfigManagerErrorCode)
  $modes = @(Get-CimInstance -ClassName Win32_VideoController | Sort-Object PNPDeviceID | Select-Object PNPDeviceID, DriverVersion, CurrentHorizontalResolution, CurrentVerticalResolution, CurrentRefreshRate)
  # PnPUtil is read-only here. Snapshot the driver-store inventory as well as
  # the device tree, active modes and both registry locations the helper owns.
  $driverStore = & "$env:SystemRoot/System32/pnputil.exe" /enum-drivers /class Display 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw 'Cannot read the driver-store inventory for the no-change check.' }
  return ([ordered]@{ devices = $devices; modes = $modes; driverStore = $driverStore; owner = (Registry-Snapshot 'HKLM:\SOFTWARE\Multitap\Display'); vendor = (Registry-Snapshot 'HKLM:\SOFTWARE\MikeTheTech\VirtualDisplayDriver') } | ConvertTo-Json -Depth 8 -Compress)
}
$before = Machine-Snapshot
$verification = Invoke-Helper $helper verify
if (-not $verification.verified -or -not $verification.catalogMembershipVerified -or -not $verification.settingsHashVerified -or $verification.microsoftWhqlVerified) { throw 'Package verification did not report the required checks honestly.' }
$status = Invoke-Helper $helper status
if ($status.owned) { throw 'This non-installing refusal QA requires no owned virtual adapter; it never tests topology or resize on an installed owned device.' }
if ($null -eq $status.displays -or $status.displayCount -ne @($status.displays).Count -or $null -eq $status.foreignDeviceCount) { throw 'Status topology schema is incomplete.' }
if ($status.helperVersion -ne '0.1.5' -or $null -eq $status.diagnostics.adapters -or $null -eq $status.diagnostics.vendorPathConflict -or $status.diagnostics.vendorPathState -notin @('unset','unowned','owned','different','ownership_unknown','unavailable') -or $null -eq $status.configurationPending -or $null -eq $status.rebootRequired) { throw 'Status diagnostic schema is incomplete.' }
if ($status.rebootRequired -and $status.rebootStatusSource -ne 'device_status') { throw 'A reboot was asserted without an observed owned-device status signal.' }
foreach ($adapter in @($status.diagnostics.adapters)) {
  if ($null -eq $adapter.statusAvailable -or $null -eq $adapter.problemCode -or $null -eq $adapter.owned -or $null -eq $adapter.matchingHardware) { throw 'Adapter diagnostics lack safe device state.' }
  if ($adapter.owned -and -not $adapter.matchingHardware) { throw 'A device was treated as owned without matching its driver hardware.' }
}
foreach ($display in @($status.displays)) {
  if (-not $display.virtual -or $display.displayDeviceName -notmatch '^\\\\\.\\DISPLAY[0-9]+$' -or $display.width -le 0 -or $display.height -le 0) { throw 'Status contains a malformed active source.' }
}
$warp = Invoke-Helper $helper warp-test
if (-not $warp.warpDeviceCreated -or $warp.virtualDisplayRuntimeTest) { throw 'WARP and virtual-driver runtime evidence were conflated.' }
$layout = Invoke-Helper $helper plan-test
if (-not $layout.layoutTestsPassed -or -not $layout.ownershipTestsPassed -or -not $layout.diagnosticTestsPassed -or $layout.operatingSystemChanged -or $layout.virtualDisplayRuntimeTest) { throw 'Pure display layout, ancestry or diagnostic tests failed.' }
$topology = Invoke-Helper $helper topology-test
if (-not $topology.topologyPlanTestsPassed -or $topology.operatingSystemChanged -or $topology.virtualDisplayRuntimeTest) { throw 'Pure owned topology tests failed.' }
$viewport = Invoke-Helper $helper viewport-test
if (-not $viewport.viewportPlanTestsPassed -or -not $viewport.xmlPreservationTest -or -not $viewport.journalRoundtripTest -or $viewport.operatingSystemChanged -or $viewport.virtualDisplayRuntimeTest) { throw 'Pure viewport XML/layout/journal tests failed.' }
$viewportStatus = Invoke-Helper $helper viewport-status 1
if ($viewportStatus.error -ne 'no_exclusive_owned_virtual_display') { throw 'Viewport support was advertised without an owned device.' }
foreach ($command in @('viewport "\\.\DISPLAY999999" 1584 884 60', 'viewport-recover')) {
  if ((Invoke-Helper $helper $command 1).error -ne 'administrator_required') { throw 'Viewport mutations were not refused at their privilege gate.' }
}
$invalidTopology = Invoke-Helper $helper 'topology 3' 1
if ($invalidTopology.error -ne 'invalid_topology_arguments') { throw 'An invalid monitor count was not rejected before display operations.' }
$unownedTopologyStatus = Invoke-Helper $helper 'topology-status' 1
if ($unownedTopologyStatus.error -ne 'no_exclusive_owned_virtual_display') { throw 'Topology support was advertised without an owned device.' }
foreach ($count in 1,2) {
  $unownedTopology = Invoke-Helper $helper ('topology ' + $count) 1
  if ($unownedTopology.error -notin @('no_exclusive_owned_virtual_display','interactive_console_required','interactive_desktop_locked','reboot_required','owned_virtual_sources_unavailable')) { throw 'An unowned topology change was not safely refused.' }
}
$refused = @()
foreach ($command in @('install', 'configure', 'name', 'remove')) {
  $result = Invoke-Helper $helper $command 1
  if ($result.error -ne 'administrator_required') { throw ('Mutation was not rejected at its initial privilege gate: ' + $command) }
  $refused += $command
}
$resizeInvalid = Invoke-Helper $helper 'resize "\\.\DISPLAY999999" 1281 720 60' 1
if ($resizeInvalid.error -ne 'invalid_resize_arguments') { throw 'Odd resize dimensions were not rejected before any display operation.' }
$resizeUnowned = Invoke-Helper $helper 'resize "\\.\DISPLAY999999" 1280 720 60' 1
if ($resizeUnowned.error -notin @('no_exclusive_owned_virtual_display','interactive_console_required','monitor_not_available')) { throw 'An unowned or nonexistent display resize was not safely refused.' }
$qaRoot = [IO.Path]::GetFullPath((Join-Path $projectRoot '.artifacts/virtual-display'))
$fixture = Join-Path $qaRoot ('qa-' + [guid]::NewGuid().ToString('N'))
if (-not $fixture.StartsWith($qaRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid QA fixture boundary.' }
New-Item -ItemType Directory -Path $fixture | Out-Null
$tamperChecks = @()
try {
  Get-ChildItem -LiteralPath $Product -File | Copy-Item -Destination $fixture
  $fixtureDriver = Join-Path $fixture 'driver'
  New-Item -ItemType Directory -Path $fixtureDriver | Out-Null
  Get-ChildItem -LiteralPath (Join-Path $Product 'driver') -File | Copy-Item -Destination $fixtureDriver
  $fixtureHelper = Join-Path $fixture 'MultitapDisplay.exe'
  foreach ($name in @('MttVDD.dll', 'MttVDD.inf', 'mttvdd.cat', 'vdd_settings.xml')) {
    $file = Join-Path $fixtureDriver $name
    $original = [IO.File]::ReadAllBytes($file)
    try {
      $altered = [byte[]]$original.Clone(); $altered[$altered.Length - 1] = $altered[$altered.Length - 1] -bxor 1
      [IO.File]::WriteAllBytes($file, $altered)
      # A forged adjacent manifest cannot replace the helper's embedded pins.
      $manifest = Get-Content -LiteralPath (Join-Path $fixtureDriver 'upstream.json') -Raw | ConvertFrom-Json
      if ($name -ne 'vdd_settings.xml') { $manifest.files.$name = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
      $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $fixtureDriver 'upstream.json') -Encoding UTF8
      $result = Invoke-Helper $fixtureHelper verify 1
      if ($result.error -ne 'driver_hash_mismatch') { throw ('Tampered package was not refused: ' + $name) }
      $tamperChecks += $name
    } finally { [IO.File]::WriteAllBytes($file, $original) }
  }
  $missing = Join-Path $fixtureDriver 'MttVDD.dll'
  Remove-Item -LiteralPath $missing
  if ((Invoke-Helper $fixtureHelper verify 1).error -ne 'driver_file_unavailable') { throw 'A missing driver file was not refused.' }
} finally {
  $resolved = [IO.Path]::GetFullPath($fixture)
  if (-not $resolved.StartsWith($qaRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or (Split-Path -Leaf $resolved) -notmatch '^qa-[0-9a-f]{32}$') { throw 'Refusing unsafe QA fixture cleanup.' }
  Remove-Item -LiteralPath $resolved -Recurse -Force
}
$after = Machine-Snapshot
if ($before -cne $after) { throw 'Machine device, registry, mode or driver-store state changed during non-installing QA.' }
[ordered]@{ helperVersion = $status.helperVersion; packageVerified = $true; catalogMembershipVerified = $true; nativeStatusRead = $true; diagnosticsSchemaChecked = $true; installed = $status.installed; activeDisplayCount = $status.displayCount; sessionId = $status.sessionId; remoteSession = $status.remoteSession; warpDeviceCreated = $true; virtualDisplayRuntimeTest = $false; layoutTestsPassed = $true; ownershipTestsPassed = $true; diagnosticTestsPassed = $true; topologyPlanTestsPassed = $true; viewportPlanTestsPassed = $true; viewportXmlPreservationPassed = $true; viewportJournalRoundtripPassed = $true; viewportPrivilegeRefused = $true; actualViewportChangeTest = $false; invalidTopologyRejected = $true; unownedTopologyRejected = $true; actualTopologyChangeTest = $false; actualDeviceNamingTest = $false; privilegeRefusals = $refused; invalidResizeRejected = $true; unownedResizeRejected = $true; tamperedFilesRejected = $tamperChecks; missingFileRejected = $true; machineStateUnchanged = $true; driverInstalledByTest = $false } | ConvertTo-Json -Depth 5

# Checks a Multitap Host install on THIS Windows after the installer and the
# "설치 마무리" step of Host settings ran. Read only: nothing is installed,
# trusted, started, stopped or changed; the one file it writes is its own JSON
# result. No administrator rights are needed.
#   powershell -NoProfile -ExecutionPolicy Bypass -File host-fresh-install-check.ps1 [-Out <result.json>]
# Prints a short summary and exits 0 when every required item is in place, else 1.
# What a working Host needs besides its own files (apps/windows_host/src/host_prerequisites.hpp):
#   the Host service, the pinned virtual display driver package, its one certificate in
#   LocalMachine Root and TrustedPublisher, one device node, the provider selection
#   (ProgramData\Multitap\Host\native-display.json) and WinFsp.
param([string]$Out)
$ErrorActionPreference = 'Stop'
if (-not $Out) { $Out = Join-Path $PSScriptRoot 'host-fresh-install-check.json' }

# Pins (setup_pnp.hpp pins(), setup_winfsp.hpp).
$thumbprint = '37E100B2B382866FA7CE04EE33FE53DF53AEDC6D'
$driverPins = [ordered]@{
  'SunshineVirtualDisplayDriver.inf' = 'c5e5abf485e35d5eff4bf8696d50f6ceb4d42f7c09e3562eeba53557b510d32a'
  'SunshineVirtualDisplayDriver.cat' = 'a310627e2b34bfb2a54c2fc7a697391e410d4ad6def52ab23a0e75648cd30c22'
  'SunshineVirtualDisplayDriver.dll' = 'c4238424b5ce2786b52a7d7e4e1cb6e366f91ea972835f275654955364a0a1fd'
  'SunshineVirtualDisplayDriver.cer' = 'a42a04d241438ac8bd9107bc302e11b1232796c9536bf45d99717dee256746a1'
}
$infPins = @{ 'c5e5abf485e35d5eff4bf8696d50f6ceb4d42f7c09e3562eeba53557b510d32a' = '1.6.5'; '3212b43e7dc20cf36046421e8fd57910d0bb5fc5e5c67c920bbfa24d6f10f95c' = '1.6.4' }
$winfspMsi = 'winfsp-2.1.25156.msi'; $winfspSha = '073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a'

$checks = New-Object System.Collections.Generic.List[object]
# state: ok | missing | attention | info. `required` items decide the exit code.
function Add-Check([string]$id, [string]$title, [string]$state, [string]$text, [bool]$required = $true, $data = $null) {
  $checks.Add([ordered]@{ id = $id; title = $title; state = $state; required = $required; text = $text; data = $data })
}
function Sha([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }

# 1. Host service.
$bin = $null; $hostVersion = ''
$service = Get-CimInstance Win32_Service -Filter "Name='MultitapHost'" -ErrorAction SilentlyContinue
if (-not $service) { Add-Check 'service' 'Host 서비스' 'missing' 'Host 서비스가 없어요. Host 설치 파일을 실행하세요.' }
else {
  $image = if ($service.PathName -match '^\s*"([^"]+)"') { $Matches[1] } else { ($service.PathName -split '\s+--')[0].Trim() }
  if (Test-Path -LiteralPath $image -PathType Leaf) { $bin = Split-Path -Parent $image; $hostVersion = (Get-Item -LiteralPath $image).VersionInfo.ProductVersion }
  $running = $service.State -eq 'Running'
  Add-Check 'service' 'Host 서비스' $(if ($running -and $bin) { 'ok' } else { 'attention' }) $(if ($running) { "실행 중 · $hostVersion" } else { "실행 중이 아니에요 ($($service.State))." }) $true @{ state = $service.State; startMode = $service.StartMode; version = $hostVersion; image = $image }
}

# 2. The driver package and the WinFsp installer this Host carries.
if ($bin) {
  $driver = Join-Path $bin 'native-display-driver'; $bad = @()
  foreach ($name in $driverPins.Keys) { $file = Join-Path $driver $name; if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { $bad += "$name 없음" } elseif ((Sha $file) -ne $driverPins[$name]) { $bad += "$name 다름" } }
  Add-Check 'driverPackage' '드라이버 파일' $(if ($bad.Count) { 'attention' } else { 'ok' }) $(if ($bad.Count) { "원본과 달라요: $($bad -join ', '). Host 설치 파일을 다시 받아 실행하세요." } else { '원본 그대로예요 (1.6.5).' }) $true @{ directory = $driver; problems = $bad }
  $msi = Join-Path (Join-Path $bin 'winfsp') $winfspMsi
  $msiOk = (Test-Path -LiteralPath $msi -PathType Leaf) -and ((Sha $msi) -eq $winfspSha)
  Add-Check 'winfspPackage' 'WinFsp 설치 파일' $(if ($msiOk) { 'ok' } else { 'attention' }) $(if ($msiOk) { '원본 그대로예요.' } else { 'Host에 들어 있는 WinFsp 설치 파일을 확인하지 못했어요.' }) $false @{ path = $msi }
  $settings = Join-Path $bin 'MultitapHostSettings.exe'
  Add-Check 'hostSettings' 'Host 설정 앱' $(if (Test-Path -LiteralPath $settings -PathType Leaf) { 'ok' } else { 'missing' }) $(if (Test-Path -LiteralPath $settings -PathType Leaf) { "있음 · $((Get-Item -LiteralPath $settings).VersionInfo.ProductVersion)" } else { '없어요.' }) $true $null
}

# 2b. Who may change the program folders (told only). A Host first installed from the Windows
# Installer package has Program Files' inherited list, which includes TrustedInstaller;
# Multitap\Host may instead carry its own protected list. A Host older than the version named
# below refuses the first kind when Host settings read the server connection or restart the service.
if ($bin) {
  $writers = @{}; $write = 0x2 -bor 0x4 -bor 0x10 -bor 0x100 -bor 0x10000 -bor 0x40000 -bor 0x80000
  try {
    foreach ($folder in $bin, (Split-Path -Parent $bin)) {
      foreach ($rule in (Get-Acl -LiteralPath $folder).Access) {
        if ($rule.AccessControlType -ne 'Allow' -or ($rule.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly) -or -not ([int]$rule.FileSystemRights -band $write)) { continue }
        $sid = try { $rule.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value } catch { "$($rule.IdentityReference)" }
        $writers[$sid] = "$($rule.IdentityReference)"
      }
    }
    $servicing = 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464'
    $others = @($writers.Keys | Where-Object { $_ -notin 'S-1-5-18', 'S-1-5-32-544', $servicing } | ForEach-Object { $writers[$_] })
    $kind = if ($others.Count) { 'other' } elseif ($writers.ContainsKey($servicing)) { 'windows-installer' } else { 'multitap' }
    $text = if ($others.Count) { "관리자가 아닌 계정도 바꿀 수 있어요: $($others -join ', ')." } elseif ($kind -eq 'windows-installer') { 'Windows Installer 기본 권한이에요 (TrustedInstaller 포함).' } else { 'Multitap 전용 보호 권한이에요.' }
    Add-Check 'installFolders' '프로그램 폴더 권한' $(if ($others.Count) { 'attention' } else { 'info' }) $text $false @{ kind = $kind; writers = @($writers.Keys) }
    # One plain line: does Host settings read this Host's state correctly? With Windows Installer's
    # default permissions it does only from 1.0.46 (an older one shows 서버 연결 as "확인 필요" on a connected
    # Host and cannot restart the service from Host settings).
    $needsUpdate = $kind -eq 'windows-installer' -and $hostVersion -match '^(\d+)\.(\d+)\.(\d+)' -and ([version]"$($Matches[1]).$($Matches[2]).$($Matches[3])" -lt [version]'1.0.46')
    Add-Check 'statusReading' 'Host 설정의 상태 표시' $(if ($needsUpdate) { 'attention' } else { 'ok' }) $(if ($needsUpdate) { "서버 연결 상태가 틀리게 보여요. Host 1.0.46 이상으로 업데이트하세요 (지금 $hostVersion)." } else { '바르게 보여요.' }) $false @{ needsHost = $(if ($needsUpdate) { '1.0.46' } else { '' }) }
  } catch { Add-Check 'installFolders' '프로그램 폴더 권한' 'info' '읽지 못했어요.' $false $null }
}

# 3. The one certificate, in exactly the two stores.
$stores = [ordered]@{}
foreach ($store in 'Root', 'TrustedPublisher') { $stores[$store] = Test-Path -LiteralPath "Cert:\LocalMachine\$store\$thumbprint" }
$trusted = $stores['Root'] -and $stores['TrustedPublisher']
$where = (@($stores.Keys | Where-Object { $stores[$_] }) -join ', ')
Add-Check 'certificate' '드라이버 서명 인증서' $(if ($trusted) { 'ok' } else { 'missing' }) $(if ($trusted) { 'Root, TrustedPublisher에 있어요.' } elseif ($where) { "$where 에만 있어요. Host 설정에서 설치 마무리를 누르세요." } else { '아직 신뢰하지 않았어요. Host 설정에서 설치 마무리를 누르세요.' }) $true @{ thumbprint = $thumbprint; root = $stores['Root']; trustedPublisher = $stores['TrustedPublisher'] }

# 4. The provider selection.
$instance = $null
$selectionPath = Join-Path $env:ProgramData 'Multitap\Host\native-display.json'
if (-not (Test-Path -LiteralPath $selectionPath -PathType Leaf)) { Add-Check 'selection' '가상 화면 선택' 'missing' '아직 없어요. Host 설정에서 설치 마무리를 누르세요.' }
else {
  try {
    $selection = [IO.File]::ReadAllText($selectionPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
    $valid = $selection.enabled -eq $true -and $selection.provider -ceq 'libvirtualdisplay-1.6.4' -and $selection.deviceInstanceId -is [string] -and $selection.deviceInstanceId -cmatch '\AROOT\\DISPLAY\\[0-9A-Fa-f]{4,8}\z'
    if ($valid) { $instance = $selection.deviceInstanceId }
    Add-Check 'selection' '가상 화면 선택' $(if ($valid) { 'ok' } else { 'attention' }) $(if ($valid) { "Multitap 가상 화면 · $instance" } else { '내용을 확인해야 해요.' }) $true @{ provider = $selection.provider; enabled = $selection.enabled; deviceInstanceId = $selection.deviceInstanceId; driverInfSha256 = $selection.driverInfSha256; monitors = $selection.monitors }
  } catch { Add-Check 'selection' '가상 화면 선택' 'attention' '파일을 읽지 못했어요.' }
}

# 5. The device node the selection names, and every node with this driver.
$deviceRestart = $false
$nodes = @(Get-PnpDevice -Class Display -ErrorAction SilentlyContinue | Where-Object { @($_.HardwareID) -contains 'Root\SunshineVirtualDisplay' })
if (-not $instance) { Add-Check 'device' '가상 화면 장치' 'missing' $(if ($nodes.Count) { "선택되지 않은 장치가 $($nodes.Count)개 있어요." } else { '아직 없어요. Host 설정에서 설치 마무리를 누르세요.' }) $true @{ nodes = @($nodes | ForEach-Object { $_.InstanceId }) } }
else {
  $device = Get-PnpDevice -InstanceId $instance -ErrorAction SilentlyContinue
  if (-not $device) { Add-Check 'device' '가상 화면 장치' 'missing' '선택된 장치가 Windows에 없어요. Host 설정에서 설치 마무리를 누르세요.' $true @{ instance = $instance } }
  else {
    $inf = ''; $infVersion = ''
    try { $inf = (Get-PnpDeviceProperty -InstanceId $instance -KeyName 'DEVPKEY_Device_DriverInfPath' -ErrorAction Stop).Data } catch { }
    if ($inf -match '\Aoem[0-9]+\.inf\z') { $published = Join-Path (Join-Path $env:WINDIR 'INF') $inf; if (Test-Path -LiteralPath $published) { $infVersion = $infPins[(Sha $published)] } }
    $deviceRestart = "$($device.Problem)" -eq 'CM_PROB_NEED_RESTART'
    $started = $device.Present -and $device.Status -eq 'OK'
    $state = if ($deviceRestart) { 'attention' } elseif ($started -and $infVersion) { 'ok' } else { 'attention' }
    $text = if ($deviceRestart) { 'Windows를 다시 시작해야 시작돼요.' } elseif (-not $started) { "시작되지 않았어요 ($($device.Status), $($device.Problem))." } elseif (-not $infVersion) { '다른 드라이버 파일이 연결돼 있어요.' } else { "시작됨 · 드라이버 $infVersion" }
    Add-Check 'device' '가상 화면 장치' $state $text $true @{ instance = $instance; present = $device.Present; status = "$($device.Status)"; problem = "$($device.Problem)"; friendlyName = $device.FriendlyName; publishedInf = $inf; driverVersion = $infVersion; nodes = @($nodes | ForEach-Object { $_.InstanceId }) }
  }
}

# 6. WinFsp.
$winfspDir = $null
try { $winfspDir = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\WOW6432Node\WinFsp' -Name InstallDir -ErrorAction Stop).InstallDir } catch { }
$winfspDll = if ($winfspDir) { Join-Path $winfspDir 'bin\winfsp-x64.dll' } else { $null }
if ($winfspDll -and (Test-Path -LiteralPath $winfspDll -PathType Leaf)) { $v = (Get-Item -LiteralPath $winfspDll).VersionInfo; Add-Check 'winfsp' '공유 폴더 구성 요소(WinFsp)' 'ok' "설치됨 · $($v.FileMajorPart).$($v.FileMinorPart).$($v.FileBuildPart)" $true @{ installDir = $winfspDir } }
else { Add-Check 'winfsp' '공유 폴더 구성 요소(WinFsp)' 'missing' '아직 없어요. Host 설정에서 설치 마무리를 누르세요.' }

# 7. A restart Windows is waiting for (told, never performed).
$pending = @()
if ($deviceRestart) { $pending += '가상 화면 장치' }
if (Test-Path -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending') { $pending += 'Windows 구성 요소' }
if (Test-Path -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired') { $pending += 'Windows 업데이트' }
try { if ((Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' -Name PendingFileRenameOperations -ErrorAction Stop).PendingFileRenameOperations) { $pending += '파일 교체' } } catch { }
Add-Check 'restart' 'Windows 재시작' $(if ($deviceRestart) { 'attention' } elseif ($pending.Count) { 'info' } else { 'ok' }) $(if ($pending.Count) { "기다리는 재시작이 있어요: $($pending -join ', ')." } else { '기다리는 재시작이 없어요.' }) $deviceRestart @{ reasons = $pending }

# 8. Registration (told only; registering is the administrator's next step).
$registered = $false
try { $registered = [bool]([IO.File]::ReadAllText((Join-Path $env:ProgramData 'Multitap\Host\settings.json'), [Text.Encoding]::UTF8) | ConvertFrom-Json).desktopId } catch { }
Add-Check 'registration' '서버 등록' 'info' $(if ($registered) { '등록됨' } else { '아직 등록하지 않았어요. Host 설정에서 서버에 연결하세요.' }) $false @{ registered = $registered }

$open = @($checks | Where-Object { $_.required -and $_.state -ne 'ok' })
$result = [ordered]@{
  schema = 1; kind = 'multitap-host-fresh-install-check'; computer = $env:COMPUTERNAME; time = (Get-Date).ToString('o')
  windows = (Get-CimInstance Win32_OperatingSystem).Caption; hostVersion = $hostVersion; ok = ($open.Count -eq 0); checks = $checks
}
try { [IO.File]::WriteAllText($Out, ($result | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding $false)) }
catch { $Out = Join-Path $env:TEMP 'host-fresh-install-check.json'; [IO.File]::WriteAllText($Out, ($result | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding $false)) }

$labels = @{ ok = '정상'; missing = '없음'; attention = '확인'; info = '참고' }
"Multitap Host 설치 확인 · $($env:COMPUTERNAME) · $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
foreach ($check in $checks) { "[{0}] {1}: {2}" -f $labels[$check.state], $check.title, $check.text }
if ($open.Count -eq 0) { '결과: 모두 정상이에요.' } else { "결과: $($open.Count)개 항목을 확인해야 해요." }
"기록: $Out"
exit $(if ($open.Count -eq 0) { 0 } else { 1 })

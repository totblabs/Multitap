param([Parameter(Mandatory=$true)][string]$HostTool)
$ErrorActionPreference='Stop'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(-not(New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){Write-Output '{"kind":"real-scm-lifecycle","tested":false,"reason":"administrator_token_required"}';exit 3}
$absolute=(Resolve-Path -LiteralPath $HostTool).Path
if([IO.Path]::GetFileName($absolute) -ne 'MultitapHostTool.exe'){throw 'Unexpected QA service executable.'}
$name='MultitapHostQA_'+$PID
if(Get-Service -Name $name -ErrorAction SilentlyContinue){throw 'QA service name is already owned.'}
$beforePort=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\RDP-Tcp').PortNumber
$beforeNla=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\RDP-Tcp').UserAuthentication
$created=$false
try{
  New-Service -Name $name -DisplayName $name -BinaryPathName ('"'+$absolute+'" --service --name '+$name+' --qa') -StartupType Manual | Out-Null;$created=$true
  Start-Service -Name $name;(Get-Service $name).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Running,[TimeSpan]::FromSeconds(15))
  Start-Sleep -Milliseconds 1200
  Stop-Service -Name $name;(Get-Service $name).WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped,[TimeSpan]::FromSeconds(20))
  $after=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\RDP-Tcp'
  if($after.PortNumber -ne $beforePort -or $after.UserAuthentication -ne $beforeNla){throw 'Windows RDP baseline changed during isolated QA.'}
  Write-Output '{"kind":"real-scm-lifecycle","tested":true,"started":true,"stopped":true,"rdpUnchanged":true,"brokerConnected":false,"captureTested":false}'
}finally{if($created){$service=Get-Service -Name $name -ErrorAction SilentlyContinue;if($service.Status -ne 'Stopped'){Stop-Service -Name $name -ErrorAction Stop;$service.WaitForStatus([ServiceProcess.ServiceControllerStatus]::Stopped,[TimeSpan]::FromSeconds(20))};& sc.exe delete $name | Out-Null;if($LASTEXITCODE -ne 0){throw 'Independent QA service removal failed.'}}}

param(
  # A built Host package (package.msi) to check. Without it a small package shaped like
  # the authored one is created in a temporary folder and run through simplify-msi.vbs.
  [string]$Package,
  [string]$Simplify = (Join-Path $PSScriptRoot '..\..\..\tools\host-installer\simplify-msi.vbs'),
  [string]$Dump = (Join-Path $PSScriptRoot '..\..\..\tools\host-installer\msi-dump.vbs'),
  [string]$Icon = (Join-Path $PSScriptRoot '..\resources\host.ico')
)
# host-removal-v1 (apps/windows_host/src/host_removal.hpp): the one custom action of the
# Host package, MultitapHostRemovalCleanup.
#
# The dangerous part is an UPDATE. An update is a Windows Installer major upgrade: the new
# package removes the old product (RemoveExistingProducts) and that nested removal has
# REMOVE=ALL too. If the cleanup ran there, every update would take the virtual display
# driver away. This test proves, with Windows Installer's own condition evaluator and
# formatter on the package itself (a package session that installs nothing:
# Installer.OpenPackage with "ignore machine state", no action is run), that
#   - the action is scheduled for a real removal and for nothing else (not for the removal
#     that belongs to an upgrade, a first install, an install over an older version, a repair);
#   - the command line it would get says the same, in exactly the texts
#     MultitapHostRemovalTest (host_removal_test.cpp) feeds to the tool's own check;
#   - it is the package's only custom action, runs an installed file of the package as
#     SYSTEM inside the transaction, after the service is deleted and before the files go,
#     and its exit code is ignored, so it can never fail an install, update or removal.
# Nothing is installed, removed or registered; only files in a temporary folder are written.
$ErrorActionPreference = 'Stop'
$cases = 0
function Assert-QA([bool]$condition, [string]$message) { $script:cases++; if (-not $condition) { throw "FAIL: $message" } }
$work = Join-Path ([IO.Path]::GetTempPath()) ('multitap-removal-action-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
  $Simplify = (Resolve-Path -LiteralPath $Simplify).Path; $Dump = (Resolve-Path -LiteralPath $Dump).Path
  $fixture = -not $Package
  $msi = Join-Path $work 'package.msi'
  if ($fixture) {
    # The tables simplify-msi.vbs reads and writes, with the authored package's keys: the
    # seven gate actions and their DLL, the service component, the two programs.
    $create = Join-Path $work 'create.vbs'
    [IO.File]::WriteAllText($create, @'
Option Explicit
Dim wi, db, v, r, si
Set wi = CreateObject("WindowsInstaller.Installer")
Set db = wi.OpenDatabase(WScript.Arguments(0), 3)
Sub Run(sql)
  Set v = db.OpenView(sql) : v.Execute : v.Close
End Sub
Run "CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` LONGCHAR NOT NULL LOCALIZABLE PRIMARY KEY `Property`)"
Run "CREATE TABLE `Directory` (`Directory` CHAR(72) NOT NULL, `Directory_Parent` CHAR(72), `DefaultDir` CHAR(255) NOT NULL LOCALIZABLE PRIMARY KEY `Directory`)"
Run "CREATE TABLE `Component` (`Component` CHAR(72) NOT NULL, `ComponentId` CHAR(38), `Directory_` CHAR(72) NOT NULL, `Attributes` SHORT NOT NULL, `Condition` CHAR(255), `KeyPath` CHAR(72) PRIMARY KEY `Component`)"
Run "CREATE TABLE `File` (`File` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL, `FileName` CHAR(255) NOT NULL LOCALIZABLE, `FileSize` LONG NOT NULL, `Version` CHAR(72), `Language` CHAR(20), `Attributes` SHORT, `Sequence` LONG NOT NULL PRIMARY KEY `File`)"
Run "CREATE TABLE `Feature` (`Feature` CHAR(38) NOT NULL, `Feature_Parent` CHAR(38), `Title` CHAR(64) LOCALIZABLE, `Description` CHAR(255) LOCALIZABLE, `Display` SHORT, `Level` SHORT NOT NULL, `Directory_` CHAR(72), `Attributes` SHORT NOT NULL PRIMARY KEY `Feature`)"
Run "CREATE TABLE `FeatureComponents` (`Feature_` CHAR(38) NOT NULL, `Component_` CHAR(72) NOT NULL PRIMARY KEY `Feature_`, `Component_`)"
Run "CREATE TABLE `Media` (`DiskId` SHORT NOT NULL, `LastSequence` LONG NOT NULL, `DiskPrompt` CHAR(64) LOCALIZABLE, `Cabinet` CHAR(255), `VolumeLabel` CHAR(32), `Source` CHAR(72) PRIMARY KEY `DiskId`)"
Run "CREATE TABLE `Shortcut` (`Shortcut` CHAR(72) NOT NULL, `Directory_` CHAR(72) NOT NULL, `Name` CHAR(128) NOT NULL LOCALIZABLE, `Component_` CHAR(72) NOT NULL, `Target` CHAR(72) NOT NULL, `Arguments` CHAR(255), `Description` CHAR(255) LOCALIZABLE, `Hotkey` SHORT, `Icon_` CHAR(72), `IconIndex` SHORT, `ShowCmd` SHORT, `WkDir` CHAR(72) PRIMARY KEY `Shortcut`)"
Run "CREATE TABLE `Upgrade` (`UpgradeCode` CHAR(38) NOT NULL, `VersionMin` CHAR(20), `VersionMax` CHAR(20), `Language` CHAR(255), `Attributes` LONG NOT NULL, `Remove` CHAR(255), `ActionProperty` CHAR(72) NOT NULL PRIMARY KEY `UpgradeCode`, `VersionMin`, `VersionMax`, `Language`, `Attributes`)"
Run "CREATE TABLE `InstallExecuteSequence` (`Action` CHAR(72) NOT NULL, `Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)"
Run "CREATE TABLE `InstallUISequence` (`Action` CHAR(72) NOT NULL, `Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)"
Run "CREATE TABLE `CustomAction` (`Action` CHAR(72) NOT NULL, `Type` SHORT NOT NULL, `Source` CHAR(72), `Target` CHAR(255), `ExtendedType` LONG PRIMARY KEY `Action`)"
Run "CREATE TABLE `Binary` (`Name` CHAR(72) NOT NULL, `Data` OBJECT NOT NULL PRIMARY KEY `Name`)"
Run "INSERT INTO `Property` (`Property`, `Value`) VALUES ('ProductCode', '{1AF1997D-C06F-507B-ADC6-31E8D7B1F53B}')"
Run "INSERT INTO `Property` (`Property`, `Value`) VALUES ('ProductName', 'Multitap Host')"
Run "INSERT INTO `Property` (`Property`, `Value`) VALUES ('ProductVersion', '1.0.52')"
Run "INSERT INTO `Property` (`Property`, `Value`) VALUES ('ProductLanguage', '1042')"
Run "INSERT INTO `Property` (`Property`, `Value`) VALUES ('Manufacturer', 'Multitap')"
Run "INSERT INTO `Property` (`Property`, `Value`) VALUES ('UpgradeCode', '{2E30742F-D372-57A6-B6C2-26D3AC872973}')"
Run "INSERT INTO `Property` (`Property`, `Value`) VALUES ('ALLUSERS', '1')"
Run "INSERT INTO `Property` (`Property`, `Value`) VALUES ('SecureCustomProperties', 'MT_UPGRADE_PRODUCTS;MT_NEWER_PRODUCTS')"
Run "INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('TARGETDIR', '', 'SourceDir')"
Run "INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('ProgramFiles64Folder', 'TARGETDIR', '.')"
Run "INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('MULTITAPROOT', 'ProgramFiles64Folder', 'Multitap')"
Run "INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('HOSTROOT', 'MULTITAPROOT', 'Host')"
Run "INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('D_ceba428ba8eb987f385fab5d7a3a', 'HOSTROOT', 'bin')"
Run "INSERT INTO `Component` (`Component`, `ComponentId`, `Directory_`, `Attributes`, `KeyPath`) VALUES ('C_07c652e489165f50893604d567b4', '{11111111-1111-4111-8111-111111111111}', 'D_ceba428ba8eb987f385fab5d7a3a', 256, 'F_07c652e489165f50893604d567b4')"
Run "INSERT INTO `Component` (`Component`, `ComponentId`, `Directory_`, `Attributes`, `KeyPath`) VALUES ('C_e7e3b467a16ee72c1ef09d1fd039', '{22222222-2222-4222-8222-222222222222}', 'D_ceba428ba8eb987f385fab5d7a3a', 256, 'F_e7e3b467a16ee72c1ef09d1fd039')"
Run "INSERT INTO `File` (`File`, `Component_`, `FileName`, `FileSize`, `Sequence`) VALUES ('F_07c652e489165f50893604d567b4', 'C_07c652e489165f50893604d567b4', 'MC67AE44|MultitapHost.exe', 1, 1)"
Run "INSERT INTO `File` (`File`, `Component_`, `FileName`, `FileSize`, `Sequence`) VALUES ('F_e7e3b467a16ee72c1ef09d1fd039', 'C_e7e3b467a16ee72c1ef09d1fd039', 'M6523F7E|MultitapHostTool.exe', 1, 2)"
Run "INSERT INTO `Feature` (`Feature`, `Title`, `Display`, `Level`, `Directory_`, `Attributes`) VALUES ('HostFeature', 'Multitap Host', 1, 1, 'HOSTROOT', 0)"
Run "INSERT INTO `FeatureComponents` (`Feature_`, `Component_`) VALUES ('HostFeature', 'C_07c652e489165f50893604d567b4')"
Run "INSERT INTO `FeatureComponents` (`Feature_`, `Component_`) VALUES ('HostFeature', 'C_e7e3b467a16ee72c1ef09d1fd039')"
Run "INSERT INTO `Media` (`DiskId`, `LastSequence`, `Cabinet`) VALUES (1, 2, '#payload.cab')"
Run "INSERT INTO `Upgrade` (`UpgradeCode`, `VersionMax`, `Attributes`, `ActionProperty`) VALUES ('{2E30742F-D372-57A6-B6C2-26D3AC872973}', '1.0.52', 1, 'MT_UPGRADE_PRODUCTS')"
Dim row, parts
For Each row In Array("FindRelatedProducts||25", "LaunchConditions||100", "CostInitialize||800", "FileCost||900", "CostFinalize||1000", "MigrateFeatureStates||1200", _
    "InstallValidate||1400", "InstallInitialize||1500", "RemoveExistingProducts|MT_UPGRADE_PRODUCTS|1510", "ProcessComponents||1600", "UnpublishFeatures||1800", _
    "StopServices||1900", "DeleteServices||2000", "RemoveRegistryValues||2600", "RemoveShortcuts||3200", "RemoveFiles||3500", "RemoveFolders||3600", "CreateFolders||3700", _
    "InstallFiles||4000", "CreateShortcuts||4500", "WriteRegistryValues||5000", "InstallServices||5800", "StartServices||5900", "RegisterUser||6000", "RegisterProduct||6100", _
    "PublishFeatures||6300", "PublishProduct||6400", "InstallFinalize||6600", _
    "MultitapHostScopeGate||1401", "MultitapHostObserverInitialize||1501", "MultitapHostObserverRollback||1502", "MultitapHostObserveService||1901", _
    "MultitapHostServiceExitBarrier||1902", "MultitapHostLegacyActivationBarrier||5899", "MultitapHostObserverCommit||6599")
  parts = Split(row, "|")
  Run "INSERT INTO `InstallExecuteSequence` (`Action`, `Condition`, `Sequence`) VALUES ('" & parts(0) & "', '" & parts(1) & "', " & parts(2) & ")"
Next
For Each row In Array("FindRelatedProducts|25", "LaunchConditions|100", "CostInitialize|800", "FileCost|900", "CostFinalize|1000", "ExecuteAction|1300")
  parts = Split(row, "|")
  Run "INSERT INTO `InstallUISequence` (`Action`, `Sequence`) VALUES ('" & parts(0) & "', " & parts(1) & ")"
Next
For Each row In Array("MultitapHostScopeGate|1", "MultitapHostObserverInitialize|1", "MultitapHostObserverRollback|1281", "MultitapHostObserveService|3073", _
    "MultitapHostServiceExitBarrier|3073", "MultitapHostLegacyActivationBarrier|3073", "MultitapHostObserverCommit|1537")
  parts = Split(row, "|")
  Run "INSERT INTO `CustomAction` (`Action`, `Type`, `Source`, `Target`) VALUES ('" & parts(0) & "', " & parts(1) & ", 'MultitapHostScopeBinary', '" & parts(0) & "')"
Next
Set r = wi.CreateRecord(2)
r.StringData(1) = "MultitapHostScopeBinary"
r.SetStream 2, WScript.ScriptFullName
Set v = db.OpenView("INSERT INTO `Binary` (`Name`, `Data`) VALUES (?, ?)") : v.Execute r : v.Close
Set si = db.SummaryInformation(8)
si.Property(2) = "Installation Database"
si.Property(3) = "Multitap Host"
si.Property(4) = "Multitap"
si.Property(7) = "x64;1042"
si.Property(9) = "{AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE}"
si.Property(14) = 500
si.Property(15) = 2
si.Property(19) = 2
si.Persist
db.Commit
WScript.Echo "ok"
'@, [Text.Encoding]::ASCII)
    $made = & cscript //nologo $create $msi
    if ($LASTEXITCODE -ne 0 -or ($made | Select-Object -Last 1) -ne 'ok') { throw "the fixture package was not created: $made" }
    $simplified = & cscript //nologo $Simplify $msi '{12345678-90AB-4CDE-8F01-234567890ABC}' (Resolve-Path -LiteralPath $Icon).Path
    Assert-QA ($LASTEXITCODE -eq 0 -and ($simplified | Select-Object -Last 1) -eq 'ok') "simplify-msi.vbs accepts a package shaped like the authored one ($simplified)"
  } else {
    Copy-Item -LiteralPath (Resolve-Path -LiteralPath $Package).Path -Destination $msi
  }

  function Query([string]$sql) {
    $out = Join-Path $work ('q' + [guid]::NewGuid().ToString('N') + '.tsv')
    & cscript //nologo $Dump $msi $out $sql | Out-Null
    if ($LASTEXITCODE) { throw "query failed: $sql" }
    # One object per row (a bare array of one row would be unrolled into its fields).
    Get-Content -LiteralPath $out -Encoding Unicode | Where-Object { $_ -ne '' } | ForEach-Object { [pscustomobject]@{ F = [string[]]($_ -split "`t") } }
  }
  $name = 'MultitapHostRemovalCleanup'
  $expectedTarget = '--product-removal-cleanup --remove=[REMOVE] --upgrading=[UPGRADINGPRODUCTCODE] --product=[ProductCode]'
  $expectedCondition = 'REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE'

  # 1. The action itself.
  $actions = @(Query 'SELECT `Action`, `Type`, `Source`, `Target` FROM `CustomAction`')
  Assert-QA ($actions.Count -eq 1 -and $actions[0].F[0] -eq $name) 'the removal cleanup is the only custom action of the package'
  $type = [int]$actions[0].F[1]
  Assert-QA ($type -eq 3154) 'type 3154'
  Assert-QA (($type -band 0x3F) -eq 18) 'it runs a file this package installed (no embedded program, no path from a property)'
  Assert-QA (($type -band 64) -ne 0) 'its exit code is ignored: it cannot fail or roll back the installer'
  Assert-QA (($type -band 128) -eq 0) 'the installer waits for it (the files are removed only afterwards)'
  Assert-QA (($type -band 1024) -ne 0 -and ($type -band 256) -eq 0 -and ($type -band 512) -eq 0) 'it runs inside the transaction, not as a rollback or commit action'
  Assert-QA (($type -band 2048) -ne 0) 'it runs as SYSTEM, not as the person who clicked'
  $tool = @(Query 'SELECT `File`, `FileName` FROM `File`' | Where-Object { ($_.F[1] -split '\|')[-1] -eq 'MultitapHostTool.exe' })
  Assert-QA ($tool.Count -eq 1 -and $actions[0].F[2] -eq $tool[0].F[0]) 'the program is the installed MultitapHostTool.exe'
  Assert-QA ($actions[0].F[3] -ceq $expectedTarget) 'the command line carries REMOVE, UPGRADINGPRODUCTCODE and the product code'
  Assert-QA (@(Query 'SELECT `Name` FROM `Binary`').Count -eq 0) 'no program is embedded in the package'

  # 2. Where it runs.
  $sequence = @{}; foreach ($row in (Query 'SELECT `Action`, `Condition`, `Sequence` FROM `InstallExecuteSequence`')) { $sequence[$row.F[0]] = @{ condition = $row.F[1]; at = [int]$row.F[2] } }
  Assert-QA ($sequence.ContainsKey($name) -and $sequence[$name].condition -ceq $expectedCondition) 'scheduled only for REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE'
  $at = $sequence[$name].at
  Assert-QA ($at -gt $sequence['InstallInitialize'].at -and $at -lt $sequence['InstallFinalize'].at) 'inside the transaction'
  Assert-QA ($at -gt $sequence['StopServices'].at -and $at -gt $sequence['DeleteServices'].at) 'after the Host service is stopped and deleted (a running stream has ended)'
  Assert-QA ($at -lt $sequence['RemoveFiles'].at) 'before the files are removed (the tool still exists)'
  Assert-QA (@($sequence.Keys | Where-Object { $sequence[$_].at -eq $at }).Count -eq 1) 'no other action shares its sequence number'
  Assert-QA (@($sequence.Keys | Where-Object { $_ -like 'MultitapHost*' -and $_ -ne $name }).Count -eq 0) 'none of the removed gate actions is back'
  Assert-QA (@(Query 'SELECT `Action` FROM `InstallUISequence`' | Where-Object { $_.F[0] -eq $name }).Count -eq 0) 'never in the user-interface sequence'
  # The update path this package uses: the old product is removed by the new package.
  if ($sequence.ContainsKey('RemoveExistingProducts')) { Assert-QA ($sequence['RemoveExistingProducts'].condition -eq 'MT_UPGRADE_PRODUCTS') 'updates remove the old product through RemoveExistingProducts' }

  # 3. What Windows Installer decides, in a session of this package that installs nothing.
  $evaluate = Join-Path $work 'evaluate.vbs'
  [IO.File]::WriteAllText($evaluate, @'
Option Explicit
Dim wi, session, view, row, condition, target, record, pair, parts, index, output
Set wi = CreateObject("WindowsInstaller.Installer")
wi.UILevel = 2
Set session = wi.OpenPackage(WScript.Arguments(0), 1)
Set view = session.Database.OpenView("SELECT `Condition` FROM `InstallExecuteSequence` WHERE `Action`='MultitapHostRemovalCleanup'")
view.Execute : Set row = view.Fetch : condition = row.StringData(1) : view.Close
Set view = session.Database.OpenView("SELECT `Target` FROM `CustomAction` WHERE `Action`='MultitapHostRemovalCleanup'")
view.Execute : Set row = view.Fetch : target = row.StringData(1) : view.Close
For index = 1 To WScript.Arguments.Count - 1
  If WScript.Arguments(index) <> "-" Then
    For Each pair In Split(WScript.Arguments(index), ";")
      parts = Split(pair, "=", 2)
      session.Property(parts(0)) = parts(1)
    Next
  End If
Next
Set record = wi.CreateRecord(1)
record.StringData(0) = target
output = session.EvaluateCondition(condition) & vbTab & session.FormatRecord(record) & vbTab & session.Property("ProductCode")
WScript.Echo output
'@, [Text.Encoding]::ASCII)
  function Decide([string]$properties) {
    $line = & cscript //nologo $evaluate $msi $(if ($properties) { $properties } else { '-' })
    if ($LASTEXITCODE -ne 0) { throw "the package session failed: $line" }
    $fields = ($line | Select-Object -Last 1) -split "`t"
    [pscustomobject]@{ Scheduled = $fields[0]; Arguments = $fields[1]; Product = $fields[2] }
  }
  $newer = '{0B1C2D3E-4F50-5A6B-8C7D-9E0F1A2B3C4D}'
  # A real removal: Windows "remove app", or msiexec /x.
  $removal = Decide 'REMOVE=ALL'
  Assert-QA ($removal.Product -match '^\{[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}\}$') 'the package has a product code'
  Assert-QA ($removal.Scheduled -eq '1') 'a real removal schedules the cleanup'
  Assert-QA ($removal.Arguments -ceq "--product-removal-cleanup --remove=ALL --upgrading= --product=$($removal.Product)") 'a real removal passes exactly: --remove=ALL --upgrading= --product={code}'
  # THE dangerous case: this product is removed because a newer package is being installed.
  $upgrade = Decide "REMOVE=ALL;UPGRADINGPRODUCTCODE=$newer"
  Assert-QA ($upgrade.Scheduled -eq '0') 'the removal that belongs to an update does NOT schedule the cleanup'
  Assert-QA ($upgrade.Arguments -ceq "--product-removal-cleanup --remove=ALL --upgrading=$newer --product=$($upgrade.Product)") 'and its command line would name the upgrading product, which the tool refuses'
  # Everything that is not a removal.
  Assert-QA ((Decide '').Scheduled -eq '0') 'a first install does not schedule it'
  Assert-QA ((Decide 'MT_UPGRADE_PRODUCTS={5F6E7D8C-9B0A-4123-8456-789ABCDEF012}').Scheduled -eq '0') 'installing over an older version does not schedule it in the new package'
  Assert-QA ((Decide 'REINSTALL=ALL;REINSTALLMODE=omus').Scheduled -eq '0') 'a repair does not schedule it'
  Assert-QA ((Decide 'REMOVE=HostFeature').Scheduled -eq '0' -and (Decide 'REMOVE=all').Scheduled -eq '0') 'only REMOVE=ALL counts'
  Assert-QA ((Decide "UPGRADINGPRODUCTCODE=$newer").Scheduled -eq '0') 'an upgrade marker alone does not schedule it'

  [ordered]@{ kind = 'host-removal-action'; cases = $cases; package = $(if ($fixture) { 'fixture' } else { 'built' }); installed = $false; removed = $false; actionsRun = 0 } | ConvertTo-Json -Compress
} finally { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }

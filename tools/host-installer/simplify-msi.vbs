' Multitap Host MSI: turn the authored package into a plain Windows Installer package.
'   cscript //nologo simplify-msi.vbs <package.msi> <new PackageCode {GUID}> <host.ico>
' Edits the given file in place (run it on a copy):
'   - removes the seven Multitap gate custom actions and their DLL. They refused real
'     upgrades on small machine differences and threw the reason away (exit 50).
'   - adds a Startup shortcut that starts the Host tray at sign-in (MultitapHost.exe --tray).
'     The authored package had none, so the old version's tray link stayed behind.
'   - sets a new PackageCode (each built package must have its own).
'   - adds the one custom action the package has: MultitapHostRemovalCleanup. It runs the
'     installed MultitapHostTool.exe while the product is REMOVED, after the service was
'     stopped and deleted and before the files go, so that the virtual display device, its
'     driver package, the certificate entries Multitap added and the display records do not
'     stay behind (apps/windows_host/src/host_removal.hpp). It is scheduled only when
'     REMOVE="ALL" AND NOT UPGRADINGPRODUCTCODE: an update is a major upgrade, in which the
'     new package removes the old product with UPGRADINGPRODUCTCODE set, and nothing may be
'     cleaned then. The same two properties are passed on the command line, where the tool
'     checks them again. Its exit code is ignored (type 64): it can never fail or roll back
'     an install, an update or a removal, unlike the gate actions removed above.
'     (apps/windows_host/tests/removal-action.test.ps1 checks all of this on a package.)
Option Explicit
Dim wi, db, v, si
Set wi = CreateObject("WindowsInstaller.Installer")
Set db = wi.OpenDatabase(WScript.Arguments(0), 1)

Sub Run(sql)
  Set v = db.OpenView(sql) : v.Execute : v.Close
End Sub
Function Count(sql)
  Dim r, n : n = 0
  Set v = db.OpenView(sql) : v.Execute
  Set r = v.Fetch
  Do Until r Is Nothing : n = n + 1 : Set r = v.Fetch : Loop
  v.Close : Count = n
End Function

Run "DELETE FROM InstallExecuteSequence WHERE Action='MultitapHostScopeGate' OR Action='MultitapHostObserverInitialize' OR Action='MultitapHostObserverRollback' OR Action='MultitapHostObserveService' OR Action='MultitapHostServiceExitBarrier' OR Action='MultitapHostLegacyActivationBarrier' OR Action='MultitapHostObserverCommit'"
Run "DELETE FROM CustomAction WHERE Source='MultitapHostScopeBinary'"
Run "DELETE FROM Binary WHERE Name='MultitapHostScopeBinary'"
If Count("SELECT Action FROM CustomAction") <> 0 Then WScript.Echo "custom actions remain" : WScript.Quit 2

Dim r, name
' Removal cleanup. Type 3154 = 18 (an installed file of this package is the program)
' + 64 (wait for it, ignore its exit code) + 1024 (deferred: runs with the other changes)
' + 2048 (not impersonated: SYSTEM). Placed right after DeleteServices.
Dim toolKey, fileName, afterServices, nextAction, sequence, cleanupAt
toolKey = ""
Set v = db.OpenView("SELECT `File`, `FileName` FROM `File`") : v.Execute
Set r = v.Fetch
Do Until r Is Nothing
  fileName = r.StringData(2)
  If InStr(fileName, "|") > 0 Then fileName = Mid(fileName, InStr(fileName, "|") + 1)
  If LCase(fileName) = "multitaphosttool.exe" Then
    If toolKey <> "" Then WScript.Echo "more than one MultitapHostTool.exe" : WScript.Quit 3
    toolKey = r.StringData(1)
  End If
  Set r = v.Fetch
Loop
v.Close
If toolKey = "" Then WScript.Echo "MultitapHostTool.exe not found" : WScript.Quit 3
afterServices = 0 : nextAction = 32767
Set v = db.OpenView("SELECT `Action`, `Sequence` FROM `InstallExecuteSequence`") : v.Execute
Set r = v.Fetch
Do Until r Is Nothing
  If r.StringData(1) = "DeleteServices" Then afterServices = r.IntegerData(2)
  Set r = v.Fetch
Loop
v.Close
If afterServices <= 0 Then WScript.Echo "DeleteServices is not sequenced" : WScript.Quit 3
Set v = db.OpenView("SELECT `Action`, `Sequence` FROM `InstallExecuteSequence`") : v.Execute
Set r = v.Fetch
Do Until r Is Nothing
  sequence = r.IntegerData(2)
  If sequence > afterServices And sequence < nextAction Then nextAction = sequence
  Set r = v.Fetch
Loop
v.Close
If nextAction - afterServices < 2 Then WScript.Echo "no room after DeleteServices" : WScript.Quit 3
cleanupAt = afterServices + (nextAction - afterServices) \ 2
Run "INSERT INTO `CustomAction` (`Action`, `Type`, `Source`, `Target`) VALUES ('MultitapHostRemovalCleanup', 3154, '" & toolKey & "', " & _
    "'--product-removal-cleanup --remove=[REMOVE] --upgrading=[UPGRADINGPRODUCTCODE] --product=[ProductCode]')"
Run "INSERT INTO `InstallExecuteSequence` (`Action`, `Condition`, `Sequence`) VALUES ('MultitapHostRemovalCleanup', " & _
    "'REMOVE=""ALL"" AND NOT UPGRADINGPRODUCTCODE', " & cleanupAt & ")"
If Count("SELECT Action FROM CustomAction") <> 1 Then WScript.Echo "unexpected custom actions" : WScript.Quit 2

' The service component's key file is MultitapHost.exe.
Set v = db.OpenView("SELECT `File`.`FileName` FROM `Component`, `File` WHERE `Component`.`Component`='C_07c652e489165f50893604d567b4' AND `File`.`File`=`Component`.`KeyPath`")
v.Execute : Set r = v.Fetch
If r Is Nothing Then WScript.Echo "service component not found" : WScript.Quit 3
name = r.StringData(1) : v.Close
If Right(name, 17) <> "|MultitapHost.exe" And name <> "MultitapHost.exe" Then WScript.Echo "service key file is " & name : WScript.Quit 3
If Count("SELECT Directory FROM Directory WHERE Directory='StartupFolder'") = 0 Then
  Run "INSERT INTO Directory (Directory, Directory_Parent, DefaultDir) VALUES ('StartupFolder', 'TARGETDIR', '.')"
End If
Run "INSERT INTO Shortcut (Shortcut, Directory_, Name, Component_, Target, Arguments, Description, ShowCmd, WkDir) " & _
    "VALUES ('S_HostTrayStartup', 'StartupFolder', 'MHTRAY|Multitap Host Tray', 'C_07c652e489165f50893604d567b4', 'HostFeature', '--tray', 'Multitap Host Tray', 7, 'D_ceba428ba8eb987f385fab5d7a3a')"

' Icons: the authored shortcuts had none, so Windows drew a blank page for them and in the app list.
Dim icon
If Count("SELECT `Name` FROM `_Tables` WHERE `Name`='Icon'") = 0 Then
  Run "CREATE TABLE `Icon` (`Name` CHAR(72) NOT NULL, `Data` OBJECT NOT NULL PRIMARY KEY `Name`)"
End If
Set icon = wi.CreateRecord(2)
icon.StringData(1) = "MultitapHost.ico"
icon.SetStream 2, WScript.Arguments(2)
Set v = db.OpenView("INSERT INTO `Icon` (`Name`, `Data`) VALUES (?, ?)") : v.Execute icon : v.Close
Run "UPDATE `Shortcut` SET `Icon_`='MultitapHost.ico', `IconIndex`=0"
Run "INSERT INTO `Property` (`Property`, `Value`) VALUES ('ARPPRODUCTICON', 'MultitapHost.ico')"

Set si = db.SummaryInformation(4)
si.Property(9) = WScript.Arguments(1)
si.Persist
db.Commit
Set v = db.OpenView("SELECT Value FROM Property WHERE Property='ProductVersion'")
v.Execute : Set r = v.Fetch
WScript.Echo "version=" & r.StringData(1)
v.Close
WScript.Echo "ok"

' Applies an operations file to an MSI in one transaction.
'   cscript //nologo msi-apply.vbs <package.msi> <ops.txt (UTF-16)>
' Lines (tab separated):
'   SQL      <statement>
'   STREAM   <_Streams name>  <file path>      replace or add a stream
'   EXTRACT  <_Streams name>  <file path>      write a stream to a file (no change)
'   SUMMARY  <property id>    <value>
Option Explicit
Dim wi, db, fso, ops, line, parts, v, r, si, s
Set wi = CreateObject("WindowsInstaller.Installer")
Set db = wi.OpenDatabase(WScript.Arguments(0), 1)
Set fso = CreateObject("Scripting.FileSystemObject")
Set ops = fso.OpenTextFile(WScript.Arguments(1), 1, False, -1)
Set si = Nothing
Do Until ops.AtEndOfStream
  line = ops.ReadLine
  If Len(line) > 0 Then
    parts = Split(line, vbTab)
    Select Case parts(0)
      Case "SQL"
        On Error Resume Next
        Set v = db.OpenView(parts(1)) : v.Execute
        If Err.Number <> 0 Then WScript.Echo "failed: " & parts(1) & " (" & Err.Description & ")" : WScript.Quit 4
        On Error GoTo 0
        v.Close
      Case "STREAM"
        Set v = db.OpenView("DELETE FROM `_Streams` WHERE `Name`='" & parts(1) & "'") : v.Execute : v.Close
        Set r = wi.CreateRecord(2) : r.StringData(1) = parts(1) : r.SetStream 2, parts(2)
        Set v = db.OpenView("INSERT INTO `_Streams` (`Name`, `Data`) VALUES (?, ?)") : v.Execute r : v.Close
      Case "EXTRACT"
        Set v = db.OpenView("SELECT `Data` FROM `_Streams` WHERE `Name`='" & parts(1) & "'") : v.Execute
        Set r = v.Fetch
        If r Is Nothing Then WScript.Echo "no stream " & parts(1) : WScript.Quit 2
        Dim stm : Set stm = CreateObject("ADODB.Stream") : stm.Type = 1 : stm.Open
        s = r.DataSize(1)
        stm.Write r.ReadStream(1, s, 1) ' 1 = msiReadStreamBytes
        stm.SaveToFile parts(2), 2 : stm.Close : v.Close
      Case "SUMMARY"
        If si Is Nothing Then Set si = db.SummaryInformation(8)
        si.Property(CInt(parts(1))) = parts(2)
      Case Else
        WScript.Echo "unknown op " & parts(0) : WScript.Quit 3
    End Select
  End If
Loop
If Not si Is Nothing Then si.Persist
db.Commit
WScript.Echo "ok"

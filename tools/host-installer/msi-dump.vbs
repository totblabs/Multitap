' Prints the rows of one MSI query, tab separated, UTF-16 safe through a file.
'   cscript //nologo msi-dump.vbs <package.msi> <output.tsv> "<SELECT ...>"
Option Explicit
Dim wi, db, v, r, f, line, i, fso, out
Set wi = CreateObject("WindowsInstaller.Installer")
Set db = wi.OpenDatabase(WScript.Arguments(0), 0)
Set fso = CreateObject("Scripting.FileSystemObject")
Set out = fso.CreateTextFile(WScript.Arguments(1), True, True)
Set v = db.OpenView(WScript.Arguments(2)) : v.Execute
Set r = v.Fetch
Do Until r Is Nothing
  line = ""
  For i = 1 To r.FieldCount
    If i > 1 Then line = line & vbTab
    line = line & r.StringData(i)
  Next
  out.WriteLine line
  Set r = v.Fetch
Loop
v.Close : out.Close

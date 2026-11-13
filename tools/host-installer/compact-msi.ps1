# Rewrites an MSI into a fresh file that holds only its live contents.
#   powershell -ExecutionPolicy Bypass -File tools\host-installer\compact-msi.ps1 -Msi <package.msi>
# An MSI is an OLE compound file. When a table row or stream is deleted (simplify-msi.vbs drops the
# old gate custom-action DLL, 8.8 MB), Windows Installer marks its sectors free but leaves the bytes
# in the file; the 1.1.1 and 1.1.2 packages carried an old DLL that way, with the build machine's
# folders in it. Copying the root storage into a new compound file (IStorage::CopyTo, the same
# streams, storages and class id) leaves those sectors behind. The file is replaced only when the
# copy holds exactly the same streams with the same bytes and has no free sector left.
param([Parameter(Mandatory)][string]$Msi)
$ErrorActionPreference = 'Stop'
$Msi = (Resolve-Path -LiteralPath $Msi).Path
if (-not ('MultitapMsiCompact' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Security.Cryptography;

[ComImport, Guid("0000000b-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMtStorage {
  void CreateStream([MarshalAs(UnmanagedType.LPWStr)] string name, uint mode, uint r1, uint r2, out IStream stream);
  void OpenStream([MarshalAs(UnmanagedType.LPWStr)] string name, IntPtr r1, uint mode, uint r2, out IStream stream);
  void CreateStorage([MarshalAs(UnmanagedType.LPWStr)] string name, uint mode, uint r1, uint r2, out IMtStorage storage);
  void OpenStorage([MarshalAs(UnmanagedType.LPWStr)] string name, IntPtr priority, uint mode, IntPtr exclude, uint r, out IMtStorage storage);
  void CopyTo(uint ciidExclude, IntPtr rgiidExclude, IntPtr snbExclude, IMtStorage destination);
  void MoveElementTo([MarshalAs(UnmanagedType.LPWStr)] string name, IMtStorage destination, [MarshalAs(UnmanagedType.LPWStr)] string newName, uint flags);
  void Commit(uint flags);
  void Revert();
  void EnumElements(uint r1, IntPtr r2, uint r3, out IMtEnumStatStg e);
  void DestroyElement([MarshalAs(UnmanagedType.LPWStr)] string name);
  void RenameElement([MarshalAs(UnmanagedType.LPWStr)] string oldName, [MarshalAs(UnmanagedType.LPWStr)] string newName);
  void SetElementTimes([MarshalAs(UnmanagedType.LPWStr)] string name, IntPtr c, IntPtr a, IntPtr m);
  void SetClass(ref Guid clsid);
  void SetStateBits(uint bits, uint mask);
  void Stat(out System.Runtime.InteropServices.ComTypes.STATSTG stat, uint flags);
}
[ComImport, Guid("0000000d-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IMtEnumStatStg {
  [PreserveSig] int Next(uint celt, [Out, MarshalAs(UnmanagedType.LPArray)] System.Runtime.InteropServices.ComTypes.STATSTG[] rgelt, out uint fetched);
  void Skip(uint celt);
  void Reset();
  void Clone(out IMtEnumStatStg e);
}
[StructLayout(LayoutKind.Sequential)]
public struct MtStgOptions { public ushort usVersion; public ushort reserved; public uint ulSectorSize; public IntPtr pwcsTemplateFile; }

public static class MultitapMsiCompact {
  [DllImport("ole32.dll")] static extern int StgOpenStorageEx([MarshalAs(UnmanagedType.LPWStr)] string name, uint mode, uint format, uint attrs, IntPtr options, IntPtr security, ref Guid riid, [MarshalAs(UnmanagedType.Interface)] out IMtStorage storage);
  [DllImport("ole32.dll")] static extern int StgCreateStorageEx([MarshalAs(UnmanagedType.LPWStr)] string name, uint mode, uint format, uint attrs, ref MtStgOptions options, IntPtr security, ref Guid riid, [MarshalAs(UnmanagedType.Interface)] out IMtStorage storage);
  const uint STGM_READ = 0, STGM_READWRITE = 2, STGM_SHARE_EXCLUSIVE = 0x10, STGM_CREATE = 0x1000, STGFMT_DOCFILE = 5;
  static Guid IID_IStorage = new Guid("0000000b-0000-0000-C000-000000000046");

  static IMtStorage Open(string path) {
    IMtStorage s; int hr = StgOpenStorageEx(path, STGM_READ | STGM_SHARE_EXCLUSIVE, STGFMT_DOCFILE, 0, IntPtr.Zero, IntPtr.Zero, ref IID_IStorage, out s);
    if (hr != 0) Marshal.ThrowExceptionForHR(hr); return s;
  }
  static void Release(object o) { if (o != null) Marshal.ReleaseComObject(o); }
  static string Hash(IStream stream) {
    using (var sha = SHA256.Create()) {
      byte[] buffer = new byte[1 << 20]; IntPtr read = Marshal.AllocHGlobal(4);
      try {
        while (true) {
          stream.Read(buffer, buffer.Length, read); int n = Marshal.ReadInt32(read);
          if (n <= 0) break; sha.TransformBlock(buffer, 0, n, null, 0);
        }
      } finally { Marshal.FreeHGlobal(read); }
      sha.TransformFinalBlock(new byte[0], 0, 0); return BitConverter.ToString(sha.Hash);
    }
  }
  // Every stream and storage under a storage: path (names joined by '/') -> SHA-256, plus each class id.
  static void Inventory(IMtStorage storage, string prefix, SortedDictionary<string, string> into) {
    System.Runtime.InteropServices.ComTypes.STATSTG self; storage.Stat(out self, 1);
    into[prefix + "<class>"] = self.clsid.ToString();
    IMtEnumStatStg e; storage.EnumElements(0, IntPtr.Zero, 0, out e);
    try {
      var one = new System.Runtime.InteropServices.ComTypes.STATSTG[1]; uint got;
      while (e.Next(1, one, out got) == 0 && got == 1) {
        string name = one[0].pwcsName;
        if (one[0].type == 2) {
          IStream s; storage.OpenStream(name, IntPtr.Zero, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, out s);
          try { into[prefix + name] = one[0].cbSize + ":" + Hash(s); } finally { Release(s); }
        } else if (one[0].type == 1) {
          IMtStorage child; storage.OpenStorage(name, IntPtr.Zero, STGM_READ | STGM_SHARE_EXCLUSIVE, IntPtr.Zero, 0, out child);
          try { Inventory(child, prefix + name + "/", into); } finally { Release(child); }
        }
      }
    } finally { Release(e); }
  }
  public static SortedDictionary<string, string> InventoryOf(string path) {
    var into = new SortedDictionary<string, string>(StringComparer.Ordinal); IMtStorage s = Open(path);
    try { Inventory(s, "", into); } finally { Release(s); } return into;
  }
  public static ushort SectorShift(string path) {
    using (var f = File.OpenRead(path)) { var b = new byte[32]; f.Read(b, 0, 32); return BitConverter.ToUInt16(b, 30); }
  }
  public static void Copy(string source, string destination) {
    IMtStorage src = Open(source), dst = null;
    try {
      var options = new MtStgOptions { usVersion = 1, ulSectorSize = (uint)(1 << SectorShift(source)) };
      int hr = StgCreateStorageEx(destination, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, STGFMT_DOCFILE, 0, ref options, IntPtr.Zero, ref IID_IStorage, out dst);
      if (hr != 0) Marshal.ThrowExceptionForHR(hr);
      src.CopyTo(0, IntPtr.Zero, IntPtr.Zero, dst);
      System.Runtime.InteropServices.ComTypes.STATSTG stat; src.Stat(out stat, 1);
      Guid clsid = stat.clsid; dst.SetClass(ref clsid);
      dst.Commit(0);
    } finally { Release(dst); Release(src); }
  }
  // Free sectors: FAT entries marked FREESECT (0xFFFFFFFF) inside the file.
  public static int FreeSectors(string path) {
    byte[] d = File.ReadAllBytes(path); int ss = 1 << BitConverter.ToUInt16(d, 30);
    uint nfat = BitConverter.ToUInt32(d, 44), difatStart = BitConverter.ToUInt32(d, 68), ndifat = BitConverter.ToUInt32(d, 72);
    var difat = new List<uint>(); for (int i = 0; i < 109; i++) difat.Add(BitConverter.ToUInt32(d, 76 + 4 * i));
    uint s = difatStart;
    for (uint k = 0; k < ndifat; k++) { long off = (long)(s + 1) * ss; for (int i = 0; i < ss / 4 - 1; i++) difat.Add(BitConverter.ToUInt32(d, (int)(off + 4 * i))); s = BitConverter.ToUInt32(d, (int)(off + ss - 4)); }
    long sectors = (d.Length - ss) / ss; int free = 0; long index = 0;
    for (int f = 0; f < nfat && f < difat.Count; f++) {
      long off = (long)(difat[f] + 1) * ss;
      for (int i = 0; i < ss / 4; i++, index++) if (index < sectors && BitConverter.ToUInt32(d, (int)(off + 4 * i)) == 0xFFFFFFFF) free++;
    }
    return free;
  }
}
'@
}
$before = [MultitapMsiCompact]::InventoryOf($Msi)
$freeBefore = [MultitapMsiCompact]::FreeSectors($Msi)
$temp = "$Msi.compact-" + [guid]::NewGuid().ToString('N')
try {
  [MultitapMsiCompact]::Copy($Msi, $temp)
  $after = [MultitapMsiCompact]::InventoryOf($temp)
  $same = $before.Count -eq $after.Count
  if ($same) { foreach ($k in $before.Keys) { if (-not $after.ContainsKey($k) -or $after[$k] -ne $before[$k]) { $same = $false; break } } }
  if (-not $same) { throw 'the compacted copy does not hold the same streams' }
  $freeAfter = [MultitapMsiCompact]::FreeSectors($temp)
  if ($freeAfter -ne 0) { throw "the compacted copy still has $freeAfter free sectors" }
  $bytesBefore = (Get-Item -LiteralPath $Msi).Length
  Move-Item -LiteralPath $temp -Destination $Msi -Force
  [ordered]@{ msi = $Msi; entries = $before.Count; freeSectorsBefore = $freeBefore; bytesBefore = $bytesBefore; bytesAfter = (Get-Item -LiteralPath $Msi).Length } | ConvertTo-Json -Compress
} finally { Remove-Item -LiteralPath $temp -ErrorAction SilentlyContinue }

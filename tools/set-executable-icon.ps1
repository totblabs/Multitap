param(
  [Parameter(Mandatory)][string]$Executable,
  [Parameter(Mandatory)][string]$Icon,
  [int]$GroupId = 101
)
# Build-time only (never at install time): replaces the icon group $GroupId of
# an unsigned PE file with the frames of $Icon. tools/release/host-msi-package.ps1 uses it to give
# MultitapHostSettings.exe, a copy of the Flutter client Multitap.exe
# (IDI_APP_ICON = 101), the Host icon.
#
# The patch is written to a temporary copy and verified before it replaces the
# file: the new group and frames read back byte for byte, the manifest and
# version resources are unchanged, every section other than .rsrc keeps its
# bytes, and the result still maps as an executable image. Files that carry an
# Authenticode signature or appended data are refused. Any failure throws and
# leaves $Executable untouched.
$ErrorActionPreference = 'Stop'
if (-not ('Multitap.ExecutableIcon' -as [type])) {
  Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.IO; using System.Linq; using System.Runtime.InteropServices;
namespace Multitap {
public static class ExecutableIcon {
  const uint DATAFILE = 0x2 | 0x20, DONT_RESOLVE = 0x1; static readonly IntPtr RT_ICON = (IntPtr)3, RT_GROUP_ICON = (IntPtr)14, RT_VERSION = (IntPtr)16, RT_MANIFEST = (IntPtr)24;
  delegate bool EnumLang(IntPtr module, IntPtr type, IntPtr name, ushort lang, IntPtr param);
  delegate bool EnumName(IntPtr module, IntPtr type, IntPtr name, IntPtr param);
  [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr LoadLibraryEx(string file, IntPtr reserved, uint flags);
  [DllImport("kernel32")] static extern bool FreeLibrary(IntPtr module);
  [DllImport("kernel32")] static extern bool EnumResourceLanguages(IntPtr module, IntPtr type, IntPtr name, EnumLang callback, IntPtr param);
  [DllImport("kernel32")] static extern bool EnumResourceNames(IntPtr module, IntPtr type, EnumName callback, IntPtr param);
  [DllImport("kernel32")] static extern IntPtr FindResourceEx(IntPtr module, IntPtr type, IntPtr name, ushort lang);
  [DllImport("kernel32")] static extern IntPtr LoadResource(IntPtr module, IntPtr info);
  [DllImport("kernel32")] static extern IntPtr LockResource(IntPtr data);
  [DllImport("kernel32")] static extern uint SizeofResource(IntPtr module, IntPtr info);
  [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr BeginUpdateResource(string file, bool deleteExisting);
  [DllImport("kernel32", SetLastError = true)] static extern bool UpdateResource(IntPtr update, IntPtr type, IntPtr name, ushort lang, byte[] data, uint size);
  [DllImport("kernel32", SetLastError = true)] static extern bool EndUpdateResource(IntPtr update, bool discard);

  class Resources { public ushort Lang; public byte[] Group; public Dictionary<int, byte[]> Icons = new Dictionary<int, byte[]>(); public Dictionary<string, byte[]> Kept = new Dictionary<string, byte[]>(); }

  static byte[] Read(IntPtr module, IntPtr type, IntPtr name, ushort lang) {
    IntPtr info = FindResourceEx(module, type, name, lang); if (info == IntPtr.Zero) return null;
    var bytes = new byte[SizeofResource(module, info)]; Marshal.Copy(LockResource(LoadResource(module, info)), bytes, 0, bytes.Length); return bytes;
  }
  static List<ushort> Languages(IntPtr module, IntPtr type, IntPtr name) {
    var list = new List<ushort>(); EnumResourceLanguages(module, type, name, (m, t, n, l, p) => { list.Add(l); return true; }, IntPtr.Zero); return list;
  }
  static Resources Load(string file, int group) {
    IntPtr module = LoadLibraryEx(file, IntPtr.Zero, DATAFILE);
    if (module == IntPtr.Zero) throw new Exception("Cannot open " + file + " as a resource file (" + Marshal.GetLastWin32Error() + ").");
    try {
      var r = new Resources(); var langs = Languages(module, RT_GROUP_ICON, (IntPtr)group);
      if (langs.Count != 1) throw new Exception("Expected one icon group " + group + ", found " + langs.Count + ".");
      r.Lang = langs[0]; r.Group = Read(module, RT_GROUP_ICON, (IntPtr)group, r.Lang);
      var ids = new List<int>(); EnumResourceNames(module, RT_ICON, (m, t, n, p) => { if (((long)n >> 16) == 0) ids.Add((int)n); return true; }, IntPtr.Zero);
      foreach (int id in ids) foreach (ushort l in Languages(module, RT_ICON, (IntPtr)id)) r.Icons[id * 65536 + l] = Read(module, RT_ICON, (IntPtr)id, l);
      foreach (var type in new[] { RT_MANIFEST, RT_VERSION }) {
        var names = new List<IntPtr>(); EnumResourceNames(module, type, (m, t, n, p) => { names.Add(n); return true; }, IntPtr.Zero);
        foreach (var n in names) { if (((long)n >> 16) != 0) throw new Exception("Named resources are not expected."); foreach (ushort l in Languages(module, type, n)) r.Kept[(long)type + "/" + (long)n + "/" + l] = Read(module, type, n, l); }
      }
      return r;
    } finally { FreeLibrary(module); }
  }
  static List<int> GroupIds(byte[] group) {
    var ids = new List<int>(); int count = BitConverter.ToUInt16(group, 4);
    for (int i = 0; i < count; i++) ids.Add(BitConverter.ToUInt16(group, 6 + 14 * i + 12)); return ids;
  }
  // Raw bytes of every section except .rsrc, which UpdateResource rewrites
  // (.reloc may move behind it but keeps its content).
  static Dictionary<string, byte[]> Sections(byte[] pe, bool refuseExtras) {
    int lfanew = BitConverter.ToInt32(pe, 0x3C);
    if (BitConverter.ToUInt32(pe, lfanew) != 0x4550) throw new Exception("Not a PE file.");
    int sections = BitConverter.ToUInt16(pe, lfanew + 6), optional = BitConverter.ToUInt16(pe, lfanew + 20), opt = lfanew + 24;
    bool pe32plus = BitConverter.ToUInt16(pe, opt) == 0x20B; int dirs = opt + (pe32plus ? 112 : 96);
    if (refuseExtras && BitConverter.ToUInt32(pe, dirs + 4 * 8 + 4) != 0) throw new Exception("Signed executables cannot be patched.");
    var result = new Dictionary<string, byte[]>(); long end = 0;
    for (int i = 0; i < sections; i++) {
      int s = opt + optional + 40 * i;
      string name = System.Text.Encoding.ASCII.GetString(pe, s, 8).TrimEnd('\0');
      int virtualSize = BitConverter.ToInt32(pe, s + 8), rawSize = BitConverter.ToInt32(pe, s + 16), raw = BitConverter.ToInt32(pe, s + 20);
      end = Math.Max(end, (long)raw + rawSize);
      if (name != ".rsrc") { var bytes = new byte[Math.Min(virtualSize, rawSize)]; Array.Copy(pe, raw, bytes, 0, bytes.Length); result[name] = bytes; }
    }
    if (refuseExtras && end != pe.Length) throw new Exception("Executables with appended data cannot be patched.");
    return result;
  }

  public static string Replace(string exe, string ico, int group) {
    byte[] icoBytes = File.ReadAllBytes(ico);
    if (BitConverter.ToUInt16(icoBytes, 0) != 0 || BitConverter.ToUInt16(icoBytes, 2) != 1) throw new Exception("Not an .ico file.");
    int frames = BitConverter.ToUInt16(icoBytes, 4); var images = new List<byte[]>(); var dir = new byte[6 + 14 * frames];
    Array.Copy(icoBytes, 0, dir, 0, 6);
    for (int i = 0; i < frames; i++) {
      int e = 6 + 16 * i; int size = BitConverter.ToInt32(icoBytes, e + 8), offset = BitConverter.ToInt32(icoBytes, e + 12);
      var image = new byte[size]; Array.Copy(icoBytes, offset, image, 0, size); images.Add(image);
      Array.Copy(icoBytes, e, dir, 6 + 14 * i, 12);
    }
    byte[] before = File.ReadAllBytes(exe); var sectionsBefore = Sections(before, true);
    var old = Load(exe, group); var oldIds = GroupIds(old.Group);
    int next = old.Icons.Keys.Select(k => k / 65536).DefaultIfEmpty(0).Max();
    var newIds = new List<int>(); for (int i = 0; i < frames; i++) newIds.Add(i < oldIds.Count ? oldIds[i] : ++next);
    for (int i = 0; i < frames; i++) BitConverter.GetBytes((ushort)newIds[i]).CopyTo(dir, 6 + 14 * i + 12);

    IntPtr update = BeginUpdateResource(exe, false);
    if (update == IntPtr.Zero) throw new Exception("BeginUpdateResource failed (" + Marshal.GetLastWin32Error() + ").");
    bool ok = true;
    for (int i = 0; i < frames; i++) ok &= UpdateResource(update, RT_ICON, (IntPtr)newIds[i], old.Lang, images[i], (uint)images[i].Length);
    foreach (int id in oldIds.Skip(frames)) ok &= UpdateResource(update, RT_ICON, (IntPtr)id, old.Lang, null, 0);
    ok &= UpdateResource(update, RT_GROUP_ICON, (IntPtr)group, old.Lang, dir, (uint)dir.Length);
    if (!ok) { EndUpdateResource(update, true); throw new Exception("UpdateResource failed (" + Marshal.GetLastWin32Error() + ")."); }
    if (!EndUpdateResource(update, false)) throw new Exception("EndUpdateResource failed (" + Marshal.GetLastWin32Error() + ").");

    var now = Load(exe, group);
    if (!now.Group.SequenceEqual(dir)) throw new Exception("The icon group did not read back.");
    for (int i = 0; i < frames; i++) { byte[] got; if (!now.Icons.TryGetValue(newIds[i] * 65536 + old.Lang, out got) || !got.SequenceEqual(images[i])) throw new Exception("Icon frame " + i + " did not read back."); }
    foreach (int id in oldIds.Skip(frames)) if (now.Icons.ContainsKey(id * 65536 + old.Lang)) throw new Exception("A replaced frame remained.");
    if (now.Kept.Count != old.Kept.Count || old.Kept.Any(k => !now.Kept.ContainsKey(k.Key) || !now.Kept[k.Key].SequenceEqual(k.Value))) throw new Exception("Manifest or version resources changed.");
    var sectionsAfter = Sections(File.ReadAllBytes(exe), false);
    foreach (var s in sectionsBefore) { byte[] got; if (!sectionsAfter.TryGetValue(s.Key, out got) || !got.SequenceEqual(s.Value)) throw new Exception("Section " + s.Key + " changed."); }
    IntPtr mapped = LoadLibraryEx(exe, IntPtr.Zero, DONT_RESOLVE);
    if (mapped == IntPtr.Zero) throw new Exception("The patched file no longer maps as an image (" + Marshal.GetLastWin32Error() + ").");
    FreeLibrary(mapped);
    return frames + " frames in icon group " + group;
  }
}
}
'@
}
$target = (Resolve-Path -LiteralPath $Executable).Path
$iconPath = (Resolve-Path -LiteralPath $Icon).Path
$work = Join-Path ([IO.Path]::GetDirectoryName($target)) ('.icon-' + [Guid]::NewGuid().ToString('N') + '.exe')
Copy-Item -LiteralPath $target -Destination $work
try {
  $result = [Multitap.ExecutableIcon]::Replace($work, $iconPath, $GroupId)
  Move-Item -LiteralPath $work -Destination $target -Force
  Write-Output "Icon set on $([IO.Path]::GetFileName($target)): $result."
} finally {
  if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Force }
}

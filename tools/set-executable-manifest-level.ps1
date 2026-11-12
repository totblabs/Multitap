param(
  [Parameter(Mandatory)][string]$Executable,
  [Parameter(Mandatory)][ValidateSet('asInvoker','requireAdministrator')][string]$Level
)
# Build-time only (never at install time): sets requestedExecutionLevel in the
# RT_MANIFEST of an unsigned PE file. tools/release/host-msi-package.ps1 uses it to turn
# MultitapHostSettings.exe, a copy of the asInvoker Flutter client Multitap.exe,
# into a requireAdministrator program (Host settings change the service,
# firewall and drivers; the client itself runs unelevated).
#
# The patch is written to a temporary copy and verified before it replaces the
# file: the manifest reads back with exactly the new level and otherwise
# identical text, the icon and version resources are unchanged, every section
# other than .rsrc keeps its bytes and the result still maps as an image.
# Signed files or files with appended data are refused; any failure throws and
# leaves $Executable untouched.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'windows-executable-policy.ps1')
if (-not ('Multitap.ManifestPatch' -as [type])) {
  Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.IO; using System.Linq; using System.Runtime.InteropServices; using System.Text;
namespace Multitap {
public static class ManifestPatch {
  const uint DATAFILE = 0x2 | 0x20, DONT_RESOLVE = 0x1;
  static readonly IntPtr RT_MANIFEST = (IntPtr)24;
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

  static byte[] Read(IntPtr module, IntPtr type, IntPtr name, ushort lang) {
    IntPtr info = FindResourceEx(module, type, name, lang); if (info == IntPtr.Zero) return null;
    var bytes = new byte[SizeofResource(module, info)]; Marshal.Copy(LockResource(LoadResource(module, info)), bytes, 0, bytes.Length); return bytes;
  }
  // Every resource except the manifest, keyed by type/name/language.
  static Dictionary<string, byte[]> Others(string file, out ushort lang, out byte[] manifest) {
    IntPtr module = LoadLibraryEx(file, IntPtr.Zero, DATAFILE);
    if (module == IntPtr.Zero) throw new Exception("Cannot open " + file + " as a resource file (" + Marshal.GetLastWin32Error() + ").");
    try {
      var result = new Dictionary<string, byte[]>(); var types = new List<IntPtr>(); ushort manifestLang = 0; byte[] manifestBytes = null; int manifests = 0;
      foreach (long type in new long[] { 3, 14, 16 }) {
        var names = new List<IntPtr>(); EnumResourceNames(module, (IntPtr)type, (m, t, n, p) => { names.Add(n); return true; }, IntPtr.Zero);
        foreach (var n in names) {
          var langs = new List<ushort>(); EnumResourceLanguages(module, (IntPtr)type, n, (m, t, nn, l, p) => { langs.Add(l); return true; }, IntPtr.Zero);
          foreach (var l in langs) result[type + "/" + (long)n + "/" + l] = Read(module, (IntPtr)type, n, l);
        }
      }
      var manifestNames = new List<IntPtr>(); EnumResourceNames(module, RT_MANIFEST, (m, t, n, p) => { manifestNames.Add(n); return true; }, IntPtr.Zero);
      foreach (var n in manifestNames) {
        if ((long)n != 1) throw new Exception("Unexpected manifest resource id " + (long)n + ".");
        var langs = new List<ushort>(); EnumResourceLanguages(module, RT_MANIFEST, n, (m, t, nn, l, p) => { langs.Add(l); return true; }, IntPtr.Zero);
        foreach (var l in langs) { manifests++; manifestLang = l; manifestBytes = Read(module, RT_MANIFEST, n, l); }
      }
      if (manifests != 1) throw new Exception("Expected exactly one manifest resource, found " + manifests + ".");
      lang = manifestLang; manifest = manifestBytes; return result;
    } finally { FreeLibrary(module); }
  }
  static Dictionary<string, byte[]> Sections(byte[] pe, bool refuseExtras) {
    int lfanew = BitConverter.ToInt32(pe, 0x3C);
    if (BitConverter.ToUInt32(pe, lfanew) != 0x4550) throw new Exception("Not a PE file.");
    int sections = BitConverter.ToUInt16(pe, lfanew + 6), optional = BitConverter.ToUInt16(pe, lfanew + 20), opt = lfanew + 24;
    bool pe32plus = BitConverter.ToUInt16(pe, opt) == 0x20B; int dirs = opt + (pe32plus ? 112 : 96);
    if (refuseExtras && BitConverter.ToUInt32(pe, dirs + 4 * 8 + 4) != 0) throw new Exception("Signed executables cannot be patched.");
    var result = new Dictionary<string, byte[]>(); long end = 0;
    for (int i = 0; i < sections; i++) {
      int s = opt + optional + 40 * i;
      string name = Encoding.ASCII.GetString(pe, s, 8).TrimEnd('\0');
      int virtualSize = BitConverter.ToInt32(pe, s + 8), rawSize = BitConverter.ToInt32(pe, s + 16), raw = BitConverter.ToInt32(pe, s + 20);
      end = Math.Max(end, (long)raw + rawSize);
      if (name != ".rsrc") { var bytes = new byte[Math.Min(virtualSize, rawSize)]; Array.Copy(pe, raw, bytes, 0, bytes.Length); result[name] = bytes; }
    }
    if (refuseExtras && end != pe.Length) throw new Exception("Executables with appended data cannot be patched.");
    return result;
  }
  public static string SetLevel(string exe, string level) {
    var before = File.ReadAllBytes(exe); var sectionsBefore = Sections(before, true);
    ushort lang; byte[] oldManifest; var others = Others(exe, out lang, out oldManifest);
    string text = new UTF8Encoding(false, true).GetString(oldManifest);
    bool bom = text.Length > 0 && text[0] == '\uFEFF'; if (bom) text = text.Substring(1);
    text = text.TrimEnd('\0');
    var pattern = new System.Text.RegularExpressions.Regex("(requestedExecutionLevel\\s[^>]*?level\\s*=\\s*\")[A-Za-z]+(\")");
    if (pattern.Matches(text).Count != 1) throw new Exception("Expected exactly one requestedExecutionLevel.");
    string patched = pattern.Replace(text, "${1}" + level + "${2}");
    var data = new UTF8Encoding(false).GetBytes(patched);
    if (bom) data = new byte[] { 0xEF, 0xBB, 0xBF }.Concat(data).ToArray();
    IntPtr update = BeginUpdateResource(exe, false);
    if (update == IntPtr.Zero) throw new Exception("BeginUpdateResource failed (" + Marshal.GetLastWin32Error() + ").");
    if (!UpdateResource(update, RT_MANIFEST, (IntPtr)1, lang, data, (uint)data.Length)) { EndUpdateResource(update, true); throw new Exception("UpdateResource failed (" + Marshal.GetLastWin32Error() + ")."); }
    if (!EndUpdateResource(update, false)) throw new Exception("EndUpdateResource failed (" + Marshal.GetLastWin32Error() + ").");
    ushort lang2; byte[] newManifest; var othersAfter = Others(exe, out lang2, out newManifest);
    if (lang2 != lang || !newManifest.SequenceEqual(data)) throw new Exception("The manifest did not read back.");
    if (othersAfter.Count != others.Count || others.Any(k => !othersAfter.ContainsKey(k.Key) || !othersAfter[k.Key].SequenceEqual(k.Value))) throw new Exception("Icon or version resources changed.");
    var sectionsAfter = Sections(File.ReadAllBytes(exe), false);
    foreach (var s in sectionsBefore) { byte[] got; if (!sectionsAfter.TryGetValue(s.Key, out got) || !got.SequenceEqual(s.Value)) throw new Exception("Section " + s.Key + " changed."); }
    IntPtr mapped = LoadLibraryEx(exe, IntPtr.Zero, DONT_RESOLVE);
    if (mapped == IntPtr.Zero) throw new Exception("The patched file no longer maps as an image (" + Marshal.GetLastWin32Error() + ").");
    FreeLibrary(mapped);
    return "requestedExecutionLevel=" + level;
  }
}
}
'@
}
$target = (Resolve-Path -LiteralPath $Executable).Path
$work = Join-Path ([IO.Path]::GetDirectoryName($target)) ('.manifest-' + [Guid]::NewGuid().ToString('N') + '.exe')
Copy-Item -LiteralPath $target -Destination $work
try {
  $result = [Multitap.ManifestPatch]::SetLevel($work, $Level)
  $policy = Get-MultitapExecutionPolicy $work
  if ($policy.level -ne $Level -or $policy.uiAccess -ne 'false') { throw "Manifest policy check failed after patching: $($policy.level)" }
  Move-Item -LiteralPath $work -Destination $target -Force
  Write-Output "Manifest of $([IO.Path]::GetFileName($target)): $result."
} finally {
  if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Force }
}

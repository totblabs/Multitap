$ErrorActionPreference = 'Stop'
if (-not ('Multitap.ExecutableManifest' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace Multitap {
  public static class ExecutableManifest {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr LoadLibraryExW(string file, IntPtr reserved, uint flags);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool FreeLibrary(IntPtr module);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll", SetLastError=true)] static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr resource);
    public static string Read(string path) {
      // Data/image resource loading only: no executable entry point or DLL initialization runs.
      var module=LoadLibraryExW(path,IntPtr.Zero,0x22);
      if(module==IntPtr.Zero)throw new Win32Exception(Marshal.GetLastWin32Error());
      try {
        var resource=FindResourceW(module,new IntPtr(1),new IntPtr(24));
        if(resource==IntPtr.Zero)throw new Win32Exception(Marshal.GetLastWin32Error());
        uint size=SizeofResource(module,resource);
        if(size==0||size>65536)throw new InvalidOperationException("Invalid executable manifest size.");
        var data=LockResource(LoadResource(module,resource));
        if(data==IntPtr.Zero)throw new Win32Exception(Marshal.GetLastWin32Error());
        var bytes=new byte[(int)size];Marshal.Copy(data,bytes,0,bytes.Length);
        return new UTF8Encoding(false,true).GetString(bytes).TrimStart('\uFEFF').TrimEnd('\0');
      } finally {FreeLibrary(module);}
    }
  }
}
'@
}
function Get-MultitapExecutionPolicy([string]$Path) {
  $resolved=(Resolve-Path -LiteralPath $Path).Path
  [xml]$manifest=[Multitap.ExecutableManifest]::Read($resolved)
  $nodes=$manifest.SelectNodes("//*[local-name()='requestedExecutionLevel']")
  if($nodes.Count -ne 1){throw "Missing or ambiguous execution policy: $resolved"}
  [pscustomobject]@{path=$resolved;level=[string]$nodes[0].level;uiAccess=[string]$nodes[0].uiAccess}
}
function Assert-MultitapAdministratorExecutable([string]$Path) {
  $policy=Get-MultitapExecutionPolicy $Path
  if($policy.level -ne 'requireAdministrator' -or $policy.uiAccess -ne 'false'){throw "Executable must require administrator approval: $Path"}
  $policy
}
function Assert-MultitapAsInvokerExecutable([string]$Path) {
  # The Windows client runs unelevated: it must not
  # ask for administrator approval; only the on-demand steps do.
  $policy=Get-MultitapExecutionPolicy $Path
  if($policy.level -ne 'asInvoker' -or $policy.uiAccess -ne 'false'){throw "Executable must run with the caller's token (asInvoker): $Path"}
  $policy
}
function Test-MultitapUnelevatedLaunchDenied([string]$Path) {
  $policy=Assert-MultitapAdministratorExecutable $Path
  $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
  try{$administrator=([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)}finally{$identity.Dispose()}
  if($administrator){return [pscustomobject]@{path=$policy.path;manifestVerified=$true;directLaunchDenied=$null;reason='Already elevated; direct-launch denial was not tested.'}}
  $process=[Diagnostics.Process]::new();$process.StartInfo=[Diagnostics.ProcessStartInfo]::new($policy.path)
  $process.StartInfo.UseShellExecute=$false
  try {
    try{$started=$process.Start()}catch{
      $errorObject=$_.Exception
      while($errorObject.InnerException){$errorObject=$errorObject.InnerException}
      if($errorObject -is [ComponentModel.Win32Exception] -and $errorObject.NativeErrorCode -eq 740){return [pscustomobject]@{path=$policy.path;manifestVerified=$true;directLaunchDenied=$true;windowsError=740}}
      throw
    }
    if($started){$process.Kill();$process.WaitForExit(5000)|Out-Null}
    throw "Windows unexpectedly permitted an unelevated direct launch: $Path"
  }finally{$process.Dispose()}
}

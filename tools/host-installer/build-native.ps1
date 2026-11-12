# Builds the Host executables (MultitapHost.exe, MultitapHostTool.exe) with the release
# configure options and runs the native fixtures.
#   powershell -ExecutionPolicy Bypass -File tools\host-installer\build-native.ps1 -Source apps\windows_host -Build .artifacts\host-native
param(
  [Parameter(Mandatory)][string]$Source,
  [Parameter(Mandatory)][string]$Build,
  [int]$Parallel = [Math]::Min(12, [Environment]::ProcessorCount),
  [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$Source = [IO.Path]::GetFullPath((Join-Path $root $Source)); $Build = [IO.Path]::GetFullPath((Join-Path $root $Build))
. (Join-Path $root '.tools\msvc-portable\env.ps1')
$cmake = Join-Path $root '.tools\cmake\cmake-3.31.8-windows-x86_64\bin\cmake.exe'
$git = Join-Path $root '.tools\git\cmd\git.exe'
$sdk = Join-Path $root '.tools\libvirtualdisplay\libvirtualdisplay-b6874684d82925d0d76f487d8e4ba3ec71c9721a'
$x264 = Join-Path $root '.tools\x264'
$nasm = Join-Path $root '.tools\nasm-2.16.03\nasm.exe'
$opusArchive = Join-Path $root '.tools\opus\1.6.1\opus-1.6.1.tar.gz'
foreach ($p in $Source, $cmake, $git, $sdk, $x264, $nasm, $opusArchive) { if (-not (Test-Path -LiteralPath $p)) { throw "missing: $p" } }

New-Item -ItemType Directory -Force $Build | Out-Null
# Opus is configured from the pinned local tarball only (no download).
$opusRoot = Join-Path $Build 'opus-src'
if (-not (Test-Path (Join-Path $opusRoot 'opus-1.6.1\CMakeLists.txt'))) {
  New-Item -ItemType Directory -Force $opusRoot | Out-Null
  Push-Location $opusRoot; try { & $cmake -E tar xzf $opusArchive; if ($LASTEXITCODE) { throw 'opus extract failed' } } finally { Pop-Location }
}
$opusSource = Join-Path $opusRoot 'opus-1.6.1'
$tree = Join-Path $Build 'tree'
# The sign-in streaming backend is not built: the in-stream sign-in screen
# is always off (signin_stream_preference.hpp signinStreamingEnabled() returns
# false), so no request can reach it, and leaving it out takes its SYSTEM code paths out of the service.
$configure = @('-S', $Source, '-B', $tree, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
  "-DMULTITAP_NATIVE_DISPLAY_SDK=$sdk", "-DMULTITAP_X264_SRC=$($x264.Replace('\','/'))", "-DMULTITAP_NASM=$($nasm.Replace('\','/'))",
  "-DGIT_EXECUTABLE:FILEPATH=$git", '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
  "-DMULTITAP_OPUS_ARCHIVE:FILEPATH=$opusArchive", "-DFETCHCONTENT_SOURCE_DIR_MULTITAP_OPUS:PATH=$opusSource",
  '-DMULTITAP_PRIVATE_SIGNIN_DIAGNOSTIC_TRIAL=OFF', '-DMULTITAP_SIGNIN_BACKEND=OFF')
& $cmake @configure; if ($LASTEXITCODE) { throw 'configure failed' }
& $cmake --build $tree --config Release --parallel $Parallel --target MultitapHost MultitapHostTool; if ($LASTEXITCODE) { throw 'build failed' }
if (-not $SkipTests) {
  $tests = @('MultitapHostWorkerServiceBindingTest','MultitapHostWorkerExitFenceTest','MultitapHostSessionStopTest','MultitapHostDriveTest','MultitapHostDriveFsTest','MultitapHostDriveOpenTest','MultitapHostCatalogMaintenanceTest','MultitapHostSigninNativeTest','MultitapHostSigninRestorationTest','MultitapHostSigninServiceAuthorityTest','MultitapHostSigninPreparationTest','MultitapHostSigninPreparationAuthorityTest','MultitapHostSigninBridgePolicyTest','MultitapHostSigninOwnedReleaseTest','MultitapHostDisplayV2ServiceTest','MultitapHostSettingsRpcTest','MultitapHostWindowsAccountRecordTest','MultitapHostServiceControlSecurityTest','MultitapHostInstallerTest',
    # The finish of a first install (virtual display trust, device, selection) and what an unattended update reports without it.
    'MultitapHostPrerequisitesTest','MultitapHostUpdateTest',
    # What removing the Host takes away (device, driver package, the certificate Multitap added), and that an update removes none of it.
    'MultitapHostRemovalTest',
    # A connection that comes back (resume, side channels dialled again), the cursor's size above 100 %, standard accounts, Console.
    # USB: descriptor policy (shared vectors), session core, controller protection sequence (no driver installed).
    'MultitapHostUsbRedirectionTest',
    # The UAC helper's scope: SYSTEM on Winlogon of the unlocked session, and a security prompt in front.
    'MultitapHostSecureDesktopScopeTest','MultitapHostSecureDesktopBridgeTest',
    # Printing: queue lifecycle with a substituted spooler (a queue that cannot be deleted is quarantined), listener.
    'MultitapHostPrintRedirectionTest',
    'MultitapHostResumeTest','MultitapHostChannelRedialTest','MultitapHostClipboardReconnectTest','MultitapHostClipboardFilesTest','MultitapHostCursorTest','MultitapHostCursorShownSizeTest','MultitapHostStandardUserAccessTest','MultitapHostQemuConsoleTest',
    # The Console's 한/영: what the session's IME reading means, the wire, when the user's helper runs.
    'MultitapHostImeStateTest',
    # Korean and English: every text of the installers, tray and service in both languages (docs/i18n.md).
    'MultitapHostUiTextTest',
    # A registration copied with the disk (full clone) is not used on another machine (host-machine-v1).
    'MultitapHostCredentialBindingTest','MultitapHostPendingConnectionTest')
  & $cmake --build $tree --config Release --parallel $Parallel --target @tests; if ($LASTEXITCODE) { throw 'test build failed' }
  foreach ($t in $tests) { & (Join-Path $tree "$t.exe") | Out-Null; if ($LASTEXITCODE) { throw "test failed: $t" }; "ok $t" }
}
foreach ($e in 'MultitapHost.exe', 'MultitapHostTool.exe') { $p = Join-Path $tree $e; "{0} {1} {2}" -f $e, (Get-Item $p).VersionInfo.ProductVersion, (Get-FileHash $p).Hash.ToLowerInvariant() }

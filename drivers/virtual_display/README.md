# Multitap display helper (legacy MttVDD provider)

`MultitapDisplay.exe` is a small project-owned console helper (version in
`CMakeLists.txt`) that installs and manages the upstream signed MttVDD virtual
display driver for the Host's legacy display provider. The current provider is
libvirtualdisplay with the SunshineVirtualDisplayDriver
(`drivers/display_bridge/`). Every Host package still carries this helper: the
installer verifies the package with it, and Hosts without the native provider
use it for their virtual outputs. On a Host whose native provider is confirmed,
the service removes a proven leftover MttVDD adapter on its own
(`apps/windows_host/src/display_adapter_inventory.hpp`).

The signed upstream files are never patched or rebuilt. The helper itself is
not signed; it is a normal program run by the Host.

## Pinned package and licence

The driver is
[VirtualDrivers/Virtual-Display-Driver release 25.7.23](https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/tag/25.7.23),
source commit `d437ebc9b44a14ce6e5cc9c8b7f6beb08d6faf77`. `upstream.json` pins
the archive, the SHA-256 of `MttVDD.inf`, `MttVDD.dll` and `mttvdd.cat`, the
signer thumbprint, the settings file and the licence. The asset name says
`x86`, but the INF targets `Standard.NTamd64`: these are x64 files.

The upstream MIT notice is kept verbatim as `LICENSE.upstream.txt`. It applies
to the upstream driver, not to the helper. The DLL and catalog carry a valid
Authenticode signature from **SignPath Foundation** (pinned thumbprint); this is
not a Microsoft WHQL signature. The helper additionally checks the fixed file
hashes, the INF and DLL catalog membership and the exact settings hash, so an
edited manifest next to the files cannot replace the embedded pins. The driver
source was reviewed at the pinned commit; that does not establish a
reproducible source-to-binary build.

## Build

```powershell
powershell -File tools\build-virtual-display.ps1 [-SelfTest] [-Product <folder>]
```

The script downloads the archive into `.artifacts/virtual-display/25.7.23`,
checks the archive hash, the three driver file hashes, the signer thumbprint,
the licence hash and the settings hash, and builds the helper with the Visual
Studio Build Tools in `.tools/`. The default output folder is
`.artifacts/virtual-display/product-<version>`:

```text
MultitapDisplay.exe
README.VirtualDisplay.md      (this file)
<VC runtime DLLs>
driver/MttVDD.inf
driver/MttVDD.dll
driver/mttvdd.cat
driver/vdd_settings.xml
driver/upstream.json
driver/LICENSE.upstream.txt
```

`-SelfTest` runs the non-installing commands (`verify`, `status`,
`warp-test`, `plan-test`, `topology-test`, `viewport-test`, `json-test`), checks
that `status` reports the built version and that the settings file declares two
monitors.

## Commands

Arguments have no leading dashes. Each command prints one JSON object. Errors
exit with code 1 and a fixed `error` code. All JSON strings are ASCII, with
other characters escaped as `\uXXXX`, so Windows PowerShell 5's console code
page cannot corrupt adapter names.

| Command | What it does |
|---|---|
| `verify` | Fixed hashes, Authenticode signer and catalog membership. Installs nothing. |
| `status` | Read-only device and display state of the current session. |
| `install` | Administrator. Refuses any MttVDD instance or vendor path it does not own, stages the pinned INF with `SetupCopyOEMInf`, creates one root device and binds that INF to that device with `DiInstallDevice` (not `DiInstallDriver`, which would also update other matching devices). |
| `configure` | Administrator, interactive console. Activates the two owned outputs at 1920x1080 60 Hz to the right of the existing screens. Never selects another display or changes its primary flag. |
| `name` | Administrator. Names the exact owned adapter `Multitap Virtual Display Adapter` and, when both have a proven PnP parent chain to it, its monitors `Multitap Display1`/`Multitap Display2` (friendly name only). |
| `remove` | Administrator. Removes only the device instance recorded in `HKLM\SOFTWARE\Multitap\Display\DeviceInstance`. Driver-store files stay. |
| `topology 1`, `topology 2` | Interactive console. Changes how many of the owned adapter's two outputs are active. Keeps the current primary, every other display and the active owned modes; newly attached outputs need an enumerated 1920x1080 60 Hz mode. |
| `topology-status` | Read-only check of the `topology` prerequisites. |
| `resize "\\.\DISPLAYn" w h hz` | Interactive console. Applies an already enumerated mode to one owned output and rolls back that output on failure. |
| `viewport "\\.\DISPLAYn" w h 60` | Administrator. Arbitrary owned output size (below). |
| `viewport-status` | Read-only: whether `viewport` can run now. Uses `SDC_VALIDATE` only. |
| `viewport-recover` | Administrator, unlocked console. Finishes an interrupted `viewport` or `mode-catalog` change. |
| `mode-catalog`, `mode-catalog-status` | Register the common mode list (below); the status form is read-only. |
| `repair-plan`, `repair-install <planId>` | Reviewed cleanup of conflicting MttVDD devices (below). |
| `plan-test`, `topology-test`, `viewport-test`, `json-test`, `warp-test` | Pure fixtures; `warp-test` creates a D3D11 WARP device and does not touch the driver. |

The Host calls only `status`, `topology-status`, `viewport-status`,
`viewport-recover`, `mode-catalog`, `mode-catalog-status`, `topology 1|2`,
`viewport`, `repair-plan` and `repair-install`
(`apps/windows_host/src/machine.hpp`); the installer uses `verify`, `status`,
`install`, `configure` and `mode-catalog`.

## Ownership and readiness

The owned device is the exact `Root\MttVDD` instance recorded under
`HKLM\SOFTWARE\Multitap\Display`. The upstream driver's own path value
(`HKLM\SOFTWARE\MikeTheTech\VirtualDisplayDriver`) is shared, so the helper
reports when it points somewhere else and clears it on `remove` only if it is
still the path this installation recorded. Other display products appear in
`status` as other adapters and are never changed.

`status` reports `installed`, `deviceReady`, `owned`, `deviceCount`,
`foreignDeviceCount`, the active owned `displays[]` (connector id, device name,
rectangle in physical pixels, refresh rate, primary flag and up to 64
enumerated modes), `configurationPending` with a `reason`, `recoveryRequired`,
`rebootRequired` and bounded diagnostics (adapter friendly names, status flags
and problem codes; no machine UUID, raw instance ids, user paths or
credentials).

A Host may treat the display as ready only when the device is owned, started,
the only matching one, and has valid active outputs **in the capture worker's
session**. Session 0, or a remote (RDP) Windows session, cannot see the
console's active displays; there the helper answers
`configurationPending: true` with `interactive_console_required`.
`rebootRequired` is true only when Windows reports `DN_NEED_RESTART` or
`CM_PROB_NEED_RESTART` on the exact owned adapter. The helper never restarts
Windows.

Outputs `0`/`1` follow the order of the two `connectorInstance` values, not GDI
display names or the driver's random monitor container ids, so they stay stable
when positions or names change.

## Mode catalog

The pinned settings file lists only 1920x1080 and 1280x720 at 60 Hz, for two
monitors, and the driver reads modes only when its device starts.
`mode-catalog` (revision 1) adds 16 common 60 Hz modes to the installed copy of
`vdd_settings.xml` (`common_modes.hpp`): 1024x768, 1280x800, 1280x1024,
1366x768, 1440x900, 1600x900, 1600x1200, 1680x1050, 1920x1200, 2048x1152,
2560x1080, 2560x1440, 2560x1600, 3440x1440, 3840x2160 and 1080x1920.

It restarts only the owned adapter, waits up to 30 s until all 18 modes are
enumerated on every active owned output, then restores and verifies the
previous one- or two-output topology. It does not change the current
resolution. Displays may blink during the restart. When the change cannot run
now (Windows restart pending, no unlocked console, owned output inactive or
device not ready) it exits 0 with `deferred: true`. A failure restores the
previous settings file and topology; an incomplete recovery keeps its journal
for `viewport-recover`. The Host allows it 85 s.

## Arbitrary output size (`viewport`)

If Windows already enumerates the requested mode, `viewport` applies it to the
owned output without restarting anything. Otherwise:

1. Write a protected journal (`multitap-viewport-pending.txt`) with the previous
   settings file, adapter identity and full active display configuration, and
   keep the pinned original as `multitap-viewport-original.xml`.
2. Add the mode to the one Multitap block in the installed settings file. At most
   32 added modes are kept: catalog modes and every active owned mode stay,
   older inactive additions go. A settings file edited by someone else is
   refused, not overwritten.
3. Restart only the owned adapter (`DIF_PROPERTYCHANGE` / `DICS_PROPCHANGE`) and
   wait until Windows enumerates the exact mode. A restart call or an updated
   file alone is never success.
4. Re-apply the saved configuration with the owned paths mapped to their new
   adapter LUIDs and ids by connector, the other displays' modes copied byte for
   byte, and the owned output at the requested size, without
   `SDC_ALLOW_CHANGES`. Then the primary and every other rectangle and mode must
   match the saved configuration.

Sizes are even, at least 320x180, within the shared frame-layout limits
(`native/windows_video/frame_layout.hpp`), at 60 Hz. A requested size that would
overlap a fixed foreign screen is refused. A failure restores the previous
settings file and topology. An incomplete recovery leaves `recoveryRequired`
with `viewport_recovery_required`; the mutating commands then refuse until
`viewport-recover` (the Host allows it 75 s) restores the journal and verifies
a fresh topology. A killed helper also leaves the journal.

Journals and backups are readable only by SYSTEM and Administrators and are
written to flushed temporary files and swapped in with `ReplaceFileW`. The
runtime settings file stays readable by authenticated users so the driver's
UMDF host can read it. Directory handles prevent rename or reparse-point swaps.
Restarting the adapter can make displays blink; it is not a seamless resize.

## Reviewed repair

Host settings can show the result of `repair-plan`: every conflicting display
device and leftover shared setting it would clean up. `repair-install <planId>`
runs only after the user confirms that list. The plan id is a SHA-256 over the
observed adapter identities, driver bindings and three registry values, so any
change in between refuses the repair. Only exact `Root\MttVDD` devices can be
removed; Microsoft basic, remote, Proxmox and unrelated devices are excluded. It
requires administrator rights and the pinned trusted package, writes a
protected backup of the settings first, and reports confirmed removals,
acknowledged but unconfirmed removals, registry restoration and an incomplete
installation separately. Removed third-party devices are not reinstalled
automatically, and Windows is never restarted.

## Upstream control pipe (unresolved)

The pinned driver source creates `\\.\pipe\MTTVirtualDisplayPipe` with the DACL
`D:(A;;GA;;;WD)` (everyone) and accepts commands such as `RELOAD_DRIVER`,
`SETDISPLAYCOUNT` and `SETGPU`. A local user could therefore reload the driver
or change its monitor count. This is a static source finding, not a tested
exploit. Multitap does not use the pipe and does not harden it; the valid
signature does not change this.

## Tests without installing

Run `powershell -File tools\test-virtual-display.ps1 -Product <product folder>`
from a **non-elevated** shell. It checks `verify` and `status`, a WARP device,
the pure layout, ownership and diagnostics fixtures, rejection of altered DLL,
INF, CAT and settings files even with a forged manifest, missing files, and the
administrator refusals of `install`, `configure`, `name` and `remove`. It
compares the display and monitor devices, display modes, related registry
values and the driver store before and after. Temporary copies stay in a
uniquely named project QA folder and are removed.

The CMake project also has unshipped unit tests (`MultitapViewportTest`,
`MultitapModeCatalogTest`, `MultitapRepairTest`, `MultitapSourceMappingTest`) that
use injected state and make no device or registry changes. None of these is a
test of the installed driver.

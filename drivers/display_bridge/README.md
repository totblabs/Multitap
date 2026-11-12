# Multitap bridge to libvirtualdisplay

Header-only adapter between the Host and the unmodified MIT
[`Nonary/libvirtualdisplay`](https://github.com/Nonary/libvirtualdisplay) control
SDK (v1.6.5, source commit `b6874684d82925d0d76f487d8e4ba3ec71c9721a`), which
drives the upstream SunshineVirtualDisplayDriver. This is the Host's native
display provider (see [ARCHITECTURE.md](../../ARCHITECTURE.md), "Displays").

A native Host build (`tools/host-installer/build-native.ps1`, CMake option
`MULTITAP_NATIVE_DISPLAY_SDK`) compiles only the SDK's control library from the
pinned source and links it statically; it includes these headers. Nothing here
installs a driver, imports a certificate or touches a display it did not create.

| File | What it does |
|---|---|
| `provider.hpp` | `Session`: one driver lease (id at least `kMinOpaqueLeaseId`, owner capability) with one or two temporary outputs named `Multitap Display 1` / `2`, 60 Hz, EDID size following the orientation. Starts only when the lease does not exist yet; 15 s lease timeout kept alive by `heartbeat`; adds or removes one output; waits up to 3 s for departures; releases the lease. |
| `runtime.hpp` | `Controller`, the thread-safe wrapper the Host service uses (start, reconfigure, heartbeat, stop, recovery state), and `quiescentOwnedLease`, a read-only check that an interrupted lease is really gone. |
| `lease_recovery.hpp` | Output size checks (frame layout plus the SDK's limits) and the live-lease recovery plan that moves verified outputs from an interrupted lease to a new one. |
| `windows_endpoint.hpp` | Opens the driver's control device only by the exact interface path or PnP instance that the Multitap installation recorded; never the first device found. |

The Host's records for these leases (state files, recovery after a crash or
reboot) are described in [docs/native-display-recovery.md](../../docs/native-display-recovery.md)
and [docs/display-manager-v2.md](../../docs/display-manager-v2.md).

## Pins and licence

- `apps/windows_host/third_party/libvirtualdisplay/upstream.json` pins the source
  archive (SHA-256 and a tree digest), the release package, the four driver files
  (INF, DLL, CAT, CER) and the signer thumbprint.
- `tools/fetch-libvirtualdisplay.ps1` downloads and checks both into
  `.tools/libvirtualdisplay` and prints the two folders;
  `-VerifyOnly` checks existing folders without downloading.
- `UPSTREAM-LICENSE.txt` is the upstream MIT licence. The Host package repeats it
  in `THIRD-PARTY-NOTICES.txt`; the apps show it as `libvirtualdisplay-LICENSE.txt`.

## Driver signature

The upstream release catalog is signed with the upstream project's own release
certificate (`CN=Sunshine Virtual Display Release Signing`), not by Microsoft.
Host settings checks the four file hashes and that signer, shows the signer
to the administrator, and adds the certificate to the machine's Root and
TrustedPublisher stores before it installs the driver
(`apps/windows_host/src/host_prerequisites.hpp`). Multitap ships the files
unchanged; changing a byte would break the catalog.

## Tests

The three bridge tests run the real upstream `ControlClient` serialization over
an in-memory transport: no driver, device or Windows display is touched.

```powershell
cmake -S drivers/display_bridge -B <build folder> -DVDD_SDK_SOURCE=<.tools/libvirtualdisplay/libvirtualdisplay-2a29ea71a52d15522c95b3de532b6612c54d75cc>
cmake --build <build folder> --config Release
```

Targets: `MultitapVirtualDisplayBridgeTest`, `MultitapNativeDisplayRuntimeTest`,
`MultitapNativeDisplayLeaseRecoveryTest`. The Host's service-side native display
tests build and run with `tools/host-installer/build-native.ps1`.

# Windows automatic login: service hooks QA

Tests for the parts that connect the automatic-login controller
([`tools/windows-autologon-qa`](../windows-autologon-qa/README.md)) to the Host
service and the Host tool.

## Hooks

- **Service start** (`apps/windows_host/src/windows_autologon_hooks.hpp`):
  after `SERVICE_RUNNING` the service schedules exactly one tracked threadpool
  launch of `MultitapHostTool.exe --windows-autologon-startup-recovery`. Path,
  ACL, version-marker and image checks and `CreateProcess` stay off the service
  and stream threads. A queued start is dropped after shutdown or after a
  30-second queue window; the callback holds its own duplicate of the service
  stop event and checks it at admission and again before `CreateProcess`. The
  child's process handle, PID, creation time and image are verified. A
  30-second observer timeout never kills or relaunches the child: the child
  owns the global mutex until it actually exits, and a successor service cannot
  admit a second mutation through that mutex. Recovery results never affect
  broker, display or streaming readiness or the service start, and the QA
  service skips the hook.
- **Host tool commands** (fixed, local administrator, no credential arguments):
  `--windows-autologon-startup-recovery` (exact-image repair),
  `--windows-autologon-update-journal-check` (read-only: no mutex re-entry, no
  LSA/Winlogon/account access, no repair), and
  `--windows-autologon-uninstall-cleanup --confirmed` (cleans only
  product-owned automatic-login state; without `--confirmed` it is refused). The
  cleanup result reports `productRemovalIntegrated:false`: removing product
  files is a separate step, and a result other than cleaned/unchanged keeps the
  tool and its recovery record.

## Tests

| Test | How to run | What it covers |
|---|---|---|
| `MultitapHostAutologonHooksMemoryTest` (`memory_test.cpp`) | this folder's CMake project, or the Host CMake target of the same name | queue admission, observation, the serialised update gate and cleanup-receipt predicates, with no Windows headers, files or processes |
| `MultitapHostAutologonLinkTest` (`native_link_test.cpp`) | Host CMake target (not built by default) | compiles and links the real native hooks; its `main` only prints fixed JSON and never calls them |
| `MultitapHostAutologonCodecMemoryTest`, `MultitapHostAutologonLegacyCodecOracle` (`native_codec_test.cpp`) | Host CMake targets (not built by default) | the native record codec, and the older schema-1 record format |
| `windows_autologon_hooks_navigation_test.dart` | `..\..\flutter.cmd test` in `apps\windows_client` | the Host settings navigation entry |

```powershell
$cmake = '.tools/visualstudio-buildtools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S tools/windows-autologon-hooks-qa -B <build> -A x64
& $cmake --build <build> --config Release
& <build>/Release/MultitapHostAutologonHooksMemoryTest.exe
```

None of these performs a real account change, service restart, installation
or boot. Automatic login does not make the sign-in screen or the secure desktop
streamable.

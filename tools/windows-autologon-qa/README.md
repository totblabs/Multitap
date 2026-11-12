# Windows automatic login: controller memory test

The Host settings **Windows 로그인** page stores the VM's shared Windows account
once and turns on Windows automatic login, so a streaming connection finds a
signed-in desktop. The controller is `apps/windows_host/src/windows_autologon.hpp`
(portable logic) with its Windows adapter in `windows_autologon_native.hpp`.
This folder tests the controller in memory: no Windows headers, registry, LSA,
credentials, logon or network.

## What the controller does

- Local Host-settings RPC only (`windowsAutologonStatus`,
  `windowsAutologonApply` with `operationId`, `expectedRevision`,
  `confirmed:true`, `username`, `password`, `passwordConfirmation`, plus
  `windowsAutologonDisable` and `windowsAutologonRepair`); never the broker,
  the client login or the relay. Rejections are fixed codes; no Windows
  exception text or credential is serialised.
- Status distinguishes `off`, `configured`, `observed`, `needs_update`,
  `external`, `policy_blocked`, `unknown` and `failed`, with an opaque SHA-256
  revision and its own operation receipt. `observed` needs an actual token SID,
  console session and matching account. A read failure is never reported as
  off.
- The adapter touches only six Winlogon values (`AutoAdminLogon`,
  `DefaultUserName`, `DefaultDomainName`, `ForceAutoLogon`, `AutoLogonCount`,
  `DefaultPassword`) in the 64-bit registry view and the LSA `DefaultPassword`
  secret. A plaintext Winlogon `DefaultPassword` is only checked for presence,
  never read, written or deleted. An existing LSA secret, enabled automatic
  login, forced/count settings or unclear ownership are not taken over. Windows
  policies are observed, never weakened. Local accounts are canonicalised;
  Internet identities are rejected. One explicit interactive validation checks
  the token SID; a bad password is never retried on a timer.
- The record `private/windows-autologon.dpapi` (machine DPAPI, in the
  SYSTEM/Administrators-only private directory, with owner/ACL/reparse checks
  and atomic replacement) holds the original baseline, the expected image and
  the one next intent. Every native write needs a durable intent and a matching
  current before-image, and the complete after-image is read back before the
  checkpoint advances. Enabling is the last step; disabling goes flag off →
  secret removal → original account values. Registry and LSA are not one atomic
  transaction, so an interrupted write keeps the pending record, and an
  explicit repair accepts only its exact before/after image.
- An RPC timeout never terminates an admitted operation: that process owns the
  machine lock until it exits, and the UI only reconciles status afterwards.

Local administrators can read LSA/DPAPI-protected secrets. Automatic login does
not make the sign-in screen or the secure desktop streamable.

## Build and run

```powershell
$cmake = '.tools/visualstudio-buildtools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S tools/windows-autologon-qa -B <build> -A x64
& $cmake --build <build> --config Release
& <build>/Release/MultitapWindowsAutologonMemoryTest.exe
```

`memory_test.cpp` drives the real controller over an in-memory access layer:
confirmation and default-off, empty and mismatched input, a foreign secret,
plaintext/force/count values, policy, restricted and identity cases, the single
authentication attempt, enable-last and the exact disable order, duplicate
operation IDs, drift, observed versus configured, a fault before and after
every native write, save faults, repair and password change. It prints one JSON
line with its check and failure counts.

`native_compile_only.cpp` is a compile probe of the native adapter (no `main`).
The Flutter side is covered by `apps/windows_client/test/windows_autologon_test.dart`
and `windows_autologon_managed_test.dart` (fake Host, synthetic labels and
passwords, no native process). The service and installer hooks are tested in
`tools/windows-autologon-hooks-qa`.

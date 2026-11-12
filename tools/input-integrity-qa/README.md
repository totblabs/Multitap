# Input integrity QA

A Windows OS test of the Host's production `host::RemoteInput`
(`apps/windows_host/src/agent.hpp`, included unchanged) against an elevated
window. It shows why the Host's input injector must run at an integrity level
at least as high as the target: Windows (UIPI) silently drops input from a
lower-integrity process. It does not install or modify the Host or client,
register a service, connect to a VM, start Task Manager or change Windows
security settings.

A medium-integrity controller creates shared state. `run.ps1` starts an
elevated target and a separate elevated injector with
`Start-Process -Verb RunAs -WindowStyle Hidden`; only the target window is
shown. Both injectors verify the target's exact PID, executable path, owner and
Windows session. The target must stay in the foreground; held keyboard
modifiers or mouse buttons stop the test. Only F8 down/up and a centre
left-button down/up are sent. The target counts the actual Windows keyboard and
mouse messages, including an extra-info marker and independent unfiltered
counters. The cursor is restored afterwards unless the user moved it. All
processes have bounded lifetimes, and the target closes when its controller
disappears.

## Build and run

The test links a Release `host_engine.lib` from
`apps/windows_host/build/Release`, so configure and build the Host there first
(target `host_engine`). Then, from the repository root:

```powershell
$cmake = '.tools/visualstudio-buildtools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S tools/input-integrity-qa -B tools/input-integrity-qa/build -G 'Visual Studio 17 2022' -A x64
& $cmake --build tools/input-integrity-qa/build --config Release
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/input-integrity-qa/run.ps1
```

`-ExecutionPolicy Bypass` applies to this invocation only; the script does not
change the machine or user policy. Windows asks for consent for both elevated
processes. No credentials or token/SID contents are printed or stored. Results
and source/binary hashes go to `tools/input-integrity-qa/results/<run-id>/`.

## Expected result

| Phase | SendInput calls | Reported inserted | Target F8 down/up | Target left down/up |
| --- | ---: | ---: | --- | --- |
| Medium injector → high target | 6 | 6 | 0 / 0 | 0 / 0 |
| High injector → same high target | 6 | 6 | 1 / 1 | 1 / 1 |

The high phase is the positive control for the same target and counters. The
`SendInput` return value alone is not evidence of delivery, and Windows does
not report UIPI as the cause
([SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)).
The harness consumes the launcher's initial `SW_HIDE` show state before it
shows its own target
([ShowWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-showwindow)).

This does not test physical keys, a relayed session, Korean composition, the
Winlogon/UAC secure desktop, protected or system-integrity windows, or the
Host's worker token selection.

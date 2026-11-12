# Console display mode

The emergency console shows the VM's QEMU display through Proxmox VNC
([browser-console-v1.md](browser-console-v1.md)). Windows does not draw its
desktop there while Multitap's virtual outputs are the active displays, so the
console would show a black or secondary screen. When the Host supports it,
opening the console therefore asks the Host to make the QEMU output the primary
display and switch the owned virtual outputs off for as long as the console is
open. Streaming still uses only the owned outputs; the console never becomes a
streaming source.

Protocol `qemu-console-v2`. Code: `server/src/qemu-console-preparation.mjs`
(`QemuConsolePreparation`), `server/src/console-relay.mjs`,
`apps/windows_host/src/qemu_console.hpp` (`prepareQemuConsoleOutput`),
`qemu_console_protocol.hpp`, and `qemuConsoleControl` in `service.hpp`.

## When the broker asks

While it issues a console grant, the broker sends
`host.qemu-console.prepare {operationId, nonce, machineUuid, model}` only when:

- the VM's Host is connected, its capabilities are fresh and advertise
  `qemuConsoleProtocol: 'qemu-console-v2'` (Hosts running display manager v2
  with the native provider);
- the VM's Proxmox configuration has the standard VGA display (`model: 'std'`,
  no primary passthrough GPU, see `server/src/stream-console-profile.mjs`).

Otherwise the console still opens, with the preparation status `unknown`. That
is the path for older or offline Hosts and for passthrough GPUs, and it is why
the console works when Windows or the Host does not start. When the broker does
ask, the Host's enrolled machine UUID must match the VM, or the console request
fails with `console_identity_changed`.

## What the Host does

A bounded worker in the signed-in console session (the browser grant is not
display authority; the service passes the worker its current lease and
deadline):

1. Identify the QEMU output from the proof and the exact adapter identity,
   never from a display number. If it is already active, primary at (0,0), with
   no owned output active and nothing overlapping, answer
   `qemu_console_active` without changing anything.
2. Make sure a restore point exists. This is display manager v2's state file
   ([display-manager-v2.md](display-manager-v2.md)). A valid one must name
   this console as active, and the other displays must be as it recorded them.
   With no usable state (none, or one from an earlier boot) the current
   topology is stored as the restore point.
3. Plan: the QEMU output at (0,0); every other active display that is not an
   owned output kept with its mode and placed to its right; owned outputs left
   out, which switches them off. A console that is inactive and has no saved
   modes is first activated with modes Windows chooses (a topology-only
   request) and then placed the same way.
4. Immediately before each validate and apply, check again that the operation
   is still current, the console identity is unchanged, the other displays are
   unchanged and the state file is the one it read.
5. Settled means the console-only layout reads back twice in a row within
   2.5 s. Then the restore point is removed and the answer is
   `qemu_console_restored`.
6. On failure after a change, re-apply the configuration from before the
   change and remove a restore point this operation created only if that
   rollback verified. After a cancellation or identity loss the Host keeps the
   state file for the ordinary recovery instead of making another change with
   expired authority.

Answers use fixed reasons: `qemu_console_active` and `qemu_console_restored`
succeed; `qemu_console_inactive`, `_state_missing` (no restore point could be
stored), `_state_invalid`, `_identity_changed`, `_foreign_changed`,
`_unavailable`, `_cancelled`, `_busy` and `_timeout` fail. A
`qemu_console_unavailable` answer with nothing attempted gives the `unknown`
fallback; any other failure ends the console request with
`console_display_<reason>` (a running stream, for example, makes the Host
answer `qemu_console_busy`).

## Without an administrator's unlocked session: the console park

The worker above exists only in the unlocked console session of a Windows
administrator. Signed out, at the sign-in screen, locked, signed in over RDP
only, or with a standard user signed in, no worker can change the display
(`qemu_console_route.hpp` decides; the facts are the active console session,
its lock state and whether its worker token is an administrator's).

In those states the service itself prepares the console, without any Windows
session: the SDK owner stops its controller exactly like an orderly service
stop (`NativeDisplayService::parkForConsole`), the owned virtual outputs leave,
and Windows falls back to the displays that remain. The controller stays
stopped while the console's hold is renewed (the park outlasts the 30 s hold by
5 s and runs out by itself if no renewal comes) and cold-starts when the hold
ends, as after a service start; the saved display state, if a stream left one,
stays for the ordinary recovery at the next administrator sign-in.

- The answer is `qemu_console_restored` with `attempted: true`, so the broker
  holds and renews it like the worker's answer. What it proves is narrower:
  the owned outputs are gone (the SDK said so). That Windows then draws on the
  QEMU output is Windows' own fallback and is not read back, because session 0
  cannot query the console session's displays.
- A parked hold is bound to the machine and to the park, not to a Windows
  session or a lease: signing in through the console does not end it.
- The SDK owner refuses to park under a reservation, a pending change, any
  recovery, or display manager v1. Then nothing changes and the answer is
  `qemu_console_unavailable` with nothing attempted (the `unknown` fallback).
- Record: hostAction 215, stage 1 parked or refused (`cause` 1 nobody signed in
  at the console, 2 locked, 3 a standard user's session; `error` 1 the SDK
  owner refused, 2 cancelled), stage 2 ended.

## Holding the layout

A finished preparation is not yet a reservation of the display:

- The Host holds the console layout under a 30 s lease. The broker renews it
  every 10 s with `host.qemu-console.renew` and an increasing sequence; the
  Host answers `host.qemu-console.held`, and an answer must arrive within 5 s.
  Reading the status never renews it.
- While the hold lasts, the Host's idle display activation and automatic
  display recovery leave the topology alone, and a stream start is refused
  with `host_maintenance`. A Windows lock does not end the hold.
- The hold ends on console close (one exact `host.qemu-console.cancel`),
  revocation, lease expiry or Host loss. The broker then treats the Host's
  capabilities as pending until the Host reports quiescence and sends a fresh
  capability report, so no stream is admitted on the old view. After the
  release the Host re-arms the activation of its owned outputs for the same
  lease and session, and streaming readiness comes back through the ordinary
  path.

## What the browser shows

The console page polls the grant's preparation status every 10 s through an
authenticated, read-only request (`isolated` or `unknown`) and exposes it as a
data attribute and a status tooltip. A failed poll becomes `unknown`. A
received black frame proves only that data arrives, not that a usable desktop
or a primary display exists, so it never upgrades the status.

## Limits

Windows display changes need the unlocked console session of an administrator;
every other state uses the console park above, which removes the owned outputs
but cannot read back what Windows shows. When the SDK owner refuses to park,
the console opens with the `unknown` status, and if the QEMU output is inactive
at that moment the console shows no desktop. The park has been exercised with
the in-memory driver only; on a real signed-out VM it is unverified.

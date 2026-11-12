# Display manager v2

While a client streams, the Windows desktop consists of exactly the
Multitap-owned virtual outputs, one per client monitor, each at the size of the
client window. Every other display (the Proxmox console, a vGPU head, a
physical monitor) is off for the stream and comes back when it ends. Display
manager v2 is the Host code that makes this change and undoes it. It runs with
the native display provider (`drivers/display_bridge/`); see
[native-display-recovery.md](native-display-recovery.md) for how the provider's
own outputs survive service restarts.

The rules:

- One state file remembers how Windows looked before the stream changed it.
- Start, resize, count change and stop verify the result from a fresh Windows
  display query. A failure restores the saved state and the Host carries on;
  nothing latches.
- Leftover state never blocks the service or a new stream. The service
  restores it in the background when it can.
- Positions of other displays are not identity. Windows re-lays out the other
  outputs whenever an owned output is recreated, and v2 does not model its
  placement algorithm.

Code: `apps/windows_host/src/display_v2.hpp` (planners, verification, state
codec, `DisplaySessionV2`), `display_v2_rpc.hpp` (private worker/service
messages, configuration switch, v1 migration), `display_v2_tracks.hpp`
(independent outputs), and the glue in `service.hpp`. The tests use an
in-memory Windows/SDK model (`apps/windows_host/tests/display-v2-world.hpp`).

## Who does what

The Session 0 service owns the native provider. It changes the size or count
of the owned outputs on request and publishes the resulting targets. The
interactive worker in the signed-in console session reads and changes the
Windows display configuration (QueryDisplayConfig / SetDisplayConfig), checks
that capture can bind the owned outputs, and asks the service for size changes
over the private worker pipe:

| Message | Purpose |
|---|---|
| `worker.v2.display.size` | one output's size |
| `worker.v2.display.sizes` | both outputs in one SDK reconfiguration |
| `worker.v2.display.count` | owned output count 1 or 2 (display operations only) |
| `worker.v2.start`, `worker.v2.tracks.start` | start a single-output or independent-outputs stream |
| `worker.v2.restore` | the service's background recovery |
| `worker.v2.state` | the state file, for a worker that is not an administrator (below) |

None of these leave the Host; the broker and client wire is described in
[remote-session-protocol.md](remote-session-protocol.md).

Windows changes are made only while the worker runs in the unlocked
interactive console session. In a locked or foreign session v2 changes
nothing, not even the SDK size, and leaves the state file for later.

## Which Windows user

The worker runs as the user signed in at the console: an administrator's
linked full token when the account has one, otherwise that user's own token.
It is never SYSTEM and never another account.

The worker of any signed-in console
user may act (`workerDisplayAuthority`), because nothing it does needs more
(the state file lives in a folder only SYSTEM and Administrators can open, so
the service keeps it for a worker that is not an administrator's):

| Step | Who does it | Needs an administrator? |
|---|---|---|
| Read and change Windows' display configuration (`QueryDisplayConfig`, `SetDisplayConfig`: validate, apply, `SDC_ALLOW_CHANGES`, topology-only; never `SDC_SAVE_TO_DATABASE`) | worker, in its own session | No. The documentation names one access condition, `ERROR_ACCESS_DENIED`: "The caller does not have access to the console session ... does not have access to the current desktop or is running on a remote session" (learn.microsoft.com, `nf-winuser-setdisplayconfig`, read 2026-11-10). No privilege is listed there or for `ChangeDisplaySettingsEx`. |
| Create, remove and size the owned virtual outputs | service (SDK calls to the driver) | The service is SYSTEM; the worker never talks to the driver. |
| Keep the state file | service for a worker that is not an administrator; an administrator's worker opens it itself as before | The folder stays SYSTEM and Administrators only. |
| Read the lease the service publishes, the provider marker | worker | No: `ProgramData\Multitap\Host` is readable by Users. |
| Capture the desktop, inject input | worker | No, for the user's own windows. See the limits. |

What still requires an administrator's worker, and answers
`administrator_required` otherwise: the older transaction model (v1) and the
older display helper (they open administrator-only files and device settings
from the worker), the mode catalog, encoder settings, and
the administrator's record reset in Host settings. The console preparation's
worker also stays an administrator's; other sessions use the console park
([console-display-mode.md](console-display-mode.md)).

### The state file over the pipe

`display_v2_state_rpc.hpp`. Four requests, each `{type: "worker.v2.state",
nonce, op}` plus the fields of its operation, answered with
`worker.v2.state.result {nonce, ok, sha256, state}`:

| `op` | Fields | Accepted when |
|---|---|---|
| `load` | none | always for an admitted worker; returns the state or that none exists |
| `save` | `expected`, `state` | the session is unlocked; `expected` is the SHA-256 of the file as it is now (`null` = no file); `state` parses and re-encodes to the same bytes, is at most 48 KiB, and names the owned outputs the service holds now or the ones in the file |
| `remove` | `expected` | the session is unlocked and `expected` matches the file |
| `discard` | none | the service itself cannot read or parse the file |

A worker is admitted only on the pipe of the one v2 operation the service is
running (a stream it started, the background recovery, an applied count
change), and only while the Windows session the service started that worker in
is the console's active session. A request that is not exactly one of the four
shapes gets no answer and ends that operation, like a malformed size request.

### Threat notes

The worker runs as the signed-in user, so that user can make any request the
worker can (same account, same integrity level). The bar is that a standard
user gains nothing they could not do sitting at the console.

- **No file access.** No path, file name or handle is in any request. The
  service reads and writes one fixed file. `load` never returns the file's
  bytes as they are: only a state that parsed, re-encoded. A file that is not
  a state (or was swapped for something else) is refused and can only be
  discarded, which moves it aside inside the protected folder.
- **What a saved state can do.** It is a Windows display configuration plus
  the owned outputs' ids and sizes. Whoever restores it (this user's worker,
  or later an administrator's) passes it to `SetDisplayConfig` and asks the
  service for the owned sizes it names, which the service validates as for any
  size request. Changing the display layout, resolution or which display is
  on is something a standard user can already do in Windows Settings, for the
  whole machine. The service never treats the content as anything else.
- **Replacing someone else's state.** `save` and `remove` name the exact file
  they replace, so a delayed or repeated request cannot overwrite a newer
  state. A user can still remove or replace the state while their own
  operation runs; what is lost is the pre-stream display layout of the
  machine, not anyone's data, and they could change that layout directly.
- **Another person's stream.** The state carries the label of the stream that
  wrote it. `load` replaces it with the constant `restricted`; the file keeps
  it for administrators' diagnostics.
- **Another session.** A worker of a session that is not the console's active
  one (disconnected by fast user switching, RDP) is refused everything; a
  locked console session can read but not write. Each pipe has one instance,
  first-instance only, opened by the exact process the service created
  (`UserWorker`), so no other process of that user or another user is on it.
- **Flooding.** Requests are bounded to one 64 KiB packet and are served only
  while the service's own operation reads that pipe.
- **Unchanged.** The secure-desktop helper (`secure_desktop_*.hpp`) is not
  touched: an unprivileged worker gets no SYSTEM capture or input beyond what
  the service already gates. No Windows policy or security setting changes.

### Limits for a standard user

- **Elevated windows.** If a standard user starts a program with an
  administrator's credentials (a UAC credential prompt), that window belongs
  to a higher integrity level than the worker. Windows does not deliver the
  worker's input to it, so the input of such a stream goes through the SYSTEM
  injector ([system-input.md](system-input.md)). An administrator's worker has
  the linked full token.
- **A stop while the service is stopping.** An administrator's worker restores
  from a protected receipt even then. Another user's worker cannot; the state
  stays and the next service start's recovery restores it.
- **Other session features.** Shared folders, USB, printing, the clipboard,
  sound, the preview picture and Host settings for a standard user are in
  [standard-user-devices.md](standard-user-devices.md).

## State file

`C:\ProgramData\Multitap\Host\private\display-v2.state`, inside the Host's
protected machine folder (SYSTEM and Administrators only). It is JSON with
schema `multitap-display-v2`, version 1:

- `session`: the operation that wrote it;
- `owned`: the owned outputs at that moment (public id `0`/`1`, adapter LUID,
  target id, size);
- `console`: the identified Proxmox console output, or `null`;
- `paths`, `modes`: the exact active `DISPLAYCONFIG_PATH_INFO` and
  `DISPLAYCONFIG_MODE_INFO` structures, hex encoded.

Writes go to a temporary file and replace the old one atomically. A file that
cannot be parsed, or that does not re-encode to the same bytes, is moved aside
as `display-v2.state.bad` and ignored. Outputs of an older Multitap virtual
display driver that the Host has positively identified as retired are never
saved or restored.

A new stream keeps an older unrestored state as the true original. A state
from an earlier boot is the exception (see below): it is replaced by the
current configuration.

## Console identification

Adapter numbers and display names are not identity. The Proxmox console output
is identified only from a proof the broker supplies (the VM's machine UUID and
its Proxmox display model, currently only standard VGA without a primary
passthrough GPU: `server/src/stream-console-profile.mjs`) matched against the
exact display adapter (`native_stream_console_identity.hpp`). Without a proof, or with an ambiguous
match, no console is identified. The console is switched off for the stream
either way; identification adds two things: Windows re-activating the console
during a change is recognised and undone, and a restore that cannot put the
exact saved configuration back still brings the console up alone, so the VM is
never left without a usable display.

## Start (one output)

1. Refuse an unsupported size before anything changes (the provider's mode
   limits).
2. Save the state file unless an older unrestored one exists.
3. If output `0` is not at the requested size, ask the service to change it.
   The SDK recreates the target; v2 waits up to 5 s for Windows to list it.
4. Apply a configuration that contains only the owned output, at (0,0) and
   therefore primary, at the requested size. Every other display is left out
   of the supplied configuration, which switches it off. The plan is validated
   first, applied with `SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VIRTUAL_MODE_AWARE`,
   and retried once with `SDC_ALLOW_CHANGES`.
5. Settled means: the owned output is active exactly once at the requested size
   at the origin, the identified console is inactive and nothing overlaps the
   owned output. Windows may still apply a layout of its own after a monitor
   arrival, so v2 re-plans from a fresh query up to five times.
6. Capture must then bind exactly that output (bounded 2.5 s). A failed binding
   re-runs the change from fresh state, up to three rounds.

Any failure restores the saved state.

## Resize

The client debounces window changes (650 ms) and sends one at a time. A resize
does not take a new snapshot: SDK size, then the same plan and verification as
a start. A failure restores the saved state and the stream ends
(`stream.resize.failed`). The Windows client then reconnects once at the
current window size.

## Independent outputs (모든 모니터 사용)

A `tracks-v1` stream names exactly the owned outputs; the output count is a
separate earlier step (next section). A group that does not match is refused
with nothing changed.

- When both outputs change size they change in one SDK reconfiguration, so
  Windows reacts to one set of target recreations.
- Output `0` is primary at the origin. Output `1` keeps the client's requested
  offset from it (left, right, above or below); an overlapping request is
  refused.
- Every other display is off, as for one output.
- Verification checks both owned places exactly; then capture must bind one
  source per output (bounded 2.5 s).

A failed group resize restores the saved state and reports
`stream.tracks.resize.failed`. As a last resort the Host can instead put the
stream's previous group back in place and report
`stream.tracks.resize.restored`; the session then continues at the previous
sizes.

## Output count 1 ↔ 2

`host.display.configure` changes how many owned outputs exist, outside a
stream. It is a persistent user choice, not a temporary stream layout:

1. Save the state file unless an older unrestored one exists. A request for the
   current count changes nothing.
2. Ask the service for the new count. Output `1` is added at output `0`'s size
   or removed; output `0` is never touched.
3. Plan: output `0` at the origin, output `1` directly to its right. Every
   display that was active before the change, the console included, is
   supplied again with its previous modes and placed to the right of the owned
   outputs. Displays that were inactive before stay inactive, so a console that
   Windows switched on while a target appeared or disappeared is off again. A
   count change never switches off a display the user had on.
4. Settled means the owned outputs are at their sizes and places, no departed
   owned target is still active, and every other display has its previous
   active state, mode size, refresh rate, rotation and pixel format. Up to
   three re-plans. Then capture must list exactly the owned outputs with
   output `0` primary.
5. On success the snapshot this operation created is deleted: the new layout
   is Windows' current configuration and the baseline of the next stream. On
   failure the previous count is put back first, then the snapshot is
   restored.

The service applies the requested count only while the operation is current
(applied, not cancelled, before its deadline, unlocked console). Putting the
previous count back after a failure is always allowed. The change gets the
operation's remaining time minus 15 s; each private call is bounded to 5 s and
the restore to 10 s, so the whole operation fits in the 30 s result wait.

## Stop, failure and recovery

Restore runs on stream stop, on any failed change, and from the service's
background recovery. With no state file there is nothing to do.

1. Put active owned outputs back to their saved Windows CCD source sizes;
   the SDK's cached target size can differ. For an output that was inactive,
   use its saved SDK size. This also applies to state files written by older
   Hosts. Put both outputs back in one SDK call when
   possible; best effort) and wait up to 3 s for Windows to list them.
2. Apply the saved active paths with their saved modes. Owned outputs are
   mapped by public id to the current SDK targets (target ids change when the
   SDK recreates them) at their saved positions. An owned output that did not
   exist at snapshot time stays active, appended to the right.
3. Verify that every expected target is active, owned outputs have their saved
   sizes and positions (including which owned output was primary), other
   displays have their saved modes, and the console is active if
   it was before. Then delete the state file.
4. If the exact restore fails and the console was active in the saved state,
   activate the identified console on its own (saved modes, then a
   topology-only `SDC_TOPOLOGY_SUPPLIED` request) and delete the state file.
5. Otherwise keep the state file for the background recovery.

A stream's stop counts as clean once its worker has exited. A state file left
by a failed restore belongs to the background recovery and does not hold back
the broker's cleanup or the next stream.

**Background recovery.** While a state file exists and no stream, console
preparation, display operation or other recovery is running, and the console
session is unlocked, the service starts a worker that runs `worker.v2.restore`
once. The signed-in user need not be an administrator, nor the user whose
stream left the state: a stream that ended with its session (sign-out) is
restored at the next sign-in of anyone. Attempts back off from 5 s, doubling to 60 s; unlock and logon reset the
backoff. A stream start that finds a recovery running is refused for that
moment with `host_maintenance`.

**Restarts.** Adapter LUIDs are valid for one boot. Before restoring, v2 checks
that every adapter and console target the state names still exists, that no
saved target is now an owned output, and that no unrecorded display is active.
Anything unresolved is not restored, so a stale state can never turn into a
configuration that switches off the display Windows uses now. A state whose
named adapters are all gone was saved before a Windows restart; it is never
restored, and the next start replaces it. A state where only some adapters are
gone is retried; after six unsuccessful attempts the worker is told to discard
it.

**v1 leftovers.** On its first recovery pass the service moves the files of the
older transaction model (`native-stream-profile.json`,
`native-stream-launch.json`, `native-stream-retirement.json`,
`native-stream-expired-console.json`, `native-stream-viewport-commit.json`,
`native-stream-reclaim.json`) into `private\display-v1-archive\<stamp>\`. If
the v1 profile still had the console switched off, its pre-stream topology
becomes the v2 state, so the ordinary recovery brings the console back.

## Private call robustness

- Session packets that reach the worker while it waits for an SDK reply
  (`stream.lossless`, `ping`, `stream.tracks.ready`, `stream.bitrate`,
  `stream.resize`, `stream.tracks.resize`, `display.scale`) are queued (at most
  256; only the newest `stream.lossless`) and handled after the change.
  Anything else still ends the call.
- A reply that carries another call's nonce (a timed-out earlier call) is
  skipped.
- A refused or lost SDK reply is asked again up to twice; the size change is
  idempotent.

## What v2 never does

It never uses `SDC_SAVE_TO_DATABASE` or the `SDC_TOPOLOGY_EXTEND`, `CLONE`,
`INTERNAL` or `EXTERNAL` shortcuts, changes drivers, restarts Windows or
changes security settings. Capture selects only owned outputs; when none is
ready the stream fails instead of falling back to the console or another
display.

## Diagnostics

v2 reports fixed numbers only, as `hostAction` events in the Host diagnostics
([host-diagnostics.md](host-diagnostics.md)):

| Action | Meaning |
|---|---|
| 210 | stream start (`changed` = console switched off); the stream's preparation record (200) carries stage 100 + v2 stage |
| 202 | resize; stage 100 + v2 stage on failure, 400 = console switched off again, 410/411/412 = queued packets, SDK retries, capture re-plans, 420 = group restored in place |
| 212 | output count change |
| 214 | restore or background recovery (`changed` = console-only fallback; `stale` = saved adapters missing) |
| 216 | v1 files archived |
| 218 | display manager selected (`changed` = v2) |

v2 stages: 1 not eligible, 2 display query, 3 state file, 4 SDK size or count,
5 owned target missing, 6 apply, 7 verify, 8 another display changed, 9
internal. At stage 8 `error` says what differed: 1 count, 2 identity, 3 size,
4 refresh rate, 5 rotation, 6 pixel format.

## Configuration

`displayManager` in the Host's `settings.json`, else in `native-display.json`:
`"v1"` selects the older transaction model, anything else or nothing selects
v2. The value is read once when the service starts.

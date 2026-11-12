# System input and the secure-desktop helper

Two SYSTEM children of the Host service exist so that a stream can be operated
like the physical console. Both are the service's own image, started by the
service in the stream's console session, and both end with the stream.

| | Secure-desktop helper | System injector |
|---|---|---|
| Entry | `MultitapHost.exe --secure-desktop-helper-v1` | `MultitapHost.exe --system-input-v1` |
| Desktop | `winsta0\Winlogon` | `winsta0\Default` |
| Runs | only while a security prompt shows on the secure desktop (UAC with the secure desktop on) | for the whole stream, when the stream needs it (below) |
| Captures | the stream's one output, for the worker to encode | nothing |
| Injects | on the secure desktop only | on the user's desktop only |
| Code | `secure_desktop_*.hpp` | `system_input.hpp`, `system_input_entry.hpp` |

## Why the injector exists

Windows drops input a process injects into a window whose process has a higher
integrity level (UIPI). `SendInput` reports no error when that happens.

* A standard user's stream worker runs at medium integrity. It cannot operate
  any elevated window, and it cannot answer a UAC prompt that Windows shows on
  the user's own desktop (`PromptOnSecureDesktop = 0`: `consent.exe` is then an
  ordinary window at System integrity). Seen on a test VM: the prompt was
  in the stream, the keyboard did nothing, and the pointer was pulled back to one
  place (it stopped at the prompt, and the Host's cursor channel reported the
  stuck position to the client as an application warp).
* An administrator's worker runs with the elevated token (High). It reaches
  elevated windows, but not `consent.exe` on the user's desktop (System is above
  High). This is inferred from the integrity rule, not measured.

## Which stream gets it

Decided once, when the stream's relay starts (`sysinput::systemInputNeed`):

| Worker | UAC policy | Injector |
|---|---|---|
| below High (standard user) | any | yes, all input of the stream |
| High (administrator) | `PromptOnSecureDesktop = 0` and `ConsentPromptBehaviorAdmin != 0` | yes, all input of the stream |
| High (administrator) | secure desktop on (Windows default), or prompts off | no: unchanged, the worker injects |
| any | independent outputs (모든 모니터) | decided by the two rows above, like one stream |

All input of such a stream goes through the injector. Switching per window was
not chosen: a prompt can appear between two events, and a key pressed by one
process would have to be released by another.

The two policy values are only read. No policy or registry value is written.

## The way of one input

```
client -> broker -> service (ServiceBridge::control)
   -> secure-desktop helper, while it streams a prompt on the secure desktop
   -> otherwise the system injector's pipe  -> SendInput on winsta0\Default
      and the worker's pipe, marked "systemInput": true
   -> otherwise the worker (as before)
```

The worker does not inject a marked input. It keeps its bookkeeping for it: the
cursor mark (so the cursor channel does not report the injector's move as an
application warp), the capture wake and the latency sample. The number of hops
from the service to `SendInput` is the same as before (one pipe).

What the injector takes is what the worker takes: keys, absolute moves,
buttons, wheel, and by the session's own grants relative mouse (게임 마우스),
text-input-v1 and touch-input-v1. Ctrl+Alt+Del stays refused by the same code
(`RemoteInput`). The stream's rectangle on the desktop comes from the worker
(`worker.input.area`, sent at the start and after a mode change); positions are
fractions of that rectangle and are refused when it is not inside the physical
desktop (DXGI output rectangles, which are never DPI-virtualized).

## Independent outputs (모든 모니터)

The client names an output and a position on that output's picture. Only the
worker's track plan knows where that output is on the desktop right now, and
whether the client's plan is still the current one. So the worker still places
every input, but it does not inject it:

```
client -> tunnel -> service: keeps the client's event, numbers it
   -> worker (marked, "systemSeq": n): the track plan admits it exactly as before
      (session, topology, the output and its epoch, the client's acknowledgement)
   <- worker.input.track {seq: n, ok, px, py}: ok and, for a positional event,
      the desktop pixel
   -> injector: the CLIENT's event and content with that pixel -> SendInput
```

* The worker supplies a position and nothing else. An `event`, key or button in
  its answer is never used; a position answered for a key is ignored.
* Answers are taken in order. An answer for an input that is not the next one
  waiting drops the ones it skipped; a second answer, or one for nothing that
  waits, injects nothing.
* Nothing is guessed. An input is dropped and counted (246/23) when the plan
  refuses it (an output that is gone or changed, an old topology, a plan the
  client has not acknowledged, a malformed event), when a positional event gets
  no usable pixel, when the worker does not answer within 2 s, or when the pixel
  is not on the physical desktop (the injector's own 246/14).
* There is no stored geometry outside the worker: every input is placed against
  the plan under the plan's lock, and the answer is written under that lock, so a
  mode change and the inputs around it stay in order.
* Independent outputs take keys, positions, buttons, wheel and, by their own
  grant, 게임 마우스 movement. Text and touch were never part of their input and
  stay refused by the plan.
* The injector takes a desktop pixel as a fraction of the physical desktop
  (DXGI). The product binds at most two independent outputs today; the
  placement itself is tested for three.
* One more pipe round trip (service to worker and back) is on the way of such an
  input compared with one stream. It is not measured on a real worker.

## The secure desktop on independent outputs

* **The stream stays** (`track_source.hpp`). While the secure desktop of the
  unlocked session shows, every output codes a still picture on its own
  encoder: the notice, or its helper's frame. When the desktop returns every
  capture is made again (up to 3 s of attempts) and each output goes on with an
  IDR. A lock under the prompt and the 10-minute notice limit end the stream as
  `no_interactive_session`, as for one output.
* **One helper per output** (`secure_desktop_tracks.hpp`). The secure desktop
  is one desktop across the outputs; each output of the stream gets the same
  SYSTEM helper a single stream gets, bound to exactly that output's rectangle.
  Every `worker.secure.*` message of such a stream carries `output` (0 or 1);
  the service keeps one launch state per output and answers with the same
  number. A helper that fails leaves the notice on its own output only.
* **Input.** The worker's track plan places each input exactly as it does for
  the injector (a desktop pixel, nothing else) and the service writes the
  client's own event to a helper: a positional event to the helper whose output
  the pixel is on, which takes a pixel only inside its own output; keys and
  other events without a position to one helper, the same one for as long as
  it shows the prompt. This also holds for a stream without an injector (an
  elevated worker): only while a helper shows the prompt is its input placed
  and written by the service; otherwise the worker injects as before.
* **Not covered.** A drag that starts on one output and ends on the other
  under the prompt (each helper releases only what it pressed). An output
  whose helper does not show the prompt takes no input.

## Boundary

* Input comes only from the service's control path, which carries the
  authenticated client's input of this desktop's own stream. The worker never
  carries input to the injector, and a `systemInput` field a client sends is
  removed by the service.
* Nothing is named. The input pipe has one instance, a DACL that admits SYSTEM
  only, and both ends are opened by the service; the child inherits exactly its
  handles (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`). The injector's token is the
  service's own token with the session set; no other process's token is used.
* The injector is in the service's worker registry (a service stop waits for
  it) and in a kill-on-close job held by the relay. It ends when the relay ends,
  when the worker exits, and by itself when its session is no longer the
  active, unlocked console session signed in as the stream's user (checked at
  most 50 ms apart; the input desktop is checked for every input).
* While the input desktop is not the user's (secure desktop, lock screen) the
  injector injects nothing. On the way back it releases what it held. The
  secure-desktop helper's rules are unchanged.
* It has no capture, no encoder, no socket, and writes no policy value.
* Only counts are recorded, never a key, a text or a position.

### Threat notes

* A process of the user (including a compromised worker) cannot make the
  injector press anything: it has no handle to the pipe, cannot open it by name
  (access denied; one instance), and the worker's messages to the service carry
  no input. It can report a wrong rectangle (`worker.input.area`): the client's
  own positions would then land elsewhere on the desktop. It cannot choose keys
  or clicks.
* The remote user gains what a person at the console has: they can type into
  elevated windows and answer UAC prompts of their own session. That is the
  decision. They gain nothing while the session is locked, signed out or not the
  console session, and nothing in another session.
* The secure desktop is not weakened: with `PromptOnSecureDesktop = 1` prompts
  stay on the secure desktop, where only the secure-desktop helper acts, under
  its own rules.
* On independent outputs a compromised worker can misplace the client's own
  pointer (answer a wrong pixel) or make inputs disappear (refuse them). It
  cannot choose what is pressed and cannot inject when the client sent nothing.
* An administrator's worker with the secure desktop on is unchanged. A window of
  a SYSTEM process on the user's desktop other than `consent.exe` stays out of
  its reach; 246/10 shows that too.

## Records

`hostAction`, values `{action: 200, stage: 246, error, ok: true}`,
`error = kind | value << 8`. All are observations.

| kind | written by | value |
|---|---|---|
| 1 | worker, when a secure-desktop helper ends | inputs the helper received (written even when 0) |
| 2 | 〃 | inputs Windows accepted |
| 3 | 〃 | dropped by the allowlist |
| 4 | 〃 | not accepted (malformed, outside the output, or 5) |
| 5 | 〃 | `SendInput` failures |
| 6 | 〃 | last `SendInput` error (Win32 code) |
| 7 | service, when a secure-desktop helper ends | inputs written to the helper |
| 8 | 〃 | inputs kept for the worker while a helper was held but not showing |
| 9 | 〃 | failed writes to the helper |
| 10 | worker without an injector | the window in front is above the worker: worker level \| window level << 4 (2 medium, 3 high, 4 system, 15 not readable). At most every 10 s |
| 11 | service, when the injector ends | inputs the injector received (written even when 0) |
| 12 | 〃 | inputs Windows accepted |
| 13 | 〃 | dropped (not an input, or not granted to the session) |
| 14 | 〃 | not accepted (malformed, outside the output, no rectangle yet, or 15) |
| 15 | 〃 | `SendInput` failures |
| 16 | 〃 | last `SendInput` error |
| 17 | 〃 | not injected because the input desktop was not the user's |
| 18 | service, at the stream's start | the injector was started: worker level \| reason << 4 (1 worker not elevated, 2 elevated worker with prompts on the user's desktop) |
| 19 | 〃 | the injector could not be started (same value); the worker injects as before |
| 20 | service, when the injector ends | inputs written to the injector (written even when 0) |
| 21 | 〃 | failed writes |
| 22 | 〃 | inputs given to the worker because no injector was alive |
| 23 | 〃 | independent outputs: inputs not injected because the worker did not place them (refused, no position, no answer) |
| 24 | 〃 | independent outputs: inputs the service asked the worker to place |
| 25 | 〃 | independent outputs under a secure desktop: placed inputs written to a secure-desktop helper. For a stream without an injector 23, 24 and 25 are written on their own |

The secure-desktop records stay `stage: 244` (1 notice without a helper,
2 desktop returned, 3 ended locked, 4 ended at the limit, 5 helper streamed the
prompt, 6 launch failed, 7 helper lost, 8 no frame in 2 s). A stream of
independent outputs writes 5 to 8 once per output (two helpers), and 2, 3 and 4
once.

## Not verified

Touch injection from SYSTEM, the administrator case with prompts on the user's
desktop, and the cost of the session observations on a real VM. On independent
outputs under a secure desktop: two helpers at once, their duplications of two
outputs, and input to them. The tests use
fakes for Windows and the real pipe and reader for the transport.

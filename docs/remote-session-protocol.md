# Remote session protocol

The wire contract between the Multitap client, the broker/relay and the Host
for streaming sessions (Multitap Streaming): Host enrollment and its control
channel, connecting, the session sockets, control JSON, binary media and the
optional features the peers negotiate. The component overview is in
[ARCHITECTURE.md](../ARCHITECTURE.md), the engine in
[multitap-streaming.md](multitap-streaming.md). The broker side is `server/src/relay.mjs`
and the modules it imports, the Host side `apps/windows_host/src/`, the client
side `apps/windows_client/lib/data/` and `apps/windows_client/windows/runner/`.

## Conventions

- Every session authenticates through the broker on its configured public HTTPS
  origin. Clients authenticate with `Authorization: Bearer <session token>`,
  Hosts with `Authorization: Host <host token>`. No credential is ever put in a
  URL, and the user never supplies a Host destination address or port. Negotiated
  UDP peer candidates are scoped to that authorized session.
- Messages are validated field by field. Unknown or missing fields, values out
  of range, or a message of a feature the session did not negotiate end the
  session (from the client: `invalid_input`) or drop the Host channel
  (`invalid_control`, `invalid_media`, ...). The relay rebuilds what it forwards.
- Optional features share one negotiation pattern:
  1. The Host advertises an exact marker in its capabilities (for example
     `cursorProtocol:'cursor-v1'`). The broker passes exactly that value to
     clients in the desktop list; anything else is omitted and never
     invalidates the other capabilities.
  2. A client sends the matching connect field only when its inventory shows
     the capability (a broker that does not know a field refuses the request).
     A malformed value is 400 with the feature's own code.
  3. The broker grants the feature (the marker in the connect response and in
     `session.start`) only when the Host advertised it and, where a permission
     applies, the assignment allows it. Not granting is not a failure unless
     stated otherwise: the session simply runs without the feature.
  4. A client rejects a grant it did not request, or with another value.

  In the compatibility tables "released" means a peer that does not know the
  feature.
- Most input and cursor features apply to ordinary single-output streams.
  Independent outputs (`tracks-v1`, 모든 모니터 사용) get a feature only
  through its own tracks marker, as noted per feature.

## Host enrollment and control channel

- Host settings ask to enrol: `POST /api/v1/hosts/requests`
  `{requestKey, hostname, machineUuid, version}` -> 201
  `{requestId, status:'pending', expiresAt, retryAfterSeconds:3}`. A Host whose
  settings window shows the verification code sends
  `X-Multitap-Host-Request-Code: v1`. The request key stays protected on the
  machine; the broker stores only its hash.
- The Host service polls `POST /api/v1/hosts/requests/status`
  `{requestId, requestKey}` and may withdraw with `.../cancel`. A request
  expires after 10 minutes. A pending request never receives a credential.
- The administrator sees each pending request with a verification code
  derived from the request key (the same code Host settings show) and the
  Proxmox VM whose SMBIOS UUID matches the claimed `machineUuid`. Approval
  reads that UUID from Proxmox again; several live requests for one machine
  must be resolved explicitly before one can be approved.
- After approval the status answer carries `credentials` (including `hostId`,
  `hostToken` and `desktopId`). The Host keeps the token in protected machine
  storage; the broker stores its hash.
- The older single-use code API (`POST /api/v1/admin/desktops/:id/enrollment`,
  then `POST /api/v1/hosts/enroll` `{code, hostname, machineUuid, version}`)
  remains for Hosts that predate the request flow.
- The Host service keeps one outbound WSS `/api/v1/hosts/channel` with its
  Host token and `X-Multitap-Host-Protocol: capabilities-v1` (one channel per VM;
  a second is 409). It sends `host.capabilities` (acknowledged with
  `host.capabilities.ack`), heartbeats and operation results; single-output
  session control (`session.start`, `session.stop`, `input.reset`, input)
  reaches it on this channel. Independent outputs and the redirection channels
  use their own per-session sockets under `/api/v1/hosts/sessions/:id/`.

### Host capabilities

`host.capabilities` reports readiness and features. Required booleans:
`service`, `interactiveSession`, `streaming` and `virtualDisplayReady`.
Optional: `monitors`
(1-16 `{id, width, height, primary, modes?}`, ids `'0'`-`'15'`),
`dynamicResolution`, `viewportProvisioning`, `resizeProtocol`,
`virtualMonitorControl`, `clipboard`, `clipboardSync`, a fixed
`viewportFailureReason`, and the feature markers in the
[capability index](#capability-index).

A running service is not streaming readiness: the worker in the user's
session must find and validate the owned virtual outputs before `streaming` is
true. Without an interactive user, an owned output or while in maintenance
the Host reports `streaming:false` and connects are refused with 409
`stream_unavailable`.

## Connecting

`POST /api/v1/desktops/:id/connect` with the Bearer token. An empty body is a
default stream; otherwise:

```json
{"mode":"stream","options":{"maxWidth":1920,"maxHeight":1080,"fps":60,"bitrate":12000000,"audio":true,"monitor":"0"}}
```

plus the connect fields of the features the client wants.

- `mode` is `stream` (the default), the only connection mode. Another value is
  refused with 400 `invalid_connection_mode` (`rdp`: 409 `rdp_not_supported`).
- `options` (the values above are the defaults): `maxWidth`/`maxHeight` even,
  128-3840 by 128-2160 (larger with [stream sizes](#stream-sizes)); `fps` 30 or
  60; `bitrate` 4-20 Mbps, or up to 100 Mbps and `bitrateMode:'auto'` with a
  [`bitrate-adaptive-v1`](#adaptive-bitrate) Host; `audio`; `monitor` an id the
  Host lists (`'0'`-`'15'`) or `all`, a composite of the owned outputs.
  Anything else is 400 `invalid_stream_options`; a monitor the Host does not
  list is 409 `monitor_unavailable`.
- The broker checks the account, the assignment, the Host's channel (409
  `host_not_registered`), its readiness (409 `stream_unavailable` with a
  reason), pending display, console or cleanup work on the VM (409, for example
  `session_cleanup_pending`) and that nobody else uses the VM: one controller
  per VM, otherwise 409 `desktop_busy` with
  `details:{reason:'controller_in_use', takeover}`.
  `POST /api/v1/desktops/:id/takeover` lets an account the VM is assigned to
  end the other session.
- The response is `{sessionId, webSocketUrl, mode, transport, options,
  clipboardPolicy, sessionCancellationProtocol:'session-cancel-v1', ...}` plus
  the granted markers and channel URLs (`clipboardWebSocketUrl`,
  `printWebSocketUrl`, `driveWebSocketUrl`, `usbWebSocketUrl`). `transport` is
  `media-v1`, `media-epoch-v1` (resizable, see
  [display size](#display-size-and-resize)) or `media-tracks-v1`
  ([independent outputs](#independent-outputs-tracks-v1)).
- The client opens `webSocketUrl` (`wss://<origin>/api/v1/sessions/<id>`,
  Bearer) within 30 seconds; one socket per session. A session the client no
  longer wants is released with `DELETE /api/v1/sessions/:id` ->
  `{sessionId, released:true}` (404 `session_not_found` when it is already
  gone).
- When the client attaches, the broker sends the Host `session.start`.
  During the session the relay re-checks account, assignment and permissions
  every 250 ms and on every assignment change. Any end sends the Host
  `input.reset` and `session.stop {sessionId, reason}` and the client
  `{type:'session.ended', reason, detail?}`; a revoked grant ends as
  `authorization_revoked`.

Media uses WSS, the broker's [UDP relay](#udp-media-udp-media-v1), or an
authenticated direct UDP path when `udp-direct-v1` is negotiated. LAN
reachability is never required. Failed direct discovery retains the relay.

## Control JSON

Broker -> Host when the client attaches (ordinary stream):

```json
{"type":"session.start","sessionId":"<uuid>","mode":"stream","width":1920,"height":1080,"fps":60,"bitrate":12000000,"audio":true,"monitor":"0","clipboardPolicy":{"textToRemote":false,"textToLocal":false,"filesToRemote":false,"filesToLocal":false}}
```

plus the session's grants (feature markers, channel URLs such as `clipboardUrl`,
`printUrl`, `driveUrl` and `usbUrl`, and `resizeProtocol:'epoch-v1', epoch:0`
for a resizable stream). The Host captures only Multitap-owned virtual
outputs, never the Proxmox console or another product's display; `width` and
`height` are encoding ceilings, never an upscale. It answers:

```json
{"type":"stream.info","codec":"h264","width":1920,"height":1080,"sourceWidth":1920,"sourceHeight":1080,"fpsTarget":60,"audio":{"sampleRate":48000,"channels":2,"format":"s16le","available":true}}
```

Source and encoded geometry are separate. Input coordinates are normalised to
the selected source rectangle, including negative origins and the `all`
composite. A Host that cannot stream sends
`stream.unavailable {sessionId, reason}` (`no_interactive_session`,
`virtual_display_required`, `worker_failed`, `capture_failed`,
`encoder_failed`, `host_maintenance`) and keeps its channel.

Host -> client once per second:
`{"type":"stream.stats","capturedFps":0,"encodedFps":0,"bitrateKbps":0}`, with
measured values only; features below add their own members.

Client -> Host (the relay adds `sessionId`):

```json
{"type":"input","event":"key","vk":91,"scan":0,"down":true,"extended":true}
{"type":"input","event":"move","x":0.5,"y":0.5}
{"type":"input","event":"button","button":"left","down":true,"x":0.5,"y":0.5}
{"type":"input","event":"wheel","delta":120,"x":0.5,"y":0.5}
{"type":"input.reset"}
{"type":"ping","id":"nonce"}
{"type":"session.stop"}
```

`vk` 1-255, `scan` 0-65535, `button` `left`, `right`, `middle`, `x1` or `x2`,
`delta` at most ±12000, coordinates 0..1. Client messages are text only, at
most 8 KiB each and 2000 messages (32 MiB) per second. A `ping` makes the full
trip to the Host and back as `{"type":"pong","id":...}`; the client shows it as
round-trip time, not as one-way video latency. Input that arrives while the
output is being prepared or resized is dropped, not queued.

While the remote view has focus, the Windows client captures ordinary Windows
shortcuts, including the Windows key and Alt+Tab, and gives them back on
focus loss with `input.reset`, so the Host releases every pressed key and
button. Ctrl+Alt+Shift+F12 releases the local capture; Ctrl+Alt+Del stays with
the local PC. The Host's worker injects with the signed-in user's token (for
an administrator, the linked elevated token), so elevated windows such as Task
Manager accept input; the secure desktop (UAC prompt, Ctrl+Alt+Del screen) is
never a target of ordinary injected input.

## Binary media

A 12-byte little-endian header followed by the payload:

| Bytes | Meaning |
|---|---|
| 0 | kind: 1 = H.264 Annex B video, 2 = audio, 3 = cursor record ([cursor-v1](#cursor-separation-cursor-v1)) |
| 1 | flags: bit 0 = key frame (video); zero otherwise |
| 2-3 | `media-epoch-v1`: uint16 epoch; `media-v1`: zero |
| 4-11 | uint64 presentation time in microseconds since the capture session began |

- Video carries SPS/PPS before the first IDR of every stream. The Host
  encodes with x264 on the CPU, with NVENC or AMF when the VM has a supported
  GPU, and with the Microsoft software encoder as fallback; clients decode with
  Media Foundation (Windows) or MediaCodec (Android). Re-encoding a cached
  frame is not a new desktop update; fresh captures, encoding and decoding are
  measured separately.
- Audio is WASAPI loopback at 48 kHz stereo: s16le PCM in whole 4-byte frames,
  or, with `audio-opus-v1`, one 10 ms Opus packet per message preceded by a
  little-endian u16 sequence number. `session.start` then carries
  `audioCodec:'opus'` and `stream.info` states the format actually sent.
  Independent outputs get Opus only with `audio-opus-tracks-v1`. The Host
  captures the default render endpoint (console role); after any capture
  error or a change of that default it opens the capture again with a backoff
  for the life of the stream, so the sound pauses instead of ending. A packet's
  PTS is its first sample on the stream clock, taken from the capture's own
  position, so consecutive packets are exactly their length apart; a silence
  or a reopened capture starts the stamps over (a gap in PTS, no sequence gap).
- A message is at most 4 MiB; each relay socket queue is bounded at 8 MiB.
  When a client's backlog passes half of that, a Host that advertises
  `keyFrameProtocol:'keyframe-v1'` receives `{type:'stream.keyframe', sessionId}`;
  the client's video stops until that IDR and audio is dropped while the
  backlog stays above the bound. Without keyframe-v1 a full queue disconnects
  the client (`client_backpressure`). Nothing is queued without bound and no
  frame rate or latency figure is invented.

## Capability index

Markers that appear in the Host capabilities, the desktop list, the connect
request and the grant. Sections below give the details.

| Marker | Host capability | Connect field | What it adds |
|---|---|---|---|
| `epoch-v1` | `resizeProtocol`, `dynamicResolution`, `viewportProvisioning` | `resizeProtocol` | the owned output follows the client's viewport ([display size](#display-size-and-resize)) |
| `epoch-v1` (initial) | `initialViewportProtocol` | `initialViewportProtocol`, `initialViewport` | the output is prepared at the window's size before the first frame |
| `separate-v1` | `viewportEncoderLimitsProtocol` | same | encoding ceilings independent of the source size |
| `stream-size-v1`, `stream-size-v2` | `streamSizeProtocol`, `streamSizeProtocols` | `streamSizeProtocol`, `decoderLimits` | source-size pictures within the decoder's limits ([stream sizes](#stream-sizes)) |
| `tracks-v1` | `videoTrackProtocol`, `trackViewportProvisioning` | `mediaProtocol`, `tracks` | one video track per owned output ([independent outputs](#independent-outputs-tracks-v1)) |
| `console-profile-v1`, `group-v1` | `streamProfileProtocol`, `streamProfileViewportProtocol`, `streamProfileTracksProtocol` | `streamProfileTracksProtocol` (tracks only) | every display other than the owned outputs (the Proxmox console, a vGPU head, a physical monitor) is switched off for the stream and restored afterwards ([display-manager-v2.md](display-manager-v2.md)). The broker adds the private `consoleProfile` proof only when Proxmox shows standard VGA for the registered VM; any other display, or a Proxmox read that fails or takes over 10 s, starts the same stream without it (and without the `group-v1` marker), recorded as `stream_console_unverified`. A VM that Proxmox reports with another SMBIOS UUID ends with `host_machine_mismatch`, a changed registration with `desktop_changed` |
| `bitrate-adaptive-v1` | `bitrateProtocol`, `maxBitrate` | `options.bitrateMode`, `options.bitrate` | [adaptive bitrate](#adaptive-bitrate) |
| `audio-opus-v1`, `audio-opus-tracks-v1` | `audioCodecProtocol`, `audioCodecTracksProtocol` | `audioCodecProtocol` | Opus audio ([binary media](#binary-media)) |
| `udp-media-v1` | `udpMediaProtocol` | `udpMediaProtocol` | media over the broker's UDP relay ([UDP media](#udp-media-udp-media-v1)) |
| `udp-direct-v1` | `udpDirectHostProtocol` (broker shows `udpDirectProtocol`) | `udpDirectProtocol` | authenticated direct UDP, retaining UDP relay and WSS fallback |
| `keyframe-v1` | `keyFrameProtocol` | - | `stream.keyframe` asks the Host for an IDR |
| `session-resume-v1`, `host-resume-v1`, `tracks-resume-v1` | `keyFrameProtocol`, `hostResumeProtocol`, `trackResumeProtocol` | `sessionResumeProtocol` | a short transport loss does not end the session ([resume](#resume)) |
| `stop-v1` | `sessionStopProtocol` | - | a confirmed Host stop before the VM takes the next session ([ending a session](#ending-a-session-stop-v1)) |
| `stream-observability-v1` | `observabilityProtocol` | `observabilityProtocol` | passive queue and timing snapshots ([stream-observability.md](stream-observability.md)) |
| `input-latency-v1`, `frame-timing-v1` | `inputLatencyProtocol` | `inputLatencyProtocol` | [input latency measurement](#input-latency-measurement-input-latency-v1) |
| `cursor-v1`, `cursor-tracks-v1`, `cursor-echo-v1`, `cursor-compose-v1` | `cursorProtocol`, `cursorTracksProtocol`, `cursorEchoProtocol`, `cursorComposeProtocol` | the same names | [cursor](#cursor-separation-cursor-v1) outside the video |
| `relative-mouse-v1`, `relative-mouse-tracks-v1` | `relativeMouseProtocol`, `relativeMouseTracksProtocol` | `relativeMouseProtocol` | [relative mouse](#relative-mouse-relative-mouse-v1) for 마우스 잠금 |
| `text-input-v1` | `textInputProtocol` | `textInputProtocol` | [committed Unicode text](#text-input-text-input-v1) |
| `touch-input-v1` | `touchInputProtocol` | `touchInputProtocol` | [Windows touch contacts](#touch-input-touch-input-v1) |
| `caret-v1` | `caretProtocol` | `caretProtocol` | [text caret position](#caret-follow-caret-v1) |
| `display-scale-v1`, `display-scale-outputs-v1` | `displayScaleProtocol`, `displayScaleOutputsProtocol` | `displayScaleProtocol`, `displayScale`, `displayScaleOutputsProtocol` | [Windows display scale](#display-scale-display-scale-v1) of the owned output |
| `tiles-v1` | in band | `lt1-` ping nonces | [lossless tiles](#lossless-tiles-tiles-v1) |
| `clipboard-v1`, `clipboard-sync-v1` | `clipboard`, `clipboardSync` | `clipboardProtocol` | [clipboard](#clipboard) channel and automatic sharing |
| `print-v1` | `printProtocol` (or `printUnavailableReason`) | `printProtocol` | [printing](#printing-print-v1) |
| `drive-v1` | `driveProtocol`, `driveFs` (or `driveUnavailableReason`) | `driveProtocol`, `driveFs` | [shared PC folders](#drives-and-usb-devices) |
| `usb-v1`, `usb-tracks-v1` | `usbProtocol` | `usbProtocol` | [USB devices](#drives-and-usb-devices) |

## Display size and resize

### Resizable streams (epoch-v1)

A Host that advertises `dynamicResolution:true`, `resizeProtocol:'epoch-v1'`
and monitor modes can be asked for `resizeProtocol:'epoch-v1'`. The response
uses `transport:'media-epoch-v1'`: header bytes 2-3 carry the epoch, starting
at 0. With `viewportProvisioning:true` (which also needs owned outputs and an
interactive session) the owned output is set to any even size from 320x180 to
3840x2160 (more with stream sizes); without it only an advertised mode is
accepted. The client debounces window, fullscreen and DPI changes (650 ms) and
keeps only its latest wish, with one change in flight.

1. Client: `{type:'stream.resize', requestId, width, height, fps, monitor}`
   for its current single monitor (`all` is refused).
2. The broker allows two requests per second and refuses a second change
   while a provisioning change is in flight. It assigns a `resizeId` and a new
   epoch (1-65535), sends the Host `input.reset` and `stream.resize`, and tells
   the client `stream.resizing` with `timeoutMs` (90 s when provisioning, 15 s
   otherwise). Input is blocked; heartbeat and stop stay available; media of
   the old epoch is dropped.
3. The Host applies the real display mode, restarts capture and encoder and
   answers with a fresh `stream.info` that echoes the session, request and
   resize IDs, the epoch, the FPS and the exact source and encoded geometry. A
   scaled picture of another source cannot satisfy a resize.
4. The broker forwards new media only from the new epoch's key frame; the
   client restarts its decoder at the new size and keeps mapping input against
   the real source.

A rejection before dispatch is `stream.resize.failed` with `unchanged:true`
(the old decoder continues). A failure after dispatch reports
`unchanged:false` and ends the session; the Host's display transaction
restores the saved configuration or reports that recovery is needed. A
superseded request cannot complete its successor; revocation, timeout and
disconnect end a pending change.

### Initial viewport (epoch-v1)

`initialViewportProtocol:'epoch-v1'` with
`initialViewport:{monitor:'0', width, height, fps}` asks the Host to prepare
the output at the window's size before the encoder starts. It needs
`resizeProtocol:'epoch-v1'`, `options.monitor:'0'` and the same `fps`, and is
not combined with tracks (otherwise 400 `invalid_initial_viewport`). The Host
must advertise `initialViewportProtocol:'epoch-v1'` with viewport
provisioning, virtual monitor control and the single owned output `'0'`
(otherwise 409 `initial_viewport_unavailable`). The response and
`session.start` carry the prepared `initialViewport` (with its own request and
resize IDs and epoch 1); the first `stream.info` must echo them. Input waits
for that picture.

### Encoder ceilings (separate-v1)

`viewportEncoderLimitsProtocol:'separate-v1'` is additive to the initial
viewport (400 `invalid_viewport_encoder_limits` without one; 409
`viewport_encoder_limits_unavailable` when the Host does not advertise it).
The Host advertises it only with a single owned output `'0'`; it is not used
with tracks-v1. Resize `width`/`height` then describe the actual Windows
source, while the original `options.maxWidth`/`maxHeight` stay encoding
ceilings. The correlated `stream.info` must show the requested source and FPS
and an even encoded size within the ceilings; the Host checks the same limits
itself. A quality change reconnects and keeps automatic viewport sizing.

### Stream sizes

`stream-size-v1` is optional for single-output viewport streams (an initial
viewport with separate-v1). It removes the fixed encode budget (4,194,304
pixels at 60 fps) and the 2160-row limit: the picture is the source's own size
and is reduced only for what cannot keep up.

**Negotiation.** The Host advertises `streamSizeProtocol:'stream-size-v1'`
together with separate-v1. A client sends `streamSizeProtocol` and
`decoderLimits:{maxWidth, maxHeight, maxPixels, maxPixelRate}` (exactly these
integers: sides 320/180 to 3840, frame 57,600 to 8,294,400 pixels, rate 0 or
1,728,000 to 497,664,000 pixels/s) only with an initial viewport,
separate-v1 and no tracks; anything else is 400 `invalid_stream_size`. Such a
request may use a portrait initial viewport (up to 2160x3840: even sides,
longer side at most 3840, at most 8,294,400 pixels) and a box of two per-side
ceilings up to 3840x3840. The broker grants it (connect response
`streamSizeProtocol`; `session.start` adds the marker and `decoderLimits`) only
for a Host that advertises it; not granting is not a failure, but a request
whose viewport or box needs the grant is 409 `stream_size_unavailable`.

| Client | Host | Broker | Picture |
|---|---|---|---|
| released | any | any | separate-v1 budget, at most 3840x2160 |
| stream-size-v1 | released | any | the same (never granted; portrait refused with 409) |
| stream-size-v1 | stream-size-v1 | released | the same (capability stripped, never asked) |
| stream-size-v1 | stream-size-v1 | stream-size-v1 | source size within the limits below; portrait up to 2160x3840 |

**stream-size-v2.** A Host that also advertises
`streamSizeProtocols:['stream-size-v1','stream-size-v2']` accepts
`streamSizeProtocol:'stream-size-v2'`. Sizes are then bounded only by the
declared decoder limits (sides of at least 16, frames of at least 256 pixels)
and the wire integers, owned outputs by at least 320x180, and the grant also
applies to independent outputs (tracks-v1). A v2 request that cannot be
granted is always 409 `stream_size_unavailable`. `stream.resize` and
`stream.tracks.resize` may carry new `decoderLimits`, and the Host may end the
stream with the additional reasons `frame_layout_unsupported`,
`frame_memory_unavailable`, `frame_memory_insufficient`,
`encoder_format_unsupported` and `unsupported_display_mode`.

**Decoder limits.** The Windows client decodes with the Microsoft H.264 MFT
(no D3D11 hardware decoder). It declares the largest sizes its decoder
interface takes with a pixel rate of 0 (not declared) and checks each planned
stream size against the decoder before asking for it; its throughput is
measured live.
Android declares what `MediaCodecInfo.VideoCapabilities` of the AVC decoder it
creates reports. An engine that cannot say declares the safe default
`{3840,2160,8294400,251658240}`.

**Host.** The picture is the largest scale of the source (aspect kept, even
sides, whole 16x16 macroblocks) within the box, the declared sides, pixels per
frame and pixels per second at the session's fps, and an optional
`settings.json` `videoPixelBudget` administrator cap (unset: no cap). A decoder
without a declared rate starts at the safe budget and goes above it only while
the client's decode reports show headroom. The Host then steps the scale down
on measured overload: two consecutive ~1 s windows in which its capture thread
was at least 90 % busy, missed 92 % of the frame rate and spent at least 90 %
of a frame interval per encoded frame, or in which the client reported a
decoder busy share of at least 900 per mille or a backlog of at least four
frame intervals (100 ms minimum). A step targets 75 % of a frame interval
(conversion and encoding scale with the picture's pixels, capture does not) or
750 per mille busy, between 10 % and 50 % per step, never below a quarter of
the source and at most every 3 s. It steps back up after two calm windows, 2 s
after any change and 10 s after the last step down, as far as the measured
projection allows (70 % / 700 per mille). Each change restarts capture and
encoder in place: a new `stream.info` of the same epoch (same `sessionId`,
`epoch`, source and fps; new `width`/`height`), then an IDR. What a session
learned carries over its resize restarts. `stream.info` and `stream.stats` of
a granted session add `encodeScale` (picture width per source width, per
mille) and `encodeLimit` (`none`, `decoder_size`, `decoder_rate`,
`decoder_load`, `host_cpu`, `admin_cap`).

**Wire.** Client -> relay -> Host
`{"type":"stream.decoder","busy":N,"backlogMs":M}` once per statistics tick
(exactly these fields; `busy` -1..1000 per mille, -1 not measured;
`backlogMs` 0..60000; the relay adds `sessionId`, forwards at most one per
400 ms and ends an ungranted or malformed one with `invalid_input`). The relay
checks every granted picture against the declared limits (initial and resize
metadata fail the change; a mid-epoch picture larger than its source, the box
or the limits drops the Host) and accepts portrait resizes only in a granted
session. The client restarts only its decoder at the new size (input capture
is kept) and drops video until the IDR.

**Owned outputs.** The native display provider (libvirtualdisplay) creates
temporary displays of the requested size and makes it the preferred target
mode with identity rotation, so a portrait size is a native mode, not a
rotated landscape one. The Host's validators share one rule
(`owned_output_size.hpp`) and the EDID physical size follows the orientation.
Lossless tiles accept portrait frames under the same rule (at most 2040
tiles).

### Virtual output count

`POST /api/v1/desktops/:id/display` with `{count:1|2}` starts one bounded
operation (202); the user polls
`GET /api/v1/desktops/:id/display/operations/:operationId` with the same
account. A concurrent controller, power action or display operation prevents
it. The Host receives `host.display.configure {operationId, nonce, count}`,
changes only its owned outputs and answers `host.display.result` with a fresh
topology. A replaced Host, a revoked assignment, a wrong nonce or a timeout
cannot complete it. This changes the active outputs; choosing `monitor:'all'`
only selects what is viewed.

## Independent outputs (tracks-v1)

모든 모니터 사용 with two local monitors streams each owned output as its own
video track to its own local monitor, instead of one wide composite
(`native/windows_video/track_media.hpp`, `server/src/track-media.mjs`).

**Negotiation.** The connect body asks with `mediaProtocol:'tracks-v1'` and
`tracks:[{id, width, height, x, y}]` plus the stream `options`; it cannot be
combined with `resizeProtocol` or an initial viewport. Track sizes are even,
320-3840 by 180-2160, within `maxWidth`/`maxHeight`, at most 8,294,400 pixels
together (stream-size-v2 lifts these bounds), placed without overlap within
±32,768. Two tracks need `options.monitor:'all'`; a single track names its
actual id. Malformed requests are 400 `invalid_media_protocol` /
`invalid_track_request`. The Host must advertise fresh
`videoTrackProtocol:'tracks-v1'`, `trackViewportProvisioning:true`, readiness
and every requested monitor (otherwise 409 `track_streaming_unavailable`). A
Host with the console profile additionally needs
`streamProfileTracksProtocol:'group-v1'`, which the client then requests. The
response uses `transport:'media-tracks-v1'` and echoes the tracks. The
tracks keep a fixed total rate (`bitrateMode:'auto'` is 400
`invalid_stream_options`); input latency, text, touch, caret and UDP media are
not granted, and cursor, display scale, relative mouse, Opus audio and resume
need their tracks markers.

**Session.** One assigned user session authorises every track. `session.start`
carries `mediaProtocol`, topology epoch 1, each track with epoch 1 and
`tunnelUrl` (`/api/v1/hosts/sessions/:id/tracks`). The Host service opens that
WSS with its Host token before it starts the interactive worker; the socket
must match the current registered Host, client and assignment. A fault on it
ends the media session, never the Host's control channel. Old sockets and
session IDs cannot reach a successor whose epochs restart at 1.

**TVT1 messages.** A 28-byte little-endian header, never confused with the
12-byte media header:

| Bytes | Meaning |
|---|---|
| 0-3 | ASCII `TVT1` |
| 4 | kind: 1 = H.264 Annex B video, 2 = shared audio, 3 = cursor record (cursor-tracks-v1) |
| 5 | video: bit 0 key frame; other bits and audio zero |
| 6 | video connector id 0/1; audio and cursor 255 |
| 7 | zero |
| 8-11 | nonzero uint32 topology epoch |
| 12-15 | nonzero uint32 video track epoch; audio and cursor zero |
| 16-23 | presentation microseconds, 0 to 2^53 - 1 |
| 24-27 | payload length, exactly the remaining bytes |

A message is at most 4 MiB. PCM payloads are whole 4-byte frames (Opus
payloads with audio-opus-tracks-v1: sequence and packet, any length).

**Metadata.** The Host sends `stream.tracks.info` with `sessionId`, `codec`
`h264`, `topologyEpoch`, the exact `fpsTarget`, the total `bitrateTarget`,
`audio` and `tracks:[{id, epoch, width, height, sourceWidth, sourceHeight,
left, top, primary, bitrateTarget}]`, after a resize also `requestId` and
`resizeId`. Each source size must equal its requested viewport (scaling a
different source is not an applied resize), the actual positions keep the
requested relative placement through one common translation, track bitrates
add up to the total within the budget, and at most one owned output is
primary (a preserved console output may be the Windows primary). New topology
metadata resets every decoder and key-frame state; identical metadata is
idempotent.

**Media and readiness.** Each video track starts with a key frame carrying
SPS, PPS and IDR NAL units; per-track and audio PTS increase; old topology or
track epochs are dropped, future ones are rejected. The relay unlocks input
and audio only after every configured track's key frame and the client's
`stream.tracks.ready {topologyEpoch, tracks:[{id, epoch}]}`, which the client
sends only after its decoders produced real frames for every output.
Preparation may take 90 s and client readiness 15 s; after that each video
output must deliver a new packet within 10 s (audio or the other output cannot
hide a stalled one). Pointer input carries `topologyEpoch`, `track` and the
track `epoch` with normalised x/y; keys carry `topologyEpoch`. The Host's
track socket is bounded at 1000 messages and 32 MiB per second and 8 KiB per
text message.

**Resize.** The client sends `stream.tracks.resize {requestId,
topologyEpoch, tracks:[{id, width, height, x, y}], fps}` with the current
topology epoch, at most one per 500 ms. The broker rejects stale requests
before dispatch (`stream.tracks.resize.failed`, `unchanged:true`), resets
input, announces `stream.tracks.resizing` with a new `resizeId` and topology
epoch, and advances the epoch of each track whose size or FPS changes. The
Host stops the source, applies all owned modes in one Windows transaction,
verifies the result and answers with correlated `stream.tracks.info` and fresh
key frames on every track. A failure after dispatch
(`stream.tracks.resize.failed`) ends the session with `unchanged:false`; in a
`group-v1` session the Host may instead report `stream.tracks.resize.restored`, after
which the previous sizes come back under the new topology epoch with fresh
metadata and key frames. Track statistics arrive as
`stream.tracks.stats {topologyEpoch, measuredMs, tracks:[{id, epoch,
capturedFps, encodedFps, bitrateKbps}]}`.

## Adaptive bitrate

A Host advertises `{bitrateProtocol:'bitrate-adaptive-v1', maxBitrate}`
(see [adaptive-bitrate.md](adaptive-bitrate.md)). Only for such a Host does
the broker accept `options.bitrateMode:'auto'|'fixed'` and a fixed `bitrate`
up to `maxBitrate` (otherwise: 400 `invalid_stream_options` above 20 Mbps, 409
`stream_bitrate_unavailable` for auto). In auto mode `options.bitrate` is the
ceiling, the response adds `bitrateProtocol`, and `session.start` carries
`bitrate` = min(20 Mbps, ceiling), `bitrateMode:'auto'` and `bitrateMax`.
That session's client sends
`{type:'stream.feedback', epoch, frames:[[pts, arrivalUs], ...], bytes,
intervalUs}` (at most 64 frames) every 100 ms; the relay forwards it with
`sessionId` and its own unsent bytes toward the client (`relayQueuedBytes`).
The Host's controller passes the new rate to its worker over the private pipe,
never during a resize. `stream.stats` then adds `bitrateTarget`,
`bitrateMode`, `bitrateMax`, `networkDelayMs` and `deliveredKbps`.
Independent outputs keep a fixed rate.

## UDP media (udp-media-v1)

Only when the broker runs its UDP relay (`MULTITAP_UDP_PORT`), for a
single-output session whose Host advertises `udpMediaProtocol:'udp-media-v1'`
and whose client asked for it. Each leg (client and Host) receives over its
own channel `{type:'udp.grant', protocol, relays, id, ticket, expiresAt,
keys?}`; the ticket is that leg's secret and proves its bind at the UDP relay.
The legs exchange X25519 public keys with `{type:'udp.key', protocol,
publicKey}`, which the broker passes on unread, and only for desktops enabled
by `MULTITAP_UDP_MEDIA` (`on`, or a list of desktop IDs). Media datagrams are
then sealed with AES-256-GCM under the agreed keys. The broker relays the
keys, so this protects each session but is not end-to-end against the
broker. Framing, fragmentation, NACK and key-frame recovery are in
`native/windows_video/udp_media.hpp` (keys in `udp_media_crypto.hpp`). Anything
uncertain keeps or puts the session back on WSS; UDP is never a reason to
refuse or end a session.

### Direct UDP (udp-direct-v1)

This is a bounded IPv4 hole-punching extension to the existing encrypted UDP
socket, not a general ICE/WebRTC implementation. It requires the existing UDP
relay grant and successful bind. It applies to ordinary single-output,
automatic-bitrate streams on Windows and Android; independent-output tracks
and fixed-bitrate streams retain their existing WSS path. Older peers never
receive the direct marker. Clipboard, files and device redirection stay on
their existing authenticated broker channels.

- A Host advertises `udpDirectHostProtocol:'udp-direct-v1'`. A supporting broker
  translates it to the visible `udpDirectProtocol` capability; old brokers that
  merely forward unknown Host fields cannot trigger a new connect field. The
  client requests it only after seeing the broker's marker. With encrypted UDP the grant
  gains `directProtocol:'udp-direct-v1'` and optional `stun:[{host,port}]`.
- The broker sends `{type:'udp.peer',protocol:'udp-direct-v1',sessionId,
  candidates:[{address,port}]}` containing only the opposite leg of this live
  session. It combines the bound UDP source, one RFC1918 candidate from
  `udp.local`, and one public `udp.local` with `candidateType:'srflx'`.
  Replaced leg IDs and retired sessions cannot offer candidates.
- Public discovery uses RFC 8489 Binding on the same socket. The default is
  `stun.cloudflare.com:3478`; `MULTITAP_STUN_SERVERS=off` disables it, or supply
  at most two `host:port` entries. STUN sees the socket's public address and a
  random transaction, without login tokens, keys or media. An exact source,
  transaction and bounded response parser are required. STUN is only a hint.
- Both peers send 53-byte proofs: `TVD`, version byte 1, kind 1 (ping) or 2
  (pong), 16 random nonce bytes, then HMAC-SHA256 of the preceding 21 bytes.
  Each direction's proof key is HMAC-SHA256(media direction key,
  `multitap-udp-direct-v1`). Only a fresh reply to that source's outstanding
  challenge selects it; an incoming authenticated ping alone does not.
  There are at most five peer candidates, 250 ms probes, 1.5 s proof freshness
  and 32 ping replies per second. Relay proofs use the existing DATA wrapper.
  Freshness without a wire change: an address learned from a
  ping takes at most two of the five slots, leaves after 3 s without a
  fresh reply to our challenge (or 10 s of silence after one), and a broker
  candidate displaces it; a ping whose nonce was already seen from another
  address (64 recent nonces, 10 s) is a replay and is neither answered nor
  learned. No peer is probed at 0.0.0.0/8, multicast, 240/4 or broadcast,
  nor at loopback unless the relay itself is loopback.
- A fresh direct path is selected if its RTT is no more than 2 ms above a
  fresh relay proof. Media keeps its existing AES-GCM, replay protection,
  fragmentation, repair and IDR recovery. Expired direct proof immediately
  selects the relay on the next send. If proofs work but media stalls with a
  healthy broker/relay, the Host backs off direct for 30 s and requests an IDR
  through UDP relay before the existing WSS fallback. Broker feedback older
  than 2 s disables direct media/input; losing the control channel pauses
  fast input. Existing session/permission revocation remains authoritative.
- Basic keys, absolute/negotiated relative mouse and input reset can travel
  directly. Their encrypted plaintext is byte `0x30`, sequence (BE uint32),
  epoch (BE uint16), UTF-8 JSON length (BE uint16), then at most 1,158 JSON
  bytes. The same event always has a reliable WSS copy:
  `{type:'input.direct',protocol:'udp-direct-v1',sequence,epoch,input}`.
  The broker validates the original input and adds the exact session ID for
  the Host. A bounded Host dispatcher deduplicates both copies and never
  skips a sequence using UDP alone. WSS can advance past denied/lost input.
  Text/touch and larger messages stay on WSS. WSS-only `input.fence` barriers
  preserve ordering around mode/resize controls; focus reset, stop and worker
  retirement discard old queued input. Resize also holds direct input until
  the next actual media epoch. No new input or redirection permission is added.
- `udpDirectPath` reports the last decrypted UDP source (1 relay, 2 direct),
  gated by the actual active video path in the UI. `udpDirectRttMs` and
  `udpRelayRttMs` are proof RTTs. The existing application RTT is still WSS
  RTT. Early real worker timing reports may wait at the broker for their WSS
  sample registration (64 reports, 2 s); no synthetic injection is reported.

The broker is trusted for session authorization and public-key exchange; this
does not provide end-to-end authentication against a malicious broker. No
UPnP, port forwarding, router setting or firewall/security-policy change is
made on the endpoints. Symmetric NAT and restricted networks may use relay.

## Resume

A short loss of the client's or the Host's transport does not end a granted
stream that already shows pictures (`server/src/session-resume.mjs`).

- `session-resume-v1` (client <-> broker): the desktop list shows
  `sessionResumeProtocol` for a Host with keyframe-v1; a client sends it back
  in the connect body and the grant adds `resumeGraceMs` (15 s). Each attach
  gives the client `{type:'session.resume.token', token, graceMs}`. A socket
  that ends without a close frame (or by a transport error, the heartbeat or
  the send bound) leaves the session waiting; the client returns by opening
  the same session URL with its Bearer token and `X-Multitap-Resume: <token>`
  and receives `{type:'session.resumed', token, graceMs}`. Tokens are single
  use, bound to the account, the desktop and the session; only their hash is
  kept. A close frame is a deliberate end.
- `host-resume-v1`: a Host that keeps its worker through a lost channel
  advertises `hostResumeProtocol`; its first message on the new channel is
  `{type:'host.session.resume', sessions:[id]}`, answered with
  `host.session.resumed` or `session.stop`. The grace is 30 s; meanwhile the
  client hears `session.resuming` and then `session.resumed`.
- `tracks-resume-v1`: for independent outputs, only from a Host that
  advertises `trackResumeProtocol`. Its track socket accepts
  `{type:'stream.keyframe', sessionId}` (an IDR on every output), and a lost
  track socket is dialled again.
- Any resume restarts the picture at an IDR; the display is not prepared
  again. Resizes in flight and streams without a first picture still end.
- `channel-resume-v1` (`server/src/channel-resume.mjs`): the side channels
  (clipboard, print-v1, drive-v1, usb-v1) of a resumed stream pair again in
  the same session. A Host that redials advertises
  `hostChannelResumeProtocol`; a client that redials sends
  `X-Multitap-Channel-Resume: channel-resume-v1` on its session socket. Where
  both did and the session resumes, the client reads `channelResumeProtocol`
  in `session.resume.token` / `session.resumed` and the Host in
  `session.start`. Then: while the client or the Host is away the pairs end
  with `<channel>_stream_away` and a dial is answered 503 (not 401); the Host
  hears `{type:'session.channels', sessionId, state:'away'|'back'}` when the
  client leaves and returns (a returning Host before its
  `host.session.resumed`); a pair that ended for a transport reason
  (`_transport_closed`, `_pair_timeout`, `_stream_away`, `_timeout`) may be
  dialled again and passes every check of a first pair (the same session and
  tokens, the stored permissions of that moment); any other end, or a
  permission taken away while the channel had no pair, is final for the
  session; a new dial of a role the broker still holds replaces that pair; new
  pairs are bounded per channel and session (6 per minute). Nothing of an
  ended pair is kept by the broker or the Host: the client offers its folders
  and USB devices again on the new pair. Both peers try three times (after 1,
  2 and 4 s), spend nothing while the stream is away and get one fresh set
  when it continues. Every other mix of peers keeps the earlier behaviour: an
  ended print, drive or USB pair stays ended for the session.

## Ending a session (stop-v1)

With `sessionStopProtocol:'stop-v1'` the broker keeps a stop record for every
ended session the Host had started: `session.stop` carries a
`stopRequestId`, and the VM accepts no new session (409
`session_cleanup_pending`) until the Host reports
`host.session.stopped {sessionId, sessionStopProtocol, stopRequestId,
workerQuiescent, profileRequired, profileRestored}` and a fresh capability
update follows. A closed socket or a timeout is never taken as a clean stop.

## Relative mouse (relative-mouse-v1)

For 마우스 잠금 (called "game mouse" in code and diagnostics): while the client
holds the mouse it sends movement instead of positions, so the VM's own
pointer speed and acceleration apply.

- Host capability and connect field `relativeMouseProtocol:'relative-mouse-v1'`;
  a malformed value is 400 `invalid_relative_mouse_protocol`.
- Granted for a single output only when the Host has one monitor (with two,
  movement could leave the streamed output). Independent outputs get it only
  when every monitor is a streamed track, the Host advertises
  `relativeMouseTracksProtocol:'relative-mouse-tracks-v1'` and cursor-compose-v1
  is granted.
- Messages (exact fields; `sample` allowed as on `move`):
  `{type:'input', event:'moveRelative', dx, dy}` with integers of at most
  16,384 in magnitude, not both zero; `button` and `wheel` without `x`/`y`
  act at the current remote cursor. In a tracks session they carry
  `topologyEpoch` and never `sample`. Sessions without the grant end on them
  (`invalid_input`).
- The Windows client can instead lend a virtual USB HID mouse through the USB
  channel when that permission is granted
  ([usb-redirection.md](usb-redirection.md)).

## Text input (text-input-v1)

For single-output streams. A client whose keyboard commits composed text (the
Android app: Korean IMEs, other scripts, emoji) sends that text instead of
emulating key positions. Key emulation types Korean only while the remote IME
is already in Hangul mode and cannot type emoji; Unicode text arrives exactly
as committed whatever the remote layout or IME mode.

**Negotiation.** Host capability and connect field
`textInputProtocol:'text-input-v1'` (stream, no tracks; malformed: 400
`invalid_text_input_protocol`). The Windows client also requests it for
ordinary streams so a phone or another remote client can type through a
Windows client into a different VM. Physical keys remain key events. Windows
native Unicode capture stays disabled for independent tracks and sign-in
streams; existing Android phase-bound text behavior is unchanged.

| Client | Host | Broker | Text |
|---|---|---|---|
| Windows, ordinary stream | text-input-v1 | text-input-v1 | keys plus negotiated committed Unicode |
| Android with text-input-v1 | released | any | keys (US layout, Hangul as 2-set keys) |
| Android with text-input-v1 | text-input-v1 | released | keys (capability stripped) |
| Android with text-input-v1 | text-input-v1 | text-input-v1 | committed text as Unicode |

**Wire.** `{"type":"input","event":"text","text":"안녕"}`, exactly these three
fields: 1-256 UTF-16 code units of well-formed text without control
characters (C0, DEL, C1). The relay ends the session with `invalid_input` for
text in an ungranted session or any malformed message, drops it like other
injection while the output is changing, and never adds a latency `sample`.
Enter, Tab, Backspace, Delete and arrows remain `key` events; the client
splits longer commits without splitting a surrogate pair.

**Host.** Only a `session.start` carrying the marker accepts text. The worker
validates the whole message first (nothing of a refused message is injected),
then injects one `KEYEVENTF_UNICODE` key down and up per UTF-16 unit, `wVk` 0.
Unicode key events hold nothing, so `input.reset` and release-on-stop are
unchanged.

**Composition (Android).** Only committed text is sent: what the phone
keyboard is still composing stays local (shown above the key row) until the
keyboard commits it, or until a click, a key-row key or closing the keyboard
sends it first. Deleting committed text is Backspace keys.

## Touch input (touch-input-v1)

For single-output streams (one monitor or the `all` composite). A client with
a touch screen (the Android app's direct touch mode) sends the contacts of
each touch frame instead of emulating a mouse; the Host injects them as
Windows touch input, so applications get real pans, pinch-zoom,
press-and-hold and touch-aware behaviour, like RDP multitouch. Mouse and key
input stay available.

**Negotiation.** Host capability and connect field
`touchInputProtocol:'touch-input-v1'` (stream, no tracks; malformed: 400
`invalid_touch_input_protocol`).

| Client | Host | Broker | Touch |
|---|---|---|---|
| released / Windows | any | any | mouse emulation or none (never requested) |
| Android with touch-input-v1 | released | any | mouse emulation (not granted) |
| Android with touch-input-v1 | touch-input-v1 | released | mouse emulation (capability stripped) |
| Android with touch-input-v1 | touch-input-v1 | touch-input-v1 | Windows touch contacts |

**Wire.** Exactly three fields:

```json
{"type":"input","event":"touch","contacts":[
  {"id":3,"phase":"down","x":0.25,"y":0.5,"pressure":0.6,"w":0.01,"h":0.012},
  {"id":4,"phase":"move","x":0.75,"y":0.5}]}
```

`contacts` holds 1-10 contacts with distinct `id`s. Each has exactly `id`
(integer 0..2^31-1, the client's pointer id), `phase` (`down`, `move`, `up`,
`cancel`), `x`/`y` (0..1 over the streamed source rectangle, the space of mouse
input), optionally `pressure` (0..1) and optionally the contact size `w`/`h`
(both or neither, fractions of the source width/height, > 0 and <= 1). The
relay ends the session with `invalid_input` for touch in an ungranted session
or any malformed frame, never adds a latency `sample`, drops frames like other
injection while the output changes, and counts them in the input rate limit.
A frame lists the contacts that changed (`down` once when a finger lands,
`move` while it moves or to say it is still there, `up` when it lifts,
`cancel` when the gesture is abandoned); unlisted contacts stay where they
were. Nothing comes back from the Host.

**Host.** Only a `session.start` carrying the marker accepts touch. The
worker validates the whole frame first, maps each position onto the session's
capture rectangle (with the `all` composite, onto the output it names) and
injects with `InitializeTouchInjection(10, TOUCH_FEEDBACK_DEFAULT)` /
`InjectTouchInput`. That call takes ordinary screen coordinates and documents
its frame, flag, cancel and timing rules, while a
`CreateSyntheticPointerDevice` device positions relative to the virtual
screen's top left, which differs whenever an owned output sits left of or
above the primary. Each injection lists every held contact: new ones
`DOWN|INRANGE|INCONTACT`, held ones `UPDATE|INRANGE|INCONTACT`, ending ones
`UP` (cancel: `UP|CANCELED` at the previous position); an `up` at a new
position is preceded by an UPDATE there. A mixed frame is injected as updates,
then ends, then new contacts. Contacts map to pointer ids 0-9; a `down` of a
held id or an eleventh contact refuses the frame; `move`/`up`/`cancel` of a
contact the worker no longer holds are skipped. Held contacts are refreshed
every 50 ms without client frames (Windows cancels a press-and-hold that is
not refreshed) and cancelled after 30 s without any frame. `ERROR_NOT_READY`
is retried; any other failure cancels every contact, as do `input.reset`,
session stop, disconnect, capture failure and worker stop. Elevated windows
accept touch like the mouse; the secure desktop is never targeted.

**Client API** (`lib/data/touch_input.dart`, `RemoteSession`). An Android
client constructs `HttpBrokerApi(requestTouchInput: true)`.
`RemoteSession.touchInput` is true when granted;
`RemoteSession.sendTouch(List<RemoteTouchContact>)` sends one frame in order
with other input and returns false (nothing sent) when not granted, invalid or
when input is not accepted now. Diagnostics rows may carry `touchInput`,
`touchFrames` and `touchContacts`; never positions.

## Caret follow (caret-v1)

A client with a soft keyboard (the Android app) keeps the text field being
typed in above its keyboard by asking the Host where the focused remote
application's text caret is. Numbers only: no text, window title, class or
process name is read or sent.

- Host capability and connect field `caretProtocol:'caret-v1'` (stream, no
  tracks; malformed: 400 `invalid_caret_protocol`). Without the grant the
  client follows taps instead.
- Client -> Host while the keyboard is open: `{"type":"caret.watch","on":true}`,
  and `on:false` when it closes (exactly these fields; ungranted: the session
  ends with `invalid_input`).
- Host -> client while watched:
  `{"type":"caret","sessionId":S,"has":true,"x","y","w","h","sw","sh"}` with
  integers 0..65535 in pixels of the streamed output (origin top-left, `sw`/`sh`
  the output size, at least 2), or `{"type":"caret","sessionId":S,"has":false}`
  when the focus has no caret or it lies outside the output. Sent on change
  only, about 15 per second at most. A malformed or ungranted report drops the
  Host; a report of another session is ignored.
- Host sources (user-session worker, out-of-context WinEvent hooks):
  `EVENT_OBJECT_LOCATIONCHANGE` with `OBJID_CARET` read through
  `IAccessible::accLocation`, `EVENT_OBJECT_FOCUS` and
  `EVENT_SYSTEM_FOREGROUND` (which reset to "no caret" until a new one shows),
  and the foreground thread's `GUITHREADINFO.rcCaret` as fallback and for a
  250 ms poll. Applications that publish neither report no caret.
- The client keeps the caret in a comfort band (56 dp above the keyboard and
  key row, 24 dp at the sides) and moves the picture only when it leaves it. A
  move by hand suspends following until the caret jumps out of view or the
  picture is tapped.

## Display scale (display-scale-v1)

Sets the Windows display scale (per-monitor DPI) of the owned output being
streamed, like Microsoft Remote Desktop's desktop scale factor. The Windows
client offers it as a scale choice in its session toolbar; the Android app
uses it to stream the phone's own pixels with readable text instead of
upscaling fewer remote pixels. Windows scale steps are 100, 125, 150, 175,
200, 225, 250, 300, 350, 400, 450 and 500 %.

**Negotiation.** Host capability `displayScaleProtocol:'display-scale-v1'`.
A client sends `displayScaleProtocol` for a stream, optionally with
`displayScale:<step>` as the initial scale; without `displayScale` it
negotiates runtime control and the current Windows scale stays. A malformed
value is 400 `invalid_display_scale`. The grant (`displayScaleProtocol`, and
`displayScale` when asked) appears in the response and `session.start`.
Independent outputs need display-scale-outputs-v1 (below).

| Client | Host | Broker | Result |
|---|---|---|---|
| released | any | any | no scale control |
| display-scale-v1 | released | any | no scale control; Android uses a whole-number fraction of the screen's pixels, drawn without smoothing |
| display-scale-v1 | display-scale-v1 | released | the same (capability stripped) |
| display-scale-v1 | display-scale-v1 | display-scale-v1 | the requested Windows scale on the owned output, clamped by Windows |

**Wire.** Client -> relay -> Host `{"type":"display.scale","scale":200}`
(exactly these fields). Host -> relay -> client
`{"type":"display.scale.applied","sessionId":...,"protocol":"display-scale-v1",
"ok":true,"scale":175,"maximum":175,"code":0}`: `scale` is the step Windows now
uses (0 unknown), `maximum` the largest Windows allows for the output's
current size, `code` 0 applied, 1 output not found, 2 unreadable, 3 refused by
Windows, 4 no recovery record, 5 unsupported output (composite capture). The
relay ends the session (`invalid_input`) for a request in an ungranted session
or a malformed one, and drops a Host whose result is malformed or arrives in
its current session without the grant.

**Host.** Only the single owned output the session's capture is bound to
changes; never the QEMU console, other products' displays or, without
display-scale-outputs-v1, independent outputs. The worker reads Windows'
relative steps for that source (DisplayConfigGetDeviceInfo, DPI-scale type
-3), clamps the request to Windows' maximum, writes the previous step to a
per-user recovery record (`%LOCALAPPDATA%\Multitap\Host\display-scale.state`)
before the first change (no record, no change) and applies it
(DisplayConfigSetDeviceInfo, type -4). The scale is applied again after every
resize restart; requests that arrive during a resize are applied once it has
finished. When the session ends the recorded step is put back; a record left
by a crash is restored by the next stream on that monitor. No registry,
policy or security setting is written directly, and a failure never affects
the stream. These DisplayConfig types are what Windows Settings uses; they are
not a documented public API.

**Independent outputs (display-scale-outputs-v1).** The Host advertises
`displayScaleOutputsProtocol:'display-scale-outputs-v1'` beside
display-scale-v1. A tracks client sends it together with
`displayScaleProtocol` (and optionally `displayScale`); the marker without
both or without tracks is 400 `invalid_display_scale`. The same
`display.scale` request then applies to every owned output: each is clamped by
Windows on its own, recorded in the same per-user record (keyed by monitor),
applied again after every track restart and put back at the end. The result
arrives on the track socket as `display.scale.applied` whose summary fields
(`ok` only when every output is, the first failing output's `code`, output
`'0'`'s `scale`, the largest `maximum`) are followed by
`outputs:[{"id":"0","ok":true,"scale":175,"maximum":175,"code":0},{"id":"1",...}]`
(exactly these fields, ids `'0'`/`'1'` once each). The Windows client shows one
value only when every monitor really uses it, otherwise each monitor's value,
and asks for its chosen scale again when it reconnects.

**Android.** With the grant the app streams the screen's own pixels (a
portrait screen beyond 2160 rows needs stream sizes, otherwise an exact half)
and asks for a scale (화면 배율: 자동 from the display's measured density, or a
fixed step). Without it, it uses a whole-number fraction of the screen's
pixels. Changing the scale with the grant sends only `display.scale`, no
resize.

## Cursor separation (cursor-v1)

For single-output streams. Without it the Host composes the cursor into the
video, so every pointer move changes the picture and the encoder codes up to
60 frames/s for the cursor alone. With it the Host leaves the cursor out of
the video and sends its shape, visibility and (rarely) position as small
binary records; the client shows the remote shape as its real local Windows
cursor, so pointer movement is local and produces no video frames.

**Negotiation.** Host capability and connect field `cursorProtocol:'cursor-v1'`
(stream; malformed: 400 `invalid_cursor_protocol`). Granted for a single
output whose Host advertises it; independent outputs need cursor-tracks-v1
(below). Without the grant the session keeps the composed cursor.

| Client | Host | Broker | Cursor |
|---|---|---|---|
| released | any | any | composed in the video |
| cursor-v1 | released | any | composed (never requested) |
| cursor-v1 | cursor-v1 | released | composed (capability stripped) |
| cursor-v1 | cursor-v1 | cursor-v1 | separated, local cursor |
| cursor-v1 | cursor-v1 | cursor-v1, tracks-v1 without cursor-tracks-v1 | composed (not granted) |

**Wire.** Host -> relay -> client binary messages with the 12-byte media
header: byte 0 = **3**, byte 1 = 0, bytes 2-3 = the media epoch exactly as
video, bytes 4-11 = the capture tick's PTS. The payload is exactly one
little-endian record:

| Shape record | Bytes |
|---|---|
| 0 | `1` |
| 1 | format: `1` colour = 32-bpp BGRA as the Windows cursor stores it (alpha when present) plus a 1-bpp AND mask; `2` monochrome = 1-bpp AND mask rows followed by 1-bpp XOR mask rows |
| 2-3 | zero |
| 4-7 | shape id, 1..2147483647, strictly increasing within one session (across epochs) |
| 8-9, 10-11 | width, height: 1..256 source pixels |
| 12-13, 14-15 | hotspot x < width, hotspot y < height |
| 16.. | mask: rows of `((width+31)/32)*4` bytes, top-down, `height` rows (colour) or `2*height` rows (AND then XOR); bits past `width` zero |
| .. | colour only: `width*height*4` BGRA bytes, top-down |

The payload length is exactly `16 + stride*rows (+ width*height*4)`.

| State record | Bytes |
|---|---|
| 0 | `2` |
| 1 | flags: bit 0 visible, bit 1 position present; other bits zero |
| 2-3 | zero |
| 4-7 | shape id: visible -> nonzero and <= the last shape id of this session; hidden -> 0 |
| 8-11, 12-15 | int32 x, y: with bit 1 the remote hotspot in source pixels (0 <= x < sourceWidth, 0 <= y < sourceHeight); otherwise 0 |

A state is exactly 16 bytes. A hidden state may carry a position (an
application that hides the cursor and re-centres it). The native Windows
formats are kept on purpose: the client recreates the identical cursor with
`CreateIconIndirect`, so alpha, AND-mask transparency and true XOR inversion
(I-beam, inverted cursors) are drawn by Windows against the remote picture.

**Host.** In a granted session no capture path composes the cursor, so
cursor-only movement leaves the frame, `FrameEncodePolicy` and lossless tiles
unchanged. The capture tick reads `GetCursorInfo` once; a changed handle (and
once per second the same handle, for an in-place scheme change) is read with
`GetIconInfo`/`GetDIBits` in the native format, hashed and cached (at most 32
live shapes, LRU). Animated cursors send their current frame. The shape has
the size Windows shows on that output, which is not always the size of the
cursor's own bitmap: Windows scales a cursor for the monitor it is on, by the
monitor's DPI over the DPI the cursor was made for. The system cursors are
sent at the nominal cursor size of the output's scale (32 pixels at 100 %, 48
at 150 %, 64 at 200 %; `CopyImage` makes that size as Windows does), a cursor
of a window Windows scales itself (DPI unaware, or system DPI aware at another
DPI) by the same ratio, and a per-monitor DPI aware program's own cursor as it
is. A shape pixel therefore stays one source pixel for every client. Visible means
`CURSOR_SHOWING`, not `CURSOR_SUPPRESSED`, a non-null handle and a hotspot
inside the captured rectangle. A cursor that cannot be represented (no bitmap,
larger than 256 pixels, unreadable) is composed into the video as before and
reported hidden, so there are never zero or two cursors. Nothing is sent
before the first key frame of a capture epoch; a shape always precedes a state
that names it; a new epoch re-sends the current shape with a new id. A
position is sent only when the remote position differs by more than 2 pixels
from where the last injected pointer event put it (application warp,
ClipCursor, console input), has been stable for two capture ticks, no input
was injected in the last 50 ms and at least 100 ms passed since the previous
position. Bounds: at most 10 shape records/s, states coalesced at least 25 ms
apart, at most 50 cursor messages/s. `stream.stats` adds
`cursor:{protocol:'cursor-v1', shapes, states, warps, composed}`.

**Relay.** Kind 3 is accepted only from the Host of a granted, ready session:
a malformed record, a non-increasing shape id, a visible state naming an id
above the last shape, or a position outside the last `stream.info` source size
drops the Host channel as `invalid_media`; more than 100 cursor messages or
4 MiB of cursor payload per second drop it as `host_rate_limit`. Well-formed
records of an ungranted session (a retired worker's packets in transit), of a
stale epoch, during a resize or before the epoch's first video key frame are
discarded silently. Cursor records are never counted as video.

**Client.** `RemoteSession` accepts kind 3 only for a granted single-output
connection (otherwise `invalid_stream_message`), with the epoch rules of media,
and hands each record to the native runner (method `cursor`); after layout
changes it reports the picture geometry (`cursorView`). The native presenter
validates every record again, caches up to 64 shapes and scales a shape by the
picture's physical width per source width. It shows the remote shape only
over the picture while the remote view owns input and the remote cursor is
visible; everywhere else (toolbar, letterbox, other windows, after focus loss)
the local cursor applies unchanged, and a hidden remote cursor or a shape the
client no longer has keeps the cursor hidden over the picture. A position
record moves the local pointer only while presenting, with the pointer inside
the picture and no local mouse movement in the last 50 ms.

**Interaction with input-latency-v1.** A pointer move no longer changes a
frame, so an injected move becomes the frame-tag candidate only while a
button is held (a drag). Buttons, wheel and key presses are tagged as usual.

### Independent outputs (cursor-tracks-v1)

Without it a tracks session composes the cursor into the video of the output
it is over, so the cursor moves only when that output sends a frame. With it
the cursor is separated as above. The Host advertises
`cursorTracksProtocol:'cursor-tracks-v1'` beside cursor-v1; a client sends both
markers only with tracks-v1 (the tracks marker without cursor-v1 or without
tracks is 400 `invalid_cursor_protocol`).

The worker then composes the cursor into no output (an unrepresentable cursor:
into the output it is over, reported hidden). Visible means the hotspot is
inside any owned output. Records are the cursor-v1 shape and state records,
never with a position, in a TVT1 envelope of their own: kind 3, flags 0,
connector 255, the current topology epoch, track epoch 0, the tick PTS and the
payload length. Nothing is sent before every output's first key frame of a
topology; a new topology re-sends the current shape under a new id. The relay
applies the cursor-v1 rules on the track socket: a malformed record or
envelope, a position, a non-increasing shape id or a state naming an unsent id
ends the session (`invalid_track_media`); more than 100 messages or 4 MiB per
second end it as `tracks_host_rate_limit`. The Windows client shows each
output 1:1 on its own monitor, so the shape keeps its pixel size and hotspot on
both.

| Client | Host | Broker | Two monitors |
|---|---|---|---|
| released | any | any | composed in the video |
| cursor-tracks-v1 | released | any | composed (capability absent) |
| cursor-tracks-v1 | cursor-tracks-v1 | released | composed (capability stripped) |
| cursor-tracks-v1 | cursor-tracks-v1 | cursor-tracks-v1 | separated, local cursor on both monitors |

### Cursor echo (cursor-echo-v1)

For 마우스 잠금 on a single output. The client moves the VM's cursor by
relative input (a virtual USB mouse or `moveRelative`), so the VM's own
pointer acceleration decides where it goes, and the warps above (at rest, at
most 10/s) cannot keep the local pointer on it while it moves. With this grant
the Host reports the VM's real position as it changes and the client holds its
pointer there.

**Negotiation.** The Host advertises `cursorEchoProtocol:'cursor-echo-v1'`
beside cursor-v1. A client sends the connect field only together with
`cursorProtocol` (the Windows client also with `relativeMouseProtocol`);
otherwise 400 `invalid_cursor_protocol`. Granted only with cursor-v1, without
tracks.

**Request.** Client -> broker `{"type":"cursor.echo","on":true|false}`,
exactly these fields; forwarded as `{type:'cursor.echo', sessionId, on}` only
in a granted session (otherwise the session ends with `invalid_input`). The
service keeps only the latest request for its worker.

**Host.** While asked, the worker reads `GetCursorInfo` every 2 ms on its own
high-resolution timer thread and sends each new position at once as an
ordinary state record (the last visibility and shape id, bit 1 set). Never two
within 1.5 ms (at most 667/s), not counted in the 50/s budget of the other
records. The capture thread and the poller hand records on under one lock, so
a state never precedes its shape. It stops on `on:false`, at the session's end
and with the capture; a resize restart of the same session keeps it.
`stream.stats` adds `echoes` to `cursor`.

**Relay.** A granted session may carry 1,500 cursor messages per second; all
other cursor-v1 rules apply.

**Client.** A hand-over in a granted session pins the pointer on one pixel
(`ClipCursor` 1x1) and moves it to each reported position mapped into the
picture; Raw Input keeps reporting this PC's motion. Every release sends
`on:false` and restores the normal clip.

### Cursor in the video (cursor-compose-v1)

For 마우스 잠금. The echo's pointer arrives one round trip after the move, the
picture later as video, so the two can be out of step. With this grant the
VM's cursor is drawn into the video, and cursor and picture arrive in the same
frame (the approach Moonlight-style game streaming takes). A client granted
both uses this one; the echo remains the fallback.

**Negotiation.** As cursor-echo-v1: the Host advertises
`cursorComposeProtocol:'cursor-compose-v1'`; the client sends it only with
`cursorProtocol` (the Windows client also with `relativeMouseProtocol`).
Granted only with cursor-v1; for independent outputs only with
cursor-tracks-v1 and a Host that advertises `relative-mouse-tracks-v1` (the
output the cursor is over composes it, and the request travels on the track
socket).

**Request.** `{"type":"cursor.compose","on":true|false}`, exactly these
fields; forwarded as `{type:'cursor.compose', sessionId, on}` only in a granted
session.

**Host.** While asked, every capture tick draws the cursor into the video as
Windows shows it, and the cursor channel reports it hidden (as for an
unrepresentable shape); resting positions still leave as hidden states with a
position. When it ends, the real state (a new shape first) follows at the next
tick. It ends with `on:false`, the session and the worker; a resize restart of
the same session keeps it. No new record and no other bound.

**Client.** A hand-over parks the pointer on one pixel (Raw Input keeps
reporting), shows no cursor of its own over the picture and sends `on:true`;
every release sends `on:false`, moves the pointer to the VM's last resting
position and presents the separated cursor again.

## Input latency measurement (input-latency-v1)

For single-output streams. It measures (1) client input -> Host injection and
(2) client input -> the first changed video frame after that injection,
decoded and handed to the texture. No key code, coordinate or other input
content appears in any report or log; only sequence numbers, durations and a
video PTS.

**Negotiation.** Host capability and connect field
`inputLatencyProtocol:'input-latency-v1'` (malformed: 400
`invalid_input_latency_protocol`); granted only without tracks. Not granting
means the session runs unmeasured.

**Client -> Host.** A sampled `input` gets one extra field `sample` (integer
1..2147483647, strictly increasing). The client samples at most one discrete
input (button, key press, wheel) per 100 ms and one move per 250 ms, with at
most 16 unacknowledged. It never samples a key release or a press of
Shift/Ctrl/Alt/Windows (they show nothing by themselves) and always samples
the release of a sampled button press (a click's result follows its release).
The message is otherwise unchanged. The relay accepts `sample` only on `input`
of a granted session, requires it to increase, and above 20 samples/s
forwards the input without its `sample`.

**Host.** Session0 stamps a private `latencyReceivedUs` (QueryPerformanceCounter
microseconds, comparable between the service and its worker) on the copy it
forwards. The worker injects the input unchanged and reports back; Session0
turns that, immediately before its WebSocket write, into
`{type:'input.ack', sessionId, sample, ok, injectUs, holdUs}`: `injectUs` =
injection call returned - Session0 receipt, `holdUs` = this write - Session0
receipt. Private messages never leave the Host.

**Frame tag.** A successful injection becomes the candidate for the next
capture that begins after it; a later injection replaces an older candidate.
Only inputs that can change the picture by themselves are candidates: key
presses other than Shift/Ctrl/Alt/Windows, button presses and releases, wheel
steps, and moves when the cursor is in the video or a button is held. The
first frame whose content actually changed (`FrameEncodePolicy`
`contentChanged`) is assigned; immediately before that frame's video packet
the worker sends `{type:'input.frame', sessionId, epoch, sample, pts}`. A
successful unsampled button change or committed text drops a pending candidate
that has not reached its frame (the next change may be that input's).

**No visible change.** An input that changes nothing (a key that does nothing,
a wheel step at the scroll limit, a click on the empty desktop) would otherwise
be credited with the next unrelated change, such as a caret blink. The Host
therefore matches only within 500 ms (`inputLatencyFrameWindowUs`); a
candidate without a changed frame by then is dropped and counted as
`expired`. It also recognises a slow periodic rhythm that no input caused:
changed frames captured while no input was injected for the previous 300 ms
are kept (five at most, 20 s); with at least three whose shortest gap is at
least 100 ms and whose other gaps are whole multiples of it (30 ms tolerance),
a changed frame on the next beat is not credited to the pending candidate and
is counted as `ambient`. Typing never counts as a rhythm; video or other fast
changes are not filtered. Both counts appear in `stream.stats`
`inputFrame:{samples, ..., expired, ambient}` (last 30 s, at most 64 events).
A rising `expired + ambient` with a flat percentile is not a latency
regression.

**Relay validation.** Exact keys; a different `sessionId` is ignored (a
retired worker); an unnegotiated session, a sample never forwarded or
durations out of range (`0 <= injectUs <= holdUs <= 10 s`) drop the Host
channel as `invalid_control`. Each sample is acknowledged at most once. Tags
are forwarded only while the stream is ready, no resize is pending, the epoch
matches and samples increase.

**After the encoder (frame-timing-v1).** The broker adds
`inputFrameTimingProtocol:'frame-timing-v1'` to a measured session's
`session.start`; tags then carry three more host-clock integers (0..10 s):
`captureUs` (injection -> capture tick of the changed frame), `encodeUs` (that
tick -> encoder output) and `queueUs` (encoder output -> this tag's write),
all three or none. Host `stream.stats` adds
`inputSend:{samples, queueP50Ms, queueP95Ms, writeP50Ms, writeP95Ms}`, the relay
adds `relayForward:{samples, p50Ms, p95Ms}` (the tagged packet -> the client
socket accepted it).

**Client.** Per acknowledged sample, `input->host = (round trip - holdUs) / 2 +
injectUs`. For a tag the next video packet with that PTS is watched until the
engine hands its frame to the texture; `input->frame = (push completed - input
sent) + queued->texture`. Per input the client also derives `inputTransfer`
(tag arrival -> packet arrival) and `inputDownlink` (packet arrival - send -
uplink estimate - the tag's Host durations). The statistics dialog shows p50/p95
over the last 64 samples within 30 s as `입력→호스트 적용`, `입력→화면 표시`,
`호스트 수신→적용` and `입력 확인 왕복`; `측정 안 함` when not granted.

| Quantity | Status |
|---|---|
| Client send -> ack received (round trip) | measured, client clock |
| Host receipt -> injection call returned (`injectUs`) | measured, Host QPC |
| Host receipt -> ack write (`holdUs`) | measured, Host QPC |
| Client -> Host one-way network and relay | **estimated** as (round trip - hold) / 2; assumes a symmetric path |
| Client input -> Host injection | **estimate** = one-way estimate + `injectUs` |
| Client send -> tagged frame handed to texture | measured on the client clock; a proxy |

Limits: the origin is the moment `RemoteSession` sends the message, so the
pointer coalescer (up to 16 ms for moves) and earlier dispatch are excluded.
SendInput returning is not the application having processed the input. The
acknowledgement travels behind video, which the estimate halves. The first
changed frame can be an unrelated change, and Windows Graphics Capture may
deliver a frame composed shortly before the injection; both under-report.
Compositor and monitor scan-out are excluded. Lossless-tile republishes are
not tagged, and independent outputs are not measured.

## Lossless tiles (tiles-v1)

In band; no broker or relay change. A client whose native engine composites
tiles sends its ping nonces as `lt1-<n>`; the Session0 service still answers
the ping and forwards `stream.lossless` to its worker, and a track socket
delivers the ping to the worker directly. Only then does the Host send tiles-v1
payloads (`TSL1` magic) inside ordinary non-key kind-1 media packets at PTS
T-1 before the tick's video frame T, which every relay, Dart and native gate
forwards unchanged (PTS still strictly increasing per track). Clients that do
not opt in never receive them; Hosts without tiles only answer the ping. Wire
format, invalidation rules and measurements:
[lossless-tiles-design.md](lossless-tiles-design.md). While 마우스 잠금 holds
the mouse the client sends `lt0-<n>` instead (at once, and in every ping until
the release): the Host drops every tile with its next video frame and sends
none until the next `lt1-`.

## Clipboard

Local PC <-> VM sharing with the ordinary Ctrl+C/Ctrl+V and context-menu copy
and paste, subject to the administrator's per-assignment permissions:
`textToRemote`, `textToLocal`, `filesToRemote`, `filesToLocal`, all default
denied and enforced by the relay on every message
(`server/src/clipboard.mjs`).

### Channel

- Offered for a stream session whose Host advertises `clipboard:true` and
  whose assignment allows any direction. The connect response then carries
  `clipboardWebSocketUrl` (`/api/v1/sessions/:id/clipboard`, Bearer) and
  `session.start` carries `clipboardUrl`
  (`/api/v1/hosts/sessions/:id/clipboard`, the VM's current Host token). Both
  stay bound to the same live session and assignment; one socket per role.
- JSON text only, at most 256 KiB per message. The relay keeps chunks in
  memory and never writes clipboard data. Losing a permission closes the pair
  (`clipboard_policy_changed`).
- Only the Host's interactive worker accesses the VM clipboard; the service
  forwards the channel over its private pipe.
- When both peers are attached the relay sends each
  `clipboard.ready {sessionId, protocol:'clipboard-v1', limits, ...}`. The
  `limits` object depends on the profiles both peers announced (below).

### Transfers

`clipboard.begin` (the manifest) -> `clipboard.chunk {index, offset, data}`
(canonical base64) acknowledged with `clipboard.chunk.ack` ->
`clipboard.item.end` with the item's SHA-256, acknowledged with
`clipboard.item.ack` -> `clipboard.commit` -> the receiver publishes and
answers `clipboard.applied` -> the relay sends `clipboard.complete` to both.
`clipboard.read {requestId, format?}` from the client asks the Host for its
clipboard; with `format` (`text` or `files`) the answering `begin` must belong
to that permission class. `clipboard.cancel` and `clipboard.error {reason}`
end a transfer.

Text is UTF-8 on the wire and CF_UNICODETEXT at the Windows ends. Files use
safe leaf names, arrive as regular files in a protected per-user transfer
folder (marked as Internet-zone content) and are published as CF_HDROP. One
transfer at a time per pair; 30 s idle ends it. Cancellation before commit
removes provisional files and keeps the previous clipboard; once the commit is
forwarded an interruption reports `clipboard_commit_uncertain`, and no
rollback is promised. Completed files survive disconnects; the per-user store
expires completed transfers after 24 hours during a later transfer and keeps
at most 16 GiB, never touching unrelated files or paths that are still on the
clipboard.

### Profiles

Each profile builds on the ones above it. A peer announces a profile with a
header when it opens its clipboard socket; the relay applies a profile only
when both peers announced it and every profile below it. Any other pairing
keeps its previous `limits` object byte for byte: a released peer receives the
released numbers exactly, and a newer peer paired with a released one receives
the released numbers plus `maxTextBytes` and `limitsProfile:2`, so it refuses
what the other side could not take before sending anything.

| Profile | Header (each peer) | `clipboard.ready` adds | What changes |
|---|---|---|---|
| released | - | `{chunkBytes:32768, maxFiles:128, maxFileBytes:256 MiB, maxTotalBytes:512 MiB, maxTextUnits:32768}` | text and flat file lists; 32 KiB stop-and-wait; 15 min per transfer |
| limits v2 | `X-Multitap-Clipboard-Limits: 2` | `limitsProfile:2`, 4 GiB per file, 8 GiB per transfer, 1 MiB text | peer reasons `clipboard_limits_exceeded`, `clipboard_disk_full`, `clipboard_file_locked` (an older recipient gets `clipboard_transfer_failed`, `clipboard_write_failed`, `clipboard_file_unavailable`); a file transfer is refused up front when the volume cannot hold it plus 64 MiB; 6 h per transfer |
| formats | `X-Multitap-Clipboard-Formats: 2` | `formatsProfile:2`, `chunkBytes:46080`, `maxEntries:10000`, `maxDepth:32`, `maxPathChars:160`, `maxRichBytes:8 MiB`, `maxImageBytes:256 MiB`, `maxImagePixels:64000000`, `windowUnits:32`, `windowBytes:1 MiB` | rich text, images, folder trees, sliding window, virtual-file sources |
| paste-pull | `X-Multitap-Clipboard-Pull: 1` | `pullProfile:1`, `maxOffers:2` | large file copies are offered and pulled when pasted |
| scale | `X-Multitap-Clipboard-Scale: 1` | `scaleProfile:1`, `maxFiles:200000`, `maxFileBytes`/`maxTotalBytes` 2^53 - 1, 64 MiB text and HTML, `maxEntries:200000`, `maxDepth:512`, `maxPathChars:1024` | no size limits, long paths, precise reasons, throttling instead of ending |
| on-paste | `X-Multitap-Clipboard-OnPaste: 1` | `onPasteProfile:1`, `pushTextBytes:1048576`, `pushImageBytes:8388608` | a copy is silent; files move only when pasted |
| pull queue | `X-Multitap-Clipboard-PullQueue: 1` (needs on-paste) | `clipboardPullQueue:'pull-queue-4-v1'` | up to 4 outstanding pulls per requester |
| settled | `X-Multitap-Clipboard-Settled: 1` (needs on-paste and clipboard-sync-v1) | `clipboardSettled:'text-image-250-v1'` | text and image changes are announced only after 250 ms of quiet |
| intent | `X-Multitap-Clipboard-Intent: 1` (needs settled) | `clipboardIntent:'text-image-pending-v1'` | `clipboard.intent` and watch epochs |

### Automatic sharing (clipboard-sync-v1)

The connect field `clipboardProtocol:'clipboard-sync-v1'` (stream only;
anything else is 400 `invalid_clipboard_protocol`) asks for automatic sharing;
it is enabled when the Host advertises `clipboardSync:true`. The response
echoes `clipboardProtocol` and `clipboardSync`, and both peers'
`clipboard.ready` carry the session's `clipboardPolicy` and `clipboardSync`.

- While the remote view is active the client sends
  `{type:'clipboard.watch', sessionId, enabled}` (with the intent profile also
  a rising `watchEpoch`); enabling needs a to-PC permission. Local copies are
  sent by the client.
- The Host announces its copies with `{type:'clipboard.changed', sessionId,
  sequence}` (with `settled:'text-image-250-v1'` and `watchEpoch` in those
  profiles), only while watched. The relay forwards at most one announcement
  per 100 ms, the latest; the client then fetches the content with
  `clipboard.read`, which in the settled profile may carry `expectedSequence`
  and `textImageOnly` (and `watchEpoch` with intent) to bind the read to the
  announced copy.
- A copy can change the clipboard several times in a row (an application
  publishing in steps, delayed rendering). The Host announces once, after
  250 ms of quiet for text and images and 80 ms for files. With the intent
  profile it also sends `{type:'clipboard.intent', sessionId, sequence, kind,
  watchEpoch}` (`kind`: `text`, `richtext`, `image` or `other`) as soon as it
  sees a copy, before the quiet period ends.
- A newer copy on either side cancels an automatic transfer that has not
  reached its commit; a transfer the user started is never cancelled.

### Formats profile

**Permission mapping** (the administrator permissions are unchanged):

| Format | Wire `format` | Permission |
| --- | --- | --- |
| plain text | `text` | text |
| text with HTML and/or RTF | `richtext` | text (the formats are parts of the text) |
| image (bitmap) | `image` | files (binary content, like a file) |
| files | `files` | files |
| folders / files and folders | `tree` | files |

A formats peer paired with an older one sends plain text and flat file lists
only, and the relay treats `richtext`, `image` and `tree` on such a pair as
`clipboard_invalid_message`.

**Manifests** (`clipboard.begin`):

- `richtext`: `items:[{kind, size}...]` with `kind` in the fixed order `text`
  (always first, UTF-8, within the text limit), `html` ("HTML Format", UTF-8,
  at most the rich limit) and `rtf`. The peer is not trusted: the receiver
  publishes CF_UNICODETEXT and a *sanitised* `HTML Format` only. A
  conservative tokenizer re-serialises the HTML (allow-listed elements and
  attributes; no script, style, object, embed, frames, forms, svg, math, link,
  meta, base, comments or event handlers; URLs only `http`, `https`, `mailto`
  (href) or `data:image/png|jpeg|gif|webp` (img src), judged after entity
  decoding and whitespace stripping, so UNC, protocol-relative, `file:`,
  `javascript:` and `ms-*:` links and CSS `url()` are removed; `SourceURL` is
  dropped) and rewrites the header offsets. The sender sanitises too. `rtf`
  stays in the manifest for compatibility but current peers never send it, and
  a received RTF item is verified and dropped (RTF can embed OLE objects and
  fetch external content). The HTML header (`Version`, `StartHTML`,
  `EndHTML`, `StartFragment`, `EndFragment`, optional selection offsets) is
  checked on both sides; HTML that does not validate is not sent (the sender
  sends the text), and HTML that arrives invalid fails the transfer without
  touching the clipboard.
- `image`: `items:[{kind:'png', size, width, height}]`, at most 64,000,000
  pixels (sides up to 65,535) and 256 MiB of PNG. The sender reads CF_DIBV5 /
  CF_DIB (BI_RGB 1-32 bit, BI_BITFIELDS, V4/V5 headers; alpha only where the
  header declares an alpha mask) and encodes PNG with WIC, or sends an
  existing "PNG" format as is when it is a plain PNG within the bounds. The
  receiver decodes it with WIC, checks the size against the manifest and
  publishes "PNG", CF_DIBV5 (32-bit, straight alpha) and CF_DIB (24-bit,
  transparency composited over white). An image is sent only when the
  clipboard has neither files nor allowed text.
- `tree`: `entries`, `files`, `bytes`, `tops`, `skipped`, then
  `clipboard.entries {start, entries:[{path, kind, size, attrs}]}` pages (at
  most 256 entries each). `path` uses `/`; every component passes the same
  leaf rules as file names (no `..`, absolute, drive, UNC or device paths, no
  `\`); `kind` is `file` or `dir` (empty folders are kept); parents come first;
  no case-insensitive duplicates; `attrs` is a subset of
  read-only/hidden/system/archive (0x27). Declared counts and sizes must be met
  exactly. Data items are the files, addressed by their ordinal among files
  (`index`), so any file can later be addressed on its own. Symbolic links,
  junctions and other reparse points are neither followed nor sent; `skipped`
  counts them. A repeated top-level name from different folders is renamed
  ("name (2).ext") by the sender. The receiver recreates the tree in a new
  private transfer folder, keeps a receipt that lists every entry so expiry
  removes exactly what was received, and publishes the top-level items as
  CF_HDROP.

**Sliding window.** A formats pair may have up to 32 units (a chunk or an item
end) and 1 MiB of chunk data unacknowledged; chunks are at most 45 KiB (46,080
bytes) because the Host's private pipe frames stay below 64 KiB. Throughput
comes from the window, not the chunk size. Acknowledgements are cumulative:
`clipboard.chunk.ack {index, offset}` releases a chunk and every earlier unit,
`clipboard.item.ack {index}` everything up to a verified item end. A receiver
acknowledges after four chunks or 128 KiB and always at an item end. The relay
closes the pair with `clipboard_out_of_order` for a unit beyond the window, an
acknowledgement that names no pending unit, or a commit while units are
pending; after a cancel or error it drops (bounded) the messages of that
request still in flight. Such pairs may move 2,048 messages and 64 MiB per
second; all pairs together 96 MiB per second, and crossing that ends (or, on a
scale pair, slows) the pair that used the most.

### Paste-pull profile

Like RDP and AnyDesk: copying a large set of files or folders announces only
its manifest, and the bytes cross when the user pastes, only for the files the
paste reads. Offers and pulls on a pair without the profile are
`clipboard_invalid_message`.

**Push or offer.** A copy of files and folders is offered when it holds more
than 16 MiB of file data or more than 256 entries; anything smaller is pushed
(CF_HDROP), so a quick copy still pastes into any application. Text, rich text
and images are never offered in this profile.

**Offer.** `clipboard.offer {requestId, format:'tree', entries, files, bytes,
tops, skipped}` followed by `clipboard.entries` pages, like a tree manifest,
with every entry also carrying `mtime` (milliseconds since 1970). The client
sends it unprompted; the Host sends it as the answer to `clipboard.read`. The
receiver publishes its OLE data object and answers `clipboard.applied`; the
relay then sends `clipboard.complete`. The offer stays valid after that: the
relay and the offering peer keep the latest two offers per direction (older
ones answer `clipboard_offer_expired`) and drop them with the channel.

**Pull.** The receiving peer asks for one file when something reads it:
`clipboard.pull {requestId:'pullc-<n>'|'pullh-<n>', offerId, index, offset}`
(`c` from the client, `h` from the Host, `n` rising per requester). The answer
is the ordinary windowed `clipboard.chunk` stream from `offset`, then
`clipboard.item.end` whose `sha256` covers the delivered range. The receiver
acknowledges as its reader consumes the data, so an unread stream stalls the
sender after one window. One pull is active per pair (with the pull queue,
four per requester, served in order, each with a 4 MiB / 128-unit window);
another request is answered `clipboard_busy` and queued by the requester.
`clipboard.cancel {requestId}` from the requester (stream released, seek,
timeout) or `clipboard.error {requestId, reason}` from either side
(`clipboard_selected_file_changed`, `clipboard_offer_expired` and the usual
peer reasons) ends a pull. The relay checks the profile, the id's letter and
order, that the offer is one of the other peer's two newest, the direction's
files permission, that `index` and `offset` are inside the manifest and the
window rules. A pull idle for 15 minutes (a paused Explorer dialog may wait
that long) is cancelled for both peers; the channel continues.

**Receiver.** Nothing is written to disk. The receiver publishes an OLE data
object with OleSetClipboard (never OleFlushClipboard, which would render every
file) offering `FileGroupDescriptorW` (every entry as a relative path with
attributes, size and write time), `FileContents` (one `IStream` per file),
`Preferred DropEffect` = copy, `ZoneIdentifier` and `UntrustedDragDrop` =
Internet zone, and `IDataObjectAsyncCapability` (Explorer extracts on a
background thread). CF_HDROP is not offered for a pulled offer: it would force
a full download at copy time, so applications that take only CF_HDROP cannot
paste one, which is why small copies are pushed. `IStream::Read` requests
bytes on demand; `Seek` to another position starts a new range there (verified
by its own hash); releasing a stream cancels its pull; a read waits at most
30 s for a byte and then fails with `ERROR_TIMEOUT`; a mismatching range hash
fails the stream with `ERROR_CRC`. The data object lives on an STA thread with
a message loop (the client's clipboard thread, the Host's interactive
clipboard worker). Our own data object is recognised (`OleIsCurrentClipboard`
and the clipboard sequence of the publish) and never announced back.

**Integrity levels.** The Host's interactive worker runs with the logged-on
user's token, for a split-token administrator the linked elevated token (High
integrity, never SYSTEM), while Explorer in the VM runs at Medium, so a
Medium-integrity Explorer calls into a High-integrity data object. Extraction
across integrity levels in both directions was checked with
`tools/clipboard-pull-qa` (`Run-Integrity.ps1`). A read that waits for data
pumps COM calls, and the data is delivered from the channel thread.

**Offerer.** The sender keeps a snapshot of the selected paths with the
identity of every entry (volume serial, file ID, size, last write time) and
opens a file only while a pull reads it, closing it between chunks with
read/write/delete sharing. If the path has gone or names another file, a
same-volume lookup by the copy-time file identity is tried; identity, size and
write time must still match before data is read and before its digest is
returned. A file that no longer matches fails that item with
`clipboard_selected_file_changed`; a file another program holds open for
writing fails with `clipboard_file_locked`; the other items still pull.

Each file costs at least one round trip, so a folder with thousands of tiny
files pastes slowly over a slow link (below the push threshold it is pushed).
Explorer does not add the Internet-zone mark (`Zone.Identifier`) to files it
extracts from the data object.

### Virtual files as a copy source

Sources that publish only an OLE data object (`FileGroupDescriptorW` +
`FileContents`, no CF_HDROP: zip-folder contents, mail attachments, phone and
cloud shell items) are read on the copying side, so nothing changes on the
wire: the staged items go through the same file and tree capture and the same
push or offer as an Explorer copy. Serving from the live data object was
rejected because a source may release it (or exit) while the peer pulls.

- `native/windows_clipboard` `stageVirtualFiles` / `read` with
  `ReadOptions::virtualFiles`, used only on formats pairs (otherwise a
  virtual-only copy is `unsupported_clipboard_format`). The staged copy lives in
  a new private `source-<32 hex>` folder and is deleted when its last holder
  lets go; folders left by a crash are swept after 24 hours.
- The OLE calls run on a dedicated STA thread, not on the clipboard worker,
  and the Win32 clipboard is released before the object is read. With no
  progress (bytes or items) for 60 s the copy gives up with `clipboard_busy`.
- Names are relative paths with `\` or `/`; a trailing separator or a
  directory attribute marks a folder; parents are implied. The whole copy is
  refused when any name is hostile (`..`, drive letters, devices, control
  characters, an unterminated 260-character field, case-insensitive repeats, a
  name used as both file and folder). `FD_FILESIZE` is a hint checked against
  the limits up front; content is always read to its end (IStream preferred,
  HGLOBAL as fallback) under the per-file and total caps. Attributes and write
  times are applied after the content.
- Nothing is sent from a copy that includes an item without `FileContents`, an
  `IStorage` medium, or an object this process published itself.

### Scale revision

Big copies are pulled when pasted and flow-controlled by the window, so their
size costs neither relay memory nor receiver disk, and the earlier caps (4 GiB
per file, 8 GiB per transfer, 128 top-level items, 10,000 entries, 1 MiB of
text) are lifted:

| | limits v2 / formats / pull | scale revision |
| --- | --- | --- |
| file, transfer | 4 GiB, 8 GiB | none (2^53 - 1 bytes) |
| top-level items | 128 | none (the entry guard) |
| entries of one copy | 10,000 | 200,000 (a memory guard, below) |
| text, HTML | 1 MiB, 8 MiB | 64 MiB |
| relative path | 160 characters, names of 240 | 1024 on the wire, 259 can be pasted; names of 255 |
| nesting | 32 levels | 512 (follows from the path length) |
| image | 256 MiB, 64 MP | unchanged |
| transfer duration | 15 min (6 h with limits v2) | none: idle 30 s, a pull idle 15 min |
| rate limit | pair ended above 64 MiB/s | pair slowed to 64 MiB/s |

**Entry guard.** A paste hands Explorer one `FILEGROUPDESCRIPTORW`, 592 bytes
per entry in one block (118 MB at 200,000 entries), which Windows builds for
every `GetData` and which cannot be produced lazily; 200,000 is the largest
list whose descriptor is still reasonable to allocate. The list is kept once
per offer and travels in pages of at most 256 entries.

**Pushed content stays small.** The peers decide with `shouldOffer` (more than
16 MiB, more than 256 entries, a list of long names that would not fit the
48 KiB `clipboard.begin`, or a path above what a descriptor holds); the relay
enforces the same bounds for a pushed tree or file list on a scale pair.

**Paths.** Sources are read through extended-length (`\\?\`) handles
everywhere in `native/windows_clipboard`, so the absolute path of copied files
is limited by Windows (32,767), not by MAX_PATH. Explorer, however, creates a
pasted item at `<destination>\<relative path>` and cannot create one of 260
characters or more. With `IFileOperation` (the engine of an Explorer paste,
measured with a copy test) on Windows 11 24H2:

| Case | Result |
| --- | --- |
| source path 292 characters, relative path 120, short destination | all copied (long source paths are read) |
| source path 400+, relative path 283 | `Top` and the first three folders were created, the file at 327 characters was not: `PerformOperations` returned S_OK with `GetAnyOperationsAborted` true |

A relative path of 260 characters or more therefore cannot be pasted into any
folder and is refused up front, before a byte is sent: by the sender's tree
walk (`PathTooLongError`, after the whole walk so the message names the
longest path), by the Dart session and the relay (`min(profile limit, 259)`),
and by the receiver. 259 is also the size of `FILEDESCRIPTORW::cFileName`, so
an offer's entries are checked the same way. The 1024 on the wire stays as the
bound a future consumer can use.

**Receiver placement (pushed tree).** The transfer folder
(`...\Multitap\ClipboardTransfers\transfer-<32 hex>`) is written through
extended-length names. When every absolute path stays below 260 characters
the top-level items are published as CF_HDROP. Otherwise the same received
content is published as a virtual-file data object (`FileGroupDescriptorW` +
`FileContents`, one `IStream` per local file), so Explorer creates the files
at the paste destination and never sees the long staging path. Its clipboard
sequence is marked as our own so it is not announced back, and the folder is
not expired while it is on the clipboard.

**Precise reasons.** In a scale pair a peer reports the limit it met:
`clipboard_path_too_long`, `clipboard_text_too_large`,
`clipboard_entry_count_limit`, `clipboard_depth_limit`,
`clipboard_image_too_large`. Every other pair is sent
`clipboard_limits_exceeded` (peers downgrade themselves, and the relay does
the same for a peer that sends one anyway). The client turns them into the
Korean message of that limit, for a path with the longest length the PC saw
(`가장 긴 경로 N자, 폴더 안의 경로는 259자까지`). Only a pair without the
revision still prints the sentence that lists every limit. Every size, count,
depth, path, text and image cause maps to its limit reason; locked, full,
busy, changed, denied, unavailable and unsupported causes map to themselves;
anything else is `clipboard_read_failed`, `clipboard_write_failed` or
`clipboard_transfer_failed`, never a limit.

**Durations and rates.** A scale pair has no total duration: a transfer ends
only after 30 s without a message, a pull after 15 minutes. Above the per-pair
rate (2,048 messages and 64 MiB per second; 96 MiB per second across the
relay) a scale pair is paused until the second is over, so the sender's
window stops it, instead of being ended.

### On-paste revision

A copy is silent: no file byte crosses the relay until a consumer pastes.

- Every file or folder copy is an offer, including an empty file. Text or
  rich text of at most 1 MiB together and PNG of at most 8 MiB may still sync
  ahead silently. Larger text, rich text and images are offered by format:
  `clipboard.offer` carries `format` `text`, `richtext` or `image` and the
  item metadata a `clipboard.begin` would (text size and units, optional HTML
  size, PNG size, width and height). Applying an offer publishes the peer's
  delayed data object; it does not mean the content was downloaded or pasted.
- The consumer's `GetData` or virtual-file stream requests `clipboard.pull` by
  item index with the existing chunk, window and hash rules. Text renders
  CF_UNICODETEXT; rich text also sanitised HTML; an image PNG, DIBV5 and DIB. A
  file set always offers FileGroupDescriptorW/FileContents; a set of at most
  16 MiB and 256 entries whose staged paths stay below MAX_PATH also offers a
  delayed CF_HDROP that stages the files on demand, and further reads reuse
  that copy for up to ten minutes. Rendering runs off the OLE thread while its
  message loop keeps dispatching; cancellation, a newer copy and permission
  or session loss abort waiting renders and active streams.
- A changed source clipboard retires its offers and sends
  `clipboard.offer.withdraw {requestId:<offerId>}`. The relay accepts only an
  offer owned by that peer; the consumer aborts its reads and empties its
  clipboard only if that exact offer is still current. Source changes while
  serving fail as `clipboard_selected_file_changed`.
- The Host reports an application in the VM that waits for the PC's data as
  `clipboard.paste.waiting {requestId:'wait-N', active}` (no names or
  content). Before Ctrl+V reaches the VM the client waits for its own
  announcement to be applied; it does not prefetch file bytes. If the user
  cancels a PC-to-VM paste between file ranges, the client withdraws that
  offer, so a later paste needs a new copy.

**Client presentation.** The client shows a transfer only when somebody waits
for it: a manual send or receive, a paste in the VM that waits for the copy,
or a paste on this PC pulling virtual files. A background copy shows no card
and no failure notice (a paste that needs it reports the failure). A waited-for
transfer gets a card after 700 ms (text after 1.5 s) with a cancel button
(`clipboard.cancel`), and, while another window is in front, a small
no-activate card window and taskbar progress. On an on-paste pair this card is
the only progress (the virtual-file descriptors omit `FD_PROGRESSUI`); on older
pairs Explorer's own copy dialog is the progress for virtual files.

### Diagnostics

Failures always end as a fixed reason (`clipboard_*`), and a cancel or a
closed relay ends the transfer with `clipboard_cancelled` /
`clipboard_transport_closed` without publishing anything of it.

- Client clipboard action 2 (a transfer) records `remote` as the transfer's
  direction (`true`: PC to VM) and `error = windowsDetail * 100 + origin * 10 +
  step`: origin 1 the native adapter threw, 2 the session's own check or send,
  3 the peer reported it, 4 the channel closed, 5 a timeout, 6 an unexpected
  exception; step 1 capture, 2 read a chunk, 3 digest a file, 4 begin
  receiving, 5 append, 6 finish an item, 7 commit. `windowsDetail` holds the
  Win32 error or low 16 bits of an HRESULT in bits 0-15, the clipboard holder
  in bits 16-17 (0 unknown, 1 another process, 2 this thread, 3 another thread
  of this process) and in bit 20 a failed retry after releasing this process's
  own hold. Action 3 distinguishes a protocol failure (1), a pull violation
  (2), a handler exception (3), socket termination (4) and relay termination
  (6). Actions 12 and 13 record pull ranges.
- Host: `hostAction` 253 (`apps/windows_host/src/clipboard_diagnostic.hpp`),
  one record per transfer start, end and failure with fixed numbers only:
  `stage` (1 receive begin, 2 send begin, 3 receive end, 4 send end, 5 receive
  failed, 6 send failed, 7 the PC reported a failure, 8 the PC cancelled, 9 a
  paste request failed), `format` (1 text, 2 files, 3 rich text, 4 image, 5
  folder, 6 offered files), `items`, `kib`, `error` (the reason exchanged with
  the peer), `cause` (the internal condition) and optionally `win32`. The
  worker sends `clip.diag` over its pipe and the service journals it.
- A clipboard open for writing retries for up to two seconds; OLE publication
  rechecks the expected clipboard sequence before every retry so that a new
  user copy made meanwhile is kept. A Host worker whose interactive desktop
  check fails reports `no_interactive_session` with the cause
  `clipboard_desktop_unavailable`.

Names, paths, text, process names and exception text never appear in any
record.

### Copy-source matrix

What is captured, sent and published, shared by the PC client and the Host
(`native/windows_clipboard` `read`, `ReadOptions{text, files, rich, images,
reportDenied}`). On an on-paste pair every file or tree row means an offer
first, small text and images can sync silently, and larger content renders on
paste.

Order of choice: a **file list** (CF_HDROP, or the shell's own item list when
every item has a file path) -> **text** (CF_UNICODETEXT, with sanitised HTML on
a formats pair) -> **image** (PNG, else CF_DIBV5/CF_DIB) -> **virtual files**
(FileGroupDescriptorW + FileContents, formats pairs only). One exception: when
an image is offered **and** the file list is 1 to 4 plain image files (`.png
.jpg .jpeg .gif .bmp .webp`, no folder), the copy is a picture (screenshot
tools save the capture and put both on the clipboard), so the **image** is
sent and the files are not. If the image cannot be read after all (too large,
damaged), the files are sent instead. Image, file list and tree need the
**files** permission; text and HTML need the **text** permission.

| Source | Formats on the clipboard | What is sent | What the receiver publishes | Expected paste |
| --- | --- | --- | --- | --- |
| Explorer, one file / many files | CF_HDROP (+ shell list) | files (flat), or an offer when large | CF_HDROP / virtual files | the same files |
| Explorer, a folder or files + folders | CF_HDROP with a directory | tree (pushed, or offered when large) | CF_HDROP of the top-level items | folders with contents |
| Explorer, one to four image files, no bitmap | CF_HDROP | files | CF_HDROP | the files (it is a file copy) |
| Explorer / Photos, one to four image files **and** a bitmap | CF_HDROP + bitmap | **image** | PNG + DIBV5 + DIB | a picture (Paint, chat, Office) |
| Image file(s) with any other file, a folder, or more than four files, bitmap present | CF_HDROP + bitmap | files / tree | as above | the files |
| Snipping Tool, Win+Shift+S (bitmap + saved PNG) | PNG + DIB(V5) + CF_HDROP(.png) + shell list + virtual files | **image** | PNG + DIBV5 + DIB | a picture |
| PrintScreen, Paint selection, browser "copy image" (PNG/DIB, + HTML `<img>`, no text) | bitmap, PNG, HTML | image | PNG + DIBV5 + DIB | a picture |
| Word / PowerPoint picture or shape, Excel chart | bitmap / PNG / EMF (EMF is not sent) | image | PNG + DIBV5 + DIB | a picture |
| Excel cells, Word text, browser text selection or link | text + HTML (+ CSV, RTF, bitmap) | rich text (RTF and bitmap dropped) | text + sanitised HTML | formatted text; the bitmap is not carried |
| Notepad, VS Code, terminal, chat text (CRLF/LF, CJK, emoji) | CF_UNICODETEXT | text, UTF-8; at most 64 MiB (scale pair) or 1 MiB | CF_UNICODETEXT | identical text |
| Text with an embedded NUL | CF_UNICODETEXT | text up to the first NUL (Windows ends it there); an unterminated block is refused: `clipboard_read_failed` | | |
| Chat app image | PNG / DIB (+ file in a temp folder) | image (a single temp image file next to the bitmap counts as the screenshot case) | PNG + DIBV5 + DIB | a picture |
| Library / Quick access / Recycle Bin items, items without a file path | shell list only | nothing: `unsupported_clipboard_format` | unchanged | the user is told |
| Zip-folder contents (Explorer), mail attachments, phone and cloud shell items, archiver virtual items | FileGroupDescriptorW + FileContents (IStream or HGLOBAL per item), no CF_HDROP | **files / tree**: staged in a private `source-<id>` folder, then sent like any file or folder copy | CF_HDROP of the staged copies (or virtual files for an offer) | the files and folders, with empty folders and declared attributes / write times |
| Virtual files, one item without FileContents, a name the tree rules refuse, a source that fails or has gone | FileGroupDescriptorW (+ partial FileContents) | refused: `unsupported_clipboard_format` (no FileContents), `clipboard_file_unavailable` (bad name, failing or released source), `clipboard_limits_exceeded` (count, depth, path, size), `clipboard_disk_full` | unchanged | the user is told |
| Virtual files whose source stops answering (an attachment still downloading, a phone asleep) | FileGroupDescriptorW + FileContents | `clipboard_busy` after 60 s without a byte or an item; slower sources that keep delivering are read to the end | unchanged | try again |
| Virtual files and text together | FileGroupDescriptorW + CF_UNICODETEXT | the text (files are tried only when nothing else is usable) | text | the text |
| UNC / mapped / WebDAV (drive redirection) / OneDrive placeholder paths | CF_HDROP with such paths | files, read when the path can be opened (a placeholder is fetched by the open); not readable: `clipboard_file_unavailable`, locked: `clipboard_file_locked` | CF_HDROP | the files |
| Source path beyond MAX_PATH (a deep folder; up to 32,767 characters) | CF_HDROP | read through extended-length handles; sent as usual | CF_HDROP, or virtual files served from the local copy when the received absolute path would reach 260 characters | the same files and folders |
| Relative path inside a copied folder of 260 to 1024 characters | CF_HDROP | refused up front: `clipboard_path_too_long` (scale pair) or `clipboard_limits_exceeded`; the PC names the longest path, e.g. "가장 긴 경로 305자, 259자까지" | unchanged | the user is told; Explorer cannot create such an item in any folder |
| Relative path of 161 to 259 characters | CF_HDROP | sent on a scale pair; an older pair refuses it (limit 160, `limits_exceeded`) | as above | the same files |
| Source path above 32,767 characters, or a name Windows cannot open | CF_HDROP | `clipboard_file_path_invalid` / `clipboard_file_unavailable` | unchanged | the user is told |
| A file above 4 GiB, more than 128 top-level items, more than 10,000 entries (scale pair) | CF_HDROP | an offer, no size or count limit; 200,000 entries at most (`clipboard_entry_count_limit`) | virtual files, pulled when pasted | the files, pasted by Explorer |
| Text of 1 MiB to 64 MiB (scale pair) | CF_UNICODETEXT | text, windowed; 64 MiB at most (`clipboard_text_too_large`); an older pair stops at 1 MiB (`limits_exceeded`) | CF_UNICODETEXT | identical text |
| Empty clipboard | nothing | `clipboard_empty` | unchanged | the user is told |
| Clipboard held open by another program | OpenClipboard fails | `clipboard_busy` after the short retries | unchanged | try again |
| Delayed rendering that fails (owner returns nothing) | format listed, no data | `clipboard_read_failed` | unchanged | the user is told |
| Anything copied, but its permission is off | any | `clipboard_policy_denied` | unchanged | the user is told |
| Pair without the formats profile | image only | `unsupported_clipboard_format`; image + files send the files | | update both sides |

The decision is visible in the Host record: stage 11 carries the formats mask
and stage 2 the chosen `format` (a screenshot shows `format` 4, image).

## Printing (print-v1)

Optional for stream sessions, single output and tracks alike. While a granted
session runs, the VM has a printer **Multitap 프린터 (PC)**; what is printed
there arrives at the client as a PDF, and the client prints it on a printer of
the user's PC (or saves it) after the user chooses, like RDP Easy Print or
Horizon integrated printing. No driver is installed anywhere. Design notes:
[print-redirection.md](print-redirection.md).

**Permission.** The assignment's `printers` redirection permission (user x VM,
default denied; stored in the per-assignment device policy,
`server/src/device-policy.mjs`). It is checked when the session is admitted and continuously by the
relay (the 250 ms authorisation sweep and the assignment update hook). A
revocation closes the print channel with `print_policy_changed` and the Host
removes its printer; the stream continues.

**Negotiation.** The Host advertises `printProtocol:'print-v1'` only when
Windows already has everything (the Spooler service runs, the in-box
"Microsoft Print To PDF" driver and the in-box "Standard TCP/IP Port" monitor
are present; checked at most once a minute); otherwise it may report
`printUnavailableReason` (`print_spooler_unavailable`,
`print_driver_unavailable`), shown to clients whose assignment allows printers
so that they can say why there is no printer. A client sends
`printProtocol:'print-v1'` for a stream (malformed: 400
`invalid_print_protocol`). The broker grants it when the Host advertises it and
the assignment allows printers: the response carries `printProtocol` and
`printWebSocketUrl` (`wss://<origin>/api/v1/sessions/<id>/print`),
`session.start` carries `printProtocol` and `printUrl`
(`wss://<origin>/api/v1/hosts/sessions/<id>/print`). Not granting is never a
failure. A client rejects a grant it did not request, a grant without its URL
and a URL for another origin or session.

| Client | Host | Broker | Printing |
|---|---|---|---|
| released | any | any | none (never requested) |
| print-v1 | released / no PDF driver | any | none (not advertised) |
| print-v1 | print-v1 | released | none (capability stripped) |
| print-v1 | print-v1 | print-v1, permission denied | none (not granted) |
| print-v1 | print-v1 | print-v1, permission allowed | VM printer, local printing |

**Channel.** One socket pair per session (the client with its Bearer token,
the Host with its Host token; a closed pair is not reopened: 409). JSON text
only, at most 256 KiB per message. When both are attached the relay sends both
`{"type":"print.ready","sessionId","protocol":"print-v1","limits":{"chunkBytes":131072,"windowChunks":8,"maxJobBytes":268435456}}`.

Host -> client (exact fields, validated by the relay):

- `{"type":"print.queue","state":"ready"}` or
  `{"type":"print.queue","state":"unavailable","reason"}` with `reason`
  `print_driver_unavailable`, `print_spooler_unavailable`,
  `print_name_conflict` (another printer already has the name) or
  `print_setup_failed`.
- `{"type":"print.job","jobId","format":"pdf","size","title"}`: `jobId`
  `[A-Za-z0-9_-]{1,64}`, new in this pair; `size` 1 byte to 256 MiB; `title` the
  spooler's document name, at most 200 UTF-16 units, no control characters,
  may be empty. One document is in transfer at a time.
- `{"type":"print.chunk","jobId","offset","data"}`: canonical base64 of up to
  128 KiB, `offset` exactly the bytes sent so far, never past `size`, and never
  more than 8 chunks (1 MiB) beyond the client's last acknowledgement.
- `{"type":"print.end","jobId","sha256"}` once every byte was sent; the relay
  hashes the chunks on the way and refuses a different digest.
- `{"type":"print.skipped","reason"}`: a document the Host received but did not
  offer (`print_job_too_large`, `print_storage_unavailable`,
  `print_job_failed`).

Client -> Host: `{"type":"print.ack","jobId","offset"}` after each chunk is
written; `{"type":"print.result","jobId","result"}` once per ended document
(`printed`, `saved`, `cancelled`, `failed`; diagnostics only). Either side may
send `{"type":"print.cancel","jobId"}` during a transfer; messages of a just
ended or cancelled job still on their way are dropped, never applied to the
next one. Anything else, a broken order, a window overrun, a bound or a digest
closes the pair with `{"type":"print.error","reason"}` (`print_invalid_message`,
`print_out_of_order`, `print_limits_exceeded`, `print_hash_mismatch`,
`print_busy`, `print_policy_changed`, `print_timeout` after 60 s without
progress or 30 min per document, `print_rate_limit`, `print_backpressure`,
`print_transport_closed`, `print_pair_timeout`, `authorization_revoked`,
`session_ended`, `server_stopping`). A print failure never ends the stream.

**Host** (`apps/windows_host/src/print_redirection.hpp`). Only a stream
`session.start` with the marker and its own `printUrl` starts anything. When
the pair is ready the service (LocalSystem) prepares the queue and answers
`print.queue`:

- It listens on a fresh loopback port (`127.0.0.1`, `SO_EXCLUSIVEADDRUSE`),
  adds an in-box Standard TCP/IP RAW port `Multitap_Print_<port>` to it (SNMP
  off) and a printer `Multitap 프린터 (PC)` with the in-box "Microsoft Print
  To PDF" driver, `winprint` and queued printing (a document is complete
  before it is sent). The printer's security descriptor lets only the
  session's Windows user (and that user's AppContainer apps) print and manage
  their own documents, SYSTEM everything and Administrators manage the queue
  without Print; nobody else has an entry. The descriptor is verified after
  creation; a queue that would be usable by anyone else is deleted.
- A loopback connection is accepted only from the Print Spooler service
  process (or its PrintIsolationHost in session 0), found as the owner of the
  connection's other end; any other process is closed unread. Each connection
  is one document, kept in a delete-on-close file in
  `ProgramData\Multitap\Host\private\print` (SYSTEM/Administrators only; every
  folder on the way opened as itself and refused when it is a link), hashed
  with SHA-256 and offered. At most 256 MiB per document and 8 documents /
  512 MiB waiting; while that budget is full the spooler's next connection
  waits (its document stays queued in the VM). A larger document is read to
  its end (so the VM's queue completes) and reported as
  `print_job_too_large`.
- The printer and its port are removed (pending documents purged) when the
  channel or the session ends. Printers on `Multitap_Print_<n>` ports, unused
  ports of that name and stale document files left by a crash or power loss
  are removed at the next service start (after waiting up to 90 s for the
  spooler) and before every install. Every other printer, port, driver and
  setting is left alone; a foreign printer with the same name is never
  touched (`print_name_conflict`).
- While the queue is ready it is the session user's default printer: the Host
  saves the previous default in a per-user record (`MultitapHost.exe
  --print-default`, `print_default.hpp`) and puts it back when the channel
  ends, unless the user picked another default meanwhile; after a crash the
  next start restores it.
- Diagnostics: `hostAction` 242 with a fixed `stage` (1 installed, 2 removed,
  3 received, 4 delivered, 5 too large, 6/19 storage, 7 untrusted peer, 8
  cancelled, 9 leftovers removed, 11 driver missing, 13 name conflict, 14
  listener, 15 port, 16 printer, 17 security, 18 user, 20 spooler, 21-24 the
  client's result, 25 channel closed, 26 read failure), `ok`, the Windows
  `error` and a count in `expected`; never a document title, name or content.

**Client** (`lib/data/print_session.dart`, `lib/ui/print_dialog.dart`,
`windows/runner/client_print.cpp`). The Windows client requests print-v1
(`HttpBrokerApi(requestPrint: true)`), opens the print socket with the first
picture, writes each document to
`%LOCALAPPDATA%\Multitap\Client\print\<session>`, acknowledges each chunk after
writing it, verifies the SHA-256 and then shows one dialog per document: the
printers of this PC (the one used last is preselected, else the default),
copies (1-20), **인쇄**, **PDF로 저장** (Windows' save dialog; a link or a
file with several names is refused) and **취소**. Printing opens the PDF with
Windows' own renderer (Windows.Data.Pdf) and sends vector pages through the
Direct2D print control on the XPS print path with the printer's defaults;
copies repeat the document (collated). Received files are deleted after the
choice, when the session ends and, after an abnormal end, by a later session
after a day. Nothing prints without the user's choice. The Android app keeps
documents in its cache directory and hands each one to Android's own print
dialog (PrintManager, `AndroidPrint.kt`), where the user picks a printer or
"Save as PDF"; the outcome is `printed` when Android wrote the document, else
`cancelled`.

## Independent-output UDP media (`udp-tracks-v1`)

The Host advertises `udpTracksHostProtocol:'udp-tracks-v1'`. A capable Broker
exposes `udpTracksProtocol` only while its media-enabled UDP relay is available.
The client explicitly requests that marker along with `tracks-v1` and
`udp-media-v1`; `udp-direct-v1` remains a separate optional request. Older
peers and disabled UDP retain the existing authenticated WSS track transport.
The connect response and Host's track `session.start` echo the granted marker.

Only after every output's real WSS IDR and the existing decoder-ready ACK does
the Broker send `udp.tracks.grant`. It contains `sessionId`, `topologyEpoch`,
`generation`, and one or two `tracks` rows (`track`, `epoch`, `primary`,
`bitrateTarget`, and the role's ordinary `udp.grant`). Each output has separate
tickets, X25519 keys, AEAD counters, sockets, and authenticated path checks.
All legs share the parent session's authorization, rate bucket, and revocation.
Audio belongs only to the primary output; TVT1 framing is restored before the
normal track decoder sees an admitted UDP frame.

`udp.tracks.key`, `.local`, and `.peer` carry the exact `sessionId`, `track`,
`topologyEpoch`, `epoch`, and `generation`. Stale tuples are ignored. All
Host-facing controls travel on the authenticated track tunnel, ordered with
reliable input. Reconfiguration retires both pairs and grants fresh keys under
a larger generation. Unsupported epochs above 65535 remain on WSS. The Host
receives scoped `udp.tracks.lease` about once a second and stops direct traffic
and input after two seconds without a current authorized lease.

Input has one session-wide sequence and deduplicator across outputs and
regrants. Authoritative WSS copies of `input.direct` continue during UDP
retirement so key releases/resets are retained; the existing track readiness
and topology checks still decide whether ordinary input may be injected.

Since direct media bypasses the Broker, `udp.tracks.feedback` reports only
new frames actually admitted by the native UDP receive path. It carries the
current topology/generation and at most two rows (`track`, `epoch`, `pts`,
`frames`, `bytes`). A newer scoped PTS refreshes only that established output's
source watchdog. Feedback cannot create an IDR, decoder readiness, or advance
the WSS media PTS. Each output independently falls back to UDP relay/WSS; mixed
paths must not be labeled as an entirely direct connection.

This key exchange authenticates the UDP legs against their grants. It does
not protect against an active Broker substituting public keys and is not a
claim of independently authenticated end-to-end encryption.

## Drives and USB devices

Both are per-session socket pairs relayed like printing, each behind its own
per-assignment permission (default denied) and closed, without ending the
stream, when that permission is withdrawn.

**Drives (drive-v1).** Folders the user shares on the PC appear as a drive in
the VM. Host capability `driveProtocol:'drive-v1'` (with an interactive
session), plus `driveFs:true` when the Host has the WinFsp backend; a Host
whose WebClient service is unavailable reports
`driveUnavailableReason:'drive_webclient_unavailable'` instead. The client
sends `driveProtocol` (and `driveFs:true` for the ranged file system, which
adds the `setsize` and `flush` operations); malformed is 400
`invalid_drive_protocol`. The `drives` permission grants it: the response
carries `driveWebSocketUrl`, `session.start` `driveUrl`. The relay checks every
request, keeps the client's share list (at most 8 shares; unknown or read-only
shares are refused there), bounds requests in flight and never stores file
data, names or paths. Contract: [drive-redirection.md](drive-redirection.md);
broker side `server/src/drive.mjs`.

**USB devices (usb-v1).** A USB device of the client PC is lent to the VM
(bulk, interrupt and control endpoint 0; never isochronous): the Host runs a
loopback USB/IP server that only the VM's usbip-win2 driver connects to and
forwards its URBs to the client.

- Request: `usbProtocol:'usb-v1'` in the connect body. Granted (`usbProtocol`
  and `usbWebSocketUrl` in the response, `usbProtocol`, `usbUrl` and
  `usbGrant:{drives, printers, smartCards, serialPorts}` in `session.start`)
  only when the Host advertised `usbProtocol` and the assignment allows
  `usbDevices`. A device whose descriptors include a class with its own
  permission (mass storage, printer, smart card, CDC serial) also needs that
  permission. In a tracks session the response adds
  `usbTracksProtocol:'usb-tracks-v1'` and the tracks `session.start` carries
  `usbUrl` too.
- Channel: paired sockets `/api/v1/sessions/{id}/usb` and
  `/api/v1/hosts/sessions/{id}/usb`; JSON `usb.ready`, `usb.status`,
  `usb.attach`, `usb.attached`, `usb.rejected`, `usb.detach` (at most 16 KiB),
  and binary URB frames with a 24-byte little-endian header.
- Contract, framing, limits, fixed reasons and the Host and client behaviour:
  [usb-redirection.md](usb-redirection.md); broker side `server/src/usb.mjs`.

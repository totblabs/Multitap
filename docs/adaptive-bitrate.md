# Adaptive streaming bitrate (`bitrate-adaptive-v1`)

With automatic bitrate ("자동", the default for new connections) the Host
follows what the path to the client carries instead of streaming at a fixed
rate. A fixed high rate would only build a queue on a slower path; a fixed low
rate wastes a fast one. The ceiling is the Host's advertised `maxBitrate`
(at most 100 Mbps); a session starts at 20 Mbps.

Wire fields are summarised in
[remote-session-protocol.md](remote-session-protocol.md#adaptive-bitrate).
Code: `apps/windows_host/src/bitrate_controller.hpp` (pure, clock-injected
logic), `bitrate_session.hpp` (glue in the Session0 service),
`server/src/stream-bitrate.mjs` (broker rules) and `StreamOptions.negotiated`
in `apps/windows_client/lib/models/stream_options.dart`.

## Where the signals are

The relay does not pass TCP backpressure from its client leg to the Host leg:
it buffers per client socket and ends the session when that buffer overflows
(`client_backpressure`). The Host's own send queue therefore sees only the
Host→relay leg, which is usually fast. The likely bottleneck (relay→client
Internet path, relay kernel buffers, a home router, a busy client) is visible
only end to end. So:

- **Receiver-measured one-way delay (primary, WSS path).** The client records,
  for each video packet, its 64-bit Host PTS and its local monotonic arrival
  time (taken when the Dart receive loop handles it, so client backlog counts
  too) and sends them every 100 ms as `stream.feedback`
  (`frames:[[pts,arrivalUs],…]` with at most 64 frames, received bytes and the
  interval). The relay validates it, adds its own unsent bytes toward the
  client (`relayQueuedBytes`) and forwards it to the Host.
- **Host send queue (local, immediate).** The service reports the queue depth
  and the oldest packet's age on every enqueue and after every WebSocket
  write, plus the bytes each completed write carried.
- RTT pings stay a display metric; the controller does not use them.

The Host maps each reported PTS to the time its packet entered the send queue,
so `arrival − enqueue` excludes capture and encode time (CPU encoding at 4K
takes tens of milliseconds and is not a network signal). A 10 s windowed
minimum of that value, allowed to rise by at most 2 ms/s (clock drift, route
change) so that a standing queue is not learned, is the baseline; the
remainder is queueing delay anywhere after the encoder.

## Controller

The controller runs in the Session0 service, which owns the broker socket. It
only publishes a target: `ServiceBridge` stores it and its pipe thread forwards
`{type:'stream.bitrate'}` to the interactive worker, never while a resize,
initial or track RPC is reading that pipe. The worker applies it to the live
encoder (no re-initialisation, no IDR) within the geometry floor and the
negotiated ceiling.

- **Burst-aware excess delay.** A virtual queue drains the bytes sent on a
  link 25 % faster than the target (plus measured audio): the delay our own
  burstiness explains (IDR, a page change and its refinement frames). The
  congestion signal is the *excess* over it, as the minimum over each feedback
  interval (a standing queue, CoDel-style), or the relay's queue converted to
  time beyond the virtual queue, whichever is larger.
- **Back off once per event, fast.** Overuse = excess > 30 ms on two
  consecutive intervals while it keeps growing (≥ 90 % of the previous
  interval), or one interval above 400 ms. Then target = min(0.85 × target,
  C × clamp(1 − excess/1 s, 0.4, 0.95) − audio), where C is the rate delivered
  over the last ~200 ms (at least half the ~500 ms rate, since a stall empties
  the former). No second cut while the queue shrinks by ≥ 15 % per interval;
  hold for clamp(excess, 0.3 s, 1 s).
- **Spurious-cut undo.** A link stall looks like a capacity drop for ~150 ms.
  If within 1 s the path delivers ≥ 1.1 × the previous send rate while the
  queue shrinks, the previous target is restored.
- **Application-limited cuts.** When video used < 70 % of the target over the
  last 0.5 s (typing, cursor, small updates), what the path delivered is only
  what was sent, not its capacity. A cut is then × 0.85 only (no C clamp, no
  congestion-rate memory), and if within 3 s the queue drains below 30 ms
  without growing, the target before the series is restored: a cut that cannot
  change what is sent did not cure the queue. A real bottleneck below the
  limited rate keeps cutting × 0.85 until video is no longer limited, then the
  ordinary C clamp applies. Without this, one delay spike during light activity
  on a test VM took the target from 52 Mbps to the geometry floor.
- **Overflow guard.** Host queue ≥ 48 packets or ≥ 4 MiB, its oldest packet
  150 ms older than the virtual queue explains, or the relay queue ≥ 4 MiB:
  target = min(½ × target, 0.8 × measured Host write rate − audio). While
  application-limited only the 4 MiB byte limit triggers it: a stalled write
  ages the queue and 100 audio packets/s reach 48 packets in ~0.4 s, but a
  lower target changes neither, and the write rate measured during a stall is
  no capacity. An adaptive session's Host queue holds 128 packets / 8 MiB
  (fixed sessions: 32 / 8 MiB); at 100 audio packets/s, 32 packets would be
  only ~0.2 s, which a 100 Mbps stream overflows before a rate change can act.
- **Probe gently, only with evidence.** After 0.5 s without congestion, with
  excess < 10 ms and the encoder using ≥ 70 % of the target: × 1.03 per
  100 ms, or × 1.006 within [0.9, 1.15] × the last congestion rate (remembered
  for 30 s), never above 1.5 × the current use or the ceiling. A frame larger
  than four per-frame budgets (a page change) holds probing for its drain plus
  1.6 s, the refinement train: refinement bursts count neither as congestion
  (the virtual queue explains them) nor as probing evidence. An idle desktop
  (only changed pictures are encoded) neither raises nor lowers the rate.
- **Geometry floor.** Never below 0.04 bit per pixel-frame, clamped to
  2–20 Mbps (1080p60 5.0, 1440p60 8.8, 4K60 19.9 Mbps); see the encoder
  measurements below.
- **Encoder limited.** If the encoder output (bursts and tiles excluded) stays
  above 1.5 × target for 2 s, cuts are futile: no further cut (overflow guard
  included), and the target follows output / 1.3 for at least 5 s.
- **Missing feedback.** No report for 2 s while video is sent: × 0.85 per
  second.

The worker applies a target through `Encoder::setBitrate` (x264: VBV maxrate,
buffer and CRF; Microsoft MFT: CBR mean bitrate) and
`FrameEncodePolicy::retarget(FrameRefinement::forCbr(…))`. Lossless tiles of
an automatic session use the part of the target that video leaves unused
(target − video of the last 0.5 s, at least 0.5 Mbps), so tiles and video
share one rate; fixed sessions keep the tiles' own `forBitrate` budget.
`stream.stats` adds `bitrateTarget`, `bitrateMode`, `bitrateMax` (worker) and
`networkDelayMs`, `deliveredKbps` (service).

## On the UDP path (`udp-media-v1`)

While media travels over UDP, `stream.feedback`, the broker channel's queue
and its write rate no longer describe it. On every path change the controller
drops its delay baseline, delivery and loss history (`pathChanged`) and is then
fed by the UDP leg (`native/windows_video/udp_media.hpp`):

- **Delay.** The round trip of a `TAIL` datagram, which skips the pacer; its
  rise above the windowed minimum is network queueing, with no virtual queue to
  subtract.
- **Loss.** Each `FEEDBACK` reports datagrams received and given up since the
  last one. Only what NACK could not repair counts. Over the last 300 ms (at
  least 30 datagrams), ≥ 1 % given up, or loss in three reports, cuts the
  target to (1 − loss) × 0.95 of the rate before the episode (at least
  0.3 ×). A single lossy report cuts at most 15 %: a blackout is not
  capacity. Loss that stays at the level a cut was made for is treated as the
  path's own (radio, outage): the rate is held, without probing, until the loss
  stops for 2 s or grows well beyond it. Any loss in the last second blocks
  probing.
- **Backlog.** Video refused by the UDP pacer triggers the overflow guard.

## Encoder measurements (Microsoft MFT, `MultitapHostEncoderBitrateTest`)

These measurements on the Microsoft software H.264 MFT (the fallback encoder)
explain the geometry floor. `CODECAPI_AVEncCommonMeanBitRate` was changed on
the running encoder; no re-initialisation, no IDR, every frame decoded by the
client's `RemoteDecoder`. 1080p60 detailed motion:

| change | frames to settle (10-frame mean ±20 %) | 2nd-half rate | first frame / budget |
|---|---|---|---|
| start 20 M | 1 | 20.7 M | IDR 7.2 |
| 20 → 8 M | 3 | 8.02 M | 2.5 |
| 8 → 60 M | 8 (QP ramps over ~10 frames) | 61.6 M | 0.13 |
| 60 → 100 M | 0 | 96.9 M | 0.8 |
| 100 → 12 M | 2 | 12.2 M | 7.7 (one frame at the old QP) |

4K60 detailed motion 60 → 19.9 (floor) → 100 → 30 Mbps: 100 M settled in 5
frames at 95.8 M, 36.8 dB. A 4K60 MFT *started* at 8 Mbps (text scroll, a
3.2 MB IDR = 161 budgets), or lowered to 10–14 Mbps under detailed motion,
entered a state of ~130–155 Mbps output at ~13–15 dB that a later 100 Mbps
target did not repair; 0.04 bit per pixel-frame recovered in every case. At
1080p a 2 Mbps start degraded but recovered at 60 Mbps.

A whole-page text change at 4K coded by the MFT is a ~4.9 MB P frame at both
20 and 100 Mbps, above the 4 MiB media packet bound. x264's VBV keeps every
frame far below it ([x264-encoder.md](x264-encoder.md)).

## Controller on a simulated link (`MultitapHostBitrateControllerTest`)

CBR encoder with the measured rate lag, 1.5 Mbps PCM audio, Host queue, FIFO
relay→client bottleneck with 20 ms propagation, client feedback every 100 ms
on a drifting clock (+40 ppm), 5 ms target delivery:

| scenario | target | delay p50 / p95 / max | cuts | queues |
|---|---|---|---|---|
| fixed 30 Mbps path, 15–60 s | mean 27.8 (of 28.5 video) | 21 / 65 / 77 ms | 12 | no overflow |
| 200 Mbps path | 20 → 100 (ceiling); ≥ 95 by 25 s, mean 89.5 over 0–40 s | 10 / 11 / 32 ms | 0 | – |
| 60 → 12 Mbps at 20 s | ≤ 10.5 by 21.5 s, mean 10.0 | max 1.33 s, p95 < 150 ms again by 23 s | 7 | no overflow |
| 12 → 60 Mbps at 40 s | 54+ by 55 s | 17 / 62 ms | – | – |
| 50 Mbps, 150 ms stall every 4 s + 0.2 % 120 ms spikes | mean 36.7 | p95 154 ms (the stalls) | 17 in 45 s | no overflow |
| page switch every 4 s on 40 Mbps | constant 20 (held) | p95 247 ms (the 1.3 MB frames) | 0 | – |
| page switch every 4 s on 12 Mbps | ≤ 12, then held | – | – | no overflow |
| 60 s idle after motion | constant | – | 0 | – |
| feedback stops | × 0.85/s | – | – | – |
| Host uplink 1 G → 40 Mbps at 100 M | mean 31.5 | p95 70 ms, max 467 ms | 10 | no Host overflow (128) |
| encoder floor 6 M on a 4 Mbps link (720p30) | holds ~4.6, no spiral | (the path cannot carry it) | 0 after detection | relay overflow, as expected |
| 4K60 on a 10 Mbps link | never below the 19.9 M floor | (cannot fit) | – | – |
| 200 Mbps, light updates after motion, 150 ms link stall every 4 s | held at 100 | max 134 ms | 0 | – |
| 200 Mbps, light updates, 300 ms Host write stall every 4 s | held at 100 | p95 378 ms (the stalls) | 0 | no Host overflow |
| 3.2 Mbps link, light updates 720p30, 20–40 s | 17 (light is not throttled) | 28 / 38 / 42 ms | 0 | no overflow |

These are simulations. Linux/Node socket buffering hides part of the relay
queue from `bufferedAmount`, and the client's arrival time includes its own
event-loop jitter.

## Negotiation and compatibility

| Peer | Field | Rule |
|---|---|---|
| Host → broker capability | `bitrateProtocol:"bitrate-adaptive-v1"`, `maxBitrate` (20–100 M) | Both or neither; a malformed value is omitted (the desktop stays fixed-rate) and never invalidates other capabilities. |
| Client → broker options | `bitrateMode:"auto"\|"fixed"`; `bitrate` ≤ `maxBitrate` | Auto: `bitrate` is the ceiling. Only for an advertising Host; otherwise 400 `invalid_stream_options` for a fixed rate above 20 M and 409 `stream_bitrate_unavailable` for auto, before any session exists. Independent outputs (tracks-v1): fixed only. |
| Broker → client | capability in the desktop list; the connect response adds `bitrateProtocol` when auto is granted | The client sends only `StreamOptions.negotiated()` options. |
| Broker → Host `session.start` | auto: `bitrate` = min(20 M, ceiling), `bitrateMode:"auto"`, `bitrateMax` | Hosts without the capability never receive these. |
| Client → relay → Host | `stream.feedback` | Only in a session that negotiated auto; any other session ends with `invalid_input`. |
| Host → client `stream.stats` | `bitrateTarget`, `bitrateMode`, `bitrateMax`, `networkDelayMs`, `deliveredKbps` | Extra fields; older brokers and clients pass or ignore them. |

| client choice \ Host | without `bitrate-adaptive-v1` | with `bitrate-adaptive-v1` |
|---|---|---|
| 자동 (default) | fixed 20 Mbps | auto, start 20 Mbps, ceiling `maxBitrate` |
| 직접 설정 | fixed 4–20 Mbps | fixed 4 Mbps–`maxBitrate` |
| two independent outputs | fixed ≤ 20 Mbps | fixed ≤ `maxBitrate` (자동 → fixed 20 Mbps) |

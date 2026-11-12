# Multitap Streaming lossless tiles (`tiles-v1`)

## Problem

H.264 (Constrained Baseline, 4:2:0) codes static desktop text and icons lossy.
Progressive refinement (`frame_encode_policy.hpp`) sharpens text, but 4:2:0
still smears ClearType colour fringes and small colourful icons. Goal: moving
content keeps the ordinary video; a region that stays static for a short time
is sent as lossless tiles composited over the decoded video, so a static desktop
becomes pixel-identical to the captured picture. A changed tile hands back to
video before the client shows the newer video frame.

## Why in band

Older relays accept only media kinds 1 (video) and 2 (audio) and drop the Host
channel on anything else (`validMediaEnvelope` in `server/src/relay.mjs`); the
track tunnel (`server/src/track-media.mjs`) and every client gate also require
strictly increasing PTS per track. Broker capabilities and `session.start` are
rebuilt from fixed fields. Therefore:

- tiles ride inside ordinary **kind-1, non-key** media packets, which every
  relay forwards unchanged;
- a tile packet uses `pts = T−1` before the video frame `T` of the same tick
  (the Host emits tiles only while the encoder has no pending input, so no
  older video PTS can follow);
- the opt-in rides in the client's ping nonce, which every relay and Host
  already passes through or answers;
- at most one tile packet per tick per output, adds at most every 50 ms; a
  drops-only packet is tiny.

## Negotiation

- A client whose native engine composites tiles sends its RTT pings as
  `lt1-<n>` instead of `<n>` (`NativeRemoteMedia(losslessTiles: true)`). Hosts
  without tiles just answer the ping; RTT matching is unchanged.
- Host: the worker sets `losslessRequested` when it sees such a ping (track
  sockets deliver pings to the worker); the Session0 service, which answers
  pings itself, forwards `{"type":"stream.lossless","protocol":"tiles-v1"}`
  over the private pipe. The flag lives for the session ID (kept across
  resizes).
- The Host never sends a tile packet without that opt-in. No broker field, no
  relay change.
- Pause: while 마우스 잠금 holds the mouse, the client's pings say `lt0-<n>`
  (one at once at the hand-over and every periodic ping after it), and `lt1-`
  again at the release. The service forwards
  `{"type":"stream.lossless","protocol":"tiles-v1","on":false}` (latest wins in
  the worker's RPC backlog). A Host without pause support reads `lt0-` as a
  plain ping and keeps its tiles.

## Retirement under a running client decoder

The client clears its tiles only with a new engine (epoch, topology, stream-size
change). Whenever tiles stop while the same decoder keeps running — the pause
above, or the capture rebuilt on a new mode of its output
(`capture_follow.hpp`, after which tiles never return in that stream) —
`losslessStreamTick` retires them: one packet that drops EVERY tile index,
due with that tick's video, which is submitted in any case (an IDR after a
follow). The source then knows nothing and keeps its sequence, so a resume
continues on the same decoder. A requested IDR while retired retires again; if
the first packet was lost in the send queue, the sequence gap makes the client
stop compositing tiles.

Independent outputs (tracks-v1, 모든 모니터) do the same per output: the pause
retires every output's tiles, and an output whose capture followed a mode change
retires its own for good while the other keeps them. A retired source frees its
BGRA mirror and previous picture (8.5 + 3.2 MB at 1912×1114) and allocates them
again on its first tick after a resume.

## Host pipeline (per output: `Agent::stream`, `TrackSourcePipeline::State`)

`losslessTick` (`apps/windows_host/src/lossless_tile_source.hpp`, shared by
both paths and the tests): `capture.frame()` → `policy.prepare(nv12, T)` → if
video is submitted, phases 1–3 run on a per-output helper thread **in parallel
with** `encoder.encode(T)` (outputs buffered) → send [tile packet, pts T−1]
then [video, pts T]. Without a submission the phases run inline; a drop then
forces one submission.

- Source of truth: the *composed BGRA at the remote's real (source)
  resolution* (`Capture::visitComposed`), cursor included when the cursor is
  drawn into the video. Unscaled: the WGC staging readback plus the cursor
  patch, exactly what became this tick's NV12. Scaled (encoder smaller than the
  viewport, see below): the unscaled WGC surface the downscaled picture was
  drawn from plus a source-resolution cursor canvas (`CursorComposer`). Scaled
  GDI capture has no unscaled view, so no tiles. The Host keeps a full BGRA
  mirror; change detection is exact (`memcmp` per tile row), so a BGRA change
  too small to show in NV12 is still detected.
- Dirty hint: no new WGC surface frame ⇒ only the previous/current cursor tiles
  can differ ⇒ a static desktop costs no comparison.
- Picture anchor: the NV12 the encoder gets decides as well, so a tile never
  shows other content than the video under it even if the BGRA view or its
  dirty hint were wrong. A tile whose part of the picture changed since the
  previous tick is dropped with that video frame and cannot settle while it
  keeps changing. Unscaled, a tile is added only if its BGRA converts
  (`bgraToNv12`, each 2×2 block on its own) to exactly the picture's bytes
  there. Downscaled, "its part" is the picture pixels whose area-average source
  box lies inside the tile (2 px further in), and a tile is added only if its
  last picture change came with a change of its own BGRA. Unscaled, a picture
  change without a BGRA change, or a refused add, is counted in
  `stream.stats.lossless.mismatches` (cumulative per source; 0 when capture is
  consistent).
- Grid: 64×64 tiles (edge tiles partial), index u16 row-major.
- Phase 1 (before the video decision): compare only tiles the client currently
  shows losslessly. Changed ⇒ *drop*, mirror updated. Drops force a video
  submission on this tick, so a drop always accompanies a decoded frame.
- Phase 2: one packet = drops (applied by the client at decoded pts ≥ T) plus
  adds. Adds need: encoder idle (no pending input), first IDR sent, ≥ 50 ms
  since the last add packet, the tile unchanged for 200 ms (`settleUs`), and a
  token bucket. Its rate is read every tick (`LosslessSettings::
  targetBytesPerSecond`): a fixed-rate session uses the video target
  (`forBitrate`); an automatic-bitrate session uses the part of the target that
  video left unused, at least 0.5 Mbps. Burst = a quarter second of the rate.
  Quiet ticks: packet ≤ 256 KiB, 3 ms encode budget. Ticks whose content
  changed (video encoded in parallel): ≤ 32 KiB, 1 ms, so static regions still
  become lossless while another region moves. An add re-verifies the tile
  against the current composed frame before encoding.
- Phase 3 (after the video was queued): compare the remaining dirty tiles to
  keep settle timing; no effect on this tick's video latency.
- Cursor: tiles carry the same composed pixels as the video, so a still cursor
  is exact and a moved cursor's old and new tiles are dropped with the video
  frame that moves it. Never erased or duplicated.

## Wire format (payload of a kind-1, non-key media packet)

```
0  'T''S''L''1'   (H.264 Annex-B payloads start with 0x00)
4  u16 width, u16 height          (source size; = decoder size except in a scaled stream)
8  u32 sequence                   (1, 2, 3… per encoder; a gap poisons the client)
12 u64 dropPts                    (= pts of the video frame that follows)
20 u16 dropCount, u16 addCount
24 u16 drop indices[dropCount]
   adds: {u16 index, u8 codec=1, u8 0, u32 size, size bytes}
```

Little endian. Tile codec `TQ1` (own implementation: QOI-style byte ops plus a
copy-from-above run; RGB only, alpha = 255 like decoded video). Header-only, no
new dependency (`native/windows_video/lossless_tiles.hpp`, shared by Host and
client, C++17).

## Client compositor (`lossless::Compositor`)

- One per decoder (`RemoteEngine`, and each track in `RemoteTrackDecoder`); a
  new epoch, topology or resize creates a new engine, so everything is cleared.
- Full-frame RGBA lossless layer plus per-tile `present` and `dropAtPts`.
- Tile packet: record drops (pending until a decoded frame with pts ≥
  `dropPts`), decode adds into the layer and republish (a copy of the latest
  published frame plus the added tiles), so lossless content appears without
  waiting for the next video frame.
- Decoded video frame: apply due drops, then write every present tile over it
  before publication. A stale tile is therefore never composited on the video
  frame that carries its change, nor on any later frame.
- A malformed payload, size mismatch or sequence gap poisons the compositor: no
  tile is ever composited again in this engine (plain video), never a stale
  tile. Within an epoch the transport is ordered and lossless (an overflow ends
  the session), so a gap is defence in depth.
- Scaled stream (single output only): the first tile packet declares the
  source size, never smaller than the video and bounded by the wire, stride and
  memory checks. While any tile is present every published frame is source
  size: present tiles exact, the rest bilinear-upscaled from the decoded video
  (`native/windows_video/rgba_upscale.hpp`, SSE2, bit-exact to its scalar
  definition). With no tile present the video-size frame is published as
  before. The view sizes its box from `sourceWidth/sourceHeight` and lets
  Flutter scale whatever texture size it gets, so no Dart change is needed.
  Tracks are never scaled (the compositor refuses a different size there).

## Scaled video

The Host encodes a viewport at its own size unless it exceeds the negotiated
codec/decoder limits or the Host's encode budget (`stream_encode_size.hpp`,
`stream_size.hpp`); only then is it downscaled, aspect kept. Tiles support
scaled video so that a static region is pixel-identical at the remote's real
resolution even then.

Measured at a 2948×1112 viewport (`MultitapHostLosslessTilesScaledTest`, 60 Hz,
12 Mbps, Microsoft MFT encoder), video downscaled to 1920×724 versus encoded at
full size:

| | 1920×724 video | | 2948×1112 video | |
|---|---|---|---|---|
| | original | **+ tiles** | original | + tiles |
| desktop static: end PSNR / to pixel-identical | 23.6 dB | **∞ / 300 ms** | 30.1 dB | ∞ / 300 ms |
| page switch static | 19.0 dB | **∞ / 550 ms** | 26.0 dB | ∞ / 550 ms |
| full scroll (moving) | 18.6 dB | 18.6 dB | 25.1 dB | 25.1 dB |
| pane scrolls, rest static: static column exact while moving | no | **yes** | no | yes |
| video bytes desktop / page / pane-scroll phases | 342 K / 703 K / 4.55 M | same | 666 K / 1.51 M / 5.22 M | same |
| tile bytes desktop / page / final settle | — | 399 K / 869 K / 758 K | — | same |
| Host encode CPU per motion frame | 2.45 ms | 2.68 ms | 4.64 ms | 5.09 ms |
| Host encode CPU, whole 13 s run | 964 ms | 1005 ms | 2149 ms | 2301 ms |
| Host tile phases, whole run (parallel to encode) | — | 627 ms | — | 691 ms |
| client compose per decoded frame with tiles | — | ≈ 3.8–4.1 ms (upscale) | — | ≈ 2.4 ms |

With scaled support, static content is pixel-identical either way and the tile
bytes are the same. Encoding at full size buys moving-content quality
(≈ +6.5 dB) for ≈ 2× Host encode CPU and ≈ 2× static video bytes; the scaled
path moves cost to the client (a CPU upscale of ≈ 4 ms per frame at 2948×1112
while tiles are present).

## Measured (synthetic pixels)

`MultitapHostLosslessTilesEncoderTest`: 1920×1080, 60 Hz synthetic clock,
12 Mbps, Microsoft MFT encoder with refinement, client `RemoteDecoder` and
compositor; a GDI-rendered desktop (icons, ClearType labels, a dense coloured
text window) and a text page; WGC-like dirty hints. One run on a busy test VM;
timings vary.

| Scene | Time to pixel-identical | End RGB PSNR original → tiles | Video submits / bytes (original = tiles) | Tile bytes | Host tile CPU |
|---|---|---|---|---|---|
| desktop static 3 s | 250 ms | 29.56 dB → ∞ | 23 / 485,597 | 278,986 | 7.5 ms |
| page switch, static 3 s | 300 ms | 26.01 dB → ∞ | 45 / 1,034,310 | 515,731 | 9.1 ms |
| scroll 3 s (every tick) | — (moving) | 26.0 → 26.8 dB | 180 / 4,953,974 | 696 | 246 ms |
| map pan 3 s (rest of window static) | — | 30.7 → 34.6 dB | 180 / 4,488,157 | 153,580 | 293 ms |
| typing (glyph every 150 ms) | between quiet glyphs | 29.5 dB → ∞ | 40 / 247,860 | 131,167 | 36 ms |
| cursor moving 2 s | — | 29.5 → 75.2 dB | 120 / 147,778 | 33,915 | 232 ms |
| cursor stopped | 200 ms | 29.54 dB → ∞ | 4 / 3,789 | 479 | 3.6 ms |

- Video submissions and bytes are identical to the pipeline without tiles in
  every scene; the encoder never held an input.
- The tile phases lengthened the video path by 6.1 ms in total over 592 frames
  (max 1.8 ms, on motion ticks that also encode adds); encoder wall time rose
  9.6 % from CPU sharing with the helper. The motion-scene CPU above is mostly
  the exact BGRA comparison of lossless tiles (≈ 1.3–1.9 ms per changed 1080p
  tick, off the video path).
- Client: compositing 0.38 ms mean / 1.7 ms max per decoded 1080p frame.
- `RemoteEngine`: pixel-identical texture 250 ms after start and after a page
  change (720p), 350 ms for a 1440×810 source from 960×540 video. Two tracks
  (real clock): both outputs pixel-identical 264 ms after the opt-in, 231 ms
  after a change; no tile packet without opt-in; per-track PTS strictly
  increasing.

`stream.stats.lossless` (single output) and the native `lossless*` counters
report what a real session does.

## Limits

- Photographic content compresses poorly in TQ1 (≈ 3–4 B/px); the token bucket
  bounds the rate, so such screens take seconds to settle.
- Scaled streams cost the client a CPU upscale of the non-lossless area on
  every decoded frame while tiles are present; AVX2 or a GPU overlay could
  reduce it. Flutter's own scaling of the plain video-size frame and the CPU
  bilinear differ slightly in filtering.
- Change detection compares every lossless tile on each tick with a new
  surface frame; WGC dirty regions could reduce that CPU. GDI fallback capture
  has no dirty hint: a full comparison every tick.
- If an add packet were lost, the Host would wrongly believe the tile present:
  harmless (the video shows), never stale.
- BGRA changes too small to show in NV12 force one small extra video frame.

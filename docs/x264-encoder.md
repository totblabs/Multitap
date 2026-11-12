# x264 encoder (Multitap Streaming)

The Host's default H.264 encoder is x264 (GPL-2.0-or-later), linked statically
from a pinned commit. A GPU encoder (NVENC, AMF, Intel QSV through its MFT) is
used only when Host settings select one, and the Microsoft software H.264 MFT is
the fallback. This document covers the x264 path: backend selection, the
bitstream contract every backend keeps, the chosen settings with the
measurements behind them, and the build and licence obligations.

## Backends and fallback

- `host::Encoder` (`apps/windows_host/src/media.hpp/.cpp`) is a facade over
  `X264Encoder` (`x264_encoder.cpp`, only in builds with x264), `MftEncoder`
  and the GPU backends (`gpu_h264*.cpp`). Callers (the single-output agent
  path, tracks-v1, refinement, lossless and resize paths) use it the same way
  whatever runs underneath.
- The choice comes from `%ProgramData%\Multitap\Host\encoder.json`, which counts
  only when only SYSTEM and Administrators can write it, and is read at every
  encoder creation (new session, resize epoch, track topology).
  `{"videoEncoder":"mft"}` selects the MFT; `auto`, `gpu`, `nvenc`, `amf` or
  `qsv` try a hardware encoder first (optionally pinned with `gpuAdapter`);
  anything else, or no file, means x264. Host settings write this file.
- Fallback inside one encoder instance goes GPU → x264 → MFT. The only way
  back is the bounded GPU retry (`encoder_gpu_retry.hpp`): after a hardware
  encoder missed its output deadline (code 41, typically an overloaded VM),
  the same GPU selection is opened again 30 s later, off the encoding thread
  and on the encoder's own device, then after 60 s and 120 s more (three
  attempts per encoder instance). The stream changes over only when the
  reopened encoder has coded the current picture as one valid IDR at its PTS
  (no `stream.info`, the epoch continues); otherwise the CPU encoder goes on
  unchanged, without an extra IDR. A lost device, an unsupported format, a
  broken output contract or a refused reconfiguration is never retried, and a
  retry that ends in one of them ends the attempts. Each step is recorded as
  hostAction 200 stage 245, error = kind | attempt << 8 | GPU code << 16 (kind
  1 started, 2 back on the GPU, 3 open failed, 4 first picture failed, 5 no
  further attempt). x264
  moves to the MFT for the rest of that encoder's life on an open failure, an
  encode error, output that breaks the contract (PTS mismatch, delayed frame,
  IDR without SPS/PPS, non-contiguous NALs), a frame above
  `encoderMaximumFrameBytes` (4 MiB − 64) or an allocation failure. The MFT
  encodes the same frame at the same PTS as an IDR with its own SPS/PPS, so the
  client decoder continues inside the epoch (one extra IDR, every frame
  decoded). Caller exceptions are never mistaken for encoder failures: outputs
  are buffered until the backend call returned.
- The fixed code (`EncoderFallback`: 1 parameters, 2 open, 3 encode, 4 output
  contract, 5 oversize, 6 memory, 7 unknown) reaches the service as a stream
  failure with stage 40, and the Host's automatic diagnostics deliver it to the
  broker.
- `stream.stats` carries `"encoder"` (`x264`, `mft`, `nvenc`, `amf`,
  `qsv-mft`; it changes on a fallback). Track statistics carry an optional
  `encoderInfo` (`h264-encoder-v1`).

## Bitstream contract (the same for every backend)

| Property | MFT | x264 |
|---|---|---|
| Profile | Constrained Baseline (66, constraint flags 0xC0), CAVLC | identical (66, 0xC0), CAVLC |
| Level (1080p60) | 4.2 | 4.2 |
| Chroma / input | 4:2:0 from the `bgraToNv12` NV12 | the same NV12 buffer (`X264_CSP_NV12`, no conversion) |
| Colour | no VUI colour description; the client treats "unspecified" as BT.601 limited | VUI: matrix 6 (BT.601), limited range, primaries/transfer unspecified |
| B-frames / reordering | none / `max_num_reorder_frames` 0 | none / 0 |
| IDR | forced first frame, GOP fps × 60 | forced first frame (`X264_TYPE_IDR`), keyint fps × 60, scene cut off |
| SPS/PPS | prepended to every IDR | repeated before every IDR (`b_repeat_headers`) |
| Output timing | synchronous, pts = input | synchronous (zerolatency, `maximum_delayed_frames` 0 enforced), pts = input |

Both encoders also code an IDR on request (video dropped on its way to the
broker). On the UDP media path a client that reports `LOSS` changes the x264
stream once: the encoder reopens with a larger DPB (`recoveryReferenceFrames`
in `reference_history.hpp`: the level's DPB, at most 16 frames; the first frame
is an IDR with the new SPS) and later answers a loss by reference invalidation
(`x264_encoder_invalidate_reference`) instead of an IDR. Every other client
keeps one reference frame.

`MultitapHostX264EncoderTest`, decoding with the client's `RemoteDecoder`: IDR
indices at GOP 60 over 130 frames with a page switch every 7 frames are
`[0,60,120]` for both backends; colour-block PSNR on saturated patches is 55 dB
(MFT) and 65 dB (x264), while a 601→709 or limited→full misread scores 25.8 /
29.0 dB, so both are decoded with the same matrix and range. A High-profile
CABAC stream (superfast/high) and the SSE-only x264 code paths (Proxmox's
default `x86-64-v2` CPU type) also decode completely on the same decoder.
Packet framing, epochs and lossless-tile ordering (TSL1 at pts T−1 before video
at T) are outside the encoder.

## Settings and why

Production (`X264Settings::production()`): preset **ultrafast**, tune
**zerolatency**, profile **baseline**, sliced threads = processors in the
process affinity (at most 8; see [x264-thread-policy.md](x264-thread-policy.md)),
CRF with VBV:

- VBV maxrate = the negotiated target (fixed, or each live
  `bitrate-adaptive-v1` target through `x264_encoder_reconfig`, no IDR); VBV
  buffer = 0.5 s at that rate, never above 24 Mbit (3 MB), so every frame stays
  far below the 4 MiB packet bound.
- CRF (quality cap) 12; above 0.16 bit per pixel-frame (1080p60 at 20 Mbps) it
  drops 2 per doubling, down to 10, so granted bandwidth buys sharper text.
- Unchanged macroblocks are flagged to x264 (`mb_info`, see "CPU per frame").

### How it was measured

`MultitapHostEncoderBenchmark` (`src/encoder_benchmark.cpp`, runner
`tools/encoder-benchmark.ps1`) drives `host::Encoder` the way the agent does:
the production `FrameEncodePolicy` with `FrameRefinement::forCbr` (change-only
submission, 1 s refresh, refinement bursts), a 60 Hz clock paced in real time,
`bgraToNv12`, and every output decoded by the client's `RemoteDecoder`. "auto"
is the controller's 20 Mbps start followed by scripted live retargets every
0.5 s (20→12→8→16→30→40→24→10 Mbps, clamped to the adaptive floor); it is not a
network-driven controller run. The benchmark draws its own scenes: a ClearType
document scrolling 6 px per frame, a map panning beside a static sidebar,
whole-page switches every 2 s (4 pages; the production policy decides
refinement and refresh), and typing (one glyph every 50 ms on a static page).
Ink PSNR scores only pixels whose source is darker than 200 (text), against the
BT.601-limited reference the client shows. Link figures assume a link that
drains at exactly the target rate (the worst case). CPU = process CPU cycles
during each encode call (all encoder threads), with the process affinity set to
4 (or 2) logical processors of a 10-vCPU test VM (AMD Ryzen 9 9950X host,
Windows build 26100) that carried 12–25 % other load.

### Preset, rate control and threads (1080p, 4 vCPU)

Each variant changes one parameter of the baseline current at the time
(ultrafast, CRF 14 at 20 Mbps / 16 at 8 Mbps, 0.5 s buffer, 4 threads).

| 1080p, 4 vCPU, 8 / 20 Mbps | Scroll ink | Map ink | Typing ink | Page end ink | Page ≥35 dB delivered ms | Scroll CPU ms/frame | Scroll encode p50 ms | Largest frame KB |
|---|---|---|---|---|---|---|---|---|
| MFT | 38.4 / 38.5 | 43.8 / 43.8 | 37.7 / 37.8 | 38.1 / 38.1 | 768 / 317 | 9.54 / 10.13 | 3.18 / 3.38 | 758 / 758 |
| x264 ultrafast, CRF 14 (16 at 8 Mbps), 0.5 s | 41.0 / 44.0 | 49.9 / 51.6 | 39.1 / 46.7 | 45.7 / 46.6 | 882 / 324 | 5.63 / 5.49 | 1.91 / 1.75 | 302 / 683 |
| preset superfast (baseline) | 37.5 / 39.5 | 47.4 / 50.0 | 37.7 / 41.4 | 39.2 / 41.6 | 876 / 296 | 6.37 / 6.57 | 2.18 / 2.20 | 282 / 594 |
| preset superfast, profile high (CABAC, 8x8) | 37.5 / 39.3 | 47.3 / 49.9 | 38.8 / 41.4 | 39.2 / 41.7 | 740 / 307 | 7.82 / 7.21 | 2.83 / 2.68 | 282 / 550 |
| CRF 12 | 42.3 / 46.0 | 53.9 / 54.3 | 40.1 / 47.6 | 46.3 / 49.4 | 904 / 331 | 5.56 / 5.99 | 1.89 / 1.99 | 410 / 683 |
| CRF 16 | 41.1 / 42.2 | 50.0 / 50.5 | 39.4 / 44.1 | 45.7 / 45.7 | 869 / 325 | 5.43 / 5.49 | 1.87 / 1.76 | 298 / 683 |
| CRF 18 | 39.5 / 39.8 | 48.4 / 48.8 | 38.3 / 42.2 | 43.4 / 43.4 | 868 / 337 | 6.21 / 5.34 | 2.12 / 1.81 | 281 / 683 |
| buffer 0.25 s | 40.5 / 44.0 | 49.8 / 51.5 | 35.3 / 46.6 | 45.7 / 46.6 | 905 / 369 | 5.94 / 5.80 | 2.05 / 1.92 | 218 / 378 |
| buffer 1 s | 42.0 / 43.9 | 50.3 / 51.8 | 43.3 / 46.4 | 45.7 / 46.7 | 838 / 318 | 5.62 / 5.51 | 1.86 / 1.83 | 576 / 898 |
| 2 threads | 41.2 / 44.0 | 49.7 / 51.3 | 39.0 / 46.6 | 45.7 / 46.7 | 895 / 367 | 5.10 / 5.56 | 2.85 / 2.96 | 342 / 681 |
| 3 threads | 41.0 / 44.0 | 49.9 / 51.5 | 39.2 / 46.7 | 45.7 / 46.7 | 906 / 353 | 5.74 / 5.29 | 2.37 / 2.13 | 310 / 681 |

- **ultrafast over superfast**: superfast (with or without High/CABAC) was
  sharper in none of the scenes at the same CRF and cost 15–40 % more CPU; its
  AQ/psy decisions spend fewer bits on text. ultrafast also keeps the MFT's own
  profile (Constrained Baseline, CAVLC), the safest choice for every client
  decoder.
- **CRF 12** gave +1.3 to +4 dB ink over CRF 14/16 for 3–30 % more bits and no
  CPU change; 16 and 18 were visibly softer. CRF only caps quality: VBV keeps
  every frame at the target. The final rule is CRF 12, lowered to 10 when the
  target exceeds 0.16 bit per pixel-frame (2 per doubling), so 40 Mbps at 1080p
  uses CRF 10.
- **0.5 s VBV buffer**: page-switch data at 8 / 20 Mbps (1080p, CRF 12,
  unpaced):

  | Buffer | First changed frame | delivered ms | ≥35 dB delivered ms | Page-end ink | Start IDR ink | Link delay p95 ms |
  |---|---|---|---|---|---|---|
  | MFT | 37.8 dB, 753 KB | 763 / 310 | 768 / 312 | 38.1 | 29.2 | 644 / 216 |
  | 0.5 s | 11.4 / 14.7 dB, 144 / 292 KB | 147 / 120 | 917 / 330 | 46.1 / 49.4 | 24.9 / 35.1 | 338 / 255 |
  | 0.75 s | 12.6 / 19.3 dB | 192 / 150 | 886 / 330 | 46.7 / 49.4 | 27.3 / 39.2 | 550 / 316 |
  | 1 s | 12.6 / 22.3 dB | 210 / 178 | 873 / 343 | 47.3 / 49.5 | 31.7 / 40.9 | 667 / 326 |
  | 1.5 s | 15.0 / 27.0 dB | 266 / 204 | 817 / 279 | 48.8 / 49.5 | 37.2 / 42.7 | 810 / 323 |

  x264's VBV lets one frame take at most half the buffer, so a page switch
  arrives as a quick low-quality frame that the refinement burst sharpens
  ("build to sharp"). Bigger buffers make that first frame and the
  stream-start IDR sharper but double the worst-case queueing behind a burst.
  0.5 s keeps the worst burst well below the MFT's (its page-change P frame is
  ~0.75 MB at any bitrate) and reaches 35 dB about as fast as the MFT at
  20 Mbps; it is the responsiveness choice. 0.25 s cut typing quality at
  8 Mbps (35.3 dB).
- **Threads**: one slice thread per vCPU gave the lowest encode wall time at the
  same CPU (1080p, 20 Mbps: p50 1.75 ms with 4, 2.13 with 3, 2.96 with 2).

### Production settings versus the MFT (4 vCPU affinity)

Quality (ink PSNR dB, steady second half of each 3 s run) and media rate,
MFT → x264:

| Size | Target | Scroll ink, moving | Map ink, moving | Typing ink, static | Scroll Mbps | Map Mbps |
|---|---|---|---|---|---|---|
| 1264×760 | 8 Mbps | 38.5 → 46.0 | 43.9 → 54.7 | 37.5 → 47.4 | 5.3 → 6.7 | 3.4 → 2.1 |
| 1264×760 | 20 Mbps | 38.5 → 47.3 | 44.0 → 56.4 | 37.4 → 51.0 | 5.4 → 7.1 | 3.4 → 2.4 |
| 1264×760 | 40 Mbps | 38.5 → 47.3 | 44.0 → 56.7 | 37.8 → 50.9 | 5.4 → 6.9 | 3.4 → 2.1 |
| 1264×760 | auto | 38.5 → 47.2 | 44.0 → 56.2 | 37.4 → 51.0 | 5.4 → 7.0 | 3.4 → 2.2 |
| 1920×1080 | 8 Mbps | 38.4 → 42.4 | 43.8 → 53.9 | 37.7 → 40.2 | 7.9 → 8.6 | 6.2 → 4.0 |
| 1920×1080 | 20 Mbps | 38.5 → 46.1 | 43.8 → 54.3 | 37.8 → 47.6 | 8.7 → 10.1 | 6.4 → 3.7 |
| 1920×1080 | 40 Mbps | 38.5 → 47.4 | 43.9 → 56.4 | 37.4 → 51.0 | 8.9 → 10.3 | 6.3 → 3.6 |
| 1920×1080 | auto | 38.5 → 46.7 | 43.8 → 54.7 | 37.7 → 49.3 | 8.7 → 10.5 | 6.4 → 3.9 |
| 2560×1440 | 8 Mbps | 38.2 → 40.8 | 44.1 → 53.7 | **37.1 → 34.0** | 8.5 → 8.7 | 8.5 → 6.1 |
| 2560×1440 | 20 Mbps | 38.3 → 47.0 | 44.0 → 54.4 | 37.5 → 47.3 | 9.7 → 10.8 | 9.9 → 5.7 |
| 2560×1440 | 40 Mbps | 38.4 → 47.3 | 44.0 → 54.7 | 37.5 → 49.5 | 10.1 → 10.7 | 10.1 → 5.4 |
| 2560×1440 | auto | 38.3 → 47.0 | 44.0 → 54.4 | 37.5 → 47.1 | 9.7 → 11.0 | 9.9 → 5.7 |

Encoder CPU (all threads) per submitted frame and encode call wall time,
MFT → x264:

| Size | Target | Scroll CPU ms/frame | Scroll encode p50/p95 ms | Map CPU ms/frame | Typing CPU ms/submit | Encoder cores, scroll at 60 fps |
|---|---|---|---|---|---|---|
| 1264×760 | 8 Mbps | 5.44 → 3.34 | 1.84/2.36 → 1.19/1.57 | 5.15 → 2.91 | 5.69 → 3.24 | 0.34 → 0.21 |
| 1264×760 | 20 Mbps | 5.27 → 3.12 | 1.79/2.40 → 1.06/1.53 | 6.15 → 3.17 | 5.97 → 2.93 | 0.32 → 0.19 |
| 1264×760 | 40 Mbps | 5.09 → 3.01 | 1.75/2.59 → 1.05/1.44 | 5.85 → 3.24 | 5.52 → 2.73 | 0.31 → 0.19 |
| 1264×760 | auto | 5.16 → 3.14 | 1.73/2.51 → 1.10/1.47 | 5.34 → 3.15 | 6.35 → 3.01 | 0.32 → 0.20 |
| 1920×1080 | 8 Mbps | 9.36 → 5.44 | 3.23/4.19 → 1.90/2.87 | 9.40 → 5.35 | 9.63 → 4.58 | 0.57 → 0.34 |
| 1920×1080 | 20 Mbps | 10.54 → 5.63 | 3.51/4.72 → 1.88/2.36 | 9.38 → 5.59 | 9.75 → 5.49 | 0.65 → 0.35 |
| 1920×1080 | 40 Mbps | 9.55 → 5.66 | 3.27/4.06 → 1.79/2.64 | 9.57 → 4.79 | 9.61 → 5.07 | 0.59 → 0.35 |
| 1920×1080 | auto | 9.65 → 5.65 | 3.26/4.17 → 1.85/2.66 | 9.60 → 5.04 | 9.91 → 5.21 | 0.59 → 0.35 |
| 2560×1440 | 8 Mbps | 15.27 → 7.86 | 5.09/6.14 → 2.66/3.45 | 15.72 → 8.54 | 16.26 → 6.90 | 0.94 → 0.49 |
| 2560×1440 | 20 Mbps | 16.10 → 9.08 | 5.13/6.84 → 2.95/4.41 | 15.57 → 8.91 | 15.71 → 8.24 | 0.99 → 0.57 |
| 2560×1440 | 40 Mbps | 16.93 → 8.93 | 5.37/7.31 → 2.84/3.87 | 15.20 → 8.53 | 16.71 → 8.45 | 1.04 → 0.56 |
| 2560×1440 | auto | 16.41 → 8.84 | 5.28/6.75 → 2.95/3.99 | 15.77 → 8.84 | 14.84 → 7.73 | 1.01 → 0.55 |

Page switches (production policy, 2 s pages), MFT → x264:

| Size | Target | First visible change, ms | its ink dB | ≥35 dB ink delivered, ms | Ink at page end | Stream-start IDR ink | Largest frame KB |
|---|---|---|---|---|---|---|---|
| 1264×760 | 8 Mbps | 392 → 155 | 37.8 → 14.8 | 396 → 443 | 38.1 → 49.5 | 29.2 → 31.0 | 390 → 279 |
| 1264×760 | 20 Mbps | 160 → 110 | 37.8 → 25.8 | 161 → 161 | 38.1 → 52.4 | 29.2 → 32.8 | 390 → 318 |
| 1264×760 | 40 Mbps | 82 → 72 | 37.8 → 35.8 | 83 → 73 | 38.1 → 52.3 | 29.2 → 42.7 | 390 → 481 |
| 1264×760 | auto | 126 → 54 | 37.8 → 15.8 | 160 → 175 | 38.1 → 52.7 | 29.2 → 33.5 | 390 → 327 |
| 1920×1080 | 8 Mbps | 763 → 149 | 37.8 → 11.9 | **767 → 904** | 38.1 → 46.1 | **29.2 → 25.2** | 758 → 395 |
| 1920×1080 | 20 Mbps | 311 → 119 | 37.8 → 14.7 | 313 → 330 | 38.1 → 49.4 | 29.2 → 35.1 | 758 → 656 |
| 1920×1080 | 40 Mbps | 162 → 100 | 37.7 → 22.9 | 163 → 159 | 38.1 → 52.4 | 29.2 → 40.9 | 757 → 863 |
| 1920×1080 | auto | 243 → 63 | 37.8 → 11.6 | 308 → 334 | 38.1 → 51.8 | 29.2 → 35.1 | 758 → 656 |
| 2560×1440 | 8 Mbps | 1,064 → 66 | 37.8 → 9.2 | **1,070 → 1,397** | 38.1 → 38.9 | **29.2 → 23.1** | 1,055 → 430 |
| 2560×1440 | 20 Mbps | 435 → 148 | 37.8 → 16.7 | 436 → 452 | 38.1 → 47.6 | 29.2 → 29.5 | 1,056 → 693 |
| 2560×1440 | 40 Mbps | 224 → 110 | 37.8 → 20.4 | 225 → 218 | 38.1 → 49.5 | 29.2 → 38.0 | 1,055 → 1,045 |
| 2560×1440 | auto | 341 → 86 | 37.8 → 14.0 | 432 → 468 | 38.1 → 48.9 | 29.2 → 29.8 | 1,056 → 698 |

2 vCPU affinity and the SSE-only code paths (`x86-64-v2`: no AVX/AVX2/AVX-512/
BMI/FMA, what a VM with Proxmox's default CPU type exposes; the MFT could not be
restricted the same way and kept AVX-512), 20 Mbps:

| Size, scene | 2 vCPU: CPU ms/frame MFT / x264 / x264 SSE-only | 2 vCPU: encode p50/p95 ms MFT / x264 / x264 SSE-only | 4 vCPU: CPU ms/frame x264 / x264 SSE-only |
|---|---|---|---|
| 1080p scroll | 8.75 / 5.72 / 6.32 | 4.69/6.69 / 3.26/4.88 / 3.46/4.98 | 5.63 / 6.88 |
| 1080p map | 8.99 / 5.12 / 5.72 | 4.81/6.64 / 2.77/3.64 / 3.13/4.26 | 5.59 / 6.16 |
| 1080p typing | 8.49 / 5.17 / 5.44 | 4.14/7.22 / 2.74/3.75 / 2.86/4.04 | 5.49 / 6.03 |
| 1080p page | 9.38 / 5.46 / 5.93 | 4.26/15.27 / 2.96/5.22 / 3.18/6.20 | 5.48 / 6.00 |
| 1440p scroll | 14.37 / 8.04 / 9.35 | 7.70/9.79 / 4.47/5.89 / 5.23/6.42 | 9.08 / 9.75 |
| 1440p map | 14.04 / 7.92 / 9.46 | 7.32/9.33 / 4.20/5.79 / 4.98/6.91 | 8.91 / 10.96 |
| 1440p typing | 14.80 / 7.95 / 8.54 | 7.25/12.63 / 4.43/5.74 / 4.77/7.53 | 8.24 / 9.18 |
| 1440p page | 12.95 / 8.09 / 8.85 | 6.47/11.16 / 4.31/6.53 / 4.83/7.81 | 8.15 / 9.28 |

Quality was identical with the SSE-only paths (same bitstream decisions within
0.1 dB).

### Summary, including where x264 is worse

- CPU: x264 needs 35–58 % less encoder CPU per frame than the MFT at every
  size, target and scene measured, on 4 and 2 vCPU; the SSE-only paths stay
  28–40 % below the MFT. 1440p60 scrolling drops from ~1.0 core to ~0.55.
  Encode call wall time p95 drops by a third to a half.
- Steady quality: +2.5 to +13 dB ink on scrolling, map panning and typing (one
  exception below), and pages settle at 46–52 dB instead of the MFT's ~38 dB
  plateau. Media rate is lower for map panning and higher for scrolling
  (+2–30 %) and typing (up to 2×), always within the VBV cap.
- Page switches: the first changed frame reaches the client 1.1–16× sooner
  (e.g. 1080p at 20 Mbps: 119 vs 311 ms) but is blurry (9–36 dB) and sharpens
  over the refinement burst; reaching 35 dB takes within +6 % of the MFT at
  20–40 Mbps, +8–9 % in the scripted auto schedule, and **longer at 8 Mbps**
  (1080p +18 %, 1440p +31 %, 1264×760 +12 %).
- **Worse at low bits per pixel**: the stream-start/resize IDR at 8 Mbps is
  softer (1080p 25.2 vs 29.2 dB, 1440p 23.1 vs 29.2), and 1440p typing at
  8 Mbps ends lower (34.0 vs 37.1 dB) because continuous small changes leave no
  quiet period for refinement after that IDR; only the 1 s refresh sharpens it.
  At ≥ 20 Mbps the IDR is sharper than the MFT's but larger (1080p up to 683 vs
  429 KB), which adds up to ~0.1 s on a link running exactly at the target.
- **More bandwidth per page switch at low targets**: at ≤ 12 Mbps a full 1080p
  text page keeps the refinement burst running to the policy's bound (100 ms
  quiet + 90 frames), because every refinement frame still adds detail at
  CRF 12 (it ends at ~54 dB, the MFT plateaus at ~44 dB): 2.4 MB versus the
  MFT's 1.0 MB over the 2 s after a switch at 12 Mbps
  (`MultitapHostFrameRefinementTest`), within the target rate. At 20 Mbps the
  burst settles after ~0.7 s.
- VBV bound: the 4K worst cases in `MultitapHostX264EncoderTest` stay at
  1.12 MB (20 Mbps) and 2.69 MB (100 Mbps) per frame, including incompressible
  noise (1.1 MB), where the MFT codes a 4K page change as ~4.9 MB (above the
  4 MiB packet bound, [adaptive-bitrate.md](adaptive-bitrate.md)).

## CPU per frame

`encodedFps` counts submitted pictures: `FrameEncodePolicy` submits only changed
pictures plus refresh and refinement frames, and a WGC frame whose NV12 equals
the last one is captured but not encoded. A capture rate above the encode rate
therefore does not by itself show an encoder limit; the per-stage times in the
client diagnostics (`captureUs`, `convertUs`, `encodeUs`, `hostWorkUs`) decide
it.

### The pipeline per frame (one capture thread)

WGC frame → `CopyResource` into a staging texture and `Map` (waits for the
copy) → conversion straight from the mapped staging memory into the NV12 frame
(no BGRA copy) → `FrameEncodePolicy` byte compare against its snapshot (and a
copy of the changed tail) → x264: copies the NV12 into its padded frame, builds
a half-resolution copy and the whole-frame SATD cost that CRF/VBV rate control
needs, then analyses and codes the macroblocks on its slice threads while the
calling thread waits. The next frame's capture and conversion do not overlap
this encode. Overlapping them would shorten the loop's wall time by up to the
conversion + readback time (about 1 ms at 1080p) but save no CPU; it matters
only where the loop is wall-bound (e.g. 5120×1440 under load).

### x264 parameters reviewed

Nothing left in the parameters saves CPU without breaking the contract.
ultrafast already sets 1 reference, no deblocking, CAVLC, no B-frames, I16x16
and P16x16 only, DIA motion search, subme 0, no 8x8 transform, AQ off, no
trellis, no weighted prediction, no mb-tree, no lookahead and scenecut 0 (psy-rd
needs subme ≥ 6 and psy-trellis needs trellis, so psy does nothing here);
PSNR/SSIM are off by default. zerolatency gives sliced threads; frame threads
would add a frame of delay each. CRF and ABR both make x264 build the
half-resolution picture and its cost on every frame (`b_have_lowres`); CQP would
skip that but has no VBV, which the 4 MiB packet bound and the bitrate contract
need. `lookahead-threads=1` (instead of one per slice thread) and 4 instead of 8
slice threads on a 10-vCPU VM gave no consistent CPU difference at 1080p
(within the run-to-run spread), so the thread policy is unchanged.

### Changes this led to

- **Unchanged macroblocks** (x264 `mb_info`, `X264_MBINFO_CONSTANT`): the
  encoder keeps its previous input and flags each 16×16 macroblock whose NV12
  pixels did not change (`x264_constant_blocks.hpp`; only changed 16-byte runs
  are copied). A P frame then codes such a macroblock as a skip without
  analysing it while the reference macroblock was coded at a QP no worse than
  this frame's, and analyses it as before otherwise, so refinement still
  sharpens an unchanged picture (x264 `analyse.c`). The first picture and IDRs
  pass no flags. One more NV12 picture per encoder (13 instead of 12 in the
  memory admission).
- **AVX2 conversion**: the BGRA → NV12 kernel
  (`native/windows_video/bgra_nv12_sse2.hpp`) runs the SSE2 step twice side by
  side when CPUID reports AVX2 and Windows saves the YMM state; the bytes are
  identical (checked against the scalar reference for all 24-bit colours). A
  Proxmox `x86-64-v2` CPU type has no AVX2 and keeps SSE2.

### Measured (10-vCPU test VM, Ryzen 9 9950X host, IDLE priority)

`MultitapHostEncoderBenchmark` (production `FrameEncodePolicy`, 60 Hz pacing,
20 Mbps, every output decoded by the client's `RemoteDecoder`). "Before" is
the same binary with `enc=x264/constant=0/kernel=sse2/threads=8`, i.e. the
previous behaviour on 10 vCPU; "after" is `enc=x264`. CPU = process TSC ticks
during the encode call (all x264 threads) per submitted frame, plus conversion
and policy; two interleaved runs each. Other processes used 93–99 % of the
machine during these runs, so encode wall times are dominated by preemption and
only CPU per frame is compared.

| 1920×1080 | encoder CPU ms/frame before → after | pipeline CPU ms/frame before → after | frames per CPU-second | ink dB before / after |
|---|---|---|---|---|
| typing | 6.69, 6.76 → 5.39, 4.89 | 7.79, 7.80 → 6.39, 5.88 | 128 → 163 | 48.0/47.5 → 47.9/47.6 |
| page switch | 6.35, 6.71 → 4.76, 4.98 (and 178 → 160 submissions) | 7.79, 7.84 → 5.66, 5.90 | 128 → 173 | page end 49.4 → 49.4 |
| scroll (all blocks change) | 6.24, 6.48 → 6.52, 7.22 | 7.98, 7.70 → 7.45, 8.82 | ~same (spread ±10 %) | 46.0/45.5 → 46.0/45.5 |
| video (motion, all blocks change) | 9.64, 10.49 → 9.66, 10.26 | 11.70, 12.13 → 10.48, 11.55 | 84 → 91 | 37.1 → 37.1 |

One run each (same method):

| Size, scene | encoder CPU ms/frame before → after | pipeline CPU ms/frame before → after | submissions | ink dB before / after |
|---|---|---|---|---|
| 2560×1440 typing | 9.73 → 8.50 | 12.33 → 10.23 | 40 / 40 | 45.9/45.7 → 45.8/46.0 |
| 2560×1440 page switch | 9.95 → 7.34 | 12.44 → 10.23 | 227 / 207 | page end 47.6 → 47.6 |
| 2560×1440 scroll | 10.82 → 11.14 | 13.00 → 13.37 | 120 / 120 | 46.2/45.4 → 46.3/45.2 |
| 2560×1440 video | 14.90 → 15.07 | 17.85 → 16.97 | 120 / 120 | 35.8 → 35.8 |
| 5120×1440 typing | 15.36 → 13.52 | 19.72 → 16.73 | 40 / 40 | 45.4/45.6 → 46.2/46.2 |
| 5120×1440 page switch | 14.58 → 13.34 | 18.27 → 16.94 | 229 / 208 | page end 47.8 → 47.8 |
| 5120×1440 scroll | 17.08 → 16.98 | 20.48 → 20.26 | 120 / 120 | 47.5/46.1 → 47.5/46.3 |
| 5120×1440 video | 24.30 → 26.06 | 27.98 → 29.49 | 120 / 120 | 34.4 → 34.4 |

Where every macroblock changes (scroll, video) the hint cannot skip anything,
and its compare/copy (one more pass over the NV12 picture, about 0.2 ms at
1080p) costs up to a few per cent, inside the run-to-run spread; the conversion
saving offsets part of it. Static and mostly static desktops (typing, page
switches and their refinement) gain 10–30 %.

Conversion alone (`tools/pixel-conversion-qa`, alternating SSE2/AVX2 rounds,
thread CPU per frame): 1280×720 0.45 → 0.29 ms, 1920×1080 0.88 → 0.63,
2560×1440 1.90 → 1.25, 3840×2160 3.32 → 2.60, 5120×1440 3.22 → 2.58 (−20 to
−38 %); both kernels byte-exact against the scalar reference, including guard
pages.

Per slice-thread count, 1920×1080, unpaced, one interleaved run (other
processes 85–99 % of the machine), frames per CPU-second of the pipeline
(conversion + policy + encode) before → after, encode call p50 ms in brackets:

| threads | typing | scroll | video |
|---|---|---|---|
| 1 | 118 → 170 (7.7 → 4.6) | 117 → 112 (8.6 → 9.8) | 91 → 100 (17.9 → 11.2) |
| 2 | 113 → 156 (6.9 → 3.5) | 102 → 125 (6.9 → 4.9) | 96 → 96 (7.6 → 6.6) |
| 4 | 115 → 140 (3.7 → 6.7) | 131 → 121 (3.4 → 3.9) | 98 → 90 (4.8 → 4.5) |
| 8 (auto here) | 104 → 162 (3.7 → 3.4) | 115 → 128 (3.1 → 3.0) | 91 → 90 (3.7 → 3.6) |

A thread sweep of the previous code (unpaced, 120 frames) gave 1–3 slice
threads the lowest CPU per frame at 1080p (6–8 threads up to +25 % on scroll)
and no clear order at 2560×1440 and 5120×1440; paced A/Bs of 4 against 8
threads (1080p) and 6 against 8 (1440p, 5120×1440) were inside the spread, so
the automatic choice (one per processor, at most 8) stays.

These figures come from synthetic scenes on a shared machine. A guest's real
vCPU scheduling, SMT siblings, frequency and Proxmox CPU type differ; the
SSE-only runs approximate the CPU type, not the host.

## Build, licence and package

- CMake: `-DMULTITAP_X264_SRC=<checkout> -DMULTITAP_NASM=<nasm.exe>` (both
  required; without them the Host is MFT-only).
  `apps/windows_host/third_party/x264/x264.cmake` refuses anything but commit
  `b35605ace3ddf7c1a5d67a2eb553f034aef41d55` (x264 0.165.3222, API build 165)
  with no modified, untracked or ignored files. Setup steps:
  [apps/windows_host/third_party/x264/README.md](../apps/windows_host/third_party/x264/README.md).
- `tools/host-installer/build-native.ps1` takes them from `.tools/x264` and
  `.tools/nasm-2.16.03/nasm.exe` (NASM 2.16.03 win64) and passes both paths to
  CMake. The x264
  tree is not vendored: it is ~10 MB of upstream GPL source that the build
  only reads, and a pinned, verified checkout keeps the repository and the
  provenance clean.
- Package `bin/`: `THIRD-PARTY-NOTICES.txt` (version, commit, upstream,
  copyright, licence, how it is linked), `COPYING.x264.txt` (GPL v2 text from
  the checkout) and `host-native-corresponding-source-<version>.zip`, which
  holds the pinned x264 source beside the Host's own build source.
- Licence: `MultitapHost.exe` and `MultitapHostTool.exe` are Multitap's
  AGPL-3.0-only code linked with x264, used under GPL version 3 as its
  "version 2 or later" licence allows; section 13 of both licences permits
  the combination. Whoever distributes a Host package must offer
  the corresponding source of the Multitap Host as well as x264's; the package
  carries x264's source, and this repository is the Host's. See
  [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Tests

- `MultitapHostX264EncoderTest` (a target of the Host's CMake project when built
  with x264): colour-check sensitivity, the contract for the MFT reference, x264
  production, superfast High/CABAC and the SSE-only paths on the real decoder,
  IDR placement versus the MFT, a live 20→8→40→12 Mbps change (no IDR, every
  frame decoded, each phase within maxrate × time + buffer), resize epochs at
  1920×1080/1264×760/2560×1440 (a fresh encoder starts with a decodable IDR),
  the 4K VBV bound at 20 and 100 Mbps, automatic fallback (open failure;
  mid-stream failure at frame 30 → IDR at the same PTS, all 60 frames decoded,
  observer called once with the code), strict x264 not swallowing failures, MFT
  tunings never selecting x264, the `encoder.json` rule, and the
  unchanged-macroblock flags against a per-pixel comparison (partial edge
  macroblocks included; typing, a page switch refined at 8 Mbps and a requested
  IDR give the same IDR placement and quality within 0.5 dB with and without
  them).
- `MultitapHostFrameRefinementTest` runs the production backend and the MFT. Its
  refinement bounds are per phase (≤ 90 per armed change, none during
  cursor-sized changes or motion) and its quiescence window starts at the
  policy's bound (quiet + 90 frames), because x264 legitimately uses the whole
  bound at 12 Mbps.
- `MultitapHostCapturePatchTest` runs its conversion checks with the AVX2 kernel
  (where the CPU has it) and with the SSE2 kernel.
- `MultitapHostEncoderBitrateTest` and `MultitapCodecBenchmark` pin the MFT (they
  measure the fallback encoder). The resize, lossless, tracks, stream-metrics
  and client-engine tests use the default backend, i.e. x264.

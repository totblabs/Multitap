# Media path QA

Times the Windows client's share of `inputFrame` (`input-latency-v1`) on the
machine it runs on. A real Flutter Windows view replays the product
`RemoteSession` receive loop: a WebSocket `await for`, one awaited platform
channel `push` per packet (video watched like a tagged packet) into the actual
`RemoteEngine` (Microsoft CPU H.264 decoder, pixel-buffer texture). The source
is a local WebSocket in another isolate sending synthetic 1580×952 H.264 (the
texture-path QA fixture) at `--fps` and 1932-byte audio packets at `--audio`
per second. Every step is timed on the process-wide monotonic clock
(`Timeline.now`):

| Field | Meaning |
|---|---|
| `sendToArrivalMs` | source write → the receive loop reached the packet (loopback) |
| `arrivalToPushedMs` | the loop's awaited push returned (product `inputPush`) |
| `queuedToPublishedMs` | native decode queue → frame handed to the texture (product `inputDecode`) |
| `arrivalToPublishedMs` | the two together: the client's share of `inputFrame` |
| `textureWaitMs` | published → taken by Flutter's raster thread (not part of `inputFrame`) |

`--burn=N` runs N normal-priority busy threads in a child process (a PC busy
with builds), `--boost=1` raises the harness process to ABOVE_NORMAL,
`--awaitAudio=0` pushes audio without awaiting it. No broker, relay, VM or
network; plain `ws` on loopback (the product uses TLS).

```powershell
$exe = tools/media-path-qa/build.ps1 -OutDir C:\mq
tools/media-path-qa/run.ps1 -Exe $exe -Out C:\mq\run.jsonl
```

`build.ps1` creates a Windows app with `flutter create`, adds this folder's
sources, links the working tree's client engine and prints the path of
`media_qa.exe` (keep `-OutDir` short). `run.ps1` runs each case
(`-Cases 'label|harness arguments'`, `-Seconds`, `-Repeat`) in its own process
and appends one JSON line per run.

Example, a GPU-less test VM (10 vCPU) viewed through a remote desktop session
(so Flutter frames at about 32 Hz), 18 fps video, 15 s per case:

| Case | arrival → pushed p50/p95 | queued → published p50/p95 | arrival → published p50/p95 | texture wait mean |
|---|---|---|---|---|
| idle, video only | 0.15 / 0.32 ms | 3.32 / 4.57 ms | 3.49 / 4.75 ms | 15.7 ms |
| + 100 audio packets/s | 0.13 / 0.22 | 3.02 / 4.34 | 3.17 / 4.50 | 16.5 |
| + 10 busy threads | 0.13 / 0.23 | 3.01 / 3.71 | 3.15 / 3.88 | 16.8 |
| + 20 busy threads | 0.11 / 0.30 | 2.96 / 3.50 | 3.09 / 3.68 | 16.2 |
| + 20 busy threads, ABOVE_NORMAL | 0.11 / 0.47 | 3.15 / 5.45 | 3.28 / 5.63 | 20.0 |

The client receive → texture path took about 3–5 ms even with every CPU busy,
and raising the process priority did not help.

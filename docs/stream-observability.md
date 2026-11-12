# Stream observability v1 (`stream-observability-v1`)

Optional, passive queue and timing snapshots for diagnosing a streaming
session. Off by default; when it is off or not negotiated, message shapes, rate
control and input behaviour are unchanged.

## Opt-in and compatibility

Host capabilities, the connect request/response and `session.start` use
`observabilityProtocol: "stream-observability-v1"`. The client requests it only
when the user enabled it AND the broker's desktop list advertises Host support.
The broker grants it only to authenticated streaming sessions whose Host has
the capability. Independent tracks may negotiate this snapshot protocol
separately from `input-latency-v1` (which stays single-output only).

The **자세한 진단** control in the statistics dialog (연결 정보) enables
client-local observation. Windows stores the choice in
`%LOCALAPPDATA%\Multitap\observability.json`, apart from server and sign-in
settings; each new detached session window reads it. A missing or invalid
file means OFF. Android keeps the choice for the current app run. Host and
client-inbox observation are selected when a connection starts: turning it OFF
stops new local collection and inbox timestamps, and a reconnect retires
negotiated Host observation. Turning it ON in an existing unobserved connection
does not invent inbox ages or retrofit Host negotiation; the UI and the export
say so.

## Optional snapshot

The optional `observability` field on `stream.stats` / `stream.tracks.stats`:

```json
{"protocol":"stream-observability-v1","metrics":{"hostQueuePackets":0,"hostQueueBytes":0,"hostQueueOldestUs":0}}
```

Allowed integer metrics (unknown ones are omitted, never reported as zero):

- `hostQueuePackets` 0..4096, `hostQueueBytes` 0..16777216.
- `hostQueueOldestUs`, `hostWriteUs`, `readbackUs`, `readbackMaxUs`
  0..60000000. Queue age covers waiting packets, not an in-flight write.
- `serviceCpuPct10`, `workerCpuPct10` 0..1000, normalised to whole-machine CPU.
- `relayQueueBytes` 0..16777216, `relayForwardUs` 0..60000000.
- `relayVideoQueuePackets` 0..4096, `relayVideoQueueOldestUs` 0..60000000.

`hostWriteUs` is the latest completed binary media write, not a new write every
second. `readbackUs` is the capture-tick window mean around the existing
CopyResource+Map, including zero on Windows Graphics Capture ticks without a new
frame; `readbackMaxUs` is that window's maximum. Track readback is pooled across
capture ticks of both outputs. GDI capture omits it. First CPU samples and
unavailable samplers are omitted. Host queue observations include waiting media
and control and exclude the write already in flight.

The relay video queue age and count track outstanding local write callbacks, not
peer buffers or network delivery. Tracking is bounded; an overflow reads as
unknown. The latest relay forwarding observation expires after three seconds or
an epoch change.

No other Host fields are accepted. Relay additions share the metrics map; they
are observed by the broker, never trusted from the Host. The client attaches
snapshots to their authenticated session/output/epoch context and discards
stale ones. Snapshots may be absent or partial with older peers and failed
samplers.

The connect grant may include categorical `observabilityPath` and
`observabilityVersions`. Path labels (`direct`, `proxy`, `plain`, `unknown`)
describe the HTTP connect request (`clientTls`) and the authenticated Host
control WebSocket (`hostTls`). They do not prove identical routing of every
media URL and do not identify a particular proxy. Versions are reported
versions, not binary integrity.

Client-local queue, input-gate and native metrics are combined only in the
local bounded report (`apps/windows_client/lib/data/stream_observability.dart`);
extending the diagnostic numeric allowlists is separate from negotiating Host
observations. Stream-statistics report batching is unchanged.

The client snapshot includes known per-output Host and decoder observations,
with their output ID and epoch; shared queues are not divided or attributed to
one output. Displayed aggregate decode FPS is the sum across outputs. Android
reports its hardware-decoder Boolean where the Windows-style decode tier is not
available.

## Measurement boundaries

Durations are monotonic within each machine only. Application RTT is not TCP
RTT. A WebSocket send completion is local socket acceptance, not peer receipt.
GPU readback timing includes CPU waiting at Map; there is no forced GPU flush
just to measure. Texture consumption is a presentation proxy, not physical
scan-out. Per-stage percentiles cannot be added up. Unsupported values are
exported as null with a reason. Local cursor response and remote content
response are distinct.

The retained report holds at most 600 snapshots. Its summary uses streaming
snapshots only and names them **snapshotP50/snapshotP95**; these are not
per-frame quantiles. Input distributions keep the latency meter's rolling
percentiles and sample counts separately. Connecting, resizing and closed
snapshots stay in the timeline. The last validated encoder facts keep their
observation time and epoch after a disconnect. `cursorNegotiatedMode` names the
grant; it does not prove that every cursor shape avoided the Host's
composition fallback.

## Collect a comparison

1. Open 연결 정보 and enable 자세한 진단. Reconnect for Host and inbox
   observation.
2. Record the network scenario, broker origin, versions, owned output size,
   target FPS/bitrate, audio and monitor count. Change one variable at a time.
3. For each run: 30 s idle; 30 s moving or dragging a window; 30 s scrolling
   text or a spreadsheet; 30 s map pan/zoom. Repeat three times.
4. Use **JSON 복사** before closing the remote window. A closed session's data
   can be exported while that session object still exists; reports are not
   kept across an application exit.
5. Collect OFF/ON results for observer overhead, then baseline versus
   candidate, on comparable networks (for example a LAN, a VPN and a mobile
   connection).

A scenario name identifies the access network, not the media route: a LAN
client using the public URL can still pass through the public proxy, and a VPN
may itself be direct or relayed. Record what you observe rather than infer it.

Suggested result columns: run ID, scenario, loss and direction, versions,
output geometry/FPS, backend and fallback, Host CPU, app RTT, input injection
and frame-proxy p50/p95, each queue age and depth, decoded and consumed FPS,
visible freezes, reconnection time, missing metrics with reasons. Physical
input-to-photon latency needs a separate visible experiment (for example
high-frame-rate footage); the report alone cannot establish it.

## Loss experiments (dedicated test path)

Do not impair the connection used to administer a working VM: pick one test
connection and keep an independent recovery console available. Test 0 %, 1 %,
3 % and 5 % loss in separate runs, one direction at a time. Record tool and
version, the affected leg and its counters; TCP recovery and application drops
are different events. A session on the UDP media path (`udp-media-v1`) sends
its media as datagrams to the broker's UDP port; a session on WSS uses the
HTTPS port.

### Windows: clumsy

Use a trusted build whose driver Windows accepts under its normal security
settings; do not enable test signing or import certificates to run it. The tool
needs administrator rights. Filter on the observed destination address, the
service port and the test socket's local port, for example:

```text
outbound and ip.DstAddr == 192.0.2.10 and tcp.DstPort == 443 and tcp.SrcPort == 53001
```

Addresses and ports above are examples. Select Drop and set Chance to 1 %, then
3 %, then 5 % in separate runs. Keep Lag/Duplicate/Throttle off for a loss-only
comparison. Start, record a run, Stop, and confirm the tool stopped capturing
before returning to baseline. Inbound and outbound are independent. Loopback
and filtering limits can distort a local test, and the tool does not reliably
filter by process; never target all TCP traffic.

Source: [clumsy manual](https://jagt.github.io/clumsy/manual.html).

### Linux: tc netem

Use an isolated test VM or network namespace with a dedicated test interface.
Inspect the existing qdisc first and keep it. The example uses `add`, not
`replace`: if another root discipline prevents the addition, stop and build an
isolated test path rather than overwrite a production policy.

```sh
TEST_IF=tvobs-test0             # replace only with a verified dedicated interface
ip link show dev "$TEST_IF"
tc -s qdisc show dev "$TEST_IF"
sudo tc qdisc add dev "$TEST_IF" root handle 7a00: netem loss random 1%
tc -s qdisc show dev "$TEST_IF"
# Record one run; change 1% to 3%/5% only in a fresh, separately labelled run.
sudo tc qdisc del dev "$TEST_IF" root handle 7a00:
tc -s qdisc show dev "$TEST_IF"
```

Check that the root qdisc is the `netem 7a00:` created for this test before
deleting it. This affects that interface's egress only; the other direction
needs a separate reverse-path interface or namespace. Kernel timing and TCP
placement can affect results.

Source: [tc-netem manual](https://man7.org/linux/man-pages/man8/tc-netem.8.html).

## Where the evidence goes

Host summaries use the protected, rotated
`%ProgramData%\Multitap\Host\events.jsonl`. `streamObservation` rows contain the
session ID, context, source (`host-service` or `host-worker`) and the projected
numeric snapshot, never input contents. They come from the asynchronous
diagnostics collector, about every 30 seconds or after a completed observation
window. A shutdown or crash can lose pending rows.

Client diagnostics keep their batching (first report after 10 seconds, then
every 30 seconds) and the server's retention and count limits; older brokers
drop unknown optional fields. Administrators read stored session statistics in
the admin console's diagnostics views. To read the broker's own output, use the
container log (or the systemd journal of a non-Docker install); check the
deployment's actual service or container name first. Never paste credential
files or authentication headers.

For OS-level TCP RTT on a Linux relay, `ss -tinp` gives per-socket
observations; match the actual endpoint tuples and timestamps. Behind a TLS
proxy, the proxy and upstream connections are separate sockets. Redact
connection metadata from shared exports. The JSON report does not include OS
TCP RTT, and app ping RTT keeps its application label.

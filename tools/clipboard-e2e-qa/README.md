# Clipboard end-to-end QA

Runs the real clipboard pieces together on one Windows machine and drives them like a user copying and pasting:

```
Dart ClipboardSession + real NativeClipboardBridge          (apps/windows_client/lib/data)
      |  MethodChannel mocked, forwarded as JSON lines
clipboard_e2e_client.exe  = the real native ClientClipboardChannel (client_clipboard.cpp) on an in-memory clipboard
      |
   WebSocket (loopback)  --->  relay_fixture.mjs = the real broker relay of a server tree
      |
clipboard_e2e_host.exe    = the real Host ClipboardWorker (clipboard_worker.hpp) on an in-memory clipboard,
                            connected like ClipboardChannel (same headers, queue bounds and message limits)
```

Both clipboards are in-memory stand-ins (`e2e_common.hpp`): the real clipboard of the desktop is never read or written, no window
station or administrator right is needed, and nothing is installed. Paste-pull offers are "pasted" the way Explorer does it
(`FileGroupDescriptorW` + one `FileContents` stream per file, read from the offer's OLE data object).

## Run

```powershell
tools\clipboard-e2e-qa\Run-ClipboardE2E.ps1 -Relays current                  # every scenario against this checkout's relay
tools\clipboard-e2e-qa\Run-ClipboardE2E.ps1 -Relays current -Only "sync/files"
tools\clipboard-e2e-qa\Run-ClipboardE2E.ps1 -Relays "current,old=<commit-ish or server dir>"
```

The script needs the repository's `.tools` (Visual Studio build tools, Flutter, Node.js 24.22.0) and a client checkout that has
built the Windows client once (`apps/windows_client/windows/flutter/ephemeral`; otherwise pass `-FlutterEphemeral <path>`, only its
C++ client wrapper headers are used). It builds the two helpers into `-Work` (default `%TEMP%\multitap-clipboard-e2e`; keep the path
short) at BelowNormal priority with at most two build workers (`-SkipBuild` reuses them), prepares one relay tree per `-Relays` entry
and runs `apps/windows_client/test/e2e/clipboard_e2e_test.dart` once against every relay in both modes. A relay entry is `current`
(this checkout's `server/`), `label=<server directory>` or `label=<commit-ish>`, extracted with `git archive` (its `node_modules`
link to `server/node_modules`). Without `-Relays` the script runs against `current` only. Labels may contain only letters, digits, dot, underscore and hyphen. Each invocation writes new
`run-<id>` and `relay-<label>-<id>` folders below `-Work` and never deletes earlier ones.

Without `MULTITAP_CLIPBOARD_E2E=1` (which the script sets) the test file is skipped, so a normal `flutter test` never starts it.

## Scenarios (each per relay and per mode)

* `manual`: the user's explicit send / receive (the session asks for each transfer);
* `sync`: Ctrl+C sharing is on, so a copy on either side is announced, read and sent by itself.

These two modes deliberately leave out the on-paste header on both peers to cover older peers. A separate `onpaste` group
negotiates the on-paste revision: copying files announces metadata only, and the receiver requests data when it pastes. A relay
without a revision skips that revision's scenarios and runs a compatibility case instead.

Common: text; 200 KB text; rich text (HTML); image; one small file; files of 100 KB, 3 B and 0 B; files just above one 45 KiB chunk;
the same two files copied three times in a row; three different file copies quickly; a folder tree; a 20 MiB file offered and
pasted; a tree of 300 files offered and pasted; unusual file names (Korean, spaces, dots, reserved-looking, accented); no echo of
received content; the Host's diagnostic records; alternating text and files in both directions.

Long paths and the [scale revision](../../docs/remote-session-protocol.md#scale-revision): a deep source folder (absolute paths of
370+ characters, served as virtual files once the received copy's absolute path reaches MAX_PATH) both ways; a 190-character
relative path (pasted with the revision, refused with the path message by a pair without it); a 298-character one (refused up
front by both, with the precise reason in the Host's record); a 5 GiB sparse file offered and its last 7 bytes pasted at their
offset; 300 top-level files; 64 MiB of text both ways and 64 MiB + 1; a newer copy replacing a 64 MiB text still in flight. The
Host helper takes `--no-scale` and `--no-onpaste` to exercise the earlier profiles.

On-paste: small files copied both ways stay idle and send no chunks until CF_HDROP or FileContents is requested; CF_HDROP and a
later virtual stream reuse the same staged copy; a 257-file set stays virtual-only; large text and rich text render on paste;
`preparePaste` waits for the announcement without moving file bytes; a Host without the optional waiting event stays silent while
serving a paste; cancelling or superseding a slow paste keeps the pair; changed or deleted sources report the precise reason;
source folders can be renamed before or during a paste and pasted folders after it; a withdrawal clears only the still-current
offer. Text-only permission, inverse denial and administrator revocation during an active text render are covered in both
directions. A legacy sequence pastes trees both ways, then images and files both ways, to catch writes that fail after an OLE file
publication.

The `formats` command inspects FD_PROGRESSUI without reading file contents. These are fixture checks, not an Explorer copy-dialog
or third-party application test.

## Throughput benchmark

`xfer_bench.mjs` pastes one way through the real Host worker (`clipboard_e2e_host.exe --quiet`) and the real relay
(`relay_fixture.mjs`, its own process), with the script as a fast client stand-in (pull queue, the receiver's acknowledgement
rhythm, SHA-256 checked). It prints one JSON line: MB/s, files/s and the CPU time of the Host, the relay and the stand-in. `--quiet`
still runs the service's JSON parse of every packet; the service's pipe and re-quote are not in it (`clipboard_stage_bench` times
them).

```powershell
node tools\clipboard-e2e-qa\xfer_bench.mjs --server <server dir> --host-exe <build>\Release\clipboard_e2e_host.exe --work <new folder named run-...> `
  --case vm2pc|pc2vm --files 1 --size 2147483648 [--discard] [--no-pullqueue] [--ack-chunks 4|22]
& <build>\Release\clipboard_stage_bench.exe <new folder named run-...>   # per-chunk CPU of each Host step
```

`vm2pc` pulls the VM's copy (one sparse file, or N real files in one folder); `pc2vm` serves the Host's pulls from memory while the
Host pastes like Explorer (FileContents streams, 64 KiB reads) into the work folder, or drops the bytes with `--discard`. Wall time
depends on the machine's load; CPU time per byte is the steadier figure. Not measured here: the Dart client, the Flutter platform
channel, TLS, a real network.

## Other targets

`<build>` is the runner's `<Work>\build` (or any configuration of this folder with `-DMULTITAP_FLUTTER_EPHEMERAL=<path>`). Build
other targets there with `cmake --build <build> --config Release --target <name>`.

| Target | What it checks |
| --- | --- |
| `clipboard_client_smoke` | the client adapter's native smoke test; run as a normal user with `$env:MULTITAP_CLIPBOARD_QA_SUBSTITUTED_ONLY = '1'` and a new folder named `run-...` as argument (no real clipboard) |
| `clipboard_recovery_qa` | the real Host worker with the real WinHTTP channel compiled in but every clipboard accessor substituted: an old publication gated before its write, close/new-ready/a fresh copy queued, ordered native mutations and the fresh copy last; private pipe pair generations, request prefixes, bounded retry delays, non-retryable WinHTTP security errors. No socket, no driver, no real clipboard |
| `clipboard_diagnostic_qa` | the real Host worker with substituted reads: after an acknowledged 86 KiB image, a failed read (`clipboard_entry_count_limit`) must report its own stage, cause, peer reason and entry bound, with unknown format/size instead of the previous image's |
| `clipboard_stage_bench` | per-chunk CPU of each Host step (above) |

These verify behaviour with stand-ins; they do not prove real TLS reconnection, Explorer/OLE behaviour or a real folder paste.

## Files

| File | Role |
| --- | --- |
| `Run-ClipboardE2E.ps1` | build, relay trees, environment, `flutter test` |
| `relay_fixture.mjs` | starts a relay from a server tree on loopback with an in-memory store; prints the clipboard endpoints and tokens as one JSON line; reports why the relay ended a pair |
| `host_e2e.cpp`, `client_e2e.cpp`, `e2e_common.hpp`, `virtual_source.hpp` | the two helper processes (JSON lines on stdin/stdout) and the shared stand-in clipboard |
| `xfer_bench.mjs`, `stage_bench.cpp` | the throughput benchmark |
| `recovery_qa.cpp`, `diagnostic_qa.cpp` | the substituted-clipboard regressions |
| `CMakeLists.txt` | builds all of the above and `clipboard_client_smoke` |
| `apps/windows_client/test/e2e/` | the scenarios (`clipboard_e2e_test.dart`) and the process/channel plumbing (`e2e_harness.dart`) |

The helpers' line protocol (a request carries `id` and `cmd` or `method`+`args`; a reply carries the same `id` and `ok` or `err`;
anything else is an event):

* both: `setText`, `setRich`, `setImage`, `setFiles`, `clear`, `get` (what the stand-in clipboard holds, with a SHA-256 for every
  file; a text above 1 MiB is reported by `textLength` and `textSha`), `describePaths`, `paste` (extract the published offer, or the
  locally served tree of a deep paste, into a folder), `pasteTail` (read the last bytes of one file through its stream),
  `makeSparse`, `ping`, `quit`;
* client: `method`/`args` = the calls of the `multitap/clipboard` MethodChannel (`capture`, `sourceChunk`, `begin`, `append`, ...);
  events `changed` and `pull`;
* host: events `emit` / `recv` (message type, request id, reason; never content), `diag` (the Host's diagnostic record),
  `channelError`.

Both helpers also accept `formats`, `renderText`, `renderHtml`, `renderImage` and `renderHdrop`. Blocking `GetData` calls run on a
separate thread while the helper dispatches its window messages. Host flags: `--drop-waiting` discards the optional waiting event,
`--emit-delay-ms N` delays each sent data chunk. Client `--serve-delay-ms N` delays each served chunk. `paste` accepts `slowMs` for
a consumer that reads slowly. These delays are local fixtures, not production latency measurements.

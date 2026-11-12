# Streaming drive redirection (drive-v1)

Folders the user picks on their PC appear inside the VM as drive letters, like
RDP's `\\tsclient\C`. The VM side has two backends:

- **WinFsp** (primary): a network volume served by a user-mode file system, so
  files over 4 GB work and a file opens without being downloaded whole.
- **The in-box WebDAV redirector** (automatic fallback where WinFsp is missing
  or a mount fails): no third-party driver, but whole-file caching and a 4 GB
  file limit.

The Windows client and the Android app share folders (the Android side is
described under [Android](#android)). The overview of all redirections is
[streaming-redirection.md](streaming-redirection.md), the protocol summary
[remote-session-protocol.md](remote-session-protocol.md#drives-and-usb-devices).

## Overview

```
VM program ─ WinFsp network volume ─ MultitapHost.exe --drive-fs (session user's token)
                                       │ pipe frames (drive_winfsp_wire.hpp)
   or      ─ MRxDAV / WebClient ─ HTTP loopback ─┐
                                                 ▼
                         Host service (LocalSystem): drive_redirection.hpp
                         one drive-v1 request multiplexer, own WSS (Host token)
                                                 │ drive-v1
broker: DriveRelay (server/src/drive.mjs): authorisation, share/path checks, flow limits
                                                 │ drive-v1
PC: DriveSession (lib/data/drive_session.dart) ─ method channel ─ native worker
    (windows/runner/drive_files_core.cpp: handle-checked paths)
```

* **The PC is the authority.** The Host never touches the VM's own files for
  this feature; every operation is a request that the client checks again
  (share, path, links, read-only). The client runs unelevated, so file
  operations have the rights of the person at the PC. If it is ever run
  elevated, its worker threads impersonate the desktop shell user's ordinary
  token, and sharing is refused without one (`drive_user_unavailable`): the VM
  never gets administrator rights through the client.
* The Host side runs in the Session-0 service, not in the streaming worker, and
  the drive channel is its own TLS WebSocket (like the clipboard), so large
  copies never share a pipe or a socket with video or input.
* Shares last for one streaming session. Nothing is shared until the user picks
  a folder in that session, and nothing is shared again automatically on the
  next connection.

## Permission and negotiation

`drives`, per assignment (user × VM), default denied (administrator console:
장치 공유 → 공유 폴더; stored in the assignment's device policy,
`server/src/device-policy.mjs`). USB storage forwarded as a raw USB
device needs this permission too.

| Client | Host | Broker | Result |
|---|---|---|---|
| does not request drives | any | any | no drives |
| `driveProtocol` | not advertised | any | no drives |
| `driveProtocol` | advertised | `drives` not allowed | not granted (the capability is not even shown to this account) |
| `driveProtocol` | advertised | `drives` allowed | drive-v1 granted, WebDAV backend |
| `driveProtocol` + `driveFs` | `driveFs` advertised | `drives` allowed | `driveFs` granted, WinFsp backend (WebDAV per share if a mount fails) |

* Host capability: `driveProtocol:'drive-v1'` with `service` and
  `interactiveSession` true, when WinFsp is installed or the WebClient service
  is usable (re-checked every 30 s); `driveFs:true` when WinFsp's DLL is
  registered. A Host with a signed-in user but neither backend reports
  `driveUnavailableReason:'drive_webclient_unavailable'` instead, so the client
  can show the 공유 폴더 button disabled with the reason. Only accounts whose
  assignment allows `drives` see these fields.
* Connect body: `driveProtocol:'drive-v1'` and optionally `driveFs:true`, for
  stream sessions only (single output or tracks); anything else is 400
  `invalid_drive_protocol`. Not granting is never a failure.
* The response then carries `driveProtocol`, `driveFs` (only when granted) and
  `driveWebSocketUrl: wss://<origin>/api/v1/sessions/<id>/drive`; the Host's
  `session.start` carries the same plus
  `driveUrl: wss://<origin>/api/v1/hosts/sessions/<id>/drive`. A client
  rejects a grant it did not request or a URL other than exactly that path on
  its own origin.

## Wire protocol

### The pair

The client connects with its Bearer token, the Host with the VM's Host token:
same live session, same Host identity, one socket per role, at most 128 pairs,
messages up to 288 KiB, no compression. When both are attached the broker sends
both `{"type":"drive.ready","sessionId":…,"protocol":"drive-v1","limits":{…}}`
with `limits = {maxShares:8, chunkBytes:262144, maxInFlight:8, listPage:256,
maxPath:1024, maxName:255, maxDepth:64}`. Any message before `drive.ready`, a
protocol violation, a rate excess, a timeout, revocation or session end closes
the pair (`{"type":"drive.error","reason":…}`); the stream continues. A closed
pair stays closed for that stream session.

### Paths

A path is relative to a share root: `''` for the root, otherwise 1-64 segments
joined with `/`, at most 1024 UTF-16 units. Each segment is a plain Windows
file name: 1-255 units, well-formed UTF-16, no control characters, none of
`<>:"/\|?*`, not ending in a dot or a space, not `.` or `..`, not a DOS device
name (`CON`, `PRN`, `AUX`, `NUL`, `CONIN$`, `CONOUT$`, `COM1-9`, `LPT1-9`,
including superscript digits, with or without an extension). The broker, the
Host and the client each check this.

### Shares (client → Host)

`{"type":"drive.shares","shares":[{"id":"a1","name":"Documents","readOnly":true}]}`
is the complete current list (0-8 shares), sent after `drive.ready` and on every
change. `id` matches `^[A-Za-z0-9_-]{1,32}$`, `name` is a safe segment of at
most 64 units (the label shown in the VM). The broker keeps the latest list and
answers a Host request for another share (`drive_share_unknown`) or a modifying
request on a read-only share (`drive_read_only`) itself.

The Host answers
`{"type":"drive.mounted","shares":[{"id":"a1","state":"mapped","letter":"Z"},{"id":"b2","state":"failed","error":"drive_letter_unavailable"}]}`
(state `mapped`, `pending` or `failed`; letter `D`-`Z`).

### Requests (Host → client)

`{"type":"drive.request","id":N,"op":…,"share":…,"path":…,…}`; `id` strictly
increases per pair; at most 8 outstanding; each must be answered within 60 s or
the broker closes the pair (`drive_timeout`).

| op | extra fields | success response |
|---|---|---|
| `stat` | — | `entry` (root: optional `space:{free,total}`) |
| `list` | `cursor` | `entries` (≤ 256 and ≤ 60,000 bytes of JSON), `next` when more |
| `read` | `offset`, `length` (1-262144) | binary kind 1 |
| `create` | `dir`, `truncate` (false for folders) | — |
| `rename` | `to` (same share, not root), `replace` | — |
| `delete` | — (a file or an **empty** folder) | — |
| `touch` | `modified` (ms since epoch) | — |
| `write` | binary kind 2 | — |
| `setsize` | `size` (`driveFs` only; truncate or zero-extend) | — |
| `flush` | — (`driveFs` only; FlushFileBuffers on a file, no-op otherwise) | — |

Responses are `{"type":"drive.response","id":N,"ok":true,…}` or
`{"type":"drive.response","id":N,"ok":false,"error":"<code>"}`.
`entry = {name, dir, size, modified, created, readOnly}`. Links and other
name-surrogate reparse points, and names that are not safe segments, are left
out of listings. The client lists a folder once for cursor 0 and serves later
pages from that snapshot (kept ≤ 30 s, ≤ 20,000 entries). Sizes and offsets
are JSON safe integers, so files above 4 GiB work.

Binary messages (little-endian):

* kind 1, client → Host, read data: `[0x01][u32 id][data]`, fewer than
  `length` bytes only at end of file.
* kind 2, Host → client, write: `[0x02][u32 id][u16 n][n bytes JSON header][data]`,
  header exactly `{"op":"write","share":…,"path":…,"offset":N,"truncate":bool}`,
  1-262144 bytes of data. `truncate:true` (offset 0) creates or empties the
  file first; otherwise the file must exist. Writing past the end extends it.

Error codes: `drive_not_found`, `drive_exists`, `drive_not_empty`,
`drive_read_only`, `drive_denied`, `drive_link_refused`, `drive_invalid_path`,
`drive_share_unknown`, `drive_busy`, `drive_no_space`, `drive_is_directory`,
`drive_not_directory`, `drive_io_failed`. Pair reasons: `drive_invalid_message`,
`drive_limits_exceeded`, `drive_out_of_order`, `drive_rate_limit`,
`drive_backpressure`, `drive_timeout`, `drive_pair_timeout`,
`drive_transport_closed`, `drive_policy_changed`, `authorization_revoked`,
`session_ended`, `server_stopping`. Mapping errors: `drive_webclient_unavailable`,
`drive_letter_unavailable`, `drive_map_failed`, `no_interactive_session`.

### Notices and opening a share

Two optional additions are negotiated without changing older shapes:

* **Notices** (`drive.notice`, Host → client, `drive_webclient_old_limit` or
  `drive_file_too_large`, once per session and code, never a name or size).
  The broker adds `"notices":true` to the Host's `drive.ready` and forwards a
  notice only to a client whose upgrade carried `X-Multitap-Drive-Notices: 1`.
  Only the WebDAV backend sends them.
* **Opening a share in the VM** (`drive.open-v1`). When both peers send
  `X-Multitap-Drive-Open: drive.open-v1`, `drive.ready` carries
  `openProtocol`, and the panel's 열기 sends `{"type":"drive.open","id","share"}`.
  The broker forwards it only for a currently mapped share, one at a time; the
  Host's user helper opens that drive in an Explorer window of the session and
  answers `drive.opened {id, share, ok, error?}` (`drive_open_unavailable`,
  `drive_open_failed`, `drive_open_unknown`, `drive_busy`, …). No path,
  command or executable is ever accepted from the client.

### Flow control, revocation and end

Honest peers are bounded by the window (8 requests of 256 KiB in flight), so a
copy is limited by the round trip and may exceed any fixed byte rate. The relay
therefore never closes a pair for being fast: above 160 MiB/s per pair,
512 MiB/s for all pairs or 2,048 messages/s it stops reading that socket until
the one-second window rolls over (TCP backpressure to the sender, nothing
buffered in the broker). Only a peer that keeps sending far past the pause
(more than 2× the byte budget or 8× the message budget in a second) is closed
with `drive_rate_limit`.

The broker rechecks every pair on its 250 ms authorisation sweep. When the
assignment no longer allows `drives` (or the assignment, account or Host is
gone) it closes the pair with `drive_policy_changed` or `authorization_revoked`.
The Host, on losing the channel for any reason, fails in-flight requests and
removes every drive of the session; the client stops serving and says once that
sharing ended.

## Host: WinFsp backend

### Processes and scope

* The file system runs in a helper started by the service once per session,
  `MultitapHost.exe --drive-fs`, with `WTSQueryUserToken` (the token Explorer
  runs with), in a job object that ends it with the service. It is user mode,
  not a service, so WinFsp defines the drive letter in **that user's logon
  session only**, as a temporary link that disappears with the volume handle
  when the helper exits or is killed. Other users of the VM do not see the
  drive; elevated programs of the same user (the split token's other logon
  session) do not either, exactly like mapped network drives.
* The volume is a network volume (`WinFsp.Net`, UNC prefix
  `\multitap\Multitap-<43 base64url characters>`, a 256-bit secret). Explorer
  treats it like a redirected drive and does not index it; deleting is
  permanent (no recycle bin). The letter is the next free one from Z down.
* The volume's only security descriptor is protected: owner and full control
  for the helper's user, SYSTEM and Administrators, nobody else, so another
  account that learns the UNC name still cannot open it. A helper that cannot
  build it refuses to mount. `SetSecurity` is accepted and ignored.
* The service trusts nothing from the helper: its `FsBridge` accepts only share
  ids it registered itself, safe paths and drive-v1 field ranges, and forwards
  calls to the same request multiplexer the WebDAV endpoint uses. Helper and
  service queues are bounded (128 calls; excess gets `drive_busy`).
* Service ↔ helper frames (`drive_winfsp_wire.hpp`): `[u32 length][u8 kind]
  [u32 id][u32 header length][header JSON][data]`, kinds call / result /
  control; controls `ready`/`unavailable`, `mount {token,id,label,readOnly}` →
  `mounted {letter}` | `failed {error,status}`, `readOnly`, `unmount` →
  `unmounted`, `stop`.

### How WinFsp operations map

| WinFsp | drive-v1 |
|---|---|
| GetSecurityByName / Open / GetFileInfo | `stat`, cached 1 s (`FileInfoTimeout`), negative answers too; any change through the volume clears the cache; errors are never cached |
| Create (file / folder) | `create` |
| Read | aligned 256 KiB blocks via `read`, all blocks of one request in flight together; sequential reads prefetch up to 8 blocks; a 48-block (12 MiB) cache per share keyed by path, size and modified time, dropped on any write |
| Write | 256 KiB `write` frames, up to 8 in flight; constrained I/O and write-to-end handled |
| SetFileSize / Overwrite | `setsize` |
| Flush, Cleanup of a written file | `flush` (`FlushAndPurgeOnCleanup`: the data is on the PC when the program closes the file) |
| SetBasicInfo | last write time → `touch`; other attributes and times are accepted and ignored |
| Rename | `rename` with `replace` (also case-only) |
| SetDelete / CanDelete + Cleanup | `list` (empty-folder check), then `delete` at cleanup |
| ReadDirectory | all `list` pages (≤ 20,000 entries) into WinFsp's directory buffer |
| GetVolumeInfo | root `stat` space (unknown: 1 TiB, so Explorer never refuses a copy); label `<name> (PC)` |

Volume: 4 KiB sectors, names ≤ 255, paths ≤ 1,024 units / 64 levels,
case-insensitive and case-preserving, no reparse points, streams or extended
attributes, `IrpTimeout` 120 s (above the relay's 60 s deadline, so the PC
answers or the pair ends first). The 읽기 전용 switch changes a mounted share
live, so every modifying callback checks it.

Errors: not found → `OBJECT_NAME_NOT_FOUND` (on an open handle `FILE_INVALID`),
exists → `NAME_COLLISION`, not empty → `DIRECTORY_NOT_EMPTY`, read-only →
`MEDIA_WRITE_PROTECTED`, denied or link → `ACCESS_DENIED`, busy →
`SHARING_VIOLATION`, no space → `DISK_FULL`, relay gone or share revoked →
`NETWORK_NAME_DELETED`, no answer within 70 s → `IO_TIMEOUT`, anything else →
`UNEXPECTED_IO_ERROR`. Nothing waits without a bound.

drive-v1 addresses files by path, not by a remote open handle. Concurrent
appends through separate handles of the same file are serialised by a
per-file mutation gate, but there is no atomicity against independent writers
on the PC. When a file is replaced through this volume, handles still open on
the old file get `STATUS_FILE_DELETED` rather than touching the replacement. A
write that times out is not cancelled on the PC, so a failed multi-chunk write
can leave later chunks applied.

### Packaging and licence

| | |
|---|---|
| Component | WinFsp 2.1 (2.1.25156), the project's own unmodified MSI |
| Source | `https://github.com/winfsp/winfsp/releases/download/v2.1/winfsp-2.1.25156.msi` |
| SHA-256 / size | `073a70e00f77423e34bed98b86e600def93393ba5822204fac57a29324db9f7a`, 2,191,360 bytes |
| MSI signature | Authenticode, NAVIMATICS LLC (DigiCert Global G3 Code Signing ECC SHA384 2021 CA1), timestamped |
| Driver | `winfsp-x64.sys`: Microsoft Windows Hardware Compatibility Publisher (attestation), timestamped |
| Licence | GPLv3 with the FLOSS exception: permission to link the WinFsp DLL and to distribute the unmodified installer with FLOSS software; Multitap is AGPL-3.0, a free software licence, so the exception applies |
| Pin | `apps/windows_host/third_party/winfsp/upstream.json`; SDK headers `winfsp.h`, `fsctl.h` and `License.txt` vendored there. The MSI is not stored in git: `tools/fetch-winfsp.ps1` downloads and checks it, and the Host package carries it as `bin/winfsp/` |

**WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos**,
<https://github.com/winfsp/winfsp>. The Host package carries the licence as
`COPYING.winfsp.txt` and a WinFsp section in `THIRD-PARTY-NOTICES.txt`. The
Host does not link `winfsp-x64.lib`: it loads `winfsp-x64.dll` from WinFsp's
registered install folder (`HKLM\SOFTWARE\WOW6432Node\WinFsp\InstallDir`,
writable by administrators only) and resolves the exports it needs, so a Host
without WinFsp starts normally.

### Installation

Host settings installs WinFsp (`setup_winfsp.hpp`, through the Host's
settings request `installWinfsp`) when it finishes a first install, and from
장치 → 공유 폴더 while WinFsp is missing. Only the default **Core**
feature is installed (`msiexec /i … /qn /norestart REBOOT=ReallySuppress`); the
developer features stay off. It accepts only the exact path
`<Program Files>\Multitap\Host\<version>\bin\winfsp\winfsp-2.1.25156.msi`,
re-checks size, SHA-256 (constants in `setup_winfsp.hpp`, compared with
`upstream.json` by the build) and the Authenticode signature, and retries while
another installation runs (error 1618). Afterwards it verifies that the DLL
loads, exports the file-system API and finds the registered driver.

| Installed | Action |
|---|---|
| nothing | install; the Host records `HKLM\SOFTWARE\Multitap\Host\WinFsp` (`InstalledByHost`=1, `Version`) |
| older | upgrade; ownership is not claimed |
| same or newer | nothing installed, usability verified only; never a downgrade |
| version unreadable | left alone |

A failure is never fatal: 공유 폴더 uses the
basic method meanwhile, and the result names a fixed code (`winfsp_package_mismatch`,
`winfsp_busy`, `winfsp_blocked`, `winfsp_install_failed`,
`winfsp_driver_missing`, `winfsp_load_failed`, `winfsp_dll_missing`,
`winfsp_path_invalid`). A Windows Installer restart request (3010) is shown as
a restart notice; until the restart the WebDAV path serves. Host
settings → 장치 → 공유 폴더 shows the state and can install the carried MSI
(WinFsp 설치).

Nothing removes WinFsp today. The marker records whether the Host installed it,
so an uninstaller may remove it only when `InstalledByHost` is set and WinFsp's
own dependency record lists no other product.

## Host: WebDAV fallback

### Why WebDAV

Without a third-party driver, Windows offers a user-mode program only one way
to present a remote file system as a drive letter: an already installed
network redirector.

| Option | No new driver | Drive letter | Verdict |
|---|---|---|---|
| WebDAV mini-redirector (WebClient, MRxDAV) to a loopback HTTP server | yes on Windows 10/11 (Server: optional feature) | yes | fallback |
| WinFsp | no, a signed third-party driver | yes | primary |
| Cloud Files API | yes | no, a folder | sync semantics; content stays on the VM disk |
| Projected File System | optional feature | no | files materialise on the VM disk |
| SMB to a user-mode server | port 445 belongs to the in-box server | yes | not practical |
| Dokan, an own minifilter | no | yes | not used |

The WebClient service is deprecated (no longer started by default, still
present in Windows 10 and 11). Where it is missing or disabled and WinFsp is
not installed, the Host does not offer drives and says why; it never installs
the feature or changes a disabled service.

### Security model

* **Loopback only.** The endpoint binds `127.0.0.1` and `::1` on one ephemeral
  port; no firewall rule is added.
* **A capability path instead of HTTP authentication.** Every share gets a
  fresh 256-bit token and the only URLs served are
  `http://localhost:<port>/Multitap-<token>/…`; anything else is 404. The
  redirector never sees a 401 or prompts for credentials, and no
  `BasicAuthLevel`, `AuthForwardServerList`, loopback check or zone setting is
  touched.
* **Who can reach it.** The token exists only in the Host service, in the
  mapping of the session user's logon session (invisible to other users'
  sessions) and in that user's `HKCU` drive label. Any process of that user can
  use the drive, like RDP's `\\tsclient`; an administrator of the VM can learn
  the path, as with any redirection.
* **Lifetime.** Tokens, port and mappings belong to one stream session. Session
  end, a lost channel, a revoked permission or a Host restart invalidate every
  token at once (in-flight requests fail with 503) and the mappings are
  removed.

### Server and mapping

* WebDAV subset (`drive_dav.hpp`, RFC 4918 as MRxDAV uses it): `OPTIONS`,
  `PROPFIND` depth 0/1, `GET`/`HEAD` with one byte range, `PUT` (streamed in
  256 KiB pieces), `DELETE`, `MKCOL`, `MOVE` (same share, `Overwrite`),
  `PROPPATCH` (last-modified time via `touch`), `LOCK`/`UNLOCK` (in-memory),
  `COPY` 501. Limits: 64 connections, 16 KiB of headers, 64 KiB request bodies
  except PUT, 120 s of socket silence per operation. Read-only shares answer
  every modifying method 403. Status mapping: not found 404, exists 405/412,
  read-only/denied/link 403, busy 423, no space 507, channel down 503, timeout
  504, others 502.
* Mapping needs the session user's ordinary token (drive mappings live per
  logon session), so the service runs a one-shot `MultitapHost.exe --drive-mount`
  with `WTSQueryUserToken`. It picks the highest free letter from Z down
  (retrying on a letter taken meanwhile), maps with `WNetAddConnection2W` without
  `CONNECT_UPDATE_PROFILE` (never persistent), sets the label `<name> (PC)`
  and announces the drive. Before mapping it removes leftovers of a crashed
  session: connections named exactly `\\localhost@<digits>\Multitap-<43 base64url>`
  and their labels, nothing else. Mapping runs on its own worker thread, never
  on the relay thread; session end and service stop return only after every
  drive is unmapped.

### WebClient size caps

The redirector's own caps (`FileSizeLimitInBytes`, default 50,000,000;
`FileAttributesLimitInBytes`, default 1,000,000) are raised by the Host to
4,294,967,295 and 10,000,000 when lower. They apply only when WebClient starts:
a stopped WebClient is started after writing them; a running one is restarted
only when it provably serves nothing but Multitap (no established non-loopback
connection of the WebClient process and no other WebDAV connection in any
logon session). Otherwise the drive is mapped anyway and the client is told
once `drive_webclient_old_limit` (`VM을 다시 시작하기 전까지는 50MB가 넘는 파일을
옮길 수 없어요`). These are size limits only; no authentication or trust value
changes.

### Limits of the fallback

* WebClient caches whole files: opening downloads the whole file first, and a
  changed file is uploaded whole on close. A single file of 4 GB or more is
  impossible (the redirector's cap is 32-bit); the Host reports the first one it
  sees once per session (`drive_file_too_large`).
* Downloaded content stays in WebClient's cache on the VM disk for a while
  (readable by VM administrators only), unlike RDP's drive redirection.
* A machine-wide proxy that does not bypass `localhost` can break the mapping;
  nothing is changed.
* The VM's paths stay within MAX_PATH through MRxDAV.

## Backend selection and Host diagnostics

A session uses WinFsp only when the broker granted `driveFs` and WinFsp is
available. Per share, a mount that fails (helper cannot start, WinFsp cannot
create or mount the volume, no letter) falls back to WebDAV for that share, and
WebClient is prepared only then. A helper that ends on its own ends the drive
channel; the stream continues.

`hostAction 247` (labels in `server/admin/js/labels.js`): `stage` 1 channel
ready, 2 mapped, 3 mapping failed, 4 unmapped, 5 channel closed, 6 WebClient
started, 7 WebClient unavailable, 8 WebClient already running, 9 WebClient
restarted for the caps, 10 old caps kept (other WebDAV use), 11 old caps kept
(restart failed), 12 a file of 4 GiB or more seen, 13 WinFsp mounts done,
14 WinFsp mount failed → WebDAV, 15 WinFsp unavailable at mount → WebDAV,
16 helper ended; with `expected` (shares), `missing` (failures), `error` (a
fixed code, `drive_protocol.hpp diagnosticCode`) and `cause` (the Win32 error or
NTSTATUS). Never names, paths or bytes.

## Client

* `lib/data/drive_session.dart`: the drive-v1 pair (opened with the first
  picture, beside the clipboard), the share list, request dispatch and the
  panel state. At most 8 native operations run at once; a ninth request is
  answered `drive_busy`, never queued. File data moves in ≤ 256 KiB requests
  and is never buffered beyond one request.
* `windows/runner/drive_channel.cpp` and `drive_files_core.cpp` (method channel
  `multitap/drive`): three worker threads. Every existing component below the
  share root is opened with `FILE_FLAG_OPEN_REPARSE_POINT` and refused when it
  is a symbolic link, junction or mount point; before anything changes, the
  opened object's final path must lie under the share root's final path;
  deletes and renames go through checked handles (rename uses
  `NtSetInformationFile` relative to a checked parent). Every path is opened in
  the `\\?\` long form. Cloud placeholders (OneDrive) are not links and hydrate
  on first read. The folder picker only allows file-system folders.
* **Files the desktop writes.** Every file the VM creates or writes into gets
  a Mark-of-the-Web (`Zone.Identifier`, ZoneId=3, fixed `about:internet`
  URLs) before content lands, so SmartScreen and Protected View apply on the
  PC; an existing Internet or Restricted zone mark is kept. Volumes without
  named streams (FAT, exFAT) cannot carry it. The VM may not create, write or
  rename to shell metadata names (`desktop.ini`, `autorun.inf`, `*.lnk`,
  `*.url`, `*.scf`, `*.library-ms`, `*.searchConnector-ms`): `drive_denied`.
* **Unreachable folders.** A root that answers a device-gone error (a pulled
  USB stick, a dropped network share) makes the share offline: the VM's
  requests get `drive_not_found` at once, the letter stays, and the row offers
  다시 시도. A request unanswered for 40 s is answered `drive_io_failed` (the
  relay would otherwise end the whole pair at 60 s); two in a row mark the
  folder offline. A shared folder renamed or moved on the PC keeps working; a
  deleted one ends its share and is never picked up again by name.
* **Panel.** The session toolbar's **공유 폴더** opens a panel (a dialog when
  there is no room): each folder with its VM drive letter and state, **열기**
  (where the Host supports it), a **읽기 전용** switch (applied at once: the
  folder is re-opened natively first, then the share list is resent), **공유
  해제**, and **폴더 추가…** (disabled with its reason at 8 folders). A new
  folder is shared read-write after one confirmation that says so; folders that
  deserve a warning (a whole drive, Windows or program folders, the whole
  profile or AppData, a network folder) are named in that confirmation, which
  never refuses. A lost channel or a revocation shows one toast.
* Client diagnostics, event `drive`: `stage` (1 ready, 2 shares sent, 3 Host
  mapping result, 4 ended, 5 add started, 6 picker cancelled, 7 confirmation
  declined, 8 native open failed with the Win32 code, 9 folder offered, 11 add
  failed, 12 picker failed, 13 folder deleted on the PC, 14 Host notice), `ok`,
  `shares`, `mapped`, request counts and a fixed `drive_*` code. Never names,
  paths or content.

## Android

The Android app shares a folder the person picks with Android's own folder
picker (`ACTION_OPEN_DOCUMENT_TREE`). It asks for no storage permission: it
reaches only the picked tree, through the Storage Access Framework, with the
grant the picker gave (taken as a persistable grant, given back when the share
ends; grants left by an earlier run are given back at start). Nothing is shared
again on the next connection. The wire protocol, the broker and the Host are
unchanged: the same `DriveSession` runs over `AndroidDriveFileSystem`
(`lib/android/android_drive.dart`), channel `multitap/android/drive`
(`AndroidDrive.kt`); a share's path is the tree URI, shown as a place in words.

**This app's own folder list.** The picker is a
system app (DocumentsUI, shown as 파일) that differs from phone to phone, can
be turned off (then every 폴더 추가 fails in a few ms) and is
missing on a TV. On Android 11+ it is therefore not used at all: 폴더 추가
opens this app's own folder list (`android_folder_chooser.dart`) with
all-files access (`MANAGE_EXTERNAL_STORAGE`), asked for on first use in
Android's settings page; back in the app the list opens. Such a share's path
is the folder's absolute path and its files go through `FileDriveStore` (the
same `DriveShare` rules over `java.io`/`Os`). Only a folder chosen in that
list in this run is opened, only inside a mounted storage volume; the desktop
sees only that folder. Android 10 and older have no such access and keep the
picker (described above); there a picker that does not open says why
(`pickFolder` error details 1-8, in the drive record's `error`), and the
request is not started when nothing answers it (a refused start would leave
the app "not in front" and without input). A USB drive Android
does not show as storage is out of reach of the list.

`DriveFiles.kt` maps paths to documents (no Android types; `DriveFilesTest`
runs it against a fake provider). A documents provider knows documents by
opaque id, so every folder on a path is listed once and remembered (64
folders, 60,000 names); a name a listing lacks is "not found" for 2 s before
the provider is asked again, and a remembered document the provider no longer
has is looked up again. Provider calls run on a pool of 8 threads, pieces of
one file in the order they came; a listing or an open is cancelled after 35 s.

| What a provider cannot do | What the desktop gets |
|---|---|
| Random-access writes (a pipe instead of a file) | data in order only (a new file written front to back works); anything else `drive_denied` |
| Cutting or growing a file in place | exact or refused: growing writes the last byte, emptying reopens the file, a length in between `drive_denied` |
| Rename, or move to another folder (`FLAG_SUPPORTS_RENAME` / `_MOVE` missing) | `drive_denied` |
| A taken name: providers silently make "name (1)" on create and rename | undone, `drive_exists` (`drive_invalid_path` when the name itself is refused) |
| Replacing a file by a rename (no atomic replace) | the old file is kept under a temporary name until the new one has its name, and put back if that fails |
| A rename that changes capitalisation only | through a temporary name where the provider would invent another |
| Deleting a folder is recursive | a folder with anything in it is `drive_not_empty` first |
| Setting a modified time | `touch` is accepted and changes nothing: the time is the time of writing |
| Creation time, free space | creation = modified; free space only for the device's own shared storage |
| Case: a volume may or may not tell `A` from `a` | an exact name first, else the only name differing in capitalisation; two such names are both listed and only an exact name opens either |
| File locking | none: two writers are not kept apart (as with any shared folder) |
| Virtual or still downloading documents, names Windows cannot open | not listed |

The picked folder itself is checked every 2 s. Access taken back (the person
or Android revoked the grant) ends that share at once; a folder the provider
no longer finds is offline for one check and ends at the second; a provider
that does not answer is offline, never ended. The share's drive is removed in
the desktop and the app says which it was (`‘<name>’ 폴더를 열 권한이 없어져서
공유를 끝냈어요` / `… 찾을 수 없어서 …`). A folder renamed or moved on the device
is "gone" (its document id is its path in Android's own storage provider).

Without `driveFs` (the WebDAV fallback) there is no `flush`: a written
document is closed after 2 s without a write, and a provider that keeps data
only on close reports a failure there to nobody. With `driveFs` the Host's
flush closes it and its failure reaches the program in the desktop.

Not tried on a device: the mapping is tested against a fake provider only.

## Cases

Messages are the Korean strings of `lib/ui/drive_panel.dart`.

| Case | What happens | Message |
|---|---|---|
| The same folder twice | Refused (case, slash and trailing-slash insensitive) | `이미 공유한 폴더예요.` |
| The same folder through another alias (junction, `subst`, UNC), or a folder inside a shared one | Not detected; both shares work independently | none |
| More than 8 folders | Button disabled with its reason | `폴더는 8개까지 공유할 수 있어요.` |
| No permission on the folder | Not added | `이 PC 사용자 권한으로 열 수 없는 폴더예요. …` |
| The folder cannot be found (renamed, deleted, drive not connected) | Not shared | `PC에서 폴더를 찾을 수 없어요. …` |
| No Explorer shell on the PC while the client runs elevated | Sharing refused, never run with administrator rights | `이 PC의 로그인한 사용자(바탕 화면)를 확인하지 못해 공유할 수 없어요. …` |
| A USB stick pulled or a network share dropped while shared | Offline row, VM gets "not found", 다시 시도 re-opens it | `PC에서 폴더에 접근할 수 없어요 · …` |
| The folder is deleted on the PC while shared | The share ends and the VM drive is removed | `‘<name>’ 폴더가 삭제돼서 공유를 끝냈어요` |
| A listing above 20,000 entries | Cut at 20,000 | `파일이 20,000개를 넘는 폴더는 VM에서 처음 20,000개만 보여요. …` |
| No free drive letter in the VM | `drive_letter_unavailable` | `VM에 남은 드라이브 문자가 없어요. …` |
| No signed-in Windows user in the VM | drive-v1 is not advertised; a session lost mid-way fails `no_interactive_session` | `VM에 로그인한 Windows 사용자가 없어요. …` |
| WebClient disabled and no WinFsp | The button is disabled with the reason | `VM의 WebClient 서비스가 꺼져 있거나 없어서 …` |
| `WNetAddConnection2` fails (WebDAV) | `drive_map_failed`, the Windows error recorded | `VM에서 드라이브를 연결하지 못했어요. …` |
| A file of 4 GB or more on the WebDAV fallback | The copy fails in Windows; the notice is shown once | `파일 하나가 4GB를 넘으면 공유 폴더로 옮길 수 없어요. 클립보드 복사·붙여넣기로 옮기세요.` |
| A file locked by a PC program | `drive_busy` (sharing violation / HTTP 423) | the VM program's own message |
| Disk full on the PC | `drive_no_space` (disk full / HTTP 507) | the VM program's own message |
| A save interrupted by a network drop or PC sleep | The file is written in place, so a half-written file can remain on the PC (as with a network share or RDP drive) | the connection-lost toast (next row) |
| The PC sleeps or the network drops | The pair closes, the Host removes every drive; reconnect and share again | `공유 폴더 연결이 끊겼어요 · 다시 연결한 뒤 공유할 폴더를 선택해 주세요` |
| The administrator removes `drives` | The pair closes, the stream continues | `공유 폴더 연결이 끝났어요 · 관리자가 권한을 해제했어요` |
| The VM user disconnects the mapped drive in Explorer | The panel keeps showing it until the session ends | none |

## Known limits

* Attributes other than read-only, creation and access times, ACLs, streams,
  hard links and reparse points are not carried; hidden files look normal.
* Names Windows cannot open safely (device names, trailing dot or space, lone
  surrogates) are not listed; alternate data streams are refused.
* Deleting from Explorer is permanent; there is no recycle bin for the PC's
  files.
* Writes from a memory-mapped view reach the PC when Windows flushes the
  mapping, at the latest on close.
* Antivirus or other scanners running as SYSTEM in the VM may read files
  through the share.
* Elevated programs of the same VM user do not see the drive.

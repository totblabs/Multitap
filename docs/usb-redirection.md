# Streaming USB redirection (usb-v1)

A USB device plugged into the user's Windows PC (control endpoint 0 plus bulk
and interrupt endpoints) shows up inside the VM, and goes back to the PC when
the user disconnects it, the session ends, the permission is revoked or either
side crashes. Typical uses: storage sticks, security tokens and smart card
readers, USB serial adapters, vendor devices and FIDO keys. The PC side uses
UsbDk, the VM side the usbip-win2 virtual host controller; why these two, the
signing rule and the shared security model are in
[streaming-redirection.md](streaming-redirection.md). The wire summary is also
in [remote-session-protocol.md](remote-session-protocol.md#drives-and-usb-devices).

Not offered: isochronous devices (webcams, headsets), audio and video classes,
keyboards and mice, hubs, Bluetooth, and USB network adapters of the PC. The
channel works in single-output and multi-monitor (tracks-v1) sessions. Only the
Windows client forwards physical devices; the Android app requests `usb-v1`
solely for the virtual mouse below.

## 마우스 잠금: the virtual USB mouse

The session toolbar's 마우스 잠금 option (game mouse in code and protocol
names) hands the mouse to the VM for games. Where the session has the USB
channel, the client lends the VM Multitap's own virtual USB HID mouse instead of
injecting input, so games that ignore injected input (for example anti-cheat
protected ones) read an ordinary USB mouse:

- The device is made up by the client, not captured: report-protocol HID
  (not a boot mouse, so the default deny rule does not apply and no exception is
  needed), VID:PID 1209:0001, 5 buttons, signed 16-bit relative X/Y, wheel and
  horizontal pan, 7-byte reports on interrupt IN 0x81. The PC's Raw Input feeds
  it while the remote window holds the mouse (`windows/runner/game_mouse_usb.h`).
  No UsbDk is needed.
- The Host recognises exactly these descriptors
  (`apps/windows_host/src/usb_virtual_mouse.hpp`; shared vector
  "multitap game mouse" in `server/test/fixtures/usb-descriptor-vectors.json`),
  answers every control request itself and keeps interrupt reads waiting at the
  client ahead of the VM's own, so a report is one network crossing from the PC
  to the VM's HID driver.
- It needs only `usbDevices`. Without the grant, without a usable controller in
  the VM or with an older Host, 마우스 잠금 uses injected relative movement
  (`relative-mouse-v1`) instead.

## Permission and negotiation

- `usbDevices`, per assignment (user × VM), default denied (stored in the
  assignment's device policy, `server/src/device-policy.mjs`). The
  grant needs the client's request (`usbProtocol: "usb-v1"` in the connect
  body), a Host that advertised `usbProtocol: "usb-v1"` and the permission.
  Anything missing leaves an ordinary stream: nothing is refused. A malformed
  request is `invalid_usb_protocol`.
- A device whose descriptors include a class with its own permission also needs
  that one: mass storage (08h) `drives`, printer (07h) `printers`, smart card
  (0Bh) `smartCards`, CDC (02h/0Ah) `serialPorts`. The broker checks this at
  attach and whenever the policy changes; the Host checks it again from the
  validated descriptors and the `usbGrant` the broker puts in `session.start`.
- Revoking `usbDevices` closes the USB channel: the Host detaches everything,
  the client gives the devices back, the stream goes on. Revoking a class
  permission detaches only the devices that needed it.
- The Host advertises `usbProtocol` only when its package carries the
  usbip-win2 files and Windows is a workstation edition, Windows 10 1809 or
  later (Windows Server loads only HLK-signed device drivers).
- Connect response: `usbProtocol`, `usbWebSocketUrl`. `session.start` to the
  Host: `usbProtocol`, `usbUrl`, `usbGrant {drives, printers, smartCards,
  serialPorts}`. In a tracks session the response adds
  `usbTracksProtocol: "usb-tracks-v1"` and the tracks `session.start` carries
  `usbUrl` too.

## Channel

`/api/v1/sessions/{id}/usb` (client, Bearer) and
`/api/v1/hosts/sessions/{id}/usb` (Host, Host token); one socket each, pair
timeout 15 s (`server/src/usb.mjs`). Text frames are JSON (at most 16 KiB),
binary frames carry URBs.

Text: `usb.ready` (relay, with limits), `usb.status` (Host: `ready`, or
`unavailable` with a fixed reason), `usb.attach {device, descriptors (base64),
speed}` (client), `usb.attached` / `usb.rejected {device, reason}` (Host),
`usb.detach {device, reason}` (either). Device numbers are 1..255, chosen by the
client and never reused in a session; at most 4 devices attached and 64 attach
attempts per session. Every reason comes from a fixed list (below); no free text
crosses.

Binary: a 24-byte little-endian header and the payload.

| off | size | field |
|---|---|---|
| 0 | 1 | type: 1 submit (Host), 2 return (client), 3 unlink (Host), 4 unlink-return (client) |
| 1 | 1 | device |
| 2 | 1 | endpoint (bit 7 = IN) |
| 3 | 1 | transfer: 0 control, 2 bulk, 3 interrupt (never 1, isochronous) |
| 4 | 4 | seq (the Host's, unique among requests in flight) |
| 8 | 4 | length (submit: bytes to write or read; return: bytes moved; unlink: target seq) |
| 12 | 4 | status (signed; submit/unlink 0; return 0 or a Linux errno from a fixed set) |
| 16 | 8 | setup packet (control only, else zero) |

Backpressure: one window per session of 1 MiB and 32 requests in flight; a
request is at most 256 KiB (the Host splits bigger USB/IP transfers into
sequential chunks; a short IN chunk ends the transfer). The kernel's connection
thread waits for room, so the VM's own queue fills instead of the relay's, and
the relay's per-socket cap (4 MiB) is never reached by honest peers. Video and
input use other sockets and never wait for this one.

The relay checks everything in both directions: header size, who may send which
type, device state, endpoint and transfer consistency, control setup direction
and length, payload size against the request, the status set, double or unknown
returns, the window and the count. A violation closes only the USB channel
(`usb.error`), never the stream. A late answer for a device that was just
detached is dropped, not an error.

## Host (VM side)

Files in `apps/windows_host/src/`: `usb_policy.hpp` (descriptor parsing and the
default policy), `usbip_wire.hpp` (USB/IP and relay codecs), `usb_core.hpp`
(session logic), `usb_driver.hpp` (package, install, ownership, protection,
VHCI ioctls), `usb_redirection.hpp` (loopback server, channel, orchestration),
`usb_virtual_mouse.hpp`; wired in `service.hpp` beside the other channels.

- **Driver pin.** usbip-win2 0.9.7.5-preview as published by the Open Source
  Codesigning Initiative (`apps/windows_host/third_party/usbip-win2/upstream.json`):
  six files pinned by SHA-256, never edited, not stored in the repository.
  `tools/fetch-usb-drivers.ps1` fetches and verifies them and
  the Host package carries them as `usb-driver/`. Without them the Host
  does not advertise USB.
- **Install only when needed.** The service installs the controller when a
  session granted `usbDevices` asks for it and it is missing, natively
  (SetupAPI/newdev; `DiInstallDriver` for the filter, `DiInstallDevice` for the
  root device `ROOT\USBIP_WIN2\UDE`; no PowerShell, nothing compiled or written
  as an executable by a script). Before installing it checks every hash, the
  Microsoft signature of each `.sys` and `.cat`, and that each `.sys` and `.inf`
  is a member of its signed catalog. A restart requirement is reported
  (`usb_restart_required`), never forced. The INF's debugging registry values
  (`VerboseOn`, WDF `VerifierOn`) are switched off after installing; they are
  registry values, not signed content.
- **One controller, ours.** The device instance the Host creates is recorded
  (`HKLM\SOFTWARE\Multitap\Host\Usb\Instance`). A controller bound to a
  byte-identical copy of the pinned package whose record was lost, or a double
  registration of it, is taken back automatically. A controller of another
  product or another usbip-win2 version is never used or changed silently: the
  session gets `usb_vhci_unavailable`, and Host settings → 장치 → USB says that
  another program uses the USB function and offers **Multitap용으로 바꾸기**.
  After its confirmation the other controller node(s) are removed (their
  attached devices detach and that product's USB sharing stops; its driver
  package stays in the driver store) and Multitap's controller is installed from
  the Host's verified package.
- **VHCI hardening.** The signed package leaves its device object open to every
  local user. After installing and on every session the Host (1) sets the
  devnode's security descriptor (`SPDRP_SECURITY_SDS`, SYSTEM and
  Administrators only), (2) sets the same descriptor on the running device
  object (`SetSecurityInfo`), and (3) **proves** it: a copy of its token with
  Administrators, SYSTEM and every other powerful group made deny-only must get
  `ERROR_ACCESS_DENIED` when opening the interface. Until that proof succeeds
  the controller is `usb_vhci_unprotected` and nothing is attached (fail
  closed).
- **Loopback USB/IP server.** Exists only during a session: `127.0.0.1`, a fresh
  port, `SO_EXCLUSIVEADDRUSE`, and it accepts only a connection whose owning
  process is the System process (PID 4, the kernel's WSK socket); a user process
  is closed unread. It answers `OP_REQ_IMPORT` once for the exact busid of a
  device waiting for its import, an empty `OP_REQ_DEVLIST`, and then
  `CMD_SUBMIT` / `CMD_UNLINK`. The attach is the non-persistent plug-in ioctl:
  nothing goes into the driver's persistent attach list.
- **Descriptors.** The client's device and configuration descriptors are parsed
  strictly; the VM's `GET_DESCRIPTOR` (device, configuration) is answered from
  that validated copy, so Windows binds drivers only to what the Host accepted.
  Only bulk and interrupt endpoints of that copy and endpoint 0 are used;
  isochronous or unknown endpoints end the connection or stall. `SET_ADDRESS`,
  `SYNCH_FRAME` and test mode are never forwarded.
- **Default deny** (one rule set in `server/src/usb.mjs`, `usb_policy.hpp` and
  the client, with shared vectors in
  `server/test/fixtures/usb-descriptor-vectors.json`): hubs (09h), wireless
  controllers and Bluetooth (E0h), audio (01h), video (0Eh), any isochronous
  endpoint, and boot-protocol HID (keyboards and mice, including a composite
  device that has one). Report-protocol HID such as FIDO keys is allowed with
  `usbDevices` alone. Limit: a keyboard without a boot interface looks like any
  HID device to the broker and the Host (the client still hides it, below).
- **Lifecycle.** A device is attached to the VM, not to a Windows user: every VM
  user sees it while it is attached. It is detached on the client's
  `usb.detach`, a policy change, the kernel closing its connection (the VM
  detached it), channel loss, session end and service stop, and anything left is
  removed at the next service start and at every session start.
- **Cancellation and start watch.** A kernel unlink is answered when the client
  confirms the reset or after 3 s, so a PC request that ignores cancellation
  cannot hold the VM's kernel indefinitely; late returns are dropped and more
  than eight unanswered originals detach the device. A read-only Configuration
  Manager lookup checks the VM's device node about once a second until it
  starts: Windows problem 10 twice, or no problem and no start within 45 s,
  detaches it with `usb_device_error`. Any other Windows problem (such as a
  missing vendor driver) is left for the VM user to resolve.

## Client (PC side)

Code: `lib/data/usb_redirection.dart`, `lib/data/usb_picker.dart`,
`lib/ui/usb_panel.dart`, `windows/runner/usb_capture*.{h,cpp}`,
`usb_topology.cpp`, `usb_describe_core.h`.

- **Panel.** The session toolbar's **USB 장치** (shown when the broker granted
  the channel) lists this PC's devices with one 연결 button each, a short note
  that a device leaves this PC while the VM uses it, and a warning for storage.
  A device is captured only after the VM accepted it (`usb.attached`), so a
  refused device never leaves the PC. 연결 끊기 gives it back. A device held by
  another remote window of this client offers **이 창으로 옮기기**: that window
  gives it back through its own session first.
- **Names.** Product = the device's own iProduct
  (`DEVPKEY_Device_BusReportedDeviceDesc`), else the friendly name, else the INF
  description; generic Windows names ("USB Composite Device", "USB 대용량 저장
  장치", …) never count. Vendor = the VID looked up in a table generated from
  `usb.ids` (BSD-3-Clause, `apps/windows_client/third_party/usb-ids`, regenerated
  with `tools/generate-usb-vendors.ps1`). VID:PID is small secondary text; with
  no usable name the title is "<vendor> 보안토큰/프린터/USB 장치". Identical
  titles get the serial's last characters, else the port number.
- **Drives first.** A storage device is shown by its volumes: `<label> (E:)`,
  `USB 드라이브 (E:)` or `<label> · 드라이브 문자 없음`, with a second line such
  as `<disk name> · 32GB · exFAT`. The chip 공동인증서 appears when a volume root
  has an `NPKI` or `GPKI` folder (existence only; nothing is read or sent). An
  empty card reader says 메모리 카드가 없어요.
- **Tokens.** Interface class 0Bh or a smart card reader child gets the chip
  보안토큰·스마트카드. A HID-only token cannot be told from other HID devices and
  is listed under 기타 like a FIDO key.
- **기타 USB 장치 (N)**, collapsed: printers, phones (MTP), cameras (PTP) and
  other devices. Classes the policy denies are not listed at all.
- **Never offered** (counted in one line under 기타): hubs (including docks),
  keyboards and mice (boot-protocol HID **or** a keyboard/mouse child device,
  which also catches gaming keyboards without a boot interface) and USB network
  adapters (CDC ECM/NCM, RNDIS, a phone's USB tethering), because sharing one
  could cut the connection the session runs over.
- **Order.** Devices plugged in within 2 minutes (`DEVPKEY_Device_LastArrivalDate`)
  come first with the badge 방금 연결됨, then devices given to a VM before (a
  hash of "vid:pid[:serial]" in `%LOCALAPPDATA%\Multitap\Client\usb-memory.json`,
  64 newest), then the rest. While the channel is up the runner watches device,
  disk and volume arrivals (`CM_Register_Notification`) and the list refreshes
  after 400 ms, keeping focus and the opened 기타 section.
- **Handing over a drive.** No eject (an ejected device leaves the PnP tree and
  UsbDk could no longer capture it). Before `usb.attach` the runner locks each
  volume of the drive (`FSCTL_LOCK_VOLUME`, which also flushes the cache) and
  releases the lock; after the VM accepts, it locks again and dismounts
  (`FSCTL_DISMOUNT_VOLUME`) immediately before the capture. A lock refused
  because files or folders are open changes nothing and says "PC에서 사용 중이라
  넘길 수 없어요. 열린 파일이나 창을 닫고 다시 눌러 주세요." The check cannot stop
  another program from opening a file between the check and the capture, and
  UsbDk's capture is still a surprise removal for Windows. A storage prepare is
  bounded by 15 s (`usb_prepare_timeout`) and can be cancelled.
- **UsbDk.** UsbDk 1.0.22 (Red Hat, Apache-2.0, attestation-signed;
  `apps/windows_client/third_party/usbdk/upstream.json`). The client runs
  unelevated; it installs UsbDk only after the user chooses **USB 드라이버
  설치…** and confirms. It verifies the bundled installer (size, SHA-256, Red
  Hat, Inc. Authenticode signature, held without write sharing) and starts
  `msiexec /i /qn /norestart` with one Windows approval prompt; a declined
  prompt is `usbdk_elevation_cancelled`. It never restarts Windows. The MSI
  ships beside the client (`usbdk/`) when the client is built after
  `tools/fetch-usb-drivers.ps1 -UsbDk`. `UsbDkHelper.dll` is loaded only after
  its signature verifies.
- **Transfers.** `usb_capture_core.h` runs each request with UsbDk's overlapped
  read/write, translates `SET_INTERFACE`, `SET_CONFIGURATION` and
  `CLEAR_FEATURE(ENDPOINT_HALT)` into UsbDk calls and maps Windows/USBD errors to
  the wire's fixed statuses (a stall to `-EPIPE`). A cancelled request answers
  `-ECONNRESET`; UsbDk aborts a whole pipe, so other requests on that endpoint
  end the same way. A control request unanswered for 5 s is aborted with
  `-ETIMEDOUT`. Capture is bounded by 20 s; a stuck cancellation gets one reset
  after 3 s and its late result is dropped. Enumeration runs on a separate lane
  from capture and transfers, so listing never blocks I/O. A cancelled or
  timed-out capture stays owned by cleanup until its handle is released exactly
  once, so a retry cannot capture the same device concurrently.
- **Giving back.** Releasing a capture (연결 끊기, `usb.detach` from the Host or
  broker, unplugging, session end, window close, channel loss) closes the UsbDk
  handle; UsbDk returns the device to Windows also when the process dies. A
  read-only check then records whether Windows restarted it. If it did not, the
  panel offers **PC로 되돌리기** (UsbDk takes the device once more and gives it
  straight back) and otherwise suggests replugging; a new capture of that device
  waits until its return is confirmed.
- **Reconnect.** Devices that were shared when a stream was replaced by a
  reconnect are offered again in the new session through the same checks as a
  click, with one notice either way.

## Reasons (fixed)

| Kind | Codes |
|---|---|
| Reject (Host) | `usb_class_denied`, `usb_hid_boot_denied`, `usb_isochronous_denied`, `usb_descriptor_invalid`, `usb_class_not_granted`, `usb_vhci_unavailable`, `usb_limit_reached`, `usb_attach_failed`, `usb_busy` |
| Detach | `usb_user_stopped`, `usb_device_removed`, `usb_vm_detached`, `usb_policy_changed`, `usb_device_error`, `usb_session_closed` |
| Host status | `usb_driver_missing`, `usb_driver_install_failed`, `usb_vhci_unprotected`, `usb_vhci_unavailable`, `usb_setup_failed`, `usb_restart_required` |
| Channel | `usb_invalid_message`, `usb_out_of_order`, `usb_limits_exceeded`, `usb_rate_limit`, `usb_backpressure`, `usb_pair_timeout`, `usb_transport_closed`, `usb_unavailable`, `usb_policy_changed` |
| Client (local) | `usbdk_missing`, `usbdk_install_failed`, `usbdk_signature_invalid`, `usbdk_elevation_cancelled`, `usbdk_restart_required`, `usb_device_busy`, `usb_capture_failed`, `usb_limit_session`, `usb_capture_timeout`, `usb_request_stuck`, `usb_device_not_returned`, `usb_prepare_timeout`, `usb_return_unknown`, `usb_pc_in_use`, `usb_busy_elsewhere`, `usb_device_redirected`, `usb_move_timeout` |

The client-local codes are diagnostics and panel messages, not relay detach
reasons.

## Diagnostics

**No vendor/product ID, name, serial number or data is ever logged or sent**:
they identify the user's own hardware, and no decision in broker, Host or
support needs them. Only the descriptor bytes the VM must see cross the relay
(they necessarily include the IDs and any serial string the device reports);
they are held in memory and not kept.

- Host `hostAction 250`: a fixed `stage` (`UsbStage` in `usb_core.hpp`; the
  admin console labels are checked against it by
  `server/test/usb-labels.test.mjs`), `error` a fixed reason number or Windows
  error, `expected` a count. Stages 24-28 describe a device that misbehaves:
  24 failed request (`error` errno, `expected` request class), 25 kernel unlink
  (`error` 0 client confirmed, 1 fallback after 3 s), 26 control request
  unanswered for 5 s, 27 per-device summary (`items` requests, `kib`, `cause`
  failed, `expected` cancelled), 28 the VM's device node did not start (`error`
  Windows problem code, `expected` seconds since attach).
- Client `usb` event: counts (`usbDevices`, `usbUrbs`, `usbKib`, `usbClass`)
  and a fixed `usb_*` code. Stages 7/8 attach requested/accepted; 9 per-device
  summary (`usbUrbs`, `usbKib`, `usbStalls`, `usbFailed`, `usbTimeouts`,
  `usbSlow`, `usbReq`, `usbUsbd` the raw USBD_STATUS of that request); 10 the
  return check (`usbBack` 1 restarted, 2 not in time, 0 unknown; with 2, `error`
  1 no device node, 2 not started, 3 no disk under it, 100 + Windows problem
  code); 11 a stuck cancellation; 13 a move between windows; 14 PC로 되돌리기;
  15 devices offered again after a reconnect.
- A request class is `bmRequestType << 8 | bRequest` for control requests
  (GET_MAX_LUN = 0xA1FE), otherwise `0x10000 | transferType << 8 |
  endpointAddress`. An attached acknowledgement alone does not prove that the
  VM started the device; stage 28 and the summaries show what happened.

## Limits

- Losing the USB socket ends USB for that stream: the broker keeps the closed
  pair and the Host closes its USB server without redialing. Reconnecting the
  stream starts a new channel (and offers the shared devices again).
- The VM start watch matches the VM's device node by VID:PID, so an identical
  second device can satisfy it.
- Throughput is bytes in flight over RTT: 1 MiB at 40 ms is about 25 MiB/s at
  best, and each control transfer costs one round trip. Interrupt devices see
  half a round trip of extra delay.
- A USB disk can be corrupted when the session drops mid-write; the panel warns,
  and storage also needs `drives`.
- UsbDk aborts a whole pipe to cancel one request, so neighbouring requests on
  that endpoint can fail.
- A keyboard without a boot interface is denied only by the client's own
  listing, not by the broker or the Host.
- The 다른 세션에서 사용 중 state is a heuristic: a device Windows has no name
  for and that this client does not hold.
- usbip-win2 is attestation-signed (no Windows Server), and HVCI compatibility
  of the two drivers has not been established
  ([streaming-redirection.md](streaming-redirection.md#third-party-drivers-considered)).

## Android: physical devices (not built)

The Android app does not lend its own USB devices to the desktop. It uses
usb-v1 only for 마우스 잠금's virtual mouse. Reading both sides showed that
the public USB host API (`android.hardware.usb`) cannot serve usb-v1
correctly, so nothing was wired: no device is listed, asked for or claimed,
and the manifest declares no USB host feature.

What usb-v1 needs from the client and the API does not give
(`UsbDeviceConnection`, `UsbRequest` reference):

| usb-v1 needs | The public API |
|---|---|
| A status per request (`-EPIPE` stall, `-ETIMEDOUT`, `-ENODEV`, `-EPROTO`); Windows class drivers act on a stall | `controlTransfer` / `bulkTransfer`: "length of data transferred (or zero) for success, or negative value for failure"; a completed `UsbRequest` has only its buffer position, so a stalled, a failed and a zero-length read look the same |
| CLEAR_FEATURE(ENDPOINT_HALT) that also resets the host's data toggle (what usbip's stub does with `usb_clear_halt`) | no clear-halt call; a raw control request tells the device only |
| The device's speed (`low`/`full`/`high` in `usb.attach`) | no speed getter |
| The device back with its own driver after 연결 끊기 | `claimInterface(force)` disconnects the kernel driver; nothing reconnects it (the device stays without a driver until it is plugged in again) |
| Isochronous endpoints | none: "UsbRequests can be used to transfer data on bulk and interrupt endpoints" (webcams and audio are denied by usb-v1 anyway) |
| Requests up to 262,144 bytes | 16,384 bytes a request before Android 9 (splitting works) |

USB storage is the device people would try first, and it is the one that
needs the first two rows in ordinary use: Windows sends commands many sticks
answer with a stall, and the Bulk-Only recovery is exactly CLEAR_FEATURE with
a toggle reset. With inferred statuses a stick would enumerate, then fail in
ways that look like a broken stick. A mounted stick would also have to be
taken from Android without an unmount call. Devices at SuperSpeed (a USB 3
stick in a USB 3 phone port) are refused by the shared descriptor rule
(`bMaxPacketSize0` must be 8-64) on every client.

The way to do it properly is native: `UsbDeviceConnection.getFileDescriptor()`
is the usbfs descriptor, and usbfs ioctls from a small JNI library give the
real status of every URB (`USBDEVFS_REAPURB`), `USBDEVFS_CLEAR_HALT`,
`USBDEVFS_CONNECT` and `USBDEVFS_GET_SPEED`, which is how libusb works on
Android. That needs an NDK library and a device to try it on; it changes
nothing in the protocol, the broker or the Host.

What exists: `UsbTransfers.kt` (`UsbPump`), the request state machine over an
abstract connection: pieces of at most what a request takes, one request at a
time per endpoint in order, cancellation, SET_CONFIGURATION / SET_INTERFACE
through the API, and the inference that stands in for the missing status
(asks the endpoint's halt bit after a read of nothing or a short write). It is
tested against a fake connection (`UsbTransfersTest`, 10 cases) and is not
referenced by the app. A native connection would give it real statuses.

## Tests

- Broker: `server/test/usb.test.mjs` (negotiation, shared descriptor vectors,
  framing violations that close only the channel, attach rules, class
  permissions, revocation, diagnostics projection), `usb-labels.test.mjs`,
  `usb-latency.test.mjs`.
- Host: `MultitapHostUsbRedirectionTest` (`usb_redirection_test.cpp`: fake kernel
  and client for the core, substituted Windows for preparation and controller
  ownership, real loopback sockets, the VHCI ABI, and with `MULTITAP_USB_PACKAGE`
  the pinned files' hashes, signatures and catalogs).
- Client: `test/usb_redirection_test.dart`, `usb_negotiation_test.dart`,
  `usb_panel_test.dart`, `usb_picker_test.dart`, `usb_reconnect_test.dart`,
  `usb_takeover_return_test.dart`, `usb_game_mouse_test.dart`,
  `native_usb_test.dart`; native `test/native/usb_capture_core_test.cpp`
  (target `multitap_usb_capture_smoke`) and `test/native/usb_describe_core_test.cpp`
  (target `multitap_usb_describe_smoke`: vendor table, names, drives, partitions,
  NPKI, tokens, composite devices, hubs, keyboards, network adapters,
  duplicates, a fake volume layer for the hand-over check).

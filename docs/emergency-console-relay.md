# Emergency console relay

The broker relays the Proxmox VNC console of an assigned VM to the user. Proxmox credentials, the `vncproxy` ticket and the VNC password stay on the broker. The relay (`server/src/console-relay.mjs`, RFB filter `console-rfb.mjs`, Proxmox adapter `proxmox.mjs`) serves two kinds of grant:

- `novnc`: the browser viewer under `/console`, also shown by the Windows and Android clients in a WebView. Its HTTP routes, cookie and clipboard rules are in [browser-console-v1.md](browser-console-v1.md).
- `native`: a bearer-authenticated WebSocket for a client that speaks RFB itself (below).

Both kinds share the admission, lifetime and bounds described here.

## Admission

A grant is created only when all of these hold:

- the bearer or console cookie belongs to an enabled account whose credentials do not need changing, and the account is currently assigned the desktop;
- the desktop is a Proxmox VM (not a local PC) with a numeric VMID of at least 100 and a stored SMBIOS UUID that is not all zeros or all `F`;
- the integration token currently holds `VM.Console` on `/vms/<vmid>` (read from Proxmox's own effective-permission API; the value only describes propagation, so `0` counts as granted). Missing privilege gives 403 `console_permission_denied`; the broker never edits permissions. The admin console's **긴급 콘솔 권한** button shows the Shell command that adds it;
- Proxmox reports the VM running (`console_vm_unavailable` otherwise), and its current configuration still has the stored SMBIOS UUID (`console_identity_changed` otherwise);
- the VM has no streaming session, no other console, no pending power action and no display operation (`console_busy`).

The identity and permission checks run again before dialling Proxmox, after dialling but before any upstream byte reaches the user, and every 5 seconds while the console is open. The account, assignment and identity are also checked on every client message and every delayed upstream delivery. Logout, account disablement, a removed assignment, a changed VM identity or Proxmox configuration end the exact grant. These checks are fresh reads, not an atomic guarantee against a VM being replaced in Proxmox.

While a console is open the VM refuses new streaming and display operations. Nothing here starts or stops a stream, changes power or touches drivers; an explicitly authorised power action can still end the VM, which ends the console.

### Limits

- One console per VM, at most 2 per account and 8 per broker (429 `console_limit_reached`).
- At most 6 new attempts per account per minute (tracked for up to 256 accounts).
- An unclaimed grant expires after 30 seconds; native authentication must arrive within 5 seconds; the RFB handshake has 10 seconds.
- An open console lasts at most one hour. The broker pings the client every 10 seconds and ends the console after 30 seconds without a pong.

## Native client contract

Assigned VM desktops in `GET /api/v1/desktops` carry `consoleRelayProtocol: "rfb-none-v1"`. Local PCs do not.

- `POST /api/v1/desktops/:desktopId/console-sessions` with the bearer and `{}`: 201 `{consoleSession:{id, ticket, relayPath, expiresAt}}`.
- `DELETE /api/v1/desktops/:desktopId/console-sessions/:id` without a body: 200 `{id, released:true}`. It cancels only that account's exact grant.
- Connect a WebSocket to the broker origin plus `relayPath` (`/relay/console/<id>`) with the same `Authorization: Bearer` header and no query string. The first message must be TEXT `{"type":"console-auth","ticket":"..."}`. The ticket is single-use and independent of any Proxmox ticket.
- After upstream authentication and a fresh identity check the broker sends TEXT `{"type":"console-ready","protocol":"rfb-none-v1","keyboardProtocol":"qemu-keyboard-v1"}`, then a binary RFB 3.8 stream with security type None. It is protected by the bearer, the ticket and TLS; it is not an open VNC port.
- The client sets the pixel format `[32,24,0,1,0,255,0,255,0,255,16,8,0,0,0,0]` and may request at most 8 encodings, all from: Raw (0), CopyRect (1), DesktopSize (-223), Cursor (-239), QEMU Extended Key Event (-258) and QEMU LED State (-261). Anything else ends the connection.
- The end of a console is a TEXT `{"type":"console-ended","code":"..."}` followed by close, or just the close if sending failed. A grant cannot be reused; reconnecting needs a new grant.

The browser profile differs only where noVNC needs it: the broker spends the ticket itself after cookie authentication, sends no JSON, removes encodings it does not allow from noVNC's request instead of failing, and passes clipboard text in the directions the assignment allows. The native profile never carries clipboard text.

## Upstream

The adapter calls the configured Proxmox HTTPS endpoint with the stored CA and full certificate verification, without redirects. It reads the effective permissions and current VM configuration, opens `vncproxy` (`websocket=1`) and connects to `vncwebsocket` with the API token. It answers the VNC challenge with the returned password, or the ticket when no password is returned (DES through Node's `des-ede3` with the key repeated; an unavailable cipher fails as `console_crypto_unavailable` without changing OpenSSL policy). Only upstream RFB 3.8 with None or VNC authentication is supported.

No credential, ticket, upstream address or raw upstream error reaches a client, a URL, a diagnostic or an audit row. Audit rows (`console_granted`, `console_ready`, `console_ended`) carry only the desktop, the grant ID and fixed end codes.

## Bounds

The relay never trusts sizes it has not checked:

- 16 MiB parser and receive buffers; 16 MiB and at most 1,024 queued pieces towards the client; 1 MiB towards Proxmox; one send in flight per socket, 5 seconds per send.
- From the client: at most 1,000 messages and 1 MiB per second.
- Framebuffer at most 8192 pixels per side and 16,777,216 pixels; at most 4096 rectangles per update; cursor at most 256×256; clipboard text at most 64 KiB.
- An oversized, unsupported or malformed stream closes the console with a fixed code instead of allocating a claimed frame.

A cancelled grant releases its slot only after its Proxmox requests and sockets have actually ended; a socket that arrives late is closed.

## Activity and updates

For the broker's activity report (`/internal/activity`, `activity-v1`) every console grant counts as display activity, so the server's idle check does not treat a VM with an open console as idle.

## Display

The QEMU display may be switched off while Multitap's virtual displays are in use, and GPU passthrough VMs often draw nothing on it. When the VM uses the standard VGA model and its Host is online and supports `qemu-console-v2`, the broker first asks the Host to make the QEMU display primary for the duration of the grant (see "QEMU display preparation" in [browser-console-v1.md](browser-console-v1.md)). Otherwise an authenticated console can still show a black picture; the relay does not change guest displays without the Host.

## Tests

`server/test/console-relay.test.mjs` (grant lifecycle, limits, revocation, cancellation, idle activity), `console-rfb.test.mjs` (RFB parsing, VNC authentication, encodings, bounds, clipboard filtering), `console-proxmox.test.mjs` (permission, identity and upstream adapter), `console-read-budget.test.mjs` and `console-novnc-protocol.test.mjs`.

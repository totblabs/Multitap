# Browser emergency console: broker contract

The emergency console shows the Proxmox VNC console of an assigned VM in a browser page served by the broker under `/console`. It works without the Host, for example when Windows does not start or no one is signed in. The Windows and Android clients open the same page in a WebView. It is a viewer of the broker's console relay (see [emergency-console-relay.md](emergency-console-relay.md)), not a new VNC server, and it is separate from streaming.

The page uses the vendored noVNC 1.7.0 (`server/admin/console/vendor/novnc/`, provenance in [browser-console-frontend.md](browser-console-frontend.md)). The browser never receives a bearer token, a Proxmox address or credential, or a VNC password, and nothing is loaded from a CDN. Code: `server/src/browser-console.mjs`, `console-webviews.mjs`, `console-relay.mjs`.

## Transport requirement

Every `/console` route requires HTTPS as the broker observed it: direct TLS or a trusted reverse proxy (`MULTITAP_TRUSTED_PROXIES`). A configured `https://` public origin alone, an arbitrary forwarded header or plain HTTP is refused with `console_https_required`. Query strings, encoded or traversing paths and a foreign `Origin` are refused.

## Routes

- `GET /console/:desktopId`: the viewer page. The desktop ID matches `[A-Za-z0-9][A-Za-z0-9_-]{0,79}`. The page itself reveals nothing about the VM; the API checks the assignment. The URL holds no token, so it can be bookmarked.
- `GET /console/assets/:path`: only files listed in `server/admin/console/assets-manifest.json`, with matching size and SHA-256. A missing or changed file disables the viewer (`console_assets_unavailable`) but not the other broker APIs.
- `POST /console/api/login` `{username, password}`: the same password check, rate limits and session limits as client sign-in. 200 `{user:{username}, csrfToken}` and the console cookie.
- `GET /console/api/session`: 200 with the same body; renews the client session within its normal lifetime.
- `POST /console/api/logout` `{}` or `DELETE /console/api/session` (no body): 204, ends this session, clears the cookie and ends its console connections.
- `POST /console/api/desktops/:desktopId/connect` `{}`: 200 `{id, wsPath:'/console/ws/'+id, expiresAt, desktop:{id,name}, clipboardPolicy:{textToRemote,textToLocal}, qemuConsolePreparation:{status}}`. The clipboard flags come from the assignment, never from the request. 202 `{code:'console_cleanup_pending'}` means a just-ended stream is still restoring its display; the user retries. A running stream or another console on the VM gives 409 `console_busy`.
- `GET /console/api/desktops/:desktopId/sessions/:id`: read-only status of a grant's QEMU display preparation (below). It renews nothing and contacts neither Proxmox nor the Host.
- `DELETE /console/api/desktops/:desktopId/sessions/:id` (no body): 204 after this user's grant is cancelled. Unknown or foreign IDs give 404 `console_session_not_found`.
- `GET /console/ws/:id` (WebSocket upgrade): needs the cookie, the exact `Origin` and HTTPS. The ID alone grants nothing; the broker spends the grant's one-use ticket internally, and the browser then receives plain binary RFB (no JSON preamble).
- `POST /console/api/mfa/verify` is reserved and always answers 503 `console_mfa_unavailable`; the broker has no second sign-in factor.

### Opening from the app

The clients do not put the user's bearer token into the WebView. The client asks `POST /api/v1/desktops/:desktopId/console-webview` (bearer, `{}`) and receives `{protocol:'console-webview-v1', path:'/console/:desktopId/attach', code, expiresAt}`. The WebView posts `code=<code>` as a form to that path and is redirected (303) to `/console/:desktopId` with a console cookie. The code is single-use, bound to the user's login, the VM and the origin, and expires after 30 seconds. The resulting console session lives only in broker memory, works only for that VM, lasts at most one hour and never longer than the parent login, and ends when the parent login, the assignment or the account ends. The client closes it with `POST /api/v1/desktops/:desktopId/console-webviews/close` or `POST /api/v1/console-webviews/close`.

Desktops advertise `consoleBrowserProtocol:'console-browser-v1'` and `consoleWebViewProtocol:'console-webview-v1'` in the desktop list only when the broker is reached over HTTPS. Local PCs have no emergency console.

## Cookie, CSRF and authority

The cookie is `__Secure-multitap-console-<first 16 hex of sha256(origin)>`, `Path=/console`, `Secure`, `HttpOnly`, `SameSite=Strict`, without `Max-Age`. It authenticates an enabled client session whose credentials do not need changing. Administrator cookies are not read, and the administrator role does not bypass VM assignment.

Every write needs the exact `Origin`; every write except login also needs `X-Multitap-Console-CSRF`, an HMAC scoped to the console, the origin and this session, which cannot authorise native or admin actions. After each body read or upstream wait the broker authenticates the session again. The relay repeats the account, assignment, VM identity and clipboard-policy check on every client message, every delayed upstream delivery and its periodic sweep. Logout, expiry, a removed assignment and a withdrawn clipboard permission end access.

Responses are `no-store`, `nosniff`, `no-referrer`. The page CSP allows only same-origin scripts, styles, fonts and connections, plus `data:`/`blob:` images and inline style attributes that unmodified noVNC needs, and `frame-ancestors 'none'`.

## Clipboard

Both directions are denied unless the assignment allows text in that direction (`textToRemote`, `textToLocal`). The relay enforces this itself: VM clipboard text in a denied direction is discarded, and a clipboard message from the page in a denied direction ends the connection (`console_clipboard_denied`). VNC clipboard text is Latin-1, so the page refuses other characters instead of letting them turn into question marks. Clipboard content is never logged. File clipboard does not exist on this path.

## QEMU display preparation

A VM streaming through Multitap shows its picture on Multitap's own virtual displays, and Windows may have switched the Proxmox (QEMU) display off. When the VM uses the standard VGA model and its Host is online and supports `qemu-console-v2`, `connect` first asks the Host to make the QEMU display the primary output with the virtual outputs off, and to hold that topology while the grant lives (a 30-second lease the broker renews every 10 seconds). `qemuConsolePreparation.status` is `isolated` only while that hold is confirmed; otherwise it is `unknown`, and the console still connects. When the console ends the Host releases the hold. The page shows the picture either way; it says when no picture arrived.

A valid RFB connection can still show a black screen: the QEMU display may be off with an older or offline Host, and GPU passthrough VMs often draw nothing on the QEMU display. The viewer never claims that Windows is ready, only that console image data arrived.

## Tests

- `server/test/browser-console-http.test.mjs`: real loopback HTTP and WebSocket, in-memory store and a synthetic Proxmox. Cookie, Origin and CSRF rules, shared login limits, assignment changes during waits, cleanup ownership, single-use grants, expiry and logout, static-asset tampering.
- `server/test/browser-console-assets.test.mjs`: the asset manifest bounds and symlink refusal.
- `server/test/console-webviews.test.mjs`: the app hand-off codes and derived sessions.
- `server/test/browser-console-ui.test.mjs` and `tools/test-browser-console.mjs`: the page (see [browser-console-frontend.md](browser-console-frontend.md)).

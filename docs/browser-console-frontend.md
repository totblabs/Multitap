# Browser VM console: page

The viewer is `/console/<desktopId>`. The URL holds no token and can be bookmarked; the broker checks the signed-in account's current assignment. It is the emergency QEMU console, separate from streaming. A working RFB connection does not mean Windows is usable: the page reports only what it observes. The broker side is in [browser-console-v1.md](browser-console-v1.md).

## Files and provenance

`server/admin/console/index.html`, `console.js`, `console.css` and `hangul-key.js` are Multitap's own code. The broker serves only the files listed in `server/admin/console/assets-manifest.json`, checking each size and SHA-256. The manifest covers the page, its font and notices and the whole vendored noVNC tree (not itself). It is an integrity inventory, not a signature; server updates are signed separately.

noVNC is release **v1.7.0** (commit `63107bd06d9e1f6136ff21aeda8cd62cbf0d433e`), pinned in `tools/vendor-novnc.pin.json` by the npm package's SHA-512 integrity and SHA-256 and by the SHA-256 of the GitHub commit archive. The vendored copy under `server/admin/console/vendor/novnc/1.7.0/` has 64 files (all of `core/` and `vendor/`, `AUTHORS`, `LICENSE.txt`, `README.md`, `package.json`, `docs/LICENSE.*`), each byte-identical in both archives and listed in `server/admin/console/vendor/novnc/manifest.json`. The MPL-2.0, BSD and pako MIT notices are kept, the unmodified JavaScript is shipped beside the page, and `server/admin/console/THIRD-PARTY-NOTICES.txt` names the upstream source and licences. No upstream script runs during vendoring.

The font is `server/admin/fonts/PretendardVariable.woff2` copied unchanged (SHA-256 `9599f12fd42fc0bce1cd50b47a0c022e108d7aa64dd0d1bb0ed44f3282d900b4`) with its SIL OFL notice. No CDN, external font, analytics or telemetry is used.

Check or restore the vendored files with Python 3:

```powershell
python tools/vendor-novnc.py                       # verify the vendored tree against the pins
python tools/test-vendor-novnc.py                  # tests of the vendoring helper
python tools/vendor-novnc.py --install --fetch     # recreate missing files from the two pinned archives
```

`--npm-archive` and `--github-archive` use local copies of the archives instead of `--fetch`. The helper checks every archive entry before use, refuses traversal, absolute paths, duplicates, links, devices and oversized entries, never calls tar extraction, never overwrites different existing bytes and refuses symlinks or reparse points in the destination. After a reviewed change to the page files, regenerate the inventory with `--write-assets-manifest`.

## Session

The page keeps the CSRF token in memory and relies on the broker's HttpOnly console cookie. It stores no token, password or theme in browser storage and puts no authority in a URL. The password field is cleared after every attempt. Requests are same-origin, `no-store`, without redirects, with a 15-second timeout; writes send `X-Multitap-Console-CSRF`.

Opening the page checks the session, shows the Multitap sign-in if needed, then connects once. A refusal, a pending cleanup or a failure shows a fixed message and a **다시 연결** button; there is no automatic reconnect loop. Retrying first closes the old RFB connection and releases the old grant. Server error text and WebSocket close reasons are never inserted into the page; only known fixed codes select local messages, with the code behind **자세히**. Logout releases the grant and then calls the broker's logout; only its 204 counts as logged out, a failure keeps the session and offers a retry, and a 401 returns to sign-in.

noVNC receives the same-origin binary WebSocket directly ([noVNC API](https://novnc.com/noVNC/docs/API.html)). An unexpected VNC credential or server-verification request ends the attempt. noVNC's logger is turned off so clipboard and transport content are not logged.

## Layout

Before a connection a top bar shows the VM, its state and the commands, and a status card shows connecting, ended and failed states. Connected, the picture fills the window and the commands hang from a grip at the top centre: they open on a click and fold after a command, on a press into the picture, on Esc or with the fold button. Commands are icon buttons with names and tooltips: 다시 연결, Ctrl+Alt+Del 보내기 (`RFB.sendCtrlAltDel()`), 텍스트 클립보드 (only when a direction is allowed), 전체 화면, 새 창에서 열기, theme, 도움말, 로그아웃. There are no power buttons or Proxmox credential fields. Controls wrap with enlarged text, and sign-in and help scroll when the window is short. A browser tab may switch dark/light for itself (that tab only).

**In the app.** The clients open the page in a WebView whose user agent contains `Multitap-ConsoleWebView/1` (a presentation hint only; the broker does not trust it). There the wordmark, 새 창, theme and 로그아웃 are hidden and the page follows the app's theme. When the Windows app's WebView bridge is present, the page asks the close question itself: the app sends only `multitap-console:close-request`, and the page answers only `multitap-console:ready`, `close-shown`, `close` or `close-cancel` (each with the `multitap-console:` prefix) and ignores anything else. 전체 화면 appears only where it works: a browser tab, or the Windows app with the bridge.

## Picture and input

The RFB `connect` event shows **콘솔 연결됨**. At 1, 5 and 10 seconds the page reads the canvas once through the public `getImageData()` when it has at most four million pixels; a non-black opaque pixel shows **콘솔 연결됨 · 화면 데이터 수신**. The pixels are discarded at once and no screenshot is kept. If no picture arrived after 10 seconds the page says so and suggests reconnecting. It never claims that Windows or a physical display is ready. While a grant is open the page also reads its QEMU display preparation status every 10 seconds (shown as a tooltip).

The page tracks the keys and mouse buttons pressed on its canvas. Window blur, hiding the tab, focus moving to the toolbar, logout, disconnect and page hide send the matching releases through noVNC's own handlers and release pointer capture, so nothing stays held in the VM.

## Clipboard and Korean input

Both directions are denied unless the assignment allows them. Received VM text appears in the clipboard dialog only when that direction is allowed, up to 65,536 characters, and is copied to the PC only with **내 PC에 복사**. Text goes to the VM only with **VM으로 보내기**. The VNC clipboard is Latin-1, so other characters (Korean included) are refused with a fixed message instead of being turned into question marks. The broker enforces the same permissions against modified pages.

Ordinary physical keyboard input goes through noVNC unchanged, with one repair for the Windows Korean toggle key ([browser-console-hangul.md](browser-console-hangul.md)). Shortcuts that the browser reserves stay on the local PC; the help recommends the installed client for full key forwarding.

## Tests

- `server/test/browser-console-ui.test.mjs`: the page against a mock API.
- `tools/test-browser-console.mjs`: drives the page in a headless Chromium browser against a loopback fixture with a real RFB WebSocket server (no guest, no live broker). It needs an existing Playwright module and browser, and installs nothing:

  ```powershell
  node tools/test-browser-console.mjs --playwright-module=C:/path/to/playwright/index.mjs --browser=C:/path/to/msedge.exe
  ```

  It covers several viewport sizes and text scales in both themes, sign-in, the toolbar, help, password clearing, no storage or bearer use, the RFB handshake and paint detection, key and button release, denied clipboard, the Latin-1 refusal, manual retry and failure messages. Results and screenshots go to `--output` (default `.artifacts/browser-console/ui`).

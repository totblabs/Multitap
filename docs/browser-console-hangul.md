# Korean toggle key in the browser console

Windows can deliver the physical Korean toggle key (한/영) to a browser as a keyup without a matching keydown (`key` `HangulMode` or `code` `Lang1`). noVNC 1.7.0's `_sendKeyEvent` (`server/admin/console/vendor/novnc/1.7.0/core/input/keyboard.js`) drops a keyup that is not in its list of pressed keys, so the toggle never reaches the VM.

`server/admin/console/hangul-key.js` (`createHangulKeyAdapter`) repairs only that case. It acts only when all of these hold: Windows, a trusted event, the focused console canvas, an active session and an RFB connection that is not view-only. It then calls the public `RFB.sendKey(0xff31, 'Lang1')`, which sends a down/up tap. Keydowns, paired releases, repeats, ordinary keys, Ctrl/Alt combinations and IME composition stay on the unchanged noVNC path. `Process` or composition events alone are not treated as this key. No noVNC private state is read or changed, and the vendored noVNC files are unmodified.

A physical `Lang1` keydown followed by its keyup is one ordinary noVNC pair even when the two events report different `key` values (`Process` down, `HangulMode` up), so no extra tap is added. A keydown without a keyup keeps noVNC's behaviour; the adapter never invents a release.

The listener sits on the `#remote` container in the capture phase, so it runs before noVNC's own listener on the canvas and before the page's held-key tracking. The page's input release (blur, hidden tab, toolbar focus, logout, disconnect) resets the adapter first. A keyup that arrives only after focus returns cannot be told apart from a new one; the adapter repairs it, preferring not to lose the first real key press after focus.

On the wire, `RFB.sendKey` uses the negotiated QEMU Extended Key Event when the server acknowledged it (keysym `0xff31`, QEMU keycode `0x72`, two 12-byte messages) and otherwise two 8-byte standard RFB key events. Windows' raw scan code `0xf2` is not the QEMU keycode. The broker's RFB filter is not relaxed for this.

Tests: `server/test/browser-hangul-key.test.mjs` (classification and lifecycle) and `tools/test-browser-hangul.mjs` (trusted CDP key events in a headless Chromium browser through the real page, unchanged noVNC and an RFB WebSocket on loopback; it checks the original drop, the repaired bytes, paired, repeated and down-only keys, reset and focus rules, ordinary keys and the legacy fallback). They do not test a real Windows guest's IME.

References: [W3C UI Events key values](https://www.w3.org/TR/uievents-key/), [W3C UI Events code values](https://www.w3.org/TR/uievents-code/), [QEMU keycode map](https://github.com/qemu/keycodemapdb/blob/master/data/keymaps.csv).
